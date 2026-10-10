#include "mod/net/enemy_relay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "common/enemy_load.h"
#include "common/keys.h"
#include "mod/game/actor_lookup.h"
#include "mod/log.h"
#include "mod/net/local_capture.h"
#include "mod/net/loot_twin.h"
#include "mod/net/native_suppress.h"
#include "mod/net/remote_puppet.h"

namespace cdmp::net {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kOpActorLoadDataAck = 2005;
constexpr uint16_t kOpRemoveActorAck = 2516;
constexpr uint16_t kOpStartActionReq = 2223;
constexpr uint16_t kOpRotateActorReq = 2828;
constexpr uint16_t kOpChangeBattleStateReq = 2257;  // 5 B: u32 actor | u8 state (0 = out of combat)
constexpr uint16_t kOpDoFrameEventReq = 2380;       // u8 4 | u32 attacker | ... | 665-byte target entries at the end
constexpr uint16_t kOpCurrentTargetChangeReq = 2984;  // u32 actor | u32 target | u64 seq | u8 | u8 reason
constexpr uint64_t kEngagedMs = 20000;
// An enemy that aimed at or struck one of us this long ago still counts as
// hostile: one locked on us sends no new CurrentTargetChangeReq, and its
// blows may all miss.
constexpr uint64_t kAimedMs = 120000;
// Server->client messages about an enemy (key at +0), relayed for its copy
// (enemigos-del-anfitrion.md §3.1): HP and death reliably, hit reactions not.
constexpr uint16_t kOpVaryHPAck = 2147;               // 92 B; +0x40 per-actor sequence, kept
constexpr uint16_t kOpNotifyHandleDeadAck = 2530;     // 19 B: dead | killer | reason
// What a server sends once a corpse is emptied (botin-invitado.md §1): a dead
// copy offered the loot prompt, but our server has no such actor and gave
// nothing (two-PC test, 6 Oct). Sent for it ~1 s after its death.
constexpr uint16_t kOpLootDropCharacterRewardListAck = 2897;  // u8 4 | u32 dead | n x 37 B
constexpr uint16_t kOpRemoveLootableAck = 2822;               // u32 dead | u32 player
constexpr uint64_t kNoLootAfterMs = 1000;
constexpr uint16_t kOpAttackTargetAck = 2922;         // hit effect on the target at +0
constexpr uint16_t kOpApplyActionRestrictionAck = 2162;
constexpr size_t kRotateActorReqSize = 176;
constexpr uint64_t kPosIntervalMs = 66;  // ~15 Hz
constexpr uint64_t kPosKeepAliveMs = 900;  // the remote buffer wants a sample every <= 1 s
constexpr size_t kStartActionReqSize = 725;
constexpr uint32_t kPuppetUser = 0x9010F001;
constexpr uint32_t kPuppetPlayer = 0xA010F001;
constexpr float kPickRadius = 40.0f;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void replace_all(Bytes& v, uint32_t from, uint32_t to) {
    if (!from || from == to) return;
    for (size_t i = 0; i + 4 <= v.size(); ++i) {
        if (u32_at(v.data() + i) == from) {
            std::memcpy(v.data() + i, &to, 4);
            i += 3;
        }
    }
}

// ---- sending side
std::mutex g_mutex;
std::map<uint32_t, Bytes> g_loads;    // world NPC key -> its ActorLoadDataAck payload
// world NPC key -> its last StartActionReqs, oldest first (up to kKeptActions):
// an enemy with two action layers needs one for each (live test: a copy with
// only its layer-0 action crashed on layer 1).
constexpr size_t kKeptActions = 4;
std::map<uint32_t, std::deque<Bytes>> g_actions;
std::map<uint32_t, uint32_t> g_sent;  // world NPC key -> copy key we announced
// World NPC key -> when it last showed it is fighting: battle state != 0, a
// blow at our player or the other player's copy, or a blow from our player.
// "Aimed" alone is not enough: 4,138 fleeing animals aim at the player, and
// 133 of 135 hostile humans but 1 of 26,654 animals enter battle state
// (pendiente-jinetes-animales.md §1.3).
std::map<uint32_t, uint64_t> g_engaged;
// World NPC key -> when it last aimed at (CurrentTargetChangeReq) or struck at
// our player or the other player's copy: with a faction, a
// hostile one (enemy_hostile_in_combat).
std::map<uint32_t, uint64_t> g_at_us;
// World NPCs spawned by a schedule (their copy goes without it, so a pose
// action of that schedule plays no animation: A-pose until the fight,
// copias-enemigos-faltan-pose-minimapa.md §3). Their copies stand in
// wait_default instead while they are not fighting.
std::set<uint32_t> g_schedule_spawned;
uint64_t g_schedule_actions_calmed = 0;  // schedule-only actions made wait_default (under g_mutex)
std::map<uint32_t, uint32_t> g_pose_of;  // its schedule pose action (the first that follows none, before a fight)
constexpr uint32_t kActionWaitDefault = 0x26107D3C;
std::set<uint32_t> g_refused;          // could not be copied safely (riders...): not retried
uint32_t g_next_net_id = 1;                    // copy key = kEnemyCopyBase + net id, 1..kMaxNetId
constexpr uint32_t kMaxNetId = kEnemyCopyMaxNetId;  // index 0xFEFFF: below the client's 0xFFFFE/F sentinels
constexpr uint64_t kNetIdQuarantineMs = 10000;  // a removed copy's id is not reused for 10 s
std::map<uint32_t, uint64_t> g_retired;         // net id -> when its copy was removed
std::deque<std::pair<proto::RelKind, Bytes>> g_outgoing;
// A new partner: what a tick read before the reset must not send (7 Oct: 24
// copies sent 79 ms after the link went, to the next partner).
uint32_t g_partner_epoch = 0;  // under g_mutex
std::map<uint32_t, Bytes> g_last_hp;  // world NPC key -> its last VaryHPAck payload
// Every actor of our world we saw loaded (any record of any ActorLoadDataAck)
// and its record type: a copy must not name any of them.
std::map<uint32_t, uint8_t> g_known;
std::deque<proto::GameEvent> g_out_events;  // actions of copied enemies
struct Sampled {
    proto::Vec3 pos{};
    uint64_t ms = 0;
    bool flip = false;
    bool has = false;
    int jumps_logged = 0;
};
constexpr float kMaxSpeed = 30.0f;  // m/s; faster is a bogus read (see below)

// The parent (u32 key) in an enemy's last StartActionReq (+0x116 Path.parent):
// during a finisher the enemy hangs from the player who does it.
uint32_t last_action_parent(uint32_t key) {
    std::lock_guard lock(g_mutex);
    auto a = g_actions.find(key);
    if (a == g_actions.end() || a->second.empty() || a->second.back().size() < 0x11A) return 0;
    return u32_at(a->second.back().data() + 0x116);
}
std::map<uint32_t, Sampled> g_sampled;  // world NPC key -> last position sent (network thread)
uint64_t g_last_sample_ms = 0;

// Our world's keys as the receiver must see them: the enemy becomes its
// copy, we become the receiver's copy of us, and the receiver's copy here
// (0xA010F001, the receiver itself) becomes kSelfMarker, which the receiver
// turns into its own player key. Without that swap, an enemy fighting the
// other player's copy here would fight OUR copy over there
// (pendiente-proyectiles.md §4.3).
void translate(Bytes& v, uint32_t key, uint32_t copy) {
    replace_all(v, kPuppetPlayer, proto::kSelfMarker);
    replace_all(v, key, copy);
    replace_all(v, capture_own_user_key(), kPuppetUser);
    replace_all(v, capture_own_player_key(), kPuppetPlayer);
}

// ---- receiving side (network thread only)
// The sender's kSelfMarker means us. False if our own key is not known yet.
bool untranslate(Bytes& v) {
    const uint32_t own = capture_own_player_key();
    if (!own) return false;
    replace_all(v, proto::kSelfMarker, own);
    return true;
}

// Sizes of the server->client and client->server messages we take from the
// other side and hand to the game (pendiente-robustez.md §4.2).
bool valid_size(uint16_t op, size_t n) {
    switch (op) {
        case kOpStartActionReq: return n == kStartActionReqSize;
        case kOpRotateActorReq: return n == kRotateActorReqSize;
        case kOpAttackTargetAck: return n >= 0xA0 && n <= 0x400;
        case kOpApplyActionRestrictionAck: return n == 77;
        case kOpVaryHPAck: return n == 92;
        case kOpNotifyHandleDeadAck: return n == 19;
        default: return false;
    }
}

std::atomic<bool> g_receive_enabled{false};
std::map<uint32_t, bool> g_copies;          // copy keys loaded here
std::map<uint32_t, uint32_t> g_copy_variant;  // copy key -> its enemy variant (load +0x18), for loot_twin
std::map<uint32_t, uint64_t> g_report_due;  // copy key -> when to log where it is

uint64_t now_ms() { return GetTickCount64(); }

struct Seen {
    uint32_t s390 = 0, s394 = 0;
    uint8_t s340 = 0, ts3dc = 0, login = 0;
    uint16_t state = 0;
};

bool read_seen(uintptr_t a, Seen& f) {
    __try {
        f.state = *reinterpret_cast<const uint16_t*>(a + 0x5E);
        f.login = *reinterpret_cast<const uint8_t*>(a + 0xCF);
        const auto comps = *reinterpret_cast<const uintptr_t*>(a + 0x68);
        if (!comps) return true;
        const auto st = *reinterpret_cast<const uintptr_t*>(comps + 0x20);
        if (st) {
            f.s390 = *reinterpret_cast<const uint32_t*>(st + 0x390);
            f.s394 = *reinterpret_cast<const uint32_t*>(st + 0x394);
            f.s340 = *reinterpret_cast<const uint8_t*>(st + 0x340);
        }
        const auto ts = *reinterpret_cast<const uintptr_t*>(comps + 0x1A0);
        if (ts) f.ts3dc = *reinterpret_cast<const uint8_t*>(ts + 0x3DC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void report(uint32_t copy) {
    const auto a = game::find_actor(copy);
    if (!a) {
        log::info("Enemy relay: copy 0x{:08X} is NOT in the game's actor list", copy);
        return;
    }
    const auto p = game::actor_world_position(copy);
    const auto me = game::actor_world_position(capture_own_player_key());
    Seen f;
    read_seen(*a, f);
    log::info("Enemy relay: copy 0x{:08X} exists: at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m from you | state +5E 0x{:X} "
              "login +CF {} | Status +390 0x{:X} +394 0x{:X} +340 {} | TS+3DC {}",
              copy, p ? p->x : NAN, p ? p->y : NAN, p ? p->z : NAN,
              p && me ? std::sqrt((p->x - me->x) * (p->x - me->x) + (p->y - me->y) * (p->y - me->y) +
                                  (p->z - me->z) * (p->z - me->z))
                      : NAN,
              f.state, f.login, f.s390, f.s394, f.s340, f.ts3dc);
}

float distance(const proto::Vec3& a, const proto::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

std::optional<proto::Vec3> my_position() {
    const uint32_t own = capture_own_player_key();
    return own ? game::actor_world_position(own) : std::nullopt;
}

// Type 3 only: type 6 is every animal down to beetles and dragonflies (the
// first live test copied those).
struct Candidate {
    uint32_t key;
    bool engaged;  // fighting in the last 20 s (g_engaged)
    float d;
};
std::vector<Candidate> candidates() {
    std::vector<Candidate> out;
    const auto me = my_position();
    if (!me) return out;
    std::vector<std::pair<uint32_t, bool>> keys;
    const uint64_t now = GetTickCount64();
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [key, load] : g_loads) {
            if (g_sent.count(key) || g_refused.count(key) || load.size() < 3 || load[2] != 3) continue;
            auto e = g_engaged.find(key);
            keys.emplace_back(key, e != g_engaged.end() && now - e->second < kEngagedMs);
        }
    }
    for (const auto& [key, engaged] : keys) {
        const auto p = game::actor_world_position(key);
        if (!p) continue;
        const float d = distance(*p, *me);
        if (d < kPickRadius) out.push_back({key, engaged, d});
    }
    return out;
}

// Ctrl+F6: the enemy we just hit, else the nearest one fighting us, else the
// nearest (the first picker preferred any attacker up to 45 m away, so the
// copies showed far from the enemy the player was looking at).
std::optional<uint32_t> pick_manual() {
    const auto list = candidates();
    const uint32_t hit = capture_last_attack_target();
    for (const auto& c : list) {
        if (c.key == hit) return c.key;
    }
    const Candidate* best = nullptr;
    for (const auto& c : list) {
        if (!best || (c.engaged && !best->engaged) || (c.engaged == best->engaged && c.d < best->d)) best = &c;
    }
    return best ? std::optional<uint32_t>(best->key) : std::nullopt;
}

// Every enemy load names where it spawns from (ids-enemigos-verificacion.md
// §2): a schedule (hash + element id), a faction camp slot (node + slot) or a
// sequence (hash + id), all relative to its first Transform T0. The copy goes
// without one, like an ambient spawn. (Not the cause of the invisible
// originals, copias-grupo-invisibles.md §6.7: that is the camp group.)
int strip_spawn_identity(Bytes& load) {
    static const uint8_t kSig[14] = {0x0C, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    size_t t0 = 0;
    for (size_t i = 0x24; i + sizeof kSig + 18 + 40 <= load.size(); ++i) {
        if (std::memcmp(load.data() + i, kSig, sizeof kSig) == 0) {
            t0 = i + 18;
            break;
        }
    }
    if (!t0) return -1;
    float scale[3];
    std::memcpy(scale, load.data() + t0 + 28, sizeof scale);
    if (scale[0] != 1.0f || scale[1] != 1.0f || scale[2] != 1.0f) return -1;
    auto at = [&](size_t off) { return t0 + off + 4 <= load.size() ? u32_at(load.data() + t0 + off) : 0u; };
    auto zero = [&](size_t off, size_t n) {
        if (t0 + off + n <= load.size()) std::memset(load.data() + t0 + off, 0, n);
    };
    int stripped = 0;
    if (const uint32_t schedule = at(42)) {
        replace_all(load, schedule, 0);  // also its copy in the sequence block
        zero(46, 6);                     // element id, instance serial
        stripped = 1;
    } else if (const uint32_t seq = at(1043); seq && (at(1047) & 0xF0000000u) == 0x20000000u) {
        replace_all(load, seq, 0);
        zero(1047, 8);
        stripped = 3;
    } else if (at(390)) {
        zero(390, 6);  // faction node + slot
        stripped = 2;
    }
    return stripped;
}

// ---- a copy must name no actor but itself (copias-grupo-invisibles.md §6)
// A camp enemy's load lists its party (number, leader, members) and, at the
// very end, its parent (+0xAC, the camp leader) and children (+0xB0, a
// leader's members). A copy kept them: a member copy joined the real party
// and never finished logging in; a LEADER copy logged in with the real
// members as its children, which set Status+0x390 bit 2 on them (model alpha
// 0) for good: the invisible originals of the live tests. The copy becomes a
// solo enemy instead.

bool is_key_shape(uint32_t v) { return keys::is_class(v, keys::kClassWorld) || keys::is_class(v, keys::kClassPlayer); }

// Our network keys on the receiving side: never a real actor there.
bool is_network_key(uint32_t v) {
    return is_enemy_copy_key(v) || v == kPuppetUser || v == kPuppetPlayer || v == 0xB010F001 || v == 0xB010F002 ||
           v == 0xB010F003;
}

// A copy key for a new enemy copy: net ids go round 1..kMaxNetId, skipping
// those in use and those removed less than 10 s ago (their removal may still
// be on its way). 0 if none is free. Caller holds g_mutex.
uint32_t alloc_copy_key_locked() {
    const uint64_t now = GetTickCount64();
    for (uint32_t tries = 0; tries < kMaxNetId; ++tries) {
        const uint32_t id = g_next_net_id;
        g_next_net_id = id >= kMaxNetId ? 1 : id + 1;
        if (auto r = g_retired.find(id); r != g_retired.end()) {
            if (now - r->second < kNetIdQuarantineMs) continue;
            g_retired.erase(r);
        }
        const uint32_t copy = kEnemyCopyBase + id;
        bool used = false;
        for (const auto& [k, c] : g_sent) used = used || c == copy;
        if (!used) return copy;
    }
    return 0;
}

// The actor tail (parent +0xAC, children +0xB0, Hierarchy): common/enemy_load.h.
using enemy_load::parse_tail;

struct PartyBlock {
    size_t start = 0, end = 0;
    uint64_t number = 0;
    size_t members = 0, followers = 0;
};

// ClientPartyActorComponent (vf11 0x21D8020), anchored on the actor's own key
// in its member list: u64 number (0x201_xxxxxxxx) | u32 leader | u32 | 12 B |
// u8 | u32 n | n keys | u32 formation | u32 own index | u8 | u32 m | m keys | u32 leader.
std::optional<PartyBlock> find_party(const Bytes& pl, uint32_t own) {
    const size_t len = pl.size();
    for (size_t pos = 8; pos + 4 <= len; ++pos) {
        if (u32_at(pl.data() + pos) != own) continue;
        for (size_t idx = 0; idx < 64; ++idx) {
            if (pos < 4 * idx + 40) break;
            const size_t start_s = pos - 4 * idx;
            const uint32_t n = u32_at(pl.data() + start_s - 4);
            if (!(idx < n && n <= 100)) continue;
            const size_t e = start_s + 4 * n;
            if (e + 13 > len || u32_at(pl.data() + e + 4) != idx) continue;
            const uint32_t m = u32_at(pl.data() + e + 9);
            if (m > 100 || e + 13 + 4 * m + 4 > len) continue;
            const size_t start = start_s - 33;
            uint64_t number;
            std::memcpy(&number, pl.data() + start, 8);
            if ((number >> 32) != 0x201 || !is_key_shape(u32_at(pl.data() + start + 8)) || pl[e + 8] > 1 ||
                pl[start_s - 5] > 1) {
                continue;
            }
            return PartyBlock{start, e + 13 + 4 * m + 4, number, n, m};
        }
    }
    return std::nullopt;
}

// The party block of a solo NPC, byte for byte (once in each of the 764 solo
// type-3 and 727 type-6 loads captured): no group, no members, index -1.
const Bytes kSoloParty = [] {
    Bytes b(50, 0);
    b[28] = 0x01;                          // u8 Party+0x24
    std::memset(b.data() + 37, 0xFF, 4);  // own index -1
    return b;
}();

// CommonTargetActorComponent vf11: u32 | u32 | u32 n | n x 12 B | u64 | u32 | u32 +0x198
// (the leader's key in a member; its own key in solos and leaders).
int fix_target198(Bytes& pl, uint32_t from, uint32_t to) {
    int fixed = 0;
    for (size_t q = 8; q + 4 <= pl.size(); ++q) {
        if (u32_at(pl.data() + q) != from) continue;
        for (uint32_t e = 0; e < 16; ++e) {
            if (q < 12 + 12 * e + 4 + 8) break;
            if (u32_at(pl.data() + q - 12 - 12 * e - 4) == e) {
                std::memcpy(pl.data() + q, &to, 4);
                ++fixed;
                break;
            }
        }
    }
    return fixed;
}

// Caller holds g_mutex. First known actor key left in `v` (not ours), or 0.
uint32_t residual_key_locked(const Bytes& v, size_t from, uint32_t self, size_t* where = nullptr) {
    for (size_t i = from; i + 4 <= v.size(); ++i) {
        const uint32_t k = u32_at(v.data() + i);
        if (k == self || k == 0 || is_network_key(k)) continue;
        if (g_known.count(k)) {
            if (where) *where = i;
            return k;
        }
    }
    return 0;
}

// Removes the list of gimmicks docked on the enemy (03 04 FF FF FF FF | u32 n
// | n keys, 1-8 keys that are all type-7 loads we saw); n becomes 0. Returns
// how many keys went.
int strip_docked_gimmicks(Bytes& load) {
    static const uint8_t kHead[6] = {0x03, 0x04, 0xFF, 0xFF, 0xFF, 0xFF};
    std::lock_guard lock(g_mutex);
    for (size_t i = 5; i + 10 <= load.size(); ++i) {
        if (std::memcmp(load.data() + i, kHead, sizeof kHead) != 0) continue;
        const uint32_t n = u32_at(load.data() + i + 6);
        if (n < 1 || n > 8 || i + 10 + 4 * n > load.size()) continue;
        bool all_gimmicks = true;
        for (uint32_t j = 0; j < n && all_gimmicks; ++j) {
            const auto k = g_known.find(u32_at(load.data() + i + 10 + 4 * j));
            all_gimmicks = k != g_known.end() && k->second == 7;
        }
        if (!all_gimmicks) continue;
        const uint32_t zero = 0;
        std::memcpy(load.data() + i + 6, &zero, 4);
        load.erase(load.begin() + static_cast<ptrdiff_t>(i + 10),
                   load.begin() + static_cast<ptrdiff_t>(i + 10 + 4 * n));
        return static_cast<int>(n);
    }
    return 0;
}

// Turns `load` (keys already translated) into a solo enemy named only by
// `copy`. False (with `why`) when it cannot be done safely: then it is not sent.
bool sanitize_load(Bytes& load, uint32_t copy, std::string& why) {
    const auto tail = parse_tail(load);
    if (!tail) {
        why = "its actor tail could not be read";
        return false;
    }
    if (!tail->hierarchy.empty()) {
        why = "it has hierarchy children";
        return false;
    }
    uint8_t parent_type = 0;
    {
        std::lock_guard lock(g_mutex);
        if (auto k = g_known.find(tail->parent); k != g_known.end()) parent_type = k->second;
    }
    if (tail->parent && (parent_type == 5 || parent_type == 7)) {
        // Riders (mount = type 5) and tower shooters (type 7) are not copied
        // yet (pendiente-jinetes-animales.md §6): say which, to count them.
        why = parent_type == 5 ? std::format("it rides mount 0x{:08X} (riders are not copied yet)", tail->parent)
                               : std::format("it stands on gimmick 0x{:08X}, a tower or cart (not copied yet)",
                                             tail->parent);
        return false;
    }
    // 1) tail: parent 0, no children
    load.resize(tail->ac);
    load.insert(load.end(), 6, 0);
    // 2) party block -> the solo template
    std::string party = "no party";
    if (auto pb = find_party(load, copy)) {
        party = std::format("party 0x{:X} ({} members, {} followers) removed", pb->number, pb->members, pb->followers);
        load.erase(load.begin() + pb->start, load.begin() + pb->end);
        load.insert(load.begin() + pb->start, kSoloParty.begin(), kSoloParty.end());
    }
    // 3) Target+0x198 names the leader in a member: its own key instead
    const int target = tail->parent ? fix_target198(load, tail->parent, copy) : 0;
    // 3b) Its docked gimmicks (an item in its hand, a separate "Game Mechanic"
    // load of type 7 socketed on it): 03 04 FF FF FF FF | u32 n | n keys. The
    // copy goes without them; before, such enemies were not copied at all and
    // fought unseen (Blinding Arrow 52666 and 52669, Shadow test 7 Oct).
    const int docked = strip_docked_gimmicks(load);
    // 4) record length
    const uint16_t len = static_cast<uint16_t>(load.size() - 2);
    std::memcpy(load.data(), &len, 2);
    // 5) nothing of our world may be left
    size_t at = 0;
    uint32_t left = 0;
    {
        std::lock_guard lock(g_mutex);
        left = residual_key_locked(load, 5, copy, &at);
    }
    if (left) {
        why = std::format("actor 0x{:08X} still named at +{}", left, at);
        return false;
    }
    why = std::format("parent 0x{:08X} and {} children removed, {}, Target+198 x{}{}", tail->parent,
                      tail->children.size(), party, target,
                      docked ? std::format(", {} docked gimmick(s) removed", docked) : std::string());
    return true;
}

// Caller holds g_mutex. Key fields of a relayed message (§6.5): enemies we
// copied become their copies, our player/user already are translated, and
// any other actor of our world becomes "none" (0, or FF x 8 for u64 parents).
void map_field_locked(Bytes& v, size_t off, bool parent64) {
    if (off + 4 > v.size()) return;
    const uint32_t k = u32_at(v.data() + off);
    if (k == 0 || k == 0xFFFFFFFFu || is_network_key(k)) return;
    if (auto c = g_sent.find(k); c != g_sent.end()) {
        std::memcpy(v.data() + off, &c->second, 4);
        return;
    }
    if (!g_known.count(k)) return;  // not an actor key
    if (parent64 && off + 8 <= v.size()) {
        std::memset(v.data() + off, 0xFF, 8);
    } else {
        std::memset(v.data() + off, 0, 4);
    }
}

// Caller holds g_mutex. Maps a relayed message (payload, keys translated);
// false if a key of our world is still in it (then it is dropped).
uint64_t g_dropped_messages = 0;
bool map_message_locked(uint16_t op, Bytes& v, uint32_t self) {
    switch (op) {
        case kOpStartActionReq: map_field_locked(v, 0x116, true), map_field_locked(v, 0x16C, false); break;
        case kOpRotateActorReq: map_field_locked(v, 0x54, true); break;
        case kOpVaryHPAck: map_field_locked(v, 0x54, false); break;
        case kOpNotifyHandleDeadAck: map_field_locked(v, 0x04, false); break;
        case kOpAttackTargetAck:
            map_field_locked(v, 0x04, false), map_field_locked(v, 0xBC, false), map_field_locked(v, 0xCE, false);
            break;
        case kOpApplyActionRestrictionAck: map_field_locked(v, 0x49, false); break;
        default: break;
    }
    size_t at = 0;
    if (const uint32_t left = residual_key_locked(v, 4, self, &at)) {
        if (g_dropped_messages++ % 50 == 0) {
            log::info("Enemy relay: message {} about 0x{:08X} dropped: actor 0x{:08X} at +{} [{} dropped]", op, self,
                      left, at, g_dropped_messages);
        }
        return false;
    }
    return true;
}

// Actions defined only in the schedule action charts (build 2976,
// enemigos-agachados-apose.md): a copy has no schedule, so it cannot play
// them and stands in its reference pose (the A-pose; crouching bandits at a
// camp). Never seen in a fight.
constexpr uint32_t kScheduleOnlyActions[] = {
#include "common/schedule_actions_2976.inc"
};
bool is_schedule_only(uint32_t action) {
    return std::binary_search(std::begin(kScheduleOnlyActions), std::end(kScheduleOnlyActions), action);
}
constexpr uint32_t kActionStandUp = 0x5BD8B2C2;  // leaving a schedule pose (common chart)

// Caller holds g_mutex. On a copy's main-layer StartActionReq (+4 the action
// before, +8 the new one): a schedule-only action becomes wait_default, and
// so does standing up from one while not fighting (the copy already stands).
// Then the learnt pose (other builds): a schedule-spawned enemy's pose action
// (one that follows no action: +4 = 0) while it is not fighting becomes
// wait_default; only that pose action (and its repeats), of a human (type 3).
void calm_pose_locked(Bytes& req, uint32_t key) {
    if (req.size() < 12) return;
    {
        const uint32_t prev = u32_at(req.data() + 4), next = u32_at(req.data() + 8);
        const bool from_schedule = prev && is_schedule_only(prev);
        bool changed = false;
        if (from_schedule) {
            std::memcpy(req.data() + 4, &kActionWaitDefault, 4);
            changed = true;
        }
        if (next && (is_schedule_only(next) || (next == kActionStandUp && from_schedule && [&] {
                         auto e = g_engaged.find(key);
                         return e == g_engaged.end() || GetTickCount64() - e->second >= kEngagedMs;
                     }()))) {
            std::memcpy(req.data() + 8, &kActionWaitDefault, 4);
            changed = true;
        }
        if (changed) {
            if (++g_schedule_actions_calmed <= 10 || g_schedule_actions_calmed % 100 == 0) {
                log::info("Enemy relay: 0x{:08X} schedule action 0x{:08X}>0x{:08X} sent as wait_default ({} so far)", key,
                          prev, next, g_schedule_actions_calmed);
            }
            return;
        }
    }
    if (!g_schedule_spawned.count(key)) return;
    auto l = g_loads.find(key);
    if (l == g_loads.end() || l->second.size() < 3 || l->second[2] != 3) return;
    const uint32_t prev = u32_at(req.data() + 4), next = u32_at(req.data() + 8);
    if (!next) return;
    const bool fighting = [&] {
        auto e = g_engaged.find(key);
        return e != g_engaged.end() && GetTickCount64() - e->second < kEngagedMs;
    }();
    auto pose = g_pose_of.find(key);
    if (pose == g_pose_of.end()) {
        if (prev != 0 || fighting) return;
        if (g_pose_of.size() > 2000) g_pose_of.clear();
        pose = g_pose_of.emplace(key, next).first;
    }
    if (next != pose->second || (prev != 0 && prev != pose->second)) return;
    std::memcpy(req.data() + 4, &kActionWaitDefault, 4);
    std::memcpy(req.data() + 8, &kActionWaitDefault, 4);
}

// `epoch`: the partner it was decided for (0 = the current one); another one
// since then and nothing is sent. True if it was sent.
bool send_enemy(uint32_t key, const char* why, uint32_t epoch = 0) {
    if (capture_key_alarm()) return false;  // the game uses keys in our range (local_capture.cpp)
    Bytes load, action;
    uint32_t copy = 0;
    {
        std::lock_guard lock(g_mutex);
        if (epoch && epoch != g_partner_epoch) return false;
        auto l = g_loads.find(key);
        if (l == g_loads.end() || g_sent.count(key)) return false;
        load = l->second;
        auto a = g_actions.find(key);
        if (a != g_actions.end()) {
            for (const Bytes& b : a->second) action.insert(action.end(), b.begin(), b.end());
        }
        copy = alloc_copy_key_locked();
        if (!copy) {
            log::warn("Enemy relay: enemy 0x{:08X} NOT copied: no free copy key", key);
            return false;
        }
        g_sent[key] = copy;
        if (!epoch) epoch = g_partner_epoch;
    }
    // Keys of OUR world, as the receiver must see them: the enemy gets its
    // copy key, our user and player become the receiver's copies of us.
    translate(load, key, copy);
    translate(action, key, copy);
    const int origin = strip_spawn_identity(load);
    std::string cleaned;
    if (!sanitize_load(load, copy, cleaned)) {
        std::lock_guard lock(g_mutex);
        g_sent.erase(key);
        g_refused.insert(key);
        log::info("Enemy relay: enemy 0x{:08X} NOT copied: {}", key, cleaned);
        return false;
    }
    {
        // Its recent actions, each mapped; those still naming our world go.
        std::lock_guard lock(g_mutex);
        if (origin == 1) g_schedule_spawned.insert(key);
        Bytes kept;
        for (size_t at = 0; at + kStartActionReqSize <= action.size(); at += kStartActionReqSize) {
            Bytes one(action.begin() + at, action.begin() + at + kStartActionReqSize);
            calm_pose_locked(one, key);
            if (map_message_locked(kOpStartActionReq, one, copy)) kept.insert(kept.end(), one.begin(), one.end());
        }
        action.swap(kept);
    }
    Bytes msg(8);
    std::memcpy(msg.data(), &copy, 4);
    const uint32_t n = static_cast<uint32_t>(load.size());
    std::memcpy(msg.data() + 4, &n, 4);
    msg.insert(msg.end(), load.begin(), load.end());
    msg.insert(msg.end(), action.begin(), action.end());
    {
        std::lock_guard lock(g_mutex);
        if (epoch != g_partner_epoch || !g_sent.count(key)) return false;  // the partner went meanwhile
        g_outgoing.emplace_back(proto::RelKind::EnemySpawn, std::move(msg));
        // Its current HP right behind (the load carries the HP it was loaded with).
        if (auto hp = g_last_hp.find(key); hp != g_last_hp.end()) {
            Bytes state(2);
            std::memcpy(state.data(), &kOpVaryHPAck, 2);
            Bytes body = hp->second;
            translate(body, key, copy);
            if (map_message_locked(kOpVaryHPAck, body, copy)) {
                state.insert(state.end(), body.begin(), body.end());
                g_outgoing.emplace_back(proto::RelKind::EnemyState, std::move(state));
            }
        }
    }
    std::string name;
    if (load.size() > 0x24) {
        uint32_t chars = 0;
        std::memcpy(&chars, load.data() + 0x20, 4);
        for (uint32_t i = 0; i < chars && i < 40 && 0x24 + 2 * i + 1 < load.size(); ++i) {
            const char c = static_cast<char>(load[0x24 + 2 * i]);
            name.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
    }
    static const char* const kOrigin[] = {"no spawn id", "schedule id removed", "camp slot removed",
                                          "sequence id removed"};
    log::info("Enemy relay: sending enemy 0x{:08X} \"{}\" as copy 0x{:08X} ({}; {} B load, {} recent actions, {}; "
              "sanitized: {})",
              key, name, copy, why, n, action.size() / kStartActionReqSize,
              origin < 0 ? "T0 not found" : kOrigin[origin], cleaned);
    return true;
}

void send_picked() {
    if (const auto key = pick_manual()) {
        send_enemy(*key, "picked");
    } else {
        log::info("Enemy relay: no enemy loaded within {:.0f} m", kPickRadius);
    }
}

// Caller holds g_mutex.
void remove_locked(uint32_t key) {
    auto it = g_sent.find(key);
    if (it == g_sent.end()) return;
    Bytes msg(4);
    std::memcpy(msg.data(), &it->second, 4);
    g_retired[it->second - kEnemyCopyBase] = GetTickCount64();
    g_outgoing.emplace_back(proto::RelKind::EnemyRemove, std::move(msg));
    g_sent.erase(it);
}

void remove_sent() {
    Bytes msg;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [key, copy] : g_sent) {
            const size_t at = msg.size();
            msg.resize(at + 4);
            std::memcpy(msg.data() + at, &copy, 4);
            g_retired[copy - kEnemyCopyBase] = GetTickCount64();
        }
        g_sent.clear();
        if (!msg.empty()) g_outgoing.emplace_back(proto::RelKind::EnemyRemove, msg);
    }
    log::info("Enemy relay: removing {} enemy copies", msg.size() / 4);
}

// Ctrl+F7: every enemy fighting us within kPickRadius gets a copy (up to
// kAutoMax); copies of those gone or far away are removed.
constexpr size_t kAutoMax = 8;
constexpr float kAutoDropRadius = 80.0f;
std::atomic<bool> g_auto{false};
std::atomic<bool> g_linked{false};
// A copy goes only after its enemy stayed away that long: during a finisher
// the PLAYER's own position reads wrong for a moment and every copy was
// dropped and sent again (they blinked; live test 2026-10-04).
constexpr uint64_t kFarForMs = 3000, kGoneForMs = 1500;
std::map<uint32_t, uint64_t> g_away_since;  // world NPC key -> first tick it looked far/gone
proto::Vec3 g_me_last{};
uint64_t g_me_last_ms = 0;

// ---- phase 5 on the HOST (fase5-suprimir-enemigos-invitado.md §4.2, §5.7):
// while the guest's enemy zone is on, every zone enemy (the shared predicate,
// common/enemy_load.h) within its radius of us gets a copy, because the guest
// hides its own there: up to kZoneMax, those fighting first, then the ones
// nearest the guest's copy; one goes beyond radius + 20 m. Outside the zone
// (or in the guest's dry run) the relay works as before.
struct PartnerZone {
    bool on = false, dry = false, caution = false;
    float radius = 150.0f;
    uint32_t seq = 0;
};
PartnerZone g_partner_zone;               // under g_mutex
std::vector<uint32_t> g_learnt_factions;  // under g_mutex: hostile by our own fights (shared with the guest)
constexpr size_t kMaxLearntFactions = 64;
constexpr size_t kZoneMax = 48;
constexpr float kZoneDropExtra = 20.0f;
constexpr size_t kZoneSendsPerTick = 6;  // a tick is 500 ms: ~12 loads a second, nearest first
// More zone enemies than kZoneMax: the nearest kZoneMax to us (the zone's
// centre) get copies, and the radius relayed in our answer shrinks to theirs,
// so the guest hides only those (the golden rule; 7 Oct: 43-54 zone enemies,
// 24 copies, the rest hidden on the guest; enemigos-faltan-invitado-7oct.md §3).
float g_zone_radius = 150.0f;  // under g_mutex: what our answers say
uint64_t g_zone_radius_ms = 0;
float g_answered_radius = 150.0f;
std::atomic<bool> g_relay_running{false};  // enemy_test_loop runs (the host's [dev] enemy_relay=1)
bool g_answered_capable = false;           // what the last answer said (network thread)
size_t g_answered_learnt = 0;
uint64_t g_answered_ms = 0;

// Caller holds g_mutex. The predicate with a camp member judged by its
// leader, both from our world's loads.
bool is_zone_enemy_locked(uint32_t key, const enemy_load::ZoneRules& rules) {
    auto l = g_loads.find(key);
    if (l == g_loads.end()) return false;
    const auto c = enemy_load::zone_check(l->second, rules);
    if (c.verdict == enemy_load::Verdict::Yes) return true;
    if (c.verdict != enemy_load::Verdict::FollowParent) return false;
    auto p = g_loads.find(c.parent);
    return p != g_loads.end() && enemy_load::zone_check(p->second, rules).verdict == enemy_load::Verdict::Yes;
}

// Caller holds g_mutex. `key` aimed at or struck one of us: a faction the
// tables do not call hostile is learnt as such for this session, if all the
// rest of the predicate holds (pendiente-3g §3.2 step 4).
void learn_faction_locked(uint32_t key) {
    auto l = g_loads.find(key);
    if (l == g_loads.end() || l->second.size() < 0x20 || l->second[2] != 3) return;
    const uint32_t faction = u32_at(l->second.data() + 0x1C);
    if (!faction || enemy_load::is_table_hostile_faction(faction) || g_learnt_factions.size() >= kMaxLearntFactions ||
        std::find(g_learnt_factions.begin(), g_learnt_factions.end(), faction) != g_learnt_factions.end()) {
        return;
    }
    const uint32_t one[] = {faction};
    if (enemy_load::zone_check(l->second, {g_partner_zone.caution, one}).verdict == enemy_load::Verdict::No) return;
    g_learnt_factions.push_back(faction);
    log::info("Enemy relay: faction {} learnt as hostile (0x{:08X} \"{}\" fought one of us)", faction, key,
              enemy_load::name(l->second));
}

struct ZoneCandidate {
    uint32_t key;
    bool engaged;
    float d_me, d_guest;
};

// Every zone enemy within the radius of `me`, not copied yet unless `all`.
std::vector<ZoneCandidate> zone_candidates(const PartnerZone& z, const proto::Vec3& me,
                                           const std::optional<proto::Vec3>& guest, bool all) {
    std::vector<std::pair<uint32_t, bool>> keys;
    const uint64_t now = GetTickCount64();
    {
        std::lock_guard lock(g_mutex);
        const enemy_load::ZoneRules rules{z.caution, g_learnt_factions};
        for (const auto& [key, load] : g_loads) {
            if (load.size() < 3 || load[2] != 3 || g_refused.count(key) || (!all && g_sent.count(key))) continue;
            if (!is_zone_enemy_locked(key, rules)) continue;
            auto e = g_engaged.find(key);
            keys.emplace_back(key, e != g_engaged.end() && now - e->second < kEngagedMs);
        }
    }
    std::vector<ZoneCandidate> out;
    for (const auto& [key, engaged] : keys) {
        const auto p = game::actor_world_position(key);
        if (!p) continue;
        const float d = enemy_load::flat_distance(*p, me);
        if (d > z.radius) continue;
        out.push_back({key, engaged, d, guest ? enemy_load::flat_distance(*p, *guest) : d});
    }
    // Nearest the zone's centre (us) first: the guest hides within a radius of us.
    std::sort(out.begin(), out.end(), [](const ZoneCandidate& a, const ZoneCandidate& b) { return a.d_me < b.d_me; });
    return out;
}

// The radius relayed for these candidates (nearest first): the guest's, or
// just inside the kZoneMax-th nearest (50 m at least: more than kZoneMax
// within 50 m and some hidden ones get no copy, logged).
float zone_radius_for(const std::vector<ZoneCandidate>& all, float radius) {
    if (all.size() <= kZoneMax) return radius;
    const float r = std::min(radius, all[kZoneMax - 1].d_me - 2.0f);
    if (r < proto::kMinZoneRadius) {
        static std::atomic<uint64_t> told{0};
        if (told++ % 120 == 0) {
            log::warn("Enemy relay: more than {} zone enemies within {:.0f} m: some the guest hides get no copy",
                      kZoneMax, proto::kMinZoneRadius);
        }
    }
    return std::max(proto::kMinZoneRadius, r);
}

void zone_tick(const PartnerZone& z, const proto::Vec3& me, bool me_jumped, uint64_t now, uint32_t epoch) {
    std::map<uint32_t, uint32_t> sent;
    std::vector<uint32_t> learnt;
    float radius = z.radius;
    {
        std::lock_guard lock(g_mutex);
        if (epoch != g_partner_epoch) return;
        sent = g_sent;
        learnt = g_learnt_factions;
    }
    const auto all = me_jumped ? std::vector<ZoneCandidate>{} : zone_candidates(z, me, puppet_position(), true);
    if (!me_jumped) {
        const float want = zone_radius_for(all, z.radius);
        std::lock_guard lock(g_mutex);
        // Down at once (the guest hides less), up at most every 3 s; 5 m steps.
        if (std::fabs(want - g_zone_radius) >= 5.0f && (want < g_zone_radius || now - g_zone_radius_ms >= 3000)) {
            log::info("Enemy relay: {} zone enemies within {:.0f} m: relaying the nearest {} (zone radius {:.0f} m)",
                      all.size(), z.radius, std::min(all.size(), kZoneMax), want);
            g_zone_radius = want;
            g_zone_radius_ms = now;
        }
        radius = g_zone_radius;
    }
    const enemy_load::ZoneRules rules{z.caution, learnt};
    for (const auto& [key, copy] : sent) {
        bool zone_enemy;
        {
            std::lock_guard lock(g_mutex);
            zone_enemy = is_zone_enemy_locked(key, rules);
        }
        const auto p = game::actor_world_position(key);
        const bool gone = !p, too_far = p && !me_jumped && enemy_load::flat_distance(*p, me) > radius + kZoneDropExtra;
        if (zone_enemy && !gone && !too_far) {
            g_away_since.erase(key);
            continue;
        }
        if (zone_enemy) {
            auto [it, first] = g_away_since.try_emplace(key, now);
            if (now - it->second < (gone ? kGoneForMs : kFarForMs)) continue;
            g_away_since.erase(it);
        }
        // Not a zone enemy (a copy from before the zone: a boss, an animal...):
        // the guest sees its own there, so this one goes at once.
        std::lock_guard lock(g_mutex);
        remove_locked(key);
        log::info("Enemy relay: enemy 0x{:08X} {}; its copy 0x{:08X} removed", key,
                  !zone_enemy ? "is not a zone enemy" : gone ? "is gone" : "left the zone", copy);
    }
    if (me_jumped) return;
    std::vector<const ZoneCandidate*> wanted;  // not copied yet, within the radius, nearest first
    size_t count = 0;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& c : all) {
            if (c.d_me <= radius && !g_sent.count(c.key)) wanted.push_back(&c);
        }
        count = g_sent.size();
        // Full: copies beyond the radius (kept by kZoneDropExtra) make room
        // for nearer ones, farthest first.
        if (count >= kZoneMax && !wanted.empty()) {
            std::vector<std::pair<float, uint32_t>> outside;
            for (const auto& c : all) {
                if (c.d_me > radius && g_sent.count(c.key)) outside.emplace_back(c.d_me, c.key);
            }
            std::sort(outside.rbegin(), outside.rend());
            for (const auto& [d, key] : outside) {
                if (count + wanted.size() <= kZoneMax) break;
                log::info("Enemy relay: enemy 0x{:08X} ({:.0f} m) makes room for a nearer one; its copy removed", key, d);
                remove_locked(key);
                --count;
            }
        }
    }
    size_t sends = 0;
    for (const ZoneCandidate* c : wanted) {
        if (count >= kZoneMax || sends >= kZoneSendsPerTick) break;
        if (send_enemy(c->key, c->engaged ? "zone, fighting" : "zone", epoch)) ++count, ++sends;
    }
}

// The guest's dry run: what we would copy, logged when it changes.
std::set<uint32_t> g_dry_zone_logged;  // under g_mutex
uint64_t g_dry_zone_ms = 0;
void zone_dry_log(const PartnerZone& z, const proto::Vec3& me, uint64_t now) {
    if (now - g_dry_zone_ms < 2000) return;
    g_dry_zone_ms = now;
    const auto list = zone_candidates(z, me, puppet_position(), true);
    std::set<uint32_t> keys;
    for (size_t i = 0; i < list.size() && i < kZoneMax; ++i) keys.insert(list[i].key);
    std::string text;
    {
        std::lock_guard lock(g_mutex);
        if (keys == g_dry_zone_logged) return;
        g_dry_zone_logged = keys;
        for (size_t i = 0; i < list.size() && i < kZoneMax; ++i) {
            auto l = g_loads.find(list[i].key);
            text += std::format("{}0x{:08X} \"{}\" {:.0f} m", i ? ", " : "", list[i].key,
                                l != g_loads.end() ? enemy_load::name(l->second) : std::string(), list[i].d_me);
        }
    }
    log::info("Enemy relay: zone (the guest's dry run) WOULD copy {} of {} zone enemies within {:.0f} m{}{}",
              keys.size(), list.size(), z.radius, keys.empty() ? "" : ": ", text);
}

void auto_tick() {
    const auto me = my_position();
    if (!me) return;
    const uint64_t now = GetTickCount64();
    // Our own position jumped: not to be trusted this tick.
    const bool me_jumped = g_me_last_ms && now - g_me_last_ms < 2000 && distance(*me, g_me_last) > 30.0f;
    if (!me_jumped) {
        g_me_last = *me;
        g_me_last_ms = now;
    }
    PartnerZone zone;
    uint32_t epoch = 0;
    {
        std::lock_guard lock(g_mutex);
        zone = g_partner_zone;
        epoch = g_partner_epoch;
    }
    if (zone.on && !zone.dry) {
        zone_tick(zone, *me, me_jumped, now, epoch);
        return;
    }
    if (zone.on) zone_dry_log(zone, *me, now);
    std::map<uint32_t, uint32_t> sent;
    {
        std::lock_guard lock(g_mutex);
        sent = g_sent;
    }
    for (const auto& [key, copy] : sent) {
        const auto p = game::actor_world_position(key);
        const bool gone = !p, too_far = p && !me_jumped && distance(*p, *me) > kAutoDropRadius;
        if (!gone && !too_far) {
            g_away_since.erase(key);
            continue;
        }
        auto [it, first] = g_away_since.try_emplace(key, now);
        if (now - it->second < (gone ? kGoneForMs : kFarForMs)) continue;
        g_away_since.erase(it);
        std::lock_guard lock(g_mutex);
        remove_locked(key);
        log::info("Enemy relay: enemy 0x{:08X} {}; its copy 0x{:08X} removed", key, gone ? "is gone" : "is far",
                  copy);
    }
    size_t count = 0;
    {
        std::lock_guard lock(g_mutex);
        count = g_sent.size();
    }
    auto list = candidates();
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    for (const auto& c : list) {
        if (count >= kAutoMax || me_jumped) break;
        if (!c.engaged) continue;
        send_enemy(c.key, "fighting you");
        ++count;
    }
}

}  // namespace

