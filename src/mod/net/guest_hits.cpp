#include "mod/net/guest_hits.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
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
constexpr uint16_t kOpAttackTargetAck = 2922;           // +0 target | +4 attacker | +8 flags | +0x24 attack hash
constexpr uint16_t kOpApplyActionRestrictionAck = 2162;  // 77 B: +0 target | +0x24 instance | +0x38 hash | +0x49 source
constexpr uint16_t kOpOnPostBuffDamagedAck = 2681;       // +0 target | +4 attacker | +0x0C u32 damage x1000
constexpr uint16_t kOpNotifyHandleFatalAck = 2899;       // +0 key | +4 by
constexpr uint16_t kOpApplyBuffAck = 2506;  // 77 B: +0 holder | +4 buff | +0x14 instance | +0x18 source |
                                            // +0x1C holder | +0x29 source position | +0x39/+0x41 server ms
constexpr uint16_t kOpVaryStatCheatReq = 2703;           // u32 key | u32 stat | u8 mode | i32 delta | u8 | u8
constexpr uint16_t kOpKillCheatReq = 2279;               // u32 key | u32 option
constexpr uint16_t kOpDieByRunOutFatalReq = 2321;        // u32 key
constexpr uint16_t kOpAttackByCollisionReq = 2815;  // u32 victim | u32 attacker | u32 0 | vec3 point | vec3 dir |
                                                    // u32 ProjectileInfo | u32 0 (44 B)
// The collision of a captured native kill (105149, t = 495.8); with our player
// as its owner it takes 7 HP and the death counts as a normal one (live test X2:
// motive 8, skill XP, loot; pieza4-botin-reaccion.md, pendiente-recompensas-progreso.md).
constexpr uint32_t kCollisionProjectile = 0xF2EE2678;
// Its hit (AttackTargetAck 8710E690, no restriction) makes the enemy die
// thrown away (18136804 -> 03A4E5C8 -> 4F69D6A0); after the player's usual
// arrow, enemies die the common way (5B636249) in 26 of 32 kills, still a
// normal kill (muerte-realista.md, 448 deaths). The arrow starts 1.5 m before
// the enemy at chest height, so it flies into it. [coop] finish_arrow.
constexpr uint32_t kArrowProjectile = 0xF653A168;
constexpr float kArrowLead = 1.5f, kArrowHeight = 1.2f;
std::atomic<bool> g_finish_arrow{true};
constexpr uint64_t kFinishFallbackMs = 600;  // no death by then: KillCheatReq (no XP, but dead)
constexpr uint32_t kStatHP = 0x000F4240;
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr size_t kAarSize = 77;
constexpr size_t kBuffSize = 77;
constexpr size_t kTargetEntry = 665;  // DoFrameEventReq target entry size
constexpr size_t kAccessor = 0x1F7;   // FrameEventAccessor: u8 type | u8 slot | u32 hash | u16 id | u32 frame
constexpr size_t kFirstEntry = 0x203;
constexpr size_t kGuestHitSize = 17;  // u32 copy | u32 action hash | u32 frame | u8 accessor type | i32 base x1000
constexpr uint8_t kFlagsClean = 0xF7;  // AttackTargetAck +8: a clean blow
constexpr uint8_t kFlagsNoDamage = 0xE3;  // frames that never hurt (pieza4-capturas.md §2.1)
constexpr double kDefaultBase = 13.0;  // median base of the table (pieza4-capturas.md §6.2)
// The other player's blows are requests to hurt OUR enemies through our own
// server, so they are bounded (security review H6, seguridad-combate.md):
// - HP per blow: about twice the strongest measured base (56 HP) x the
//   highest defence factor seen; their base (their gear) above it is cut;
constexpr double kMaxHitHp = 120.0;
// - their copy must be by the enemy: melee 15 m (8 m reach + both copies'
//   playout lag, ~0.6 s at a sprint), arrows (accessor type 1) 80 m; plus
//   twice the test offset [coop] offset_m (their copy is offset here, and the
//   enemy copy they hit was offset over there);
constexpr float kMaxMeleeDistance = 15.0f, kMaxArrowDistance = 80.0f;
// - at most one blow every 50 ms on one enemy and 15 a second in all (one
//   attack's hit frames can be ~90 ms apart; several enemies at once are fine).
constexpr uint64_t kMinHitGapMs = 50;

// An attack: accessor action hash + frame. Arrows (accessor type 1, 24 B:
// u8 1 | u32 owner | u32 projectile index | u32 ProjectileInfo key | u8 |
// u32 a | u8 b | u32 shooter | u8) are keyed by their ProjectileInfo key and
// a << 8 | b, which decide their attack and damage (pendiente-proyectiles.md §1.3).
constexpr uint64_t act_key(uint32_t hash, uint32_t frame) { return static_cast<uint64_t>(hash) << 32 | frame; }
constexpr uint64_t kArrow = act_key(0xF653A168u, 0);            // the player's usual arrow: 28 HP
constexpr uint64_t kArrowB1 = act_key(0xE912512Du, 3 << 8 | 1);  // another of the player's arrows: 17 HP
constexpr uint64_t kArrowB2 = act_key(0xE912512Du, 6 << 8 | 2);  // 17 HP

