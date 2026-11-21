// EXP-067 desk tests for the drawn fingers (src/fingers.h, playbook 14.6 / 14.7 / 14.10 / 14.12) on a whole rig from
// the game files (IW_JHOB, tools/rig_fixture.py): rest shape, own-axis hinges (thumb included), amounts from touch,
// rate limit, segment lengths, closing toward the palm, smoothness, no fingertip through the palm, held-item blend.
#include "../src/fingers.h"
#include "../src/bones_crc.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include "rigmap_fixture.inc"

using namespace b2s;
using fingers::Mat4;
using fingers::Vec3;
namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

uint32_t Find(const char* name) {
    const uint32_t c = bones::Crc32(name);
    for (uint32_t i = 0; i < kFxCount; ++i)
        if (kFxCrc[i] == c) return i;
    return fingers::kNone;
}
// The fixture holds the rig's rest WORLDS (model space, the bind pose composed): the rest locals are world x parent^-1.
std::vector<Mat4> RestLocals() {
    std::vector<Mat4> w(kFxCount), l(kFxCount);
    for (uint32_t i = 0; i < kFxCount; ++i)
        for (int k = 0; k < 16; ++k) w[i].m[k] = kFxRest[i * 16 + k];
    for (uint32_t i = 0; i < kFxCount; ++i) {
        l[i] = w[i];
        if (kFxParents[i] < kFxCount) fingers::LocalOf(w[i], w[kFxParents[i]], l[i]);
    }
    return l;
}
std::vector<Mat4> Worlds(const std::vector<Mat4>& local, const Mat4& top) {
    std::vector<Mat4> w(kFxCount + 1);
    std::vector<bool> done(kFxCount, false);
    for (uint32_t pass = 0; pass < kFxCount; ++pass)
        for (uint32_t i = 0; i < kFxCount; ++i) {
            if (done[i]) continue;
            const uint32_t p = kFxParents[i];
            if (p < kFxCount && !done[p]) continue;
            w[i] = pose::Mul(local[i], p < kFxCount ? w[p] : top);
            done[i] = true;
        }
    return w;
}
fingers::Hand MakeHand(bool right) {
    static const char* const kName[fingers::kCount] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    uint32_t chains[fingers::kCount][4];
    for (int f = 0; f < fingers::kCount; ++f)
        for (int k = 0; k < 4; ++k) {
            char name[48];
            if (f == fingers::kThumb) std::snprintf(name, sizeof name, "PX_%c_HandThumb%d", right ? 'R' : 'L', k + 1);
            else std::snprintf(name, sizeof name, "PX_%c_Hand%s%d", right ? 'R' : 'L', kName[f], k);
            chains[f][k] = (f == fingers::kThumb && k == 3) ? fingers::kNone : Find(name);
        }
    const uint32_t root = Find(right ? "LC_R_Hand" : "LC_L_Hand");
    return fingers::MakeHand(kFxParents, kFxCount, root, chains, right);
}
// A moved, turned body (the hand far from the rig's rest place): the rules must not depend on where the hand is.
Mat4 Top() {
    const float a = 0.7f;
    Mat4 m = pose::FromPose({0.f, std::sin(a * 0.5f), 0.f, std::cos(a * 0.5f)}, {3.f, -1.f, 12.f});
    return m;
}
// A fingertip past the last bone (no tip bone on the game's rigs), its +-X sign from the rig's rest pose.
std::vector<Mat4> g_restWorld;
Vec3 Tip(const fingers::Hand& h, const Mat4* w, int f) {
    const uint32_t last = h.bone[f][h.bones[f] - 1];
    const uint32_t prev = h.bone[f][h.bones[f] - 2];
    const float len = fp::Len(fp::Sub(fingers::Pos(w[last]), fingers::Pos(w[prev])));
    const float along = fingers::AlongSign(g_restWorld[last], g_restWorld[prev]);
    return fp::Add(fingers::Pos(w[last]), fp::Mul(fingers::BoneDirection(w[last], nullptr, along), 0.8f * len));
}

