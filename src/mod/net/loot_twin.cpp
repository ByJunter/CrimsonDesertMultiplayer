#include "mod/net/loot_twin.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/enemy_load.h"
#include "common/keys.h"
#include "mod/game/actor_lookup.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/local_capture.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kOpActorLoadDataAck = 2005;
constexpr uint16_t kOpDoFrameEventReq = 2380;              // u8 4 | u32 attacker | ... | 665-byte targets at the end
constexpr uint16_t kOpSpawnCharacterCheatReq = 2115;       // u32 variant | u32 0 | vec3 | u8 spawn reason
constexpr uint16_t kOpKillCheatReq = 2279;                 // u32 victim | u32 option (0 = dies at once)
constexpr uint16_t kOpNotifyHandleDeadAck = 2530;          // u32 dead | ...
constexpr uint16_t kOpLootDropCharacterRewardListAck = 2897;  // u8 4 | u32 dead | n x 37 B
constexpr uint16_t kOpRemoveLootableAck = 2822;            // u32 dead | u32 player
constexpr uint16_t kOpInteractionPreemptionStepReq = 2671;  // u32 who | u32 target | ... (25 B)
constexpr uint16_t kOpInteractionDoStepReq = 2720;          // u32 who | u32 target | ... (16 B)
constexpr uint16_t kOpInteractionPreemptionStepSuccessAck = 2663;  // u32 who | the 25 B (target at +8)
constexpr uint16_t kOpInteractionPreemptionStepFailAck = 3000;     // u32 who | u32 target | ... | u32 error at +12
constexpr uint16_t kOpLootingDropFromDeadBodyReq = 3052;            // u32 dead | u8 | u8 (botin-invitado.md §1.4)
constexpr uint32_t kErrInteractionCondition = 0x2569C661;          // eErrNoInteractionTargetConditionCheckFail
constexpr uint64_t kRescueWaitMs = 3000;
constexpr uint16_t kOpInteractionDoStepSuccessAck = 2916;          // u32 player | the 16 B (target at +8)
constexpr uint16_t kOpAttackerAck = 2509;           // PostProcessAttackResult_AttackerAck: u32 attacker | u32 target
constexpr uint16_t kOpRemoveActorAck = 2516;        // 04 | u32 0 | u8 reason | n x u32 key
constexpr uint8_t kSpawnReasonCheat = 33;
constexpr size_t kTargetEntry = 665, kFirstEntry = 0x203;

// The guest earns loot from a copy it hit lately, close to it; a few at most.
constexpr uint64_t kHitWindowMs = 30000;
constexpr float kMaxLootDistance = 30.0f;
// The twin's load comes ~15 ms after the request (live test, 7 Oct) and is
// matched by variant and place: copies dying together each get theirs.
constexpr uint64_t kMinSpawnGapMs = 0;
constexpr size_t kMaxSpawnsPer5Min = 30;
// The twin's load came 5-7 s after the request in other tools; matched by
// variant and place. One that comes later still never shows and dies, with
// no loot (it was already given up).
constexpr uint64_t kTwinWaitMs = 15000;
constexpr uint64_t kKillOnSightMs = 120000;
constexpr float kTwinMatchDistance = 3.0f;
// A twin not seen dead this long after KillCheatReq: asked again, a few times.
constexpr uint64_t kKillConfirmMs = 3000;
constexpr int kKillTries = 3;
constexpr size_t kMaxTracked = 256;

struct Pending {
    uint32_t copy = 0, variant = 0;
    float pos[3] = {};
    uint64_t asked_ms = 0;
    bool loot = true;  // false: given up, only hidden and killed if it comes
};
struct Twin {
    uint32_t copy = 0;  // 0: no loot for it (it came late)
    uint64_t kill_ms = 0;
    int tries = 0;
    bool dead = false;
    uint64_t refused_ms = 0;  // the server refused to loot it (condition check): rescue asked then
};

