#pragma once
// The other player's character (the puppet) as an icon on our minimap.
// Every actor has a ClientMinimapActorComponent; the game asks 0x9E89C0
// which icon an actor gets and decides "none" for the puppet (another
// player who is not in our mercenary clan). An inline hook at the start of
// that function answers for the puppet only, with an icon from the game's
// own icon table; the game then creates the icon, moves and turns it every
// frame and removes it when the puppet goes (docs/research/mapa-minimapa.md
// §2, §4). Build 1.0.0.2976.

#include <cstdint>

namespace cdmp::net {

// What the puppet shows:
//   0           nothing (hook not installed)
//   -1          diagnostic: the game decides, its answer is logged
//   0xEA-0x16C  the icon at that offset of the game's icon table
//               (0x126 is the local player's own arrow)
// Call once before the puppet is loaded. False if the hook is not in place.
bool puppet_minimap_install(int table_offset);

// Dev: switch the icon. The game computes the new one on its next refresh
// but draws it only for a new copy (reload the puppet to see it).
void puppet_minimap_set_offset(int table_offset);
int puppet_minimap_offset();
// The icon (table offset, 0 = the game's) of the other player's enemy copies
// when the game gives them none.
void puppet_minimap_set_enemy_offset(int table_offset);

// Any thread, regularly: logs what happened in the hook.
void puppet_minimap_tick();

}  // namespace cdmp::net