void TestAmounts() {
    float a[fingers::kCount];
    fingers::Input in;
    Check(fingers::Targets(in, a) == fingers::Source::Resting, "no touch reported: resting bases");
    Check(Near(a[fingers::kMiddle], 0.45f) && Near(a[fingers::kIndex], 0.25f) && Near(a[fingers::kThumb], 0.35f),
          "resting bases 0.45 / 0.25 / 0.35");
    in.squeeze = 1.f;
    fingers::Targets(in, a);
    Check(Near(a[fingers::kRing], 1.f) && Near(a[fingers::kThumb], 0.8f), "resting: full grip closes, thumb 0.8");
    // Touch: point = grip held, index lifted.
    in = {};
    in.touchSeen = true;
    in.squeeze = 1.f;
    in.thumbTouch = true;
    Check(fingers::Targets(in, a) == fingers::Source::Touch, "touch reported: touch poses");
    Check(Near(a[fingers::kIndex], 0.f) && Near(a[fingers::kMiddle], 1.f) && Near(a[fingers::kThumb], 0.9f), "point");
    // Owner 2026-11-21 (playbook 14.12): no thumb-touch pose. Lifting the thumb while gripping (the old thumbs up)
    // leaves it closed with the grip.
    in.thumbTouch = false;
    in.triggerTouch = true;
    fingers::Targets(in, a);
    Check(Near(a[fingers::kThumb], 0.9f) && Near(a[fingers::kIndex], 0.35f) && Near(a[fingers::kLittle], 1.f),
          "thumb lifted while gripping: no thumbs up, the thumb follows the grip");
    in.thumbTouch = true;
    in.trigger = 1.f;
    fingers::Targets(in, a);
    Check(Near(a[fingers::kIndex], 1.f) && Near(a[fingers::kThumb], 0.9f) && Near(a[fingers::kRing], 1.f), "fist");
    in = {};
    in.touchSeen = true;
    in.squeeze = 1.f;
    fingers::Targets(in, a);
    Check(Near(a[fingers::kIndex], 0.f) && Near(a[fingers::kThumb], 0.9f) && Near(a[fingers::kMiddle], 1.f),
          "index and thumb lifted: no gun, a point with the thumb closed by the grip");
    in = {};
    in.touchSeen = true;
    in.thumbTouch = true;  // open hand, the real thumb resting on the stick: the old thumb-down pose
    in.triggerTouch = true;
    fingers::Targets(in, a);
    Check(Near(a[fingers::kThumb], 0.f), "thumb resting on the stick with an open hand: the thumb stays open");
    in = {};
    in.touchSeen = true;
    in.trigger = 0.5f;  // pulling without a touch report still curls the index
    fingers::Targets(in, a);
    Check(a[fingers::kIndex] > 0.35f, "a pulled trigger counts as touched");
    // Rate limit: 12 units per second.
    float cur[fingers::kCount] = {0, 0, 0, 0, 0}, tgt[fingers::kCount] = {1, 1, 1, 1, 0};
    fingers::Step(cur, tgt, 1.f / 90.f);
    Check(Near(cur[0], 12.f / 90.f), "12 units per second");
    for (int i = 0; i < 20; ++i) fingers::Step(cur, tgt, 1.f / 90.f);
    Check(Near(cur[0], 1.f) && Near(cur[4], 0.f), "reaches the target, never overshoots");
    Check(Near(fingers::JointAngle(false, 1, 1.f), 95.f * 0.017453293f) &&
              Near(fingers::JointAngle(true, 0, 0.f), 4.f * 0.017453293f),
          "angle = 4 + (limit - 4) x amount");
}

