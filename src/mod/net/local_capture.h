#pragma once
// The SENDING side of a remote player: turns what the local player does into
// a stream of game events that another game can replay on its copy of us
// (remote_puppet.h). Used both by the single-PC mirror (events go straight to
// our own puppet) and by co-op (events go over the network).
//
// Sources, all on the sniffer's writer thread:
//   - the player's own client->server messages (MoveActorReq, StartActionReq,
//     RotateActorReq), passed on unchanged;
//   - moves synthesised from memory while the game sends none but the player
//     does move (vehicles...);
//   - the ledge climb-up path (UpdateWallUpData) read from memory, which the
//     single-player game never sends;
//   - equipment changes (OtherEquipItemAck), from a hook on the in-process
//     server's writer of that message.
// The player's ActorLoadDataAck packets are kept as templates for SpawnInfo.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "common/protocol.h"

namespace cdmp::net {

// Writer thread: every captured server->client packet (templates).
void capture_observe_s2c(const uint8_t* packet, size_t size);
// Writer thread: every captured client->server packet (the player's Reqs).
void capture_observe_c2s(const uint8_t* packet, size_t size);
// Writer thread, every loop: synthesised moves and the climb-up path.
void capture_tick();

// Where events go (called on the writer thread). Empty = capture is idle.
using EventSink = std::function<void(const proto::GameEvent&)>;
void capture_set_sink(EventSink sink);

// What another game needs to load a copy of the local player, positioned at
// `pos`. nullopt until a save was loaded with the sniffer running.
std::optional<proto::SpawnInfo> capture_spawn_info(const proto::Vec3& pos, const proto::Quat& rot);

// Hooks the server's OtherEquipItemAck writer (read-only, arguments only).
// Call once after the world exists.
bool capture_install_equip_hook();
// Changes whenever new templates are captured (a save or area was loaded).
uint32_t capture_template_generation();
// Our horse's key if its template is captured and it is in the world now
// (what capture_spawn_info would send), else 0.
uint32_t capture_horse_present();
// Our own player actor's key (0xA01...), 0 until a save was loaded.
uint32_t capture_own_player_key();
// Our own user key (0x901...), 0 until a save was loaded.
uint32_t capture_own_user_key();
// True for a world character load seen (NPCs and enemies; not our pet,
// horse or companions).
bool capture_is_enemy(uint32_t key);
// True once the game loaded an actor whose key falls in the range of the keys
// the mod invents (keys.h): copies must not be created any more.
bool capture_key_alarm();
// Last known HP (x1000, -1 unknown) and death of one of our world's enemies.
struct EnemyState {
    int64_t hp = -1;
    bool dead = false;
};
EnemyState capture_enemy_state(uint32_t key);

// This game's clock as used in the Req timestamps (ms): read from the game
// once that reading agrees with our Reqs (it stops in menus), else
// extrapolated from the player's latest own Req. 0 while unknown.
uint64_t capture_game_clock_ms();
// True once the game's own clock (game_clock.h) is known to agree with our Reqs.
bool capture_game_clock_trusted();
// The check of the game's clock read is over (trusted or estimated instead).
bool capture_game_clock_decided();

// Combat tests: the nearest world actor (0xB01 key, not ours) whose movement
// our client reported in the last 3 s, within max_m of `from`, AND that has
// hit points (the server sent a VaryHPAck for it, or the player hit it).
// Anything else (gimmicks: levers, doors...) crashed the server when given
// damage (live test, 2026-10-03).
struct NearbyActor {
    uint32_t key;
    proto::Vec3 pos;
    float distance;
};
std::optional<NearbyActor> capture_nearest_actor(const proto::Vec3& from, float max_m);
// Attack hash (+0x24) of the latest PostProcessAttackResult_AttackTargetAck
// where the local player was the attacker; 0 if none yet.
uint32_t capture_last_attack_hash();
// Key of the actor the local player hit last (0 if none).
uint32_t capture_last_attack_target();

// Mirrored damage (docs/research/combate-coop.md, option b). The sender emits
// a Damage event whenever its server reports HP lost by an enemy to the local
// player; the receiver calls this to apply it to its own matching enemy
// through its own server (VaryStatByAccInputReq, verified live). Writer or
// network thread. Returns false if no matching enemy was found.
bool apply_remote_damage(const std::vector<uint8_t>& event);
// [coop] damage=0 turns both directions off.
void capture_set_damage_enabled(bool on);

// Timestamp (sender's game clock, ms) inside a GameEvent payload; 0 if the
// kind carries none (WallUp) or the payload is malformed.
uint64_t event_timestamp(const proto::GameEvent& ev);

}  // namespace cdmp::net
