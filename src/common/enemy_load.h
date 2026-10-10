#pragma once
// Reading an NPC's ActorLoadDataAck payload (u16 len | one record), and the
// enemy-zone predicate of phase 5 (fase5-suprimir-enemigos-invitado.md §3.2):
// which of a world's enemies the HOST relays as copies inside the zone and,
// exactly the same ones, which the GUEST hides there. It lives here, once,
// so both sides use the same function (the "golden rule": if the host relays
// less than the guest hides, the guest sees empty camps; more, duplicates).
//
// Offsets of the payload (build 1.0.0.2976): +2 u8 type | +3 u32 key |
// +7 u32 owner | +0x14 control user | +0x18 variant | +0x1C faction |
// +0x20 u32 n + UTF-16 name | ... T0 (first Transform, after the signature
// 0C 00 | 8 x 00 | FF x 4 + 4) ... | last Transform: position (12 B) and
// rotation (16 B) right before the scale (1, 1, 1) and FF x 8 | ... |
// Hierarchy u16 m + m keys | u32 parent (actor+0xAC) | u16 n + n child keys
// (actor+0xB0).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/protocol.h"

namespace cdmp::enemy_load {

// The fixed header of a one-record load; nullopt if `load` is not exactly one
// record of at least 0x40 bytes.
struct Head {
    uint8_t type = 0;  // 3 person, 4 named character, 5 mount, 6 animal, 7 object
    uint32_t key = 0, owner = 0, variant = 0, faction = 0;
};
std::optional<Head> read_head(std::span<const uint8_t> load);

// Visible name (+0x20), printable ASCII, at most 40 characters.
std::string name(std::span<const uint8_t> load);

// The load's last Transform: where the actor is placed.
struct Transform {
    size_t at = 0;  // offset of the position
    proto::Vec3 pos;
    proto::Quat rot;
};
std::optional<Transform> last_transform(std::span<const uint8_t> load);

// Read from the end with its own counters: u16 n | n keys ends the record,
// the parent before it, and the Hierarchy u16 m | m keys before that.
struct Tail {
    size_t ac = 0;  // offset of actor+0xAC (parent)
    uint32_t parent = 0;
    std::vector<uint32_t> children;   // actor+0xB0 (a camp leader's members)
    std::vector<uint32_t> hierarchy;  // Hierarchy children, just before
};
std::optional<Tail> parse_tail(std::span<const uint8_t> load);

// Where the enemy spawns from (ids-enemigos-verificacion.md §2), relative to
// its first Transform T0: a schedule (hash + element id), a sequence (hash +
// id), a faction camp node (node + slot) or nothing (road bandits).
enum class OriginKind : char { Unknown = '?', Schedule = 'S', Sequence = 'Q', FactionNode = 'F', None = '-' };
struct Origin {
    OriginKind kind = OriginKind::Unknown;  // Unknown: T0 not found
    uint32_t id = 0;   // schedule / sequence hash, or faction node
    uint32_t sub = 0;  // element id, sequence id or camp slot
};
Origin origin(std::span<const uint8_t> load);

// Horizontal distance (the zone is a disk: x, z).
float flat_distance(const proto::Vec3& a, const proto::Vec3& b);

// ---- the predicate
struct ZoneRules {
    bool caution = false;  // also node and faction event sequences ([coop] suppress_node_events)
    // Hostile factions learnt in this session (the host's, shared through
    // EnemyZone), on top of the static table.
    std::span<const uint32_t> learnt_factions;
};
enum class Verdict : uint8_t {
    No,            // never hidden nor relayed by zone
    Yes,           // a zone enemy (still to be checked against the zone disk)
    FollowParent,  // a camp member: hidden / relayed exactly when its leader is
};
struct Check {
    Verdict verdict = Verdict::No;
    uint32_t parent = 0;           // for FollowParent
    const char* why = "";          // why not, for the logs
};
// Everything is read from the load, nothing from game memory:
//   one record, type 3 (a person: never 4 named, 5 mount, 6 animal, 7 object),
//   owner 0 (no mercenary or pet), a key of the game's world (not our copies),
//   a hostile faction (table or learnt), not a Boss / MiddleBoss / Unique
//   variant, an allowed origin (schedule or sequence in the allowed list, or
//   caution with the switch; a faction node; none), no Hierarchy children.
//   With a parent: FollowParent (the parent decides; one that is a mount or a
//   tower is not a zone enemy, so riders never are).
Check zone_check(std::span<const uint8_t> load, const ZoneRules& rules);

// Lookups in the build-2976 tables (enemy_tables_2976.h).
bool is_table_hostile_faction(uint32_t faction);
bool is_excluded_variant(uint32_t variant);
// 1 allowed, 2 caution, 0 neither (quests, bosses, contents...: never).
int origin_class(uint32_t hash);

// A short identity of where an enemy comes from, for the breaker (a spawner
// that keeps sending what it does not see): variant + origin, and for loads
// with no origin the variant + their 10 m cell.
uint64_t spawn_identity(std::span<const uint8_t> load);

}  // namespace cdmp::enemy_load