// Measured on the host's player over 49 captures (build/research/pieza4/seed_*.csv,
// pieza4-capturas.md §2.3): HP of each attack on a defence-1.0 enemy, and
// each variant's defence (variants with >= 3 samples; 2.0 outliers = crits left out).
struct SeedBase {
    uint64_t act;
    double hp;
};
constexpr SeedBase kSeedBase[] = {
    {kArrow, 28.00},                   {kArrowB1, 17.00},                 {kArrowB2, 17.00},
    {act_key(0xF104F0B7u, 4), 14.00},  {act_key(0x1FBA327Au, 56), 26.83},
    {act_key(0x267BE8EFu, 13), 13.00}, {act_key(0x5A2EA805u, 4), 13.00},  {act_key(0x0F357AC7u, 39), 5.83},
    {act_key(0x0F357AC7u, 24), 7.00},  {act_key(0x0F357AC7u, 49), 14.00}, {act_key(0x267BE8EFu, 11), 7.00},
    {act_key(0xF0CB3CF9u, 6), 12.83},  {act_key(0x70D78997u, 7), 26.83},  {act_key(0xF0CB3CF9u, 8), 14.00},
    {act_key(0x0F357AC7u, 14), 7.00},  {act_key(0xE3D7CB5Au, 31), 39.00}, {act_key(0x26AA9809u, 6), 12.92},
    {act_key(0xE3D7CB5Au, 24), 28.00}, {act_key(0x26AA9809u, 8), 14.00},  {act_key(0x73FA3F0Au, 11), 21.25},
    {act_key(0x2E2C561Au, 13), 5.92},  {act_key(0x2E2C561Au, 23), 28.00}, {act_key(0x787485E3u, 10), 23.00},
    {act_key(0x73FA3F0Au, 10), 16.50}, {act_key(0x8E66A374u, 15), 28.00}, {act_key(0xD2820A30u, 11), 28.00},
    {act_key(0x8E66A374u, 12), 5.33},  {act_key(0xE977E182u, 58), 56.00}, {act_key(0x833586A5u, 7), 42.00},
};
// Attacks that stagger (P(AAR) >= 0.8 in pieza4-capturas.md §2.3): when we
// have no batch of that very attack, the latest batch of any of ours will do.
constexpr uint64_t kSeedStaggers[] = {
    kArrow,                   kArrowB1,                 kArrowB2,                 act_key(0xF104F0B7u, 4),  act_key(0x5A2EA805u, 4),  act_key(0x0F357AC7u, 49),
    act_key(0x0F357AC7u, 39), act_key(0x1FBA327Au, 56), act_key(0x0F357AC7u, 24), act_key(0x267BE8EFu, 13),
    act_key(0x267BE8EFu, 11), act_key(0x70D78997u, 7),  act_key(0xF0CB3CF9u, 8),  act_key(0xF0CB3CF9u, 6),
    act_key(0x0F357AC7u, 14), act_key(0xE3D7CB5Au, 24), act_key(0x26AA9809u, 6),  act_key(0x26AA9809u, 8),
};
bool seed_staggers(uint64_t act) {
    for (uint64_t a : kSeedStaggers) {
        if (a == act) return true;
    }
    return false;
}

struct SeedDefence {
    uint32_t variant;
    double factor;
};
constexpr SeedDefence kSeedDefence[] = {
    {54111, 1.000},   {54109, 1.000},   {1000452, 0.857}, {54139, 1.000},   {1000451, 0.857}, {1000456, 0.857},
    {1002203, 0.857}, {1000450, 0.857}, {50004, 1.000},   {54140, 1.000},   {54122, 1.000},   {54107, 0.857},
    {54117, 1.000},   {54113, 0.857},   {1000244, 1.000}, {54127, 1.000},   {1002263, 0.857}, {50020, 1.000},
    {54121, 1.000},   {54102, 1.000},   {1000246, 0.833}, {54136, 1.013},   {54124, 1.000},   {50019, 0.846},
    {51002, 1.000},   {50008, 1.000},   {50001, 0.846},   {54104, 1.000},   {54126, 0.857},   {50018, 0.846},
    {50017, 0.846},   {50059, 0.821},   {1002205, 1.000}, {50002, 1.000},
};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_stagger{false};
std::mutex g_mutex;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
bool is_key_shape(uint32_t v) { return keys::is_class(v, keys::kClassWorld) || keys::is_class(v, keys::kClassPlayer); }
bool is_real_enemy(uint32_t key) { return keys::is_game_world_key(key); }
uint64_t now_ms() { return GetTickCount64(); }

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

// ---- what each of our attacks does (learnt from our own blows, also when
// we play alone, and kept in cdmp_learnt.txt between sessions)
// The votes of one attack never pass this: halved then, so new gear outvotes
// the old base after a few clean blows.
constexpr int kMaxVotes = 20;
struct Attack {
    std::map<int, int> base_votes;  // base HP x10 -> clean blows (the mode wins: crits are x2, 4 %)
    uint32_t clean = 0, no_damage = 0, staggers = 0, blows = 0;
    Bytes ata, aar, buff;  // latest templates; aar and buff from the same server batch
};
std::map<uint64_t, Attack> g_attacks;
// Variant -> its defence factor x100 -> blows that gave it, from its first
// blows with an attack we know (seeds as fallback). The mode wins: a crit on
// the first blow made one type 1.72 instead of 0.86 (live test, 6 Oct).
constexpr int kDefenceVotes = 9;
std::map<uint32_t, std::map<int, int>> g_defence;

