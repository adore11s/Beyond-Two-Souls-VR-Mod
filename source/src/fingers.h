#pragma once
// First-person fingers on the controllers (playbook Chapter 14.6 / 14.7 / 14.10 / 14.12, revision 25; REQ-15; EXP-067).
//
// Rig evidence (STATIC, the game's own rig data: tools/hand_poses.py over all 272 character rigs with finger bones in
// the BigFile archives, both hands, docs/evidence/fingers/SUMMARY.md):
//   * chains under LC_<S>_Hand (the copy of <S>_Hand, parented to M_Hips): PX_<S>_Hand<F>0 (metacarpal) -> <F>1
//     (knuckle) -> <F>2 -> <F>3 for Index / Middle / Ring / Pinky, PX_<S>_HandThumb1 -> 2 -> 3 for the thumb; joint 3
//     is a leaf on every rig (no tip bone); all class word 4 (fingers).
//   * every finger bone lies along its OWN local X (row 0: +X on the left hand, -X on the right on every rig, a mirrored
//     rig), and every rest bend is about its own local Z (row 2):
//     0.00 degrees off on all middle joints, the thumb included (its Thumb2 bend), at most 0.63 degrees at the
//     knuckles. So, unlike Detroit's long fingers, B2S needs no palm-built axis at all: every joint, thumb included,
//     hinges about its own measured local Z (the 14.12 thumb rule, applied to all five).
//   * palm frame from the rest knuckles: f = wrist -> middle knuckle, s = little knuckle -> index knuckle, palm normal
//     (out of the palm) n = f x s on the left hand and s x f on the right: the long fingers' rest bend lies on the n
//     side on 2028 of 2034 chains, the thumb tip on all 544. A turn about +Z moves every finger and the thumb AWAY
//     from the palm on all 272 rigs, both hands: the closing sign is found per finger at its first joint anyway.
//   * the rest shape is the rig's bind pose: the NODEAR rest locals the loader copies to skeleton +0x300 (STATIC
//     FUN_140979af0; the same pose the skinned mesh's vertices are stored in, EXP-064). The mod logs once that the
//     skin's inverse binds (render element +0x60) times these rest worlds give the identity ("FINGERS rest check").
//
// Every rig the mod drives, young Jodie (JHO9) included (owner 2026-11-17: "Yes, young Jodie too").
// Pure functions only (no engine access): fpbody owns the arrays, tests/fingers_tests.cpp and tools/hand_poses.py
// check the same rules.
#include "fp_math.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace b2s::fingers {
using pose::Mat4;
using pose::Vec3;

enum : int { kThumb = 0, kIndex = 1, kMiddle = 2, kRing = 3, kLittle = 4, kCount = 5 };
constexpr uint32_t kNone = 0xffffffffu;

// ---- amounts (14.7, 14.10, 14.12) ----------------------------------------------------------------------------------
struct Input {
    float trigger = 0.f, squeeze = 0.f;  // analog 0..1
    bool triggerTouch = false;           // index finger on (or pulling) the trigger
    bool thumbTouch = false;             // thumb on the stick, a face button or the thumb rest
    bool touchSeen = false;              // this controller has reported a finger touch since VR started
};
enum class Source : int { Resting = 0, Touch = 1 };

inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float GripAmount(float squeeze) { return Clamp01((squeeze - 0.05f) / 0.30f); }
inline float TriggerAmount(float trigger) { return Clamp01((trigger - 0.05f) / 0.90f); }

// Half-Life: Alyx style from the touch sensors when the controller reports them (point = index lifted, fist =
// everything held); 14.10's resting bases otherwise (a controller without finger sensing: a hand resting on it reads
// as partly closed). The thumb follows the grip only (0.9 x grip): the thumb-touch curl (the thumb going down whenever
// the real thumb rests on the stick / buttons / thumb rest) and the thumbs up / gun that came from lifting it were
// removed at the owner's request (2026-11-21, playbook 14.12).
inline Source Targets(const Input& in, float out[kCount]) {
    const float grip = GripAmount(in.squeeze), trigger = TriggerAmount(in.trigger);
    if (in.touchSeen) {
        out[kMiddle] = out[kRing] = out[kLittle] = 0.15f + 0.85f * grip;
        out[kIndex] = (in.triggerTouch || trigger > 0.f) ? 0.35f + 0.65f * trigger : 0.f;
        out[kThumb] = 0.9f * grip;  // owner 2026-11-21: no thumb-touch pose
        return Source::Touch;
    }
    out[kMiddle] = out[kRing] = out[kLittle] = 0.45f + 0.55f * grip;
    out[kIndex] = 0.25f + 0.75f * trigger;
    out[kThumb] = std::max(0.35f, 0.8f * std::max(grip, trigger));
    return Source::Resting;
}

