#include "mod/net/horse_diag.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include <safetyhook.hpp>

#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;

constexpr uint32_t kCopyHorse = 0xB010F002;

struct Site {
    uintptr_t rva;
    uint8_t head[16];
    const char* name;
};
constexpr Site kCcUpdate = {0x85FBA0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                       0x41, 0x56, 0x41, 0x57}, "CC update"};
constexpr Site kCcPost = {0x860D30, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                     0x41, 0x56, 0x48, 0x83}, "CC post-update"};
constexpr Site kRestore = {0x8646D0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x48, 0x89, 0x50, 0x10, 0x55,
                                      0x56, 0x57, 0x41, 0x54}, "restore actions"};
constexpr Site kPending = {0x2329D70, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                       0x74, 0x24, 0x20, 0x57}, "pending actions"};

SafetyHookMid g_cc_hook, g_post_hook, g_restore_hook, g_pending_hook;
uint8_t* g_ret = nullptr;
bool g_contain = false;
std::atomic<uint32_t> g_watch_real{0};
std::atomic<int> g_lines{0};
constexpr int kMaxLines = 400;
std::atomic<uint64_t> g_skipped{0}, g_skipped_post{0};
uint64_t g_t0 = 0;

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct Layer {
    uintptr_t a0, a8, a10, a18, chart;
};
struct State {
    uint32_t key = 0;
    Layer layer[2]{};
    uint8_t pending = 0;     // CC+0xA8
    uintptr_t pending_list = 0;  // CC+0x80
    uint8_t restore = 0xFF;  // [[CC+0x40]+0x30]
};

