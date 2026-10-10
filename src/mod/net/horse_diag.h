#pragma once
// Diagnostic for the horse-copy crash (docs/INFORME-CABALLO-CRASH.md and the
// outside review of it). Read-only mid-hooks at the entry of:
//   0x85FBA0  character-control (CC) update, every frame per character
//   0x8646D0  restores the actions from the load data after the character's
//             resources loaded; does nothing if byte [[CC+0x40]+0x30] == 0
//   0x2329D70 processes the pending actions (CC+0xA8 flag, CC+0x80 list)
// For the watched keys only (the copy 0xB010F002 and the sender's horse), it
// logs every change of the CC's action state, straight to the log file so the
// last lines survive a crash. Containment (optional): the copy's CC update
// (0x85FBA0 and the following 0x860D30) is skipped while its main layer has a
// chart but no current action. Build 1.0.0.2976.

#include <cstdint>

namespace cdmp::net {

// Call once (after the world exists). `contain` enables the skip.
bool horse_diag_install(bool contain);
// The sender's horse key, to watch it next to the copy (any thread).
void horse_diag_watch(uint32_t key);
// Writer thread, regularly: summary lines.
void horse_diag_tick();

}  // namespace cdmp::net
