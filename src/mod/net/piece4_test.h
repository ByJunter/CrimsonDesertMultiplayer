#pragma once
// Piece 4 stagger experiments (docs/research/pieza4-tambaleo.md §7-8), dev only
// ([dev] piece4_test=1). They write to the game: back up the saves first and
// never save afterwards. Target: the enemy the player last hit (else the
// nearest one within 15 m). Each injects the last REAL staggering batch of our
// own blows (AttackTargetAck + ApplyActionRestrictionAck + hit buff),
// retargeted, with a new instance id:
//   Ctrl+F10        R1e  hit from the other player's copy, restriction and buff from nobody (0)
//   Ctrl+Shift+F10  R1f  no AttackTargetAck, restriction and buff from the copy
//   Ctrl+F11        R1c  everything from nobody
// Live results: with the copy or our own player as the source of everything
// (R1b, R1d) the enemy goes straight to the end of the reaction; with nobody
// (R1c) it staggers or falls. R1e/R1f tell whether the valid attacker or the
// valid source is what spoils it.

#include <cstddef>
#include <cstdint>

namespace cdmp::net {

// Sniffer writer thread: server->client packets (keeps the batch templates).
void piece4_observe_s2c(const uint8_t* packet, size_t size);
// Sniffer writer thread: client->server packets (logs the tested enemy's next actions).
void piece4_observe_c2s(const uint8_t* packet, size_t size);
// Own thread; `stop` is a HANDLE.
void piece4_test_loop(void* stop);

}  // namespace cdmp::net
