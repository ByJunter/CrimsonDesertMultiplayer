#pragma once
// Actor keys of the game (docs/research/pendiente-rango-claves.md, build 2976).
// bit 31 valid | bit 30 client-local | bits 28-29 class (1 user, 2 player,
// 3 world) | bits 20-27 generation | bits 0-19 index. The server hands out
// indices from one queue of 0x3C8D slots and bumps the generation each time an
// index comes back, so after 30-60 min of play real keys look like 0xB02...:
// never test `key >> 20 == 0xB01`.
// The keys the mod invents use indices >= 0x8000 (the server never reaches
// them) and generation 1.

#include <cstdint>

namespace cdmp::net::keys {

inline constexpr uint32_t kClassUser = 1;
inline constexpr uint32_t kClassPlayer = 2;
inline constexpr uint32_t kClassWorld = 3;
// Indices from here up are ours: the server's queue holds 0x3C8D of them.
inline constexpr uint32_t kFirstModIndex = 0x8000;
// Generations seen so far are 1 and 2 (one more every 30-60 min). The bound
// only keeps hashes that happen to look like keys out of payload scans
// (0xA73D6EC4, an action hash, would be generation 0x73): 64 is 30+ hours.
inline constexpr uint32_t kMaxGeneration = 64;

constexpr uint32_t key_class(uint32_t k) { return (k >> 28) & 3; }
constexpr uint32_t key_generation(uint32_t k) { return (k >> 20) & 0xFF; }
constexpr uint32_t key_index(uint32_t k) { return k & 0xFFFFF; }

// The shape of a server actor key, ours or the game's: valid, not
// client-local, a class, a generation from 1.
constexpr bool is_actor_key(uint32_t k) {
    return (k & 0xC0000000u) == 0x80000000u && key_class(k) != 0 && key_generation(k) != 0 &&
           key_generation(k) <= kMaxGeneration;
}
constexpr bool is_class(uint32_t k, uint32_t cls) { return is_actor_key(k) && key_class(k) == cls; }
// A key the game handed out (never one of ours).
constexpr bool is_game_key(uint32_t k) { return is_actor_key(k) && key_index(k) < kFirstModIndex; }
// A key the mod invented (the other player's copy, enemy copies).
constexpr bool is_mod_key(uint32_t k) { return is_actor_key(k) && key_index(k) >= kFirstModIndex; }
// One of the game's world actors (NPCs, enemies, objects, our body...).
constexpr bool is_game_world_key(uint32_t k) { return is_game_key(k) && key_class(k) == kClassWorld; }

static_assert(is_game_world_key(0xB0100223) && is_game_world_key(0xB0200005) && !is_game_world_key(0xB01F0004));
static_assert(is_mod_key(0xA010F001) && is_mod_key(0xB010F002) && is_mod_key(0xB01F0001) && !is_mod_key(0xA0100001));
static_assert(is_class(0x9010173F, kClassUser) && is_class(0xA0201740, kClassPlayer) && !is_actor_key(0x0F357AC7) &&
              !is_actor_key(0xA73D6EC4));

}  // namespace cdmp::net::keys
