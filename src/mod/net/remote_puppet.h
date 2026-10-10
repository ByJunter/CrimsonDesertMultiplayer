#pragma once
// The RECEIVING side of a remote player: a copy of another player's
// character loaded into this game (MMO code path: the client believes the
// server sent another player) and driven by that player's game events.
//
// Spawn injects the sender's two ActorLoadDataAck packets (user-owned actor
// + body) with fresh keys of ours. Events (local_capture.h) become the Acks a
// server would have relayed (MoveActorAck, StartActionAck, RotateActorAck,
// UpdateWallUpDataAck), queued and injected on the client's NetworkThread
// from inside the ClientNet::OnRecvPacket hook (troc_sniffer.cpp).
//
// Feed events from ONE thread only (the sniffer's writer thread for the
// mirror, the network thread for co-op).

#include <cstdint>
#include <optional>
#include <vector>

#include "common/protocol.h"

namespace cdmp::net {

// Any thread. Queue the load of a copy of the sender; false (with a log line)
// if a puppet already exists.
bool puppet_spawn(const proto::SpawnInfo& info);
// Any thread. Queue its removal (with fade-out); false if there is none.
bool puppet_remove();
bool puppet_spawned();
// True while this game sits paused in a menu (its clock stopped).
bool puppet_local_paused();
// True once after the sender dismounted: without a real link to a horse the
// copy stays stuck in the dismount pose (live tests), so co-op reloads it.
bool puppet_take_reload_request();
// Ask co-op to reload the copy where the sender is (any thread).
void puppet_request_reload();
// A copy of one of the sender's enemies (enemy_relay.h): its load, keys
// already translated, moved by the puppet offset, plus its recent actions
// (n x 725-byte StartActionReq, oldest first) queued right behind it. Network thread.
bool puppet_spawn_enemy(uint32_t copy_key, std::vector<uint8_t> load, std::vector<uint8_t> action_req);
// Moves / animates such a copy (network thread): a still sample where the
// enemy stands (built on the other player's last Move) or one of its
// StartActionReq / RotateActorReq, as the Acks a server would relay.
void puppet_enemy_move(uint32_t copy_key, uint64_t ts, const proto::Vec3& pos, const proto::Quat& rot,
                       int64_t ts_shift_ms, uint64_t delay_ms);
void puppet_enemy_action(uint16_t req_op, const std::vector<uint8_t>& req, int64_t ts_shift_ms,
                         uint64_t delay_ms);
// Removes one actor this side loaded (RemoveActorAck). Any thread.
void puppet_remove_actor(uint32_t key);

// Dev tests: queue one server->client packet for the client, now (any thread).
void puppet_inject_s2c(uint16_t op, const std::vector<uint8_t>& payload, uint64_t delay_ms = 0);
// Several packets, in this order and due together, so the client normally
// gets them in one OnRecvPacket pass like a real server batch.
void puppet_inject_s2c_batch(const std::vector<std::pair<uint16_t, std::vector<uint8_t>>>& packets);
// The game dropped the copy by itself (our loading screen, area change):
// forget it without sending a removal, so it can be loaded again.
void puppet_forget();
// Where the puppet is drawn right now (read from game memory); nullopt if
// it is not spawned or not loaded yet.
std::optional<proto::Vec3> puppet_position();

// The other player as a pin on our map (world map, and the minimap when
// near): the client half of the game's own pin markers, never told to our
// server, so never saved. `owner` is our own player key; nullopt removes the
// pin; `replace` removes the previous one first (needed to move it). Any thread.
void puppet_map_pin(uint32_t owner, const std::optional<proto::Vec3>& pos, bool replace = false);
// Shape (0-13) and colour (0-14) of that pin; out-of-range values are ignored.
void puppet_map_pin_style(uint8_t shape, uint8_t colour);

// Replays one event on the puppet.
//   ts_shift_ms: added to the event's timestamps to express them on THIS
//                game's clock, plus the playout delay;
//   delay_ms:    how long to hold the event before injecting it.
// Mirror: both are the mirror delay (same clock). Co-op: shift = estimated
// clock offset + delay.
void puppet_event(const proto::GameEvent& ev, int64_t ts_shift_ms, uint64_t delay_ms);

// Event thread, regularly: logs.
void puppet_tick();

// NetworkThread, inside the OnRecvPacket hook: next packet that is due.
// Never blocks (gives up if the queue is busy).
// While nothing is taken (the world loading): overdue movement goes anyway.
void puppet_drop_stale_movement();
bool puppet_take_injection(std::vector<uint8_t>& out);

// Lets the puppet follow its network position during free wall climbs
// (hooks PointClimb::Enter; only the puppet is affected). Call once.
bool puppet_install_climb_fix();

// Crash guards for network-created copies whose action layers have a chart
// but nothing playing yet (INFORME-CABALLO-CRASH.md). Call once; idempotent.
bool puppet_install_layer_guards();

// Sideways offset of the puppet (world X, metres); 0 = the sender's exact path.
void puppet_set_offset(float metres);
float puppet_offset_m();

// Sniffer writer thread, every server->client packet: which scene objects
// (trees) are loaded as gimmicks here, for the other player's tree hook.
void puppet_observe_s2c(const uint8_t* packet, size_t size);
// [dev] rope_piece_lookup: the other player's tree hook names our client's
// piece of that tree (found by its scene id) so the rope has a body to pull.
void puppet_set_rope_lookup(bool on);
// [dev] rope_bend: the tree the other player's copy pulls bends here too
// (a mid-hook in the actor update calling the game's SetBend). After the world exists.
bool puppet_install_rope_bend();
void puppet_want_rope_bend(bool on);

// Load and drive the sender's horse too, with the mount/dismount fixes
// ([coop] horse=1; experimental, off by default). Call before
// puppet_install_climb_fix.
void puppet_set_horse_enabled(bool on);
// Apply the sender's equipment changes ([coop] equip=1; experimental).
void puppet_set_equip_enabled(bool on);

}  // namespace cdmp::net