// Every amount moves at most 12 units per second (a full unit in ~83 ms: a quick transition, never a jump).
constexpr float kRate = 12.f;
inline float Approach(float current, float target, float maxStep) {
    if (!std::isfinite(current)) return target;
    if (target > current) return std::min(target, current + maxStep);
    return std::max(target, current - maxStep);
}
inline void Step(float current[kCount], const float target[kCount], float dt) {
    const float step = kRate * std::clamp(dt, 0.f, 0.25f);
    for (int f = 0; f < kCount; ++f) current[f] = Approach(current[f], target[f], step);
}

// angle = 4 deg + (limit - 4 deg) x amount; long fingers 80 / 95 / 65 deg, thumb 30 / 45 / 20 deg (14.10, 14.12).
constexpr float kRelaxedDeg = 4.f;
constexpr float kLongLimitDeg[3] = {80.f, 95.f, 65.f};
constexpr float kThumbLimitDeg[3] = {30.f, 45.f, 20.f};
inline float JointAngle(bool thumb, int joint, float amount) {
    const float limit = (thumb ? kThumbLimitDeg : kLongLimitDeg)[std::clamp(joint, 0, 2)];
    return (kRelaxedDeg + (limit - kRelaxedDeg) * Clamp01(amount)) * 0.017453293f;
}

// ---- the rig's hand --------------------------------------------------------------------------------------------------
struct Hand {
    uint32_t root = kNone;           // the bone the chains hang from (LC_<S>_Hand)
    uint32_t bone[kCount][4]{};      // each chain from the root's child down
    uint8_t bones[kCount]{};         // bones in the chain (long fingers 4, thumb 3)
    uint8_t firstCurl[kCount]{};     // first bone that bends: the knuckle (1) on long fingers, Thumb1 (0) on the thumb
    bool right = false;
    bool valid = false;
};

// Chains from bone indices (kNone = absent). A chain must hang from `root` link by link and is kept down to its last
// linked bone (one rig, the Black Sun shield copy IW_FXS_BLACKSUN_CROSSSET_EXT_BSSHIELD, has no PX_L_HandPinky3); a
// long finger needs its metacarpal, knuckle and one more bone, the thumb two bones, else it is left out (not driven).
// The hand is valid with the thumb and at least the index, middle and little chains.
inline Hand MakeHand(const uint32_t* parents, uint32_t count, uint32_t root, const uint32_t chains[kCount][4],
                     bool right) {
    Hand h;
    h.right = right;
    if (!parents || root >= count) return h;
    h.root = root;
    for (int f = 0; f < kCount; ++f) {
        const int want = f == kThumb ? 3 : 4;
        uint32_t parent = root;
        int n = 0;
        for (int k = 0; k < want; ++k) {
            const uint32_t b = chains[f][k];
            if (b >= count || parents[b] != parent) break;
            h.bone[f][n++] = b;
            parent = b;
        }
        h.bones[f] = uint8_t(n >= (f == kThumb ? 2 : 3) ? n : 0);
        h.firstCurl[f] = uint8_t(f == kThumb ? 0 : 1);
    }
    h.valid = h.bones[kThumb] && h.bones[kIndex] && h.bones[kMiddle] && h.bones[kLittle];
    return h;
}

