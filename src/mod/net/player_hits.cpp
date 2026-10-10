#include "mod/net/player_hits.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <format>
#include <iterator>
#include <mutex>
#include <string>

#include "mod/game/actor_lookup.h"
#include "mod/log.h"
#include "mod/net/aggro_test.h"
#include "mod/net/enemy_relay.h"
#include "common/keys.h"
#include "mod/net/local_capture.h"
#include "mod/net/remote_puppet.h"
#include "mod/net/troc_sniffer.h"

namespace cdmp::net {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kOpDoFrameEventReq = 2380;
constexpr uint16_t kOpVaryHPAck = 2147;
constexpr uint16_t kOpVaryStatByAccInputReq = 2181;
constexpr uint16_t kOpAttackTargetAck = 2922;            // +0 target | +4 attacker
constexpr uint16_t kOpApplyActionRestrictionAck = 2162;  // 77 B: +0 target | +0x24 instance | +0x49 source
constexpr uint16_t kOpApplyBuffAck = 2506;               // 77 B: +0 holder | +4 buff | +0x14 instance |
                                                         // +0x18 source | +0x1C holder | +0x29 source pos |
                                                         // +0x39/+0x41 server ms
constexpr size_t kBatchSize = 77;
constexpr size_t kMinAtaSize = 0xA0;   // real ones are 199 B (211 from a projectile or a rider)
constexpr size_t kMaxAtaSize = 0x300;  // so a PlayerHit with its templates stays under kMaxEventPayload
// PlayerHit wire (proto::EventKind::PlayerHit): u32 attacker copy | u32 action hash |
// i64 damage x1000 (0 = unknown) | then, optionally: u8 flags | u16 ATA size | ATA |
// [AAR (77) | hit buff (77) when kHitStaggers]. In the templates the sender's
// own player is proto::kSelfMarker and the attacker is already its copy key.
constexpr size_t kHitBaseSize = 16;
constexpr uint8_t kHitHasTemplates = 1, kHitStaggers = 2;
constexpr uint32_t kStatHP = 0x000F4240;
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr size_t kTargetEntry = 665;     // DoFrameEventReq target entry size
constexpr size_t kAccessor = 0x1F7;      // FrameEventAccessor: u8 type | ...
constexpr size_t kAccessorHash = 0x1F9;  // type 0: u8 type | u8 slot | u32 action hash
constexpr size_t kFirstEntry = 0x203;
constexpr uint64_t kLearnWindowMs = 600;  // a blow on us and its HP loss
constexpr double kDefaultShare = 0.05;    // unknown attack: 5% of max HP

// A PlayerHit hurts OUR player through our own server on the other player's
// word, so it is bounded (security review H4, H5, seguridad-combate.md):
// - from a copy of one of their enemies loaded here and near us; the event
//   does not say whether it was an arrow or a musket, so one generous reach;
constexpr float kMaxHitDistance = 100.0f;
// - at most 6 blows at once, then 5 a second (8 copies may hit together);
// - one blow at most half our max HP (as before), and all blows of the last
//   second together at most half as well;
constexpr uint64_t kDamageWindowMs = 1000;
// - a lethal blow only after another blow in the last 5 s, never alone.
constexpr uint64_t kLethalAfterMs = 5000;
// The hit buffs and their restriction keys seen on real blows (CC_Push...,
// pieza4-tambaleo.md §4.1, pendiente-reaccion-muerte-invitado.md §1.2): any
// other buff or restriction from the other side is not applied to us.
constexpr uint32_t kHitBuffs[] = {7, 1104, 1105, 1106, 1107, 1109, 1112, 1120, 10159, 12031, 15033, 60008};
constexpr uint32_t kHitRestrictions[] = {0x79156D1B, 0x67AE8750, 0xF931B81A, 0xFFCFA077,
                                         0x58B15AA0, 0xE55D252F, 0x5A6EBE95, 0x38121DC3};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_react{false};   // [coop] player_hit_react
std::atomic<bool> g_lethal{false};  // [coop] player_hit_lethal
std::mutex g_mutex;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool is_key_shape(uint32_t v) { return keys::is_class(v, keys::kClassWorld) || keys::is_class(v, keys::kClassPlayer); }

uint64_t now_ms() { return GetTickCount64(); }

// Enemy projectiles by ProjectileInfo key: HP they took from the host's player
// (pendiente-proyectiles.md §2.2), for when nothing was learnt yet.
int64_t seed_projectile_damage(uint32_t pkey) {
    switch (pkey) {
        case 0x719C2056: return 26000;  // Bleed Bandit archers
        case 0x84637121: return 6000;   // Wolf Pursuer muskets
        case 0x53ADD11E: return 60000;
        case 0x8903CF57: return 59000;
        default: return 0;
    }
}

// ---- sending side
struct Pending {
    uint32_t attacker = 0;
    uint64_t key = 0;  // variant << 32 | action hash
    uint64_t ms = 0;
};
std::deque<Pending> g_pending;            // blows on our player waiting for their HP loss
std::map<uint64_t, int64_t> g_damage;     // variant << 32 | hash -> HP x1000 it took
std::map<uint32_t, int64_t> g_by_variant;  // variant -> last HP x1000 any of its attacks took
int64_t g_own_hp = -1;                     // our player's HP x1000 (last VaryHPAck)
// How our player reacts to each enemy attack, from the blows it takes: the
// server's batch AttackTargetAck + ApplyActionRestrictionAck + hit buff
// (pendiente-reaccion-muerte-invitado.md §4; pieza4-tambaleo.md).
struct Reaction {
    uint32_t blows = 0, staggers = 0;
    Bytes ata, aar, buff;
};
std::map<uint64_t, Reaction> g_reactions;     // variant << 32 | hash -> templates
std::map<uint32_t, uint64_t> g_variant_batch;  // variant -> key of its latest complete staggering batch
uint64_t g_last_batch_key = 0;                  // the latest complete staggering batch of any enemy
uint64_t g_learnt_reactions = 0;
struct OpenBatch {
    uint64_t key = 0, ms = 0;
    uint32_t attacker = 0, instance = 0;
    Bytes aar;
};
OpenBatch g_open;  // the batch of the latest blow on us, being assembled
uint64_t g_server_ms = 0, g_server_ms_at = 0;  // server game clock (ApplyBuffAck +0x39)
std::deque<proto::GameEvent> g_out;
uint64_t g_hits_sent = 0, g_learnt = 0;

// ---- receiving side
int64_t g_own_hp_max = 0;  // x1000
uint64_t g_hits_applied = 0, g_hits_refused = 0, g_odd_templates = 0;
uint32_t g_next_instance = 0;  // our injected restrictions on ourselves: 0x8000E000 + n

// At most `burst` at once, then `per_s` a second.
struct RateLimit {
    double burst, per_s;
    double tokens = 0;
    uint64_t at = 0;
    bool take(uint64_t now) {
        tokens = at ? std::min(burst, tokens + static_cast<double>(now - at) * per_s / 1000.0) : burst;
        at = now;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};
// Network thread (hits_on_event).
RateLimit g_hit_rate{6.0, 5.0};
std::deque<std::pair<uint64_t, int64_t>> g_recent_damage;  // when, HP x1000 of the blows applied lately
uint64_t g_last_applied_ms = 0;

bool listed(const uint32_t* list, size_t n, uint32_t v) { return std::find(list, list + n, v) != list + n; }

// A key of an actor of ours in a template from the other side, other than us
// and the attacking copy (keys the mod invents are never real actors here).
uint32_t foreign_actor(const Bytes& v, uint32_t own, uint32_t copy) {
    for (size_t i = 4; i + 4 <= v.size(); ++i) {
        const uint32_t k = u32_at(v.data() + i);
        if (!keys::is_actor_key(k) || k == own || k == copy || keys::is_mod_key(k)) continue;
        if (game::find_actor(k)) return k;
    }
    return 0;
}

bool finite_vec3(const Bytes& v, size_t off) {
    if (off + 12 > v.size()) return false;
    float f[3];
    std::memcpy(f, v.data() + off, sizeof f);
    return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
}

void replace_u32(Bytes& v, uint32_t from, uint32_t to) {
    if (!from) return;
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        if (u32_at(v.data() + i) == from) std::memcpy(v.data() + i, &to, 4);
    }
}

// Target entries are at the end: len - k*665, each starting with its key.
std::vector<uint32_t> targets_of(const uint8_t* p, size_t n) {
    std::vector<uint32_t> out;
    for (size_t k = 1; k <= 30 && n >= k * kTargetEntry + kFirstEntry; ++k) {
        const uint32_t key = u32_at(p + n - k * kTargetEntry);
        if (!is_key_shape(key)) break;
        out.push_back(key);
    }
    return out;
}

}  // namespace