// After a change of our gear, the attacks learnt before it, not rescaled yet,
// and the new/old ratios of the clean blows seen since: two that agree are
// the change of our attack, applied to every one of those attacks at once
// (a crit, x2, never agrees with a normal blow).
bool g_gear_changed = false;
std::set<uint64_t> g_stale;
std::deque<double> g_gear_ratios;
constexpr double kRatioAgree = 0.05, kRatioSame = 0.03;

int mode_of(const std::map<int, int>& votes) {
    int best = 0, n = 0;
    for (const auto& [v, c] : votes) {
        if (c > n) best = v, n = c;
    }
    return best;
}
int votes_of(const std::map<int, int>& votes) {
    int n = 0;
    for (const auto& [v, c] : votes) n += c;
    return n;
}

struct PendingBlow {  // a DoFrameEventReq of ours waiting for its AttackTargetAcks
    uint64_t act = 0;
    std::vector<uint32_t> targets;
    uint64_t ms = 0;
};
std::deque<PendingBlow> g_pending;
struct LastBlow {  // per enemy: our last blow on it, to pair its AAR / damage
    uint64_t act = 0;
    uint8_t flags = 0;
    uint64_t ms = 0;
};
std::map<uint32_t, LastBlow> g_last_blow;
struct PendingAar {  // a restriction of ours waiting for its hit buff (the next packet, pieza4-tambaleo.md §4.1)
    uint32_t target = 0, instance = 0;
    uint64_t act = 0, ms = 0;
    Bytes aar;
};
PendingAar g_pending_aar;
uint64_t g_last_stagger_act = 0;  // the attack of the latest complete staggering batch
uint64_t g_server_ms = 0, g_server_ms_at = 0;  // latest server game clock (ApplyBuffAck +0x39), when seen

// ---- owner side
std::map<uint32_t, uint64_t> g_cheated;  // enemy -> when we last sent it a VaryStatCheatReq
struct Finishing {  // a lethal blow sent as 1 HP + collision, waiting for the death
    uint64_t ms = 0;
    bool fatal = false;  // the enemy was left "fatal" (0 HP): fall back to DieByRunOutFatalReq
};
std::map<uint32_t, Finishing> g_finishing;
uint32_t g_next_instance = 0;
uint64_t g_applied = 0, g_learnt_log = 0, g_sent_log = 0, g_refused_log = 0;
// Network thread (apply_hit): the bounds above.
RateLimit g_hit_rate{15.0, 15.0};
std::map<uint32_t, uint64_t> g_last_hit;  // enemy -> when the other player's last blow on it was applied
uint64_t g_no_reaction = 0;  // blows applied with no reaction to show
std::map<uint32_t, uint64_t> g_last_act;  // enemy -> that blow's attack (act_key): only a repeat of it is too soon
std::deque<proto::GameEvent> g_out;

std::vector<uint32_t> targets_of(const uint8_t* p, size_t n) {
    std::vector<uint32_t> out;
    for (size_t k = 1; k <= 30 && n >= k * kTargetEntry + kFirstEntry; ++k) {
        const uint32_t key = u32_at(p + n - k * kTargetEntry);
        if (!is_key_shape(key)) break;
        out.push_back(key);
    }
    return out;
}

uint64_t act_of(const uint8_t* p) {
    const uint8_t type = p[kAccessor];
    if (type == 1) return act_key(u32_at(p + kAccessor + 9), u32_at(p + kAccessor + 14) << 8 | p[kAccessor + 18]);
    return act_key(u32_at(p + kAccessor + 2), u32_at(p + kAccessor + 8));
}

double seed_factor(uint32_t variant) {
    for (const auto& s : kSeedDefence) {
        if (s.variant == variant) return s.factor;
    }
    return 0;
}

// Caller holds g_mutex.
double factor_locked(uint32_t variant) {
    if (auto f = g_defence.find(variant); f != g_defence.end() && !f->second.empty()) return mode_of(f->second) / 100.0;
    const double s = seed_factor(variant);
    return s > 0 ? s : 1.0;
}

// Base HP of an attack on a defence-1.0 enemy, learnt by us; 0 if unknown. Caller holds g_mutex.
double learnt_base_locked(uint64_t act) {
    auto a = g_attacks.find(act);
    if (a == g_attacks.end() || a->second.base_votes.empty()) return 0;
    const auto best = std::max_element(a->second.base_votes.begin(), a->second.base_votes.end(),
                                       [](const auto& x, const auto& y) { return x.second < y.second; });
    return best->first / 10.0;
}

double seed_base(uint64_t act) {
    for (const auto& s : kSeedBase) {
        if (s.act == act) return s.hp;
    }
    return 0;
}

std::string act_text(uint64_t act) {
    return std::format("{:08X}/{}", static_cast<uint32_t>(act >> 32), static_cast<uint32_t>(act));
}

Bytes packet(uint16_t op, const uint8_t* payload, size_t n) {
    Bytes p(5 + n);
    const uint16_t len = static_cast<uint16_t>(n);
    std::memcpy(p.data(), &op, 2);
    p[2] = 0;
    std::memcpy(p.data() + 3, &len, 2);
    std::memcpy(p.data() + 5, payload, n);
    return p;
}