uint32_t enemy_variant(uint32_t key) {
    std::lock_guard lock(g_mutex);
    auto l = g_loads.find(key);
    return l != g_loads.end() && l->second.size() >= 0x1C ? u32_at(l->second.data() + 0x18) : 0;
}

uint32_t enemy_copy_of(uint32_t key) {
    std::lock_guard lock(g_mutex);
    auto c = g_sent.find(key);
    return c != g_sent.end() ? c->second : 0;
}

uint32_t enemy_original_of(uint32_t copy) {
    std::lock_guard lock(g_mutex);
    for (const auto& [key, c] : g_sent) {
        if (c == copy) return key;
    }
    return 0;
}

bool enemy_hostile_in_combat(uint32_t key) {
    if (suppress_is_hidden(key)) return false;  // held out of our client (native_suppress.h)
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    // Load +0x1C: faction key, 0 for animals and civilians (enemigos-del-anfitrion.md §1.1).
    auto l = g_loads.find(key);
    if (l == g_loads.end() || l->second.size() < 0x20 || l->second[2] != 3 || u32_at(l->second.data() + 0x1C) == 0) {
        return false;
    }
    auto a = g_at_us.find(key);
    // Aiming at one of us is the AI's own decision to fight us: an archer
    // shooting from afar sends no battle state and lands no blow for long.
    return a != g_at_us.end() && now - a->second < kAimedMs;
}