int64_t hits_own_hp() {
    std::lock_guard lock(g_mutex);
    return g_own_hp;
}

void hits_set_react(bool on, bool lethal) {
    g_react = on;
    g_lethal = lethal;
    log::info("Co-op: our player {} to the other player's enemies' blows, and {} die from them",
              on ? "reacts" : "does not react", lethal ? "can" : "cannot");
}

void hits_set_enabled(bool on) {
    g_enabled = on;
    log::info("Co-op: our enemies' blows on the other player's copy {} relayed", on ? "are" : "are not");
}

void hits_observe_c2s(const uint8_t* packet, size_t size) {
    // Learning (blows on us) runs even when we play alone; relaying blows on
    // the other player's copy only with [coop] enemies_target_copy.
    if (size < 5) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpDoFrameEventReq) return;
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    if (n < kFirstEntry + kTargetEntry || p[0] != 0x04) return;
    const uint32_t attacker = u32_at(p + 1);
    const uint32_t variant = enemy_variant(attacker);
    if (!variant) return;  // not one of our enemies
    // Arrows (accessor type 1) carry their ProjectileInfo key at +9, which decides
    // their damage; at +2 they have no hash (pendiente-proyectiles.md §2.2).
    const uint32_t hash = p[kAccessor] == 1 ? u32_at(p + kAccessor + 9) : u32_at(p + kAccessorHash);
    const uint64_t key = static_cast<uint64_t>(variant) << 32 | hash;
    const uint32_t own = capture_own_player_key();
    std::lock_guard lock(g_mutex);
    for (uint32_t target : targets_of(p, n)) {
        if (target == own) {
            // A blow on us: its HP loss teaches what this attack costs.
            g_pending.push_back({attacker, key, now_ms()});
            while (g_pending.size() > 32) g_pending.pop_front();
        } else if (target == kPuppetPlayer && g_enabled) {
            if (aggro_copy_dead()) continue;  // a blow on a corpse: the other player is already dead
            int64_t damage = 0;
            if (auto d = g_damage.find(key); d != g_damage.end()) {
                damage = d->second;
            } else if (const int64_t seed = p[kAccessor] == 1 ? seed_projectile_damage(hash) : 0; seed > 0) {
                damage = seed;
            } else if (auto v = g_by_variant.find(variant); v != g_by_variant.end()) {
                damage = v->second;
            }
            Bytes ev(kHitBaseSize);
            const uint32_t copy = enemy_copy_of(attacker);
            std::memcpy(ev.data(), &copy, 4);
            std::memcpy(ev.data() + 4, &hash, 4);
            std::memcpy(ev.data() + 8, &damage, 8);
            // How OUR player reacted to this attack, else the latest staggering
            // batch of that enemy type, else of any enemy (78 % of the enemy
            // blows that hurt the player stagger it, pendiente-reaccion-muerte-
            // invitado.md §1.2), for the other player's own player.
            const Reaction* r = nullptr;
            bool borrowed = false;  // another attack's batch, chosen because it staggers
            const auto own_stats = g_reactions.find(key);  // may hold only counts (cdmp_learnt.txt)
            if (own_stats != g_reactions.end() && !own_stats->second.ata.empty()) {
                r = &own_stats->second;
            } else if (auto v = g_variant_batch.find(variant); v != g_variant_batch.end()) {
                r = &g_reactions[v->second], borrowed = true;
            } else if (g_last_batch_key) {
                r = &g_reactions[g_last_batch_key], borrowed = true;
            }
            if (g_react && r && !r->ata.empty() && r->ata.size() <= kMaxAtaSize) {
                // Its own attack: does it (nearly) always stagger? A borrowed batch
                // was picked for its stagger (live test: archers' arrows borrowed one
                // that staggered 2 times in 3 and got no reaction).
                // A borrowed batch staggers, unless this attack is known not to
                // (counted in earlier sessions, its templates not seen yet in this one).
                const bool known_soft = own_stats != g_reactions.end() && own_stats->second.blows >= 3 &&
                                        own_stats->second.staggers * 10 < own_stats->second.blows * 8;
                const bool staggers = (borrowed ? !known_soft : (r->blows && r->staggers * 10 >= r->blows * 8)) &&
                                      r->aar.size() == kBatchSize && r->buff.size() == kBatchSize;
                Bytes t = r->ata;
                if (staggers) {
                    t.insert(t.end(), r->aar.begin(), r->aar.end());
                    t.insert(t.end(), r->buff.begin(), r->buff.end());
                }
                replace_u32(t, own, proto::kSelfMarker);
                if (copy) replace_u32(t, attacker, copy);
                const uint16_t n_ata = static_cast<uint16_t>(r->ata.size());
                ev.push_back(kHitHasTemplates | (staggers ? kHitStaggers : 0));
                ev.resize(ev.size() + 2);
                std::memcpy(ev.data() + ev.size() - 2, &n_ata, 2);
                ev.insert(ev.end(), t.begin(), t.end());
            }
            if (g_out.size() < 64) g_out.push_back({proto::EventKind::PlayerHit, std::move(ev)});
            if (g_hits_sent++ < 50) {
                log::info("Co-op: our enemy 0x{:08X} hit the other player's copy (attack 0x{:08X}, {})", attacker,
                          hash, damage ? std::format("{:.1f} hp", damage / 1000.0) : std::string("damage not learnt"));
            }
        }
    }
}

