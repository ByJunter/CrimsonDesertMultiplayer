#include "mod/net/piece4_test.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mod/game/actor_lookup.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"
#include "common/keys.h"
#include "mod/net/local_capture.h"
#include "mod/net/player_hits.h"
#include "mod/net/troc_sniffer.h"
#include "mod/net/remote_puppet.h"

namespace cdmp::net {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kOpAttackTargetAck = 2922;            // +0 target | +4 attacker | +0x24 attack hash
constexpr uint16_t kOpApplyActionRestrictionAck = 2162;  // 77 B: +0 target | +0x24 instance | +0x49 source
constexpr uint16_t kOpApplyBuffAck = 2506;               // 77 B: +0 holder | +4 buff | +0x14 instance | +0x18 source |
                                                         // +0x1C holder | +0x29 source position
constexpr uint16_t kOpStartActionReq = 2223;             // +0 actor | +4 previous action | +8 new action
constexpr size_t kBatchSize = 77;
constexpr uint32_t kPuppetPlayer = 0xA010F001;

std::mutex g_mutex;
// The latest real staggering batch of our own blow, being assembled and complete.
struct Batch {
    uint32_t target = 0, instance = 0;
    uint64_t ms = 0;
    Bytes ata, aar, buff;
};
Batch g_partial, g_batch;
uint32_t g_next_instance = 0;  // private instance ids: 0x8000D000 + n (guest_hits uses 0x8000F000)
// The enemy under test: log its next actions.
uint32_t g_watch = 0;
uint64_t g_watch_ms = 0;
int g_watch_left = 0;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

struct Target {
    uint32_t key = 0;
    float distance = 0;
};

std::optional<Target> pick_target() {
    const uint32_t own = capture_own_player_key();
    const auto me = own ? game::actor_world_position(own) : std::nullopt;
    if (!me) return std::nullopt;
    auto dist = [&](const proto::Vec3& p) {
        return std::sqrt((p.x - me->x) * (p.x - me->x) + (p.y - me->y) * (p.y - me->y) + (p.z - me->z) * (p.z - me->z));
    };
    if (const uint32_t hit = capture_last_attack_target(); keys::is_game_world_key(hit)) {
        if (const auto p = game::actor_world_position(hit); p && dist(*p) < 15.0f) return Target{hit, dist(*p)};
    }
    if (const auto n = capture_nearest_actor(*me, 15.0f)) return Target{n->key, n->distance};
    return std::nullopt;
}

// `attacker`: the AttackTargetAck's attacker (nullopt = send no AttackTargetAck);
// `source`: the restriction's and the hit buff's source.
void stagger_test(const char* what, std::optional<uint32_t> attacker, uint32_t source) {
    const auto t = pick_target();
    Batch b;
    {
        std::lock_guard lock(g_mutex);
        b = g_batch;
    }
    if (!t || b.buff.empty()) {
        log::info("Piece 4 test {}: {}", what,
                  !t ? "no enemy within 15 m" : "no real staggering blow of ours seen yet (hit an enemy first)");
        return;
    }
    if ((attacker == kPuppetPlayer || source == kPuppetPlayer) && !puppet_spawned()) {
        log::info("Piece 4 test {}: the other player's copy is not in the world", what);
        return;
    }
    const uint32_t instance = 0x8000D000u + (g_next_instance++ & 0xFFF);
    std::memcpy(b.aar.data(), &t->key, 4);
    std::memcpy(b.aar.data() + 0x24, &instance, 4);
    std::memcpy(b.aar.data() + 0x49, &source, 4);
    std::memcpy(b.buff.data(), &t->key, 4);
    std::memcpy(b.buff.data() + 0x14, &instance, 4);
    std::memcpy(b.buff.data() + 0x18, &source, 4);
    std::memcpy(b.buff.data() + 0x1C, &t->key, 4);
    if (const auto pos = game::actor_world_position(source ? source : capture_own_player_key())) {
        const float xyz[3] = {pos->x, pos->y, pos->z};
        std::memcpy(b.buff.data() + 0x29, xyz, sizeof xyz);
    }
    const uint32_t buff_id = u32_at(b.buff.data() + 4);
    {
        std::lock_guard lock(g_mutex);
        g_watch = t->key;
        g_watch_ms = GetTickCount64();
        g_watch_left = 4;
    }
    std::vector<std::pair<uint16_t, Bytes>> batch;
    if (attacker) {
        std::memcpy(b.ata.data(), &t->key, 4);
        std::memcpy(b.ata.data() + 4, &*attacker, 4);
        batch.emplace_back(kOpAttackTargetAck, std::move(b.ata));
    }
    batch.emplace_back(kOpApplyActionRestrictionAck, std::move(b.aar));
    batch.emplace_back(kOpApplyBuffAck, std::move(b.buff));
    puppet_inject_s2c_batch(batch);
    log::info("Piece 4 test {}: enemy 0x{:08X} ({:.1f} m) got our last staggering batch (buff {}): {}, restriction "
              "source 0x{:08X}, instance 0x{:08X}",
              what, t->key, t->distance, buff_id,
              attacker ? std::format("AttackTargetAck from 0x{:08X}", *attacker) : std::string("no AttackTargetAck"),
              source, instance);
}

// D1/D2: can our own player die natively, as a normal death (not the cheat
// path, whose death telemetry says "DevCheat", pendiente-recompensas-progreso.md)?
// Down to 1 HP through the generic stat path, then a collision on us like the
// one that finishes enemies (pieza4-botin-reaccion.md X2), from `attacker`.
void die_test(const char* what, uint32_t attacker) {
    const uint32_t own = capture_own_player_key();
    const auto me = own ? game::actor_world_position(own) : std::nullopt;
    if (!me) return;
    // 1) to 1 HP: VaryStatByAccInputReq never kills (it would leave 0 = knocked out).
    const int64_t hp = hits_own_hp();
    if (hp > 1000) {
        Bytes b(16);
        const int64_t delta = -(hp - 1000);
        const uint32_t stat = 0x000F4240;
        std::memcpy(b.data(), &own, 4);
        std::memcpy(b.data() + 4, &stat, 4);
        std::memcpy(b.data() + 8, &delta, 8);
        Bytes pkt(5 + b.size());
        const uint16_t op = 2181, len = 16;
        std::memcpy(pkt.data(), &op, 2);
        std::memcpy(pkt.data() + 3, &len, 2);
        std::memcpy(pkt.data() + 5, b.data(), b.size());
        sniffer_inject_c2s(std::move(pkt));
        Sleep(600);
    }
    // 2) the collision: victim, attacker, u32 0, vec3 point, vec3 direction, u32 ProjectileInfo, u32 0.
    Bytes b(44, 0);
    const float point[3] = {me->x, me->y + 1.0f, me->z};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    const uint32_t projectile = 0xF2EE2678;
    std::memcpy(b.data(), &own, 4);
    std::memcpy(b.data() + 4, &attacker, 4);
    std::memcpy(b.data() + 12, point, sizeof point);
    std::memcpy(b.data() + 24, dir, sizeof dir);
    std::memcpy(b.data() + 36, &projectile, 4);
    Bytes pkt(5 + b.size());
    const uint16_t op = 2815, len = static_cast<uint16_t>(b.size());
    std::memcpy(pkt.data(), &op, 2);
    std::memcpy(pkt.data() + 3, &len, 2);
    std::memcpy(pkt.data() + 5, b.data(), b.size());
    const bool ok = sniffer_inject_c2s(std::move(pkt));
    log::info("Piece 4 test {}: our player from {:.1f} hp to 1, then a collision from 0x{:08X} {}", what,
              hp / 1000.0, attacker, ok ? "queued" : "NOT queued");
}

}  // namespace

