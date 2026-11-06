// VR settings panel (src/panel_math.h): B2S_ELEVEN_ROW_V2 geometry (GOW_STYLE_TEN_ROW_V4 + one row), ray hits, the trigger rule and the
// size / turning steppers (playbook chapter 2).
#include "panel_math.h"

#include <cmath>
#include <cstdio>

using namespace b2s::panel;
using b2s::pose::Vec3;

static bool Near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); ++fails; } else ++checks; } while (0)

int main() {
    int fails = 0, checks = 0;
    // The profile: 1000 x 1690 px, 1.2 m x 2.028 m; the last label at 1402, its buttons end at 1518, then the
    // three footer lines 1540 .. 1668.
    static_assert(kWidthPx == 1000 && kHeightPx == 1690 && kRows == 11);
    CHECK(Near(kHeightM, 2.028f));
    static_assert(kTop + (kRows - 1) * kRow == 1402 && kTop + (kRows - 1) * kRow + kButtonY + kButtonH == 1518);
    static_assert(kHeightPx - 150 == 1540 && kHeightPx - 22 == 1668);

    // Placement: right of a 2.2 m screen 1.8 m ahead, the gap 0.15 m, turned toward the head.
    const Placement p = Place({0.f, 0.f, -1.8f}, 2.2f, {0.f, 0.f, 0.f});
    CHECK(Near(p.center.x, 1.1f + 0.15f + 0.6f) && Near(p.center.z, -1.8f));
    CHECK(p.yaw < 0.f);  // its front turned left, toward the head
    // A ray from the head at the panel's centre hits the canvas middle.
    Vec3 d{p.center.x, p.center.y, p.center.z};
    const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    d = {d.x / n, d.y / n, d.z / n};
    float px = 0.f, py = 0.f;
    Vec3 q{};
    CHECK(Hit(p, {0.f, 0.f, 0.f}, d, &px, &py, &q) && Near(px, 500.f, 1.f) && Near(py, 845.f, 1.f));
    // From behind (the ray leaving through its back) and pointing away: no hit.
    CHECK(!Hit(p, {0.f, 0.f, 0.f}, {-d.x, -d.y, -d.z}, &px, &py, &q));
    // Above the panel: no hit.
    Vec3 up{p.center.x, p.center.y + 1.5f, p.center.z};
    const float un = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    CHECK(!Hit(p, {0.f, 0.f, 0.f}, {up.x / un, up.y / un, up.z / un}, &px, &py, &q));

    // Trigger: one click per pull; holding does not repeat; re-armed only below 0.35.
    TriggerClick t;
    CHECK(!t.Feed(0.5f) && t.Feed(0.8f) && !t.Feed(0.9f) && !t.Feed(0.5f) && !t.Feed(0.8f));
    CHECK(!t.Feed(0.2f) && t.Feed(0.8f));

    // Picture size: 1440 .. 2816, Auto one step above when known; '+' at Auto and '-' at the game's size stay.
    CHECK(StepSize(2048, 1, 0) == 2304 && StepSize(2048, -1, 0) == 1728);
    CHECK(StepSize(2816, 1, 0) == 2816 && StepSize(2816, 1, 2112) == kAuto);
    CHECK(StepSize(kAuto, 1, 2112) == kAuto && StepSize(kAuto, -1, 2112) == 2048);
    CHECK(StepSize(kAuto, -1, 3200) == 2816);
    CHECK(StepSize(kGame, 1, 0) == 1440 && StepSize(kGame, -1, 0) == kGame && StepSize(1440, -1, 0) == 1440);
    CHECK(StepSize(2000, 1, 0) == 2304);  // an ini size between presets steps from the nearest one

    // Turning presets (GoW): 20 .. 100 % and 15 .. 90 deg, clamped at both ends.
    CHECK(Near(StepPreset(kSpeeds, 1.f, 1), 1.f) && Near(StepPreset(kSpeeds, 1.f, -1), 0.9f));
    CHECK(Near(StepPreset(kSpeeds, 0.2f, -1), 0.2f));
    CHECK(Near(StepPreset(kSnaps, 45.f, 1), 60.f) && Near(StepPreset(kSnaps, 90.f, 1), 90.f) &&
          Near(StepPreset(kSnaps, 15.f, -1), 15.f));

    std::printf("PANEL CHECKS: %d passed, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