// Caller holds g_mutex. A blow of `attacker` on our player: the key of the
// DoFrameEventReq it came from (0 if none in the last 250 ms).
uint64_t pending_key_locked(uint32_t attacker) {
    for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it) {
        if (now_ms() - it->ms > 250) break;
        if (it->attacker == attacker) return it->key;
    }
    return 0;
}

// Caller holds g_mutex: the server's batch for a blow on our player.
void learn_reaction_locked(uint16_t op, const uint8_t* p, size_t n, uint32_t own) {
    if (op == kOpApplyBuffAck && n == kBatchSize) {
        uint64_t server_ms;
        std::memcpy(&server_ms, p + 0x39, 8);
        if (server_ms) g_server_ms = server_ms, g_server_ms_at = now_ms();
    }
    if (n < 8 || u32_at(p) != own) return;
    if (op == kOpAttackTargetAck && n >= 0x30) {
        const uint32_t attacker = u32_at(p + 4);
        const uint64_t key = pending_key_locked(attacker);
        if (!key) return;
        Reaction& r = g_reactions[key];
        ++r.blows;
        r.ata.assign(p, p + n);
        g_open = {key, now_ms(), attacker, 0, {}};
    } else if (op == kOpApplyActionRestrictionAck && n == kBatchSize) {
        if (!g_open.key || u32_at(p + 0x49) != g_open.attacker || now_ms() - g_open.ms > 50) return;
        ++g_reactions[g_open.key].staggers;
        g_open.instance = u32_at(p + 0x24);
        g_open.aar.assign(p, p + n);
    } else if (op == kOpApplyBuffAck && n == kBatchSize) {
        const uint32_t buff = u32_at(p + 4);
        if (g_open.aar.empty() || u32_at(p + 0x14) != g_open.instance || buff == 1021 || buff == 1023) return;
        Reaction& r = g_reactions[g_open.key];
        r.aar = std::move(g_open.aar);
        r.buff.assign(p, p + n);
        g_variant_batch[static_cast<uint32_t>(g_open.key >> 32)] = g_open.key;
        if (g_learnt_reactions++ < 30) {
            log::info("Co-op: learnt how we react to attack 0x{:08X} of enemy type {} (buff {})",
                      static_cast<uint32_t>(g_open.key), g_open.key >> 32, buff);
        }
        g_last_batch_key = g_open.key;
        g_open = {};
    }
}