bool enemy_guest_may_hit(uint32_t key) {
    if (enemy_hostile_in_combat(key)) return true;
    if (suppress_is_hidden(key)) return false;
    std::lock_guard lock(g_mutex);
    if (!g_partner_zone.on || g_partner_zone.dry || !g_sent.count(key)) return false;
    auto l = g_loads.find(key);
    if (l == g_loads.end() || l->second.size() < 0x20 || l->second[2] != 3 || u32_at(l->second.data() + 0x1C) == 0) {
        return false;
    }
    return is_zone_enemy_locked(key, {g_partner_zone.caution, g_learnt_factions});
}

void enemy_note_guest_blow(uint32_t key) {
    std::lock_guard lock(g_mutex);
    if (g_loads.count(key)) g_engaged[key] = g_at_us[key] = GetTickCount64();
}

void enemy_observe_s2c(const uint8_t* packet, size_t size) {
    if (size < 5) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    if ((op == kOpVaryHPAck || op == kOpNotifyHandleDeadAck || op == kOpAttackTargetAck ||
         op == kOpApplyActionRestrictionAck) &&
        n >= 4) {
        const uint32_t key = u32_at(p);
        std::lock_guard lock(g_mutex);
        if (op == kOpAttackTargetAck && n >= 8) {
            const uint32_t attacker = u32_at(p + 4), own = capture_own_player_key();
            const uint64_t t = GetTickCount64();
            if (own && attacker == own && g_loads.count(key)) g_engaged[key] = t;
            if (own && key == own && g_loads.count(attacker)) {
                g_engaged[attacker] = t, g_at_us[attacker] = t;
                learn_faction_locked(attacker);
            }
        }
        if (op == kOpVaryHPAck && g_loads.count(key)) g_last_hp[key] = Bytes(p, p + n);
        auto sent = g_sent.find(key);
        if (sent == g_sent.end()) return;
        Bytes msg(2);
        std::memcpy(msg.data(), &op, 2);
        Bytes body(p, p + n);
        translate(body, key, sent->second);
        if (!map_message_locked(op, body, sent->second)) return;
        msg.insert(msg.end(), body.begin(), body.end());
        if (op == kOpVaryHPAck || op == kOpNotifyHandleDeadAck) {
            g_outgoing.emplace_back(proto::RelKind::EnemyState, std::move(msg));
        } else if (g_out_events.size() < 512) {
            g_out_events.push_back({proto::EventKind::EnemyAct, std::move(msg)});
        }
        return;
    }
    if (op == kOpActorLoadDataAck && n >= 7) {
        std::lock_guard lock(g_mutex);
        if (g_known.size() > 20000) g_known.clear();
        for (size_t at = 0; at + 7 <= n;) {
            uint16_t rl;
            std::memcpy(&rl, p + at, 2);
            if (rl < 5 || at + 2 + rl > n) break;
            const uint32_t k = u32_at(p + at + 3);
            if (!is_network_key(k)) g_known[k] = p[at + 2];
            at += 2 + static_cast<size_t>(rl);
        }
    }
    if (op == kOpActorLoadDataAck && n >= 0x40) {
        // One record: u16 len | u8 type | u32 key | u32 owner | ...
        uint16_t len;
        std::memcpy(&len, p, 2);
        const uint8_t type = p[2];
        const uint32_t key = u32_at(p + 3), owner = u32_at(p + 7);
        if (static_cast<size_t>(len) + 2 != n || (type != 3 && type != 6) || !keys::is_game_world_key(key) ||
            owner != 0) {
            return;
        }
        std::lock_guard lock(g_mutex);
        if (g_loads.size() > 1500) g_loads.clear(), g_actions.clear(), g_engaged.clear(), g_at_us.clear(), g_schedule_spawned.clear();
        g_loads[key] = Bytes(p, p + n);
    } else if (op == kOpRemoveActorAck && n >= 10) {
        // 04 | u32 | u8 reason | keys...
        std::lock_guard lock(g_mutex);
        for (size_t i = 6; i + 4 <= n; i += 4) {
            const uint32_t key = u32_at(p + i);
            g_loads.erase(key);
            g_actions.erase(key);
            g_engaged.erase(key);
            g_at_us.erase(key);
            g_schedule_spawned.erase(key);
            g_pose_of.erase(key);
            g_last_hp.erase(key);
            g_known.erase(key);
            g_refused.erase(key);
            remove_locked(key);
        }
    }
}

