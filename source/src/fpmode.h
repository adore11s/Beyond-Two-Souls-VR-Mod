#pragma once
// Pure first-person camera logic (F1 mode, docs/superpowers/specs/2026-09-28-f1-first-person-design.md).
// No engine types: desk-tested in tests/fp_tests.cpp.
#include <cmath>

#include "posemath.h"

namespace b2s::fp {

enum class BehaviourClass { Unknown, Gameplay, Cutscene, NativeFirstPerson };
enum class Mode { Cinematic, FirstPerson, NativeFirstPerson };

// F1 enables gameplay first person. F3 is the independent cutscene camera
// override, so cutscene mode is controlled by F3 alone. Aiden / the game's
// own first person are never overridden; anything unrecognised stays cinematic.
inline Mode Classify(BehaviourClass c, bool f1, bool f3) {
    switch (c) {
        case BehaviourClass::NativeFirstPerson: return Mode::NativeFirstPerson;
        case BehaviourClass::Gameplay: return f1 ? Mode::FirstPerson : Mode::Cinematic;
        case BehaviourClass::Cutscene: return f3 ? Mode::FirstPerson : Mode::Cinematic;
        default: return Mode::Cinematic;
    }
}

// Heading of a world matrix: 0 looking down -Z, positive turning left; pitch and roll ignored.
// LIVE (owner, 0.9.3-0.9.5): views/bodies built from the head->eyes direction faced exactly
// backwards (F3 "flipped 180", body turned 180), while the game camera was verified to look
// along -Z. Jodie's front is the opposite of the head->eyes vector in this rig: add this.
// Runtime-switchable until measured on a clean (uncorrupted) pose: `fpflip 0|1`.
// LIVE owner 0.10.2: F3 needs the flip (view from the head->eyes vector showed her back), while
// the F1 body turn needs none (with the flip the drawn body faced away). Separate switches:
// `fpflip 0|1` (F3 view, default 1) and `fpbodyflip 0|1` (body turn, default 0).
inline float& HeadToEyesFlipRef() { static float v = 0.f; return v; }
inline float HeadToEyesFlip() { return HeadToEyesFlipRef(); }
inline float& BodyFlipRef() { static float v = 0.f; return v; }
inline float BodyFlip() { return BodyFlipRef(); }

// Yaw that faces from the head bone towards the midpoint of the eyes (F3: "where Jodie is
// looking"). Independent of the head bone's axis convention. False when the eyes are not usable.
inline bool LookYaw(pose::Vec3 head, pose::Vec3 eyeL, pose::Vec3 eyeR, float* yaw) {
    const float dx = (eyeL.x + eyeR.x) * 0.5f - head.x, dz = (eyeL.z + eyeR.z) * 0.5f - head.z;
    if (!yaw || !std::isfinite(dx) || !std::isfinite(dz) || dx * dx + dz * dz < 0.01f * 0.01f) return false;
    *yaw = std::atan2(-dx, -dz);  // forward = -Z rotated by yaw (YawQuat)
    return true;
}

// A character the game is not animating stands in its bind pose: a T-pose, both arms straight out
// at shoulder height (owner 0.16.11: F3 from inside a T-posed Jodie at a scene change). True when
// each hand is level with its shoulder (within 15 cm) and at least 45 cm out from it, and the hands
// are at least 1.2 m apart. Hanging, folded or reaching arms fail at least one of these.
inline bool LooksLikeTPose(pose::Vec3 shoulderL, pose::Vec3 handL, pose::Vec3 shoulderR, pose::Vec3 handR) {
    auto out = [](pose::Vec3 s, pose::Vec3 h) {
        const float dx = h.x - s.x, dz = h.z - s.z;
        return std::fabs(h.y - s.y) < 0.15f && dx * dx + dz * dz > 0.45f * 0.45f;
    };
    const float sx = handL.x - handR.x, sy = handL.y - handR.y, sz = handL.z - handR.z;
    return out(shoulderL, handL) && out(shoulderR, handR) && sx * sx + sy * sy + sz * sz > 1.2f * 1.2f;
}

// Moves angle `from` towards `to` by fraction t along the shorter arc.
inline float ApproachAngle(float from, float to, float t) {
    float d = std::remainder(to - from, 6.28318531f);
    return std::remainder(from + d * t, 6.28318531f);
}

inline float YawOf(const pose::Mat4& m) {
    const float fx = -m.m[8], fz = -m.m[10];  // row 2 = local +Z axis; forward is -Z
    return std::atan2(-fx, -fz);
}

// Camera base for first person: position = head + (eyeFwd forward, eyeUp up) in the body-yaw
// frame; orientation = body yaw only, the HMD supplies all head rotation (playbook CAM-001).
inline pose::Mat4 FirstPersonBase(const pose::Mat4& head, float bodyYaw, float eyeFwd, float eyeUp) {
    const pose::Quat yq = pose::YawQuat(bodyYaw);
    const pose::Mat4 rot = pose::FromPose(yq, {0, 0, 0});
    const pose::Vec3 off = pose::TransformPoint({0.f, eyeUp, -eyeFwd}, rot);
    return pose::FromPose(yq, {head.m[12] + off.x, head.m[13] + off.y, head.m[14] + off.z});
}

// Rejects non-finite positions and teleport-sized jumps between frames.
inline bool PlausibleStep(const pose::Vec3& prev, const pose::Vec3& next, float maxMetres) {
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) return false;
    const float dx = next.x - prev.x, dy = next.y - prev.y, dz = next.z - prev.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= maxMetres;
}

// Rising-edge detector for a polled key (GetAsyncKeyState high bit).
struct Toggle {
    bool was = false;
    bool Feed(bool down) {
        const bool edge = down && !was;
        was = down;
        return edge;
    }
};

}  // namespace b2s::fp
