#include "mod/net/native_suppress.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/enemy_load.h"
#include "common/enemy_zone.h"
#include "common/keys.h"
#include "mod/game/actor_lookup.h"
#include "mod/game/safe_memory.h"
#include "mod/log.h"
#include "mod/net/enemy_relay.h"
#include "mod/net/local_capture.h"
#include "mod/net/remote_puppet.h"

namespace cdmp::net {
namespace {

using Bytes = std::vector<uint8_t>;
using enemy_load::Verdict;

constexpr uint16_t kOpActorLoadDataAck = 2005;
constexpr uint16_t kOpRemoveActorAck = 2516;  // 04 | u32 0 | u8 reason | n x u32 key
constexpr uint16_t kOpNotifyHandleDeadAck = 2530;  // 19 B: dead | killer | reason
constexpr uint16_t kOpAttackTargetAck = 2922;      // u32 target | u32 attacker | ...
constexpr uint16_t kOpUpdateFactionRelationDataAck = 2597;
// The server's messages about a held native, given back behind its load
// (fase5 §5.5; key at +0, apariencia-estado-coop.md §3): the latest of each,
// buffs in order.
constexpr uint16_t kOpVaryHPAck = 2147;               // 92 B
constexpr uint16_t kOpApplyBuffAck = 2506;            // 77 B
constexpr uint16_t kOpUnapplyBuffAck = 3007;          // 77 B
constexpr uint16_t kOpChangeMoveSessionIDAck = 3097;  // 16 B
constexpr uint16_t kOpUpdateElementalAck = 2642;      // 118 B
constexpr size_t kMaxStatePerKey = 32;
// Client->server: a native of ours fights us.
constexpr uint16_t kOpCurrentTargetChangeReq = 2984;  // u32 actor | u32 target | ...
constexpr uint16_t kOpDoFrameEventReq = 2380;         // 04 | u32 attacker | ... | 665-byte target entries
constexpr uint16_t kOpChangeBattleStateReq = 2257;    // u32 actor | u8 state
// 04 | u32 NPC | u16: the NPC is about to get its weapon (a type-7 gimmick
// whose load names it; pendiente-3g §1.5).
constexpr uint16_t kOpOnDemandEquipDockingGimmickSpawnReq = 2249;

constexpr uint8_t kReasonFar = 25;          // the server's own "far away"
constexpr uint8_t kReasonLoadScreen = 19;   // everything goes
constexpr uint8_t kReasonWeaponGone = 45;   // an NPC's weapon gimmick, after the NPC
constexpr size_t kMaxLoad = 16 * 1024;      // NPC loads are 6.6-11 KB
constexpr size_t kMaxHeld = 512;            // ~4 MB of loads at most
constexpr size_t kMaxNatives = 512;
constexpr size_t kMaxSmallSet = 1024;
constexpr uint64_t kFightMs = 10000;        // a native that fought us this recently stays
constexpr uint64_t kArmingMs = 10000;       // a weapon load this soon after its request
constexpr uint64_t kSweepEveryMs = 1000;
constexpr uint64_t kStatsEveryMs = 10000;
constexpr uint64_t kReportAfterMs = 2000, kLoginCheckAfterMs = 5000;

uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t now_ms() { return GetTickCount64(); }

// No C++ objects here: SEH and destructors do not mix.
bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- state shared by the three threads, under g_lock. The game's network
// thread only ever tries for it (a few spins) and delivers when it cannot.
SRWLOCK g_lock = SRWLOCK_INIT;
struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
struct TryLock {
    bool ok = false;
    TryLock() {
        for (int i = 0; i < 64 && !(ok = TryAcquireSRWLockExclusive(&g_lock)); ++i) YieldProcessor();
    }
    ~TryLock() {
        if (ok) ReleaseSRWLockExclusive(&g_lock);
    }
    TryLock(const TryLock&) = delete;
    TryLock& operator=(const TryLock&) = delete;
};

// A native held out of the client: its load as the server sent it, and what
// the server said about it since.
struct Held {
    uint64_t seq = 0;  // arrival order (a leader before its members)
    Bytes load;
    uint32_t parent = 0;
    bool swept = false;  // was in the client: given back where it last stood
    std::optional<proto::Vec3> pos;
    std::optional<proto::Quat> rot;
    std::deque<std::pair<uint16_t, Bytes>> state;
    uint64_t since_ms = 0;
};
std::map<uint32_t, Held> g_held;
// Loads of the natives in the client that may be swept: zone enemies by the
// predicate (except where they stand) and every camp member (whatever its
// own verdict: it goes with its leader). enemy_relay's g_loads is wiped at
// 1,500 and cannot be used to give anything back.
struct Native {
    uint64_t seq = 0;
    Bytes load;
    uint32_t parent = 0;
    Verdict verdict = Verdict::No;
    std::vector<uint32_t> children;
};
std::map<uint32_t, Native> g_natives;
std::map<uint32_t, uint64_t> g_fought;         // native -> last time it fought us
std::map<uint32_t, uint64_t> g_arming;         // native -> when it asked for a weapon
std::map<uint32_t, uint32_t> g_weapon_of;      // weapon gimmick -> native holding it
std::map<uint32_t, uint64_t> g_weapon_ms;      // weapon gimmick -> when it came
std::map<uint32_t, std::pair<uint8_t, uint64_t>> g_battle;  // native -> its last battle state, when
std::set<uint32_t> g_dead;                     // natives that died (corpses stay, to be looted)
std::set<uint32_t> g_dry_held;                 // dry run: what would be held (for camp members)
std::vector<uint32_t> g_learnt;                // the host's learnt hostile factions
std::deque<std::string> g_logs;                // lines from the game's thread, written by the co-op thread
uint64_t g_seq = 0;
SpawnBreaker g_breaker;
uint64_t g_drop_lines = 0;
struct Counters {
    uint64_t dropped = 0, would_drop = 0, swept = 0, would_sweep = 0, deferred = 0, restored = 0, restore_failed = 0,
             server_removed = 0, lock_misses = 0;
} g_count;
std::string g_hud;

// ---- read by the game's network thread without the lock
std::atomic<SuppressMode> g_mode{SuppressMode::Off};
std::atomic<bool> g_dropping{false};  // the zone is on: drop new zone loads
std::atomic<bool> g_dry{true};        // log only (also forced through cdmp_peer)
std::atomic<size_t> g_held_count{0};
std::atomic<float> g_center_x{0}, g_center_z{0}, g_radius{150};
// What the host relays (its answer's radius: less than ours when more enemies
// than it copies are in the zone): only those are hidden. 0 = ours.
std::atomic<float> g_host_radius{0};
float hide_radius() {
    const float host = g_host_radius.load(std::memory_order_relaxed);
    const float ours = g_radius.load(std::memory_order_relaxed);
    return host > 0 && host < ours ? host : ours;
}
std::atomic<bool> g_caution{false};
std::atomic<bool> g_breaker_tripped{false};
std::atomic<bool> g_loading_screen{false};
std::atomic<uint64_t> g_lock_misses{0};

// The server's removals, seen on the game's network thread, in a lock-free
// ring: slot = (u32 tag of its sequence + 1) << 32 | key. The co-op thread
// drains it; the network thread also checks it before giving a load back.
constexpr size_t kRemovedRing = 4096;
std::atomic<uint64_t> g_removed_slots[kRemovedRing];
std::atomic<uint64_t> g_removed_head{0};
uint64_t g_removed_tail = 0;  // co-op thread

void note_server_removed(uint32_t key) {
    const uint64_t s = g_removed_head.fetch_add(1, std::memory_order_relaxed);
    const uint64_t tag = static_cast<uint32_t>(s + 1);
    g_removed_slots[s % kRemovedRing].store(tag << 32 | key, std::memory_order_release);
}

bool removed_lately(uint32_t key) {
    for (const auto& slot : g_removed_slots) {
        const uint64_t v = slot.load(std::memory_order_acquire);
        if ((v >> 32) && static_cast<uint32_t>(v) == key) return true;
    }
    return false;
}

// Removed by the server since the ring's head was `from` (tag = sequence + 1).
bool removed_since(uint32_t key, uint64_t from) {
    for (const auto& slot : g_removed_slots) {
        const uint64_t v = slot.load(std::memory_order_acquire);
        if ((v >> 32) > static_cast<uint32_t>(from) && static_cast<uint32_t>(v) == key) return true;
    }
    return false;
}

// ---- settings (written before co-op starts)
SuppressSettings g_set;
EnemyZoneMachine g_zone;
bool g_lab_on = false, g_lab_key_was = false;
std::atomic<bool> g_broken{false};  // the breaker tripped: no hiding for the rest of the session
bool g_echo_told = false;

enemy_load::ZoneRules rules_locked() { return {g_caution.load(), g_learnt}; }

void queue_log(std::string line) {
    if (g_logs.size() < 256) g_logs.push_back(std::move(line));
}

const char* origin_text(enemy_load::OriginKind k) {
    switch (k) {
        case enemy_load::OriginKind::Schedule: return "S";
        case enemy_load::OriginKind::Sequence: return "Q";
        case enemy_load::OriginKind::FactionNode: return "F";
        case enemy_load::OriginKind::None: return "-";
        case enemy_load::OriginKind::Unknown: break;
    }
    return "?";
}

// ---- game network thread
void on_server_removal(const uint8_t* packet, uint16_t size) {
    const size_t whole = size - 5u;
    if (whole < 10) return;
    // A loading screen's removal can name more keys than fit here: its
    // reason is read first, then as many keys as fit.
    const size_t n = whole > 4096 ? 4096 : whole;
    uint8_t pl[4096];
    if (!guarded_copy(pl, packet + 5, n) || pl[0] != 0x04) return;
    if (pl[5] == kReasonLoadScreen) g_loading_screen = true;
    for (size_t i = 6; i + 4 <= n; i += 4) note_server_removed(u32_at(pl + i));
}

// Holds `load` (one record) if it is a zone native, or a member of a camp
// whose leader is held. True = do not deliver it.
bool decide(Bytes load, bool dry) {
    const auto head = enemy_load::read_head(load);
    if (!head) return false;
    const auto tail = enemy_load::parse_tail(load);
    const uint32_t parent = tail ? tail->parent : 0;
    TryLock lock;
    if (!lock.ok) {
        g_lock_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool member = parent && (dry ? g_dry_held.count(parent) != 0 : g_held.count(parent) != 0);
    float d = 0;
    if (!member) {
        if (!g_dropping || head->type != 3) return false;
        if (enemy_load::zone_check(load, rules_locked()).verdict != Verdict::Yes) return false;
        const auto t = enemy_load::last_transform(load);
        if (!t) return false;
        d = enemy_load::flat_distance(t->pos, {g_center_x.load(), 0, g_center_z.load()});
        if (d > hide_radius()) return false;
    }
    if (!dry && g_held.size() >= kMaxHeld && !member) return false;
    const uint64_t now = now_ms();
    if (!member && g_breaker.note(enemy_load::spawn_identity(load), now)) {
        // A spawner that keeps sending what it does not see: stop (co-op thread).
        g_breaker_tripped = true;
        g_dropping = false;
        return false;
    }
    if (g_drop_lines++ < 50 || g_drop_lines % 20 == 0) {
        const auto o = enemy_load::origin(load);
        queue_log(member ? std::format("Suppress: {} 0x{:08X} \"{}\", member of held 0x{:08X}",
                                       dry ? "WOULD DROP" : "dropped", head->key, enemy_load::name(load), parent)
                         : std::format("Suppress: {} load 0x{:08X} \"{}\" faction {} variant {} origin {} {:.0f} m "
                                       "from the host",
                                       dry ? "WOULD DROP" : "dropped", head->key, enemy_load::name(load),
                                       head->faction, head->variant, origin_text(o.kind), d));
    }
    if (dry) {
        if (g_dry_held.size() >= kMaxSmallSet) g_dry_held.clear();
        g_dry_held.insert(head->key);
        ++g_count.would_drop;
        return false;
    }
    Held h;
    h.seq = ++g_seq;
    h.parent = parent;
    h.since_ms = now;
    h.load = std::move(load);
    g_held[head->key] = std::move(h);
    g_natives.erase(head->key);
    g_held_count = g_held.size();
    ++g_count.dropped;
    return true;
}

// ---- co-op thread helpers
float distance_to_center(const proto::Vec3& p) {
    return enemy_load::flat_distance(p, {g_center_x.load(), 0, g_center_z.load()});
}

// +5E state and +CF login of an actor in the client (enemy_relay's report).
bool read_login(uint32_t key, uint16_t& state, uint8_t& login) {
    const auto a = game::find_actor(key);
    return a && game::safe_read(*a + 0x5E, &state, sizeof state) && game::safe_read(*a + 0xCF, &login, sizeof login);
}

// Every copy of the load's last position (and its rotation) becomes where the
// native stood when it was swept (the method of puppet_spawn_enemy).
void place(Bytes& load, const proto::Vec3& pos, const std::optional<proto::Quat>& rot) {
    const auto t = enemy_load::last_transform(load);
    if (!t) return;
    uint8_t old_pos[12], old_rot[16];
    std::memcpy(old_pos, load.data() + t->at, sizeof old_pos);
    std::memcpy(old_rot, load.data() + t->at + 12, sizeof old_rot);
    const float p[3] = {pos.x, pos.y, pos.z};
    for (size_t j = 0; j + 12 <= load.size(); ++j) {
        if (std::memcmp(load.data() + j, old_pos, 12) == 0) {
            std::memcpy(load.data() + j, p, 12);
            j += 11;
        }
    }
    if (!rot) return;
    const float q[4] = {rot->x, rot->y, rot->z, rot->w};
    for (size_t j = 0; j + 16 <= load.size(); ++j) {
        if (std::memcmp(load.data() + j, old_rot, 16) == 0) {
            std::memcpy(load.data() + j, q, 16);
            j += 15;
        }
    }
}

Bytes remove_payload(const std::vector<uint32_t>& keys, uint8_t reason = kReasonFar) {
    Bytes p = {0x04, 0, 0, 0, 0, reason};
    for (uint32_t k : keys) {
        p.resize(p.size() + 4);
        std::memcpy(p.data() + p.size() - 4, &k, 4);
    }
    return p;
}

// At most `burst` at once, then `per_s` a second.
struct RateLimit {
    double burst, per_s;
    double tokens = 0;
    uint64_t at = 0;
    bool take(uint64_t now, double n) {
        tokens = at ? std::min(burst, tokens + static_cast<double>(now - at) * per_s / 1000.0) : burst;
        at = now;
        if (tokens < std::min(n, burst)) return false;
        tokens -= std::min(n, burst);
        return true;
    }
};

// ---- co-op thread state
struct Group {
    uint32_t leader = 0;
    std::vector<uint32_t> keys;  // leader first, members in arrival order
};
std::deque<Group> g_restore;
RateLimit g_restore_rate{10.0, 10.0};
struct LoginCheck {
    Group group;
    std::string name;
    uint64_t report_at = 0, check_at = 0;
    bool reported = false;
    uint64_t removed_from = 0;  // the removal ring's head when it was given back
};
std::vector<LoginCheck> g_checks;
uint64_t g_last_sweep = 0, g_last_stats = 0;
std::set<uint32_t> g_deferred_told;  // leaders whose deferral was logged this zone
std::set<uint32_t> g_would_sweep;    // dry run: leaders already logged
uint32_t g_answer_seq = 0;
bool g_answer_on = false, g_answered = false;
Counters g_count_logged;

// Groups of the held natives: each leader with every held native under it,
// in arrival order (the leader came first). A member whose leader is no
// longer held (the server removed it) is not given back: without its parent
// the client destroys it (eErrNoParentsNotExist); it is forgotten. Groups go
// in the order their leaders came. Caller holds the lock.
std::deque<Group> held_groups_locked() {
    std::map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> by_root;
    std::vector<uint32_t> orphans;
    for (const auto& [k, h] : g_held) {
        uint32_t root = k;
        bool orphan = false;
        for (int depth = 0; depth < 8; ++depth) {
            const uint32_t parent = g_held.at(root).parent;
            if (!parent) break;
            if (!g_held.count(parent)) {
                orphan = true;
                break;
            }
            root = parent;
        }
        if (orphan) orphans.push_back(k);
        else by_root[root].emplace_back(h.seq, k);
    }
    for (uint32_t k : orphans) g_held.erase(k);
    g_held_count = g_held.size();
    if (!orphans.empty()) {
        queue_log(std::format("Suppress: {} held camp members without their leader forgotten", orphans.size()));
    }
    std::vector<std::pair<uint64_t, Group>> groups;
    for (auto& [root, members] : by_root) {
        std::sort(members.begin(), members.end());
        Group g{root, {}};
        g.keys.push_back(root);
        for (const auto& m : members) {
            if (m.second != root) g.keys.push_back(m.second);
        }
        groups.emplace_back(g_held.at(root).seq, std::move(g));
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::deque<Group> out;
    for (auto& [seq, g] : groups) out.push_back(std::move(g));
    return out;
}

void forget_held(const char* why) {
    size_t n = 0;
    {
        Lock lock;
        n = g_held.size();
        g_held.clear();
        g_held_count = 0;
    }
    g_restore.clear();
    if (n) log::info("Suppress: {} held natives forgotten ({}); they come back with the next load", n, why);
}

const char* why_text(EnemyZoneMachine::Why w) {
    using W = EnemyZoneMachine::Why;
    switch (w) {
        case W::Far: return "host far away";
        case W::LinkLost: return "link lost";
        case W::HostGone: return "the host's character is gone";
        case W::HostJumped: return "the host travelled";
        case W::YouJumped: return "you travelled";
        case W::LoadingScreen: return "loading screen";
        case W::NotInWorld: return "you are not in the world";
        case W::HostStopped: return "the host stopped relaying";
        case W::Stopped: return "stopped";
        case W::None: break;
    }
    return "-";
}

proto::EnemyZoneMsg request(bool on) {
    proto::EnemyZoneMsg m;
    m.flags = static_cast<uint8_t>((on ? proto::EnemyZoneMsg::kOn : 0) |
                                   (g_dry ? proto::EnemyZoneMsg::kDryRun : 0) |
                                   (g_caution ? proto::EnemyZoneMsg::kCaution : 0));
    m.seq = g_zone.seq();
    m.radius_m = g_radius;
    return m;
}

size_t natives_in_zone() {
    Lock lock;
    size_t n = 0;
    for (const auto& [k, nat] : g_natives) {
        if (nat.verdict != Verdict::Yes) continue;
        const auto t = enemy_load::last_transform(nat.load);
        if (t && distance_to_center(t->pos) <= hide_radius()) ++n;
    }
    return n;
}

// The step's consequences (zone on / off) and the message for the host.
std::optional<proto::EnemyZoneMsg> apply(const EnemyZoneMachine::Step& st) {
    std::optional<proto::EnemyZoneMsg> out;
    const bool lab = g_set.lab;
    if (st.ask && !lab) out = request(true);
    if (st.withdraw && !lab) out = request(false);
    if (st.turned_on) {
        g_deferred_told.clear();
        g_would_sweep.clear();
        g_restore.clear();  // back in the zone: whatever was still held stays so
        g_dropping = true;
        g_last_sweep = 0;
        log::info("Suppress: zone ON ({} {:.0f} m away, {} natives to hide){}", lab ? "lab centre" : "host",
                  g_zone.last_distance(), natives_in_zone(), g_dry ? " - DRY RUN: nothing is hidden" : "");
    }
    if (st.turned_off) {
        g_dropping = false;
        g_host_radius = 0;
        {
            Lock lock;
            g_dry_held.clear();
        }
        if (!g_dry) {
            // The host's copies go here and now; the host removes its own on EnemyZone{off}.
            if (!lab) enemy_drop_copies_here();
            if (st.restore && g_set.restore) {
                Lock lock;
                g_restore = held_groups_locked();
            }
        }
        size_t held = 0;
        {
            Lock lock;
            held = g_held.size();
        }
        log::info("Suppress: zone OFF ({}), {}", why_text(st.why),
                  g_dry ? std::string("dry run")
                  : !st.restore ? std::format("nothing to give back ({} held forgotten)", held)
                  : !g_set.restore ? std::format("{} held stay hidden ([coop] suppress_restore=0)", held)
                                   : std::format("restoring {} in {} groups", held, g_restore.size()));
        if (!g_dry && (!st.restore || !g_set.restore)) forget_held(why_text(st.why));
    } else if (st.paused) {
        log::info("Suppress: the host died and reappeared {:.0f} m away: zone kept {} s (your enemies stay hidden)",
                  g_zone.last_distance(), g_zone.settings().respawn_grace_ms / 1000);
    } else if (st.resumed) {
        log::info("Suppress: the host is back ({:.0f} m): the zone continues", g_zone.last_distance());
    } else if (st.why == EnemyZoneMachine::Why::HostStopped) {
        log::warn("Suppress: the host does not relay its enemies ([dev] enemy_relay=1 and automatic copies, "
                  "Ctrl+F7, on the host): nothing is hidden; asking again in 30 s");
    }
    return out;
}

// A native holding a weapon stays while it fights: its last battle state is
// not 0, or went 0 less than kFightMs ago, or (no state seen) the weapon is
// under 30 s old. The server does not take weapons back when a fight ends
// (zona-tras-muerte-anfitrion.md C3): without this they were kept for good.
bool fighting_or_fresh(uint32_t npc, uint64_t now, const std::map<uint32_t, std::pair<uint8_t, uint64_t>>& battle,
                       uint64_t weapon_ms) {
    if (auto b = battle.find(npc); b != battle.end()) {
        return b->second.first != 0 || now - b->second.second < kFightMs;
    }
    return now - weapon_ms < 30000;
}

// The host now relays a smaller radius: held natives beyond it get no copy
// (the golden rule), so they are given back.
void give_back_beyond(float radius) {
    std::vector<std::pair<uint32_t, size_t>> told;
    {
        Lock lock;
        std::deque<Group> groups = held_groups_locked();
        for (Group& g : groups) {
            const auto it = g_held.find(g.leader);
            if (it == g_held.end()) continue;
            std::optional<proto::Vec3> at = it->second.pos;
            if (!at) {
                if (const auto t = enemy_load::last_transform(it->second.load)) at = t->pos;
            }
            if (!at || distance_to_center(*at) <= radius) continue;  // as decide() and sweep()
            if (std::any_of(g_restore.begin(), g_restore.end(), [&](const Group& r) { return r.leader == g.leader; })) {
                continue;
            }
            told.emplace_back(g.leader, g.keys.size());
            g_restore.push_back(std::move(g));
        }
    }
    for (const auto& [leader, n] : told) {
        log::info("Suppress: group 0x{:08X} ({} natives) is beyond the host's relayed {:.0f} m: given back", leader, n,
                  radius);
    }
}

// ---- the sweep: natives already in the client, at rest, inside the zone
void sweep(uint64_t now) {
    struct Info {
        uint32_t parent;
        Verdict verdict;
        std::vector<uint32_t> children;
    };
    std::map<uint32_t, Info> natives;
    std::map<uint32_t, uint64_t> fought, arming;
    std::map<uint32_t, uint64_t> armed;  // native -> its newest weapon's time
    std::map<uint32_t, std::vector<uint32_t>> weapons;  // native -> its weapon gimmicks
    std::map<uint32_t, std::pair<uint8_t, uint64_t>> battle;
    std::set<uint32_t> dead;
    {
        Lock lock;
        for (const auto& [k, n] : g_natives) natives[k] = {n.parent, n.verdict, n.children};
        fought = g_fought;
        arming = g_arming;
        battle = g_battle;
        for (const auto& [gimmick, npc] : g_weapon_of) {
            weapons[npc].push_back(gimmick);
            const auto w = g_weapon_ms.find(gimmick);
            armed[npc] = std::max(armed[npc], w == g_weapon_ms.end() ? uint64_t{0} : w->second);
        }
        dead = g_dead;
    }
    const float radius = hide_radius();
    for (const auto& [leader, info] : natives) {
        if (info.verdict != Verdict::Yes) continue;
        const auto pos = game::actor_world_position(leader);
        if (!pos || distance_to_center(*pos) > radius) continue;
        std::vector<uint32_t> group = {leader};
        for (const auto& [k, n] : natives) {
            if (k != leader && n.parent == leader) group.push_back(k);
        }
        // Members the leader names that are in the client but not known here
        // (no load to give back): the whole group stays.
        const char* why = nullptr;
        for (uint32_t c : info.children) {
            if (!natives.count(c) && game::find_actor(c)) why = "a member's load is unknown";
        }
        for (uint32_t k : group) {
            if (why) break;
            if (!game::find_actor(k)) why = "not all of it is loaded";
            else if (auto f = fought.find(k); f != fought.end() && now - f->second < kFightMs) why = "fighting you";
            else if (auto w = armed.find(k); w != armed.end() && fighting_or_fresh(k, now, battle, w->second)) {
                why = "holding a weapon in a fight";
            }
            else if (auto a = arming.find(k); a != arming.end() && now - a->second < kArmingMs) why = "taking a weapon";
            else if (dead.count(k)) why = "dead (a corpse to loot)";
        }
        if (why) {
            if (g_deferred_told.insert(leader).second) {
                ++g_count.deferred;
                log::info("Suppress: kept 0x{:08X} (group of {}): {}", leader, group.size(), why);
            }
            continue;
        }
        const float d = distance_to_center(*pos);
        std::string name;
        if (g_dry) {
            if (g_would_sweep.insert(leader).second) {
                ++g_count.would_sweep;
                {
                    Lock lock;
                    if (auto n = g_natives.find(leader); n != g_natives.end()) name = enemy_load::name(n->second.load);
                }
                log::info("Suppress: WOULD SWEEP group 0x{:08X} ({} natives, \"{}\", {:.0f} m from the host)", leader,
                          group.size(), name, d);
            }
            continue;
        }
        std::vector<std::pair<uint32_t, std::pair<std::optional<proto::Vec3>, std::optional<proto::Quat>>>> where;
        for (uint32_t k : group) where.push_back({k, {game::actor_world_position(k), game::actor_world_rotation(k)}});
        {
            Lock lock;
            bool all = g_held.size() + group.size() <= kMaxHeld;
            for (uint32_t k : group) all = all && g_natives.count(k) && !g_held.count(k);
            if (!all) continue;
            for (const auto& [k, pr] : where) {
                Native& n = g_natives[k];
                if (k == leader) name = enemy_load::name(n.load);
                Held h;
                h.seq = n.seq;
                h.load = std::move(n.load);
                h.parent = n.parent;
                h.swept = true;
                h.pos = pr.first;
                h.rot = pr.second;
                h.since_ms = now;
                g_held[k] = std::move(h);
                g_natives.erase(k);
            }
            g_held_count = g_held.size();
            g_count.swept += group.size();
        }
        puppet_inject_s2c(kOpRemoveActorAck, remove_payload(group));
        // Their weapons too, as the server does (reason 45 after the NPC): not
        // given back, the server hands a new one at the next request.
        std::vector<uint32_t> gimmicks;
        for (uint32_t k : group) {
            if (auto w = weapons.find(k); w != weapons.end()) gimmicks.insert(gimmicks.end(), w->second.begin(), w->second.end());
        }
        if (!gimmicks.empty()) {
            puppet_inject_s2c(kOpRemoveActorAck, remove_payload(gimmicks, kReasonWeaponGone));
            Lock lock;
            for (uint32_t gk : gimmicks) g_weapon_of.erase(gk), g_weapon_ms.erase(gk);
        }
        log::info("Suppress: removed group 0x{:08X} ({} natives{}, \"{}\", {:.0f} m from the host)", leader, group.size(),
                  gimmicks.empty() ? std::string() : std::format(" and their {} weapons", gimmicks.size()), name, d);
    }
}

// ---- giving back: one group per tick, at most 10 loads a second
void restore_tick(uint64_t now, bool in_world) {
    if (g_restore.empty() || !in_world) return;
    Group& g = g_restore.front();
    if (!g_restore_rate.take(now, static_cast<double>(g.keys.size()))) return;
    std::vector<std::pair<uint16_t, Bytes>> packets;
    std::string name;
    uint64_t oldest = now;
    bool whole = true;
    {
        Lock lock;
        // Whole groups or nothing: a member without its leader is destroyed,
        // a leader without a member stays half logged in (copias-grupo-invisibles.md §2.2).
        for (uint32_t k : g.keys) whole = whole && g_held.count(k);
        for (uint32_t k : g.keys) {
            auto it = g_held.find(k);
            if (it == g_held.end()) continue;
            Held& h = it->second;
            if (whole) {
                if (k == g.leader) name = enemy_load::name(h.load);
                oldest = std::min(oldest, h.since_ms);
                if (h.swept && h.pos) place(h.load, *h.pos, h.rot);
                packets.emplace_back(kOpActorLoadDataAck, std::move(h.load));
                if (g_set.restore_state) {
                    for (auto& [op, body] : h.state) packets.emplace_back(op, std::move(body));
                }
            }
            g_held.erase(it);
        }
        g_held_count = g_held.size();
    }
    if (!whole) {
        log::info("Suppress: group 0x{:08X} not given back: the server removed part of it (the rest comes back "
                  "with the next load)",
                  g.leader);
        g_restore.pop_front();
        return;
    }
    puppet_inject_s2c_batch(packets);
    g_count.restored += g.keys.size();
    log::info("Suppress: restored group 0x{:08X} ({} natives, \"{}\", held {} s; {} messages)", g.leader, g.keys.size(),
              name, (now - oldest) / 1000, packets.size());
    if (g_checks.size() < 256) {
        g_checks.push_back({g, name, now + kReportAfterMs, now + kLoginCheckAfterMs, false,
                            g_removed_head.load(std::memory_order_acquire)});
    }
    g_restore.pop_front();
}

void check_logins(uint64_t now) {
    for (auto it = g_checks.begin(); it != g_checks.end();) {
        LoginCheck& c = *it;
        if (!c.reported && now >= c.report_at) {
            c.reported = true;
            for (uint32_t k : c.group.keys) {
                uint16_t state = 0;
                uint8_t login = 0;
                const auto p = game::actor_world_position(k);
                if (read_login(k, state, login)) {
                    log::info("Suppress: restored 0x{:08X} exists: state +5E 0x{:X} login +CF {} at ({:.1f}, {:.1f}, "
                              "{:.1f})",
                              k, state, login, p ? p->x : NAN, p ? p->y : NAN, p ? p->z : NAN);
                } else {
                    log::info("Suppress: restored 0x{:08X} is NOT in the game's actor list", k);
                }
            }
        }
        if (now < c.check_at) {
            ++it;
            continue;
        }
        bool all = true;
        for (uint32_t k : c.group.keys) {
            uint16_t state = 0;
            uint8_t login = 0;
            all = all && read_login(k, state, login) && login == 1;
        }
        bool server = false;
        for (uint32_t k : c.group.keys) server = server || removed_since(k, c.removed_from);
        if (!all && server) {
            log::info("Suppress: group 0x{:08X} (\"{}\") was removed by the server after coming back (it comes back "
                      "by itself)",
                      c.group.leader, c.name);
        } else if (!all) {
            ++g_count.restore_failed;
            puppet_inject_s2c(kOpRemoveActorAck, remove_payload(c.group.keys));
            log::warn("Suppress: group 0x{:08X} (\"{}\") did not log in 5 s after coming back; removed again: your "
                      "enemies come back when you move away (about 250 m) or after a loading screen",
                      c.group.leader, c.name);
        }
        it = g_checks.erase(it);
    }
}

// The server removed these keys: nothing to give back for them.
void drain_server_removals() {
    const uint64_t head = g_removed_head.load(std::memory_order_acquire);
    if (head - g_removed_tail > kRemovedRing) g_removed_tail = head - kRemovedRing;
    std::vector<uint32_t> keys;
    for (; g_removed_tail < head; ++g_removed_tail) {
        const uint64_t v = g_removed_slots[g_removed_tail % kRemovedRing].load(std::memory_order_acquire);
        const auto want = static_cast<uint32_t>(g_removed_tail + 1);
        const auto tag = static_cast<uint32_t>(v >> 32);
        if (tag == want) {
            keys.push_back(static_cast<uint32_t>(v));
        } else if (static_cast<int32_t>(tag - want) < 0) {
            break;  // not written yet: next tick
        }
    }
    if (keys.empty()) return;
    Lock lock;
    for (uint32_t k : keys) {
        if (g_held.erase(k)) {
            if (g_count.server_removed++ < 20) {
                queue_log(std::format("Suppress: the server removed held 0x{:08X} (it comes back by itself)", k));
            }
        }
    }
    g_held_count = g_held.size();
}

void log_stats(uint64_t now) {
    if (now - g_last_stats < kStatsEveryMs) return;
    g_last_stats = now;
    Counters c;
    size_t held = 0;
    {
        Lock lock;
        c = g_count;
        held = g_held.size();
    }
    c.lock_misses = g_lock_misses.load();
    const bool changed = std::memcmp(&c, &g_count_logged, sizeof c) != 0;
    if (!changed && !g_zone.on() && !held) return;
    g_count_logged = c;
    log::info("Suppress: zone {} | held {}, dropped {}, swept {}, deferred {}, restored {}, restore failed {}, server "
              "removed while held {} | dry run: would drop {}, would sweep {} | lock misses {}{}",
              g_zone.on() ? "on" : "off", held, c.dropped, c.swept, c.deferred, c.restored, c.restore_failed,
              c.server_removed, c.would_drop, c.would_sweep, c.lock_misses, g_broken ? " | breaker: off" : "");
}

void update_hud() {
    std::string text;
    const bool en = g_set.english;
    if (g_broken) {
        text = en ? "Zone: off (too many respawns; see cdmp.log)" : "Zona: apagada (demasiadas apariciones; ver cdmp.log)";
    } else if (g_dry) {
        text = !g_zone.on() ? ""
               : en         ? std::format("Zone {:.0f} m (test): your enemies stay", g_radius.load())
                            : std::format("Zona {:.0f} m (prueba): tus enemigos siguen", g_radius.load());
    } else if (g_zone.on() && g_zone.grace_until() > now_ms()) {
        const uint64_t left = (g_zone.grace_until() - now_ms() + 999) / 1000;
        text = en ? std::format("Zone paused: the host is coming back ({} s)", left)
                  : std::format("Zona en pausa: el anfitrión vuelve ({} s)", left);
    } else if (g_zone.on()) {
        if (g_set.lab) {
            text = en ? std::format("Lab: your enemies hidden ({:.0f} m)", g_radius.load())
                      : std::format("Laboratorio: tus enemigos ocultos ({:.0f} m)", g_radius.load());
        } else {
            text = en ? std::format("Host's enemies ({:.0f} m zone)", hide_radius())
                      : std::format("Enemigos del anfitrión (zona {:.0f} m)", hide_radius());
        }
    } else if (!g_restore.empty()) {
        text = en ? "Your enemies (coming back)" : "Tus enemigos (volviendo)";
    } else if (g_zone.state() != EnemyZoneMachine::State::Off) {
        text = en ? "Your enemies (host's zone near)" : "Tus enemigos (zona del anfitrión cerca)";
    }
    Lock lock;
    g_hud = std::move(text);
}

}  // namespace

void suppress_configure(const SuppressSettings& s) {
    g_set = s;
    const float radius = s.lab ? s.lab_radius_m : s.radius_m;
    const float off = s.lab ? s.lab_radius_m + 20.0f : s.off_m;
    g_zone = EnemyZoneMachine({radius, off, static_cast<uint64_t>(s.wait_fight_s) * 1000,
                               s.lab ? 0 : static_cast<uint64_t>(s.respawn_grace_s) * 1000});
    g_radius = radius;
    g_caution = s.node_events;
    g_dry = s.mode != SuppressMode::On;
    g_mode = s.mode;
    if (s.mode == SuppressMode::Off) return;
    log::info("Suppress: {} | zone {:.0f} m on, {:.0f} m off | give back {} (with state {}) | fight wait {} s | "
              "host respawn grace {} s | node events {}{}",
              s.mode == SuppressMode::On ? "ON" : "DRY RUN (only logs what it would hide)", radius, off,
              s.restore ? "on" : "off", s.restore_state ? "on" : "off", s.wait_fight_s, s.respawn_grace_s,
              s.node_events ? "on" : "off",
              s.lab ? " | LAB: Ctrl+Home turns a zone around you on and off (no host, no relay)" : "");
}

SuppressMode suppress_mode() { return g_mode.load(); }

bool suppress_filter_s2c(const uint8_t* packet, uint16_t size) {
    const SuppressMode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == SuppressMode::Off || g_broken || !packet || size < 5) return false;
    uint8_t head[5 + 11];
    if (!guarded_copy(head, packet, 5)) return false;
    uint16_t op;
    std::memcpy(&op, head, 2);
    if (op == kOpRemoveActorAck) {
        on_server_removal(packet, size);
        return false;
    }
    if (op != kOpActorLoadDataAck) return false;
    const bool dry = g_dry.load(std::memory_order_relaxed);
    const bool held = g_held_count.load(std::memory_order_relaxed) > 0;
    if (!g_dropping.load(std::memory_order_relaxed) && !held) return false;
    const size_t n = size - 5u;
    if (n < 0x40 || n > kMaxLoad || !guarded_copy(head + 5, packet + 5, 11)) return false;
    // u16 len | u8 type | u32 key | u32 owner: one NPC record of the game's world.
    uint16_t len;
    std::memcpy(&len, head + 5, 2);
    const uint8_t type = head[7];
    if (static_cast<size_t>(len) + 2 != n || u32_at(head + 12) != 0 || !keys::is_game_world_key(u32_at(head + 8))) {
        return false;
    }
    // Type 6 only as a member of a held camp (an animal never is a zone enemy).
    if (type != 3 && !(type == 6 && held)) return false;
    Bytes load(n);
    if (!guarded_copy(load.data(), packet + 5, n)) return false;
    return decide(std::move(load), dry);
}

bool suppress_allow_injection(const uint8_t* packet, size_t size) {
    if (g_mode.load(std::memory_order_relaxed) == SuppressMode::Off || size < 5 + 0x40) return true;
    uint16_t op, len;
    std::memcpy(&op, packet, 2);
    std::memcpy(&len, packet + 5, 2);
    if (op != kOpActorLoadDataAck || static_cast<size_t>(len) + 7 != size) return true;
    const uint32_t key = u32_at(packet + 8);
    if (!keys::is_game_world_key(key) || !removed_lately(key)) return true;
    return false;
}

void suppress_observe_s2c(const uint8_t* packet, size_t size) {
    if (g_mode.load(std::memory_order_relaxed) == SuppressMode::Off || size < 5 + 4) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    const uint64_t now = now_ms();
    switch (op) {
        case kOpActorLoadDataAck: {
            const auto head = enemy_load::read_head({p, n});
            if (!head || head->owner != 0 || !keys::is_game_world_key(head->key)) return;
            if (head->type == 7) {
                // A weapon for a native that asked for one (each that comes
                // within kArmingMs: a second weapon too).
                Lock lock;
                if (g_arming.empty() || g_weapon_of.size() >= kMaxSmallSet) return;
                for (size_t i = 5; i + 4 <= n; ++i) {
                    const uint32_t k = u32_at(p + i);
                    auto a = g_arming.find(k);
                    if (a == g_arming.end() || now - a->second > kArmingMs) continue;
                    g_weapon_of[head->key] = k;
                    g_weapon_ms[head->key] = now;
                    break;
                }
                return;
            }
            if (head->type != 3 && head->type != 6) return;
            const std::span<const uint8_t> load(p, n);
            const auto tail = enemy_load::parse_tail(load);
            const uint32_t parent = tail ? tail->parent : 0;
            std::vector<uint32_t> learnt;
            {
                Lock lock;
                learnt = g_learnt;
            }
            const auto check =
                head->type == 3 ? enemy_load::zone_check(load, {g_caution.load(), learnt}) : enemy_load::Check{};
            if (check.verdict != Verdict::Yes && !parent) return;
            Lock lock;
            if (g_held.count(head->key)) return;  // dropped: the game never saw it
            if (g_natives.size() >= kMaxNatives && !g_natives.count(head->key)) {
                auto oldest = std::min_element(g_natives.begin(), g_natives.end(),
                                               [](const auto& a, const auto& b) { return a.second.seq < b.second.seq; });
                g_natives.erase(oldest);
            }
            g_natives[head->key] = {++g_seq, Bytes(p, p + n), parent, check.verdict,
                                    tail ? tail->children : std::vector<uint32_t>{}};
            return;
        }
        case kOpRemoveActorAck: {
            // The server's and ours: either way it is not in the client any more.
            if (n < 10 || p[0] != 0x04) return;
            Lock lock;
            for (size_t i = 6; i + 4 <= n; i += 4) {
                const uint32_t k = u32_at(p + i);
                g_natives.erase(k);
                g_fought.erase(k);
                g_arming.erase(k);
                g_dead.erase(k);
                g_battle.erase(k);
                g_weapon_of.erase(k);
                g_weapon_ms.erase(k);
                std::erase_if(g_weapon_of, [&](const auto& w) {
                    if (w.second != k) return false;
                    g_weapon_ms.erase(w.first);
                    return true;
                });
            }
            return;
        }
        case kOpNotifyHandleDeadAck: {
            if (n != 19) return;
            Lock lock;
            if (g_natives.count(u32_at(p)) && g_dead.size() < kMaxSmallSet) g_dead.insert(u32_at(p));
            return;
        }
        case kOpAttackTargetAck: {
            if (n < 8) return;
            const uint32_t target = u32_at(p), attacker = u32_at(p + 4), own = capture_own_player_key();
            if (!own) return;
            Lock lock;
            if (attacker == own && g_natives.count(target)) g_fought[target] = now;
            if (target == own && g_natives.count(attacker)) g_fought[attacker] = now;
            return;
        }
        case kOpUpdateFactionRelationDataAck: {
            static uint64_t told = 0;
            if (told++ < 20 || told % 100 == 0) {
                log::info("Suppress: UpdateFactionRelationDataAck ({} B): faction relations changed in your world",
                          n);
            }
            return;
        }
        case kOpVaryHPAck:
        case kOpApplyBuffAck:
        case kOpUnapplyBuffAck:
        case kOpChangeMoveSessionIDAck:
        case kOpUpdateElementalAck: {
            Lock lock;
            auto it = g_held.find(u32_at(p));
            if (it == g_held.end()) return;
            auto& state = it->second.state;
            const bool latest_only = op != kOpApplyBuffAck && op != kOpUnapplyBuffAck;
            if (latest_only) std::erase_if(state, [&](const auto& s) { return s.first == op; });
            if (state.size() >= kMaxStatePerKey) {
                // Make room: the oldest buff goes (the latest HP and the like stay).
                auto buff = std::find_if(state.begin(), state.end(), [](const auto& s) {
                    return s.first == kOpApplyBuffAck || s.first == kOpUnapplyBuffAck;
                });
                if (buff == state.end()) return;
                state.erase(buff);
            }
            state.emplace_back(op, Bytes(p, p + n));
            return;
        }
        default: return;
    }
}

void suppress_observe_c2s(const uint8_t* packet, size_t size) {
    if (g_mode.load(std::memory_order_relaxed) == SuppressMode::Off || size < 5 + 5) return;
    uint16_t op;
    std::memcpy(&op, packet, 2);
    const uint8_t* p = packet + 5;
    const size_t n = size - 5;
    const uint32_t own = capture_own_player_key();
    const uint64_t now = now_ms();
    if (op == kOpCurrentTargetChangeReq && n >= 8) {
        if (!own || u32_at(p + 4) != own) return;
        Lock lock;
        if (g_natives.count(u32_at(p))) g_fought[u32_at(p)] = now;
    } else if (op == kOpChangeBattleStateReq && n == 5) {
        Lock lock;
        const uint32_t k = u32_at(p);
        if (!g_natives.count(k)) return;
        if (g_battle.size() < kMaxSmallSet || g_battle.count(k)) g_battle[k] = {p[4], now};
        if (p[4] != 0) g_fought[k] = now;
    } else if (op == kOpDoFrameEventReq && n >= 0x203 + 665 && p[0] == 0x04) {
        if (!own) return;
        bool at_us = false;
        for (size_t k = 1; k <= 30 && n >= k * 665 + 0x203; ++k) {
            const uint32_t t = u32_at(p + n - k * 665);
            if (!keys::is_actor_key(t)) break;
            at_us = at_us || t == own;
        }
        if (!at_us) return;
        Lock lock;
        if (g_natives.count(u32_at(p + 1))) g_fought[u32_at(p + 1)] = now;
    } else if (op == kOpOnDemandEquipDockingGimmickSpawnReq && n >= 5 && p[0] == 0x04) {
        Lock lock;
        if (g_natives.count(u32_at(p + 1)) && g_arming.size() < kMaxSmallSet) g_arming[u32_at(p + 1)] = now;
    }
}

std::optional<proto::EnemyZoneMsg> suppress_tick(const ZoneInput& in) {
    if (g_mode.load() == SuppressMode::Off) return std::nullopt;
    const uint64_t now = now_ms();
    {
        std::deque<std::string> lines;
        {
            Lock lock;
            lines.swap(g_logs);
        }
        for (const auto& l : lines) log::info("{}", l);
    }
    drain_server_removals();
    std::optional<proto::EnemyZoneMsg> out;
    // Through cdmp_peer this PC is host and guest at once: hiding would also
    // hide what its relay copies. Only the dry run (and the lab) there.
    if (in.echo && !g_set.lab && g_mode == SuppressMode::On) {
        if (!g_dry) log::warn("Suppress: through cdmp_peer only the dry run (use [coop] suppress_lab=1 to hide)");
        g_dry = true;
        g_echo_told = true;
    } else if (!in.echo && g_echo_told && g_mode == SuppressMode::On && !g_zone.on()) {
        g_dry = false;  // a real partner now
        g_echo_told = false;
    }
    if (g_breaker_tripped.exchange(false) && !g_broken) {
        g_broken = true;
        log::warn("Suppress: BREAKER: the same enemies keep coming back (or more than 60 loads a minute were "
                  "dropped): hiding is off for this session and your enemies are given back");
        if (auto msg = apply(g_zone.stop(EnemyZoneMachine::Why::Stopped, true, now))) out = msg;
    }
    if (g_set.lab) {
        const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_HOME) & 0x8000);
        if (key && !g_lab_key_was && !g_broken) {
            g_lab_on = !g_lab_on;
            log::info("Suppress lab: zone around you {} (radius {:.0f} m)", g_lab_on ? "ON" : "OFF", g_radius.load());
            if (!g_lab_on) apply(g_zone.stop(EnemyZoneMachine::Why::Stopped, true, now));
        }
        g_lab_key_was = key;
    }
    if (!g_broken) {
        EnemyZoneMachine::Input z;
        z.now_ms = now;
        z.in_world = in.in_world;
        z.me = in.me;
        z.host_dead = !g_set.lab && in.host_dead;
        z.loading_screen = g_loading_screen.exchange(false);
        // A loading screen removes everything at once (more keys than the
        // ring may keep): whatever is still held, zone on or not, is gone.
        if (z.loading_screen && !g_dry && g_zone.state() != EnemyZoneMachine::State::On) {
            forget_held("loading screen");
        }
        if (g_set.lab) {
            z.linked = g_lab_on;
            z.host = in.me;
            z.host_confirmed = true;
        } else {
            z.linked = in.linked;
            z.host = in.host;
            z.host_confirmed = g_answered && g_answer_seq == g_zone.seq() && g_answer_on;
            z.host_refused = g_answered && g_answer_seq == g_zone.seq() && !g_answer_on;
        }
        {
            Lock lock;
            for (const auto& [k, t] : g_fought) z.last_fight_ms = std::max(z.last_fight_ms, t);
        }
        // The zone's centre, before a step may turn it on (the game's thread
        // reads it as soon as dropping starts).
        if (z.host) g_center_x = z.host->x, g_center_z = z.host->z;
        if (g_set.lab && !g_lab_on && g_zone.state() == EnemyZoneMachine::State::Off) {
            // Lab off: nothing to tick.
        } else if (auto msg = apply(g_zone.tick(z))) {
            out = msg;
        }
        if (g_zone.on()) {
            if (now - g_last_sweep >= kSweepEveryMs) {
                g_last_sweep = now;
                sweep(now);
            }
        }
    } else {
        g_dropping = false;
    }
    restore_tick(now, in.in_world);
    check_logins(now);
    log_stats(now);
    update_hud();
    return out;
}