// Our server learns of a blow ~9 ms after our DoFrameEventReq (pieza4-capturas.md §4.1).
void on_attack_target(const uint8_t* p, size_t n, uint32_t own) {
    const uint32_t target = u32_at(p), attacker = u32_at(p + 4);
    if (attacker != own || !is_real_enemy(target) || n < 0x30) return;
    const uint64_t t = now_ms();
    for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it) {
        if (t - it->ms > 250) break;
        if (std::find(it->targets.begin(), it->targets.end(), target) == it->targets.end()) continue;
        const uint8_t flags = p[8];
        Attack& a = g_attacks[it->act];
        ++a.blows;
        if (flags == kFlagsNoDamage) ++a.no_damage;
        a.ata.assign(p, p + n);
        g_last_blow[target] = {it->act, flags, t};
        return;
    }
}

void on_restriction(const uint8_t* p, size_t n, uint32_t own) {
    if (n != kAarSize || u32_at(p + 0x49) != own) return;
    const uint32_t target = u32_at(p);
    auto b = g_last_blow.find(target);
    if (b == g_last_blow.end() || now_ms() - b->second.ms > 50) return;
    ++g_attacks[b->second.act].staggers;
    // Kept only with its hit buff: without it the client skips the stagger (live test R1).
    g_pending_aar = {target, u32_at(p + 0x24), b->second.act, now_ms(), Bytes(p, p + n)};
}

void on_buff(const uint8_t* p, size_t n) {
    if (n != kBuffSize) return;
    uint64_t server_ms;
    std::memcpy(&server_ms, p + 0x39, 8);
    if (server_ms) g_server_ms = server_ms, g_server_ms_at = now_ms();
    const uint32_t buff = u32_at(p + 4);
    if (g_pending_aar.aar.empty() || u32_at(p) != g_pending_aar.target || u32_at(p + 0x14) != g_pending_aar.instance ||
        now_ms() - g_pending_aar.ms > 50 || buff == 1021 || buff == 1023) {  // 1021/1023: the damage buff
        return;
    }
    Attack& a = g_attacks[g_pending_aar.act];
    a.aar = std::move(g_pending_aar.aar);
    a.buff.assign(p, p + n);
    g_last_stagger_act = g_pending_aar.act;
    g_pending_aar = {};
}

// Caller holds g_mutex: one clean blow's new/old ratio after a gear change.
void on_gear_ratio(double ratio) {
    if (!std::isfinite(ratio) || ratio <= 0) return;
    g_gear_ratios.push_back(ratio);
    while (g_gear_ratios.size() > 6) g_gear_ratios.pop_front();
    double r = 0;
    for (size_t i = 0; i + 1 < g_gear_ratios.size() && !r; ++i) {
        for (size_t j = i + 1; j < g_gear_ratios.size() && !r; ++j) {
            const double x = g_gear_ratios[i], y = g_gear_ratios[j];
            if (std::fabs(x - y) <= kRatioAgree * std::max(x, y)) r = (x + y) / 2;
        }
    }
    if (!r) return;
    size_t rescaled = 0;
    if (std::fabs(r - 1.0) > kRatioSame) {
        for (uint64_t act : g_stale) {
            auto it = g_attacks.find(act);
            if (it == g_attacks.end() || it->second.base_votes.empty()) continue;
            // The old base, scaled, with few votes: its own blows confirm it.
            const int base = static_cast<int>(std::lround(mode_of(it->second.base_votes) * r));
            it->second.base_votes = {{std::clamp(base, 1, learnt::kMaxBaseX10), 2}};
            ++rescaled;
        }
    }
    log::info("Piece 4: our gear changed: our attacks now do x{:.2f}{}", r,
              rescaled ? std::format(" ({} learnt attacks rescaled)", rescaled) : std::string(" (the same)"));
    g_gear_changed = false;
    g_stale.clear();
    g_gear_ratios.clear();
}

void on_damaged(const uint8_t* p, size_t n, uint32_t own) {
    if (n < 0x10 || u32_at(p + 4) != own) return;
    const uint32_t target = u32_at(p);
    auto b = g_last_blow.find(target);
    if (b == g_last_blow.end() || now_ms() - b->second.ms > 100 || b->second.flags != kFlagsClean) return;
    const double hp = u32_at(p + 0x0C) / 1000.0;
    const uint32_t variant = enemy_variant(target);
    if (hp <= 0 || !variant) return;
    const uint64_t act = b->second.act;
    Attack& a = g_attacks[act];
    const double known = learnt_base_locked(act);
    const double ref = known > 0 ? known : seed_base(act);
    const auto d = g_defence.find(variant);
    const int defence_votes = d != g_defence.end() ? votes_of(d->second) : 0;
    const bool factor_known = defence_votes > 0 || seed_factor(variant) > 0;
    double base = 0;
    if (ref > 0 && defence_votes < kDefenceVotes && seed_factor(variant) <= 0 && !g_gear_changed) {
        // This type's defence, from an attack we know (1.0 or ~0.86 in practice).
        // Not right after a gear change: our own change would pass for its defence.
        const double f = hp / ref;
        if (f >= learnt::kMinDefence && f <= learnt::kMaxDefence) {
            auto& votes = g_defence[variant];
            const bool first = votes.empty();
            ++votes[static_cast<int>(std::lround(f * 100))];
            if (first && g_learnt_log++ < 50) log::info("Piece 4: enemy type {} has defence {:.3f}", variant, f);
        }
        base = ref;
    } else if (factor_known) {
        base = hp / factor_locked(variant);
    } else {
        base = hp;  // nothing known: assume defence 1.0
    }
    if (g_gear_changed && factor_known && known > 0 && g_stale.count(act)) on_gear_ratio(base / known);
    const bool fresh = a.base_votes.empty();
    ++a.base_votes[static_cast<int>(std::lround(base * 10))];
    if (votes_of(a.base_votes) > kMaxVotes) {
        for (auto& [value, count] : a.base_votes) count /= 2;
        std::erase_if(a.base_votes, [](const auto& v) { return v.second == 0; });
    }
    ++a.clean;
    b->second.flags = 0;  // one damage per blow
    if (fresh && g_learnt_log++ < 50) {
        log::info("Piece 4: learnt our attack {}: {:.1f} hp on a defence-1.0 enemy", act_text(act), base);
    }
}

