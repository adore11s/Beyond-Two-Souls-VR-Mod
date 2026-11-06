#pragma once
// Pure parts of the head-tracked listener (audio.h): which listener follows the game camera, and the head's
// position / front / up in the engine's convention (row-vector, Y up, -Z forward, metres).
// Desk test: tests/audio_tests.cpp.
#include <cmath>
#include <cstdint>

#include "posemath.h"

namespace b2s::audio {

// The head of the last stereo pair (game thread, stereo.cpp RecordList).
struct HeadPublish {
    bool valid = false;
    uint64_t tickMs = 0;         // when it was published
    pose::Mat4 head{};           // the head (between the eyes) in the world
    pose::Mat4 authored{};       // the game camera's own world (what the game's listener feed reads)
};

// The listener being fed follows the game camera: the position the game hands it is that camera's own
// position (STATIC: FUN_1411d4210 / 1411da390 read the camera node's world row 3, +0xB0), within `tolerance`
// metres (a cut to another camera, or another listener of the ten, is further away).
inline bool FollowsCamera(const float pos[3], const pose::Mat4& authored, float tolerance = 1.f) {
    const float dx = pos[0] - authored.m[12], dy = pos[1] - authored.m[13], dz = pos[2] - authored.m[14];
    return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) && dx * dx + dy * dy + dz * dz < tolerance * tolerance;
}

// The head's position and the game's two listener vectors: front = world (0, 0, -1), up = world (0, 1, 0).
inline void HeadVectors(const pose::Mat4& head, float pos[4], float front[4], float up[4]) {
    pos[0] = head.m[12]; pos[1] = head.m[13]; pos[2] = head.m[14]; pos[3] = 1.f;
    front[0] = -head.m[8]; front[1] = -head.m[9]; front[2] = -head.m[10]; front[3] = 0.f;
    up[0] = head.m[4]; up[1] = head.m[5]; up[2] = head.m[6]; up[3] = 0.f;
}

inline bool Fresh(const HeadPublish& p, uint64_t nowMs, uint64_t maxAgeMs = 250) {
    return p.valid && p.tickMs <= nowMs && nowMs - p.tickMs <= maxAgeMs;
}

}  // namespace b2s::audio
