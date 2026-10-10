#pragma once
// Finds an actor by its key in the client's actor manager and reads its
// rendered world position. Read-only, every hop guarded; returns nullopt
// whenever anything is missing (loading screens, actor not loaded yet).
// Recipe: docs/research/movimiento-remoto-cliente.md §7 (build 1.0.0.2976).

#include <cstdint>
#include <optional>

#include "common/protocol.h"

namespace cdmp::game {

std::optional<uintptr_t> find_actor(uint32_t key);

// World position of the actor's TransformSync (what is drawn), Y up.
std::optional<proto::Vec3> actor_world_position(uint32_t key);
// Its facing (TransformSync quaternion x, y, z, w); nullopt unless unit length.
std::optional<proto::Quat> actor_world_rotation(uint32_t key);
// Both with one actor lookup (the host samples up to 48 enemies twice a second).
bool actor_world_transform(uint32_t key, proto::Vec3& pos, proto::Quat& rot);

}  // namespace cdmp::game
