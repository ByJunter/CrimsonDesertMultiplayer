#include "mod/game/camera_reader.h"

#include <windows.h>

#include <cmath>

#include "common/math3d.h"
#include "mod/game/offsets.h"
#include "mod/game/safe_memory.h"

namespace cdmp::game {
namespace off = build_2976;
using proto::Vec3;

namespace {

Vec3 normalize(const Vec3& v) {
    const float len = math::length(v);
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The view matrix may be stored row- or column-major. Its rotation part holds
// right/up/(+-)forward; pick the layout whose third axis matches the forward
// vector the game stores separately, then right = axis 0, up = axis 1.
bool axes_from_view(const float m[16], const Vec3& forward, Vec3& right, Vec3& up) {
    const Vec3 rows[3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    const Vec3 cols[3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
    for (const Vec3* axes : {rows, cols}) {
        if (!math::is_orthonormal(axes[0], axes[1], axes[2], 0.05f)) continue;
        if (std::fabs(math::dot(axes[2], forward)) < 0.98f) continue;
        right = axes[0];
        up = axes[1];
        return true;
    }
    return false;
}

}  // namespace

bool CameraReader::init() {
    base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base_ + dos->e_lfanew);
    enabled_ = base_ == off::kImageBase && nt->FileHeader.TimeDateStamp == off::kPeTimestamp;
    if (!enabled_) last_failure_ = "unsupported game build";
    return enabled_;
}

std::nullopt_t CameraReader::fail(const char* why) {
    last_failure_ = why;
    return std::nullopt;
}

std::optional<CameraData> CameraReader::read() {
    if (!enabled_) return fail("unsupported game build");
    auto camera = read_ptr(base_ + off::kCameraSlot);
    if (!camera || !has_vtable(*camera, base_ + off::kVtCamera)) return fail("no camera");
    auto block = read_ptr(*camera + off::kCamera_Block);
    if (!block) return fail("no camera block");
    auto sentinel = game::read<float>(*block + off::kCameraBlock_Sentinel);
    if (!sentinel || *sentinel != off::kCameraBlockSentinelValue) return fail("camera block sentinel mismatch");

    float pos[3], fwd[3], view[16], proj[16];
    if (!safe_read(*block + off::kCameraBlock_Position, pos, sizeof pos) ||
        !safe_read(*block + off::kCameraBlock_Forward, fwd, sizeof fwd) ||
        !safe_read(*block + off::kCameraBlock_View, view, sizeof view) ||
        !safe_read(*block + off::kCameraBlock_Projection, proj, sizeof proj)) {
        return fail("camera data unreadable");
    }

    CameraData cam;
    cam.position = {pos[0], pos[1], pos[2]};
    cam.forward = normalize({fwd[0], fwd[1], fwd[2]});
    cam.p00 = proj[0];
    cam.p11 = proj[5];
    if (!math::is_finite(cam.position) || math::length(cam.forward) < 0.5f ||
        !std::isfinite(cam.p00) || !std::isfinite(cam.p11) || cam.p00 <= 0 || cam.p11 <= 0) {
        return fail("camera values invalid");
    }

    cam.axes_from_view = axes_from_view(view, cam.forward, cam.right, cam.up);
    if (!cam.axes_from_view) {
        // Fallback assumes a left-handed, Y-up world (D3D convention).
        cam.right = normalize(cross({0, 1, 0}, cam.forward));
        cam.up = cross(cam.forward, cam.right);
    }
    last_failure_.clear();
    return cam;
}

std::optional<ScreenPoint> project(const CameraData& cam, const Vec3& camera_pos,
                                   const Vec3& target, float width, float height) {
    const Vec3 d = math::sub(target, camera_pos);
    const float depth = math::dot(d, cam.forward);
    if (depth < 0.1f) return std::nullopt;
    const float x_ndc = math::dot(d, cam.right) * cam.p00 / depth;
    const float y_ndc = math::dot(d, cam.up) * cam.p11 / depth;
    ScreenPoint p;
    p.x = (x_ndc * 0.5f + 0.5f) * width;
    p.y = (0.5f - y_ndc * 0.5f) * height;
    p.depth = depth;
    return p;
}

}  // namespace cdmp::game
