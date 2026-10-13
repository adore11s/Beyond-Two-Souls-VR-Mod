#include <cmath>
#include <cstdio>

#include "../src/xr.h"
#include "../src/xr_recenter.h"

using namespace b2s;
namespace {
int failed = 0;
void Check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failed += !ok;
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}

int main() {
    xr::FrameInput in;
    Check(!xr::FrameInputUsable(in, 1000), "empty frame cannot drive controls");
    for (const auto& h : in.hand) {
        Check(!h.active && !h.gripValid && !h.aimValid && h.trigger == 0.f && h.squeeze == 0.f &&
                  h.stick[0] == 0.f && h.stick[1] == 0.f && !h.a && !h.b && !h.menu && !h.stickClick,
              "unavailable controller starts neutral");
        Check(Near(h.gripRel.m[0], 1.f) && Near(h.gripRel.m[5], 1.f) && Near(h.gripRel.m[10], 1.f) &&
                  Near(h.gripRel.m[15], 1.f) && Near(h.gripRel.m[12], 0.f),
              "unavailable grip has a safe identity matrix");
    }
    in.serial = 1;
    in.tickMs = 1000;
    in.focused = true;
    Check(xr::FrameInputUsable(in, 1000), "focused current frame accepted");
    Check(xr::FrameInputUsable(in, 1250), "250 ms age accepted");
    Check(!xr::FrameInputUsable(in, 1251), "stale frame rejected");
    Check(!xr::FrameInputUsable(in, 999), "future timestamp rejected");
    in.focused = false;
    Check(!xr::FrameInputUsable(in, 1000), "focus loss neutralizes controls");
    in.focused = true;
    in.serial = 0;
    Check(!xr::FrameInputUsable(in, 1000), "missing frame serial rejected");

    // Reference at +1 X and controller at +2 X: the relative hand is +1 X.
    // A +90-degree world base at (10,3,20) maps it to (10,3,19).
    const auto reference = pose::FromPose({0, 0, 0, 1}, {1, 0, 0});
    const auto grip = pose::FromPose({0, 0, 0, 1}, {2, 0, 0});
    const auto base = pose::FromPose(pose::YawQuat(1.57079632679f), {10, 3, 20});
    const auto world = pose::Mul(pose::Mul(grip, pose::RigidInverse(reference)), base);
    Check(Near(world.m[12], 10.f) && Near(world.m[13], 3.f) && Near(world.m[14], 19.f),
          "grip uses eye reference then selected world base");
    xr::ReferenceChangeSchedule schedule;
    schedule.Add(7, 1, 200);
    Check(!schedule.ConsumeReady(7, 1, 199), "reference change waits before changeTime");
    Check(schedule.ConsumeReady(7, 1, 200), "reference change fires at changeTime once");
    Check(!schedule.ConsumeReady(7, 1, 201), "reference change is consumed once");
    schedule.Add(7, 1, 300); schedule.Add(7, 1, 250); schedule.Add(7, 1, 275);
    Check(schedule.ConsumeReady(7, 1, 260), "staggered changes preserve ready event");
    Check(schedule.ConsumeReady(7, 1, 300), "out-of-order changes are not lost");
    for (int i=0; i<9; ++i) schedule.Add(7, 1, 400+i);
    Check(schedule.ConsumeReady(7, 1, 400), "earliest pending change survives nine-event burst");
    schedule.Reset();
    schedule.Add(8, 1, 500); schedule.Add(7, 2, 500);
    Check(!schedule.ConsumeReady(7, 1, 500), "wrong session or space is not consumed");
    schedule.Reset();
    Check(!schedule.ConsumeReady(7, 1, 1000), "reset clears pending changes");
    std::printf("%d failed\n", failed);
    return failed ? 1 : 0;
}