void enemy_observe_c2s(const uint8_t* packet, size_t size) {
    if (size < 5 + 8) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    if (op == kOpChangeBattleStateReq && size == 5 + 5) {
        const uint32_t actor = u32_at(packet + 5);
        std::lock_guard lock(g_mutex);
        if (packet[9] != 0 && g_loads.count(actor)) g_engaged[actor] = GetTickCount64();
        return;
    }
    if (op == kOpCurrentTargetChangeReq) {
        // An NPC our client runs picked us or the other player's copy.
        const uint32_t actor = u32_at(packet + 5), target = u32_at(packet + 9), own = capture_own_player_key();
        if ((own && target == own) || target == kPuppetPlayer) {
            std::lock_guard lock(g_mutex);
            if (g_loads.count(actor)) {
                g_at_us[actor] = GetTickCount64();
                learn_faction_locked(actor);
            }
        }
        return;
    }
    if (op == kOpDoFrameEventReq && size >= 5 + 0x203 + 665 && packet[5] == 0x04) {
        const uint32_t attacker = u32_at(packet + 6), own = capture_own_player_key();
        const uint8_t* p = packet + 5;
        const size_t n = size - 5;
        bool at_us = false;
        for (size_t k = 1; k <= 30 && n >= k * 665 + 0x203; ++k) {
            const uint32_t t = u32_at(p + n - k * 665);
            if (!keys::is_actor_key(t)) break;
            at_us = at_us || (own && t == own) || t == kPuppetPlayer;
        }
        if (at_us) {
            std::lock_guard lock(g_mutex);
            if (g_loads.count(attacker)) {
                g_engaged[attacker] = g_at_us[attacker] = GetTickCount64();
                learn_faction_locked(attacker);
            }
        }
        return;
    }
    const bool action = op == kOpStartActionReq && size == 5 + kStartActionReqSize;
    const bool rotate = op == kOpRotateActorReq && size == 5 + kRotateActorReqSize;
    if (!action && !rotate) return;
    const uint32_t key = u32_at(packet + 5);
    std::lock_guard lock(g_mutex);
    // Copied already: relay it as it happens.
    if (auto sent = g_sent.find(key); sent != g_sent.end()) {
        Bytes ev(2);
        std::memcpy(ev.data(), &op, 2);
        ev.insert(ev.end(), packet + 5, packet + size);
        Bytes body(ev.begin() + 2, ev.end());
        translate(body, key, sent->second);
        if (action) calm_pose_locked(body, key);
        if (map_message_locked(op, body, sent->second)) {
            std::copy(body.begin(), body.end(), ev.begin() + 2);
            if (g_out_events.size() < 512) g_out_events.push_back({proto::EventKind::EnemyAct, std::move(ev)});
        }
    }
    if (!action || !g_loads.count(key)) return;
    auto& list = g_actions[key];
    list.emplace_back(packet + 5, packet + size);
    while (list.size() > kKeptActions) list.pop_front();
}