void piece4_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + kBatchSize) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpAttackTargetAck && op != kOpApplyActionRestrictionAck && op != kOpApplyBuffAck) return;
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    const uint32_t own = capture_own_player_key();
    const uint32_t target = u32_at(p);
    if (!own || !keys::is_game_world_key(target)) return;
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    if (op == kOpAttackTargetAck) {
        if (u32_at(p + 4) != own) return;
        g_partial = {target, 0, now, Bytes(p, p + n), {}, {}};
    } else if (op == kOpApplyActionRestrictionAck) {
        if (n != kBatchSize || u32_at(p + 0x49) != own || g_partial.target != target || now - g_partial.ms > 20) return;
        g_partial.instance = u32_at(p + 0x24);
        g_partial.aar.assign(p, p + n);
    } else if (n == kBatchSize && !g_partial.aar.empty() && g_partial.target == target &&
               u32_at(p + 0x14) == g_partial.instance && u32_at(p + 0x18) == own) {
        g_partial.buff.assign(p, p + n);
        g_batch = std::move(g_partial);
        g_partial = {};
    }
}

void piece4_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5 + 12) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpStartActionReq) return;
    const uint8_t* p = packet + 5;
    uint64_t ago = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_watch || u32_at(p) != g_watch) return;
        ago = GetTickCount64() - g_watch_ms;
        if (ago > 1500 || g_watch_left-- <= 0) {
            g_watch = 0;
            return;
        }
    }
    // A73D6EC4 = end of a hit reaction; a real stagger is 3C4A7F1D, 90A9C337, 4A641121... (pieza4-tambaleo.md §4.2).
    log::info("Piece 4 test: enemy 0x{:08X} {} ms later: action {:08X} -> {:08X}{}", u32_at(p), ago, u32_at(p + 4),
              u32_at(p + 8), u32_at(p + 8) == 0xA73D6EC4 ? " (end of reaction: NO stagger)" : "");
}

void piece4_test_loop(void* stop) {
    log::warn("Piece 4 stagger tests ON: Ctrl+F10 = R1e (hit from the copy, restriction from nobody), "
              "Ctrl+Shift+F10 = R1f (no hit, restriction from the copy), Ctrl+F11 = D1 (we die: 1 HP + "
              "collision from nobody), Ctrl+Shift+F11 = D2 (the same, from ourselves). "
              "They WRITE to the game: do not save afterwards");
    bool was10 = false, was11 = false;
    while (WaitForSingleObject(static_cast<HANDLE>(stop), 50) == WAIT_TIMEOUT) {
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool f10 = ctrl && (GetAsyncKeyState(VK_F10) & 0x8000);
        const bool f11 = ctrl && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (f10 && !was10) {
            if (shift) stagger_test("R1f", std::nullopt, kPuppetPlayer);
            else stagger_test("R1e", kPuppetPlayer, 0);
        }
        if (f11 && !was11) {
            if (shift) die_test("D2", capture_own_player_key());
            else die_test("D1", 0);
        }
        was10 = f10;
        was11 = f11;
    }
}

}  // namespace cdmp::net
