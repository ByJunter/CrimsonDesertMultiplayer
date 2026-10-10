#include "mod/net/aggro_test.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <mutex>
#include <vector>

#include <safetyhook.hpp>

#include "mod/game/actor_lookup.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "common/keys.h"
#include "mod/net/local_capture.h"
#include "mod/net/remote_puppet.h"

namespace cdmp::net {
namespace {
namespace off = game::build_2976;

constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr uint16_t kOpCurrentTargetChangeReq = 2984;   // u32 actor | u32 target | u64 seq | u8 | u8 reason
constexpr uint16_t kOpCurrentTargetChangedAck = 3040;  // same 18 B; u8 at +0x10 = force
constexpr uint16_t kOpDoFrameEventReq = 2380;

constexpr uintptr_t kIsTargetable = 0x22141E0;    // (target comp, actor* candidate, u8 mode)
constexpr uint8_t kIsTargetableHead[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                         0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr uintptr_t kSetCurrentTarget = 0x2217290;  // (comp, int* res, u32 target, u64* seq, ...)
constexpr uint8_t kSetCurrentTargetHead[] = {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x44,
                                             0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55};

// AICondition_CheckAIBattleType vf1 (vtable 0x58C2BD8): (cond, enemy's AI
// object, AI context) -> 0 when the condition holds. Its body (.xtls
// 0xC5DC280) gives "attack" (0 within the attack quota, 1 few attackers) or
// "wait" (2) by the enemy's place in the TARGET's rank list, which the game
// never fills for the other player's copy (it only does for actors this
// client controls): enemies on the copy always wait and circle
// (copia-objetivo-sin-ataques.md section 7). The entry is a 5-byte jmp into .xtls.
constexpr uintptr_t kCheckAIBattleType = 0x22E0A30;
constexpr uint8_t kCheckAIBattleTypeHead[] = {0xE9, 0x3B, 0xB4, 0x8A, 0x0C};

SafetyHookInline g_is_targetable;
SafetyHookInline g_battle_type;
std::atomic<int> g_copy_slots{0};  // enemies attacking the copy at once; 0 = the game decides
constexpr uint64_t kTurnMs = 4000;  // how long a pair of enemies keeps its turn on the copy
std::atomic<uint64_t> g_bt_copy{0}, g_bt_copy_wait{0}, g_bt_overridden{0}, g_bt_other{0};
SafetyHookMid g_set_target;
std::atomic<uint64_t> g_puppet_candidate{0}, g_player_candidate{0};
std::atomic<uint64_t> g_puppet_rejected{0}, g_player_rejected{0};
std::atomic<uint32_t> g_puppet_candidate_modes{0};  // bit per mode seen
std::atomic<uint32_t> g_puppet_rejected_modes{0};
std::atomic<uint32_t> g_player_rejected_modes{0};
// Ctrl+F11: answer "targetable" for the copy where the game said no.
std::atomic<bool> g_force{false};
std::atomic<uint64_t> g_forced{0};
std::atomic<uint64_t> g_set_to_puppet{0};
std::atomic<uint32_t> g_set_to_puppet_by{0};
// Our own player key for the hooks, which run on game threads (AI): they must
// not take local_capture's mutex (security review H12). Refreshed from the
// sniffer's writer thread (aggro_observe_c2s) and the test loop.
std::atomic<uint32_t> g_own_key{0};
void refresh_own_key() { g_own_key.store(capture_own_player_key(), std::memory_order_relaxed); }

// Last enemy that aimed at our own player, and its reason byte (sniffer thread).
std::mutex g_mutex;
uint32_t g_last_enemy = 0;
uint8_t g_last_reason = 0x18;
uint64_t g_last_enemy_ms = 0;
int g_logged_req = 0, g_logged_hits = 0;

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t key_of_actor(uintptr_t actor) {
    __try {
        return actor ? *reinterpret_cast<const uint32_t*>(actor + off::kActor_Key) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The copy is rejected for its "driven by the network" mark (TS+0x3DC = 1,
// live test 2026-10-04); its other states must still count: dead, hidden or
// untouchable (Status +0x390/+0x394 flags, +0x340, +0x276; IsTargetable
// rules 1, 3, 9 and 11) are never overridden.
bool copy_state_normal(uintptr_t actor) {
    __try {
        const auto comps = *reinterpret_cast<const uintptr_t*>(actor + off::kActor_ComponentTable);
        if (!comps) return false;
        const auto st = *reinterpret_cast<const uintptr_t*>(comps + 0x20);
        if (!st) return false;
        // +0x273: dead (pieza4-dano-servidor.md §1.4). A dead copy must not stay a
        // target (pendiente-reaccion-muerte-invitado.md).
        return (*reinterpret_cast<const uint32_t*>(st + 0x390) | *reinterpret_cast<const uint32_t*>(st + 0x394)) == 0 &&
               *reinterpret_cast<const uint8_t*>(st + 0x340) == 0 && *reinterpret_cast<const uint8_t*>(st + 0x276) == 0 &&
               *reinterpret_cast<const uint8_t*>(st + 0x273) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copy_dead_at(uintptr_t actor) {
    __try {
        const auto comps = *reinterpret_cast<const uintptr_t*>(actor + off::kActor_ComponentTable);
        if (!comps) return false;
        const auto st = *reinterpret_cast<const uintptr_t*>(comps + 0x20);
        return st && *reinterpret_cast<const uint8_t*>(st + 0x273) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What CheckAIBattleType would decide if the copy had a rank list: its
// attackers of the enemy's battle-order type stand in for it. -1 when the
// call is not about the copy (or anything is unreadable); else the state.
// Only reads, guarded; no C++ objects here (SEH).
int copy_battle_state(uintptr_t obj_ia, uintptr_t ctx, int slots) {
    __try {
        if (!*reinterpret_cast<const uint8_t*>(ctx + 0x268)) return -1;
        const uintptr_t target = *reinterpret_cast<const uintptr_t*>(ctx + 0x260);
        if (!target || *reinterpret_cast<const uint32_t*>(target + off::kActor_Key) != kPuppetPlayer) return -1;
        // The enemy asking: [[[obj_ia+0x68]]+8].
        const uintptr_t a = *reinterpret_cast<const uintptr_t*>(obj_ia + 0x68);
        const uintptr_t b = a ? *reinterpret_cast<const uintptr_t*>(a) : 0;
        const uintptr_t enemy = b ? *reinterpret_cast<const uintptr_t*>(b + 8) : 0;
        if (!enemy) return -1;
        const uint32_t enemy_key = *reinterpret_cast<const uint32_t*>(enemy + off::kActor_Key);
        if (!keys::is_game_world_key(enemy_key)) return -1;
        const uintptr_t ecomps = *reinterpret_cast<const uintptr_t*>(enemy + off::kActor_ComponentTable);
        const uintptr_t eai = ecomps ? *reinterpret_cast<const uintptr_t*>(ecomps + 0x58) : 0;
        const uintptr_t esub = eai ? *reinterpret_cast<const uintptr_t*>(eai + 0x50) : 0;
        if (!esub) return -1;
        const uint8_t t = *reinterpret_cast<const uint8_t*>(esub + 0x40);
        if (t > 3) return -1;
        const uintptr_t gcomps = *reinterpret_cast<const uintptr_t*>(target + off::kActor_ComponentTable);
        const uintptr_t c = gcomps ? *reinterpret_cast<const uintptr_t*>(gcomps + 0x90) : 0;
        if (!c) return -1;
        // Attackers of type t: {ptr, u32 n, u32 cap} at C+0x98+16t, 8-byte entries {u32 key, ...}.
        const uintptr_t list = *reinterpret_cast<const uintptr_t*>(c + 0x98 + 16 * t);
        const uint32_t n_t = *reinterpret_cast<const uint32_t*>(c + 0xA0 + 16 * t);
        if (!list || n_t > 256) return -1;
        int index = -1;
        for (uint32_t i = 0; i < n_t; ++i) {
            if (*reinterpret_cast<const uint32_t*>(list + 8 * i) == enemy_key) {
                index = static_cast<int>(i);
                break;
            }
        }
        if (index < 0) return -1;  // not an attacker of the copy: as the game says
        const uint32_t n = *reinterpret_cast<const uint32_t*>(c + 0xA0) + *reinterpret_cast<const uint32_t*>(c + 0xB0);
        if (n <= 2) return 1;
        // The turns go round every few seconds, as the game's attack quota does
        // for our player (live test: with "the first N" one bandit chief gave
        // 26 of 41 blows).
        const uint32_t turn = static_cast<uint32_t>(GetTickCount64() / kTurnMs) % n_t;
        const uint32_t place = (static_cast<uint32_t>(index) + n_t - turn) % n_t;
        return place < static_cast<uint32_t>(slots) ? 0 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uint8_t cond_param(uintptr_t cond) {
    __try {
        return *reinterpret_cast<const uint8_t*>(cond + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFF;
    }
}

uintptr_t ctx_target(uintptr_t ctx) {
    __try {
        return *reinterpret_cast<const uint8_t*>(ctx + 0x268) ? *reinterpret_cast<const uintptr_t*>(ctx + 0x260) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool __fastcall check_ai_battle_type(uintptr_t cond, uintptr_t obj_ia, uintptr_t ctx) {
    const bool original = g_battle_type.call<bool>(cond, obj_ia, ctx);
    const uintptr_t target = ctx_target(ctx);
    if (!target || key_of_actor(target) != kPuppetPlayer) {
        g_bt_other.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    g_bt_copy.fetch_add(1, std::memory_order_relaxed);
    if (original) g_bt_copy_wait.fetch_add(1, std::memory_order_relaxed);
    const int slots = g_copy_slots.load(std::memory_order_relaxed);
    if (!original || slots <= 0 || !copy_state_normal(target)) return original;
    const int state = copy_battle_state(obj_ia, ctx, slots);
    const uint8_t param = cond_param(cond);
    if (state < 0 || param == 0xFF) return original;
    const bool result = param != state;
    if (!result) g_bt_overridden.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool __fastcall is_targetable(void* comp, uintptr_t candidate, uint8_t mode) {
    const bool ok = g_is_targetable.call<bool>(comp, candidate, mode);
    const uint32_t key = key_of_actor(candidate);
    const uint32_t bit = 1u << (mode & 31);
    if (key == kPuppetPlayer) {
        g_puppet_candidate.fetch_add(1, std::memory_order_relaxed);
        g_puppet_candidate_modes.fetch_or(bit, std::memory_order_relaxed);
        if (!ok) {
            g_puppet_rejected.fetch_add(1, std::memory_order_relaxed);
            g_puppet_rejected_modes.fetch_or(bit, std::memory_order_relaxed);
            if (g_force.load(std::memory_order_relaxed) && copy_state_normal(candidate)) {
                g_forced.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    } else if (key && key == g_own_key.load(std::memory_order_relaxed)) {
        g_player_candidate.fetch_add(1, std::memory_order_relaxed);
        if (!ok) {
            g_player_rejected.fetch_add(1, std::memory_order_relaxed);
            g_player_rejected_modes.fetch_or(bit, std::memory_order_relaxed);
        }
    }
    return ok;
}

// The fields IsTargetable reads (companeros-mercenarios.md §3.3), for the
// copy next to our own player.
struct Fields {
    uint8_t c2 = 0xEE, type = 0xEE, local = 0xEE;
    uint8_t s276 = 0xEE, s340 = 0xEE, s398 = 0xEE, ts3dc = 0xEE;
    uint32_t s390 = 0xEEEEEEEE, s394 = 0xEEEEEEEE;
    uint32_t key = 0, owner = 0;
};

bool read_fields(uintptr_t a, Fields& f) {
    __try {
        f.key = *reinterpret_cast<const uint32_t*>(a + off::kActor_Key);
        f.owner = *reinterpret_cast<const uint32_t*>(a + 0x90);
        f.local = *reinterpret_cast<const uint8_t*>(a + 0x96);
        f.c2 = *reinterpret_cast<const uint8_t*>(a + 0xC2);
        const auto info = *reinterpret_cast<const uintptr_t*>(a + 0x88);
        if (info) f.type = *reinterpret_cast<const uint8_t*>(info + 1);
        const auto comps = *reinterpret_cast<const uintptr_t*>(a + off::kActor_ComponentTable);
        if (!comps) return true;
        const auto st = *reinterpret_cast<const uintptr_t*>(comps + 0x20);
        if (st) {
            f.s276 = *reinterpret_cast<const uint8_t*>(st + 0x276);
            f.s340 = *reinterpret_cast<const uint8_t*>(st + 0x340);
            f.s390 = *reinterpret_cast<const uint32_t*>(st + 0x390);
            f.s394 = *reinterpret_cast<const uint32_t*>(st + 0x394);
            f.s398 = *reinterpret_cast<const uint8_t*>(st + 0x398);
        }
        const auto ts = *reinterpret_cast<const uintptr_t*>(comps + 0x1A0);
        if (ts) f.ts3dc = *reinterpret_cast<const uint8_t*>(ts + 0x3DC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void log_fields(const char* who, uint32_t key) {
    const auto a = game::find_actor(key);
    Fields f;
    if (!a || !read_fields(*a, f)) {
        log::info("Aggro test: {} 0x{:08X} not readable", who, key);
        return;
    }
    log::info("Aggro test: {} 0x{:08X}: type {} owner 0x{:08X} local(+96) {} +C2 {} | Status +276 {} +340 {} "
              "+390 0x{:X} +394 0x{:X} +398 {} | TS+3DC {}",
              who, f.key, f.type, f.owner, f.local, f.c2, f.s276, f.s340, f.s390, f.s394, f.s398, f.ts3dc);
}

// Dev "bait" (aggro_set_bait): our enemies that pick our own player pick the
// other player's copy instead, so its attacks can be watched on one PC.
std::atomic<bool> g_bait{false};
std::atomic<uint64_t> g_baited{0};

void on_set_target(safetyhook::Context& ctx) {
    const uint32_t own = g_own_key.load(std::memory_order_relaxed);
    if (g_bait.load(std::memory_order_relaxed) && own && static_cast<uint32_t>(ctx.r8) == own) {
        uintptr_t actor = 0;
        __try {
            actor = *reinterpret_cast<const uintptr_t*>(ctx.rcx + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (keys::is_game_world_key(key_of_actor(actor))) {
            if (const auto copy = game::find_actor(kPuppetPlayer); copy && copy_state_normal(*copy)) {
                ctx.r8 = kPuppetPlayer;  // SetCurrentTarget's target argument (r8d)
                g_baited.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (static_cast<uint32_t>(ctx.r8) != kPuppetPlayer) return;
    g_set_to_puppet.fetch_add(1, std::memory_order_relaxed);
    __try {
        const auto actor = *reinterpret_cast<const uintptr_t*>(ctx.rcx + 0x08);
        g_set_to_puppet_by.store(key_of_actor(actor), std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool hook(uintptr_t rva, const uint8_t* head, size_t n, SafetyHookMid& out, safetyhook::MidHookFn fn,
          const char* name) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<uint8_t> got(n);
    if (!game::safe_read(base + rva, got.data(), n) || std::memcmp(got.data(), head, n) != 0) {
        log::warn("Aggro test: {} not recognised", name);
        return false;
    }
    auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(base + rva), fn);
    if (!h) return false;
    out = std::move(*h);
    return true;
}

// u64 [[[actor+0x68]+0x90]+0x170]: the target component's change counter.
bool read_target_seq(uintptr_t actor, uint64_t& seq) {
    __try {
        const auto comps = *reinterpret_cast<const uintptr_t*>(actor + off::kActor_ComponentTable);
        if (!comps) return false;
        const auto comp = *reinterpret_cast<const uintptr_t*>(comps + 0x90);
        if (!comp) return false;
        seq = *reinterpret_cast<const uint64_t*>(comp + 0x170);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void force_target_on_puppet() {
    uint32_t enemy = 0;
    uint8_t reason = 0x18;
    uint64_t when = 0;
    {
        std::lock_guard lock(g_mutex);
        enemy = g_last_enemy;
        reason = g_last_reason;
        when = g_last_enemy_ms;
    }
    if (!puppet_spawned()) {
        log::info("Aggro test: the other player's copy is not in the world");
        return;
    }
    if (!enemy || now_ms() - when > 30000) {
        log::info("Aggro test: no enemy aimed at you in the last 30 s; let one see you first");
        return;
    }
    const auto actor = game::find_actor(enemy);
    uint64_t seq = 0;
    if (!actor || !read_target_seq(*actor, seq)) {
        log::info("Aggro test: enemy 0x{:08X} not readable (gone?)", enemy);
        return;
    }
    if (reason == 0x15) reason = 0x18;  // 0x15 has extra checks
    std::vector<uint8_t> ack(18, 0);
    std::memcpy(ack.data(), &enemy, 4);
    std::memcpy(ack.data() + 4, &kPuppetPlayer, 4);
    const uint64_t next = seq + 1;
    std::memcpy(ack.data() + 8, &next, 8);
    ack[0x10] = 1;  // force: the enemy's AI is local
    ack[0x11] = reason;
    puppet_inject_s2c(kOpCurrentTargetChangedAck, ack);
    log::info("Aggro test: told enemy 0x{:08X} to target the other player's copy (seq {}, reason 0x{:02X})",
              enemy, next, reason);
}

}  // namespace

bool aggro_test_install() {
    refresh_own_key();
    if (g_is_targetable) return true;
    bool a = false;
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uint8_t got[sizeof kIsTargetableHead];
        if (game::safe_read(base + kIsTargetable, got, sizeof got) &&
            std::memcmp(got, kIsTargetableHead, sizeof got) == 0) {
            g_is_targetable = safetyhook::create_inline(reinterpret_cast<void*>(base + kIsTargetable),
                                                        reinterpret_cast<void*>(is_targetable));
            a = static_cast<bool>(g_is_targetable);
        } else {
            log::warn("Aggro test: IsTargetable not recognised");
        }
    }
    const bool b = hook(kSetCurrentTarget, kSetCurrentTargetHead, sizeof kSetCurrentTargetHead, g_set_target,
                        on_set_target, "SetCurrentTarget");
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uint8_t got[sizeof kCheckAIBattleTypeHead];
        if (game::safe_read(base + kCheckAIBattleType, got, sizeof got) &&
            std::memcmp(got, kCheckAIBattleTypeHead, sizeof got) == 0) {
            g_battle_type = safetyhook::create_inline(reinterpret_cast<void*>(base + kCheckAIBattleType),
                                                      reinterpret_cast<void*>(check_ai_battle_type));
        }
        log::info("Aggro: CheckAIBattleType {}", g_battle_type ? "hooked" : "NOT recognised");
    }
    log::info("Aggro test: probes {} / {}; Ctrl+F5 = make the last enemy that aimed at you target the "
              "other player's copy; Ctrl+F11 = force the copy to count as a valid target",
              a ? "on" : "off", b ? "on" : "off");
    return a && b;
}

void aggro_set_bait(bool on) {
    g_bait = on;
    log::info("Aggro: bait {} (our enemies {} the other player's copy instead of us)", on ? "ON" : "OFF",
              on ? "pick" : "no longer pick");
}

bool aggro_bait() { return g_bait.load(); }
uint64_t aggro_baited() { return g_baited.load(); }

void aggro_set_copy_attack_slots(int slots) {
    g_copy_slots = slots;
    log::info("Co-op: up to {} of our enemies at a time may attack the other player's copy{}", slots,
              slots <= 0 ? " (the game decides: they mostly wait)" : "");
}

int aggro_copy_attack_slots() { return g_copy_slots.load(); }

std::string aggro_battle_type_stats() {
    return std::format("CheckAIBattleType: copy {} (wait {}, made to attack {}) | others {}", g_bt_copy.load(),
                       g_bt_copy_wait.load(), g_bt_overridden.load(), g_bt_other.load());
}

// The target probe (analisis-ia2/P2 §7): after a request, what the enemy's
// AI did for 3 s: its target changes (C>S CurrentTargetChangeReq, ms after
// the request) and its attack frames on the copy and on our player.
struct TargetProbe {
    uint64_t asked_ms = 0;
    std::string changes;
    int on_copy = 0, on_us = 0;
};
std::map<uint32_t, TargetProbe> g_target_probes;  // under g_mutex
uint64_t g_target_probe_logs = 0;

// Caller holds g_mutex: probes over 3 s old are written out.
void flush_target_probes_locked(uint64_t now) {
    for (auto it = g_target_probes.begin(); it != g_target_probes.end();) {
        if (now - it->second.asked_ms < 3000) {
            ++it;
            continue;
        }
        if (g_target_probe_logs++ < 60) {
            log::info("Target probe: enemy 0x{:08X} told to target the other player's copy: target changes in 3 s:{} | "
                      "attack frames on their copy {}, on us {}",
                      it->first, it->second.changes.empty() ? std::string(" none") : it->second.changes,
                      it->second.on_copy, it->second.on_us);
        }
        it = g_target_probes.erase(it);
    }
}

bool aggro_request_target_on_copy(uint32_t enemy) {
    if (!g_force.load(std::memory_order_relaxed) || !keys::is_game_world_key(enemy)) return false;
    static std::mutex m;
    static std::map<uint32_t, uint64_t> last;  // enemy -> when asked
    static uint64_t logged = 0;
    const uint64_t now = now_ms();
    {
        std::lock_guard lock(m);
        if (auto l = last.find(enemy); l != last.end() && now - l->second < 2000) return false;
    }
    const auto copy = game::find_actor(kPuppetPlayer);
    const auto actor = game::find_actor(enemy);
    uint64_t seq = 0;
    if (!copy || !copy_state_normal(*copy) || !actor || !read_target_seq(*actor, seq)) return false;
    {
        std::lock_guard lock(m);
        if (last.size() > 256) std::erase_if(last, [&](const auto& e) { return now - e.second >= 2000; });
        last[enemy] = now;
    }
    std::vector<uint8_t> ack(18, 0);
    std::memcpy(ack.data(), &enemy, 4);
    std::memcpy(ack.data() + 4, &kPuppetPlayer, 4);
    const uint64_t next = seq + 1;
    std::memcpy(ack.data() + 8, &next, 8);
    ack[0x10] = 1;     // force: the enemy's AI is local
    ack[0x11] = 0x18;  // the reason seen when an enemy picks a player that attacks it
    puppet_inject_s2c(kOpCurrentTargetChangedAck, ack);
    {
        std::lock_guard lock(g_mutex);
        flush_target_probes_locked(now);
        if (g_target_probes.size() < 64 && !g_target_probes.count(enemy)) g_target_probes[enemy] = {now, {}, 0, 0};
    }
    if (logged++ < 30 || logged % 100 == 0) {
        log::info("Co-op: enemy 0x{:08X} hit by the other player now targets their copy (seq {}) [{} so far]", enemy,
                  next, logged);
    }
    return true;
}

void aggro_set_copy_targetable(bool on) {
    g_force = on;
    log::info("Co-op: the other player's copy {} a valid target for our enemies", on ? "is" : "is not");
}

void aggro_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5) return;
    refresh_own_key();
    uint16_t op = 0;
    std::memcpy(&op, packet, 2);
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    if (op == kOpCurrentTargetChangeReq && n >= 18) {
        uint32_t actor = 0, target = 0;
        std::memcpy(&actor, p, 4);
        std::memcpy(&target, p + 4, 4);
        std::lock_guard lock(g_mutex);
        if (auto t = g_target_probes.find(actor); t != g_target_probes.end() && t->second.changes.size() < 200) {
            t->second.changes += std::format(" +{} ms {}", now_ms() - t->second.asked_ms,
                                             target == kPuppetPlayer                ? std::string("their copy")
                                             : target == capture_own_player_key() ? std::string("us")
                                                                                  : std::format("0x{:08X}", target));
        }
        flush_target_probes_locked(now_ms());
        if (target == capture_own_player_key() && keys::is_game_world_key(actor) && capture_is_enemy(actor)) {
            g_last_enemy = actor;
            g_last_reason = p[0x11];
            g_last_enemy_ms = now_ms();
        }
        if (target == kPuppetPlayer && g_logged_req < 60) {
            ++g_logged_req;
            log::info("Aggro test: enemy 0x{:08X} now targets the other player's copy (reason 0x{:02X})", actor,
                      p[0x11]);
        } else if (actor == g_last_enemy && g_logged_req > 0 && g_logged_req < 60) {
            ++g_logged_req;
            log::info("Aggro test: enemy 0x{:08X} switched its target to 0x{:08X}", actor, target);
        }
    } else if (op == kOpDoFrameEventReq && n >= 8) {
        if (p[0] == 4) {
            uint32_t attacker = 0;
            std::memcpy(&attacker, p + 1, 4);
            std::lock_guard lock(g_mutex);
            if (auto t = g_target_probes.find(attacker); t != g_target_probes.end()) {
                const uint32_t own = capture_own_player_key();
                bool copy = false, us = false;
                for (size_t i = 5; i + 4 <= n; ++i) {
                    uint32_t v = 0;
                    std::memcpy(&v, p + i, 4);
                    copy = copy || v == kPuppetPlayer;
                    us = us || (own && v == own);
                }
                t->second.on_copy += copy;
                t->second.on_us += us;
            }
        }
        // An attack frame whose target list names the puppet.
        for (size_t i = 0; i + 4 <= n; ++i) {
            uint32_t v = 0;
            std::memcpy(&v, p + i, 4);
            if (v == kPuppetPlayer) {
                std::lock_guard lock(g_mutex);
                if (g_logged_hits++ < 60) {
                    uint32_t attacker = 0;
                    std::memcpy(&attacker, p + 1, 4);
                    log::info("Aggro test: ATTACK frame aimed at the other player's copy (attacker 0x{:08X}, "
                              "{} bytes, key at +0x{:X})",
                              attacker, n, i);
                }
                break;
            }
        }
    }
}

void aggro_test_loop(void* stop) {
    bool was = false;
    uint64_t last_report = 0;
    uint64_t seen_puppet = 0, seen_player = 0, seen_set = 0;
    bool was_f11 = false;
    uint64_t last_fields = 0;
    while (WaitForSingleObject(static_cast<HANDLE>(stop), 50) == WAIT_TIMEOUT) {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000);
        if (down && !was) force_target_on_puppet();
        was = down;
        const bool f11 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (f11 && !was_f11) {
            const bool on = !g_force.load();
            g_force = on;
            log::info("Aggro test: forcing the copy as a valid target {}", on ? "ON" : "OFF");
        }
        was_f11 = f11;
        refresh_own_key();
        const uint64_t t = now_ms();
        if (t - last_fields >= 15000 && puppet_spawned()) {
            last_fields = t;
            log_fields("copy", kPuppetPlayer);
            if (const uint32_t own = capture_own_player_key()) log_fields("you ", own);
        }
        if (t - last_report >= 5000) {
            last_report = t;
            const uint64_t a = g_puppet_candidate.load(), b = g_player_candidate.load(), c = g_set_to_puppet.load();
            if (a != seen_puppet || b != seen_player || c != seen_set) {
                log::info("Aggro test: checks on the copy {} (modes 0x{:X}), REJECTED {} (modes 0x{:X}), "
                          "forced {} | on you {}, rejected {} (modes 0x{:X}) | targets set to the copy {} "
                          "(last by 0x{:08X})",
                          a, g_puppet_candidate_modes.load(), g_puppet_rejected.load(),
                          g_puppet_rejected_modes.load(), g_forced.load(), b, g_player_rejected.load(),
                          g_player_rejected_modes.load(), c, g_set_to_puppet_by.load());
                seen_puppet = a;
                seen_player = b;
                seen_set = c;
            }
        }
    }
}

bool aggro_copy_dead() {
    const auto actor = game::find_actor(kPuppetPlayer);
    return actor && copy_dead_at(*actor);
}

}  // namespace cdmp::net