// Test aid (copias-grupo-invisibles.md §6.8): every 2 s, our real type-3
// actors within 60 m whose Status+0x390 has bit 2 (the flag that hid the
// originals: model alpha 0) are logged when they get or lose it.
std::map<uint32_t, uint32_t> g_hidden_seen;  // key -> its +0x390 when logged

void watch_hidden() {
    const auto me = my_position();
    if (!me) return;
    std::vector<uint32_t> keys;
    std::set<uint32_t> with_copy;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [key, load] : g_loads) {
            if (load.size() > 3 && load[2] == 3) keys.push_back(key);
        }
        for (const auto& [key, copy] : g_sent) with_copy.insert(key);
    }
    int hidden_now = 0;
    for (uint32_t key : keys) {
        const auto a = game::find_actor(key);
        const auto pos = game::actor_world_position(key);
        if (!a || !pos || distance(*pos, *me) > 60.0f) continue;
        Seen f;
        if (!read_seen(*a, f)) continue;
        const bool hidden = (f.s390 & 0x2) != 0;
        hidden_now += hidden;
        auto it = g_hidden_seen.find(key);
        const bool was = it != g_hidden_seen.end() && (it->second & 0x2);
        if (hidden != was) {
            log::info("Enemy relay: REAL enemy 0x{:08X} {} the hidden flag (Status+0x390 0x{:X}, +0x394 0x{:X}, "
                      "state +5E 0x{:X}){}",
                      key, hidden ? "GOT" : "lost", f.s390, f.s394, f.state,
                      with_copy.count(key) ? ", it has a copy" : "");
        }
        g_hidden_seen[key] = f.s390;
    }
    static int last_count = -1;
    if (hidden_now != last_count) {
        log::info("Enemy relay: real enemies within 60 m with the hidden flag: {}", hidden_now);
        last_count = hidden_now;
    }
}

