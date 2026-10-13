#pragma once
// Pose maths shared by the XR layer and tests/pose_tests.cpp (PORT-06 desk tests).
// Convention (docs/POSE_OWNERSHIP.md): row-major, row-vector (p' = p * M), right-handed,
// Y up, -Z forward, metres. OpenXR LOCAL space uses the same basis and units as the engine
// camera, so no axis swap or unit scale exists between them.
#include <cmath>

namespace b2s::pose {

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};
struct Mat4 {
    float m[16];
};

inline Mat4 Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

// Normalises q in place. Returns false for a zero/non-finite quaternion (an untracked view
// can report one); callers must then keep their previous pose.
inline bool Normalize(Quat& q) {
    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!(n2 > 1e-8f) || !std::isfinite(n2)) return false;
    const float inv = 1.f / std::sqrt(n2);
    q = {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    return true;
}

// Pose (rotation q, position p) as a local-to-parent matrix.
inline Mat4 FromPose(const Quat& q, const Vec3& p) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    // Column-vector rotation R; the row-vector matrix stores R transposed.
    const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
    return {{r00, r10, r20, 0, r01, r11, r21, 0, r02, r12, r22, 0, p.x, p.y, p.z, 1}};
}

// Inverse of a rigid (rotation + translation) matrix.
inline Mat4 RigidInverse(const Mat4& a) {
    Mat4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = a.m[j * 4 + i];
    for (int j = 0; j < 3; ++j)
        r.m[12 + j] = -(a.m[12] * r.m[0 * 4 + j] + a.m[13] * r.m[1 * 4 + j] + a.m[14] * r.m[2 * 4 + j]);
    r.m[15] = 1.f;
    return r;
}

inline Vec3 TransformPoint(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12],
            p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13],
            p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14]};
}

// Heading of q in radians: 0 looking down -Z, positive turning left (counter-clockwise about +Y).
inline float Yaw(const Quat& q) {
    // Forward = R * (0,0,-1).
    const float fx = -2 * (q.x * q.z + q.y * q.w), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}

inline Quat YawQuat(float yaw) { return {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)}; }

inline float AngleBetween(const Quat& a, const Quat& b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(d > 1.f ? 1.f : d);
}

// Playbook A2.5: accept a recenter pose only after kNeeded consecutive tracked samples agree;
// a jump resets progress instead of becoming a permanent offset. Applies at startup and at
// every recenter request.
class SettleLatch {
public:
    static constexpr int kNeeded = 8;
    static constexpr float kMaxMetres = 0.25f;
    static constexpr float kMaxRadians = 0.785398f;

    void Reset() {
        stable_ = 0;
        have_ = false;
    }
    // Returns true on the frame the pose is accepted.
    bool Feed(const Quat& q, const Vec3& p, bool positionTracked, bool orientationTracked) {
        if (!positionTracked || !orientationTracked) {
            Reset();
            return false;
        }
        const float dx = p.x - lastP_.x, dy = p.y - lastP_.y, dz = p.z - lastP_.z;
        if (have_ && std::sqrt(dx * dx + dy * dy + dz * dz) < kMaxMetres && AngleBetween(q, lastQ_) < kMaxRadians) {
            if (++stable_ >= kNeeded) {
                stable_ = 0;
                have_ = false;
                return true;
            }
        } else {
            stable_ = 0;
        }
        lastQ_ = q;
        lastP_ = p;
        have_ = true;
        return false;
    }

private:
    int stable_ = 0;
    bool have_ = false;
    Quat lastQ_{0, 0, 0, 1};
    Vec3 lastP_{0, 0, 0};
};

// View offset post-multiplied onto the engine view (V_eye = V * offset) for an eye whose pose
// in LOCAL space is (q, p), relative to the seated reference refInv.
inline Mat4 EyeViewOffset(const Quat& q, const Vec3& p, const Mat4& refInv) {
    return RigidInverse(Mul(FromPose(q, p), refInv));
}

}  // namespace b2s::pose