// The finishing blow: a collision owned by our player, from the other
// player's copy towards the enemy, so the kill is a normal one (skill XP,
// challenges; KillCheatReq and the "fatal" run-out give none).
bool send_collision(uint32_t enemy) {
    const uint32_t own = capture_own_player_key();
    const auto at = game::actor_world_position(enemy);
    auto from = game::actor_world_position(kPuppetPlayer);
    if (!from) from = game::actor_world_position(own);
    if (!own || !at || !from) return false;
    uint8_t b[44] = {};
    const float dx = at->x - from->x, dz = at->z - from->z;
    const float len = std::sqrt(dx * dx + dz * dz) + 1e-4f;
    const float dir[3] = {dx / len, 0.0f, dz / len};
    const bool arrow = g_finish_arrow.load();
    const float point[3] = {at->x - (arrow ? dir[0] * kArrowLead : 0.0f), at->y + (arrow ? kArrowHeight : 1.0f),
                            at->z - (arrow ? dir[2] * kArrowLead : 0.0f)};
    const uint32_t projectile = arrow ? kArrowProjectile : kCollisionProjectile;
    std::memcpy(b, &enemy, 4);
    std::memcpy(b + 4, &own, 4);
    std::memcpy(b + 12, point, sizeof point);
    std::memcpy(b + 24, dir, sizeof dir);
    std::memcpy(b + 36, &projectile, 4);
    return sniffer_inject_c2s(packet(kOpAttackByCollisionReq, b, sizeof b));
}

void on_fatal(const uint8_t* p, size_t n) {
    // VaryStatCheatReq down to 0 (its HP was not known) leaves the enemy
    // "fatal" (stunned) and the game only finishes it ~5 s later with
    // DieByRunOutFatalReq (live test V2, capture 114648), which gives no XP:
    // try the collision now, and the run-out if it does not die. The game
    // never does this on its own (0 "fatal" in 49 captures), so only for
    // enemies we just hurt. Caller holds g_mutex.
    if (n < 8) return;
    const uint32_t key = u32_at(p);
    auto c = g_cheated.find(key);
    if (c == g_cheated.end() || now_ms() - c->second > 1500) return;
    g_cheated.erase(c);
    const bool ok = send_collision(key);
    g_finishing[key] = {now_ms(), true};
    log::info("Piece 4: enemy 0x{:08X} left stunned at 0 hp by the other player's blow: finishing it{}", key,
              ok ? "" : " (collision NOT sent)");
}

// GuestHits wait for the playout delay of the other player's copy, so the
// enemy reacts when the copy's blow lands on screen, not ~300 ms before it.
struct DelayedHit {
    uint64_t due = 0;
    proto::GameEvent ev;
};
std::deque<DelayedHit> g_delayed;

}  // namespace

void apply_hit(const proto::GameEvent& ev);

void guest_hits_learnt_get(LearntCombat& out) {
    std::lock_guard lock(g_mutex);
    for (const auto& [act, a] : g_attacks) {
        if (!a.blows && a.base_votes.empty()) continue;
        auto& o = out.attacks[act];
        o.base_votes = a.base_votes;
        o.clean = a.clean, o.no_damage = a.no_damage, o.staggers = a.staggers, o.blows = a.blows;
    }
    for (const auto& [variant, votes] : g_defence) {
        if (!votes.empty()) out.defence[variant] = mode_of(votes) / 100.0;
    }
}

void guest_hits_learnt_put(const LearntCombat& in) {
    std::lock_guard lock(g_mutex);
    // What this session learnt already wins.
    for (const auto& [act, a] : in.attacks) {
        Attack& mine = g_attacks[act];
        if (mine.blows || !mine.base_votes.empty()) continue;
        mine.base_votes = a.base_votes;
        mine.clean = a.clean, mine.no_damage = a.no_damage, mine.staggers = a.staggers, mine.blows = a.blows;
    }
    for (const auto& [variant, f] : in.defence) {
        auto& votes = g_defence[variant];
        if (votes.empty()) votes[static_cast<int>(std::lround(f * 100))] = learnt::kKeptVotes;
    }
}

void guest_hits_on_gear_change() {
    std::lock_guard lock(g_mutex);
    g_gear_changed = true;
    g_gear_ratios.clear();
    for (const auto& [act, a] : g_attacks) {
        if (!a.base_votes.empty()) g_stale.insert(act);
    }
}

void guest_hits_set_finish_arrow(bool on) { g_finish_arrow = on; }

void guest_hits_set_enabled(bool on, bool stagger) {
    g_enabled = on;
    g_stagger = stagger;
    log::info("Co-op: the other player's blows on our enemies' copies {} applied{}", on ? "are" : "are not",
              on && stagger ? " (with the stagger)" : "");
}

