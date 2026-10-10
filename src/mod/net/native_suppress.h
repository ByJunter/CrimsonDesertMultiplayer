#pragma once
// Phase 5 on the GUEST (docs/research/fase5-suprimir-enemigos-invitado.md):
// while we are within 150 m of the host, our own world's hostile enemies
// there ("natives") are hidden and the host's copies of its enemies stand in
// for them; beyond 250 m they come back. Only our client is touched, never
// our server: for it those natives stay alive and still where they were.
//   - New loads of zone natives (ActorLoadDataAck) are not delivered to the
//     client but held (the sniffer's hook skips ClientNet::OnRecvPacket).
//   - Natives already loaded, at rest, are swept: a RemoveActorAck (reason 25,
//     the server's own "far away") per camp group, their load held.
//   - When the zone goes off, the held loads the server still has are given
//     back (whole groups, in arrival order, where they stood), at most 10
//     loads a second; one that does not log in within 5 s is removed again.
// Which natives: the shared predicate (common/enemy_load.h), the same the
// host relays with. Nothing is hidden until the host confirms (EnemyZone)
// that it relays every one of them. The state machine is common/enemy_zone.h.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "common/protocol.h"

namespace cdmp::net {

enum class SuppressMode : uint8_t { Off, DryRun, On };

struct SuppressSettings {
    SuppressMode mode = SuppressMode::DryRun;  // [coop] suppress = 0 | dry | 1
    float radius_m = 150.0f;                   // suppress_radius_m
    float off_m = 250.0f;                      // suppress_off_m
    bool restore = true;                       // suppress_restore: give our natives back when it ends
    bool restore_state = true;                 // suppress_restore_state: with the server's messages about them
    uint32_t wait_fight_s = 60;                // suppress_wait_fight_s
    uint32_t respawn_grace_s = 60;             // suppress_respawn_grace_s (0: a respawn ends the zone)
    bool node_events = false;                  // suppress_node_events: node and faction events too
    bool lab = false;                          // suppress_lab: Ctrl+Home zone around you, no host, no relay
    float lab_radius_m = 60.0f;                // suppress_lab_radius_m
    bool english = true;                       // [overlay] language: the panel's line
};
// Late init, before co-op starts; only on the guest.
void suppress_configure(const SuppressSettings& settings);
SuppressMode suppress_mode();

// ---- game network thread, troc_sniffer's ClientNet::OnRecvPacket hook
// A real server packet (not one of our injections): true = do not deliver
// it (a zone native's load, now held). Never blocks; when in doubt, delivers.
bool suppress_filter_s2c(const uint8_t* packet, uint16_t size);
// One of our injections about to be delivered: false = skip it (a held load
// given back whose key the server removed in the meantime).
bool suppress_allow_injection(const uint8_t* packet, size_t size);

// ---- sniffer writer thread: what the client sees and does
void suppress_observe_s2c(const uint8_t* packet, size_t size);
void suppress_observe_c2s(const uint8_t* packet, size_t size);

// ---- co-op network thread
struct ZoneInput {
    bool linked = false;    // connected to a partner and the game ready
    bool in_world = false;  // our player is in the world
    bool echo = false;      // the partner is cdmp_peer (this PC talking to itself)
    std::optional<proto::Vec3> host;  // the host where it really is (its copy here, minus the offset)
    std::optional<proto::Vec3> me;
    bool host_dead = false;  // the host's PlayerState: 0 HP
};
// Every co-op update: the zone, the sweep (1 Hz), the giving back (10/s).
// Returns an EnemyZone request for the host when one is due.
std::optional<proto::EnemyZoneMsg> suppress_tick(const ZoneInput& in);
// The host's answer to our request (already decoded and validated).
void suppress_on_answer(const proto::EnemyZoneMsg& answer);
// Partner gone or changed, co-op off: the zone ends (our natives back if `restore`).
// Returns the EnemyZone{off} to send, if the host had been told.
std::optional<proto::EnemyZoneMsg> suppress_stop(bool restore);

// Any thread.
bool suppress_zone_on();               // hiding our natives right now (not in a dry run)
bool suppress_is_hidden(uint32_t key);  // one of our natives held out of the client
std::string suppress_hud_text();        // a line for the overlay ("" = none)

}  // namespace cdmp::net
