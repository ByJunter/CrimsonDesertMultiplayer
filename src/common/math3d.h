#pragma once

#include <cmath>

#include "common/protocol.h"

namespace cdmp::math {

using proto::Quat;
using proto::Vec3;

inline Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline bool is_finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Unit quaternion from an orthonormal basis given as the object's local axes
// expressed in world space (the columns of its rotation matrix).
inline Quat quat_from_axes(const Vec3& x, const Vec3& y, const Vec3& z) {
    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    Quat q;
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-6f) {
        q.x /= len;
        q.y /= len;
        q.z /= len;
        q.w /= len;
    }
    return q;
}

// True if the three axes are unit length and mutually perpendicular, i.e. we
// are really looking at a rotation and not at garbage memory.
inline bool is_orthonormal(const Vec3& x, const Vec3& y, const Vec3& z, float tol = 0.02f) {
    if (!is_finite(x) || !is_finite(y) || !is_finite(z)) return false;
    return std::fabs(length(x) - 1) < tol && std::fabs(length(y) - 1) < tol &&
           std::fabs(length(z) - 1) < tol && std::fabs(dot(x, y)) < tol &&
           std::fabs(dot(y, z)) < tol && std::fabs(dot(x, z)) < tol;
}

}  // namespace cdmp::math