void guest_hits_observe_c2s(const uint8_t* packet, size_t size) {
    // Learning runs even when we play alone; GuestHits only with [coop] guest_hits.
    if (size < 5) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpDoFrameEventReq) return;
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    if (n < kFirstEntry + kTargetEntry || p[0] != 0x04) return;
    const uint32_t own = capture_own_player_key();
    if (!own || u32_at(p + 1) != own) return;
    const uint64_t act = act_of(p);
    const auto targets = targets_of(p, n);
    std::lock_guard lock(g_mutex);
    g_pending.push_back({act, targets, now_ms()});
    while (g_pending.size() > 32) g_pending.pop_front();
    for (uint32_t target : targets) {
        if (!g_enabled || !is_enemy_copy_key(target)) continue;
        // A copy of the other player's enemy: tell them. Our own base for this
        // attack if we learnt it (our gear), else 0 and they use theirs.
        const int32_t base = static_cast<int32_t>(std::lround(learnt_base_locked(act) * 1000));
        Bytes ev(kGuestHitSize);
        const uint32_t hash = static_cast<uint32_t>(act >> 32), frame = static_cast<uint32_t>(act);
        std::memcpy(ev.data(), &target, 4);
        std::memcpy(ev.data() + 4, &hash, 4);
        std::memcpy(ev.data() + 8, &frame, 4);
        ev[12] = p[kAccessor];
        std::memcpy(ev.data() + 13, &base, 4);
        if (g_out.size() < 64) g_out.push_back({proto::EventKind::GuestHit, std::move(ev)});
        if (g_sent_log++ < 50) {
            log::info("Piece 4: we hit the copy 0x{:08X} (attack {}, our base {})", target, act_text(act),
                      base ? std::format("{:.1f} hp", base / 1000.0) : std::string("not learnt"));
        }
    }
}

void guest_hits_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5 + 8) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op != kOpAttackTargetAck && op != kOpApplyActionRestrictionAck && op != kOpOnPostBuffDamagedAck &&
        op != kOpNotifyHandleFatalAck && op != kOpApplyBuffAck) {
        return;
    }
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    const uint32_t own = capture_own_player_key();
    if (!own) return;
    std::lock_guard lock(g_mutex);
    switch (op) {
        case kOpAttackTargetAck: on_attack_target(p, n, own); break;
        case kOpApplyActionRestrictionAck: on_restriction(p, n, own); break;
        case kOpOnPostBuffDamagedAck: on_damaged(p, n, own); break;
        case kOpNotifyHandleFatalAck:
            if (g_enabled) on_fatal(p, n);
            break;
        case kOpApplyBuffAck: on_buff(p, n); break;
    }
}

std::vector<proto::GameEvent> guest_hits_take_events() {
    std::vector<proto::GameEvent> due;
    {
        std::lock_guard lock(g_mutex);
        while (!g_delayed.empty() && g_delayed.front().due <= now_ms()) {
            due.push_back(std::move(g_delayed.front().ev));
            g_delayed.pop_front();
        }
    }
    for (const auto& ev : due) apply_hit(ev);
    std::lock_guard lock(g_mutex);
    // Network thread, every loop: lethal blows whose collision did not kill.
    for (auto it = g_finishing.begin(); it != g_finishing.end();) {
        if (now_ms() - it->second.ms < kFinishFallbackMs) {
            ++it;
            continue;
        }
        const uint32_t enemy = it->first;
        const bool fatal = it->second.fatal;
        it = g_finishing.erase(it);
        if (capture_enemy_state(enemy).dead) continue;
        // The cheat path kills anything for good: only an enemy still
        // fighting us. Otherwise it stays at 1 HP for a native blow (H6).
        if (!fatal && !enemy_hostile_in_combat(enemy)) {
            log::info("Piece 4: enemy 0x{:08X} survived the finishing collision and is not fighting us: left at 1 hp",
                      enemy);
            continue;
        }
        uint8_t b[8] = {};
        std::memcpy(b, &enemy, 4);
        const bool ok = fatal ? sniffer_inject_c2s(packet(kOpDieByRunOutFatalReq, b, 4))
                              : sniffer_inject_c2s(packet(kOpKillCheatReq, b, sizeof b));
        log::info("Piece 4: enemy 0x{:08X} survived the finishing collision: {}{}", enemy,
                  fatal ? "DieByRunOutFatalReq" : "KillCheatReq", ok ? "" : " (NOT sent)");
    }
    std::vector<proto::GameEvent> out(std::make_move_iterator(g_out.begin()), std::make_move_iterator(g_out.end()));
    g_out.clear();
    return out;
}

