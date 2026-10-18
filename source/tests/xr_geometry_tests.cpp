// Desk tests for src/xr_geometry.h (session-log headset geometry checks).
// Run: build\RelWithDebInfo\xr_geometry_tests.exe (or ctest).
#include <cmath>
#include <cstdio>

#include "../src/xr_geometry.h"

using namespace b2s;
using namespace b2s::xrgeom;

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

constexpr float kDeg = 0.0174532925f;

pose::Quat AxisAngle(pose::Vec3 axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

// A plain headset: parallel eyes 64 mm apart, mirrored asymmetric frusta (outer side wider).
Eye LeftEye() { return {{0, 0, 0, 1}, {-0.032f, 0, 0}, -54 * kDeg, 44 * kDeg, 48 * kDeg, -52 * kDeg}; }
Eye RightEye() { return {{0, 0, 0, 1}, {0.032f, 0, 0}, -44 * kDeg, 54 * kDeg, 48 * kDeg, -52 * kDeg}; }

}  // namespace

int main() {
    {
        const Report r = Evaluate(LeftEye(), RightEye());
        Check(r.flags == 0, "plain headset: no findings");
        Check(Near(r.ipdMm, 64.f, 0.01f), "plain headset: ipd 64 mm");
        Check(Near(r.cantDeg, 0.f), "plain headset: no cant");
    }
    {
        const Report r = Evaluate(RightEye(), LeftEye());
        Check((r.flags & kSwapped) != 0, "eyes given right-then-left: swapped");
    }
    {
        Eye l = LeftEye(), rt = RightEye();
        l.p.x = -0.015f;
        rt.p.x = 0.015f;
        Check((Evaluate(l, rt).flags & kIpdOutOfRange) != 0, "30 mm apart: ipd out of range");
    }
    {
        Eye rt = RightEye();
        rt.p.y = 0.004f;
        const Report r = Evaluate(LeftEye(), rt);
        Check((r.flags & kVertical) != 0 && Near(r.dyMm, 4.f, 0.01f), "right eye 4 mm higher: vertical offset");
    }
    {
        Eye rt = RightEye();
        rt.p.z = -0.003f;
        Check((Evaluate(LeftEye(), rt).flags & kDepth) != 0, "right eye 3 mm forward: depth offset");
    }
    {
        // Canted displays: each eye turned 5 degrees outwards (left eye yaws left = positive).
        Eye l = LeftEye(), rt = RightEye();
        l.q = AxisAngle({0, 1, 0}, 5 * kDeg);
        rt.q = AxisAngle({0, 1, 0}, -5 * kDeg);
        const Report r = Evaluate(l, rt);
        Check((r.flags & kCanted) != 0 && (r.flags & kRotated) != 0, "outward-canted eyes: canted + rotated");
        Check(Near(r.cantDeg, 10.f, 0.01f), "outward-canted eyes: cant 10 deg");
        Check(Near(r.eye[0].yaw, 5.f, 0.01f) && Near(r.eye[1].yaw, -5.f, 0.01f), "outward-canted eyes: yaw +5 / -5");
    }
    {
        // Both eyes pitched the same way: not canted, but not looking along the head either.
        Eye l = LeftEye(), rt = RightEye();
        l.q = rt.q = AxisAngle({1, 0, 0}, 2 * kDeg);
        const Report r = Evaluate(l, rt);
        Check((r.flags & kCanted) == 0 && (r.flags & kRotated) != 0, "both eyes pitched: rotated, not canted");
        Check(Near(r.eye[0].pitch, 2.f, 0.01f), "both eyes pitched: pitch +2");
    }
    {
        Eye l = LeftEye();
        l.q = AxisAngle({0, 0, 1}, 3 * kDeg);
        Check(Near(Evaluate(l, RightEye()).eye[0].roll, 3.f, 0.01f), "left eye rolled: roll +3");
    }
    {
        Eye l = LeftEye();
        l.angleLeft = 50 * kDeg;  // left edge right of the right edge (44 deg)
        Check((Evaluate(l, RightEye()).flags & kFovInvalid) != 0, "inverted frustum: fov invalid");
        Eye rt = RightEye();
        rt.angleUp = 95 * kDeg;
        Check((Evaluate(LeftEye(), rt).flags & kFovInvalid) != 0, "95 deg half angle: fov invalid");
    }
    {
        // The game's view matrices: parallel eyes give (+IPD, 0, 0) for the right eye in the left view.
        const pose::Mat4 camL = pose::FromPose(AxisAngle({0, 1, 0}, 30 * kDeg), {1.f, 1.6f, 2.f});
        const pose::Mat4 offR = pose::FromPose({0, 0, 0, 1}, {0.064f, 0, 0});
        const pose::Mat4 camR = pose::Mul(offR, camL);  // 64 mm along the left camera's +X
        const pose::Vec3 d = RightEyeInLeftView(pose::RigidInverse(camL), pose::RigidInverse(camR));
        Check(Near(d.x, 0.064f, 1e-4f) && Near(d.y, 0.f, 1e-4f) && Near(d.z, 0.f, 1e-4f),
              "game views: right eye at +64 mm in the left view");
        const pose::Vec3 s = RightEyeInLeftView(pose::RigidInverse(camR), pose::RigidInverse(camL));
        Check(s.x < 0.f, "game views swapped: right eye at negative x");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
