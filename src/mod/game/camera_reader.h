#pragma once
// Reads the game camera (read-only) and projects world positions to screen
// pixels. Layout from CrimsonDesertTelemetry + live checks with cdmp_probe:
//   camera  = [exe+0x6C8CF30] (vtable exe+0x5D20718)
//   block   = [camera+0x428]  (sanity: block+0xAC0 == 6360000.0f)
//   block+0x80 position, +0x90 forward, +0x3E0 view 4x4, +0x4E0 projection 4x4

#include <cstdint>
#include <optional>
#include <string>

#include "common/protocol.h"

namespace cdmp::game {

struct CameraData {
    proto::Vec3 position;  // as stored by the game (space decided by caller)
    proto::Vec3 forward;
    proto::Vec3 right;
    proto::Vec3 up;
    float p00 = 0, p11 = 0;  // projection scale X / Y
    bool axes_from_view = false;  // right/up taken from the view matrix
};

class CameraReader {
public:
    bool init();
    std::optional<CameraData> read();
    const std::string& last_failure() const { return last_failure_; }

private:
    std::nullopt_t fail(const char* why);
    bool enabled_ = false;
    uintptr_t base_ = 0;
    std::string last_failure_;
};

struct ScreenPoint {
    float x = 0, y = 0;
    float depth = 0;  // distance along the view direction (m)
};

// `target` and `camera_pos` must be in the same space. Returns nullopt when
// the point is behind the camera or too close to it.
std::optional<ScreenPoint> project(const CameraData& cam, const proto::Vec3& camera_pos,
                                   const proto::Vec3& target, float width, float height);

}  // namespace cdmp::game