// 14.12: a rest matrix is used only when finite, |det| within 0.05..20 and its w column (0, 0, 0, 1).
inline bool UsableRest(const Mat4& m) {
    for (float v : m.m)
        if (!std::isfinite(v)) return false;
    if (std::fabs(m.m[3]) + std::fabs(m.m[7]) + std::fabs(m.m[11]) > 1e-4f || std::fabs(m.m[15] - 1.f) > 1e-4f)
        return false;
    const float det = m.m[0] * (m.m[5] * m.m[10] - m.m[6] * m.m[9]) - m.m[1] * (m.m[4] * m.m[10] - m.m[6] * m.m[8]) +
                      m.m[2] * (m.m[4] * m.m[9] - m.m[5] * m.m[8]);
    return std::fabs(det) >= 0.05f && std::fabs(det) <= 20.f;
}

inline Vec3 Pos(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }
inline Vec3 Row(const Mat4& m, int r) { return {m.m[r * 4], m.m[r * 4 + 1], m.m[r * 4 + 2]}; }
inline void SetRow(Mat4& m, int r, Vec3 v) { m.m[r * 4] = v.x; m.m[r * 4 + 1] = v.y; m.m[r * 4 + 2] = v.z; }

// Rodrigues: v turned about the unit axis a by `angle` (right hand).
inline Vec3 Turn(Vec3 v, Vec3 a, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return fp::Add(fp::Add(fp::Mul(v, c), fp::Mul(fp::Cross(a, v), s)), fp::Mul(a, fp::Dot(a, v) * (1.f - c)));
}
// The bone's frame turned about a world axis through its own origin (its position and row lengths are kept).
inline void TurnRows(Mat4& m, Vec3 axis, float angle) {
    for (int r = 0; r < 3; ++r) SetRow(m, r, Turn(Row(m, r), axis, angle));
}

// Affine inverse (3x3 part may hold scale); false when singular.
inline bool Inverse(const Mat4& m, Mat4& out) {
    const float a = m.m[0], b = m.m[1], c = m.m[2], d = m.m[4], e = m.m[5], f = m.m[6], g = m.m[8], h = m.m[9],
                i = m.m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-10f) return false;
    const float s = 1.f / det;
    out = pose::Identity();
    out.m[0] = (e * i - f * h) * s; out.m[1] = (c * h - b * i) * s; out.m[2] = (b * f - c * e) * s;
    out.m[4] = (f * g - d * i) * s; out.m[5] = (a * i - c * g) * s; out.m[6] = (c * d - a * f) * s;
    out.m[8] = (d * h - e * g) * s; out.m[9] = (b * g - a * h) * s; out.m[10] = (a * e - b * d) * s;
    const Vec3 t = pose::TransformPoint({-m.m[12], -m.m[13], -m.m[14]}, out);
    out.m[12] = t.x; out.m[13] = t.y; out.m[14] = t.z;
    return true;
}
// local = world x parentWorld^-1 (the engine's composition is world = local x parent world).
inline bool LocalOf(const Mat4& world, const Mat4& parentWorld, Mat4& out) {
    Mat4 inv;
    if (!Inverse(parentWorld, inv)) return false;
    out = pose::Mul(world, inv);
    return true;
}

struct Palm {
    Vec3 n{}, s{}, f{};  // out of the palm, little -> index side, wrist -> middle knuckle (unit)
    bool flipped = false;  // n had to be flipped to put the thumb on the palm side (never on the game's rigs)
    bool ok = false;
};
// The palm frame from the knuckles of a posed hand (14.12): n = f x s on the left hand, s x f on the right, flipped
// when the thumb's end does not lie on the n side.
inline Palm PalmFrame(const Hand& h, const Mat4* world) {
    Palm p;
    if (!h.valid) return p;
    const Vec3 wrist = Pos(world[h.root]);
    const Vec3 middle = Pos(world[h.bone[kMiddle][1]]), index = Pos(world[h.bone[kIndex][1]]),
               little = Pos(world[h.bone[kLittle][1]]);
    p.f = fp::Norm(fp::Sub(middle, wrist));
    p.s = fp::Norm(fp::Sub(index, little));
    p.n = fp::Norm(h.right ? fp::Cross(p.s, p.f) : fp::Cross(p.f, p.s));
    if (fp::Len(p.f) < 0.5f || fp::Len(p.s) < 0.5f || fp::Len(p.n) < 0.5f) return p;
    const Vec3 thumbEnd = Pos(world[h.bone[kThumb][h.bones[kThumb] - 1]]);
    if (fp::Dot(fp::Sub(thumbEnd, wrist), p.n) < 0.f) {
        p.n = fp::Mul(p.n, -1.f);
        p.flipped = true;
    }
    p.ok = true;
    return p;
}