void suppress_on_answer(const proto::EnemyZoneMsg& a) {
    if (g_mode.load() == SuppressMode::Off) return;
    const bool news = !g_answered || a.seq != g_answer_seq || a.on() != g_answer_on;
    g_answered = true;
    g_answer_seq = a.seq;
    g_answer_on = a.on();
    if (!g_set.lab) {
        const float r = a.on() ? std::min(a.radius_m, g_radius.load()) : 0.0f;
        const float was = hide_radius();
        if (a.on() && std::fabs(r - was) >= 1.0f) {
            log::info("Suppress: the host relays its enemies within {:.0f} m (ours {:.0f} m)", r, g_radius.load());
        }
        g_host_radius = r;
        if (a.on() && r + 1.0f < was && g_zone.on() && !g_dry) give_back_beyond(r);
    }
    {
        Lock lock;
        g_learnt.assign(a.factions.begin(), a.factions.begin() +
                                                static_cast<ptrdiff_t>(std::min(a.factions.size(),
                                                                                proto::kMaxZoneFactions)));
    }
    if (news) {
        log::info("Suppress: the host answers request {}: {}{} ({} learnt factions)", a.seq,
                  a.on() ? "it relays every zone enemy" : "it does NOT relay", a.dry_run() ? " (dry run)" : "",
                  a.factions.size());
    }
    if (a.caution() != g_caution.load()) {
        log::warn("Suppress: the host answered with node events {} (ours {})", a.caution() ? "on" : "off",
                  g_caution.load() ? "on" : "off");
    }
}

std::optional<proto::EnemyZoneMsg> suppress_stop(bool restore) {
    const uint64_t now = now_ms();
    if (g_mode.load() == SuppressMode::Off) return std::nullopt;
    g_answered = false;
    return apply(g_zone.stop(EnemyZoneMachine::Why::Stopped, restore, now));
}

bool suppress_zone_on() { return g_dropping.load() && !g_dry.load(); }

bool suppress_is_hidden(uint32_t key) {
    if (g_held_count.load(std::memory_order_relaxed) == 0) return false;
    AcquireSRWLockShared(&g_lock);
    const bool held = g_held.count(key) != 0;
    ReleaseSRWLockShared(&g_lock);
    return held;
}

std::string suppress_hud_text() {
    if (g_mode.load() == SuppressMode::Off) return {};
    AcquireSRWLockShared(&g_lock);
    std::string s = g_hud;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

}  // namespace cdmp::net