void TestRig(bool right) {
    const char* side = right ? "right" : "left";
    char why[160];
    const fingers::Hand h = MakeHand(right);
    std::snprintf(why, sizeof why, "%s hand chains found on the game rig", side);
    Check(h.valid && h.bones[fingers::kThumb] == 3 && h.bones[fingers::kIndex] == 4 && h.bones[fingers::kRing] == 4, why);
    if (!h.valid) return;
    const auto local = RestLocals();
    std::vector<bool> ok(kFxCount);
    for (uint32_t i = 0; i < kFxCount; ++i) ok[i] = fingers::UsableRest(local[i]);
    const auto rest = Worlds(local, Top());
    g_restWorld = rest;
    // The animated pose: rest with the fingers spread / twisted by the "animation" (every chain bone turned 20 deg about
    // its row 1): the rest shape must take them back.
    auto animated = rest;
    {
        std::vector<Mat4> twisted = local;
        for (int f = 0; f < fingers::kCount; ++f)
            for (int k = 0; k < h.bones[f]; ++k) {
                Mat4& m = twisted[h.bone[f][k]];
                fingers::TurnRows(m, fp::Norm(fingers::Row(m, 1)), 0.35f);
            }
        animated = Worlds(twisted, Top());
    }
    float open[fingers::kCount] = {0, 0, 0, 0, 0}, fist[fingers::kCount] = {1, 1, 1, 1, 1};
    // 1. Rest shape at amount 0 with rest shape on: every chain bone at its rest local, bent only by the relaxed 4 deg.
    auto w = animated;
    fingers::Report rep;
    std::snprintf(why, sizeof why, "%s hand pose succeeds", side);
    bool okArr[kFxCount];
    for (uint32_t i = 0; i < kFxCount; ++i) okArr[i] = ok[i];
    w = animated;
    Check(fingers::Pose(h, local.data(), okArr, open, true, w.data(), &rep), why);
    std::snprintf(why, sizeof why, "%s hand: every joint hinges about its own local Z (no palm-built axis)", side);
    Check(rep.palmAxis == 0 && rep.ownAxis == 15 && rep.restUsed == 19 && !rep.palmFlipped, why);
    float worstRest = 0.f;
    for (int f = 0; f < fingers::kCount; ++f) {
        const uint32_t b = h.bone[f][h.firstCurl[f]];
        worstRest = std::max(worstRest, fp::Len(fp::Sub(fingers::Pos(w[b]), fingers::Pos(rest[b]))));
    }
    std::snprintf(why, sizeof why, "%s hand: the animation's spread / twist is gone (knuckles at rest, %.5f m)", side,
                  worstRest);
    Check(worstRest < 1e-4f, why);
    // 2. Segment lengths kept and fingers closing toward the palm, smoothly, for every amount.
    const fingers::Palm palm = fingers::PalmFrame(h, rest.data());
    std::snprintf(why, sizeof why, "%s hand palm frame (thumb on the n side as measured)", side);
    Check(palm.ok && !palm.flipped, why);
    for (int f = 0; f < fingers::kCount; ++f) {
        const bool thumb = f == fingers::kThumb;
        const Vec3 toward = thumb ? fp::Norm(fp::Sub(palm.n, palm.s)) : palm.n;
        std::vector<Vec3> tips;
        float lastSide = -1e9f;
        bool monotonic = true, lengths = true, plane = true;
        for (int step = 0; step <= 20; ++step) {
            float amount[fingers::kCount] = {0, 0, 0, 0, 0};
            amount[f] = step / 20.f;
            auto p = animated;
            fingers::Pose(h, local.data(), okArr, amount, true, p.data());
            for (int k = 1; k < h.bones[f]; ++k) {
                const uint32_t a = h.bone[f][k - 1], b = h.bone[f][k];
                const float l0 = fp::Len(fp::Sub(fingers::Pos(rest[b]), fingers::Pos(rest[a])));
                const float l1 = fp::Len(fp::Sub(fingers::Pos(p[b]), fingers::Pos(p[a])));
                lengths &= std::fabs(l0 - l1) < 1e-5f;
            }
            // Rows keep their lengths (no shrinking).
            for (int k = 0; k < h.bones[f]; ++k)
                for (int r = 0; r < 3; ++r) {
                    const uint32_t b = h.bone[f][k];
                    lengths &= std::fabs(fp::Len(fingers::Row(p[b], r)) - fp::Len(fingers::Row(rest[b], r))) < 1e-4f;
                }
            // Hinge plane: every bone of the finger keeps its local Z parallel to the first joint's (a pure hinge),
            // and the chain's positions keep their offset along that axis (the thumb nail does not sweep sideways).
            const uint32_t first = h.bone[f][h.firstCurl[f]];
            const Vec3 axis0 = fp::Norm(fingers::Row(rest[first], 2));
            for (int k = h.firstCurl[f]; k < h.bones[f]; ++k) {
                const uint32_t b = h.bone[f][k];
                plane &= std::fabs(std::fabs(fp::Dot(fp::Norm(fingers::Row(p[b], 2)), axis0)) - 1.f) < 1e-4f;
                const float d0 = fp::Dot(fp::Sub(fingers::Pos(rest[b]), fingers::Pos(rest[first])), axis0);
                const float d1 = fp::Dot(fp::Sub(fingers::Pos(p[b]), fingers::Pos(p[first])), axis0);
                plane &= std::fabs(d0 - d1) < 1e-4f;
            }
            const Vec3 tip = Tip(h, p.data(), f);
            tips.push_back(tip);
            // The tip's angle about the first joint, from the finger's rest direction toward the palm side: it only
            // grows as the amount grows (a fist passes over the top and comes back down toward the palm, so the height
            // along n alone is not monotonic; the swept angle is).
            const Vec3 v = fp::Sub(tip, fingers::Pos(p[first]));
            const Vec3 d0 = fp::Norm(fp::Sub(fingers::Pos(rest[h.bone[f][h.firstCurl[f] + 1]]), fingers::Pos(rest[first])));
            const Vec3 up = fp::Norm(fp::Sub(toward, fp::Mul(d0, fp::Dot(toward, d0))));
            float sideOf = std::atan2(fp::Dot(v, up), fp::Dot(v, d0));
            while (step > 0 && sideOf < lastSide - 3.14159265f) sideOf += 6.2831853f;
            if (step > 0 && sideOf < lastSide - 1e-4f) monotonic = false;
            lastSide = sideOf;
        }
        float mean = 0.f, worst = 0.f;
        for (size_t i = 1; i < tips.size(); ++i) {
            const float d = fp::Len(fp::Sub(tips[i], tips[i - 1]));
            mean += d;
            worst = std::max(worst, d);
        }
        mean /= float(tips.size() - 1);
        std::snprintf(why, sizeof why, "%s finger %d: segment and row lengths kept", side, f);
        Check(lengths, why);
        std::snprintf(why, sizeof why, "%s finger %d: closes toward the palm without reversing", side, f);
        Check(monotonic, why);
        std::snprintf(why, sizeof why, "%s finger %d: smooth (worst step %.4f <= 1.6 x mean %.4f)", side, f, worst, mean);
        Check(worst <= 1.6f * mean, why);
        std::snprintf(why, sizeof why, "%s finger %d: stays in its hinge plane, not twisted", side, f);
        Check(plane, why);
    }
    // 3. A fist: no fingertip through the palm (every long fingertip in front of the palm plane, on the n side).
    w = animated;
    fingers::Pose(h, local.data(), okArr, fist, true, w.data());
    {
        const fingers::Palm pp = fingers::PalmFrame(h, w.data());
        const Vec3 wrist = fingers::Pos(w[h.root]);
        float worst = 1.f;
        for (int f = fingers::kIndex; f < fingers::kCount; ++f)
            worst = std::min(worst, fp::Dot(fp::Sub(Tip(h, w.data(), f), wrist), pp.n));
        std::snprintf(why, sizeof why, "%s fist: fingertips in front of the palm (nearest %.4f m)", side, worst);
        Check(worst > 0.f, why);
        const float tipToWrist = fp::Len(fp::Sub(Tip(h, w.data(), fingers::kMiddle), wrist));
        const float openTip = fp::Len(fp::Sub(Tip(h, rest.data(), fingers::kMiddle), fingers::Pos(rest[h.root])));
        std::snprintf(why, sizeof why, "%s fist: the middle tip comes back toward the wrist (%.3f m from %.3f m)", side,
                      tipToWrist, openTip);
        Check(tipToWrist < 0.7f * openTip, why);
    }
    // 4. The thumb's old palm-built axis (14.10: parent x (n - s)) against its own Z: logged as evidence.
    {
        const uint32_t t1 = h.bone[fingers::kThumb][0], t2 = h.bone[fingers::kThumb][1];
        const Vec3 parentDir = fp::Norm(fp::Sub(fingers::Pos(rest[t2]), fingers::Pos(rest[t1])));
        const Vec3 palmAxis = fp::Norm(fp::Cross(parentDir, fp::Sub(palm.n, palm.s)));
        const float deg = std::acos(std::fabs(fp::Dot(palmAxis, fp::Norm(fingers::Row(rest[t1], 2))))) * 57.29578f;
        std::printf("%s thumb: the palm-built hinge is %.1f deg off the bone's own Z\n", side, deg);
    }
    // 5. Held-item blend: weight 0 = the animation's hand exactly; weight 0.5 keeps every row's length.
    {
        auto driven = animated;
        fingers::Pose(h, local.data(), okArr, fist, true, driven.data());
        auto out = animated;
        Check(fingers::BlendHand(h, animated.data(), driven.data(), 0.f, out.data()), "blend weight 0");
        float d = 0.f;
        for (int f = 0; f < fingers::kCount; ++f)
            for (int k = 0; k < h.bones[f]; ++k)
                for (int m = 0; m < 16; ++m)
                    d = std::max(d, std::fabs(out[h.bone[f][k]].m[m] - animated[h.bone[f][k]].m[m]));
        std::snprintf(why, sizeof why, "%s blend 0 = the animation's hand (%.6f)", side, d);
        Check(d < 1e-4f, why);
        out = animated;
        fingers::BlendHand(h, animated.data(), driven.data(), 0.5f, out.data());
        bool rows = true;
        for (int f = 0; f < fingers::kCount; ++f)
            for (int k = 0; k < h.bones[f]; ++k)
                for (int r = 0; r < 3; ++r)
                    rows &= std::fabs(fp::Len(fingers::Row(out[h.bone[f][k]], r)) - 1.f) < 1e-3f;
        std::snprintf(why, sizeof why, "%s blend 0.5 keeps unit rows (no shrinking)", side);
        Check(rows, why);
        out = animated;
        fingers::BlendHand(h, animated.data(), driven.data(), 1.f, out.data());
        d = 0.f;
        for (int f = 0; f < fingers::kCount; ++f)
            for (int k = 0; k < h.bones[f]; ++k)
                for (int m = 0; m < 16; ++m)
                    d = std::max(d, std::fabs(out[h.bone[f][k]].m[m] - driven[h.bone[f][k]].m[m]));
        std::snprintf(why, sizeof why, "%s blend 1 = the driven fingers (%.6f)", side, d);
        Check(d < 1e-4f, why);
    }
    // 6. Rest shape off: the animation's locals (twisted) are kept, only the bend is added.
    {
        auto p = animated;
        fingers::Pose(h, local.data(), okArr, open, false, p.data(), &rep);
        Check(rep.restUsed == 0, "rest shape off uses the animation's locals");
        const uint32_t k1 = h.bone[fingers::kIndex][1];
        Check(fp::Len(fp::Sub(fingers::Pos(p[k1]), fingers::Pos(animated[k1]))) < 1e-4f, "knuckle stays animated");
    }
}

void TestUsableRest() {
    Mat4 m = pose::Identity();
    Check(fingers::UsableRest(m), "identity usable");
    m.m[3] = 0.5f;
    Check(!fingers::UsableRest(m), "w column must be 0 0 0 1");
    m = pose::Identity();
    m.m[0] = m.m[5] = m.m[10] = 0.2f;  // det 0.008
    Check(!fingers::UsableRest(m), "|det| below 0.05 refused");
    m = pose::Identity();
    m.m[12] = NAN;
    Check(!fingers::UsableRest(m), "non-finite refused");
}
}  // namespace

int main() {
    TestAmounts();
    TestUsableRest();
    TestRig(false);
    TestRig(true);
    if (failures) {
        std::printf("fingers_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("fingers_tests: all passed\n");
    return 0;
}
