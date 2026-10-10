#pragma once
// Can the host's enemies target the other player's copy (the puppet)?
// docs/research/companeros-mercenarios.md §3-§7. In every capture the enemy
// AI (which runs on the client) never chose the puppet, although it passes
// the target checks. This dev test:
//   P1  read-only hooks at the entry of IsTargetable 0x22141E0 and
//       SetCurrentTarget 0x2217290: does the puppet ever come up?
//   P2  Ctrl+F5: inject CurrentTargetChangedAck (3040) so the last enemy that
//       targeted us targets the puppet instead (native "server says: your
//       target is X"); then the log shows whether it keeps it and attacks
//       (CurrentTargetChangeReq / DoFrameEventReq aimed at the puppet).
// Build 1.0.0.2976.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cdmp::net {

// Once, after the world exists. False if a hook site was not recognised.
bool aggro_test_install();
// Co-op: our enemies may pick the other player's copy as a target. The AI
// rejects it only for its "driven by the network" mark; this answers
// "targetable" instead (never while it is dead or hidden).
void aggro_set_copy_targetable(bool on);
// How many of our enemies may attack the other player's copy at once (they
// mostly wait otherwise: copia-objetivo-sin-ataques.md section 7); 0 = the game.
void aggro_set_copy_attack_slots(int slots);
int aggro_copy_attack_slots();
// Dev: our enemies that pick our own player as their target pick the other
// player's copy instead (one-PC tests of its fights).
void aggro_set_bait(bool on);
bool aggro_bait();
uint64_t aggro_baited();
// Counters of the CheckAIBattleType hook, for the log.
std::string aggro_battle_type_stats();
// Host, network thread: the other player's copy just hit our enemy `enemy`:
// it now targets that copy (CurrentTargetChangedAck 3040, force, reason
// 0x18, its target counter + 1), at most every 2 s per enemy, only while the
// copy is targetable (aggro_set_copy_targetable) and alive and shown
// (analisis-ia2/P2 §4.2-4.3). True if the request was queued.
bool aggro_request_target_on_copy(uint32_t enemy);
// True while the other player's copy is loaded and dead (Status +0x273).
bool aggro_copy_dead();
// Sniffer writer thread: every client->server packet (header included).
void aggro_observe_c2s(const uint8_t* packet, size_t size);
// Dev hotkey loop (Ctrl+F5). Blocks until `stop` (a HANDLE) is signalled.
void aggro_test_loop(void* stop);

}  // namespace cdmp::net