void enemy_set_auto(bool on) { g_auto = on; }
void enemy_set_linked(bool on) { g_linked = on; }

namespace {
bool zone_capable() { return g_relay_running.load() && g_auto.load(); }

proto::EnemyZoneMsg zone_answer_locked(bool capable) {
    proto::EnemyZoneMsg a;
    const PartnerZone& z = g_partner_zone;
    a.flags = static_cast<uint8_t>(proto::EnemyZoneMsg::kAnswer | (z.on && capable ? proto::EnemyZoneMsg::kOn : 0) |
                                   (z.dry ? proto::EnemyZoneMsg::kDryRun : 0) |
                                   (z.caution ? proto::EnemyZoneMsg::kCaution : 0));
    a.seq = z.seq;
    // What we relay: the guest's radius, or less when more than kZoneMax are in it.
    a.radius_m = std::clamp(std::min(z.radius, g_zone_radius), proto::kMinZoneRadius, proto::kMaxZoneRadius);
    g_answered_radius = a.radius_m;
    a.factions = g_learnt_factions;
    return a;
}
}  // namespace

proto::EnemyZoneMsg enemy_on_zone_request(const proto::EnemyZoneMsg& req) {
    const bool capable = zone_capable();
    bool was_relaying = false, relays = false;
    proto::EnemyZoneMsg answer;
    // The radius we relay, already in this first answer: the guest sweeps as
    // soon as it has it (a smaller one later left hidden natives nobody copied).
    std::optional<float> computed;
    if (req.on() && !req.dry_run()) {
        if (const auto me = my_position()) {
            const PartnerZone asked{true, false, req.caution(), req.radius_m, req.seq};
            computed = zone_radius_for(zone_candidates(asked, *me, puppet_position(), true), req.radius_m);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        const PartnerZone before = g_partner_zone;
        was_relaying = before.on && !before.dry;
        g_partner_zone = {req.on(), req.dry_run(), req.caution(), req.radius_m, req.seq};
        // Without our position (loading) a zone already on keeps its smaller radius.
        const float radius = computed ? *computed : before.on ? std::min(g_zone_radius, req.radius_m) : req.radius_m;
        if (std::fabs(radius - g_zone_radius) >= 0.5f) {
            g_zone_radius = radius;
            g_zone_radius_ms = GetTickCount64();
        }
        relays = req.on() && !req.dry_run();
        answer = zone_answer_locked(capable);
        if (before.on != req.on() || before.dry != req.dry_run()) g_dry_zone_logged.clear();
    }
    g_answered_capable = capable;
    g_answered_learnt = answer.factions.size();
    g_answered_ms = GetTickCount64();
    log::info("Enemy relay: the guest's enemy zone {} (request {}, {:.0f} m{}{}): {}", req.on() ? "ON" : "off",
              req.seq, req.radius_m, req.dry_run() ? ", dry run" : "", req.caution() ? ", node events" : "",
              !req.on()      ? "back to copying only the enemies that fight"
              : !capable     ? "we do NOT relay (automatic copies off: Ctrl+F7, [coop] enemy_auto=1)"
              : relays       ? "copying every zone enemy"
                             : "only logging what we would copy");
    // Leaving the zone: its copies go (the guest removed them already).
    if (was_relaying && !relays) remove_sent();
    return answer;
}

std::optional<proto::EnemyZoneMsg> enemy_zone_update() {
    const bool capable = zone_capable();
    std::lock_guard lock(g_mutex);
    if (!g_partner_zone.on) return std::nullopt;
    const uint64_t now = GetTickCount64();
    const bool learnt_more = g_learnt_factions.size() != g_answered_learnt && now - g_answered_ms >= 5000;
    const float radius = std::clamp(std::min(g_partner_zone.radius, g_zone_radius), proto::kMinZoneRadius,
                                    proto::kMaxZoneRadius);
    const bool radius_changed = std::fabs(radius - g_answered_radius) >= 1.0f;
    if (capable == g_answered_capable && !learnt_more && !radius_changed) return std::nullopt;
    g_answered_capable = capable;
    g_answered_learnt = g_learnt_factions.size();
    g_answered_ms = now;
    return zone_answer_locked(capable);
}

void enemy_test_loop(void* stop) {
    g_relay_running = true;
    log::info("Enemy relay: Ctrl+F6 = copy the enemy you hit / the nearest, Ctrl+Shift+F6 = remove the copies, "
              "Ctrl+F7 = automatic (every enemy fighting you): {}",
              g_auto ? "ON from the start ([coop] enemy_auto=1)" : "off");
    bool was6 = false, was7 = false;
    uint64_t last_auto = 0, last_watch = 0;
    while (WaitForSingleObject(static_cast<HANDLE>(stop), 50) == WAIT_TIMEOUT) {
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool f6 = ctrl && (GetAsyncKeyState(VK_F6) & 0x8000);
        const bool f7 = ctrl && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (f6 && !was6) {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) remove_sent();
            else send_picked();
        }
        if (f7 && !was7) {
            g_auto = !g_auto.load();
            log::info("Enemy relay: automatic copies {}", g_auto ? "ON" : "OFF");
            if (!g_auto) remove_sent();
        }
        was6 = f6;
        was7 = f7;
        const uint64_t now = GetTickCount64();
        if (g_auto && g_linked && now - last_auto >= 500) {
            last_auto = now;
            auto_tick();
        }
        if (now - last_watch >= 2000) {
            last_watch = now;
            watch_hidden();
        }
    }
}

std::optional<std::pair<proto::RelKind, std::vector<uint8_t>>> enemy_take_outgoing() {
    std::lock_guard lock(g_mutex);
    if (g_outgoing.empty()) return std::nullopt;
    auto m = std::move(g_outgoing.front());
    g_outgoing.pop_front();
    return m;
}

std::vector<proto::GameEvent> enemy_host_tick() {
    std::vector<proto::GameEvent> out;
    std::map<uint32_t, uint32_t> sent;
    {
        std::lock_guard lock(g_mutex);
        while (!g_out_events.empty()) {
            out.push_back(std::move(g_out_events.front()));
            g_out_events.pop_front();
        }
        sent = g_sent;
    }
    const uint64_t now = GetTickCount64();
    if (sent.empty() || now - g_last_sample_ms < kPosIntervalMs) return out;
    g_last_sample_ms = now;
    const uint64_t ts = capture_game_clock_ms();
    if (!ts) return out;
    for (const auto& [key, copy] : sent) {
        proto::Vec3 at;
        proto::Quat turn;
        if (!game::actor_world_transform(key, at, turn)) continue;
        const proto::Vec3* pos = &at;
        const proto::Quat* rot = &turn;
        Sampled& last = g_sampled[key];
        const float dx = pos->x - last.pos.x, dy = pos->y - last.pos.y, dz = pos->z - last.pos.z;
        // A finisher made the copy vanish for a moment (live test): the
        // enemy is attached to the player then and its transform seems to
        // turn relative to him. Skip impossible jumps (the copy holds still)
        // and log the raw numbers to confirm.
        const float dt = std::max<float>(static_cast<float>(now - last.ms), 66.0f) / 1000.0f;
        if (last.has && dx * dx + dy * dy + dz * dz > (kMaxSpeed * dt) * (kMaxSpeed * dt) + 4.0f) {
            if (last.jumps_logged++ < 12) {
                const auto me = my_position();
                log::info("Enemy relay: enemy 0x{:08X} read at ({:.1f}, {:.1f}, {:.1f}), {:.1f} m from its last "
                          "sample in {:.2f} s; skipped | you at ({:.1f}, {:.1f}, {:.1f}) | its last action's "
                          "parent 0x{:08X}",
                          key, pos->x, pos->y, pos->z, std::sqrt(dx * dx + dy * dy + dz * dz), dt,
                          me ? me->x : NAN, me ? me->y : NAN, me ? me->z : NAN, last_action_parent(key));
            }
            continue;
        }
        const bool moved = dx * dx + dy * dy + dz * dz > 0.05f * 0.05f;
        if (!moved && now - last.ms < kPosKeepAliveMs) continue;
        proto::Vec3 p = *pos;
        if (!moved) {
            // Standing still: the remote buffer still wants a sample every
            // second, and an identical one is ignored. 1 mm up or down.
            last.flip = !last.flip;
            p.y += last.flip ? 0.001f : -0.001f;
        } else {
            last.pos = *pos;
        }
        last.has = true;
        last.ms = now;
        Bytes ev(proto::kEnemyPosEventSize);
        const float f[7] = {p.x, p.y, p.z, rot->x, rot->y, rot->z, rot->w};
        std::memcpy(ev.data(), &copy, 4);
        std::memcpy(ev.data() + 4, &ts, 8);
        std::memcpy(ev.data() + 12, f, sizeof f);
        out.push_back({proto::EventKind::EnemyPos, std::move(ev)});
    }
    return out;
}

// ---- receiving side: the other player's mod may be modified, so nothing it
// sends is trusted (security review, docs/research/seguridad/seguridad-combate.md
// H2, H3, H7-H11). Network thread only.
namespace {

constexpr size_t kMaxLoadSize = 16 * 1024;     // enemy loads are 6.6-11 KB (enemigos-del-anfitrion.md §0)
constexpr size_t kMaxCopiesHere = 64;          // loaded + waiting; the sender's zone keeps up to 48
constexpr float kMaxLoadDistance = 300.0f;     // a copy farther than this from us waits (not loaded yet)
constexpr uint64_t kCopyQuarantineMs = 10000;  // as the sender's kNetIdQuarantineMs
constexpr float kMaxWorldXZ = 1e6f, kMaxWorldY = 1e5f;

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
// Loads of copies: the sender's automatic mode sends up to 8 at once.
RateLimit g_load_rate{8.0, 2.0};

// Log lines the other side can trigger at will: the first 20, then 1 in 100.
bool log_turn(uint64_t& n) { return n++ < 20 || n % 100 == 0; }
uint64_t g_first_action_logged = 0, g_refused_logged = 0, g_odd_control_logged = 0, g_bad_pos_logged = 0, g_removed_logged = 0,
         g_died_logged = 0;

// A copy waiting for its turn to be loaded (enemy_tick), with the state
// messages about it that came meanwhile (its HP is sent right behind it).
struct WaitingCopy {
    uint32_t copy = 0;
    Bytes spawn;  // the EnemySpawn message
    Bytes hp, dead;
    uint64_t state_delay_ms = 0;
    std::optional<proto::Vec3> pos;  // its latest EnemyPos while waiting (before: its load's place)
};
std::deque<WaitingCopy> g_waiting;
std::map<uint32_t, uint64_t> g_removed_at;  // copy key -> when its copy here was removed

bool sane_point(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) <= kMaxWorldXZ &&
           std::fabs(z) <= kMaxWorldXZ && std::fabs(y) <= kMaxWorldY;
}
// A rotation: a quaternion of length 0.9-1.1.
bool sane_rotation(float x, float y, float z, float w) {
    const float n2 = x * x + y * y + z * z + w * w;
    return std::isfinite(n2) && n2 >= 0.81f && n2 <= 1.21f;
}