std::atomic<bool> g_enabled{false};
SRWLOCK g_lock = SRWLOCK_INIT;
std::map<uint32_t, uint64_t> g_hit_ms;  // copy -> our last blow on it
std::set<uint32_t> g_done;              // copies already decided
std::deque<Pending> g_pending;          // twins asked for, not seen yet
std::map<uint32_t, uint32_t> g_twin_of;  // copy -> twin
std::map<uint32_t, Twin> g_twins;        // twin -> its state
std::deque<uint64_t> g_spawns;           // when we asked for twins
std::atomic<uint64_t> g_logged{0};
std::vector<std::string> g_lines;  // under g_lock: written by loot_tick (no log I/O on the game's thread)

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

uint64_t now_ms() { return GetTickCount64(); }
uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Game packet buffers: copied and written under SEH (no C++ objects here).
bool guarded_read(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool guarded_write(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Bytes make_packet(uint16_t op, const Bytes& payload) {
    Bytes p(5 + payload.size());
    const uint16_t len = static_cast<uint16_t>(payload.size());
    std::memcpy(p.data(), &op, 2);
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, payload.data(), payload.size());
    return p;
}

bool send_kill(uint32_t twin) {
    Bytes b(8);
    std::memcpy(b.data(), &twin, 4);
    return sniffer_inject_c2s(make_packet(kOpKillCheatReq, b));
}

// What a server sends once a corpse is emptied: the copy offers no loot.
void no_loot_here(uint32_t copy) {
    const uint32_t own = capture_own_player_key();
    if (!own || !copy) return;
    Bytes empty(5);
    empty[0] = 4;
    std::memcpy(empty.data() + 1, &copy, 4);
    Bytes gone(8);
    std::memcpy(gone.data(), &copy, 4);
    std::memcpy(gone.data() + 4, &own, 4);
    puppet_inject_s2c(kOpLootDropCharacterRewardListAck, empty);
    puppet_inject_s2c(kOpRemoveLootableAck, gone);
}

// The load's last Transform: scale (1,1,1) then an all-FF parent; the
// position is 28 bytes before it (as enemy_relay's load_transform_ok).
bool load_position(const Bytes& load, float out[3]) {
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (load.size() < sizeof kMarker + 28) return false;
    for (size_t i = load.size() - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(load.data() + i, kMarker, sizeof kMarker) != 0) continue;
        std::memcpy(out, load.data() + i - 28, 12);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    return false;
}

float distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Caller holds g_lock. The copy's twin forgotten (it keeps its own kill state).
void unalias_locked(uint32_t copy) {
    if (auto t = g_twin_of.find(copy); t != g_twin_of.end()) {
        if (auto tw = g_twins.find(t->second); tw != g_twins.end()) tw->second.copy = 0;
        g_twin_of.erase(t);
    }
}

}  // namespace

void loot_set_enabled(bool on) {
    g_enabled = on;
    log::info("Co-op: loot from the other player's enemies we help kill: {}", on ? "on (a twin on our server)" : "off");
}

void loot_observe_c2s(const uint8_t* packet, size_t size) {
    if (!g_enabled || size < 5) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpDoFrameEventReq) return;
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    if (n < kFirstEntry + kTargetEntry || p[0] != 4) return;
    const uint32_t own = capture_own_player_key();
    if (!own || u32_at(p + 1) != own) return;
    const uint64_t now = now_ms();
    Lock lock;
    for (size_t k = 1; k <= 30 && n >= k * kTargetEntry + kFirstEntry; ++k) {
        const uint32_t target = u32_at(p + n - k * kTargetEntry);
        if (!keys::is_actor_key(target)) break;
        if (!is_valid_enemy_copy_key(target)) continue;
        if (g_hit_ms.size() >= kMaxTracked) g_hit_ms.clear();
        g_hit_ms[target] = now;
    }
}

