#pragma once
// Piece 3 of host-authoritative enemies (docs/research/enemigos-del-anfitrion.md):
// the guest sees copies of the HOST's enemies. Stage 3a (dev test): Ctrl+F6
// sends the nearest enemy, still, with its last action; the receiver loads a
// network-driven copy of it (key 0xB01F0000 + net id, control user = the
// sender's copy user, so control mode 2) and gives it that action right
// away (a network-created actor needs one: INFORME-CABALLO-CRASH.md §11).
// Ctrl+Shift+F6 removes the copies.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "common/protocol.h"

namespace cdmp::net {

inline constexpr uint32_t kEnemyCopyBase = 0xB01F0000;  // + net id (1..0xEFFF)
inline bool is_enemy_copy_key(uint32_t key) { return (key & 0xFFFF0000u) == kEnemyCopyBase; }
// The net ids a sender hands out: index 0xF0000 + id stays below the client's
// 0xFFFFE/0xFFFFF sentinels. The receiver takes no other copy key.
inline constexpr uint32_t kEnemyCopyMaxNetId = 0xEFFF;
inline bool is_valid_enemy_copy_key(uint32_t key) {
    return is_enemy_copy_key(key) && (key & 0xFFFFu) >= 1 && (key & 0xFFFFu) <= kEnemyCopyMaxNetId;
}

// ---- sending side (sniffer writer thread: everything the client sees)
void enemy_observe_s2c(const uint8_t* packet, size_t size);
void enemy_observe_c2s(const uint8_t* packet, size_t size);

// Character variant (characterinfo key, load +0x18) of one of our world's
// enemies, 0 if unknown; and its copy key on the other side, 0 if none.
uint32_t enemy_variant(uint32_t key);
uint32_t enemy_copy_of(uint32_t key);
// The reverse: our enemy behind one of the copies we announced, 0 if none.
uint32_t enemy_original_of(uint32_t copy);
// One of our world's NPCs that is hostile and fighting right now: a type-3
// load with a faction (+0x1C != 0), in combat in the last 20 s, and that aimed
// at or struck our player or the other player's copy in that time. Anything
// the other player asks to hurt in our world must be one (security review
// H1, H6: never townsfolk, merchants, quest NPCs or allies), and not one we
// hold out of our client (native_suppress.h). Any thread.
bool enemy_hostile_in_combat(uint32_t key);
// Host: the guest may hit this enemy: fighting one of us (above), or a zone
// enemy we relay a copy of (the same predicate the copy was sent by), so the
// guest can start a fight (copias-sin-dano-7oct.md). An accepted blow marks
// it as fighting us (enemy_note_guest_blow).
bool enemy_guest_may_hit(uint32_t key);
void enemy_note_guest_blow(uint32_t key);

// ---- dev hotkeys (own thread); `stop` is a HANDLE
void enemy_test_loop(void* stop);
// [coop] enemy_auto: the copies go out on their own (Ctrl+F7 still toggles).
// Call before enemy_test_loop starts.
void enemy_set_auto(bool on);
// Co-op, every loop: a partner is connected and the game is ready. Copies are
// only made then (nothing would take them).
void enemy_set_linked(bool on);

// ---- phase 5, host (fase5-suprimir-enemigos-invitado.md §5.6-5.7). Network thread.
// The guest's EnemyZone request: while its zone is on (and not a dry run),
// the automatic copies are every zone enemy (common/enemy_load.h) within the
// zone's radius of us, up to 24, instead of only those fighting within 40 m;
// when it goes off, those copies go. Returns the answer: whether we relay
// (only with the automatic copies on), and the hostile factions we learnt.
proto::EnemyZoneMsg enemy_on_zone_request(const proto::EnemyZoneMsg& request);
// Every loop: the answer again when it changed (Ctrl+F7, factions learnt).
std::optional<proto::EnemyZoneMsg> enemy_zone_update();
// ---- phase 5, guest. Network thread: the zone went off, the copies of the
// other player's enemies here go now (it removes its own on EnemyZone{off}).
void enemy_drop_copies_here();

// Co-op (network thread): reliable messages to send.
// EnemySpawn: u32 copy key | u32 load size | load | n x StartActionReq (725 B each,
// oldest first; n may be 0).
// EnemyRemove: n x u32 copy key.
// EnemyState: u16 opcode | payload (VaryHPAck, NotifyHandleDeadAck; keys translated).
std::optional<std::pair<proto::RelKind, std::vector<uint8_t>>> enemy_take_outgoing();
// Co-op (network thread, every loop): events about the enemies we copied to
// the other player (their actions and, at 15 Hz, their positions).
std::vector<proto::GameEvent> enemy_host_tick();

// ---- receiving side (network thread)
// Copies of the other player's enemies are loaded only once their crash
// guards are installed (mod.cpp).
void enemy_set_receive_enabled(bool on);
// Checked before it goes in (one solo enemy record named only by its copy key,
// its actions all of that copy), and loaded from enemy_tick at a bounded rate
// (at most 48 copies here, 8 at once then 2 a second, a key not again within
// 10 s of its removal).
void enemy_on_spawn(const std::vector<uint8_t>& bytes);
// Network thread: a copy of the other side's enemies is loaded here.
bool enemy_copy_loaded(uint32_t copy);
void enemy_on_remove(const std::vector<uint8_t>& bytes);
// Network thread: the other side changed (new partner, restart, link lost):
// our copies sent there are forgotten (to be sent again) and the copies of
// theirs here are removed.
void enemy_reset_partner();
// `delay_ms`: the playout delay of the copies' actions.
void enemy_on_state(const std::vector<uint8_t>& bytes, uint64_t delay_ms);
// Network thread, every loop: loads the copies waiting their turn and logs
// where new copies ended up.
void enemy_tick();
// Network thread: an EnemyAct / EnemyPos event, with the clock shift and
// playout delay co-op uses for the other player's own events.
void enemy_on_event(const proto::GameEvent& ev, int64_t ts_shift_ms, uint64_t delay_ms);

}  // namespace cdmp::net
