#pragma once
// Dev probe F0 of docs/research/copia-objetivo-sin-ataques.md §5 ([dev]
// attack_probe=1): why enemies that target the other player's copy hardly
// ever attack it. Every 2 s, read only (guarded), it logs for our player and
// for the copy their control mode, AI battle-order byte, "in battle" byte and
// the attacker lists of their target component (CommonTargetActorComponent),
// and for every enemy whose current target is the copy, which lists it sits
// in (the copy's or our player's). Build 1.0.0.2976 offsets.

#include <cstddef>
#include <cstdint>

namespace cdmp::net {

// Sniffer writer thread: client->server packets (CurrentTargetChangeReq).
void attack_probe_observe_c2s(const uint8_t* packet, size_t size);
// Own thread; `stop` is a HANDLE.
void attack_probe_loop(void* stop);

}  // namespace cdmp::net