bool loot_on_copy_died(uint32_t copy, uint32_t variant, uint64_t /*delay_ms*/) {
    if (!g_enabled || !variant) return false;
    const uint64_t now = now_ms();
    const char* why = nullptr;
    float pos[3] = {};
    {
        Lock lock;
        if (g_done.count(copy)) {
            // A repeated death message: still on its way, or already here.
            const bool pending = std::any_of(g_pending.begin(), g_pending.end(),
                                             [&](const Pending& p) { return p.copy == copy && p.loot; });
            return pending || g_twin_of.count(copy) != 0;
        }
        if (g_done.size() >= kMaxTracked) g_done.clear();
        g_done.insert(copy);
        auto hit = g_hit_ms.find(copy);
        while (!g_spawns.empty() && now - g_spawns.front() >= 300000) g_spawns.pop_front();
        if (enemy_load::is_excluded_variant(variant)) {
            why = "a boss or unique enemy";
        } else if (hit == g_hit_ms.end() || now - hit->second > kHitWindowMs) {
            why = "we did not hit it lately";
        } else if (g_spawns.size() >= kMaxSpawnsPer5Min) {
            why = "too many twins lately";
        }
    }
    const auto at = game::actor_world_position(copy);
    const auto me = game::actor_world_position(capture_own_player_key());
    if (!why && (!at || !me)) why = "it or we are not in the world";
    if (!why) {
        pos[0] = at->x, pos[1] = at->y, pos[2] = at->z;
        const float mine[3] = {me->x, me->y, me->z};
        if (distance(pos, mine) > kMaxLootDistance) why = "we are too far from it";
    }
    if (why) {
        if (g_logged++ < 40) log::info("Loot: copy 0x{:08X} died; no loot for us: {}", copy, why);
        return false;
    }
    Bytes b(21);
    std::memcpy(b.data(), &variant, 4);
    std::memcpy(b.data() + 8, pos, 12);
    b[20] = kSpawnReasonCheat;
    if (!sniffer_inject_c2s(make_packet(kOpSpawnCharacterCheatReq, b))) {
        log::info("Loot: copy 0x{:08X} died; its twin could not be asked for", copy);
        return false;
    }
    {
        Lock lock;
        Pending p;
        p.copy = copy, p.variant = variant, p.asked_ms = now;
        std::memcpy(p.pos, pos, sizeof pos);
        if (g_pending.size() >= kMaxTracked) g_pending.pop_front();
        g_pending.push_back(p);
        g_spawns.push_back(now);
    }
    if (g_logged++ < 40) {
        log::info("Loot: copy 0x{:08X} died; asking our server for its twin (variant {}) at ({:.1f}, {:.1f}, {:.1f})",
                  copy, variant, pos[0], pos[1], pos[2]);
    }
    return true;
}