// The sign of a finger bone's own row 0 along the finger, taken on the UNBENT pose from the segment that reaches it
// (measured: every finger bone lies along local X, +X left / -X right). Taken once, before any bend: a joint bent past
// 90 degrees would flip a sign taken from the bent pose (the shield rig's shortened little finger showed it).
inline float AlongSign(const Mat4& bone, const Mat4& parent) {
    return fp::Dot(Row(bone, 0), fp::Sub(Pos(bone), Pos(parent))) < 0.f ? -1.f : 1.f;
}
// Where a bone points: toward its child in the chain, else (last joint, no tip bone on B2S's rigs) along its own row 0
// with its AlongSign (never "the row nearest the segment", 14.12's 48-degree flip).
inline Vec3 BoneDirection(const Mat4& bone, const Mat4* child, float along) {
    if (child) {
        const Vec3 d = fp::Sub(Pos(*child), Pos(bone));
        if (fp::Len(d) > 1e-4f) return fp::Norm(d);
    }
    return fp::Mul(fp::Norm(Row(bone, 0)), along);
}

struct Report {
    uint32_t restUsed = 0, restRefused = 0;  // chain bones placed at their rest local / kept the animation's local
    uint32_t ownAxis = 0, palmAxis = 0;      // joints bent about their own local Z / about the palm-built fallback
    bool palmFlipped = false;
};

// Pose one hand's finger chains in `world` (in place). On entry `world` holds the drawn pose with the root already on
// the solved hand and every chain bone carried from it with this frame's animated locals. Each chain bone is placed at
// its REST local from its parent (rest shape: no animated spread, twist or knuckle motion; a bone whose rest local is
// unusable keeps the animated one), then each bending joint turns about its own local Z, signed once at the finger's
// first joint so a positive amount closes toward the palm (the thumb toward the palm and across it, n - s), by
// JointAngle(amount). An axis that is not a hinge for its bone (|axis . bone direction| >= 0.2) falls back to the
// palm-built one. Segment lengths are kept: only rotations change below the root.
inline bool Pose(const Hand& h, const Mat4* restLocal, const bool* restOk, const float amount[kCount], bool restShape,
                 Mat4* world, Report* report = nullptr) {
    if (!h.valid || !restLocal || !world) return false;
    Report rep;
    Mat4 local[kCount][4];
    // Pass 1: every chain bone from its local (rest or animated), no bend yet.
    for (int f = 0; f < kCount; ++f) {
        uint32_t parent = h.root;
        for (int k = 0; k < h.bones[f]; ++k) {
            const uint32_t b = h.bone[f][k];
            const bool rest = restShape && restOk && restOk[b] && UsableRest(restLocal[b]);
            if (rest) {
                local[f][k] = restLocal[b];
                ++rep.restUsed;
            } else {
                if (!LocalOf(world[b], world[parent], local[f][k])) return false;
                if (restShape) ++rep.restRefused;
            }
            parent = b;
        }
    }
    float along[kCount][4];
    for (int f = 0; f < kCount; ++f) {
        uint32_t parent = h.root;
        for (int k = 0; k < h.bones[f]; ++k) {
            world[h.bone[f][k]] = pose::Mul(local[f][k], world[parent]);
            along[f][k] = AlongSign(world[h.bone[f][k]], world[parent]);
            parent = h.bone[f][k];
        }
    }
    // The palm from the placed (unbent) knuckles: bending starts at the knuckles and never moves them.
    const Palm palm = PalmFrame(h, world);
    if (!palm.ok) return false;
    rep.palmFlipped = palm.flipped;
    // Pass 2: bend, parents first, rebuilding each bone from its parent's bent frame.
    for (int f = 0; f < kCount; ++f) {
        if (!h.bones[f]) continue;
        const bool thumb = f == kThumb;
        const Vec3 toward = thumb ? fp::Norm(fp::Sub(palm.n, palm.s)) : palm.n;
        Vec3 previous{};
        bool havePrevious = false;
        uint32_t parent = h.root;
        for (int k = 0; k < h.bones[f]; ++k) {
            const uint32_t b = h.bone[f][k];
            world[b] = pose::Mul(local[f][k], world[parent]);
            parent = b;
            if (k < h.firstCurl[f]) continue;
            const int joint = k - h.firstCurl[f];
            Mat4 child{};
            const bool haveChild = k + 1 < h.bones[f];
            if (haveChild) child = pose::Mul(local[f][k + 1], world[b]);
            const Vec3 dir = BoneDirection(world[b], haveChild ? &child : nullptr, along[f][k]);
            Vec3 axis = fp::Norm(Row(world[b], 2));
            if (fp::Len(axis) < 0.5f || std::fabs(fp::Dot(axis, dir)) >= 0.2f) {
                axis = fp::Norm(fp::Cross(dir, toward));  // palm-built fallback: turning +about it closes toward `toward`
                ++rep.palmAxis;
                if (fp::Len(axis) < 0.5f) return false;
            } else {
                ++rep.ownAxis;
            }
            if (!havePrevious) {
                // Sign fixed once at the first bending joint: + must close toward the palm.
                if (fp::Dot(fp::Cross(axis, dir), toward) < 0.f) axis = fp::Mul(axis, -1.f);
            } else if (fp::Dot(axis, previous) < 0.f) {
                axis = fp::Mul(axis, -1.f);  // the same hinge sense down the finger
            }
            previous = axis;
            havePrevious = true;
            TurnRows(world[b], axis, JointAngle(thumb, joint, amount[f]));
        }
    }
    if (report) *report = rep;
    return true;
}