// The load's last Transform, where puppet_spawn_enemy places the copy: scale
// (1,1,1) then an all-FF parent; position 28 B and rotation 16 B before it.
bool load_transform_ok(const Bytes& load) {
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = load.size() - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(load.data() + i, kMarker, sizeof kMarker) != 0) continue;
        float f[7];
        std::memcpy(f, load.data() + i - 28, sizeof f);
        return sane_point(f[0], f[1], f[2]) && sane_rotation(f[3], f[4], f[5], f[6]);
    }
    return true;  // none: the copy is placed by its first position sample
}

// The key fields of a message about `copy` may name only us, the other
// player's copy, this copy or another copy loaded here; anything else (an
// actor of ours, a client-local key) becomes "none" (H10).
bool allowed_key(uint32_t v, uint32_t copy, uint32_t own) {
    return v == 0 || v == 0xFFFFFFFFu || v == copy || v == kPuppetPlayer || (own && v == own) || g_copies.count(v);
}
void scrub_field(Bytes& v, size_t off, uint32_t copy, uint32_t own) {
    if (off + 4 > v.size()) return;
    const uint32_t k = u32_at(v.data() + off);
    if ((k & 0x80000000u) && !allowed_key(k, copy, own)) std::memset(v.data() + off, 0, 4);
}
// Fields that hold a key only now and then: other values stay.
void scrub_key_shaped(Bytes& v, size_t off, uint32_t copy, uint32_t own) {
    if (off + 4 > v.size()) return;
    const uint32_t k = u32_at(v.data() + off);
    if (keys::is_actor_key(k) && !allowed_key(k, copy, own)) std::memset(v.data() + off, 0, 4);
}
// An enemy copy hangs from nothing (riders are not copied): no parent, not
// even our copy of the other player's horse.
void no_parent(Bytes& v, size_t off) {
    if (off + 8 <= v.size()) std::memset(v.data() + off, 0xFF, 8);
}
// Same fields as map_message_locked on the sending side.
void scrub_message(uint16_t op, Bytes& v, uint32_t copy, uint32_t own) {
    switch (op) {
        case kOpStartActionReq:
            no_parent(v, 0x116);
            scrub_field(v, 0x16C, copy, own);
            scrub_key_shaped(v, 0xAA, copy, own);
            break;
        case kOpRotateActorReq: no_parent(v, 0x54); break;
        case kOpVaryHPAck: scrub_field(v, 0x54, copy, own); break;
        case kOpNotifyHandleDeadAck: scrub_field(v, 0x04, copy, own); break;
        case kOpAttackTargetAck:
            // +0xBC/+0xCE hold the attacker in most blows, other data in some.
            scrub_field(v, 0x04, copy, own);
            scrub_key_shaped(v, 0xBC, copy, own), scrub_key_shaped(v, 0xCE, copy, own);
            break;
        case kOpApplyActionRestrictionAck: scrub_field(v, 0x49, copy, own); break;
        default: break;
    }
}

// Our own barrier against a sender without the sanitiser (H2, H3, H9): the
// load must be ONE solo enemy record (type 3, key = the copy, owner 0) that
// hangs from nothing and names no actor of ours, not even our player; its
// actions (at most kKeptActions) must all be of the copy itself. Fills `load`
// and `action`; false with `why` if it must not go in.
bool check_spawn(uint32_t copy, const Bytes& spawn, Bytes& load, Bytes& action, std::string& why) {
    const uint32_t own = capture_own_player_key(), own_user = capture_own_user_key();
    if (!own) {
        why = "our own player is not known yet";
        return false;
    }
    const uint32_t n = u32_at(spawn.data() + 4);
    load.assign(spawn.begin() + 8, spawn.begin() + 8 + n);
    action.assign(spawn.begin() + 8 + n, spawn.end());
    // u16 len | u8 type | u32 key | u32 owner | ... | +0x14 control user | +0x18 variant
    uint16_t len = 0;
    std::memcpy(&len, load.data(), 2);
    const uint32_t key = u32_at(load.data() + 3), owner = u32_at(load.data() + 7);
    if (static_cast<size_t>(len) + 2 != load.size() || load[2] != 3 || key != copy || owner != 0 ||
        u32_at(load.data() + 0x18) == 0) {
        why = std::format("not one enemy record of the copy (length {}+2 of {}, type {}, key 0x{:08X}, owner 0x{:08X})",
                          len, load.size(), load[2], key, owner);
        return false;
    }
    // The control user (+0x14 = actor+0x90) is the sender's copy user, so the
    // copy is driven by the network (control mode 2). Never our own user: our
    // client would run its AI and report it to our server under that key.
    if (const uint32_t control = u32_at(load.data() + 0x14); control != kPuppetUser) {
        if (log_turn(g_odd_control_logged)) {
            log::info("Enemy relay: copy 0x{:08X} came with control user 0x{:08X}; set to 0x{:08X}", copy, control,
                      kPuppetUser);
        }
        std::memcpy(load.data() + 0x14, &kPuppetUser, 4);
    }
    const auto tail = parse_tail(load);
    if (!tail || tail->parent || !tail->children.empty() || !tail->hierarchy.empty()) {
        why = "it has a parent or children";
        return false;
    }
    if (const auto party = find_party(load, copy)) {
        why = std::format("it is in party 0x{:X} ({} members)", party->number, party->members);
        return false;
    }
    for (size_t i = 5; i + 4 <= load.size(); ++i) {
        const uint32_t k = u32_at(load.data() + i);
        if (!keys::is_actor_key(k) || k == copy || is_network_key(k)) continue;
        if (k == own || (own_user && k == own_user) || game::find_actor(k)) {
            why = std::format("it names our actor 0x{:08X} at +{}", k, i);
            return false;
        }
    }
    if (!load_transform_ok(load)) {
        why = "its position or rotation is not a place in the world";
        return false;
    }
    // The sender's kSelfMarker (its copy of us, e.g. in an aggro entry) means
    // our own player, as before the review: only it may name us, and only
    // after the barrier above refused our key written in directly.
    replace_all(load, proto::kSelfMarker, own);
    if (action.size() % kStartActionReqSize != 0 || action.size() > kKeptActions * kStartActionReqSize) {
        why = std::format("{} bytes of actions", action.size());
        return false;
    }
    for (size_t at = 0; at < action.size(); at += kStartActionReqSize) {
        Bytes one(action.begin() + static_cast<ptrdiff_t>(at),
                  action.begin() + static_cast<ptrdiff_t>(at + kStartActionReqSize));
        if (u32_at(one.data()) != copy) {
            why = std::format("its action {} is of actor 0x{:08X}", at / kStartActionReqSize, u32_at(one.data()));
            return false;
        }
        replace_all(one, proto::kSelfMarker, own);  // its target may be us
        scrub_message(kOpStartActionReq, one, copy, own);
        for (size_t i = 4; i + 4 <= one.size(); ++i) {
            const uint32_t k = u32_at(one.data() + i);
            if (!keys::is_actor_key(k) || k == copy || is_network_key(k)) continue;
            if (k == own && (i == 0x16C || i == 0xAA)) continue;
            if (k == own || (own_user && k == own_user) || game::find_actor(k)) {
                why = std::format("its action {} names our actor 0x{:08X} at +{}", at / kStartActionReqSize, k, i);
                return false;
            }
        }
        std::copy(one.begin(), one.end(), action.begin() + static_cast<ptrdiff_t>(at));
    }
    return true;
}

void load_copy(WaitingCopy& w) {
    Bytes load, action;
    std::string why;
    if (!check_spawn(w.copy, w.spawn, load, action, why)) {
        if (log_turn(g_refused_logged)) log::warn("Enemy relay: copy 0x{:08X} refused: {}", w.copy, why);
        return;
    }
    const uint32_t variant = load.size() > 0x1C && load[2] == 3 ? u32_at(load.data() + 0x18) : 0;
    if (!puppet_spawn_enemy(w.copy, std::move(load), std::move(action))) return;
    g_copies[w.copy] = true;
    g_copy_variant[w.copy] = variant;
    g_report_due[w.copy] = now_ms() + 2000;
    if (!w.hp.empty()) enemy_on_state(w.hp, w.state_delay_ms);
    if (!w.dead.empty()) enemy_on_state(w.dead, w.state_delay_ms);
}

// Loads the copies whose turn came: we are in the world, the key's previous
// copy went 10 s ago or more (its removal may still be on its way, as the
// sender also assumes), and at most 8 at once then 2 a second, so a flood
// cannot load hundreds of actors in one go (H8).
// The actions an EnemySpawn carries behind its load (0 if malformed).
size_t spawn_actions(const Bytes& spawn) {
    if (spawn.size() < 8) return 0;
    const size_t n = u32_at(spawn.data() + 4);
    return n <= spawn.size() - 8 ? (spawn.size() - 8 - n) / kStartActionReqSize : 0;
}

// Where an EnemySpawn's load puts its copy (its last Transform), if it has one.
std::optional<proto::Vec3> spawn_place(const Bytes& spawn) {
    static const uint8_t kMarker[20] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
                                        0x80, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (spawn.size() < 8 + 64) return std::nullopt;
    const size_t n = u32_at(spawn.data() + 4);
    if (n < 64 || n > spawn.size() - 8) return std::nullopt;
    const uint8_t* load = spawn.data() + 8;
    for (size_t i = n - sizeof kMarker; i > 28; --i) {
        if (std::memcmp(load + i, kMarker, sizeof kMarker) != 0) continue;
        float f[3];
        std::memcpy(f, load + i - 28, sizeof f);
        if (!sane_point(f[0], f[1], f[2])) return std::nullopt;
        return proto::Vec3{f[0], f[1], f[2]};
    }
    return std::nullopt;
}

// Not before this client's game clock is trusted (the copy's first actions
// are stamped with it), not a copy far from us (one sent while we were
// loading or travelling, 7 Oct: 18 loaded 1.2 km away), and never one with
// no action: an actor whose action layer has nothing playing crashes the
// game (the horse, caballo-coop.md §9; Shadow crashed 2.3 s after 16 enemy
// copies came with "0 recent actions", 7 Oct 17:46:35). It waits for its
// first relayed StartAction (enemy_on_event).
void load_waiting() {
    if (g_waiting.empty() || !capture_game_clock_decided() || !capture_game_clock_ms()) return;
    const auto me = my_position();
    if (!me) return;
    // Loading (the tunnel walk into the world): our actor is parked at
    // (0, 1000, 0) (both of Shadow's crashes, 7 Oct).
    if (std::fabs(me->x) < 50.0f && std::fabs(me->z) < 50.0f && std::fabs(me->y - 1000.0f) < 50.0f) return;
    const uint64_t now = now_ms();
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        if (const auto at = it->pos ? it->pos : spawn_place(it->spawn);
            at && enemy_load::flat_distance(*at, *me) > kMaxLoadDistance) {
            ++it;
            continue;
        }
        if (!spawn_actions(it->spawn)) {
            ++it;
            continue;
        }
        if (auto r = g_removed_at.find(it->copy); r != g_removed_at.end()) {
            if (now - r->second < kCopyQuarantineMs) {
                ++it;
                continue;
            }
            g_removed_at.erase(r);
        }
        if (!g_load_rate.take(now)) return;
        WaitingCopy w = std::move(*it);
        it = g_waiting.erase(it);
        load_copy(w);
    }
}