void loot_tick() {
    if (!g_enabled) return;
    const uint64_t now = now_ms();
    std::vector<uint32_t> no_loot, resend, refused;
    std::vector<std::string> lines;
    {
        Lock lock;
        lines.swap(g_lines);
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (it->loot && now - it->asked_ms > kTwinWaitMs) {
                // Given up: no loot, but if it still comes it is hidden and killed.
                no_loot.push_back(it->copy);
                it->loot = false;
            }
            if (!it->loot && now - it->asked_ms > kKillOnSightMs) {
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = g_twins.begin(); it != g_twins.end();) {
            Twin& t = it->second;
            if (t.refused_ms && t.copy && now - t.refused_ms > kRescueWaitMs) {
                // Still not looted: no loot from it; its prompt goes (our pet stops trying).
                refused.push_back(t.copy);
                g_twin_of.erase(t.copy);
                t.copy = 0;
            }
            if (t.dead || now - t.kill_ms < kKillConfirmMs) {
                // Dead ones are kept until looted (RemoveLootableAck) or too old.
                if (t.dead && t.copy == 0 && now - t.kill_ms > kKillOnSightMs) {
                    it = g_twins.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            if (sniffer_world_loading()) {  // its KillCheatReq would only wait: no try spent
                ++it;
                continue;
            }
            if (t.tries < kKillTries) {
                ++t.tries;
                t.kill_ms = now;
                resend.push_back(it->first);
                ++it;
                continue;
            }
            // It never died: no loot from it.
            if (t.copy) {
                no_loot.push_back(t.copy);
                g_twin_of.erase(t.copy);
            }
            log::warn("Loot: twin 0x{:08X} did not die after {} KillCheatReq; left alone", it->first, kKillTries + 1);
            it = g_twins.erase(it);
        }
    }
    for (const auto& l : lines) log::info("{}", l);
    for (uint32_t copy : no_loot) {
        log::info("Loot: no loot for copy 0x{:08X} (its twin never came or never died)", copy);
        no_loot_here(copy);
    }
    for (uint32_t copy : refused) {
        log::info("Loot: copy 0x{:08X}: its twin was refused; 3 s after asking for its drop no loot was confirmed "
                  "(result unknown: check the inventory); its prompt goes",
                  copy);
        no_loot_here(copy);
    }
    for (uint32_t twin : resend) {
        log::info("Loot: twin 0x{:08X} not dead yet; KillCheatReq again", twin);
        send_kill(twin);
    }
}

bool loot_keeps_copy(uint32_t copy) {
    if (!g_enabled) return false;
    Lock lock;
    if (g_twin_of.count(copy)) return true;
    return std::any_of(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.copy == copy && p.loot; });
}

void loot_forget_copy(uint32_t copy) {
    Lock lock;
    // A twin still on its way for this copy is hidden and killed, with no loot.
    for (Pending& p : g_pending) {
        if (p.copy == copy) p.loot = false;
    }
    unalias_locked(copy);
    g_hit_ms.erase(copy);
    g_done.erase(copy);
}

void loot_reset() {
    Lock lock;
    for (auto& [twin, t] : g_twins) t.copy = 0;  // still killed if alive, no loot to the new partner's copies
    g_twin_of.clear();
    for (Pending& p : g_pending) p.loot = false;
    g_hit_ms.clear();
    g_done.clear();
}

bool loot_filter_s2c(uint8_t* packet, uint16_t size) {
    if (!g_enabled || !packet || size < 5 + 8) return false;
    uint8_t head[5 + 12] = {};
    if (!guarded_read(head, packet, size >= sizeof head ? sizeof head : 13)) return false;
    uint16_t op;
    std::memcpy(&op, head, 2);
    const uint8_t* p = head + 5;
    uint32_t kill = 0, copy = 0, gone_copy = 0, rescue = 0;
    bool loot = false, drop_gone = false;
    {
        Lock lock;
        if (g_pending.empty() && g_twins.empty()) return false;
        if (op == kOpActorLoadDataAck) {
            if (g_pending.empty() || size < 5 + 0x40 || p[2] != 3 || u32_at(p + 7) != 0 ||
                !keys::is_game_world_key(u32_at(p + 3))) {
                return false;
            }
            // One record of a new NPC: our twin, if variant and place match a
            // pending one (a native of the same camp is never a new key here).
            uint16_t len;
            std::memcpy(&len, p, 2);
            if (static_cast<size_t>(len) + 2 != size - 5u) return false;
            Bytes load(size - 5u);
            if (!guarded_read(load.data(), packet + 5, load.size())) return false;
            const uint32_t variant = u32_at(load.data() + 0x18), key = u32_at(load.data() + 3);
            float at[3];
            if (!load_position(load, at) || g_twins.count(key) || game::find_actor(key)) return false;
            for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
                if (it->variant != variant || distance(at, it->pos) > kTwinMatchDistance) continue;
                kill = key;
                loot = it->loot;
                copy = loot ? it->copy : 0;
                g_twins[key] = Twin{copy, now_ms(), 0, false};
                if (copy) g_twin_of[copy] = key;
                g_pending.erase(it);
                break;
            }
            if (!kill) return false;
        } else if (op == kOpRemoveActorAck) {
            // The server's clean-up of a twin's corpse (~150 s after its
            // death): the client never had it, and the copy keeps no loot.
            uint32_t k = 0;
            if (p[0] != 4 || size != 5 + 10 || !guarded_read(&k, packet + 5 + 6, 4)) return false;
            auto t = g_twins.find(k);
            if (t == g_twins.end()) return false;
            gone_copy = t->second.copy;
            if (gone_copy && g_lines.size() < 64) {
                // Our server took the corpse before we looted it (7 Oct night: all 7, reason 0x19).
                g_lines.push_back(std::format("Loot: twin 0x{:08X} of copy 0x{:08X} removed by our server (reason "
                                              "0x{:02X}, {} s after we asked for its death): its loot is gone",
                                              k, gone_copy, p[5], (now_ms() - t->second.kill_ms) / 1000));
            }
            if (gone_copy) g_twin_of.erase(gone_copy);
            g_twins.erase(t);
            drop_gone = true;
        } else {
            // Messages about a twin: its loot ones go to the copy, the rest nowhere.
            size_t at = 0;
            if (op == kOpLootDropCharacterRewardListAck && p[0] == 4) {
                at = 1;
            } else if (op == kOpInteractionPreemptionStepSuccessAck || op == kOpInteractionDoStepSuccessAck) {
                at = 8;
            } else if (op == kOpInteractionPreemptionStepFailAck || op == kOpAttackerAck) {
                at = 4;
            }
            if (at && 5 + at + 4 <= size) {
                uint32_t k = 0;
                if (!guarded_read(&k, packet + 5 + at, 4)) return false;
                if (auto t = g_twins.find(k); t != g_twins.end()) {
                    if (!t->second.copy) return true;  // no loot from this one
                    uint32_t error = 0;
                    if (op == kOpInteractionPreemptionStepFailAck && size == 5 + 23 && g_lines.size() < 64) {
                        // who | twin | interaction | error | condition index | origin | tail (P4 §3.2).
                        uint8_t f[23];
                        if (guarded_read(f, packet + 5, sizeof f)) {
                            g_lines.push_back(std::format(
                                "Loot: our server refused twin 0x{:08X} (copy 0x{:08X}): who 0x{:08X} interaction "
                                "0x{:08X} error 0x{:08X} condition {} origin {} tail {:02X} {:02X}",
                                k, t->second.copy, u32_at(f), u32_at(f + 8), u32_at(f + 12), u32_at(f + 16), f[20],
                                f[21], f[22]));
                        }
                    }
                    if (op == kOpInteractionPreemptionStepFailAck && size >= 5 + 16 &&
                        guarded_read(&error, packet + 5 + 12, 4) && error == kErrInteractionCondition &&
                        !t->second.refused_ms) {
                        // Some twins the server will not let us loot (7 Oct night: 2
                        // of 11, our pet then stuck retrying): ask once for the
                        // drop directly, then give up on it (loot_tick).
                        t->second.refused_ms = now_ms();
                        rescue = k;
                        copy = t->second.copy;
                    }
                    if (op == kOpLootDropCharacterRewardListAck) {
                        const size_t items = size > 5 + 5 ? (size - 5 - 5) / 37 : 0;
                        if (g_logged++ < 80 && g_lines.size() < 64) {
                            g_lines.push_back(std::format("Loot: twin 0x{:08X} of copy 0x{:08X} dropped {} items for us",
                                                          k, t->second.copy, items));
                        }
                    }
                    guarded_write(packet + 5 + at, &t->second.copy, 4);
                    if (!rescue) return false;
                }
            }
            if (rescue) {
                // fall through to the rescue below (outside the lock)
            } else {
            auto t = g_twins.find(u32_at(p));
            if (t == g_twins.end()) return false;
            if (op == kOpNotifyHandleDeadAck) t->second.dead = true;
            if (op != kOpRemoveLootableAck || !t->second.copy) return true;  // its HP, death, buffs, actions...
            copy = t->second.copy;
            guarded_write(packet + 5, &copy, 4);
            // Looted: the twin is done with.
            if (g_logged++ < 80 && g_lines.size() < 64) {
                g_lines.push_back(std::format("Loot: copy 0x{:08X} looted (its twin 0x{:08X})", copy, t->first));
            }
            g_twin_of.erase(copy);
            g_twins.erase(t);
            return false;
            }
        }
    }
    if (rescue) {
        Bytes b(6);
        std::memcpy(b.data(), &rescue, 4);
        const bool ok = sniffer_inject_c2s(make_packet(kOpLootingDropFromDeadBodyReq, b));
        log::info("Loot: our server refused to let us loot twin 0x{:08X} (copy 0x{:08X}, condition check); asked for "
                  "its drop directly{}",
                  rescue, copy, ok ? "" : " (NOT sent)");
        return false;
    }
    if (drop_gone) {
        if (gone_copy) no_loot_here(gone_copy);
        return true;
    }
    // Our twin's load: never shown; it dies at once, and its loot is rolled.
    const bool ok = send_kill(kill);
    if (copy) {
        log::info("Loot: twin 0x{:08X} of copy 0x{:08X} is on our server; killing it for its loot{}", kill, copy,
                  ok ? "" : " (KillCheatReq NOT sent; tried again)");
    } else {
        log::info("Loot: late twin 0x{:08X} hidden and killed (no loot from it){}", kill,
                  ok ? "" : " (KillCheatReq NOT sent; tried again)");
    }
    if (!ok) {
        Lock lock;
        if (auto t = g_twins.find(kill); t != g_twins.end()) t->second.kill_ms = 0;  // retried at the next tick
    }
    return true;
}