void hits_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 8) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op == kOpAttackTargetAck || op == kOpApplyActionRestrictionAck || op == kOpApplyBuffAck) {
        const uint32_t own = capture_own_player_key();
        if (!own) return;
        std::lock_guard lock(g_mutex);
        learn_reaction_locked(op, packet + 5, size - 5, own);
        return;
    }
    if (op != kOpVaryHPAck || size < 5 + 0x58) return;
    const uint8_t* p = packet + 5;
    // VaryHPAck: u32 key | u32 stat | i64 hp x1000 | i64 | i64 max x1000 | ... | +0x54 u32 source.
    if (u32_at(p) != capture_own_player_key() || u32_at(p + 4) != kStatHP) return;
    int64_t hp = 0, max = 0;
    std::memcpy(&hp, p + 0x08, 8);
    std::memcpy(&max, p + 0x18, 8);
    const uint32_t source = u32_at(p + 0x54);
    std::lock_guard lock(g_mutex);
    if (max > 0) g_own_hp_max = max;
    const int64_t before = g_own_hp;
    g_own_hp = hp;
    if (before < 0 || hp >= before) return;
    const uint64_t t = now_ms();
    for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it) {
        if (it->attacker != source || t - it->ms > kLearnWindowMs) continue;
        const int64_t lost = before - hp;
        const bool fresh = !g_damage.count(it->key);
        g_damage[it->key] = lost;
        g_by_variant[static_cast<uint32_t>(it->key >> 32)] = lost;
        if (fresh && g_learnt++ < 50) {
            log::info("Co-op: learnt attack 0x{:08X} of enemy type {}: {:.1f} hp", static_cast<uint32_t>(it->key),
                      it->key >> 32, lost / 1000.0);
        }
        g_pending.erase(std::next(it).base());
        break;
    }
}

