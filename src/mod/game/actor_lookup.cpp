#include "mod/game/actor_lookup.h"

#include <windows.h>

#include <cmath>

#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"

namespace cdmp::game {
namespace {
namespace off = build_2976;

// Hash table of the ClientActorManager (same layout as the server's).
constexpr uint32_t kManager_Table = 0x08;
constexpr uint32_t kTable_BucketCount = 0x88;
constexpr uint32_t kTable_Buckets = 0x98;
constexpr uint32_t kTable_Nodes = 0xA0;
constexpr uint32_t kBucketStride = 0x100;
constexpr uint32_t kBucketMaxEntries = (kBucketStride - 8) / 8;
constexpr uint32_t kNode_Key = 0x04;
constexpr uint32_t kNode_Actor = 0x08;
// TransformSync component and its world position.
constexpr uint32_t kComponents_TransformSync = 0x1A0;
constexpr uint32_t kTransformSync_Position = 0xB4;   // vec3, local to the sector
constexpr uint32_t kTransformSync_Sector = 0xC0;     // i16 x, i16 z (1000 m sectors)
constexpr uint32_t kTransformSync_Rotation = 0xA4;   // quaternion (caballo-coop.md §5)
constexpr float kSectorSize = 1000.0f;

}  // namespace

std::optional<uintptr_t> find_actor(uint32_t key) {
    static const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base != off::kImageBase) return std::nullopt;
    auto world = read_ptr(base + off::kWorldSystemSlot);
    if (!world) return std::nullopt;
    auto manager = read_ptr(*world + off::kWorldSystem_ActorManager);
    if (!manager) return std::nullopt;
    auto table = read_ptr(*manager + kManager_Table);
    if (!table) return std::nullopt;
    auto buckets = read<uint32_t>(*table + kTable_BucketCount);
    if (!buckets || *buckets == 0 || *buckets > (1u << 24)) return std::nullopt;
    auto bucket_base = read_ptr(*table + kTable_Buckets);
    auto nodes = read_ptr(*table + kTable_Nodes);
    if (!bucket_base || !nodes) return std::nullopt;
    const uintptr_t bucket = *bucket_base + static_cast<uintptr_t>(key % *buckets) * kBucketStride;
    auto count = read<uint32_t>(bucket);
    if (!count) return std::nullopt;
    for (uint32_t i = 0; i < *count && i < kBucketMaxEntries; ++i) {
        auto k = read<uint32_t>(bucket + 8 + i * 8);
        if (!k || *k != key) continue;
        auto idx = read<uint32_t>(bucket + 0xC + i * 8);
        if (!idx) return std::nullopt;
        auto node = read_ptr(*nodes + static_cast<uintptr_t>(*idx) * 8);
        if (!node) return std::nullopt;
        auto nk = read<uint32_t>(*node + kNode_Key);
        if (!nk || *nk != key) return std::nullopt;
        return read_ptr(*node + kNode_Actor);
    }
    return std::nullopt;
}

std::optional<proto::Vec3> actor_world_position(uint32_t key) {
    auto actor = find_actor(key);
    if (!actor) return std::nullopt;
    auto components = read_ptr(*actor + off::kActor_ComponentTable);
    if (!components) return std::nullopt;
    auto ts = read_ptr(*components + kComponents_TransformSync);
    if (!ts) return std::nullopt;
    float p[3];
    int16_t sector[2];
    if (!safe_read(*ts + kTransformSync_Position, p, sizeof p) ||
        !safe_read(*ts + kTransformSync_Sector, sector, sizeof sector)) {
        return std::nullopt;
    }
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return std::nullopt;
    return proto::Vec3{p[0] + sector[0] * kSectorSize, p[1], p[2] + sector[1] * kSectorSize};
}


std::optional<proto::Quat> actor_world_rotation(uint32_t key) {
    auto actor = find_actor(key);
    if (!actor) return std::nullopt;
    auto components = read_ptr(*actor + build_2976::kActor_ComponentTable);
    if (!components) return std::nullopt;
    auto ts = read_ptr(*components + kComponents_TransformSync);
    if (!ts) return std::nullopt;
    float q[4];
    if (!safe_read(*ts + kTransformSync_Rotation, q, sizeof q)) return std::nullopt;
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!std::isfinite(n) || std::fabs(n - 1.0f) > 0.02f) return std::nullopt;
    return proto::Quat{q[0], q[1], q[2], q[3]};
}

bool actor_world_transform(uint32_t key, proto::Vec3& pos, proto::Quat& rot) {
    auto actor = find_actor(key);
    if (!actor) return false;
    auto components = read_ptr(*actor + off::kActor_ComponentTable);
    if (!components) return false;
    auto ts = read_ptr(*components + kComponents_TransformSync);
    if (!ts) return false;
    float p[3], q[4];
    int16_t sector[2];
    if (!safe_read(*ts + kTransformSync_Position, p, sizeof p) ||
        !safe_read(*ts + kTransformSync_Sector, sector, sizeof sector) ||
        !safe_read(*ts + kTransformSync_Rotation, q, sizeof q)) {
        return false;
    }
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!std::isfinite(n) || std::fabs(n - 1.0f) > 0.02f) return false;
    pos = {p[0] + sector[0] * kSectorSize, p[1], p[2] + sector[1] * kSectorSize};
    rot = {q[0], q[1], q[2], q[3]};
    return true;
}

}  // namespace cdmp::game
