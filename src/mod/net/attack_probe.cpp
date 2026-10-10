#include "mod/net/attack_probe.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mod/game/actor_lookup.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/aggro_test.h"
#include "mod/net/local_capture.h"

namespace cdmp::net {
namespace {

namespace off = game::build_2976;

constexpr uint16_t kOpCurrentTargetChangeReq = 2984;  // u32 actor | u32 target | ...
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr uint64_t kRecentMs = 5000;
constexpr uint32_t kMaxListed = 16;

std::mutex g_mutex;
std::map<uint32_t, std::pair<uint32_t, uint64_t>> g_targets;  // enemy -> (its target, when)

// A {ptr, u32 n, u32 cap} list of 8-byte entries starting with a key.
struct List {
    uint32_t n = 0;
    std::vector<uint32_t> keys;
};

List read_list(uintptr_t at, size_t entry) {
    List l;
    const auto ptr = game::read_ptr(at);
    const auto n = game::read<uint32_t>(at + 8);
    if (!n || *n > 4096) return l;
    l.n = *n;
    if (!ptr) return l;
    for (uint32_t i = 0; i < l.n && i < kMaxListed; ++i) {
        if (const auto k = game::read<uint32_t>(*ptr + i * entry)) l.keys.push_back(*k);
    }
    return l;
}

struct Target {
    bool ok = false;
    uint8_t mode = 0xEE, order = 0xEE, battle = 0xEE;
    bool has_ai = false;
    uint32_t current = 0;  // its own current target (+0x178)
    List other[4], rank[4], attackers[4], aggro[2];
};

std::optional<uintptr_t> comps_of(uint32_t key) {
    const auto a = game::find_actor(key);
    if (!a) return std::nullopt;
    return game::read_ptr(*a + off::kActor_ComponentTable);
}

uint8_t order_of(uintptr_t comps) {
    const auto ai = game::read_ptr(comps + 0x58);
    const auto sub = ai ? game::read_ptr(*ai + 0x50) : std::nullopt;
    const auto order = sub ? game::read<uint8_t>(*sub + 0x40) : std::nullopt;
    return order ? *order : 0xEE;
}

Target read_target(uint32_t key) {
    Target t;
    const auto a = game::find_actor(key);
    if (!a) return t;
    if (const auto m = game::read<uint8_t>(*a + 0xC8)) t.mode = *m;
    const auto comps = game::read_ptr(*a + off::kActor_ComponentTable);
    if (!comps) return t;
    t.has_ai = game::read_ptr(*comps + 0x58).has_value();
    t.order = order_of(*comps);
    const auto c = game::read_ptr(*comps + 0x90);
    if (!c) return t;
    t.ok = true;
    for (int i = 0; i < 4; ++i) {
        t.other[i] = read_list(*c + 0x18 + 16 * i, 8);
        t.rank[i] = read_list(*c + 0x58 + 16 * i, 8);
        t.attackers[i] = read_list(*c + 0x98 + 16 * i, 8);
    }
    t.aggro[0] = read_list(*c + 0x120, 20);
    t.aggro[1] = read_list(*c + 0x130, 20);
    if (const auto b = game::read<uint8_t>(*c + 0x164)) t.battle = *b;
    if (const auto cur = game::read<uint32_t>(*c + 0x178)) t.current = *cur;
    return t;
}

std::string counts(const List (&l)[4]) {
    return std::format("{}/{}/{}/{}", l[0].n, l[1].n, l[2].n, l[3].n);
}

std::string keys_text(const List (&l)[4]) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        for (uint32_t k : l[i].keys) s += std::format(" {}:{:08X}", i, k);
    }
    return s.empty() ? std::string(" -") : s;
}

bool listed(const List (&l)[4], uint32_t key) {
    for (const auto& x : l) {
        for (uint32_t k : x.keys) {
            if (k == key) return true;
        }
    }
    return false;
}

