#pragma once
// Geometry of the VR settings panel (GoW-style panel, playbook chapter 2), in the seated reference frame
// (yaw-only, origin at the recentered head; Y up, -Z forward, metres). Pure: tests/panel_tests.cpp.
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include "posemath.h"

namespace b2s::panel {

// GOW_STYLE_TEN_ROW_V4 (playbook revision 6: the nine V1 rows plus "Hand smoothing (first person)" after Motion
// controls, "Rendering method" with Native | AFR | Mono; Beyond has no status meters, so no health-bars row) plus the B2S row "Aiden possessions (first person)"
// after the two first-person rows (owner 2026-10-28, EXP-046): B2S_ELEVEN_ROW_V2, 1000 x 1690 px at 1.2 m wide
// (2.028 m high), 0.15 m right of the game screen.
constexpr int kWidthPx = 1000, kHeightPx = 1690;
constexpr float kWidthM = 1.2f, kHeightM = kWidthM * float(kHeightPx) / float(kWidthPx);
constexpr float kGapM = 0.15f;
// Row geometry: the label of row i at y = kTop + i * kRow (44 px), its buttons 48 px below (68 px high).
constexpr int kTop = 122, kRow = 128, kLabelH = 44, kButtonY = 48, kButtonH = 68;
constexpr int kRows = 11;

struct Placement {
    pose::Vec3 center{};
    float yaw = 0.f;  // the panel's front (+Z) faces the head: yaw 0 = front towards +Z
};

// To the right of a screen centred at `screen` (facing the reference, i.e. its front towards +Z) of width
// `screenW`, at the same height, turned toward the head (yaw only).
inline Placement Place(const pose::Vec3& screen, float screenW, const pose::Vec3& head) {
    Placement p;
    p.center = {screen.x + 0.5f * screenW + kGapM + 0.5f * kWidthM, screen.y, screen.z};
    const float tx = head.x - p.center.x, tz = head.z - p.center.z;
    p.yaw = std::atan2(tx, tz);
    return p;
}

// A ray (origin, unit direction) against the panel's front: true with canvas pixel coordinates and the
// hit point when it hits within the panel and 0..20 m away (GoW: front only, dot(dir, normal) < -0.001).
inline bool Hit(const Placement& p, const pose::Vec3& o, const pose::Vec3& d, float* px, float* py, pose::Vec3* point) {
    const float nx = std::sin(p.yaw), nz = std::cos(p.yaw);  // +Z (front normal)
    const float rx = std::cos(p.yaw), rz = -std::sin(p.yaw);  // +X (panel right)
    const float dn = d.x * nx + d.z * nz;
    if (!(dn < -1e-3f)) return false;
    const float t = ((p.center.x - o.x) * nx + (p.center.z - o.z) * nz) / dn;
    if (!(t > 0.f && t < 20.f)) return false;
    const pose::Vec3 q{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    const float lx = (q.x - p.center.x) * rx + (q.z - p.center.z) * rz, ly = q.y - p.center.y;
    const float u = lx / kWidthM + 0.5f, v = 0.5f - ly / kHeightM;
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)) return false;
    *px = u * float(kWidthPx);
    *py = v * float(kHeightPx);
    *point = q;
    return true;
}

// The trigger (GoW): a pull past 0.75 clicks once; it re-arms below 0.35. Holding never repeats.
struct TriggerClick {
    bool armed = true;
    bool Feed(float trigger) {
        if (trigger < 0.35f) armed = true;
        if (armed && trigger > 0.75f) {
            armed = false;
            return true;
        }
        return false;
    }
};

// VR picture size stepper (GoW): 1440 .. 2816, then Auto (the runtime's own eye size) one step above the largest,
// only once that size is known. kGame (the game's own size, b2svr.ini render_size=off) sits below 1440.
constexpr int kGame = 0, kAuto = -1;
constexpr int kSizes[] = {1440, 1728, 2048, 2304, 2560, 2816};
constexpr int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));
// The setting after one '+' (dir 1) or '-' (dir -1). At Auto '+' does nothing and '-' gives the largest size
// below the Auto size; '-' at the game's own size does nothing.
inline int StepSize(int setting, int dir, int autoPx) {
    if (setting == kAuto) {
        if (dir > 0) return kAuto;
        int i = kSizeCount - 1;
        while (i > 0 && autoPx > 0 && kSizes[i] >= autoPx) --i;
        return kSizes[i];
    }
    if (setting == kGame) return dir > 0 ? kSizes[0] : kGame;
    int best = 0;
    for (int i = 1; i < kSizeCount; ++i)
        if (std::abs(kSizes[i] - setting) < std::abs(kSizes[best] - setting)) best = i;
    const int i = best + dir;
    if (i >= kSizeCount) return autoPx > 0 ? kAuto : kSizes[kSizeCount - 1];
    if (i < 0) return kSizes[0];
    return kSizes[i];
}

// Turning speed (smooth: 20 .. 100 %) and snap angle (15 .. 90 deg) presets (GoW).
constexpr float kSpeeds[] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
constexpr int kSnaps[] = {15, 30, 45, 60, 90};
template <class T, size_t N>
inline float StepPreset(const T (&list)[N], float value, int dir) {
    int best = 0;
    for (int i = 1; i < int(N); ++i)
        if (std::fabs(float(list[i]) - value) < std::fabs(float(list[best]) - value)) best = i;
    int i = best + dir;
    i = i < 0 ? 0 : i >= int(N) ? int(N) - 1 : i;
    return float(list[i]);
}

}  // namespace b2s::panel
