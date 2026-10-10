#pragma once
// Piece 2 of host-authoritative enemies: our enemies hit the other player.
// With aggro_set_copy_targetable our enemies attack the other player's copy;
// each blow is a DoFrameEventReq whose target list names the copy, and our
// server, which does not know the copy, computes no damage. So:
//   sender: learns how much each attack hurts from the blows OUR player takes
//           (enemy variant + action hash -> HP lost), and sends a PlayerHit
//           event with that amount (0 when not learnt yet) for every blow on
//           the copy;
//   receiver: applies it to its own player through its own server
//           (VaryStatByAccInputReq), 5% of its max HP when the amount is unknown,
//           and reacts to it like the sender's player did (hits_set_react).
// combate-coop.md §1.3-1.4 (formats), §4.3 (c2).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/learnt_file.h"
#include "common/protocol.h"

namespace cdmp::net {

// Sniffer writer thread: every packet (header included).
void hits_observe_c2s(const uint8_t* packet, size_t size);
void hits_observe_s2c(const uint8_t* packet, size_t size);

// Co-op (network thread): PlayerHit events to send.
std::vector<proto::GameEvent> hits_take_events();
// Co-op (network thread): the other player's enemies hit us.
void hits_on_event(const proto::GameEvent& ev);

void hits_set_enabled(bool on);
// [coop] player_hit_react: PlayerHits carry how the sender's player reacts to
// that attack, and we react the same (stagger, knock-down, push).
// [coop] player_hit_lethal: a blow that takes all our HP kills us (a normal
// death); otherwise we stay at 1 HP.
void hits_set_react(bool on, bool lethal);
// Our player's HP x1000 from its last VaryHPAck (-1 unknown).
int64_t hits_own_hp();
// cdmp_learnt.txt (learnt_store.cpp): what the enemies' attacks do to us.
// Learnt whether or not co-op hits are on, so playing alone counts too.
void hits_learnt_get(LearntCombat& out);
void hits_learnt_put(const LearntCombat& in);

}  // namespace cdmp::net