void log_target(const char* who, uint32_t key, const Target& t) {
    if (!t.ok) {
        log::info("Attack probe {} 0x{:08X}: not readable (mode {}, AI {})", who, key, t.mode, t.has_ai);
        return;
    }
    log::info("Attack probe {} 0x{:08X}: mode {} AI {} order {} battle {} target 0x{:08X} | rank {} [{}] | "
              "attackers {} [{}] | other {} | aggro {}/{}",
              who, key, t.mode, t.has_ai, t.order, t.battle, t.current, counts(t.rank), keys_text(t.rank),
              counts(t.attackers), keys_text(t.attackers), counts(t.other), t.aggro[0].n, t.aggro[1].n);
}

void probe() {
    const uint32_t own = capture_own_player_key();
    if (!own) return;
    const Target p = read_target(own);
    const Target g = read_target(kPuppetPlayer);
    log_target("P", own, p);
    log_target("G", kPuppetPlayer, g);
    std::vector<std::pair<uint32_t, uint32_t>> recent;
    {
        std::lock_guard lock(g_mutex);
        const uint64_t now = GetTickCount64();
        for (auto it = g_targets.begin(); it != g_targets.end();) {
            if (now - it->second.second > 60000) {
                it = g_targets.erase(it);
                continue;
            }
            if (now - it->second.second < kRecentMs || it->second.first == kPuppetPlayer) {
                recent.emplace_back(it->first, it->second.first);
            }
            ++it;
        }
    }
    for (const auto& [enemy, aimed] : recent) {
        const auto comps = comps_of(enemy);
        if (!comps) continue;
        uint32_t current = 0;
        if (const auto c = game::read_ptr(*comps + 0x90)) {
            if (const auto cur = game::read<uint32_t>(*c + 0x178)) current = *cur;
        }
        if (current != kPuppetPlayer && aimed != kPuppetPlayer) continue;
        const auto e = game::actor_world_position(enemy);
        const auto gp = game::actor_world_position(kPuppetPlayer);
        const float dist = e && gp ? std::sqrt((e->x - gp->x) * (e->x - gp->x) + (e->z - gp->z) * (e->z - gp->z)) : -1;
        const float dy = e && gp ? e->y - gp->y : 0;
        log::info("Attack probe E 0x{:08X}: target now 0x{:08X} (last CTC 0x{:08X}) order {} | {:.1f} m from G, "
                  "dY {:.2f} | in G rank {} attackers {} | in P rank {} attackers {}",
                  enemy, current, aimed, order_of(*comps), dist, dy, listed(g.rank, enemy), listed(g.attackers, enemy),
                  listed(p.rank, enemy), listed(p.attackers, enemy));
    }
}

}  // namespace

void attack_probe_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5 + 8) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpCurrentTargetChangeReq) return;
    uint32_t actor, target;
    std::memcpy(&actor, packet + 5, 4);
    std::memcpy(&target, packet + 9, 4);
    std::lock_guard lock(g_mutex);
    g_targets[actor] = {target, GetTickCount64()};
}

void attack_probe_loop(void* stop) {
    log::warn("Attack probe ON (read only): every 2 s, the attacker lists of our player (P), the other player's "
              "copy (G) and the enemies aiming at G; Ctrl+F9 = enemies attack the copy (on/off); Ctrl+F10 = "
              "bait: enemies go for the copy instead of us (on/off)");
    uint64_t last_probe = 0, last_stats = 0;
    bool was9 = false, was10 = false;
    const int slots = aggro_copy_attack_slots() > 0 ? aggro_copy_attack_slots() : 2;
    while (WaitForSingleObject(static_cast<HANDLE>(stop), 50) == WAIT_TIMEOUT) {
        const uint64_t now = GetTickCount64();
        const bool f9 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (f9 && !was9) aggro_set_copy_attack_slots(aggro_copy_attack_slots() > 0 ? 0 : slots);
        was9 = f9;
        const bool f10 = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F10) & 0x8000);
        if (f10 && !was10) aggro_set_bait(!aggro_bait());
        was10 = f10;
        if (now - last_probe >= 2000) {
            last_probe = now;
            probe();
        }
        if (now - last_stats >= 10000) {
            last_stats = now;
            log::info("Attack probe: {} (override {}) | bait {} ({} targets moved to the copy)",
                      aggro_battle_type_stats(), aggro_copy_attack_slots() > 0 ? "on" : "off",
                      aggro_bait() ? "on" : "off", aggro_baited());
        }
    }
}

}  // namespace cdmp::net
