#pragma once
// The client's game clock, the "now" its handlers use for the network buffers
// of remote actors and that stamps our own Reqs: exe+0x1416B80 (on the
// client's threads, a value cached once per frame) plus the client-to-server
// offset [exe+0x6D69438] that ServerTimeInfoAck sets
// (movimiento-remoto-cliente.md section 2, pausa-copia-congelada.md).
// It stops while the game is paused in a menu. Read only, every read guarded;
// no game function is called.

#include <cstdint>
#include <optional>

namespace cdmp::game {

// nullopt if the clock is not readable or not running yet.
std::optional<uint64_t> client_game_clock_ms();

}  // namespace cdmp::game