// ---- held items: blending between the animation's hand and the driven fingers (19.8 point 2, revision 15) -----------
// Blend two locals: rows and translation interpolated, rows re-orthonormalised and given their interpolated lengths
// (a plain row blend shrinks the rows, 14.12's first bug).
inline Mat4 BlendLocal(const Mat4& a, const Mat4& b, float t) {
    t = Clamp01(t);
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    Mat4 out = a;
    Vec3 r[3];
    float len[3];
    for (int k = 0; k < 3; ++k) {
        const Vec3 ra = Row(a, k), rb = Row(b, k);
        len[k] = fp::Len(ra) * (1.f - t) + fp::Len(rb) * t;
        r[k] = fp::Add(fp::Mul(fp::Norm(ra), 1.f - t), fp::Mul(fp::Norm(rb), t));
    }
    const Vec3 x = fp::Norm(r[0]);
    Vec3 y = fp::Norm(fp::Sub(r[1], fp::Mul(x, fp::Dot(r[1], x))));
    Vec3 z = fp::Cross(x, y);
    if (fp::Dot(z, r[2]) < 0.f) z = fp::Mul(z, -1.f);  // keep the original handedness
    if (fp::Len(x) < 0.5f || fp::Len(y) < 0.5f) return t < 0.5f ? a : b;
    SetRow(out, 0, fp::Mul(x, len[0]));
    SetRow(out, 1, fp::Mul(y, len[1]));
    SetRow(out, 2, fp::Mul(z, len[2]));
    for (int k = 12; k < 15; ++k) out.m[k] = a.m[k] * (1.f - t) + b.m[k] * t;
    return out;
}

// The chains of `driven` mixed with `animated` (both full worlds, the root identical), weight 1 = driven, 0 = the
// animation's own hand: each chain bone's local is blended, then composed from its blended parent. Writes `out`.
inline bool BlendHand(const Hand& h, const Mat4* animated, const Mat4* driven, float weight, Mat4* out) {
    if (!h.valid) return false;
    for (int f = 0; f < kCount; ++f) {
        uint32_t parent = h.root;
        for (int k = 0; k < h.bones[f]; ++k) {
            const uint32_t b = h.bone[f][k];
            Mat4 la, lb;
            if (!LocalOf(animated[b], animated[parent], la) || !LocalOf(driven[b], driven[parent], lb)) return false;
            out[b] = pose::Mul(BlendLocal(la, lb, weight), out[parent]);
            parent = b;
        }
    }
    return true;
}
}  // namespace b2s::fingers
