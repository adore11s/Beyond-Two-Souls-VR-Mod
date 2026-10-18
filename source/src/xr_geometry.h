#pragma once
// Headset view geometry checks for the session log. Player report (0.17.14, Quest 3 over Quest
// Link, Meta's OpenXR runtime): near people and objects doubled, walls and sky fine, each eye
// clean on its own. The log now states what the runtime reports for each eye relative to the head
// and what the game cameras render with, so one log separates eye placement from frustum issues.
// Pure maths, desk-tested in tests/xr_geometry_tests.cpp.
#include <cmath>

#include "posemath.h"

namespace b2s::xrgeom {

// One eye as the runtime reports it in OpenXR VIEW space (relative to the head), angles in radians.
struct Eye {
    pose::Quat q;
    pose::Vec3 p;
    float angleLeft, angleRight, angleUp, angleDown;
};

enum Flag : unsigned {
    kSwapped = 1u << 0,        // the second eye is not to the right of the first
    kIpdOutOfRange = 1u << 1,  // eye distance outside 45..80 mm
    kVertical = 1u << 2,       // eyes at different heights (> 2 mm)
    kDepth = 1u << 3,          // eyes at different depths (> 2 mm)
    kCanted = 1u << 4,         // the eyes look in different directions (> 0.5 deg)
    kRotated = 1u << 5,        // an eye does not look along the head's forward (> 0.5 deg)
    kFovInvalid = 1u << 6,     // a frustum is empty, inverted, not finite or >= 90 deg on a side
};
constexpr unsigned kFlagCount = 7;

inline const char* FlagText(unsigned bit) {
    switch (bit) {
    case kSwapped: return "the right eye is not to the right of the left eye (eyes swapped)";
    case kIpdOutOfRange: return "eye distance outside 45..80 mm";
    case kVertical: return "the eyes are at different heights";
    case kDepth: return "the eyes are at different depths";
    case kCanted: return "the eyes look in different directions (canted views)";
    case kRotated: return "an eye does not look straight ahead of the head";
    case kFovInvalid: return "an eye frustum is invalid";
    default: return "?";
    }
}

struct Angles {
    float yaw, pitch, roll;  // degrees; yaw + turns left, pitch + looks up, roll + counter-clockwise
};

struct Report {
    float ipdMm = 0.f, dyMm = 0.f, dzMm = 0.f;  // right eye minus left eye, head frame
    float cantDeg = 0.f;
    Angles eye[2]{};
    unsigned flags = 0;
};

inline Angles EyeAngles(pose::Quat q) {
    constexpr float kToDeg = 57.2957795f;
    if (!pose::Normalize(q)) return {0.f, 0.f, 0.f};
    const pose::Mat4 m = pose::FromPose(q, {0.f, 0.f, 0.f});
    // Row-vector matrix: row 0 = image of +X (right), row 1 = +Y (up), row 2 = +Z (back).
    const float fx = -m.m[8], fy = -m.m[9], fz = -m.m[10];
    const float pitch = std::asin(fy < -1.f ? -1.f : fy > 1.f ? 1.f : fy);
    return {std::atan2(-fx, -fz) * kToDeg, pitch * kToDeg, std::atan2(m.m[1], m.m[5]) * kToDeg};
}

inline bool FovValid(const Eye& e) {
    constexpr float kLimit = 1.5707963f;
    const float a[] = {e.angleLeft, e.angleRight, e.angleUp, e.angleDown};
    for (float v : a)
        if (!std::isfinite(v) || std::fabs(v) >= kLimit) return false;
    return e.angleLeft < e.angleRight && e.angleDown < e.angleUp;
}

inline Report Evaluate(const Eye& left, const Eye& right) {
    constexpr float kToDeg = 57.2957795f;
    Report r;
    const float dx = right.p.x - left.p.x, dy = right.p.y - left.p.y, dz = right.p.z - left.p.z;
    r.ipdMm = 1000.f * std::sqrt(dx * dx + dy * dy + dz * dz);
    r.dyMm = 1000.f * dy;
    r.dzMm = 1000.f * dz;
    pose::Quat ql = left.q, qr = right.q;
    const bool qOk = pose::Normalize(ql) && pose::Normalize(qr);
    r.cantDeg = qOk ? pose::AngleBetween(ql, qr) * kToDeg : 0.f;
    r.eye[0] = EyeAngles(left.q);
    r.eye[1] = EyeAngles(right.q);
    if (!(dx > 0.f)) r.flags |= kSwapped;
    if (!(r.ipdMm >= 45.f && r.ipdMm <= 80.f)) r.flags |= kIpdOutOfRange;
    if (std::fabs(r.dyMm) > 2.f) r.flags |= kVertical;
    if (std::fabs(r.dzMm) > 2.f) r.flags |= kDepth;
    if (r.cantDeg > 0.5f) r.flags |= kCanted;
    const pose::Quat head{0.f, 0.f, 0.f, 1.f};
    if (!qOk || pose::AngleBetween(ql, head) * kToDeg > 0.5f || pose::AngleBetween(qr, head) * kToDeg > 0.5f)
        r.flags |= kRotated;
    if (!FovValid(left) || !FovValid(right)) r.flags |= kFovInvalid;
    return r;
}

// Right-eye camera position in the left eye's view space (metres), from the two world-to-view
// matrices the game renders with. Parallel eyes give about (+IPD, 0, 0).
inline pose::Vec3 RightEyeInLeftView(const pose::Mat4& viewLeft, const pose::Mat4& viewRight) {
    const pose::Mat4 camRight = pose::RigidInverse(viewRight);
    return pose::TransformPoint({camRight.m[12], camRight.m[13], camRight.m[14]}, viewLeft);
}

}  // namespace b2s::xrgeom