bool read_key(uintptr_t cc, uint32_t& key) {
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(cc + 0x08);
        if (!actor) return false;
        key = *reinterpret_cast<const uint32_t*>(actor + off::kActor_Key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_state(uintptr_t cc, State& s) {
    __try {
        for (int i = 0; i < 2; ++i) {
            const auto l = *reinterpret_cast<const uintptr_t*>(cc + 0x88 + 8 * i);
            if (!l) continue;
            s.layer[i] = {*reinterpret_cast<const uintptr_t*>(l + 0x00), *reinterpret_cast<const uintptr_t*>(l + 0x08),
                          *reinterpret_cast<const uintptr_t*>(l + 0x10), *reinterpret_cast<const uintptr_t*>(l + 0x18),
                          *reinterpret_cast<const uintptr_t*>(l + 0x110)};
        }
        s.pending = *reinterpret_cast<const uint8_t*>(cc + 0xA8);
        s.pending_list = *reinterpret_cast<const uintptr_t*>(cc + 0x80);
        const auto st = *reinterpret_cast<const uintptr_t*>(cc + 0x40);
        s.restore = st ? *reinterpret_cast<const uint8_t*>(st + 0x30) : 0xFE;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void read_restore_data(uintptr_t cc, uint8_t (&raw)[0x10], uint64_t& f8) {
    __try {
        const auto st = *reinterpret_cast<const uintptr_t*>(cc + 0x40);
        if (st) {
            std::memcpy(raw, reinterpret_cast<const void*>(st + 0x30), sizeof raw);
            f8 = *reinterpret_cast<const uint64_t*>(st + 0xF8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool is_copy(uint32_t key) { return key == kCopyHorse || is_enemy_copy_key(key); }

bool watched(uint32_t key) {
    return is_copy(key) || (key && key == g_watch_real.load(std::memory_order_relaxed));
}

const char* who(uint32_t key) {
    return key == kCopyHorse ? "COPY" : is_enemy_copy_key(key) ? "ENEMY COPY" : "real";
}

// Same "shape" = same nullness of every field (pointers themselves change).
uint32_t shape(const State& s) {
    uint32_t v = 0;
    for (int i = 0; i < 2; ++i) {
        const Layer& l = s.layer[i];
        v = v << 5 | (l.a0 != 0) | (l.a8 != 0) << 1 | (l.a10 != 0) << 2 | (l.a18 != 0) << 3 | (l.chart != 0) << 4;
    }
    return v << 10 | (s.pending != 0) << 9 | (s.pending_list != 0) << 8 | s.restore;
}

bool take_line() { return g_lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines; }

void log_state(const char* where, const State& s) {
    if (!take_line()) return;
    log::info("HorseDiag +{} ms {} {} 0x{:08X}: L0 a0={} a8={} cur(+10)={} play(+18)={} chart={} | "
              "L1 cur={} play={} chart={} | pending(+A8)={} list(+80)={} restoreByte={}",
              now_ms() - g_t0, where, who(s.key), s.key, s.layer[0].a0 != 0, s.layer[0].a8 != 0,
              s.layer[0].a10 != 0, s.layer[0].a18 != 0, s.layer[0].chart != 0, s.layer[1].a10 != 0,
              s.layer[1].a18 != 0, s.layer[1].chart != 0, s.pending, s.pending_list != 0, s.restore);
}

// A layer with its own chart but nothing playing: the update crashes on it.
// Both layers: an enemy copy ("Wolf Tracker") got its layer-0 action and
// crashed on layer 1 (live test 2026-10-04).
bool layer_without_action(const State& s) {
    for (const Layer& l : s.layer) {
        if (l.chart && (!l.a10 || !l.a18)) return true;
    }
    return false;
}

// Last shape seen per watched actor (2 slots: copy, real).
std::atomic<uint32_t> g_last_shape[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};

void on_cc_update(safetyhook::Context& ctx) {
    uint32_t key = 0;
    if (!read_key(ctx.rcx, key) || !watched(key)) return;
    State s;
    s.key = key;
    if (!read_state(ctx.rcx, s)) return;
    const uint32_t sh = shape(s);
    auto& last = g_last_shape[key == kCopyHorse ? 0 : is_enemy_copy_key(key) ? 2 : 1];
    if (last.exchange(sh, std::memory_order_relaxed) != sh) log_state("update", s);
    // Containment: a copy whose main layer has a chart but no current action
    // crashed three different functions of this update (INFORME §5).
    if (g_contain && is_copy(key) && layer_without_action(s)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        ctx.rip = reinterpret_cast<uintptr_t>(g_ret);
    }
}

void on_cc_post(safetyhook::Context& ctx) {
    if (!g_contain) return;
    uint32_t key = 0;
    if (!read_key(ctx.rcx, key) || !is_copy(key)) return;
    State s;
    if (!read_state(ctx.rcx, s)) return;
    if (layer_without_action(s)) {
        g_skipped_post.fetch_add(1, std::memory_order_relaxed);
        ctx.rip = reinterpret_cast<uintptr_t>(g_ret);
    }
}

void on_restore(safetyhook::Context& ctx) {
    uint32_t key = 0;
    if (!read_key(ctx.rcx, key) || !watched(key)) return;
    State s;
    s.key = key;
    if (!read_state(ctx.rcx, s)) return;
    log_state("RESTORE(0x8646D0)", s);
    // The restore data: bytes +0x30..+0x40 and the u64 at +0xF8 of [CC+0x40].
    uint8_t raw[0x10] = {};
    uint64_t f8 = 0;
    read_restore_data(ctx.rcx, raw, f8);
    if (take_line()) {
        log::info("HorseDiag   restore data [CC+0x40]+0x30: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                  "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} | +0xF8 0x{:X}",
                  raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
                  raw[11], raw[12], raw[13], raw[14], raw[15], f8);
    }
}

void on_pending(safetyhook::Context& ctx) {
    uint32_t key = 0;
    if (!read_key(ctx.rcx, key) || !watched(key)) return;
    State s;
    s.key = key;
    if (!read_state(ctx.rcx, s)) return;
    if (s.pending) log_state("PENDING(0x2329D70)", s);
}

bool hook(const Site& site, SafetyHookMid& out, safetyhook::MidHookFn fn) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t head[16];
    if (!game::safe_read(base + site.rva, head, sizeof head) || std::memcmp(head, site.head, sizeof head) != 0) {
        log::warn("HorseDiag: {} (exe+0x{:X}) not recognised", site.name, site.rva);
        return false;
    }
    auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(base + site.rva), fn);
    if (!h) {
        log::warn("HorseDiag: {} hook failed", site.name);
        return false;
    }
    out = std::move(*h);
    return true;
}

}  // namespace

bool horse_diag_install(bool contain) {
    if (g_cc_hook) return true;
    g_t0 = now_ms();
    g_ret = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_ret) return false;
    g_ret[0] = 0xC3;
    DWORD old = 0;
    VirtualProtect(g_ret, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_ret, 16);
    const bool ok = hook(kCcUpdate, g_cc_hook, on_cc_update) && hook(kRestore, g_restore_hook, on_restore) &&
                    hook(kPending, g_pending_hook, on_pending);
    if (!ok) {
        g_cc_hook = {};
        g_restore_hook = {};
        g_pending_hook = {};
        return false;
    }
    g_contain = contain && hook(kCcPost, g_post_hook, on_cc_post);
    log::info("HorseDiag: installed (containment {})", g_contain ? "on" : "off");
    return true;
}

void horse_diag_watch(uint32_t key) {
    g_watch_real = key;
    log::info("HorseDiag: watching the copy 0x{:08X} and the real horse 0x{:08X}", kCopyHorse, key);
}

void horse_diag_tick() {
    static uint64_t logged = 0, logged_post = 0, last = 0;
    const uint64_t a = g_skipped.load(std::memory_order_relaxed), b = g_skipped_post.load(std::memory_order_relaxed);
    const uint64_t t = now_ms();
    if ((a != logged || b != logged_post) && t - last >= 2000) {
        log::info("HorseDiag: copy's update skipped {} times (post-update {}) while it has no action", a, b);
        logged = a;
        logged_post = b;
        last = t;
    }
}

}  // namespace cdmp::net
