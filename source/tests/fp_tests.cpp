// Desk tests for src/fpmode.h and src/bones_crc.h (F1 first person, EXP-031).
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../src/bones_crc.h"
#include "../src/fpmode.h"

using namespace b2s;
using namespace b2s::fp;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }
}  // namespace

int main() {
    const float deg = 3.14159265f / 180.f;
    // Classification truth table.
    Check(Classify(BehaviourClass::Gameplay, false, false) == Mode::Cinematic, "gameplay, F1 off -> cinematic");
    Check(Classify(BehaviourClass::Gameplay, true, false) == Mode::FirstPerson, "gameplay, F1 on -> first person");
    Check(Classify(BehaviourClass::Cutscene, true, false) == Mode::Cinematic, "cutscene, F1 only -> cinematic");
    Check(Classify(BehaviourClass::Cutscene, true, true) == Mode::FirstPerson, "cutscene, F1+F3 -> first person");
    Check(Classify(BehaviourClass::Cutscene, false, true) == Mode::FirstPerson,
          "cutscene, F3 alone -> first person");
    Check(Classify(BehaviourClass::NativeFirstPerson, true, true) == Mode::NativeFirstPerson,
          "Aiden/native first person is never overridden");
    Check(Classify(BehaviourClass::Unknown, true, true) == Mode::Cinematic, "unknown behaviour fails safe");

    // Yaw extraction ignores pitch and roll.
    {
        const pose::Quat q = {0.f, std::sin(40 * deg / 2), 0.f, std::cos(40 * deg / 2)};
        pose::Mat4 m = pose::FromPose(q, {1, 2, 3});
        Check(Near(YawOf(m), 40 * deg), "YawOf reads a pure yaw");
        const pose::Quat pitch = {std::sin(30 * deg / 2), 0.f, 0.f, std::cos(30 * deg / 2)};
        const pose::Mat4 pm = pose::Mul(pose::FromPose(pitch, {0, 0, 0}), m);
        Check(Near(YawOf(pm), 40 * deg, 1e-3f), "YawOf ignores head pitch");
    }

    // First-person base: eye point in front of/above the head, oriented by body yaw only.
    {
        const pose::Quat tilt = {std::sin(25 * deg / 2), 0.f, 0.f, std::cos(25 * deg / 2)};  // head pitched
        const pose::Mat4 head = pose::FromPose(tilt, {0.f, 1.5f, 0.f});
        const pose::Mat4 base = FirstPersonBase(head, 0.f, 0.09f, 0.06f);
        const pose::Vec3 eye = pose::TransformPoint({0, 0, 0}, base);
        Check(Near(eye.x, 0.f) && Near(eye.y, 1.56f) && Near(eye.z, -0.09f), "eye point = head + yaw-frame offset");
        const pose::Vec3 fwd = pose::TransformPoint({0, 0, -1}, base);
        Check(Near(fwd.y - eye.y, 0.f), "base has no pitch (HMD supplies it)");
        const pose::Mat4 turned = FirstPersonBase(head, 90 * deg, 0.09f, 0.06f);
        const pose::Vec3 e2 = pose::TransformPoint({0, 0, 0}, turned);
        Check(Near(e2.x, -0.09f) && Near(e2.z, 0.f), "eye offset follows body yaw (+90 deg -> -X)");
    }

    // Jump guard.
    Check(PlausibleStep({0, 1, 0}, {0.1f, 1, 0}, 2.f), "small step accepted");
    Check(!PlausibleStep({0, 1, 0}, {10, 1, 0}, 2.f), "teleport-sized jump rejected");
    Check(!PlausibleStep({0, 1, 0}, {NAN, 1, 0}, 2.f), "NaN rejected");

    // Key toggle edges.
    {
        Toggle t;
        int presses = 0;
        for (bool down : {false, true, true, true, false}) presses += t.Feed(down);
        Check(presses == 1, "held key toggles once");
        for (bool down : {true, false, true, false}) presses += t.Feed(down);
        Check(presses == 3, "press-release-press toggles twice more");
    }

    // Bone name hashes match the engine (FUN_140986110).
    Check(bones::Crc32("M_Head") == 0x9ec266b5u, "CRC32(M_Head) matches the engine's bone hash");
    Check(bones::Crc32("M_Neck") == 0x1920467au, "CRC32(M_Neck)");
    Check(bones::Crc32("player") == 0x98197a65u, "CRC32(player)");

    // Bind pose (T-pose) detection from shoulders and hands.
    {
        const pose::Vec3 sL{-0.18f, 1.40f, 0}, sR{0.18f, 1.40f, 0};
        Check(LooksLikeTPose(sL, {-0.75f, 1.40f, 0}, sR, {0.75f, 1.42f, 0}), "arms straight out -> T-pose");
        Check(!LooksLikeTPose(sL, {-0.25f, 0.85f, 0.05f}, sR, {0.25f, 0.85f, 0.05f}), "arms hanging -> not a T-pose");
        Check(!LooksLikeTPose(sL, {-0.75f, 1.40f, 0}, sR, {0.25f, 0.85f, 0}), "one arm out -> not a T-pose");
        Check(!LooksLikeTPose(sL, {-0.10f, 1.35f, -0.50f}, sR, {0.10f, 1.35f, -0.50f}),
              "both arms reaching forward (hands close) -> not a T-pose");
        Check(!LooksLikeTPose(sL, {-0.55f, 1.05f, 0}, sR, {0.55f, 1.05f, 0}), "A-pose / arms half down -> not a T-pose");
    }
    // F3 look direction: head -> eye midpoint, in the YawQuat convention (forward = -Z at yaw 0).
    {
        float yaw = 99.f;
        Check(LookYaw({0, 1, 0}, {-0.03f, 1.07f, -0.08f}, {0.03f, 1.07f, -0.08f}, &yaw) && std::fabs(yaw) < 1e-4f,
              "eyes in front along -Z -> yaw 0");
        Check(LookYaw({0, 1, 0}, {-0.08f, 1.07f, 0.03f}, {-0.08f, 1.07f, -0.03f}, &yaw) &&
                  std::fabs(yaw - 1.5707963f) < 1e-4f,
              "eyes towards -X -> yaw +90 deg (YawQuat turns -Z towards -X)");
        const pose::Vec3 fwd = pose::TransformPoint({0, 0, -1}, pose::FromPose(pose::YawQuat(yaw), {0, 0, 0}));
        Check(fwd.x < -0.99f, "LookYaw agrees with YawQuat's forward");
        Check(!LookYaw({0, 1, 0}, {0, 1.07f, 0}, {0, 1.07f, 0}, &yaw), "eyes straight above the head -> no yaw");
        Check(std::fabs(ApproachAngle(3.0f, -3.0f, 0.5f) - 3.1415927f) < 0.01f, "smoothing takes the short arc");
        Check(std::fabs(ApproachAngle(0.f, 1.f, 1.f) - 1.f) < 1e-5f, "t=1 reaches the target");
        // F3 pitch: positive pitch about +X looks up (forward -Z gains +Y), as fpcam assumes.
        const float p = 0.5f;
        const pose::Quat pq{std::sin(p * 0.5f), 0.f, 0.f, std::cos(p * 0.5f)};
        const pose::Vec3 up = pose::TransformPoint({0, 0, -1}, pose::FromPose(pq, {0, 0, 0}));
        Check(up.y > 0.4f && up.z < -0.8f, "positive pitch looks up");
    }
    // F3 facing from her face: the eye line gives two sides, the face itself says which is front.
    {
        // Head bone at the origin; eyes 9 cm forward, 7 cm up, 3 cm apart, for facing yaw y and pitch p.
        auto eyes = [](float y, float p, pose::Vec3* l, pose::Vec3* r) {
            const float fy = 0.07f * std::cos(p) + 0.09f * std::sin(p), fz = 0.07f * std::sin(p) - 0.09f * std::cos(p);
            // local mid (0, fy, fz) and right (1, 0, 0), turned by yaw (forward -Z, right +X at yaw 0)
            const pose::Vec3 mid{fz * std::sin(y), fy, fz * std::cos(y)};
            const pose::Vec3 right{std::cos(y), 0.f, -std::sin(y)};
            *l = {mid.x - 0.03f * right.x, mid.y, mid.z - 0.03f * right.z};
            *r = {mid.x + 0.03f * right.x, mid.y, mid.z + 0.03f * right.z};
        };
        const pose::Vec3 head{0.f, 0.f, 0.f};
        pose::Vec3 l{}, r{};
        float yaw = 0.f;
        bool fromFace = false;
        eyes(30 * deg, 0.f, &l, &r);
        Check(FaceFront(l, r, head, 30 * deg, 30 * deg, &yaw, &fromFace) && Near(yaw, 30 * deg, 1e-3f) && fromFace,
              "face front: level face at 30 deg");
        Check(FaceFront(l, r, head, 190 * deg, 30 * deg, &yaw, &fromFace) && Near(yaw, 30 * deg, 1e-3f) && fromFace,
              "the view drifted behind her: still her front (no flip)");
        eyes(30 * deg, -60 * deg, &l, &r);
        Check(FaceFront(l, r, head, 200 * deg, 30 * deg, &yaw, &fromFace) && Near(yaw, 30 * deg, 1e-3f),
              "looking down 60 deg, view behind: her front");
        eyes(30 * deg, 0.f, &l, &r);
        Check(FaceFront(l, r, head, 0.f, 0.f, &yaw, &fromFace) && Near(yaw, 30 * deg, 1e-3f),
              "head turned 30 deg from her body: her face wins");
        // Stale face-rig data (0.16.9): eyes behind her head, body front the other way -> side nearest the view.
        eyes(210 * deg, 0.f, &l, &r);
        Check(FaceFront(l, r, head, 30 * deg, 30 * deg, &yaw, &fromFace) && Near(std::remainder(yaw - 30 * deg, 6.2831853f), 0.f, 1e-3f) &&
                  !fromFace,
              "eyes behind her head (stale): falls back to the side nearest the view");
        // Eyes straight above the head bone: no forward from the face -> side nearest the view.
        l = {-0.03f, 0.11f, 0.f};
        r = {0.03f, 0.11f, 0.f};
        Check(FaceFront(l, r, head, 175 * deg, 0.f, &yaw, &fromFace) && Near(std::remainder(yaw - 180 * deg, 6.2831853f), 0.f, 1e-3f) &&
                  !fromFace,
              "no forward from the face: side nearest the view");
        l = {0.f, 0.05f, -0.09f};
        r = {0.f, 0.11f, -0.09f};
        Check(!FaceFront(l, r, head, 0.f, 0.f, &yaw, &fromFace), "vertical eye line: no facing");
    }
    // Snap turning: one step per push, repeats while held, re-arms at the centre.
    {
        SnapTurn s;
        const float dt = 1.f / 90.f;
        Check(s.Step(0.5f, dt) == 0, "half push does not snap");
        Check(s.Step(0.9f, dt) == 1, "full push right -> one step right");
        int extra = 0;
        for (int i = 0; i < 20; ++i) extra += s.Step(0.9f, dt);  // ~0.22 s held
        Check(extra == 0, "holding shorter than the repeat delay gives no second step");
        for (int i = 0; i < 12; ++i) extra += s.Step(0.9f, dt);  // past 0.33 s
        Check(extra == 1, "holding past the repeat delay steps again");
        Check(s.Step(0.5f, dt) == 0 && s.Step(0.9f, dt) == 0, "back to half without centring: not re-armed");
        Check(s.Step(0.1f, dt) == 0 && s.Step(-0.9f, dt) == -1, "centre, then full left -> one step left");
        s.Block();
        Check(s.Step(-0.9f, dt) == 0, "blocked (prompts): a stick already held does not snap");
        Check(s.Step(0.f, dt) == 0 && s.Step(-0.8f, dt) == -1, "after centring it snaps again");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