void loot_rewrite_c2s(uint8_t* packet, uint16_t size) {
    if (!g_enabled || !packet || size < 5 + 8) return;
    uint8_t head[5 + 8];
    if (!guarded_read(head, packet, sizeof head)) return;
    uint16_t op;
    std::memcpy(&op, head, 2);
    if (op == kOpDoFrameEventReq) {
        // The loot itself: after the interaction the looter (we or our pet)
        // "hits" the corpse, its only target; the server gives the items then
        // (31 of 31 real loots, botin-invitado.md, 7 Oct section). With more
        // targets the server wants them in key order: left alone.
        const size_t n = size - 5u;
        if (head[5] != 4 || n < kFirstEntry + kTargetEntry) return;
        // The looter: our player or our pet (a world actor), never a copy.
        const uint32_t attacker = u32_at(head + 6);
        const uint32_t own = capture_own_player_key();
        if (attacker != own && (!keys::is_class(attacker, keys::kClassWorld) || is_enemy_copy_key(attacker))) return;
        uint32_t target = 0, before = 0;
        if (!guarded_read(&target, packet + 5 + n - kTargetEntry, 4) || !is_valid_enemy_copy_key(target)) return;
        // Exactly one target entry (the loot hit is 1307 B: a longer head).
        if (n >= kFirstEntry + 2 * kTargetEntry &&
            (!guarded_read(&before, packet + 5 + n - 2 * kTargetEntry, 4) || keys::is_actor_key(before))) {
            return;
        }
        Lock lock;
        if (auto t = g_twin_of.find(target); t != g_twin_of.end()) {
            guarded_write(packet + 5 + n - kTargetEntry, &t->second, 4);
        }
        return;
    }
    if (op != kOpInteractionPreemptionStepReq && op != kOpInteractionDoStepReq) return;
    const uint32_t target = u32_at(head + 5 + 4);
    if (!is_valid_enemy_copy_key(target)) return;
    Lock lock;
    if (auto t = g_twin_of.find(target); t != g_twin_of.end()) guarded_write(packet + 5 + 4, &t->second, 4);
}

}  // namespace cdmp::net
