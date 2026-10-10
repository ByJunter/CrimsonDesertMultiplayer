#pragma once
// Loot for the guest from the host's enemies (docs/research/botin-invitado.md).
//
// A dead copy of the host's enemy exists only in the guest's client: its loot
// prompt gave nothing. When the guest took part in the kill, the guest's own
// server makes a "twin" of that enemy (SpawnCharacterCheatReq, spawn reason
// Cheat) at the copy's place, the twin's load never reaches the guest's
// client, and KillCheatReq kills it at once (option 0: no XP, no quest
// credit, the variant's loot rolled natively). The guest then loots "the
// copy": its interaction requests are sent to the twin, and the twin's loot
// messages come back named after the copy. The host sends no items, only
// "copy N died"; the guest decides with its own data (it hit that copy in
// the last 30 s, is within 30 m of it, a plain enemy variant, rate limits).

#include <cstddef>
#include <cstdint>

namespace cdmp::net {

// [coop] guest_loot.
void loot_set_enabled(bool on);

// Sniffer writer thread: our own DoFrameEventReq (blows on copies).
void loot_observe_c2s(const uint8_t* packet, size_t size);

// Network thread: a copy of the other player's enemy died here. True if a
// twin is on its way (the copy keeps its loot prompt); false: no loot here.
bool loot_on_copy_died(uint32_t copy, uint32_t variant, uint64_t delay_ms);
// Network thread, every loop: twins that never came; forgotten copies.
void loot_tick();
// Network thread: the copy was removed.
void loot_forget_copy(uint32_t copy);
// Network thread: a dead copy whose twin is still to be looted (or on its
// way): its removal waits (the host's respawn sweep took them, 7 Oct night).
bool loot_keeps_copy(uint32_t copy);
// Network thread: the other player changed or their copies went (copy keys are
// reused by a restarted host).
void loot_reset();

// The game's network thread, every real server->client packet: true = do not
// deliver it (the twin's load and messages). May rewrite the packet in place.
bool loot_filter_s2c(uint8_t* packet, uint16_t size);
// The game's thread posting a client->server packet: interaction requests on
// a copy go to its twin (rewritten in place).
void loot_rewrite_c2s(uint8_t* packet, uint16_t size);

}  // namespace cdmp::net