void apply_hit(const proto::GameEvent& ev) {
    if (!g_enabled || ev.kind != proto::EventKind::GuestHit || ev.payload.size() != kGuestHitSize) return;
    const uint8_t* e = ev.payload.data();
    const uint32_t copy = u32_at(e), hash = u32_at(e + 4), frame = u32_at(e + 8);
    const uint8_t type = e[12];
    int32_t their_base = 0;
    std::memcpy(&their_base, e + 13, 4);
    // Arrows (type 1) come as ProjectileInfo key + a << 8 | b, like act_of().
    const uint64_t act = act_key(hash, frame);
    const uint32_t enemy = enemy_original_of(copy);
    if (!enemy) {
        if (g_applied++ < 50) log::info("Piece 4: the other player hit copy 0x{:08X}, which is not ours (any more)", copy);
        return;
    }
    const auto state = capture_enemy_state(enemy);
    if (state.dead || state.hp == 0) return;  // a corpse (or a stunned enemy about to die)
    // Only a hostile enemy that is fighting, with their copy by it, at a
    // plausible rate (H6): never townsfolk, allies or quest NPCs we copied.
    const char* refused = nullptr;
    const uint64_t now = now_ms();
    float distance = -1.0f;
    if (puppet_local_paused()) {
        // Our game is paused (the respawn menu after we died): our client and
        // server stand still, a queued blow would only wait and be dropped.
        refused = "our game is paused";
    } else if (!enemy_guest_may_hit(enemy)) {
        refused = "it is not fighting either of us (nor a zone enemy we copied)";
    } else {
        const auto at = game::actor_world_position(enemy);
        const auto from = game::actor_world_position(kPuppetPlayer);
        if (at && from) {
            const float dx = at->x - from->x, dy = at->y - from->y, dz = at->z - from->z;
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (distance < 0) {
            refused = "their copy or the enemy is not here";
        } else if (distance > (type == 1 ? kMaxArrowDistance : kMaxMeleeDistance) + 2.0f * std::fabs(puppet_offset_m())) {
            refused = "their copy is too far from it";
        } else if (auto l = g_last_hit.find(enemy);
                   l != g_last_hit.end() && now - l->second < kMinHitGapMs && g_last_act[enemy] == act) {
            // The same attack frame again within 50 ms: a repeat. Another frame
            // or attack is another blow (7 Oct night: 6 of 21 real blows refused).
            refused = "too soon after their last blow on it";
        } else if (!g_hit_rate.take(now)) {
            refused = "too many blows";
        }
    }
    if (refused) {
        if (g_refused_log++ < 20 || g_refused_log % 100 == 0) {
            log::info("Piece 4: the other player's blow on our enemy 0x{:08X} (copy 0x{:08X}, {:.1f} m) refused: {} "
                      "[{} refused]",
                      enemy, copy, distance, refused, g_refused_log);
        }
        return;
    }
    g_last_hit[enemy] = now;
    g_last_act[enemy] = act;
    enemy_note_guest_blow(enemy);  // fighting us now: the guest started it
    aggro_request_target_on_copy(enemy);  // and it turns on the one who hit it
    if (g_last_hit.size() > 256) {
        std::erase_if(g_last_hit, [&](const auto& h) { return now - h.second >= kMinHitGapMs; });
        std::erase_if(g_last_act, [&](const auto& h) { return !g_last_hit.count(h.first); });
    }
    const uint32_t variant = enemy_variant(enemy);

    // How much: their base for this attack (their gear) if they know it,
    // else ours (learnt, then measured seeds, then the median), times this
    // enemy's defence. Attacks whose result is always "no damage" do none.
    double base = 0, factor = 1;
    bool no_damage = false;
    Bytes ata, aar, buff;
    uint64_t server_ms = 0;
    const char* source = "theirs";
    {
        std::lock_guard lock(g_mutex);
        factor = factor_locked(variant);
        auto a = g_attacks.find(act);
        // Stagger only for attacks that (nearly) always stagger (pieza4-capturas.md §2.3: P(AAR) is ~binary).
        const bool known_stagger =
            a != g_attacks.end() && a->second.blows >= 2 && a->second.staggers * 10 >= a->second.blows * 8;
        if (a != g_attacks.end()) {
            no_damage = a->second.no_damage >= 2 && a->second.clean == 0;
            ata = a->second.ata;
            if (known_stagger) {
                aar = a->second.aar;
                buff = a->second.buff;
            }
        }
        // Its own batch not seen in this session (counted in an earlier one),
        // or not enough of our own blows with that attack to know (then the
        // measured table decides): the latest staggering batch of any attack.
        if (aar.empty() && (known_stagger || ((a == g_attacks.end() || a->second.blows < 5) && seed_staggers(act)))) {
            if (auto l = g_attacks.find(g_last_stagger_act); l != g_attacks.end()) {
                aar = l->second.aar;
                buff = l->second.buff;
            }
        }
        if (ata.empty()) {
            // An attack of theirs we never used: any of our templates (only the AI event matters).
            for (const auto& [k, v] : g_attacks) {
                if (!v.ata.empty()) {
                    ata = v.ata;
                    break;
                }
            }
        }
        if (g_server_ms) server_ms = g_server_ms + (now_ms() - g_server_ms_at);
        if (their_base > 0) {
            base = std::min(their_base / 1000.0, kMaxHitHp);
        } else if (const double ours = learnt_base_locked(act); ours > 0) {
            base = ours, source = "ours";
        } else if (const double seed = seed_base(act); seed > 0) {
            base = seed, source = "measured";
        } else {
            base = kDefaultBase, source = "default";
        }
    }
    const int32_t damage =
        no_damage ? 0 : std::max(1, static_cast<int32_t>(std::lround(std::min(base * factor, kMaxHitHp))));
    const bool lethal = damage > 0 && state.hp > 0 && static_cast<int64_t>(damage) * 1000 >= state.hp;

    bool ok = true;
    if (lethal) {
        // Down to 1 HP with attribution (never 0: that leaves it "fatal"), then
        // the collision of our player kills it as a normal kill (motive 8,
        // skill XP, loot). KillCheatReq only if it survives (no XP).
        const int32_t above_one = static_cast<int32_t>((state.hp - 1) / 1000);
        if (above_one > 0) {
            uint8_t b[15] = {};
            const int32_t delta = -above_one;
            std::memcpy(b, &enemy, 4);
            std::memcpy(b + 4, &kStatHP, 4);
            std::memcpy(b + 9, &delta, 4);
            ok = sniffer_inject_c2s(packet(kOpVaryStatCheatReq, b, sizeof b));
        }
        ok = send_collision(enemy) && ok;
        std::lock_guard lock(g_mutex);
        g_finishing[enemy] = {now_ms(), false};
    } else if (damage > 0) {
        // VaryStatCheatReq mode 0: HP lost with our player as the source (live test V2).
        uint8_t b[15] = {};
        const int32_t delta = -damage;
        std::memcpy(b, &enemy, 4);
        std::memcpy(b + 4, &kStatHP, 4);
        std::memcpy(b + 9, &delta, 4);
        ok = sniffer_inject_c2s(packet(kOpVaryStatCheatReq, b, sizeof b));
        std::lock_guard lock(g_mutex);
        g_cheated[enemy] = now_ms();
        if (g_cheated.size() > 256) g_cheated.erase(g_cheated.begin());
    }

    // The blow as our client sees it, from the other player's copy. A staggering
    // blow is the restriction plus the hit buff (CC_Push...) with the same
    // instance id (pieza4-tambaleo.md §5.1), in one batch, and WITHOUT its
    // AttackTargetAck: an AttackTargetAck from a real actor (the copy, or even
    // our own player replaying an untouched real batch) sends the enemy
    // straight to the end of the reaction, while the restriction and the buff
    // alone, with the copy as their source, stagger it or knock it down (live
    // tests R1b-R1f, pieza4-tambaleo.md §8-9). The buff also tells the AI who
    // hit it. Other blows get the AttackTargetAck (push and AI event).
    // The stagger needs only the restriction and the buff; the plain reaction
    // needs the AttackTargetAck (none learnt until our own player hit an
    // enemy: 7 Oct night, 0 in the whole session, analisis-ia2/P2 §4.1).
    const bool copy_here = puppet_spawned();
    const bool can_stagger = g_stagger && !lethal && aar.size() == kAarSize && buff.size() == kBuffSize;
    bool staggered = false;
    const char* reaction = "";
    if (copy_here && (can_stagger || ata.size() >= 0x30)) {
        std::vector<std::pair<uint16_t, Bytes>> batch;
        if (can_stagger) {
            const uint32_t instance = 0x8000F000u + (g_next_instance++ & 0xFFF);
            std::memcpy(aar.data(), &enemy, 4);
            std::memcpy(aar.data() + 0x24, &instance, 4);
            std::memcpy(aar.data() + 0x49, &kPuppetPlayer, 4);
            std::memcpy(buff.data(), &enemy, 4);
            std::memcpy(buff.data() + 0x14, &instance, 4);
            std::memcpy(buff.data() + 0x18, &kPuppetPlayer, 4);
            std::memcpy(buff.data() + 0x1C, &enemy, 4);
            if (const auto pos = game::actor_world_position(kPuppetPlayer)) {
                const float xyz[3] = {pos->x, pos->y, pos->z};
                std::memcpy(buff.data() + 0x29, xyz, sizeof xyz);
            }
            if (server_ms) {
                std::memcpy(buff.data() + 0x39, &server_ms, 8);
                std::memcpy(buff.data() + 0x41, &server_ms, 8);
            }
            batch.emplace_back(kOpApplyActionRestrictionAck, std::move(aar));
            batch.emplace_back(kOpApplyBuffAck, std::move(buff));
            staggered = true;
        } else {
            std::memcpy(ata.data(), &enemy, 4);
            std::memcpy(ata.data() + 4, &kPuppetPlayer, 4);
            batch.emplace_back(kOpAttackTargetAck, std::move(ata));
        }
        puppet_inject_s2c_batch(batch);
    } else if (copy_here) {
        reaction = " [no reaction learnt yet for that attack: hit an enemy yourself once]";
        ++g_no_reaction;
    }
    if (g_applied++ < 100 || (g_no_reaction && g_no_reaction % 50 == 1 && *reaction)) {
        log::info("Piece 4: the other player hit our enemy 0x{:08X} (type {}, hp {}) with {}: {}{}{}", enemy, variant,
                  state.hp >= 0 ? std::format("{:.0f}", state.hp / 1000.0) : std::string("?"), act_text(act),
                  no_damage ? std::string("no damage (that attack never hurts)")
                            : std::format("-{} hp ({} base {:.1f} x defence {:.3f}){}", damage, source, base, factor,
                                          lethal ? ", lethal" : ""),
                  ok ? "" : " (NOT sent)",
                  !copy_here ? " [their copy is not here: no reaction]" : staggered ? " + stagger" : reaction);
    }
}

void guest_hits_on_event(const proto::GameEvent& ev, uint64_t delay_ms) {
    if (!g_enabled || ev.kind != proto::EventKind::GuestHit) return;
    std::lock_guard lock(g_mutex);
    if (g_delayed.size() < 128) g_delayed.push_back({now_ms() + delay_ms, ev});
}

}  // namespace cdmp::net