void hits_learnt_get(LearntCombat& out) {
    std::lock_guard lock(g_mutex);
    out.enemy_hits.insert(g_damage.begin(), g_damage.end());
    out.enemy_types.insert(g_by_variant.begin(), g_by_variant.end());
    for (const auto& [key, r] : g_reactions) {
        if (r.blows) out.reactions[key] = {r.blows, r.staggers};
    }
}

void hits_learnt_put(const LearntCombat& in) {
    std::lock_guard lock(g_mutex);
    // What this session learnt already wins.
    g_damage.insert(in.enemy_hits.begin(), in.enemy_hits.end());
    g_by_variant.insert(in.enemy_types.begin(), in.enemy_types.end());
    for (const auto& [key, r] : in.reactions) {
        Reaction& mine = g_reactions[key];
        if (!mine.blows) mine.blows = r.blows, mine.staggers = r.staggers;
    }
}

std::vector<proto::GameEvent> hits_take_events() {
    std::lock_guard lock(g_mutex);
    std::vector<proto::GameEvent> out(std::make_move_iterator(g_out.begin()), std::make_move_iterator(g_out.end()));
    g_out.clear();
    return out;
}

// Our own player reacts to a blow it took over there, from our copy of the
// enemy that gave it, as the server would send it (pieza4-tambaleo.md §8-9:
// a staggering blow goes as the restriction + hit buff WITHOUT its
// AttackTargetAck, which would spoil the stagger; other blows as the
// AttackTargetAck alone). The templates are the other side's bytes: their
// sizes, buff and restriction must be those of real blows, every key field
// is set here, and no other actor of ours may be named (H5).
bool inject_reaction(const proto::GameEvent& ev, uint32_t own, uint32_t copy, uint64_t server_ms) {
    const auto& e = ev.payload;
    if (e.size() < kHitBaseSize + 3 || !(e[kHitBaseSize] & kHitHasTemplates)) return false;
    uint16_t n_ata = 0;
    std::memcpy(&n_ata, e.data() + kHitBaseSize + 1, 2);
    bool staggers = (e[kHitBaseSize] & kHitStaggers) != 0;
    const size_t at = kHitBaseSize + 3;
    if (n_ata < kMinAtaSize || n_ata > kMaxAtaSize || at + n_ata + (staggers ? 2 * kBatchSize : 0) != e.size()) {
        return false;
    }
    // Our copy of that enemy may not be here (its copies are sent only while it
    // fights): then the reaction comes from nobody, which staggers just the
    // same (live test R1c), and there is no AttackTargetAck (it needs an attacker).
    if (copy && !game::find_actor(copy)) copy = 0;
    Bytes t(e.begin() + static_cast<ptrdiff_t>(at), e.end());
    replace_u32(t, proto::kSelfMarker, own);
    if (staggers) {
        // Only the hit buffs (CC_Push...) and their restrictions.
        const uint32_t key = u32_at(t.data() + n_ata + 0x34), buff_id = u32_at(t.data() + n_ata + kBatchSize + 4);
        if (!listed(kHitRestrictions, std::size(kHitRestrictions), key) ||
            !listed(kHitBuffs, std::size(kHitBuffs), buff_id)) {
            if (g_odd_templates++ < 20 || g_odd_templates % 100 == 0) {
                log::info("Co-op: the other player's hit reaction (restriction 0x{:08X}, buff {}) is not a known "
                          "stagger; {}",
                          key, buff_id, copy ? "plain hit reaction instead" : "no reaction");
            }
            staggers = false;
        }
    }
    if (!copy && !staggers) return false;
    std::vector<std::pair<uint16_t, Bytes>> batch;
    if (staggers) {
        Bytes aar(t.begin() + n_ata, t.begin() + n_ata + kBatchSize);
        Bytes buff(t.begin() + n_ata + kBatchSize, t.end());
        // Fixed in every real restriction: no delay (-1 would never end it) and
        // the fixed slot, not the list.
        std::memset(aar.data() + 0x14, 0, 8);
        aar[0x44] = 0;
        const uint32_t instance = 0x8000E000u + (g_next_instance++ & 0xFFF);
        std::memcpy(aar.data(), &own, 4);
        std::memcpy(aar.data() + 0x24, &instance, 4);
        std::memcpy(aar.data() + 0x49, &copy, 4);
        std::memcpy(buff.data(), &own, 4);
        std::memcpy(buff.data() + 0x14, &instance, 4);
        std::memcpy(buff.data() + 0x18, &copy, 4);
        std::memcpy(buff.data() + 0x1C, &own, 4);
        std::memcpy(buff.data() + 0x49, buff.data() + 4, 4);  // the buff id again, in every real one
        if (const auto pos = copy ? game::actor_world_position(copy) : std::nullopt) {
            const float xyz[3] = {pos->x, pos->y, pos->z};
            std::memcpy(buff.data() + 0x29, xyz, sizeof xyz);
        }
        if (server_ms) {
            std::memcpy(buff.data() + 0x39, &server_ms, 8);
            std::memcpy(buff.data() + 0x41, &server_ms, 8);
        }
        batch.emplace_back(kOpApplyActionRestrictionAck, std::move(aar));
        batch.emplace_back(kOpApplyBuffAck, std::move(buff));
    } else {
        Bytes ata(t.begin(), t.begin() + n_ata);
        // Where the blow landed (+0x69 target, +0x83 point): real numbers.
        if (!finite_vec3(ata, 0x69) || !finite_vec3(ata, 0x83)) return false;
        std::memcpy(ata.data(), &own, 4);
        std::memcpy(ata.data() + 4, &copy, 4);
        // +0xBC (and +0xCE in the 211-byte ones) name the attacker in most
        // blows: the copy, never another actor.
        for (const size_t off : {size_t{0xBC}, size_t{0xCE}}) {
            if (off + 4 > ata.size()) continue;
            const uint32_t k = u32_at(ata.data() + off);
            if (keys::is_actor_key(k) && k != own) std::memcpy(ata.data() + off, &copy, 4);
        }
        batch.emplace_back(kOpAttackTargetAck, std::move(ata));
    }
    for (const auto& packet : batch) {
        if (const uint32_t k = foreign_actor(packet.second, own, copy)) {
            if (g_odd_templates++ < 20 || g_odd_templates % 100 == 0) {
                log::info("Co-op: the other player's hit reaction names our actor 0x{:08X}; not applied", k);
            }
            return false;
        }
    }
    puppet_inject_s2c_batch(batch);
    return true;
}

