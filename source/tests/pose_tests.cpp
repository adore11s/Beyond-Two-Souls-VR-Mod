// PORT-06 desk tests for src/posemath.h. Run: build\RelWithDebInfo\pose_tests.exe (or ctest).
#include <cmath>
#include <cstdio>

#include "../src/posemath.h"

using namespace b2s::pose;

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
bool Near(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

Quat AxisAngle(Vec3 axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

Quat QMul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Independent cross-check (playbook A1.2): rotate by q v q*, no matrix involved.
Vec3 QRotate(const Quat& q, const Vec3& v) {
    const Quat p{v.x, v.y, v.z, 0};
    const Quat c{-q.x, -q.y, -q.z, q.w};
    const Quat r = QMul(QMul(q, p), c);
    return {r.x, r.y, r.z};
}

}  // namespace

int main() {
    const float deg = 3.14159265f / 180.f;

    // Quaternion normalisation and zero rejection.
    {
        Quat q{0, 0, 0, 0};
        Check(!Normalize(q), "zero quaternion is rejected");
        Quat n{0, NAN, 0, 1};
        Check(!Normalize(n), "non-finite quaternion is rejected");
        Quat s{0, 2, 0, 2};
        Check(Normalize(s) && Near(s.y, 0.70710677f) && Near(s.w, 0.70710677f), "scaled quaternion normalises");
    }

    // Quaternion -> matrix agrees with q v q* for an arbitrary rotation (A1.1/A1.2).
    {
        Quat q = QMul(AxisAngle({0, 1, 0}, 37 * deg), AxisAngle({1, 0, 0}, -21 * deg));
        q = QMul(q, AxisAngle({0, 0, 1}, 12 * deg));
        const Mat4 m = FromPose(q, {0, 0, 0});
        const Vec3 v{0.3f, -0.7f, 1.9f};
        Check(Near(TransformPoint(v, m), QRotate(q, v)), "FromPose matches quaternion rotation (independent check)");
    }

    // Basis: right-handed, Y up, -Z forward; positive yaw turns left.
    {
        const Mat4 m = FromPose(AxisAngle({0, 1, 0}, 90 * deg), {0, 0, 0});
        Check(Near(TransformPoint({0, 0, -1}, m), {-1, 0, 0}), "+90 deg yaw turns forward (-Z) to the left (-X)");
        Check(Near(Yaw(AxisAngle({0, 1, 0}, 30 * deg)), 30 * deg), "Yaw() reads a pure yaw back");
    }

    // Rigid inverse.
    {
        Quat q = AxisAngle({0.267f, 0.535f, 0.802f}, 1.1f);
        Normalize(q);  // the axis is only approximately unit length
        const Mat4 m = FromPose(q, {1.5f, -2.f, 0.25f});
        const Mat4 r = Mul(m, RigidInverse(m));
        bool ok = true;
        for (int i = 0; i < 16; ++i) ok &= Near(r.m[i], (i % 5 == 0) ? 1.f : 0.f);
        Check(ok, "M * RigidInverse(M) = I");
    }

    // Yaw-only recenter preserves pitch and roll, and removes heading and position.
    {
        const Quat head = QMul(AxisAngle({0, 1, 0}, 50 * deg), AxisAngle({1, 0, 0}, 20 * deg));
        const Vec3 pos{0.4f, 1.2f, -0.3f};
        const Mat4 refInv = RigidInverse(FromPose(YawQuat(Yaw(head)), pos));
        const Mat4 rel = Mul(FromPose(head, pos), refInv);
        const Vec3 fwd = TransformPoint({0, 0, -1}, rel);
        const Vec3 origin = TransformPoint({0, 0, 0}, rel);
        Check(Near(fwd.x, 0.f) && Near(fwd.y, std::sin(20 * deg)), "recenter removes yaw, keeps 20 deg pitch");
        Check(Near(origin, {0, 0, 0}), "recenter removes position");
    }

    // Head yaw is independent of the game camera heading: the eye offset only depends on the
    // head relative to the reference, whatever the engine view V is.
    {
        const Mat4 refInv = Identity();
        const Mat4 off = EyeViewOffset(AxisAngle({0, 1, 0}, 15 * deg), {0, 0, 0}, refInv);
        const Mat4 camA = RigidInverse(FromPose(AxisAngle({0, 1, 0}, 0.f), {0, 0, 0}));
        const Mat4 camB = RigidInverse(FromPose(AxisAngle({0, 1, 0}, 90 * deg), {0, 0, 0}));
        // Looking straight ahead in eye space maps to camera heading + 15 deg in both cases.
        const Mat4 worldA = RigidInverse(Mul(camA, off)), worldB = RigidInverse(Mul(camB, off));
        const Vec3 fa = TransformPoint({0, 0, -1}, worldA), fb = TransformPoint({0, 0, -1}, worldB);
        Check(Near(std::atan2(-fa.x, -fa.z), 15 * deg) && Near(std::atan2(-fb.x, -fb.z), 105 * deg),
              "head yaw composes on any camera heading without double application");
    }

    // Eye transform conjugation: the right eye (+X 32 mm) sees a centred point 32 mm to its left.
    {
        const Mat4 off = EyeViewOffset({0, 0, 0, 1}, {0.032f, 0, 0}, Identity());
        const Vec3 p = TransformPoint({0, 0, -2}, off);
        Check(Near(p, {-0.032f, 0, -2}), "right-eye offset shifts view-space points left by half the IPD");
    }

    // Settle latch (A2.5): needs 8 stable tracked samples; a jump or lost tracking resets it.
    {
        SettleLatch l;
        const Quat q{0, 0, 0, 1};
        int acceptedAt = -1;
        for (int i = 0; i < 20 && acceptedAt < 0; ++i)
            if (l.Feed(q, {0, 1.6f, 0}, true, true)) acceptedAt = i;
        Check(acceptedAt == SettleLatch::kNeeded, "stable pose accepted after kNeeded agreeing samples");
        l.Reset();
        bool early = false;
        for (int i = 0; i < 6; ++i) early |= l.Feed(q, {0, 1.6f, 0}, true, true);
        early |= l.Feed(q, {0, -1.2f, 0}, true, true);  // 2.8 m jump
        for (int i = 0; i < 6; ++i) early |= l.Feed(q, {0, -1.2f, 0}, true, true);
        Check(!early, "a jump restarts the count instead of being accepted");
        l.Reset();
        bool untracked = false;
        for (int i = 0; i < 20; ++i) untracked |= l.Feed(q, {0, 0, 0}, false, true);
        Check(!untracked, "untracked position is never accepted");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