void note_removed(uint32_t copy) {
    const uint64_t now = now_ms();
    if (g_removed_at.size() > 512) {
        std::erase_if(g_removed_at, [&](const auto& r) { return now - r.second >= kCopyQuarantineMs; });
    }
    g_removed_at[copy] = now;
}

}  // namespace

bool enemy_copy_loaded(uint32_t copy) { return g_copies.count(copy) != 0; }

void enemy_on_event(const proto::GameEvent& ev, int64_t ts_shift_ms, uint64_t delay_ms) {
    if (ev.payload.size() < 4) return;
    if (ev.kind == proto::EventKind::EnemyPos) {
        if (ev.payload.size() != proto::kEnemyPosEventSize) return;
        const uint32_t copy = u32_at(ev.payload.data());
        if (!g_copies.count(copy)) {
            // Waiting: where it is now decides when it may load.
            float p[3];
            std::memcpy(p, ev.payload.data() + 12, sizeof p);
            if (!sane_point(p[0], p[1], p[2])) return;
            for (WaitingCopy& w : g_waiting) {
                if (w.copy == copy) w.pos = proto::Vec3{p[0], p[1], p[2]};
            }
            return;
        }
        uint64_t ts = 0;
        float f[7];
        std::memcpy(&ts, ev.payload.data() + 4, 8);
        std::memcpy(f, ev.payload.data() + 12, sizeof f);
        // NaN, 1e30 or a non-rotation would reach the client's physics (H9).
        if (!sane_point(f[0], f[1], f[2]) || !sane_rotation(f[3], f[4], f[5], f[6])) {
            if (log_turn(g_bad_pos_logged)) log::warn("Enemy relay: impossible position for copy 0x{:08X} dropped", copy);
            return;
        }
        const float inv = 1.0f / std::sqrt(f[3] * f[3] + f[4] * f[4] + f[5] * f[5] + f[6] * f[6]);
        puppet_enemy_move(copy, ts, {f[0], f[1], f[2]}, {f[3] * inv, f[4] * inv, f[5] * inv, f[6] * inv},
                          ts_shift_ms, delay_ms);
    } else if (ev.kind == proto::EventKind::EnemyAct) {
        uint16_t op = 0;
        std::memcpy(&op, ev.payload.data(), 2);
        Bytes req(ev.payload.begin() + 2, ev.payload.end());
        if (!valid_size(op, req.size())) return;
        if (op == kOpStartActionReq && !g_copies.count(u32_at(req.data()))) {
            // A waiting copy that came with no action: this one goes behind its
            // load, as if sent with it (check_spawn vets it); then it may load.
            for (WaitingCopy& w : g_waiting) {
                if (w.copy != u32_at(req.data()) || spawn_actions(w.spawn)) continue;
                w.spawn.insert(w.spawn.end(), req.begin(), req.end());
                if (log_turn(g_first_action_logged)) {
                    log::info("Enemy relay: copy 0x{:08X} came with no action; loading it with its first one", w.copy);
                }
                load_waiting();
                break;
            }
            return;
        }
        if (!g_copies.count(u32_at(req.data())) || !untranslate(req)) return;
        scrub_message(op, req, u32_at(req.data()), capture_own_player_key());
        if (op == kOpAttackTargetAck || op == kOpApplyActionRestrictionAck) {
            puppet_inject_s2c(op, req, delay_ms);  // hit reaction, in step with its actions
        } else {
            puppet_enemy_action(op, req, ts_shift_ms, delay_ms);
        }
    }
}

void enemy_set_receive_enabled(bool on) { g_receive_enabled = on; }

namespace {
// Dead copies whose loot is still ours to take: removed when looted or after
// kLootKeepMs (the sender's removal waits; 7 Oct night: the host's respawn
// took 3 corpses before the guest could loot them).
constexpr uint64_t kLootKeepMs = 120000;
std::map<uint32_t, uint64_t> g_kept_corpses;  // copy -> when its removal came

void remove_copy_now(uint32_t copy) {
    g_kept_corpses.erase(copy);
    if (!g_copies.erase(copy)) return;
    g_copy_variant.erase(copy);
    loot_forget_copy(copy);
    g_report_due.erase(copy);
    note_removed(copy);
    puppet_remove_actor(copy);
    if (log_turn(g_removed_logged)) log::info("Enemy relay: enemy copy 0x{:08X} removed", copy);
}
}  // namespace

void enemy_on_spawn(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8) return;
    if (!g_receive_enabled) {
        static bool told = false;
        if (!told) log::warn("Enemy relay: the other player's enemy copies are refused (no crash guards)");
        told = true;
        return;
    }
    const uint32_t copy = u32_at(bytes.data());
    const uint32_t n = u32_at(bytes.data() + 4);
    if (g_kept_corpses.count(copy)) remove_copy_now(copy);  // its key is used again: the corpse goes
    // Only the net ids a sender hands out (H7), a load of a real size and at
    // most kKeptActions actions behind it.
    if (!is_valid_enemy_copy_key(copy) || n < 0x40 || n > kMaxLoadSize || 8 + static_cast<size_t>(n) > bytes.size() ||
        bytes.size() - 8 - n > kKeptActions * kStartActionReqSize) {
        if (log_turn(g_refused_logged)) {
            log::warn("Enemy relay: malformed enemy copy 0x{:08X} ({} bytes) ignored", copy, bytes.size());
        }
        return;
    }
    // A copy already here (or waiting) is not loaded again: a sender removes
    // it first (H8).
    const bool waiting =
        std::any_of(g_waiting.begin(), g_waiting.end(), [&](const WaitingCopy& w) { return w.copy == copy; });
    if (g_copies.count(copy) || waiting || g_copies.size() + g_waiting.size() >= kMaxCopiesHere) {
        if (log_turn(g_refused_logged)) {
            log::warn("Enemy relay: copy 0x{:08X} refused: {}", copy,
                      (waiting || g_copies.count(copy)) ? "it is already here"
                                                      : std::format("{} copies here already", kMaxCopiesHere));
        }
        return;
    }
    g_waiting.push_back({copy, bytes, {}, {}, 0});
    load_waiting();
}

void enemy_kept_corpses_tick();

void enemy_tick() {
    load_waiting();
    enemy_kept_corpses_tick();
    const uint64_t t = now_ms();
    for (auto it = g_report_due.begin(); it != g_report_due.end();) {
        if (t >= it->second) {
            report(it->first);
            it = g_report_due.erase(it);
        } else {
            ++it;
        }
    }
}

void enemy_on_state(const std::vector<uint8_t>& bytes, uint64_t delay_ms) {
    if (bytes.size() < 6) return;
    uint16_t op = 0;
    std::memcpy(&op, bytes.data(), 2);
    if (op != kOpVaryHPAck && op != kOpNotifyHandleDeadAck) return;
    Bytes payload(bytes.begin() + 2, bytes.end());
    if (!valid_size(op, payload.size())) return;
    const uint32_t copy = u32_at(payload.data());
    if (!g_copies.count(copy)) {
        // Still waiting for its turn: applied once it is loaded.
        for (WaitingCopy& w : g_waiting) {
            if (w.copy != copy) continue;
            (op == kOpVaryHPAck ? w.hp : w.dead) = bytes;
            w.state_delay_ms = delay_ms;
        }
        return;
    }
    if (!untranslate(payload)) return;
    scrub_message(op, payload, copy, capture_own_player_key());
    puppet_inject_s2c(op, payload, delay_ms);  // in step with its actions
    if (op == kOpNotifyHandleDeadAck) {
        // Its loot is the other player's, unless we helped kill it: then our
        // server's twin of it gives us ours (loot_twin.h).
        const auto v = g_copy_variant.find(copy);
        const bool twin = loot_on_copy_died(copy, v != g_copy_variant.end() ? v->second : 0, delay_ms);
        if (const uint32_t own = capture_own_player_key(); own && !twin) {
            Bytes empty(5);
            empty[0] = 4;
            std::memcpy(empty.data() + 1, &copy, 4);
            Bytes gone(8);
            std::memcpy(gone.data(), &copy, 4);
            std::memcpy(gone.data() + 4, &own, 4);
            puppet_inject_s2c(kOpLootDropCharacterRewardListAck, empty, delay_ms + kNoLootAfterMs);
            puppet_inject_s2c(kOpRemoveLootableAck, gone, delay_ms + kNoLootAfterMs);
        }
        if (log_turn(g_died_logged)) {
            log::info("Enemy relay: enemy copy 0x{:08X} died ({})", copy, twin ? "loot from its twin" : "no loot here");
        }
    }
}

void enemy_reset_partner() {
    loot_reset();
    size_t sent = 0, copies = g_copies.size();
    {
        std::lock_guard lock(g_mutex);
        sent = g_sent.size();
        for (const auto& [key, copy] : g_sent) g_retired[copy - kEnemyCopyBase] = GetTickCount64();
        g_sent.clear();
        g_outgoing.clear();
        g_out_events.clear();
        g_partner_zone = {};  // a new guest asks again
        g_dry_zone_logged.clear();
        ++g_partner_epoch;  // what a tick decided before this is not sent
        g_zone_radius = g_answered_radius = 150.0f;
    }
    g_answered_capable = false;
    g_answered_learnt = 0;
    g_sampled.clear();
    for (const auto& [copy, loaded] : g_copies) {
        puppet_remove_actor(copy);
        note_removed(copy);
    }
    g_copies.clear();
    g_kept_corpses.clear();
    g_copy_variant.clear();
    g_waiting.clear();
    g_report_due.clear();
    if (sent || copies) {
        log::info("Enemy relay: other side changed: {} enemies to copy again, {} copies of theirs removed", sent,
                  copies);
    }
}

void enemy_drop_copies_here() {
    const size_t n = g_copies.size() + g_waiting.size();
    std::vector<uint32_t> copies;
    for (const auto& [copy, loaded] : g_copies) copies.push_back(copy);
    size_t kept = 0;
    for (uint32_t copy : copies) {
        // A corpse with our loot still to take stays a while (as on a removal).
        if (!g_kept_corpses.count(copy) && loot_keeps_copy(copy) && g_kept_corpses.size() < 32) {
            g_kept_corpses[copy] = now_ms();
        }
        if (g_kept_corpses.count(copy)) {
            ++kept;
            continue;
        }
        remove_copy_now(copy);
    }
    g_waiting.clear();
    if (n) {
        log::info("Enemy relay: the {} copies of the other player's enemies here removed (zone off){}", n - kept,
                  kept ? std::format("; {} corpses with our loot kept a while", kept) : std::string());
    }
}


void enemy_on_remove(const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const uint32_t copy = u32_at(bytes.data() + i);
        std::erase_if(g_waiting, [&](const WaitingCopy& w) { return w.copy == copy; });
        if (!g_copies.count(copy) || g_kept_corpses.count(copy)) continue;
        if (loot_keeps_copy(copy) && g_kept_corpses.size() < 32) {
            g_kept_corpses[copy] = now_ms();
            log::info("Enemy relay: enemy copy 0x{:08X} kept a while: its loot is still ours to take", copy);
            continue;
        }
        remove_copy_now(copy);
    }
}

void enemy_kept_corpses_tick() {
    const uint64_t now = now_ms();
    std::vector<uint32_t> done;
    for (const auto& [copy, since] : g_kept_corpses) {
        if (now - since >= kLootKeepMs || !loot_keeps_copy(copy)) done.push_back(copy);
    }
    for (uint32_t copy : done) remove_copy_now(copy);
}

}  // namespace cdmp::net
