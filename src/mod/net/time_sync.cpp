#include "mod/net/time_sync.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <cstring>
#include <format>
#include <string>

#include "common/field_time.h"
#include "mod/game/game_clock.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;

constexpr uint32_t kSendEveryMs = 5000;
// Below this difference nothing is done: ~2 game minutes, 10 real seconds.
constexpr uint64_t kToleranceRaw = 120'000;
// After setting the time, give it a while to show before trying again (it
// fails silently while loading).
constexpr uint32_t kRetryAfterMs = 15000;
constexpr size_t kEventSize = 15;  // u32 seq | u64 raw | u16 scale | u8 flags

std::atomic<int> g_mode{0};
std::atomic<bool> g_dev_key{false};
bool g_dev_was_down = false;
uint32_t g_last_send = 0, g_seq = 0;
uint32_t g_last_set = 0;
// Real adjustments are rare (joining, after a load or a menu): at most this
// many in 5 minutes, so the other player cannot flip our day and night in a
// loop (security review H10).
constexpr size_t kMaxSetsPer5Min = 8;
std::deque<uint32_t> g_sets;
bool g_matched = false;
std::string g_last_problem;

uint32_t wall_ms() { return GetTickCount(); }

void problem(const std::string& what) {
    if (what == g_last_problem) return;
    g_last_problem = what;
    if (!what.empty()) log::info("Time sync: {}", what);
}

std::string hhmm(uint64_t raw) {
    FieldTime f;
    f.set_raw(raw);
    return std::format("day {} {:02}:{:02}", f.day, f.hour, f.minute);
}

// Our current time of day, from the client's copy (exe+0x6A6D218) carried
// forward to now. The game copies it without a lock: read it twice.
std::optional<std::pair<FieldTime, uint64_t>> own_time() {
    if (!capture_game_clock_trusted()) {
        problem("waiting for the game clock (move once)");
        return std::nullopt;
    }
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t a[FieldTime::kMemSize], b[FieldTime::kMemSize];
    if (!game::safe_read(base + off::kFieldTimeClient, a, sizeof a) ||
        !game::safe_read(base + off::kFieldTimeClient, b, sizeof b) || std::memcmp(a, b, sizeof a) != 0) {
        return std::nullopt;
    }
    const auto ft = FieldTime::from_memory(a);
    if (!ft) {
        problem("our time of day is not readable");
        return std::nullopt;
    }
    // A level with a fixed time (dungeons) leaves the anchor at 0: nothing to sync there.
    if (ft->anchor == 0) {
        problem("this place has a fixed time of day; not synchronised here");
        return std::nullopt;
    }
    const auto now = game::client_game_clock_ms();
    if (!now) return std::nullopt;
    problem("");
    return std::make_pair(*ft, ft->raw_at(*now));
}

// SetFieldTimeReq (2125, 36 bytes): FieldTime (31) | u8 flag 0 | u32 key 0.
// With flag 0 the server only sets the time (as when waking up), saves it
// with the field and tells its client with SetFieldTimeAck.
bool set_time(FieldTime ft, uint64_t raw) {
    const auto now = game::client_game_clock_ms();
    if (!now) return false;
    ft.set_raw(raw);
    ft.anchor = *now;  // the server carries it forward from here, on its own clock
    std::vector<uint8_t> pk(5 + 36, 0);
    const uint16_t op = off::kOpSetFieldTimeReq, len = 36;
    std::memcpy(pk.data(), &op, 2);
    pk[2] = 0;  // slot: our character
    std::memcpy(pk.data() + 3, &len, 2);
    ft.to_wire(pk.data() + 5);
    return sniffer_inject_c2s(std::move(pk));
}

}  // namespace

void time_sync_set_mode(TimeSyncMode mode) {
    g_mode = static_cast<int>(mode);
    if (mode != TimeSyncMode::Off) {
        log::info("Time sync: {}", mode == TimeSyncMode::Send ? "sending our time of day"
                                                              : "taking the other player's time of day");
    }
}

void time_sync_set_dev_key(bool on) {
    g_dev_key = on;
    if (on) log::warn("Time sync (dev): Ctrl+Shift+F12 = our time of day 3 hours forward");
}

std::optional<proto::GameEvent> time_sync_tick(uint32_t now_ms) {
    if (g_dev_key.load()) {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                          (GetAsyncKeyState(VK_F12) & 0x8000);
        if (down && !g_dev_was_down) {
            if (const auto own = own_time()) {
                FieldTime later;
                later.set_raw(own->second);
                later.hour = (later.hour + 3) % 24;
                const uint64_t target = nearest_raw_with_time_of(own->second + FieldTime::kRawDay / 4, later.to_raw());
                const bool ok = set_time(own->first, target);
                log::info("Time sync (dev): {} -> {}{}", hhmm(own->second), hhmm(target),
                          ok ? "; SetFieldTimeReq queued" : "; NOT sent");
            }
        }
        g_dev_was_down = down;
    }
    if (g_mode.load() != static_cast<int>(TimeSyncMode::Send) || now_ms - g_last_send < kSendEveryMs) {
        return std::nullopt;
    }
    g_last_send = now_ms;
    const auto own = own_time();
    if (!own) return std::nullopt;
    proto::GameEvent ev{proto::EventKind::TimeSync, std::vector<uint8_t>(kEventSize, 0)};
    const uint32_t seq = ++g_seq;
    std::memcpy(ev.payload.data(), &seq, 4);
    std::memcpy(ev.payload.data() + 4, &own->second, 8);
    std::memcpy(ev.payload.data() + 12, &own->first.scale, 2);
    if (seq % 12 == 1) log::info("Time sync: our time of day is {} (sent every 5 s)", hhmm(own->second));
    return ev;
}

void time_sync_on_event(const proto::GameEvent& ev) {
    if (g_mode.load() != static_cast<int>(TimeSyncMode::Apply) || ev.payload.size() < kEventSize) return;
    // In a menu our clock stands still and nothing we send goes out: after it.
    if (puppet_local_paused()) return;
    uint64_t theirs;
    std::memcpy(&theirs, ev.payload.data() + 4, 8);
    const auto own = own_time();
    if (!own) return;
    const uint64_t ours = own->second;
    const uint64_t target = nearest_raw_with_time_of(ours, theirs);
    const uint64_t diff = target > ours ? target - ours : ours - target;
    if (diff <= kToleranceRaw) {
        if (!g_matched) log::info("Time sync: our time of day matches the other player's ({})", hhmm(ours));
        g_matched = true;
        return;
    }
    g_matched = false;
    const uint32_t now = wall_ms();
    if (g_last_set && now - g_last_set < kRetryAfterMs) return;
    while (!g_sets.empty() && now - g_sets.front() > 300000) g_sets.pop_front();
    if (g_sets.size() >= kMaxSetsPer5Min) {
        problem("the other player's time of day changed too often; not followed for a while");
        return;
    }
    g_sets.push_back(now);
    g_last_set = now;
    const bool ok = set_time(own->first, target);
    log::info("Time sync: ours {} -> theirs {} ({} {:.0f} game min){}", hhmm(ours), hhmm(target),
              target > ours ? "forward" : "back", static_cast<double>(diff) / 60000.0,
              ok ? "; SetFieldTimeReq queued" : "; NOT sent");
}

}  // namespace cdmp::net
