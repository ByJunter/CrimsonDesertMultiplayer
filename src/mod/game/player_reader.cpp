#include "mod/game/player_reader.h"

#include <windows.h>

#include <cmath>

#include "common/math3d.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"

namespace cdmp::game {
namespace off = build_2976;

namespace {

std::optional<proto::Vec3> read_vec3(uintptr_t addr) {
    float f[3];
    if (!safe_read(addr, f, sizeof f)) return std::nullopt;
    proto::Vec3 v{f[0], f[1], f[2]};
    if (!math::is_finite(v)) return std::nullopt;
    return v;
}

}  // namespace

bool PlayerReader::init() {
    base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base_ + dos->e_lfanew);
    enabled_ = base_ == off::kImageBase && nt->FileHeader.TimeDateStamp == off::kPeTimestamp;
    if (!enabled_) last_failure_ = "unsupported game build";
    return enabled_;
}

std::nullopt_t PlayerReader::fail(const char* why) {
    last_failure_ = why;
    return std::nullopt;
}

std::optional<uintptr_t> PlayerReader::resolve_actor() {
    auto world = read_ptr(base_ + off::kWorldSystemSlot);
    if (!world) return fail("WorldSystem not created");
    auto manager = read_ptr(*world + off::kWorldSystem_ActorManager);
    if (!manager || !has_vtable(*manager, base_ + off::kVtClientActorManager))
        return fail("ClientActorManager missing");
    auto actor = read_ptr(*manager + off::kActorManager_ControlledActor);
    if (!actor) return fail("no controlled actor (not in world)");
    if (!has_vtable(*actor, base_ + off::kVtControlledActor))
        return fail("controlled actor has unexpected type");
    return actor;
}

std::optional<uintptr_t> PlayerReader::character_control() {
    if (!enabled_) return fail("unsupported game build");
    auto actor = resolve_actor();
    if (!actor) return std::nullopt;
    auto components = read_ptr(*actor + off::kActor_ComponentTable);
    if (!components) return fail("component table missing");
    auto control = read_ptr(*components + off::kComponents_CharacterControl);
    if (!control || !has_vtable(*control, base_ + off::kVtCharacterControl))
        return fail("CharacterControl component missing");
    return control;
}

std::optional<PlayerSnapshot> PlayerReader::read() {
    if (!enabled_) return fail("unsupported game build");

    auto actor = resolve_actor();
    if (!actor) return std::nullopt;

    auto components = read_ptr(*actor + off::kActor_ComponentTable);
    if (!components) return fail("component table missing");
    auto control = read_ptr(*components + off::kComponents_CharacterControl);
    if (!control || !has_vtable(*control, base_ + off::kVtCharacterControl))
        return fail("CharacterControl component missing");
    auto inner = read_ptr(*control + off::kControl_Inner);
    if (!inner) return fail("control inner missing");
    auto physics = read_ptr(*inner + off::kControlInner_Physics);
    if (!physics || !has_vtable(*physics, base_ + off::kVtPhysicsObject))
        return fail("physics object missing");

    auto local = read_vec3(*physics + off::kPhysics_Position);
    auto origin = read_vec3(base_ + off::kChunkOrigin);
    auto ax = read_vec3(*physics + off::kPhysics_AxisX);
    auto ay = read_vec3(*physics + off::kPhysics_AxisY);
    auto az = read_vec3(*physics + off::kPhysics_AxisZ);
    if (!local || !origin || !ax || !ay || !az) return fail("physics data unreadable");
    if (!math::is_orthonormal(*ax, *ay, *az)) return fail("physics basis not a rotation");

    PlayerSnapshot snap;
    snap.position = math::add(*local, *origin);
    snap.local_position = *local;
    snap.chunk_origin = *origin;
    snap.rotation = math::quat_from_axes(*ax, *ay, *az);

    // The game keeps its own world-space copy; a large mismatch means our
    // chunk-origin math or chain is off (it lags by a frame, so allow some).
    if (auto global = read_vec3(base_ + off::kStaticPlayerPos)) {
        snap.static_pos_error = math::length(math::sub(snap.position, *global));
    }

    // While a save loads, the game parks the actor at (0, 1000, 0) for tens
    // of seconds before placing it. Don't broadcast that as a real position.
    if (std::fabs(snap.position.x) < off::kLoadingParkRadius &&
        std::fabs(snap.position.z) < off::kLoadingParkRadius &&
        std::fabs(snap.position.y - off::kLoadingParkHeight) < off::kLoadingParkRadius) {
        return fail("loading (actor parked at 0,1000,0)");
    }

    read_stats(*components, snap);
    last_failure_.clear();
    return snap;
}

void PlayerReader::read_stats(uintptr_t components, PlayerSnapshot& out) {
    auto status = read_ptr(components + off::kComponents_Status);
    if (!status || !has_vtable(*status, base_ + off::kVtStatusComponent)) return;
    auto container = read_ptr(*status + off::kStatus_StatContainer);
    if (!container) return;
    auto entries = read_ptr(*container + off::kStatContainer_Entries);
    auto count = game::read<uint32_t>(*container + off::kStatContainer_Count);
    if (!entries || !count || *count > off::kMaxStatEntries) return;

    // Look stats up by id: the entry order changes between patches.
    for (uint32_t i = 0; i < *count; ++i) {
        const uintptr_t e = *entries + static_cast<uintptr_t>(i) * off::kStatEntry_Size;
        auto id = game::read<uint64_t>(e + off::kStatEntry_Id);
        if (!id) continue;
        float* cur_out = nullptr;
        float* max_out = nullptr;
        if (*id == off::kStatId_Health) {
            cur_out = &out.health;
            max_out = &out.health_max;
        } else if (*id == off::kStatId_Stamina) {
            cur_out = &out.stamina;
            max_out = &out.stamina_max;
        } else {
            continue;
        }
        auto cur = game::read<int64_t>(e + off::kStatEntry_Current);
        auto max = game::read<int64_t>(e + off::kStatEntry_Max);
        if (!cur || !max || *max <= 0 || *cur < 0 || *cur > *max) continue;
        *cur_out = static_cast<float>(*cur) / 1000.0f;
        *max_out = static_cast<float>(*max) / 1000.0f;
    }
}

}  // namespace cdmp::game
