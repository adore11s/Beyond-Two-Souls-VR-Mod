#include "vrinput_math.h"

#include <cmath>
#include <cstdio>

using namespace b2s::vrinput;
using namespace b2s::pose;
using namespace b2s::xr;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); ++fails; } else ++checks; } while (0)

int main() {
    int fails = 0, checks = 0;
    Sample neutral{};
    neutral.focused = true;
    LegacyState n = Map(neutral, true);
    CHECK(Near(n.moveX, 0.f) && Near(n.moveY, 0.f) && n.buttons == 0);

    Sample stale{};
    stale.focused = true;
    stale.fresh = false;
    stale.leftX = 1.f;
    CHECK(Near(Map(stale, true).moveX, 0.f));

    Sample forward{};
    forward.focused = true;
    forward.fresh = true;
    forward.leftY = 1.f;
    forward.headYaw = 1.57079632679f;
    LegacyState moved = Map(forward, true);
    CHECK(Near(moved.moveX, -1.f) && Near(moved.moveY, 0.f));

    Sample buttons{};
    buttons.focused = buttons.fresh = true;
    buttons.leftA = buttons.rightB = buttons.rightMenu = true;
    buttons.rightX = .7f;
    LegacyState gameplay = Map(buttons, true);
    CHECK((gameplay.buttons & (kLeftA | kRightB | kRightMenu)) ==
           (kLeftA | kRightB | kRightMenu));
    CHECK(Near(gameplay.viewX, .7f));  // prompts need the right stick in gameplay too
    LegacyState menu = Map(buttons, false);
    CHECK(Near(menu.viewX, .7f) && Near(menu.moveX, 0.f));
    Sample axes{}; axes.focused = axes.fresh = true; axes.leftX = .4f; axes.leftY = -.6f;
    axes.headYaw = 1.57079632679f; LegacyState menuAxes = Map(axes, false);
    CHECK(Near(menuAxes.moveX, .4f) && Near(menuAxes.moveY, -.6f));
    b2s::xr::FrameInput frame{}; frame.serial = 1; frame.tickMs = 100; frame.focused = true;
    frame.headRel = b2s::pose::FromPose(b2s::pose::YawQuat(1.57079632679f), {});
    frame.hand[0].active = frame.hand[0].gripValid = true;
    frame.hand[0].stick[0] = 2.f; frame.hand[0].stick[1] = -2.f; frame.hand[0].trigger = .75f;
    frame.hand[0].stickClick = true; frame.hand[1].active = frame.hand[1].gripValid = true;
    frame.hand[1].squeeze = .8f; frame.hand[1].a = true;
    Sample mapped = SampleFromFrame(frame, 150);
    CHECK(mapped.focused && Near(mapped.leftX, 1.f) && Near(mapped.leftY, -1.f) &&
           Near(mapped.headYaw, 1.57079632679f) && mapped.leftA == false);
    LegacyState mappedState = Map(mapped, true);
    CHECK((mappedState.buttons & (kLeftStickClick | kRightSqueeze |
                                  kRightA | kLeftTriggerButton)) ==
           (kLeftStickClick | kRightSqueeze | kRightA | kLeftTriggerButton));
    CHECK((mappedState.buttons & kLeftGrip) == 0);
    frame.hand[0].squeeze = .8f;
    mapped = SampleFromFrame(frame, 150);
    CHECK((Map(mapped, true).buttons & kLeftGrip) != 0);
    frame.hand[0].active = frame.hand[0].gripValid = false;
    CHECK((Map(SampleFromFrame(frame, 150), true).buttons & (kLeftGrip | kLeftSqueeze)) == 0);
    Sample dpad{}; dpad.focused = dpad.fresh = true; dpad.leftY = 1.f;
    LegacyState dpadState = Map(dpad, false);
    CHECK((dpadState.buttons & (kDpadUp | kDpadDown | kDpadLeft | kDpadRight)) == 0);
    dpad.leftSqueeze = true; dpadState = Map(dpad, false);
    CHECK((dpadState.buttons & kDpadUp) != 0 && (dpadState.buttons & kDpadDown) == 0);
    dpad.leftY = -1.f; dpad.leftX = -1.f;
    dpadState = Map(dpad, false);
    CHECK((dpadState.buttons & (kDpadDown | kDpadLeft)) == (kDpadDown | kDpadLeft));
    frame.headRel.m[0] = NAN;
    CHECK(!SampleFromFrame(frame, 150).focused);
    frame.headRel.m[0] = 1.f; frame.tickMs = 0;
    CHECK(!SampleFromFrame(frame, 300).focused);
    Sample nanYaw = axes; nanYaw.headYaw = NAN;
    CHECK(Map(nanYaw, true).moveX == 0.f && Map(nanYaw, true).moveY == 0.f);
    std::printf("VRINPUT CHECKS: %d passed, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
