#pragma once
// The game reports its internal failures through one function (exe+0x35D8D0,
// ~2900 callers) with a printf-style message (often Korean, UTF-8), an error
// code and a severity, but writes them nowhere. A read-only mid-hook copies
// them so cdmp.log shows why the game rejected something we injected
// (caballo-coop.md §8.3). Build 1.0.0.2976.

namespace cdmp::game {

// Call once after the game is up. Safe to call more than once.
bool game_errors_install();
// Writer thread, regularly: logs what was collected (deduplicated, capped).
void game_errors_drain();

}  // namespace cdmp::game
