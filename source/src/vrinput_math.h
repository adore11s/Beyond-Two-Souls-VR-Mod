#pragma once

#include <cmath>
#include <cstdint>
#include "xr.h"

namespace b2s::vrinput {

struct Sample {
    bool focused = false;
    bool fresh = false;
    float headYaw = 0.f;
    float leftX = 0.f, leftY = 0.f;
    float rightX = 0.f, rightY = 0.f;
    float leftTrigger = 0.f, rightTrigger = 0.f;
    bool leftA = false, leftB = false, rightA = false, rightB = false;
    bool leftMenu = false, rightMenu = false;
    bool leftGrip = false, rightGrip = false;
    bool leftSqueeze = false, rightSqueeze = false;
    bool leftStickClick = false, rightStickClick = false;
};

struct LegacyState {
    float moveX = 0.f, moveY = 0.f;
    float viewX = 0.f, viewY = 0.f;
    float leftTrigger = 0.f, rightTrigger = 0.f;
    uint32_t buttons = 0;
};

constexpr uint32_t kLeftA = 1u << 0;
constexpr uint32_t kLeftB = 1u << 1;
constexpr uint32_t kRightA = 1u << 2;
constexpr uint32_t kRightB = 1u << 3;
constexpr uint32_t kLeftMenu = 1u << 4;
constexpr uint32_t kRightMenu = 1u << 5;
constexpr uint32_t kLeftGrip = 1u << 6;
constexpr uint32_t kRightGrip = 1u << 7;
constexpr uint32_t kLeftSqueeze = 1u << 8;
constexpr uint32_t kRightSqueeze = 1u << 9;
constexpr uint32_t kLeftStickClick = 1u << 10;
constexpr uint32_t kRightStickClick = 1u << 11;
constexpr uint32_t kLeftTriggerButton = 1u << 12;
constexpr uint32_t kRightTriggerButton = 1u << 13;
constexpr uint32_t kDpadUp = 1u << 14;
constexpr uint32_t kDpadDown = 1u << 15;
constexpr uint32_t kDpadLeft = 1u << 16;
constexpr uint32_t kDpadRight = 1u << 17;

inline float Clamp(float v, float lo = -1.f, float hi = 1.f) {
    if (!std::isfinite(v)) return 0.f;
    return v < lo ? lo : (v > hi ? hi : v);
}

// Converts the left stick from the seated reference into game-world movement.
// X is right and Y is forward; headYaw rotates the local vector into world space.
inline void HeadRelativeMove(float localX, float localY, float headYaw,
                             float& worldX, float& worldZ) {
    const float c = std::cos(headYaw), s = std::sin(headYaw);
    worldX = localX * c - localY * s;
    worldZ = localX * s + localY * c;
}

// The right stick always reaches the game: Beyond's interaction / QTE prompts are mostly
// right-stick gestures (owner, 2026-10-08: prompts could not be done in F1/F3 while the right
// stick was kept back for the view). In first-person gameplay it also turns the view (fpcam);
// the left stick is rotated to where the player looks.
inline LegacyState Map(const Sample& in, bool gameplay) {
    LegacyState out{};
    if (!in.focused || !in.fresh || !std::isfinite(in.headYaw)) return out;
    if (gameplay) {
        HeadRelativeMove(Clamp(in.leftX), Clamp(in.leftY), in.headYaw,
                         out.moveX, out.moveY);
    } else {
        out.moveX = Clamp(in.leftX);
        out.moveY = Clamp(in.leftY);
    }
    out.viewX = Clamp(in.rightX);
    out.viewY = Clamp(in.rightY);
    out.leftTrigger = Clamp(in.leftTrigger, 0.f, 1.f);
    out.rightTrigger = Clamp(in.rightTrigger, 0.f, 1.f);
    if (in.leftA) out.buttons |= kLeftA;
    if (in.leftB) out.buttons |= kLeftB;
    if (in.rightA) out.buttons |= kRightA;
    if (in.rightB) out.buttons |= kRightB;
    if (in.leftMenu) out.buttons |= kLeftMenu;
    if (in.rightMenu) out.buttons |= kRightMenu;
    if (in.leftGrip) out.buttons |= kLeftGrip;
    if (in.rightGrip) out.buttons |= kRightGrip;
    if (in.leftSqueeze) out.buttons |= kLeftSqueeze;
    if (in.rightSqueeze) out.buttons |= kRightSqueeze;
    if (in.leftStickClick) out.buttons |= kLeftStickClick;
    if (in.rightStickClick) out.buttons |= kRightStickClick;
    if (out.leftTrigger >= .5f) out.buttons |= kLeftTriggerButton;
    if (out.rightTrigger >= .5f) out.buttons |= kRightTriggerButton;
    // Left grip is the explicit D-pad modifier; locomotion alone never
    // generates menu directions or double-drives the game's input lane.
    constexpr float kDpadThreshold = .5f;
    if ((out.buttons & kLeftSqueeze) != 0) {
        if (out.moveY > kDpadThreshold) out.buttons |= kDpadUp;
        if (out.moveY < -kDpadThreshold) out.buttons |= kDpadDown;
        if (out.moveX < -kDpadThreshold) out.buttons |= kDpadLeft;
        if (out.moveX > kDpadThreshold) out.buttons |= kDpadRight;
    }
    return out;
}

inline Sample SampleFromFrame(const xr::FrameInput& frame, uint64_t now) {
    Sample out{};
    if (!xr::FrameInputUsable(frame, now)) return out;
    auto finiteMatrix = [](const pose::Mat4& m) {
        for (float v : m.m) if (!std::isfinite(v)) return false;
        return true;
    };
    if (!finiteMatrix(frame.headRel)) return out;
    out.focused = out.fresh = true;
    out.headYaw = std::atan2(frame.headRel.m[8], frame.headRel.m[10]);
    if (!std::isfinite(out.headYaw)) return Sample{};
    const auto& l = frame.hand[0]; const auto& r = frame.hand[1];
    out.leftX = Clamp(l.stick[0]); out.leftY = Clamp(l.stick[1]);
    out.rightX = Clamp(r.stick[0]); out.rightY = Clamp(r.stick[1]);
    out.leftTrigger = Clamp(l.trigger, 0.f, 1.f); out.rightTrigger = Clamp(r.trigger, 0.f, 1.f);
    out.leftA=l.a; out.leftB=l.b; out.rightA=r.a; out.rightB=r.b;
    out.leftMenu=l.menu; out.rightMenu=r.menu;
    // Pose validity controls arm rendering only.  Bumpers/D-pad use the
    // physical squeeze action so tracked hands are not reported as held.
    out.leftGrip=l.active && l.squeeze >= .5f; out.rightGrip=r.active && r.squeeze >= .5f;
    out.leftSqueeze=l.active && l.squeeze >= .5f; out.rightSqueeze=r.active && r.squeeze >= .5f;
    out.leftStickClick=l.stickClick; out.rightStickClick=r.stickClick;
    return out;
}

} // namespace b2s::vrinput