// The finishing blow on ourselves: a collision from ourselves (live test D2:
// NotifyHandleDeadAck with motive 8, killer = us; the game's own death and
// revive follow, and the other side sees them on its copy of us).
bool die_by_collision(uint32_t own) {
    const auto me = game::actor_world_position(own);
    if (!me) return false;
    uint8_t b[44] = {};
    const float point[3] = {me->x, me->y + 1.0f, me->z};
    const float dir[3] = {1.0f, 0.0f, 0.0f};
    const uint32_t projectile = 0xF2EE2678;  // the collision of a captured native kill
    std::memcpy(b, &own, 4);
    std::memcpy(b + 4, &own, 4);
    std::memcpy(b + 12, point, sizeof point);
    std::memcpy(b + 24, dir, sizeof dir);
    std::memcpy(b + 36, &projectile, 4);
    Bytes pkt(5 + sizeof b);
    const uint16_t op = 2815, len = sizeof b;  // AttackByCollisionReq
    std::memcpy(pkt.data(), &op, 2);
    std::memcpy(pkt.data() + 3, &len, 2);
    std::memcpy(pkt.data() + 5, b, sizeof b);
    return sniffer_inject_c2s(std::move(pkt));
}

void hits_on_event(const proto::GameEvent& ev) {
    if (ev.kind != proto::EventKind::PlayerHit || ev.payload.size() < kHitBaseSize) return;
    const uint32_t own = capture_own_player_key();
    if (!own) return;
    int64_t damage = 0;
    uint32_t hash = 0, copy = 0;
    std::memcpy(&copy, ev.payload.data(), 4);
    std::memcpy(&hash, ev.payload.data() + 4, 4);
    std::memcpy(&damage, ev.payload.data() + 8, 8);
    int64_t max = 0, hp = -1;
    uint64_t server_ms = 0;
    {
        std::lock_guard lock(g_mutex);
        max = g_own_hp_max;
        hp = g_own_hp;
        if (g_server_ms) server_ms = g_server_ms + (now_ms() - g_server_ms_at);
    }
    if (hp == 0) return;  // dead here: a blow on our corpse over there
    // Only with our HP known (nothing else bounds the blow, and at 0 HP the
    // player is left "alive at 0", untested), from a copy of their enemy that
    // is here and near us, at a plausible rate (H4).
    const uint64_t now = now_ms();
    const char* refused = nullptr;
    float distance = -1.0f;
    if (hp < 0 || max <= 0) {
        refused = "our HP is not known yet";
    } else if (!is_valid_enemy_copy_key(copy) || !enemy_copy_loaded(copy)) {
        refused = "its attacker is not a copy of their enemies here";
    } else {
        const auto from = game::actor_world_position(copy);
        const auto me = game::actor_world_position(own);
        if (from && me) {
            const float dx = from->x - me->x, dy = from->y - me->y, dz = from->z - me->z;
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (distance < 0) {
            refused = "its attacker is not in the world";
        } else if (distance > kMaxHitDistance) {
            refused = "its attacker is too far";
        } else if (!g_hit_rate.take(now)) {
            refused = "too many blows";
        }
    }
    if (refused) {
        if (g_hits_refused++ < 20 || g_hits_refused % 100 == 0) {
            log::info("Co-op: the other player's enemy 0x{:08X} hit us (attack 0x{:08X}, {:.1f} m): ignored, {} "
                      "[{} ignored]",
                      copy, hash, distance, refused, g_hits_refused);
        }
        return;
    }
    const bool reacted = g_react && inject_reaction(ev, own, copy, server_ms);
    if (damage <= 0) {
        // Not learnt on the other side: a share of our max HP.
        damage = static_cast<int64_t>(static_cast<double>(max) * kDefaultShare);
    }
    damage = std::min(damage, max / 2);  // one blow never takes more than half
    // ...nor all those of the last second together.
    while (!g_recent_damage.empty() && now - g_recent_damage.front().first >= kDamageWindowMs) {
        g_recent_damage.pop_front();
    }
    int64_t recent = 0;
    for (const auto& r : g_recent_damage) recent += r.second;
    damage = std::min(damage, max / 2 - recent);
    // The generic stat path never kills: at 0 HP the player would only be
    // "knocked out" (pieza4-dano-servidor.md §8). Down to 1 HP, and a lethal
    // blow then finishes us with a collision of our own: a normal death
    // (motive 8, live tests D1/D2), not the cheat path whose death telemetry
    // says "DevCheat" (pendiente-recompensas-progreso.md). Never from one
    // blow alone: it needs another blow shortly before.
    const bool lethal =
        g_lethal && damage >= hp && g_last_applied_ms && now - g_last_applied_ms < kLethalAfterMs;
    damage = std::min(damage, hp - 1000);
    if (damage <= 0 && !lethal) return;
    g_last_applied_ms = now;
    if (damage > 0) {
        g_recent_damage.emplace_back(now, damage);
        while (g_recent_damage.size() > 64) g_recent_damage.pop_front();
    }
    // VaryStatByAccInputReq: u32 key | u32 stat | i64 delta (x1000), to our own server.
    Bytes pkt(5 + 16);
    const uint16_t op = kOpVaryStatByAccInputReq, len = 16;
    const int64_t delta = -damage;
    std::memcpy(pkt.data(), &op, 2);
    pkt[2] = 0;
    std::memcpy(pkt.data() + 3, &len, 2);
    std::memcpy(pkt.data() + 5, &own, 4);
    std::memcpy(pkt.data() + 9, &kStatHP, 4);
    std::memcpy(pkt.data() + 13, &delta, 8);
    bool ok = damage <= 0 || sniffer_inject_c2s(std::move(pkt));
    if (lethal) ok = die_by_collision(own) && ok;
    if (g_hits_applied++ < 50 || lethal) {
        log::info("Co-op: the other player's enemy hit us (attack 0x{:08X}): -{:.1f} hp{}{}{}", hash,
                  damage / 1000.0, lethal ? ", LETHAL" : "", ok ? "" : " (NOT applied)",
                  reacted ? ((ev.payload[kHitBaseSize] & kHitStaggers) ? ", stagger" : ", hit reaction") : "");
    }
}

}  // namespace cdmp::net
