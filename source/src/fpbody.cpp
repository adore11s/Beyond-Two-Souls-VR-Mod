#include "fpbody.h"
#include "bones.h"
#include "fpmode.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

// First-person body edits, written into Jodie's skeleton world array (see fpbody.h).
//
// EXP-035 (STATIC): the palette at *(mesh+0x258)+0x60 that 0.9.0/0.9.1 edited is a
// QDT::M3D::CLOTH_MATRIX_MAP (cloth simulation input), and +0x58 a KPHYSICS TRANSFORM_SETS: neither
// is the render path, which is why no edit was ever visible. The renderers upload bone constants
// that are copied from the skeleton's world matrices, so the edits now go into those matrices right
// after the frame's final animation build (and again after any later rebuild in the same frame).
// The text below describes the earlier palette design and is kept for history.
//
// Why the palette and not the skeleton: 50+ engine paths rebuild a skeleton's world matrices
// (HumanIK, character post-updates, vehicles, sequences; callers of SKELETON::BuildWorld), so an
// edit written into the skeleton at one of them is overwritten before the meshes are drawn, and
// the earlier "write only if nothing changed since" guard refused to write whenever any of them
// ran. MESH::PaletteCopy (STATIC FUN_1409ab770, callers MESH::vfunc_133 and FUN_1408e9d20) is
// where every skinned mesh copies the skeleton's current worlds (+0x10) into its own palette
// (+0x18, remapped through +0x8, count +0x10): the edits are computed from exactly the pose being
// copied and written into the palette right after the engine's own copy.
namespace b2s::fpbody {
namespace {
using pose::Mat4;
using pose::Vec3;
constexpr uint32_t kAbsent = detail::kMaxBones;
constexpr uint32_t kForeHash[2] = {0x5c68ade3, 0x386a3400};  // L_ForeArm, R_ForeArm
constexpr uint32_t kHandHash[2] = {0xd50f0103, 0xecd362e8};  // L_Hand, R_Hand
const uint32_t kHeadHash = bones::Crc32("M_Head");
const uint32_t kUpperHash[2] = {bones::Crc32("L_Arm"), bones::Crc32("R_Arm")};  // names in the exe's strings
// LIVE (young Jodie): constraint copies parented to M_Hips that follow their driver bone. The
// fingers hang under LC_*_Hand and the face under LC_M_Head, not under the hand / head bones.
const uint32_t kLcHandHash[2] = {bones::Crc32("LC_L_Hand"), bones::Crc32("LC_R_Hand")};
const uint32_t kLcHeadHash = bones::Crc32("LC_M_Head");
const uint32_t kEyeHash[2] = {bones::Crc32("FA_L_Eye"), bones::Crc32("FA_R_Eye")};  // LIVE: face rig bones


using BuildFn = void (*)(void*, const float*);
using PaletteFn = void (*)(void* palette, void* skeleton);
BuildFn g_original = nullptr;
PaletteFn g_paletteOriginal = nullptr;
void* g_finalReturn = nullptr;
std::mutex g_mutex;
Plan g_plan;
uint64_t g_planSerial = 1;
std::atomic<void*> g_active{nullptr};   // skeleton whose palettes get the edits (lock-free filter)
std::atomic<void*> g_watched{nullptr};  // skeleton whose clean camera anchors are captured
void* g_watchedNode = nullptr;
uint32_t g_reportedIssues = 0;
Status g_status{};
std::atomic<uint64_t> g_autoDumpAt{0};  // one element dump per new rig, a few seconds in (hair diagnostics)

// Clean camera anchors from the final animation build (the engine's own pose, never edited).
struct Anchors {
    void* skeleton = nullptr;
    uint32_t head = kAbsent, count = 0, eye[2]{kAbsent, kAbsent};
    uint32_t upper[2]{kAbsent, kAbsent}, hand[2]{kAbsent, kAbsent};  // T-pose test
    Mat4 headWorld{}, rootWorld{};
    Vec3 eyePos[2]{};
    bool valid = false, eyesValid = false;
    // Bind pose: entered after 9 T-pose builds in a row (~0.1 s), left after 27 without (~0.3 s).
    uint32_t tposeRun = 0, normalRun = 0;
    bool tposed = false;
} g_anchors;

struct HandRig {
    uint32_t upper = kAbsent, fore = kAbsent, hand = kAbsent;
    uint32_t lc = kAbsent;                     // LC_*_Hand: finger group carried with the solved hand
    uint32_t index = kAbsent, pinky = kAbsent;  // knuckle bones beside / farthest from the thumb
    uint32_t fingers[8]{};                     // knuckle bones of the non-thumb fingers
    uint32_t fingerCount = 0;
    float palmLength = 0.f;                    // wrist to knuckle centroid, metres
    bool anatomical = false;                   // fingers found: the wrist can follow the controller
};
struct Rig {
    void* skeleton = nullptr;
    uint32_t count = 0, head = kAbsent;
    // 0.16.41 (owner: adult Jodie needs fixes; "these changes shouldn't affect the small Jodie model"):
    // an adult rig has arms (shoulder -> elbow -> wrist) longer than 0.42 m (LIVE: young Jodie 0.34 m,
    // red-dress adult Jodie 0.48 m). Everything gated on `adult` leaves young Jodie exactly as before.
    bool adult = false;
    float armScale = 1.f;  // adult: arm length / 0.34 (1..1.6), scales the helper distances
    std::array<bool, detail::kMaxBones> clothKeep{};  // adult: hidden bones left in place in the cloth pose
    std::array<uint32_t, detail::kMaxBones> parents{};
    std::array<bool, detail::kMaxBones> hideTree{};
    std::array<bool, detail::kMaxBones> carried[2]{};  // bones that follow each solved hand
    // Finger pose frozen relative to the hand (owner: the hand on the controller still played the
    // animation): carried bone = fingerRel * hand, captured once per rig.
    std::array<Mat4, detail::kMaxBones> fingerRel[2]{};
    bool fingerRelValid[2]{};
    // Learned arm helpers (owner: a point at the wrist stayed with the animated body and the skin
    // stretched from it to the controller-driven hand). Candidates: non-core bones near an arm,
    // outside the arm and finger subtrees. Over the first frames their offset in each arm bone's
    // frame is tracked; a bone that stays within 2 cm in one arm bone's frame is attached to it.
    struct Candidate {
        uint32_t bone = 0;
        uint8_t hand = 0;
        Vec3 lo[4]{}, hi[4]{};  // per frame of reference (upper, fore, hand, hips): local offset box
        float rel0[3][9]{};     // first rotation in the upper / fore / hand frame (row-major 3x3)
        float maxAngle[3]{};    // largest turn away from rel0 since, radians
        uint32_t samples = 0;
        uint32_t sinceDecided = 0;  // prop sockets: poses since the last decision (re-checked)
        bool decided = false;
    };
    std::array<Candidate, 96> cand{};
    uint32_t candCount = 0;
    std::array<int16_t, detail::kMaxBones> attachDriver{};  // -1 none, else bone index of the driver
    bool attachLearned = false;
    uint32_t hidden = 0, collapseAt = kAbsent;  // hidden bones shrink onto collapseAt (neck base)
    HandRig hand[2];
    bool valid = false;
} g_rig;

struct Overlay {
    void* skeleton = nullptr;
    uint32_t count = 0;
    uint64_t serial = 0;
    bool ready = false;
    std::array<Mat4, detail::kMaxBones + 1> clean{}, edited{};
    std::array<bool, detail::kMaxBones> changed{};
} g_overlay;
std::array<Mat4, detail::kMaxBones + 1> g_body, g_work, g_arm;

Vec3 Position(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }
void SetPosition(Mat4& m, Vec3 p) { m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z; }

void IssueOnce(uint32_t bit, const char* message) {
    if (g_reportedIssues & bit) return;
    g_reportedIssues |= bit;
    Log("FPBODY %s", message);
}

const Mat4* RenderSource(const Mat4* src);
bool SourceArrays(void* skel, const Mat4** a, const Mat4** b, uint32_t* count);
extern bool g_renderReady;
extern std::atomic<bool> g_editsOn;

alignas(16) Mat4 g_propWorld;
bool g_loggedItem = false;
// Bones that scene nodes hang off (prop sockets), seen by PropWorldFromEdited: candidates for the
// arm-helper learning even when they are not near an arm at rig build time.
std::array<bool, detail::kMaxBones> g_socketBone{};
bool ReadElementHeader(void* elem, int* type, const Mat4** world, const Mat4** bones, uint32_t* count);
bool PropWorldFromEdited(const Mat4* parent, uint32_t count);

// ---- engine memory: tiny SEH functions without C++ unwind objects ----
struct SkeletonView {
    uint32_t count;
    const Mat4* worlds;
    const uint32_t* parents;
};
bool ReadSkeleton(void* skel, SkeletonView* v) {
    __try {
        if (!skel) return false;
        v->count = *static_cast<uint32_t*>(skel);
        v->worlds = *reinterpret_cast<Mat4**>(static_cast<char*>(skel) + 0x10);
        v->parents = *reinterpret_cast<uint32_t**>(static_cast<char*>(skel) + 0x30);
        return v->count && v->count <= detail::kMaxBones && v->worlds && v->parents;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CopyMem(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Bone index of a name hash from the skeleton's own table: +0x270 holds 8-byte records whose
// first dword is the bone's CRC32, record i describing bone i (STATIC FUN_140977950).
uint32_t HashIndex(void* skel, uint32_t count, uint32_t hash) {
    __try {
        const uint8_t* table = *reinterpret_cast<uint8_t**>(static_cast<char*>(skel) + 0x270);
        if (!table) return kAbsent;
        for (uint32_t i = 0; i < count; ++i)
            if (*reinterpret_cast<const uint32_t*>(table + size_t(i) * 8) == hash) return i;
        return kAbsent;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return kAbsent; }
}

// ---- rig description (once per skeleton) ----
void BuildHand(Rig& r, const Mat4* w, unsigned h) {
    HandRig hr{};
    hr.fore = HashIndex(r.skeleton, r.count, kForeHash[h]);
    hr.hand = HashIndex(r.skeleton, r.count, kHandHash[h]);
    // Upper arm: the named one when it is above the forearm, else the forearm's parent. Up to
    // two twist bones may sit between the forearm and the hand (they are rebuilt with the arm).
    hr.upper = HashIndex(r.skeleton, r.count, kUpperHash[h]);
    if (hr.fore >= r.count || hr.upper >= r.count || !detail::Descendant(r.parents.data(), r.count, hr.fore, hr.upper) ||
        hr.upper == hr.fore)
        hr.upper = hr.fore < r.count ? r.parents[hr.fore] : kAbsent;
    uint32_t steps = 0;
    for (uint32_t b = hr.hand; b < r.count && b != hr.fore && steps < 4; b = r.parents[b]) ++steps;
    if (hr.upper >= r.count || hr.hand >= r.count || hr.fore >= r.count || steps < 1 || steps > 3 ||
        !detail::Descendant(r.parents.data(), r.count, hr.hand, hr.fore)) {
        IssueOnce(4u << h, h == 0 ? "left named arm chain missing; authored left arm retained"
                                  : "right named arm chain missing; authored right arm retained");
        r.hand[h] = HandRig{};
        r.carried[h].fill(false);
        return;
    }
    detail::Fingers f = detail::FindFingers(r.parents.data(), r.count, w, hr.hand);
    const uint32_t lc = HashIndex(r.skeleton, r.count, kLcHandHash[h]);
    // A finger group outside the hand's subtree must be carried with the solved hand by hand.
    if (lc < r.count && !detail::Descendant(r.parents.data(), r.count, lc, hr.hand)) {
        hr.lc = lc;
        for (uint32_t i = 0; i < r.count; ++i) r.carried[h][i] = detail::Descendant(r.parents.data(), r.count, i, lc);
        if (!f.anatomical) f = detail::FindFingers(r.parents.data(), r.count, w, hr.hand, lc);
    }
    for (uint32_t i = 0; i < f.count; ++i) hr.fingers[i] = f.knuckles[i];
    hr.fingerCount = f.count;
    hr.index = f.index;
    hr.pinky = f.pinky;
    hr.palmLength = f.palmLength;
    hr.anatomical = f.anatomical;
    if (!hr.anatomical)
        IssueOnce(16u << h, h == 0 ? "left hand fingers not found; the left wrist keeps the animation's rotation"
                                   : "right hand fingers not found; the right wrist keeps the animation's rotation");
    r.hand[h] = hr;
}

bool BuildRig(void* skel, const SkeletonView& v, const Mat4* clean) {
    Rig& r = g_rig;
    r.skeleton = skel;
    r.count = v.count;
    r.valid = false;
    r.hand[0] = r.hand[1] = HandRig{};
    r.carried[0].fill(false);
    r.carried[1].fill(false);
    r.fingerRelValid[0] = r.fingerRelValid[1] = false;
    if (!CopyMem(r.parents.data(), v.parents, v.count * sizeof(uint32_t)) ||
        !detail::ValidHierarchy(r.parents.data(), v.count)) {
        g_status.lastFailure = 1;
        IssueOnce(1, "layout/hierarchy unavailable; body edits disabled for this actor");
        return false;
    }
    r.head = HashIndex(skel, v.count, kHeadHash);
    if (r.head >= v.count) {
        g_status.lastFailure = 2;
        IssueOnce(2, "M_Head missing; body/head hiding disabled for this actor");
        return false;
    }
    BuildHand(r, clean, 0);
    BuildHand(r, clean, 1);
    uint32_t protect[4], np = 0;
    for (unsigned h = 0; h < 2; ++h) {
        if (r.hand[h].upper < v.count) protect[np++] = r.hand[h].upper;
        const uint32_t lc = HashIndex(skel, v.count, kLcHandHash[h]);
        if (lc < v.count) protect[np++] = lc;
    }
    r.hidden = detail::HeadHideSet(r.parents.data(), v.count, clean, r.head, HashIndex(skel, v.count, kLcHeadHash),
                                   protect, np, r.hideTree.data(), &r.collapseAt);
    {
        float armLen = 0.f;
        for (unsigned h = 0; h < 2; ++h) {
            const HandRig& hr = r.hand[h];
            if (hr.upper >= v.count || hr.fore >= v.count || hr.hand >= v.count) continue;
            armLen = std::max(armLen, fp::Len(fp::Sub(Position(clean[hr.fore]), Position(clean[hr.upper]))) +
                                          fp::Len(fp::Sub(Position(clean[hr.hand]), Position(clean[hr.fore]))));
        }
        r.adult = armLen > 0.42f;
        r.armScale = r.adult ? std::clamp(armLen / 0.34f, 1.f, 1.6f) : 1.f;
        // Adult cloth (owner 0.16.40: "her simulated red dress is now too down"): the cloth pose drops the
        // hidden bones 30 m (0.16.24, so young Jodie's hair is simulated out of sight). On the adult the hidden
        // set also holds bones rooted near her neck below its base - the neckline the dress hangs from - so
        // the dress was pulled down. Adult: hidden bones more than 1 cm below the neck base stay in place in
        // the cloth pose; the head and hair above it still drop.
        r.clothKeep.fill(false);
        uint32_t kept = 0;
        if (r.adult && r.collapseAt < v.count) {
            const float neckY = Position(clean[r.collapseAt]).y;
            for (uint32_t b = 0; b < v.count; ++b)
                if (r.hideTree[b] && Position(clean[b]).y < neckY - 0.01f) {
                    r.clothKeep[b] = true;
                    ++kept;
                }
        }
        Log("FPBODY rig arms %.2f m: %s", armLen, r.adult ? "adult" : "child (unchanged rules)");
        if (r.adult)
            Log("FPBODY adult rig: arm scale %.2f; %u hidden bone(s) below her neck base stay in the cloth pose", r.armScale,
                kept);
    }
    g_autoDumpAt = GetTickCount64() + 4000;
    r.attachDriver.fill(-1);
    r.attachLearned = false;
    r.candCount = 0;
    for (uint32_t b = 0; b < v.count && r.candCount < r.cand.size(); ++b) {
        if (r.hideTree[b] || r.carried[0][b] || r.carried[1][b]) continue;
        bool skip = false;
        for (unsigned h = 0; h < 2 && !skip; ++h) {
            const HandRig& hr = r.hand[h];
            if (hr.upper < v.count && (detail::Descendant(r.parents.data(), v.count, b, hr.upper) ||
                                       detail::Descendant(r.parents.data(), v.count, hr.upper, b)))
                skip = true;  // the arm chain itself or its ancestors (shoulder, spine)
        }
        if (skip || detail::Descendant(r.parents.data(), v.count, r.head, b)) continue;
        for (unsigned h = 0; h < 2; ++h) {
            const HandRig& hr = r.hand[h];
            if (hr.upper >= v.count || hr.fore >= v.count || hr.hand >= v.count) continue;
            const Vec3 p = Position(clean[b]);
            const float d = std::min({fp::Len(fp::Sub(p, Position(clean[hr.upper]))), fp::Len(fp::Sub(p, Position(clean[hr.fore]))),
                                      fp::Len(fp::Sub(p, Position(clean[hr.hand])))});
            if (d < 0.15f) {
                auto& c = r.cand[r.candCount++];
                c = {};
                c.bone = b;
                c.hand = uint8_t(h);
                break;
            }
        }
    }
    r.valid = true;
    g_status.bonesCount = v.count;
    g_status.lastHeadIndex = r.head;
    g_status.skeleton = reinterpret_cast<uintptr_t>(skel);
    g_status.worlds = reinterpret_cast<uintptr_t>(v.worlds);
    for (unsigned h = 0; h < 2; ++h) {
        g_status.upper[h] = r.hand[h].upper;
        g_status.fore[h] = r.hand[h].fore;
        g_status.hand[h] = r.hand[h].hand;
    }
    g_status.hiddenBones = r.hidden;
    Log("FPBODY rig skeleton=%p bones=%u head=%u (%u hidden) L arm %u/%u/%u lc=%u carried=%u fingers=%u palm=%.3f "
        "R arm %u/%u/%u lc=%u carried=%u fingers=%u palm=%.3f",
        skel, v.count, r.head, r.hidden,
        r.hand[0].upper, r.hand[0].fore, r.hand[0].hand, r.hand[0].lc,
        unsigned(std::count(r.carried[0].begin(), r.carried[0].begin() + v.count, true)), r.hand[0].fingerCount,
        r.hand[0].palmLength, r.hand[1].upper, r.hand[1].fore, r.hand[1].hand, r.hand[1].lc,
        unsigned(std::count(r.carried[1].begin(), r.carried[1].begin() + v.count, true)), r.hand[1].fingerCount,
        r.hand[1].palmLength);
    return true;
}

// ---- the edits ----
Mat4 Rotation(const Mat4& m) {
    Mat4 r = pose::Identity();
    for (int row = 0; row < 3; ++row) {
        const Vec3 axis = fp::Norm({m.m[row * 4], m.m[row * 4 + 1], m.m[row * 4 + 2]});
        r.m[row * 4] = axis.x; r.m[row * 4 + 1] = axis.y; r.m[row * 4 + 2] = axis.z;
    }
    return r;
}
// Rotate the authored basis towards a new segment, preserving bone-axis choices.
Mat4 PointSegment(const Mat4& authored, Vec3 from, Vec3 to) {
    const Vec3 a = fp::Norm(from), b = fp::Norm(to);
    const float dot = std::clamp(fp::Dot(a, b), -1.f, 1.f);
    Vec3 cross = fp::Cross(a, b);
    pose::Quat q{cross.x, cross.y, cross.z, 1.f + dot};
    if (dot < -0.9999f) {
        cross = fp::Norm(fp::Cross(a, std::fabs(a.x) < 0.8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
        q = {cross.x, cross.y, cross.z, 0};
    }
    if (!pose::Normalize(q)) return authored;
    auto result = pose::Mul(authored, pose::FromPose(q, {0, 0, 0}));
    SetPosition(result, Position(authored));
    return result;
}

// The knuckle centroid and thumb-side line of a hand in a pose.
bool RigHandBasis(const HandRig& hr, const Mat4* w, Mat4* out) {
    Vec3 c{0, 0, 0};
    for (uint32_t i = 0; i < hr.fingerCount; ++i) c = fp::Add(c, Position(w[hr.fingers[i]]));
    c = fp::Mul(c, 1.f / float(hr.fingerCount));
    return fp::HandBasis(fp::Sub(c, Position(w[hr.hand])), fp::Sub(Position(w[hr.index]), Position(w[hr.pinky])), out);
}

// Feed one clean pose into the attachment learning (called once per new clean pose). A candidate
// is decided only once it has moved >= 6 cm relative to the hips (otherwise "rigid" just means
// "nothing moved", which would glue the dress to the arms): attached if it stayed within 2 cm in
// one arm bone's frame meanwhile, else left alone. Undecided candidates keep learning.
// Rotation of `bone` in `frame`'s axes, both normalised (row vectors: bone * frame^T), row-major 3x3.
bool RelativeRotation(const Mat4& bone, const Mat4& frame, float out[9]) {
    Vec3 b[3], f[3];
    for (int k = 0; k < 3; ++k) {
        b[k] = fp::Norm({bone.m[k * 4], bone.m[k * 4 + 1], bone.m[k * 4 + 2]});
        f[k] = fp::Norm({frame.m[k * 4], frame.m[k * 4 + 1], frame.m[k * 4 + 2]});
        if (!fp::Finite(b[k]) || !fp::Finite(f[k]) || fp::Len(b[k]) < 0.5f || fp::Len(f[k]) < 0.5f) return false;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 3 + j] = fp::Dot(b[i], f[j]);
    return true;
}
// Angle of the rotation taking b to a (both orthonormal 3x3, row-major): acos((trace(a * b^T) - 1) / 2).
float RotationAngle(const float a[9], const float b[9]) {
    float trace = 0.f;
    for (int i = 0; i < 3; ++i) trace += a[i * 3] * b[i * 3] + a[i * 3 + 1] * b[i * 3 + 1] + a[i * 3 + 2] * b[i * 3 + 2];
    return std::acos(std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f));
}

void LearnAttachments(const Mat4* body) {
    Rig& r = g_rig;
    // Prop sockets seen since the rig was built join the candidates (nearest arm).
    for (uint32_t b = 0; b < r.count && r.candCount < r.cand.size(); ++b) {
        if (!g_socketBone[b] || r.hideTree[b] || r.carried[0][b] || r.carried[1][b] || r.attachDriver[b] >= 0) continue;
        // A socket on a bone of her body (hips, spine, legs: anything with a chain below it) is a
        // clothing / accessory mount, never a held item (owner, 0.16.1: the right arm dragged a leg —
        // LIVE sockets 2 / 6 / 7 were re-decided onto the right arm).
        uint32_t below = 0;
        for (uint32_t k = 0; k < r.count && below < 3; ++k)
            if (k != b && detail::Descendant(r.parents.data(), r.count, k, b)) ++below;
        if (below >= 3) continue;
        bool known = false;
        for (uint32_t i = 0; i < r.candCount && !known; ++i) known = r.cand[i].bone == b;
        if (known) continue;
        unsigned h = 0;
        if (r.hand[1].hand < r.count && r.hand[0].hand < r.count &&
            fp::Len(fp::Sub(Position(body[b]), Position(body[r.hand[1].hand]))) <
                fp::Len(fp::Sub(Position(body[b]), Position(body[r.hand[0].hand]))))
            h = 1;
        auto& c = r.cand[r.candCount++];
        c = {};
        c.bone = b;
        c.hand = uint8_t(h);
        Log("FPBODY prop socket bone %u joins the arm-helper learning (hand %u)", b, h);
        // 0.16.41 adult (owner: "items should appear right as F1 appears"): an item whose socket is in a hand
        // already (within 10 cm of the wrist, scaled) is held by it from the first frame; the learning
        // below keeps checking it (every ~0.3 s on the adult) and moves or drops it if that was wrong.
        if (r.adult && r.hand[h].hand < r.count &&
            fp::Len(fp::Sub(Position(body[b]), Position(body[r.hand[h].hand]))) < 0.10f * r.armScale) {
            r.attachDriver[b] = int16_t(r.hand[h].hand);
            c.decided = true;
            Log("FPBODY prop socket bone %u -> %s hand at once (adult)", b, h ? "right" : "left");
        }
    }
    if (!r.candCount) return;
    uint32_t attached = 0, decided = 0;
    for (uint32_t i = 0; i < r.candCount; ++i) {
        auto& c = r.cand[i];
        if (c.decided) {
            // Prop sockets are re-checked every ~2 s: an item is picked up, put down or changes hands
            // (owner, 2026-10-11: learned during an F3 cutscene, the purse's socket never went to
            // the left hand and was never looked at again). The current attachment stays meanwhile.
            if (!g_socketBone[c.bone] || ++c.sinceDecided < (r.adult ? 30u : 120u)) {
                ++decided;
                attached += r.attachDriver[c.bone] >= 0;
                continue;
            }
            c.decided = false;
            c.samples = 0;
            c.sinceDecided = 0;
            for (float& a : c.maxAngle) a = 0.f;
            if (r.hand[0].hand < r.count && r.hand[1].hand < r.count)
                c.hand = fp::Len(fp::Sub(Position(body[c.bone]), Position(body[r.hand[1].hand]))) <
                                 fp::Len(fp::Sub(Position(body[c.bone]), Position(body[r.hand[0].hand])))
                             ? 1
                             : 0;
        }
        const HandRig& hr = r.hand[c.hand];
        const uint32_t frames[4] = {hr.upper, hr.fore, hr.hand, 0};
        for (int k = 0; k < 4; ++k) {
            Mat4 inv;
            if (!detail::AffineInverse(body[frames[k]], inv)) continue;
            const Vec3 local = pose::TransformPoint(Position(body[c.bone]), inv);
            if (!c.samples) { c.lo[k] = c.hi[k] = local; }
            c.lo[k] = {std::min(c.lo[k].x, local.x), std::min(c.lo[k].y, local.y), std::min(c.lo[k].z, local.z)};
            c.hi[k] = {std::max(c.hi[k].x, local.x), std::max(c.hi[k].y, local.y), std::max(c.hi[k].z, local.z)};
        }
        for (int k = 0; k < 3; ++k) {
            float rel[9];
            if (!RelativeRotation(body[c.bone], body[frames[k]], rel)) continue;
            if (!c.samples) std::memcpy(c.rel0[k], rel, sizeof(rel));
            else c.maxAngle[k] = std::max(c.maxAngle[k], RotationAngle(rel, c.rel0[k]));
        }
        ++c.samples;
        if (fp::Len(fp::Sub(c.hi[3], c.lo[3])) < 0.06f) continue;  // has not moved against the body yet
        const Vec3 p = Position(body[c.bone]);
        const Vec3 joints[4] = {Position(body[hr.upper]), Position(body[hr.fore]), Position(body[hr.hand]),
                                hr.index < r.count ? Position(body[hr.index]) : Position(body[hr.hand])};
        auto segDist = [&](int k) {
            const Vec3 a = joints[k], b = joints[k + 1], ab = fp::Sub(b, a);
            const float t = std::clamp(fp::Dot(fp::Sub(p, a), ab) / std::max(fp::Dot(ab, ab), 1e-6f), 0.f, 1.f);
            return fp::Len(fp::Sub(p, fp::Add(a, fp::Mul(ab, t))));
        };
        auto spread = [&](int k) { return fp::Len(fp::Sub(c.hi[k], c.lo[k])); };
        int best = -1;
        if (g_socketBone[c.bone]) {
            // Prop sockets (a held item hangs off them) belong to the hand when they move with it.
            if (spread(2) < 0.02f) best = 2;
        } else {
            // 0.16.15: the arm bone the helper follows exactly, in position (1 cm) AND rotation (1.5 deg).
            // Owner 0.16.14 (young Jodie): "the sleeves move with the right wrist; the left shoulder is
            // gone". LIVE: the twins right 204 / left 216 (0.5 cm from the wrist) went to the hand / the
            // forearm, left 205-207 (at the elbow) to the forearm. Near a joint a position test cannot
            // tell the two bones apart (it barely moves either way, so the right arm's nearest-segment
            // and the left arm's most-rigid rules guessed), but a helper copying the forearm keeps its
            // rotation in the forearm's frame exactly and turns in the hand's whenever the wrist bends.
            // Adult (0.16.41, owner: "both of her hands are stretched at the elbow"): her helpers sit further
            // from the bone lines (LIVE: sleeve helpers 235 / 242 at 9.5 - 9.7 cm, over the 8 cm cap) and the
            // upper-arm twist bones (40: 1.1 cm on the upper arm, turning 16 deg with the forearm; 44: 0.6 cm,
            // 6.7 deg) failed the tiers, so they stayed animated and the skin stretched to them at the elbow.
            // Distances scale with her arm (x1.41); a bone that stays on one arm bone within 1.5 cm (scaled)
            // goes with it even when it turns against it.
            const float sc = r.adult ? r.armScale : 1.f;
            int first = -1, second = -1;
            for (int k = 0; k < 3; ++k) {
                if (spread(k) >= 0.012f * sc) continue;
                if (first < 0 || c.maxAngle[k] < c.maxAngle[first]) { second = first; first = k; }
                else if (second < 0 || c.maxAngle[k] < c.maxAngle[second]) second = k;
            }
            const float a1 = first >= 0 ? c.maxAngle[first] : 9.f, a2 = second >= 0 ? c.maxAngle[second] : 9.f;
            // Keep watching until one bone is clearly the driver (0.5 deg apart) or ~10 s have passed.
            if (a2 - a1 < 0.0087f && c.samples < 900) continue;
            constexpr float kDeg = 57.29578f;
            // 0.16.18 (owner: "the wrist stays as part of the body ... stretched all the way to the
            // motion-controlled hands"). LIVE 0.16.17: wrist helpers 218 / 219 / 221 / 222 follow the
            // hand within 0.1-0.3 cm but turn 1.5-2.7 deg against it, and the forearm twist bones
            // (196, 201-203, 208, 213-215) sit exactly on the forearm (0.0-0.1 cm) while taking part
            // of the hand's roll: left animated, the skin stretched from them to the IK hand.
            // Tiers: (1) exact driver: rotation within 1.5 deg (position within 1.2 cm);
            // (2) a bone it stays on in position (< 0.6 cm): the tightest one, then the least turn;
            // (3) a near driver: rotation within 6 deg. Otherwise it is not an arm helper.
            int exactPos = -1;
            for (int k = 0; k < 3; ++k)
                if (spread(k) < 0.006f * sc &&
                    (exactPos < 0 || spread(k) < spread(exactPos) - 0.001f ||
                     (spread(k) < spread(exactPos) + 0.001f && c.maxAngle[k] < c.maxAngle[exactPos])))
                    exactPos = k;
            if (first >= 0 && a1 < 0.026f) {
                best = first;
            } else if (exactPos >= 0) {
                best = exactPos;
            } else if (first >= 0 && a1 < 0.105f) {
                best = first;
            } else if (r.adult) {
                int tight = -1;
                for (int k = 0; k < 3; ++k)
                    if (spread(k) < 0.015f * sc && (tight < 0 || spread(k) < spread(tight))) tight = k;
                best = tight;
            }
            if (best < 0) {
                Log("FPBODY helper bone %u not attached: follows no single %s arm bone (upper %.1f / fore %.1f / hand "
                    "%.1f deg, %.1f / %.1f / %.1f cm)", c.bone, c.hand ? "right" : "left", c.maxAngle[0] * kDeg,
                    c.maxAngle[1] * kDeg, c.maxAngle[2] * kDeg, spread(0) * 100.f, spread(1) * 100.f, spread(2) * 100.f);
            }
        }
        c.decided = true;
        ++decided;
        const float bestDist = best >= 0 ? segDist(best) : 0.f;
        // A helper sits on the arm (LIVE 0.16.13: bones 6 / 243, low in her leg chain, were attached to
        // the left hand from 9.3 cm and the left arm moved her left foot). Arm helpers seen: <= 7.5 cm.
        if (best >= 0 && !g_socketBone[c.bone] && bestDist > 0.08f * (r.adult ? r.armScale : 1.f)) {
            Log("FPBODY helper bone %u not attached: %.1f cm from the arm", c.bone, bestDist * 100.f);
            best = -1;
        }
        // A helper must be closer to the arm than to her spine (owner, 0.16.1: lifting the left arm
        // pulled the left chest; LIVE bones 30 / 31 were attached to the left forearm from 15 cm).
        if (best >= 0 && !g_socketBone[c.bone]) {
            float spine = 1e9f;
            for (uint32_t a = r.head < r.count ? r.parents[r.head] : r.count; a < r.count && r.parents[a] < r.count;
                 a = r.parents[a]) {
                const Vec3 x = Position(body[a]), y = Position(body[r.parents[a]]), xy = fp::Sub(y, x);
                const float t = std::clamp(fp::Dot(fp::Sub(p, x), xy) / std::max(fp::Dot(xy, xy), 1e-6f), 0.f, 1.f);
                spine = std::min(spine, fp::Len(fp::Sub(p, fp::Add(x, fp::Mul(xy, t)))));
            }
            if (spine < bestDist) {
                Log("FPBODY helper bone %u stays with the body (%.1f cm from the spine, %.1f cm from the arm)", c.bone,
                    spine * 100.f, bestDist * 100.f);
                best = -1;
            }
        }
        if (g_socketBone[c.bone]) {  // re-decided: attach to a hand, move to the other hand, or release
            // Only an item actually at the hand (LIVE 0.16.2: shoe mounts on her feet, bones 3 / 7, were
            // put on the right hand and the arm pulled her left foot).
            const bool atHand = fp::Len(fp::Sub(p, Position(body[hr.hand]))) < 0.25f;
            const int16_t driver = best == 2 && atHand ? int16_t(frames[best]) : int16_t(-1);
            if (driver != r.attachDriver[c.bone])
                Log("FPBODY prop socket bone %u -> %s", c.bone,
                    driver < 0 ? "not held" : c.hand ? (best == 2 ? "right hand" : "right arm") : (best == 2 ? "left hand" : "left arm"));
            r.attachDriver[c.bone] = driver;
            attached += driver >= 0;
            continue;
        }
        if (best >= 0) {
            r.attachDriver[c.bone] = int16_t(frames[best]);
            ++attached;
            Log("FPBODY arm helper bone %u -> %s %s (%.1f cm from it; turned upper %.2f / fore %.2f / hand %.2f deg)",
                c.bone, c.hand ? "right" : "left", best == 0 ? "upper arm" : best == 1 ? "forearm" : "hand",
                bestDist * 100.f, c.maxAngle[0] * 57.29578f, c.maxAngle[1] * 57.29578f, c.maxAngle[2] * 57.29578f);
        }
    }
    if (attached != g_status.attachedBones || (decided == r.candCount && !r.attachLearned)) {
        r.attachLearned = decided == r.candCount;
        Log("FPBODY arm helpers: %u attached, %u of %u candidates decided", attached, decided, r.candCount);
    }
    g_status.attachedBones = attached;
}

float g_targetLift = 0.f;  // vertical offset of the hand targets (ComputeEdits, F1 height dead zone)
// Which side of her shoulder line is her front, learned per skeleton (ComputeEdits).
struct TorsoFront {
    void* skeleton = nullptr;
    uint32_t same = 0, opposite = 0;
    bool known = false;
    float flip = 0.f;
} g_torso;
bool SolveArmImpl(unsigned h);
bool SolveArm(unsigned h) {
    const Rig& r = g_rig;
    const HandRig& hr = r.hand[h];
    if (hr.upper >= r.count || hr.fore >= r.count || hr.hand >= r.count) { g_status.armState[h] = 4; return false; }
    if (!g_plan.handValid[h] || !fp::ValidMatrix(g_plan.handWorld[h])) { g_status.armState[h] = 2; return false; }
    const bool ok = SolveArmImpl(h);
    g_status.armState[h] = ok ? 1 : 3;
    return ok;
}
bool SolveArmImpl(unsigned h) {
    const Rig& r = g_rig;
    const HandRig& hr = r.hand[h];
    const Mat4& grip = g_plan.handWorld[h];
    const Vec3 shoulder = Position(g_body[hr.upper]), elbow = Position(g_body[hr.fore]), wrist = Position(g_body[hr.hand]);
    // Wrist rotation: the rig's own hand frame (knuckles, thumb side) carried onto the
    // controller's (OpenXR grip axes). Without fingers the animation's wrist rotation stays.
    Mat4 wristRot = Rotation(g_body[hr.hand]);
    Mat4 target, rigBasis;
    const bool haveTarget = fp::GripHandBasis(grip, g_plan.wristPitch, &target);
    if (hr.anatomical && haveTarget && RigHandBasis(hr, g_body.data(), &rigBasis))
        wristRot = fp::RetargetRotation(Rotation(g_body[hr.hand]), rigBasis, target);
    // The grip position is the palm centroid (OpenXR spec): the wrist is half a palm behind it.
    // The hand goes where the controller is (owner: scaling the reach made the arms too short).
    // LIVE young Jodie: arm 0.34 m, controllers 0.4-0.65 m from her shoulder -> the arm is
    // stretched (bone lengths scaled, up to 2x) instead of stopping short of the controller.
    const float upperLen = fp::Len(fp::Sub(elbow, shoulder)), lowerLen = fp::Len(fp::Sub(wrist, elbow));
    Vec3 wristTarget = Position(grip);
    wristTarget.y += g_targetLift;  // the vertical follow the body skipped (feet planted)
    // Hands exactly at the controllers (owner, 2026-10-08: scaled reach put her hands away from the
    // real hands; stretching over-extended her arms). Beyond her reach the arm stays straight.
    g_status.armScale = 1.f;
    if (haveTarget)
        wristTarget = fp::Sub(wristTarget, fp::Mul(Vec3{target.m[0], target.m[1], target.m[2]},
                                                   (hr.anatomical ? hr.palmLength * 0.5f : 0.045f)));
    const auto solve = fp::TwoBone(shoulder, wristTarget, upperLen, lowerLen,
                                   fp::ArmPole(g_plan.bodyYaw, h == 1));
    g_status.armReach[h] = fp::Len(fp::Sub(wristTarget, shoulder));
    g_status.armLength[h] = fp::Len(fp::Sub(elbow, shoulder)) + fp::Len(fp::Sub(wrist, elbow));
    g_status.armClamped[h] = solve.targetClamped;
    if (!solve.valid) return false;
    g_arm = g_work;
    const auto upperWorld = PointSegment(g_body[hr.upper], fp::Sub(elbow, shoulder), fp::Sub(solve.elbow, shoulder));
    if (!detail::RebuildDescendants(g_body.data(), r.parents.data(), r.count, hr.upper, upperWorld, g_arm.data())) return false;
    auto foreWorld = PointSegment(g_body[hr.fore], fp::Sub(wrist, elbow), fp::Sub(solve.wrist, solve.elbow));
    SetPosition(foreWorld, solve.elbow);
    // Forearm twist (owner: "something around the wrist doesn't move"): when the controller rolls
    // the hand about the forearm, the forearm takes half of that roll, so the wrist skin turns
    // with the hand instead of wringing.
    {
        const Vec3 axis = fp::Norm(fp::Sub(solve.wrist, solve.elbow));
        Mat4 foreInv;
        if (fp::Finite(axis) && detail::AffineInverse(Rotation(g_body[hr.fore]), foreInv)) {
            const Mat4 rel0 = pose::Mul(Rotation(g_body[hr.hand]), foreInv);  // hand relative to forearm
            const Mat4 predicted = pose::Mul(rel0, Rotation(foreWorld));    // hand with no extra roll
            int row = 0;
            float best = 2.f;
            for (int k = 0; k < 3; ++k) {
                const float d = std::fabs(fp::Dot({predicted.m[k * 4], predicted.m[k * 4 + 1], predicted.m[k * 4 + 2]}, axis));
                if (d < best) { best = d; row = k; }
            }
            auto project = [&](const Mat4& m) {
                const Vec3 v{m.m[row * 4], m.m[row * 4 + 1], m.m[row * 4 + 2]};
                return fp::Norm(fp::Sub(v, fp::Mul(axis, fp::Dot(v, axis))));
            };
            const Vec3 a = project(predicted), b = project(wristRot);
            const float roll = std::atan2(fp::Dot(fp::Cross(a, b), axis), fp::Dot(a, b));
            if (fp::Finite(a) && fp::Finite(b) && std::isfinite(roll)) {
                // Half the roll, both arms alike. (Owner 0.16.13, young Jodie: the right arm's 35 deg cap
                // let the hand and its cuff turn past the sleeve and showed her real wrist "at certain
                // degrees"; the uncapped left wrist was right: "copy the left wrist to the right".)
                // 0.16.29 (owner: "sometimes the whole right arm is stretched"; LIVE right hand turned up to
                // 147 deg from the animation, so a 73 deg forearm twist, left at most ~45 deg): at most
                // 50 deg on both arms, which never limits the left as seen so far.
                const float share = std::clamp(roll * 0.5f, -0.8727f, 0.8727f);
                const float half = share * 0.5f;  // quaternion half-angle
                const pose::Quat q{axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half)};
                foreWorld = pose::Mul(foreWorld, pose::FromPose(q, {0, 0, 0}));
                SetPosition(foreWorld, solve.elbow);
            }
        }
    }
    if (!detail::RebuildDescendants(g_body.data(), r.parents.data(), r.count, hr.fore, foreWorld, g_arm.data())) return false;
    Mat4 wristWorld = wristRot;
    for (int row = 0; row < 3; ++row) {  // keep the authored bone scale
        const float scale = fp::Len({g_body[hr.hand].m[row * 4], g_body[hr.hand].m[row * 4 + 1], g_body[hr.hand].m[row * 4 + 2]});
        for (int col = 0; col < 3; ++col) wristWorld.m[row * 4 + col] *= scale;
    }
    SetPosition(wristWorld, solve.wrist);
    if (!detail::RebuildDescendants(g_body.data(), r.parents.data(), r.count, hr.hand, wristWorld, g_arm.data())) return false;
    // The finger group (LC_*_Hand) follows the hand rigidly, in a pose captured once (not the
    // animation's current finger motion).
    if (hr.lc < r.count) {
        Mat4 inverse;
        if (!detail::AffineInverse(g_body[hr.hand], inverse)) return false;
        Rig& rig = g_rig;
        if (!rig.fingerRelValid[h]) {
            for (uint32_t i = 0; i < r.count; ++i)
                if (r.carried[h][i]) rig.fingerRel[h][i] = pose::Mul(g_body[i], inverse);
            rig.fingerRelValid[h] = true;
        }
        for (uint32_t i = 0; i < r.count; ++i)
            if (r.carried[h][i]) g_arm[i] = pose::Mul(rig.fingerRel[h][i], g_arm[hr.hand]);
    }
    // Learned helpers (wrist / sleeve / twist bones outside the arm subtree) follow their driver.
    for (uint32_t i = 0; i < r.count; ++i) {
        const int16_t d = r.attachDriver[i];
        if (d < 0) continue;
        if (uint32_t(d) != hr.upper && uint32_t(d) != hr.fore && uint32_t(d) != hr.hand) continue;
        Mat4 inv;
        if (!detail::AffineInverse(g_body[d], inv)) continue;
        g_arm[i] = pose::Mul(pose::Mul(g_body[i], inv), g_arm[d]);
    }
    {   // diagnostic: how far the drawn hand is turned from the animated one
        const Mat4 a = Rotation(g_body[hr.hand]), b = Rotation(g_arm[hr.hand]);
        const float tr = a.m[0] * b.m[0] + a.m[1] * b.m[1] + a.m[2] * b.m[2] + a.m[4] * b.m[4] + a.m[5] * b.m[5] +
                         a.m[6] * b.m[6] + a.m[8] * b.m[8] + a.m[9] * b.m[9] + a.m[10] * b.m[10];
        g_status.handTurn[h] = std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f)) * 57.29578f;
        g_status.handRetargeted[h] = hr.anatomical && haveTarget;
    }
    g_work = g_arm;
    return true;
}

// Idle hold (F1 standing; owner, 2026-10-11: the idle animation sways her body enough to break the
// arm IK). While she stands still, the drawn body keeps a snapshot of her pose relative to her root
// instead of the idle animation. A real movement (root moving 8 cm, or bone 0 / head more than 25 cm
// from the snapshot: walking, sitting, kneeling, bending) releases it at once; it is taken again
// after 0.4 s of standing still. 0.15 s blends both ways.
// 0.16.13 (owner: young Jodie still swayed): stillness is the root staying within 3 cm for 0.4 s,
// and the hold ends when the root is 8 cm from where it was taken. (A per-frame root speed under
// 0.05 m/s read a few millimetres of character-controller jitter per 11 ms frame as movement.)
struct IdleHold {
    void* skeleton = nullptr;
    alignas(16) std::array<Mat4, detail::kMaxBones> rel{};
    bool holding = false;
    float blend = 0.f, still = 0.f;
    Vec3 anchor{};       // where the still window started / the hold was taken
    bool anchored = false;
    LARGE_INTEGER last{};
    uint64_t lastHoldLog = 0, lastReleaseLog = 0;
} g_idle;
alignas(16) std::array<Mat4, detail::kMaxBones + 1> g_heldPose;

Vec3 g_standingHead{};              // her head in her root's frame when last held standing
void* g_standingSkeleton = nullptr;

// Row-wise blend of two affine matrices, re-orthonormalised with each row's blended length.
Mat4 BlendAffine(const Mat4& a, const Mat4& b, float t) {
    Mat4 m = a;
    Vec3 row[3];
    float len[3];
    for (int k = 0; k < 3; ++k) {
        const Vec3 ra{a.m[k * 4], a.m[k * 4 + 1], a.m[k * 4 + 2]}, rb{b.m[k * 4], b.m[k * 4 + 1], b.m[k * 4 + 2]};
        row[k] = fp::Add(fp::Mul(ra, 1.f - t), fp::Mul(rb, t));
        len[k] = fp::Len(ra) * (1.f - t) + fp::Len(rb) * t;
    }
    const Vec3 x = fp::Norm(row[0]);
    const Vec3 y = fp::Norm(fp::Sub(row[1], fp::Mul(x, fp::Dot(row[1], x))));
    Vec3 z = fp::Cross(x, y);
    if (fp::Dot(z, row[2]) < 0.f) z = fp::Mul(z, -1.f);
    if (!fp::Finite(x) || !fp::Finite(y) || fp::Len(x) < 0.5f || fp::Len(y) < 0.5f) return t < 0.5f ? a : b;
    const Vec3 axes[3] = {x, y, z};
    for (int k = 0; k < 3; ++k) {
        m.m[k * 4] = axes[k].x * len[k];
        m.m[k * 4 + 1] = axes[k].y * len[k];
        m.m[k * 4 + 2] = axes[k].z * len[k];
    }
    SetPosition(m, fp::Add(fp::Mul(Position(a), 1.f - t), fp::Mul(Position(b), t)));
    return m;
}

// The pose the F1 body is built from: the clean pose, or the held idle snapshot (blended).
const Mat4* IdleSource(const Mat4* clean, uint32_t count) {
    IdleHold& h = g_idle;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = h.last.QuadPart ? std::clamp(float(double(now.QuadPart - h.last.QuadPart) / double(freq.QuadPart)), 0.f, 0.25f)
                                     : 0.f;
    h.last = now;
    const Rig& r = g_rig;
    Mat4 rootInv;
    if (!g_plan.turnBody || !detail::AffineInverse(clean[count], rootInv) || h.skeleton != g_rig.skeleton) {
        h = IdleHold{};
        h.skeleton = g_rig.skeleton;
        h.last = now;
        return clean;
    }
    const Vec3 root = Position(clean[count]);
    auto flat = [](Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); };
    const uint64_t nowMs = GetTickCount64();
    if (h.holding) {
        // Bone 0 and the head only, 25 cm (sitting, kneeling, bending). LIVE 0.16.13: the young Jodie
        // idle ("swings her body left and right") moves her upper arms 12 cm; with the upper arms in
        // this test at 12 cm the hold let go of exactly the sway it is for.
        const uint32_t core[2] = {0, r.head};
        float worst = 0.f;
        uint32_t worstBone = 0;
        for (uint32_t b : core) {
            if (b >= count) continue;
            const Mat4 rel = pose::Mul(clean[b], rootInv);
            const float d = fp::Len(fp::Sub(Position(rel), Position(h.rel[b])));
            if (d > worst) { worst = d; worstBone = b; }
        }
        // 0.16.15: walking is the movement stick pushed (8 cm then). Without it, turning in place and
        // settling steps move her root a few cm (LIVE 0.16.14: holds let go within 1 s at 161-164 s
        // while the owner turned, and her body swung); only a 40 cm move (a gamepad walk) lets go.
        const float moved = flat(root, h.anchor);
        const float walkLimit = g_plan.moveStick ? 0.08f : 0.40f;
        if (moved > walkLimit || worst > 0.25f) {
            h.holding = false;
            h.still = 0.f;
            h.anchored = false;
            if (nowMs - h.lastReleaseLog >= 1000) {
                h.lastReleaseLog = nowMs;
                Log("FPBODY idle hold released: %s (moved %.2f m, bone %u %.2f m from the held pose)",
                    moved > walkLimit ? "she moves" : "she sits / kneels / bends", moved, worstBone, worst);
            }
        }
    } else {
        // Standing: the root stays within 3 cm for 0.4 s, or within 10 cm while the stick is released.
        if (!h.anchored || flat(root, h.anchor) > (g_plan.moveStick ? 0.03f : 0.10f)) {
            h.anchor = root;
            h.anchored = true;
            h.still = 0.f;
        } else {
            h.still += dt;
        }
        if (h.still >= 0.4f) {
            for (uint32_t i = 0; i < count; ++i) h.rel[i] = pose::Mul(clean[i], rootInv);
            h.holding = true;
            if (r.head < count) {  // her standing head, for fpcam's interaction check (HeadOffStanding)
                g_standingHead = pose::TransformPoint(Position(clean[r.head]), rootInv);
                g_standingSkeleton = g_rig.skeleton;
            }
            if (nowMs - h.lastHoldLog >= 1000) {
                h.lastHoldLog = nowMs;
                Log("FPBODY idle hold: standing still, idle animation held");
            }
        }
    }
    g_status.idleHeld = h.holding;
    const float target = h.holding ? 1.f : 0.f;
    h.blend += (target - h.blend) * (1.f - std::exp(-dt / 0.15f));
    if (h.blend < 0.01f && !h.holding) {
        h.blend = 0.f;
        return clean;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const Mat4 held = pose::Mul(h.rel[i], clean[count]);
        g_heldPose[i] = h.blend > 0.99f ? held : BlendAffine(clean[i], held, h.blend);
        if (!fp::ValidMatrix(g_heldPose[i])) g_heldPose[i] = clean[i];
    }
    g_heldPose[count] = clean[count];
    return g_heldPose.data();
}

// Hair that is not under her head in the skeleton (owner 0.16.22: young Jodie's hair still shows in
// F1 / F3; LIVE: the hide set by distance and its re-checks never caught it, and only ever added arm /
// shoulder helpers). What only hair (and earrings) does: it keeps its place in her HEAD's frame while
// her head turns against her body. Bones within 30 cm of her head are tracked in the head's frame and in
// her root's frame; one that stays within 4 cm in the head's frame while moving 6 cm or more against
// her body is hidden with its subtree. Arm, shoulder and collar bones move against the head when it
// turns, so they never qualify.
struct HeadFollow {
    void* skeleton = nullptr;
    std::array<Vec3, detail::kMaxBones> headLo{}, headHi{}, bodyLo{}, bodyHi{};
    std::array<uint16_t, detail::kMaxBones> samples{};
    std::array<bool, detail::kMaxBones> decided{};
    uint32_t tick = 0;
} g_headFollow;

void LearnHeadFollowers(const Mat4* pose, uint32_t count) {
    Rig& r = g_rig;
    HeadFollow& f = g_headFollow;
    if (f.skeleton != r.skeleton) {
        f = HeadFollow{};
        f.skeleton = r.skeleton;
    }
    if (++f.tick % 3 || r.head >= count) return;
    Mat4 headInv, rootInv;
    if (!detail::AffineInverse(pose[r.head], headInv) || !detail::AffineInverse(pose[count], rootInv)) return;
    const Vec3 head = Position(pose[r.head]);
    uint32_t protect[2] = {r.hand[0].upper, r.hand[1].upper};
    for (uint32_t b = 0; b < count; ++b) {
        if (f.decided[b] || r.hideTree[b] || r.carried[0][b] || r.carried[1][b] || r.attachDriver[b] >= 0) continue;
        const Vec3 p = Position(pose[b]);
        if (!f.samples[b] && fp::Len(fp::Sub(p, head)) > 0.30f) continue;
        // never her spine / neck chain or the arms (ancestors of the head or of the upper arms)
        if (detail::Descendant(r.parents.data(), count, r.head, b)) { f.decided[b] = true; continue; }
        bool armChain = false;
        for (uint32_t u : protect)
            armChain |= u < count && (detail::Descendant(r.parents.data(), count, u, b) || detail::Descendant(r.parents.data(), count, b, u));
        if (armChain) { f.decided[b] = true; continue; }
        const Vec3 inHead = pose::TransformPoint(p, headInv), inBody = pose::TransformPoint(p, rootInv);
        if (!f.samples[b]) {
            f.headLo[b] = f.headHi[b] = inHead;
            f.bodyLo[b] = f.bodyHi[b] = inBody;
        }
        auto grow = [](Vec3& lo, Vec3& hi, Vec3 v) {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        };
        grow(f.headLo[b], f.headHi[b], inHead);
        grow(f.bodyLo[b], f.bodyHi[b], inBody);
        if (f.samples[b] < 60000) ++f.samples[b];
        const float inHeadSpread = fp::Len(fp::Sub(f.headHi[b], f.headLo[b]));
        const float againstBody = fp::Len(fp::Sub(f.bodyHi[b], f.bodyLo[b]));
        if (inHeadSpread >= 0.04f) { f.decided[b] = true; continue; }  // moves against her head: not hair
        if (f.samples[b] < 60 || againstBody < 0.06f) continue;          // her head has not turned enough yet
        f.decided[b] = true;
        uint32_t added = 0;
        for (uint32_t d = 0; d < count; ++d)
            if (!r.hideTree[d] && !r.carried[0][d] && !r.carried[1][d] && r.attachDriver[d] < 0 &&
                (d == b || detail::Descendant(r.parents.data(), count, d, b))) {
                r.hideTree[d] = true;
                ++added;
            }
        r.hidden += added;
        g_status.hiddenBones = r.hidden;
        Log("FPBODY bone %u follows her head (%.1f cm in the head's frame, %.1f cm against her body): hidden as hair "
            "with %u bone(s), %u hidden now", b, inHeadSpread * 100.f, againstBody * 100.f, added, r.hidden);
    }
}

bool ComputeEdits() {
    const Rig& r = g_rig;
    auto& o = g_overlay;
    const uint32_t count = r.count;
    if (!g_plan.firstPerson || !fp::Finite(g_plan.eyePivot) || !std::isfinite(g_plan.bodyYaw) ||
        !std::isfinite(g_plan.pivotRight) || !std::isfinite(g_plan.pivotBack) || std::fabs(g_plan.pivotRight) > 1.f ||
        std::fabs(g_plan.pivotBack) > 1.f || fp::Len(fp::Sub(g_plan.eyePivot, Position(o.clean[r.head]))) > 10.f) {
        g_status.lastFailure = 3;
        return false;
    }
    Mat4 delta = pose::Identity();
    g_targetLift = 0.f;
    float authoredYaw = std::atan2(o.clean[count].m[8], o.clean[count].m[0]);
    // The root's yaw says where -Z points; LIVE the character faces the other way (+Z), which
    // turned the drawn body 180 deg from the view. Her face (head -> eye midpoint) decides which
    // of the two root directions is her front.
    float faceYaw = 0.f;
    if (g_anchors.eye[0] < count && g_anchors.eye[1] < count && r.head < count &&
        fp::LookYaw(Position(o.clean[r.head]), Position(o.clean[g_anchors.eye[0]]), Position(o.clean[g_anchors.eye[1]]),
                    &faceYaw) &&
        std::fabs(std::remainder(faceYaw + fp::BodyFlip() - authoredYaw, 6.28318531f)) > 1.5707963f)
        authoredYaw += 3.14159265f;
    const Mat4* pose = IdleSource(o.clean.data(), count);
    // Owner 0.16.17: walking backwards "breaks the body ik for a few seconds, the whole body turns".
    // The game turns her round to walk the other way: her root takes the new heading at once and the
    // turn animation brings the body round over a second or two, so a body turned by the root's yaw
    // faced away from the view meanwhile. Her torso's own heading (square to the shoulder line, from
    // the pose actually drawn) is used once its front is learned from frames where it agrees with
    // the root / face heading (or is opposite to it); the learning restarts with each rig.
    if (r.hand[0].upper < count && r.hand[1].upper < count) {
        const Vec3 across = fp::Sub(Position(pose[r.hand[1].upper]), Position(pose[r.hand[0].upper]));
        if (across.x * across.x + across.z * across.z > 0.01f * 0.01f) {
            const float torso = std::atan2(-across.z, across.x);
            if (g_torso.skeleton != r.skeleton) g_torso = TorsoFront{r.skeleton};
            if (!g_torso.known) {
                const float d = std::fabs(std::remainder(torso - authoredYaw, 6.28318531f));
                if (d < 0.5f) ++g_torso.same;
                else if (d > 3.14159265f - 0.5f) ++g_torso.opposite;
                if (g_torso.same + g_torso.opposite >= 90) {
                    g_torso.known = true;
                    g_torso.flip = g_torso.opposite > g_torso.same ? 3.14159265f : 0.f;
                    Log("FPBODY torso heading learned (%u frames agree, %u opposite): the drawn body follows her torso",
                        g_torso.same, g_torso.opposite);
                }
            }
            if (g_torso.known) authoredYaw = std::remainder(torso + g_torso.flip, 6.28318531f);
        }
    }
    if (g_plan.turnBody)
        delta = detail::BodyTransform(authoredYaw, g_plan.bodyYaw, g_plan.eyePivot, g_plan.pivotRight, g_plan.pivotBack);
    g_status.authoredRootYaw = authoredYaw;
    for (uint32_t i = 0; i < count; ++i) {
        g_body[i] = g_plan.turnBody ? pose::Mul(pose[i], delta) : o.clean[i];
        // A bone the engine itself scaled to zero (hidden accessory) is left exactly as authored.
        if (!fp::ValidMatrix(g_body[i])) { g_body[i] = o.clean[i]; g_status.lastFailure = 5; }
    }
    g_body[count] = o.clean[count];
    // Keep her head under the camera (owner: "I see the body in front of me when I start
    // moving"): the eye is a calibrated standing position, the walking animation leans her head
    // forward. Shift the drawn body horizontally so her head is where the eye expects it.
    // EXP-036 (LIVE 0.10.5): with the body anchored to the calibrated eye, the controllers sat
    // ~1.1 m from her shoulders whenever the player's real head was away from the recentre pose
    // (sitting, leaning): the arm could never reach and the hand / wrist stayed animated. The
    // drawn body now follows the actual headset position (3D), so her shoulders are always under
    // the player's head and the controllers are within her reach.
    if (g_plan.turnBody && r.head < count) {
        const Vec3 fwd = pose::TransformPoint({0, 0, -1}, pose::FromPose(pose::YawQuat(g_plan.bodyYaw), {0, 0, 0}));
        const Vec3 eye = fp::Finite(g_plan.headPos) ? g_plan.headPos : g_plan.eyePivot;
        const Vec3 want = fp::Sub(fp::Sub(eye, fp::Mul(fwd, g_plan.eyeForward)), Vec3{0.f, g_plan.eyeUp, 0.f});
        const Vec3 head = Position(g_body[r.head]);
        Vec3 shift = fp::Sub(want, head);
        // Owner 0.16.12: young Jodie and adult Jodie in the red dress stood too low, feet through the
        // floor: any head height below the calibrated eye sank the whole body. Within 20 cm her feet
        // stay planted; only a larger difference (sitting, crouching) moves her, by the excess.
        g_status.headDeltaY = shift.y;
        const float followed = std::clamp(shift.y, -0.8f, 0.4f);  // the full follow (0.16.12)
        shift.y = shift.y > 0.2f ? shift.y - 0.2f : shift.y < -0.2f ? shift.y + 0.2f : 0.f;
        shift.y = std::clamp(shift.y, -0.6f, 0.2f);
        g_status.bodyShiftY = shift.y;
        // Owner 0.16.13: with her shoulders no longer lowered with the body, the controllers were out
        // of her reach (LIVE reach 0.41-0.44 m vs 0.34 m arm, "the whole arms look stretched"). The
        // hand targets take the part of the follow the body skipped, so her arms bend as before.
        g_targetLift = shift.y - followed;
        if (fp::Finite(shift) && fp::Len(shift) < 1.5f)
            for (uint32_t i = 0; i < count; ++i) SetPosition(g_body[i], fp::Add(Position(g_body[i]), shift));
    }
    g_work = g_body;
    // Helpers are learned in any first person, F3 cutscenes included, so they are known before the
    // arms first follow the controllers (owner 0.16.20: "when I first got into F1 the wrists broke for
    // a second, then they were good" = the learning time). 0.16.14 learned in F1 only because in an F3
    // cutscene two foot bones were taken for hand helpers by position alone (9.3 cm away); the 0.16.15+
    // rules (exact rotation / position within 0.6 cm, never beyond 8 cm) refuse those.
    if (g_plan.firstPerson) LearnAttachments(g_body.data());
    if (g_plan.armsActive) {
        SolveArm(0);
        SolveArm(1);
    } else {
        g_status.armState[0] = g_status.armState[1] = 0;
    }
    // (0.16.18-0.16.20 re-checked the hide set over time and around her eyes. LIVE 0.16.20: it only ever
    // added arm / shoulder helpers that came near her face in cutscene poses (175, 181, 188, 240, 244),
    // never hair: young Jodie's hair is not her skeleton's bones. Removed; the set is the build-time one.)
    LearnHeadFollowers(o.clean.data(), count);
    // Collapse the head, face and hair bones (detail::HeadHideSet). Keeping the translation leaves
    // a valid affine matrix for the skinning code; the tiny basis shrinks everything bound to them.
    // EXP-036: the edited pose only feeds Havok's skinning matrices (render), never the skeleton,
    // so no game system reads these bones and a near-zero basis is safe again.
    Mat4 hidden = g_work[r.collapseAt < count ? r.collapseAt : r.head];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) hidden.m[row * 4 + col] *= 0.001f;
    for (uint32_t i = 0; i < count; ++i) {
        o.edited[i] = r.hideTree[i] ? hidden : g_work[i];
        o.changed[i] = std::memcmp(&o.edited[i], &o.clean[i], sizeof(Mat4)) != 0;
    }
    return true;
}


// In-place edit of a skeleton's world array (+0x10). SEH: the array can be freed under us.
bool WriteWorlds(Mat4* worlds, uint32_t count, const Mat4* edited, const bool* changed) {
    __try {
        for (uint32_t i = 0; i < count; ++i)
            if (changed[i]) worlds[i] = edited[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Jodie's skeleton was just built: compute the edits from the clean pose and write them.
// `rebuild`: a later build in the same frame. It may rebuild only part of the array, so a bone
// still holding our edited value keeps its earlier clean value; only bones the engine rewrote are
// clean again. (0.9.3 re-read the whole array and took edited / collapsed bones for clean ->
// invalid matrices, failure 5, no arms.)
std::array<Mat4, detail::kMaxBones + 1> g_current;
void ApplyInPlace(void* skel, bool rebuild) {
    SkeletonView v{};
    g_status.lastFailure = 0;  // per frame: what went wrong in this application, if anything
    if (!ReadSkeleton(skel, &v)) { g_status.lastFailure = 1; return; }
    auto& o = g_overlay;
    const size_t bytes = (size_t(v.count) + 1) * sizeof(Mat4);
    if (rebuild && o.ready && o.skeleton == skel && o.count == v.count) {
        if (!CopyMem(g_current.data(), v.worlds, bytes)) return;
        for (uint32_t i = 0; i <= v.count; ++i)
            if (i == v.count || !o.changed[i] || std::memcmp(&g_current[i], &o.edited[i], sizeof(Mat4)) != 0)
                o.clean[i] = g_current[i];
    } else if (!CopyMem(o.clean.data(), v.worlds, bytes)) {
        return;
    }
    o.ready = false;
    // Already corrupt (non-finite bones): never edit on top of it.
    for (uint32_t i = 0; i <= v.count; ++i) {
        if (!fp::Finite(Position(o.clean[i])) || !std::isfinite(o.clean[i].m[0])) {
            g_status.lastFailure = 6;
            IssueOnce(64, "Jodie's pose is non-finite (corrupted game state); body edits paused");
            return;
        }
    }
    o.skeleton = skel;
    o.count = v.count;
    o.serial = g_planSerial;
    if (g_rig.skeleton != skel || g_rig.count != v.count) BuildRig(skel, v, o.clean.data());
    if (!g_rig.valid || !ComputeEdits()) return;
    o.ready = true;
    ++g_status.captureValid;
    if (!WriteWorlds(const_cast<Mat4*>(v.worlds), v.count, o.edited.data(), o.changed.data())) {
        g_status.lastFailure = 4;
        return;
    }
    ++g_status.applyWrites;
    if (!g_status.firstSuccess) {
        g_status.firstSuccess = true;
        Log("FPBODY LIVE first-person pose written to the skeleton: skeleton=%p bones=%u hidden=%u arms=%d turn=%d", skel,
            v.count, g_rig.hidden, int(g_plan.armsActive), int(g_plan.turnBody));
    }
}

// One plan per animation frame (owner: "copies" of body / hands while turning): every element of
// a frame is drawn from the plan captured at that frame's final build, never a mix of two plans.
Plan g_framePlan;
uint64_t g_framePlanSerial = 0;
bool g_framePlanValid = false;
const void* g_finalWorlds = nullptr;  // world array edited at this frame's final build
bool g_afterFinal = false;
std::atomic<bool> g_restoreAtPublish{true};
std::atomic<bool> g_editsOn{true};    // render-only edits through Havok skinning (EXP-036)
std::atomic<bool> g_inPlace{false};   // EXP-035 in-place skeleton writes: corrupt Jodie (NaN), dev only

// EXP-035: the edits are only for drawing. 0.9.3/0.9.4 left them in the skeleton, and the next
// frame's stateful systems (hair / cloth dynamics, look-at, IK) read the collapsed head and the
// turned body as their previous pose: glitching, then a corrupted (vanished) Jodie and a dead F1.
// Before the per-frame swap (which precedes animation) the clean values are put back wherever
// our edited value is still in place.
bool RestoreWorlds(Mat4* worlds, uint32_t count, const Mat4* clean, const Mat4* edited, const bool* changed) {
    __try {
        for (uint32_t i = 0; i < count; ++i)
            if (changed[i] && std::memcmp(&worlds[i], &edited[i], sizeof(Mat4)) == 0) worlds[i] = clean[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using PublishFn = void (*)(void*);
PublishFn g_publishOriginal = nullptr;
void HookPublish(void* skel) {
    if (skel && g_restoreAtPublish.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& o = g_overlay;
        SkeletonView v{};
        if (o.ready && o.skeleton == skel && ReadSkeleton(skel, &v) && v.worlds == g_finalWorlds && v.count == o.count) {
            if (RestoreWorlds(const_cast<Mat4*>(v.worlds), v.count, o.clean.data(), o.edited.data(), o.changed.data()))
                ++g_status.restores;
            o.ready = false;
            g_afterFinal = false;
        }
    }
    g_publishOriginal(skel);
}

// Clothes (owner, 2026-10-08: "the clothes don't move when I turn"): her clothes are Havok cloth
// whose bone input is this CLOTH_MATRIX_MAP (palette remap +0x8, count +0x10, destination +0x18),
// copied from the skeleton by MESH::PaletteCopy. Physics input only (never game logic): after the
// engine's copy, the edited pose (turned body, IK arms, hidden head) is written over it.
bool WriteClothPalette(void* palette, uint32_t boneCount, const Mat4* edited, const bool* changed) {
    __try {
        const uint8_t* remap = *reinterpret_cast<uint8_t**>(static_cast<char*>(palette) + 0x8);
        const uint32_t paletteCount = *reinterpret_cast<uint32_t*>(static_cast<char*>(palette) + 0x10);
        Mat4* dest = *reinterpret_cast<Mat4**>(static_cast<char*>(palette) + 0x18);
        if (!remap || !dest) return false;
        const uint32_t n = paletteCount < boneCount ? paletteCount : boneCount;
        for (uint32_t i = 0; i < n; ++i)
            if (changed[i] && remap[i] != 0xff) dest[remap[i]] = edited[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void HookPalette(void* palette, void* skeleton) {
    g_paletteOriginal(palette, skeleton);
    if (!palette || !skeleton || skeleton != g_active.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4 *a = nullptr, *b = nullptr;
    uint32_t count = 0;
    if (!g_editsOn.load() || !g_plan.firstPerson || skeleton != g_plan.skeleton || !SourceArrays(skeleton, &a, &b, &count))
        return;
    if (RenderSource(a) == a || !g_renderReady) return;
    if (WriteClothPalette(palette, count, g_overlay.edited.data(), g_overlay.changed.data())) ++g_status.clothWrites;
}

// Clean head and root for the camera, at the final animation build of the watched skeleton.
void CaptureAnchors(void* skel) {
    SkeletonView v{};
    auto& a = g_anchors;
    if (!ReadSkeleton(skel, &v)) { a.valid = false; return; }
    if (a.skeleton != skel || a.count != v.count) {
        a.skeleton = skel;
        a.count = v.count;
        a.head = HashIndex(skel, v.count, kHeadHash);
        a.eye[0] = HashIndex(skel, v.count, kEyeHash[0]);
        a.eye[1] = HashIndex(skel, v.count, kEyeHash[1]);
        for (int s = 0; s < 2; ++s) {
            a.upper[s] = HashIndex(skel, v.count, kUpperHash[s]);
            a.hand[s] = HashIndex(skel, v.count, kHandHash[s]);
        }
        a.tposeRun = a.normalRun = 0;
        a.tposed = false;
    }
    a.valid = a.head < v.count && CopyMem(&a.headWorld, &v.worlds[a.head], sizeof(Mat4)) &&
              CopyMem(&a.rootWorld, &v.worlds[v.count], sizeof(Mat4)) && fp::ValidMatrix(a.headWorld) &&
              fp::ValidMatrix(a.rootWorld);
    a.eyesValid = false;
    if (a.valid && a.eye[0] < v.count && a.eye[1] < v.count) {
        Mat4 l{}, r{};
        a.eyesValid = CopyMem(&l, &v.worlds[a.eye[0]], sizeof(Mat4)) && CopyMem(&r, &v.worlds[a.eye[1]], sizeof(Mat4)) &&
                      fp::Finite(Position(l)) && fp::Finite(Position(r));
        a.eyePos[0] = Position(l);
        a.eyePos[1] = Position(r);
    }
    if (a.valid && a.upper[0] < v.count && a.upper[1] < v.count && a.hand[0] < v.count && a.hand[1] < v.count) {
        Mat4 sL{}, hL{}, sR{}, hR{};
        const bool read = CopyMem(&sL, &v.worlds[a.upper[0]], sizeof(Mat4)) && CopyMem(&hL, &v.worlds[a.hand[0]], sizeof(Mat4)) &&
                          CopyMem(&sR, &v.worlds[a.upper[1]], sizeof(Mat4)) && CopyMem(&hR, &v.worlds[a.hand[1]], sizeof(Mat4));
        const bool tpose = read && fp::LooksLikeTPose(Position(sL), Position(hL), Position(sR), Position(hR));
        a.tposeRun = tpose ? a.tposeRun + 1 : 0;
        a.normalRun = tpose ? 0 : a.normalRun + 1;
        if (!a.tposed && a.tposeRun >= 9) a.tposed = true;
        if (a.tposed && a.normalRun >= 27) a.tposed = false;
    }
}

// ---- EXP-036: render-only edits at Havok's skinning-matrix preparation ----
// LIVE (fpwatch 0.9.8): the only per-frame readers of Jodie's bone worlds that feed drawing are
// Havok's "TtPrepare Matrices" (FUN_140654920 on a Havok worker) and HAVOK_CLOTH_INSTANCE: her mesh
// is skinned on the CPU and streamed to vertex buffers. The prepare functions take a pointer to
// the source world array; for Jodie's array we pass an edited copy. The skeleton is never written.
using SkinIndexedFn = void (*)(float*, const Mat4*, const uint16_t*, const float*, uint64_t, float*);
using SkinContiguousFn = void (*)(float*, const Mat4*, const float*, uint64_t, float*);
SkinIndexedFn g_skinIndexedOriginal = nullptr;
SkinContiguousFn g_skinContiguousOriginal = nullptr;
// 16-byte aligned: the game reads these with MOVAPS (LIVE crash 0.12.1: misaligned copy ->
// #GP reported as an access violation at 0xFFFFFFFFFFFFFFFF in FUN_1409e6da0).
alignas(16) std::array<Mat4, detail::kMaxBones + 1> g_renderPose;
// The same pose without the head / neck collapse, for the cloth simulation (owner, 2026-10-08: a
// necklace spun in circles when its simulation was anchored to the collapsed neck bones).
alignas(16) std::array<Mat4, detail::kMaxBones + 1> g_physicsPose;
bool Aligned16(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 15) == 0; }
const Mat4* g_renderSource = nullptr;
uint64_t g_renderSerial = 0;
bool g_renderReady = false;

bool SourceArrays(void* skel, const Mat4** a, const Mat4** b, uint32_t* count) {
    __try {
        *count = *static_cast<uint32_t*>(skel);
        *a = *reinterpret_cast<Mat4**>(static_cast<char*>(skel) + 0x10);
        *b = *reinterpret_cast<Mat4**>(static_cast<char*>(skel) + 0x18);
        return *count && *count <= detail::kMaxBones;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Caller holds g_mutex. Returns the edited copy for Jodie's world array `src`, else src.
const Mat4* RenderSourceImpl(const Mat4* src);
const Mat4* RenderSource(const Mat4* src) {
    if (!g_framePlanValid || g_framePlan.skeleton != g_plan.skeleton || !g_plan.firstPerson) return RenderSourceImpl(src);
    // Draw from the plan of this animation frame; the live plan is put back afterwards.
    const Plan live = g_plan;
    const uint64_t liveSerial = g_planSerial;
    g_plan = g_framePlan;
    g_planSerial = g_framePlanSerial;
    const Mat4* out = RenderSourceImpl(src);
    g_plan = live;
    g_planSerial = liveSerial;
    return out;
}
const Mat4* RenderSourceImpl(const Mat4* src) {
    if (!src || !g_editsOn.load(std::memory_order_relaxed) || !g_plan.firstPerson || !g_plan.skeleton) return src;
    const Mat4 *a = nullptr, *b = nullptr;
    uint32_t count = 0;
    if (!SourceArrays(g_plan.skeleton, &a, &b, &count) || (src != a && src != b)) return src;
    auto& o = g_overlay;
    const size_t bytes = (size_t(count) + 1) * sizeof(Mat4);
    // Same source content and plan as the last edit: reuse (several meshes per frame).
    if (g_renderReady && g_renderSource == src && g_renderSerial == g_planSerial && o.count == count &&
        CopyMem(g_current.data(), src, bytes) && std::memcmp(g_current.data(), o.clean.data(), bytes) == 0)
        return g_renderPose.data();
    g_renderReady = false;
    if (!CopyMem(o.clean.data(), src, bytes)) return src;
    for (uint32_t i = 0; i <= count; ++i)
        if (!fp::Finite(Position(o.clean[i])) || !std::isfinite(o.clean[i].m[0])) {
            g_status.lastFailure = 6;
            return src;
        }
    o.skeleton = g_plan.skeleton;
    o.count = count;
    SkeletonView v{};
    if (g_rig.skeleton != g_plan.skeleton || g_rig.count != count) {
        if (!ReadSkeleton(g_plan.skeleton, &v)) return src;
        BuildRig(g_plan.skeleton, v, o.clean.data());
    }
    g_status.lastFailure = 0;
    if (!g_rig.valid || !ComputeEdits()) return src;
    // Cloth anchors (g_physicsPose): her hidden head / hair bones are moved 30 m down at full size, so a
    // cloth simulated from them (young Jodie's hair: owner 0.16.23 "the hair is still there"; EXP-034 had
    // it hidden with her head, and since the 2026-10-08 necklace fix cloth got the un-hidden head) is
    // simulated out of sight. Full-size frames keep the simulation stable (the necklace spun on the
    // shrunken ones); the two neck bones stay, so cloth hung from her neck is unaffected.
    const Rig& rig = g_rig;
    const uint32_t neck1 = rig.head < count ? rig.parents[rig.head] : kAbsent;
    const uint32_t neck0 = neck1 < count ? rig.parents[neck1] : kAbsent;
    for (uint32_t i = 0; i <= count; ++i) {
        g_renderPose[i] = i < count && o.changed[i] ? o.edited[i] : o.clean[i];
        g_physicsPose[i] = i < count ? g_work[i] : o.clean[i];
        if (i < count && rig.hideTree[i] && i != neck1 && i != neck0 && !rig.clothKeep[i])
            SetPosition(g_physicsPose[i], fp::Add(Position(g_physicsPose[i]), Vec3{0.f, -30.f, 0.f}));
    }
    g_renderSource = src;
    g_renderSerial = g_planSerial;
    g_renderReady = true;
    ++g_status.captureValid;
    if (!g_status.firstSuccess) {
        g_status.firstSuccess = true;
        Log("FPBODY LIVE first-person pose drawn through Havok skinning: bones=%u hidden=%u arms=%d turn=%d", count,
            g_rig.hidden, int(g_plan.armsActive), int(g_plan.turnBody));
    }
    return g_renderPose.data();
}

void HookSkinIndexed(float* xf, const Mat4* src, const uint16_t* idx, const float* invBind, uint64_t count, float* out) {
    if (!g_active.load(std::memory_order_acquire)) return g_skinIndexedOriginal(xf, src, idx, invBind, count, out);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* use = RenderSource(src);
    if (!Aligned16(use)) use = src;
    if (use != src) ++g_status.applyWrites;
    g_skinIndexedOriginal(xf, use, idx, invBind, count, out);
}
void HookSkinContiguous(float* xf, const Mat4* src, const float* invBind, uint64_t count, float* out) {
    if (!g_active.load(std::memory_order_acquire)) return g_skinContiguousOriginal(xf, src, invBind, count, out);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* use = RenderSource(src);
    if (!Aligned16(use)) use = src;
    if (use != src) ++g_status.applyWrites;
    g_skinContiguousOriginal(xf, use, invBind, count, out);
}

// ---- EXP-036: the actual render path. LIVE fpwatch on a face, spine and hand bone: besides the
// animation internals the only per-frame reader is FUN_1409e69c0, which builds a skinned element's
// 3x4 bone palette (+0x80) from its bone world array (+0x70) and inverse binds (+0x60); its
// sibling FUN_1409e6da0 computes the element's bounds from the same array. For Jodie's array the
// element is pointed at the edited copy for the duration of the call, then restored.
using ElementFn = void (*)(void*);
ElementFn g_paletteBuildOriginal = nullptr;
ElementFn g_paletteBoundsOriginal = nullptr;
bool SwapElementSource(void* elem, const Mat4* from, const Mat4* to) {
    __try {
        auto** field = reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x70);
        if (*field != from) return false;
        *field = to;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const Mat4* ElementSource(void* elem) {
    __try {
        return *reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x70);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Copied skeletons (clothing / sleeves driven by SKELETON_MAPPING): an element whose bone array is
// not Jodie's but whose bones sit exactly on her clean bones. Each such bone is drawn from the
// edited pose of the bone it copies (owner: "the wrist is not part of motion controls" while the
// hand bone was verifiably turned by the controller).
#pragma warning(push)
#pragma warning(disable : 4324)  // padded for the 16-byte aligned pose (intended)
struct Follower {
    const Mat4* src = nullptr;
    uint32_t count = 0;
    std::array<int16_t, 256> map{};
    uint32_t mapped = 0;
    uint64_t builtSerial = 0;
    bool logged = false;
    alignas(16) std::array<Mat4, 256> pose{};
    // Rigid attachment to one of Jodie's bones (owner, 2026-10-08: a held item and a red garment
    // stayed with the animated body): objects with their own skeleton parented to her hand / spine.
    struct Cand { int16_t bone = -1; Vec3 lo{}, hi{}, wlo{}, whi{}; };
    std::array<Cand, 8> cand{};
    uint32_t samples = 0;
    uint64_t lastSample = 0;
    int16_t attachBone = -1;
    uint16_t refBone = 0;           // the object's bone used for the test (nearest to her)
    uint64_t decidedSerial = 0;
    Vec3 attachLocal{};
    bool attachDecided = false;
};
#pragma warning(pop)
// More slots than skinned elements near her in a crowded scene (LIVE 0.14.2: 64 slots thrashed in
// the Embassy reception, every element re-created and re-checked each frame).
std::array<Follower, 256> g_followers;
std::atomic<uint32_t> g_characterLogs{0};
constexpr uint32_t kMaxRigidFollowerBones = 64;  // held items / props; characters have 256
static_assert(alignof(Follower) >= 16 && offsetof(Follower, pose) % 16 == 0, "MOVAPS alignment");
uint32_t ElementCount(void* elem) {
    __try {
        return *reinterpret_cast<uint32_t*>(static_cast<char*>(elem) + 0x78);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool SameBone(const Mat4& a, const Mat4& b) {
    for (int k : {12, 13, 14})
        if (std::fabs(a.m[k] - b.m[k]) > 0.003f) return false;
    for (int k : {0, 1, 2, 4, 5, 6})
        if (std::fabs(a.m[k] - b.m[k]) > 0.02f) return false;
    return true;
}
// Learn / apply a rigid attachment of an external skeleton (its first bone) to one of Jodie's bones.
// Attached = its offset in that bone's frame stays within 1 cm while the bone moves >= 5 cm in the
// world (furniture never qualifies: the bone moves, the furniture does not follow).
const Mat4* RigidFollower(Follower& f, const Mat4* src, uint32_t count) {
    const auto& o = g_overlay;
    // Not attached: re-check every ~3 s with fresh candidates (an item lying on a table is picked
    // up later; LIVE 0.13.1: the held 4-bone item was decided "not attached" for good).
    if (f.attachDecided && f.attachBone < 0) {
        if (g_renderSerial - f.decidedSerial < 270) return src;
        f.attachDecided = false;
        f.samples = 0;
        f.cand = {};
    }
    if (!f.samples) {  // the object's bone nearest to any of her bones is the reference
        float best = 1e9f;
        f.refBone = 0;
        for (uint32_t j = 0; j < count && j < 64; ++j) {
            const Vec3 pj = Position(g_current[j]);
            if (!fp::Finite(pj)) continue;
            for (uint32_t k = 0; k < o.count; k += 2) {
                const float d = fp::Len(fp::Sub(pj, Position(o.clean[k])));
                if (d < best) { best = d; f.refBone = uint16_t(j); }
            }
        }
    }
    const Vec3 p0 = Position(g_current[f.refBone]);
    if (!fp::Finite(p0)) return src;
    if (f.attachBone >= 0) {
        Mat4 inv;
        if (!detail::AffineInverse(o.clean[f.attachBone], inv)) return src;
        if (fp::Len(fp::Sub(pose::TransformPoint(p0, inv), f.attachLocal)) > 0.03f) {  // released
            const int16_t was = f.attachBone;
            f.attachBone = -1;
            f.attachDecided = false;
            f.samples = 0;
            f.cand = {};
            Log("FPBODY attached object released from bone %d (relearning)", int(was));
            return src;
        }
        const Mat4 delta = pose::Mul(inv, g_renderPose[f.attachBone]);
        for (uint32_t j = 0; j < count; ++j) f.pose[j] = pose::Mul(g_current[j], delta);
        ++g_status.propDraws;
        return f.pose.data();
    }
    if (f.lastSample == g_renderSerial) return src;  // one sample per animation frame
    f.lastSample = g_renderSerial;
    if (!f.samples) {
        // Candidates: her 8 bones nearest to the object's first bone, if it is near her at all.
        const Vec3 hips = Position(o.clean[0]);
        if (fp::Len(fp::Sub(p0, hips)) > 1.2f) {
            f.attachDecided = true;  // far from her: not attached (re-checked later)
            f.decidedSerial = g_renderSerial;
            return src;
        }
        float dist[8];
        for (auto& c : f.cand) c.bone = -1;
        for (int i = 0; i < 8; ++i) dist[i] = 1e9f;
        for (uint32_t k = 0; k < o.count; ++k) {
            const float d = fp::Len(fp::Sub(Position(o.clean[k]), p0));
            for (int i = 0; i < 8; ++i)
                if (d < dist[i]) {
                    for (int m = 7; m > i; --m) { dist[m] = dist[m - 1]; f.cand[m].bone = f.cand[m - 1].bone; }
                    dist[i] = d;
                    f.cand[i].bone = int16_t(k);
                    break;
                }
        }
    }
    for (auto& c : f.cand) {
        if (c.bone < 0) continue;
        Mat4 inv;
        if (!detail::AffineInverse(o.clean[c.bone], inv)) continue;
        const Vec3 local = pose::TransformPoint(p0, inv), world = Position(o.clean[c.bone]);
        if (!f.samples) { c.lo = c.hi = local; c.wlo = c.whi = world; }
        c.lo = {std::min(c.lo.x, local.x), std::min(c.lo.y, local.y), std::min(c.lo.z, local.z)};
        c.hi = {std::max(c.hi.x, local.x), std::max(c.hi.y, local.y), std::max(c.hi.z, local.z)};
        c.wlo = {std::min(c.wlo.x, world.x), std::min(c.wlo.y, world.y), std::min(c.wlo.z, world.z)};
        c.whi = {std::max(c.whi.x, world.x), std::max(c.whi.y, world.y), std::max(c.whi.z, world.z)};
    }
    ++f.samples;
    int best = -1;
    float bestSize = 0.015f;
    bool moved = false;
    const bool headOnly = count > kMaxRigidFollowerBones;
    for (int i = 0; i < 8; ++i) {
        const auto& c = f.cand[i];
        if (c.bone < 0) continue;
        if (headOnly && (uint32_t(c.bone) >= g_rig.count || !g_rig.hideTree[c.bone])) continue;
        const float travel = fp::Len(fp::Sub(c.whi, c.wlo)), size = fp::Len(fp::Sub(c.hi, c.lo));
        if (travel >= 0.05f) {
            moved = true;
            if (size < bestSize) { bestSize = size; best = i; }
        }
    }
    if (best >= 0 && f.samples >= 20) {
        const auto& c = f.cand[best];
        f.attachBone = c.bone;
        f.attachLocal = fp::Mul(fp::Add(c.lo, c.hi), 0.5f);
        f.attachDecided = true;
        Log("FPBODY object with its own skeleton (%u bones) is attached to Jodie's bone %d%s: drawn from her edited pose",
            count, int(c.bone), g_rig.hideTree[c.bone] ? " (head / hair: hidden with it)" : "");
    } else if (moved && best < 0 && f.samples >= 120) {
        f.attachDecided = true;  // her bones moved, it did not follow any: not attached (for now)
        f.decidedSerial = g_renderSerial;
    }
    return src;
}

const Mat4* FollowerSource(const Mat4* src, uint32_t count) {
    if (!g_renderReady || !g_renderSource || count == 0 || count > 256) return src;
    Follower* f = nullptr;
    for (auto& e : g_followers) if (e.src == src && e.count == count) f = &e;
    if (!f) {
        Follower* oldest = &g_followers[0];
        for (auto& e : g_followers) {
            if (!e.src) { f = &e; break; }
            if (e.builtSerial < oldest->builtSerial) oldest = &e;
        }
        if (!f) f = oldest;
        *f = Follower{};
        f->src = src;
        f->count = count;
    }
    // Not a bone copy of Jodie and not attached: skip cheaply until the next periodic re-check.
    if (f->builtSerial && f->mapped < 8 && f->attachDecided && f->attachBone < 0 && g_planSerial - f->builtSerial <= 45)
        return src;
    RenderSource(g_renderSource);  // her edited pose for this frame's clean pose
    const auto& o = g_overlay;
    if (!g_renderReady || !CopyMem(g_current.data(), src, count * sizeof(Mat4))) return src;
    // (Re)build the bone map every ~0.5 s of plan updates, or when a mapped bone stopped matching.
    bool valid = f->builtSerial && f->mapped;
    if (valid)
        for (uint32_t j = 0; j < count && valid; j += 7)
            if (f->map[j] >= 0 && !SameBone(g_current[j], o.clean[f->map[j]])) valid = false;
    if (!valid || g_planSerial - f->builtSerial > 45) {
        f->mapped = 0;
        for (uint32_t j = 0; j < count; ++j) {
            f->map[j] = -1;
            for (uint32_t k = 0; k < o.count; ++k)
                if (SameBone(g_current[j], o.clean[k])) { f->map[j] = int16_t(k); ++f->mapped; break; }
        }
        f->builtSerial = g_planSerial;
        if (f->mapped >= 8 && !f->logged) {
            f->logged = true;
            Log("FPBODY copied skeleton found: %u of %u bones follow Jodie's (drawn from the edited pose)", f->mapped, count);
        }
    }
    if (f->mapped >= 8) {
        for (uint32_t j = 0; j < count; ++j)
            f->pose[j] = f->map[j] >= 0 ? g_renderPose[f->map[j]] : g_current[j];
        ++g_status.attachedHidden;
        return f->pose.data();
    }
    // Large skeletons (> 64 bones) may only attach to her head / hair bones (RigidFollower): LIVE
    // 0.14.1 (Embassy reception) every 256-bone character "attached" to her fingers / arm helpers for
    // 2-14 s (handshakes) was redrawn from her pose and lost its shadows; but hair or a hat with its
    // own skeleton rides on her head and must collapse with it (owner, 0.15.6: adult Jodie's hair).
    return RigidFollower(*f, src, count);
}

// A skinned item whose model node hangs off one of her bones (LIVE 0.13.2: a 4-bone held item,
// node parent = her node, bone 223, a prop socket in her secondary group): its bones are drawn
// moved by (clean node world)^-1 x (node world from her edited bone).
alignas(16) std::array<Mat4, 256> g_itemPose;
bool ReadMatrix(const Mat4* m, Mat4* out) {
    __try {
        *out = *m;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const Mat4* ItemSource(void* elem, const Mat4* src) {
    uint32_t count = ElementCount(elem);
    const Mat4 *ja = nullptr, *jb = nullptr;
    uint32_t jcount = 0;
    int type = 0;
    const Mat4 *world = nullptr, *bones = nullptr;
    uint32_t n = 0;
    if (!src || count == 0 || count > 256 || !g_renderReady || !ReadElementHeader(elem, &type, &world, &bones, &n) || !world ||
        !SourceArrays(g_plan.skeleton, &ja, &jb, &jcount))
        return src;
    if (RenderSource(ja) == ja || !PropWorldFromEdited(world, jcount)) return src;
    Mat4 clean, inv;
    if (!ReadMatrix(world, &clean) || !detail::AffineInverse(clean, inv)) return src;
    const Mat4 delta = pose::Mul(inv, g_propWorld);
    if (!CopyMem(g_itemPose.data(), src, count * sizeof(Mat4))) return src;
    for (uint32_t j = 0; j < count; ++j) g_itemPose[j] = pose::Mul(g_itemPose[j], delta);
    ++g_status.propDraws;
    if (!g_loggedItem) {
        g_loggedItem = true;
        Log("FPBODY skinned item (%u bones) on her bone drawn from the edited pose", count);
    }
    return g_itemPose.data();
}

void CallWithEdited(ElementFn original, void* elem) {
    if (!elem || !g_active.load(std::memory_order_acquire)) return original(elem);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* src = ElementSource(elem);
    const Mat4* use = RenderSource(src);
    if (use == src && src) use = ItemSource(elem, src);
    if (use == src && src) use = FollowerSource(src, ElementCount(elem));
    if (use == src || !Aligned16(use) || !SwapElementSource(elem, src, use)) return original(elem);
    ++g_status.applyWrites;
    original(elem);
    SwapElementSource(elem, use, src);
}
void HookPaletteBuild(void* elem) { CallWithEdited(g_paletteBuildOriginal, elem); }

// Rigidly attached elements (types 0xe / 0x24, and the world refresh of type 3) read a single
// parent world matrix through elem+0x18. When that points at one of Jodie's bones (a cuff, watch
// or hand piece on her wrist), point it at the same bone of the edited copy for the call.
ElementFn g_rigidOriginal = nullptr, g_worldUpdateOriginal = nullptr, g_transform24Original = nullptr;
bool ReadElementParent(void* elem, const Mat4** out) {
    __try {
        *out = *reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteElementParent(void* elem, const Mat4* value) {
    __try {
        *reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x18) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Held items (owner: "the item is on my body, not in my hand"): a prop's scene node hangs off one
// of Jodie's bones (STATIC NODE::UpdateWorld: world = local(+0x40) x parent world; FUN_14026ac90:
// node+0x114 >= 0 -> bone index of the parent node +0x118, else the parent node's world +0x80).
// Its drawn pieces read the node world through elem+0x18; when the node chain ends on her bone,
// the world is recomputed from the edited bone for the draw call.
struct NodeLink { const char* node; Mat4 local; };
bool ReadNodeLink(const char* node, Mat4* local, int* bone, const char** parent) {
    __try {
        std::memcpy(local, node + 0x40, sizeof(Mat4));
        *bone = *reinterpret_cast<const int*>(node + 0x114);
        *parent = *reinterpret_cast<const char* const*>(node + 0x118);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// parent = the matrix pointer an element reads; returns true and fills g_propWorld when it is the
// world of a node chained to one of Jodie's bones.
bool PropWorldFromEdited(const Mat4* parent, uint32_t count) {
    if (!g_plan.node || !parent) return false;
    NodeLink chain[6];
    int n = 0;
    const char* node = reinterpret_cast<const char*>(parent) - 0x80;
    for (int depth = 0; depth < 6; ++depth) {
        Mat4 local;
        int bone = -1;
        const char* up = nullptr;
        if (!ReadNodeLink(node, &local, &bone, &up) || !up) return false;
        chain[n++] = {node, local};
        if (up == g_plan.node) {
            if (bone < 0 || uint32_t(bone) >= count) return false;
            g_socketBone[bone] = true;
            Mat4 world = g_renderPose[bone];
            for (int i = n - 1; i >= 0; --i) world = pose::Mul(chain[i].local, world);
            if (!fp::ValidMatrix(world)) return false;
            g_propWorld = world;
            return true;
        }
        if (bone >= 0) return false;  // attached to a bone of someone else
        node = up;
    }
    return false;
}

void CallWithEditedParent(ElementFn original, void* elem) {
    if (!elem || !g_active.load(std::memory_order_acquire)) return original(elem);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* parent = nullptr;
    const Mat4 *a = nullptr, *b = nullptr;
    uint32_t count = 0;
    if (!g_editsOn.load() || !g_plan.firstPerson || !g_plan.skeleton || !ReadElementParent(elem, &parent) || !parent ||
        !SourceArrays(g_plan.skeleton, &a, &b, &count))
        return original(elem);
    const Mat4* base = nullptr;
    for (const Mat4* arr : {a, b})
        if (arr && parent >= arr && parent < arr + count + 1 &&
            (reinterpret_cast<uintptr_t>(parent) - reinterpret_cast<uintptr_t>(arr)) % sizeof(Mat4) == 0)
            base = arr;
    if (!base) {
        // Not one of her bones: maybe a prop node chained to one.
        if (RenderSource(a) == a || !g_renderReady || !PropWorldFromEdited(parent, count)) return original(elem);
        if (!WriteElementParent(elem, &g_propWorld)) return original(elem);
        ++g_status.propDraws;
        original(elem);
        WriteElementParent(elem, parent);
        return;
    }
    const Mat4* edited = RenderSource(base);
    if (edited == base) return original(elem);
    const Mat4* use = edited + (parent - base);
    if (!Aligned16(use) || !WriteElementParent(elem, use)) return original(elem);
    ++g_status.attachedHidden;  // reused counter: rigid pieces drawn from the edited pose
    original(elem);
    WriteElementParent(elem, parent);
}
void HookRigid(void* elem) { CallWithEditedParent(g_rigidOriginal, elem); }

// Havok cloth reads its bone input through TRANSFORM_SETS::vfunc_01 (STATIC 0x1409ab4a0:
// returns count, writes the worlds pointer captured from *(skeleton+0x10) and the remap). For
// Jodie's arrays it gets the edited pose (owner: "the red clothing doesn't move").
using TransformSetsFn = uint32_t (*)(void*, const Mat4**, void**);
TransformSetsFn g_transformSetsOriginal = nullptr;
uint32_t HookTransformSets(void* self, const Mat4** worlds, void** remap) {
    const uint32_t n = g_transformSetsOriginal(self, worlds, remap);
    if (!worlds || !*worlds || !g_active.load(std::memory_order_acquire)) return n;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* use = RenderSource(*worlds);
    if (use != *worlds && Aligned16(use)) {
        *worlds = use;
        ++g_status.clothWrites;
    }
    return n;
}

// Cloth simulation (owner: "the dress is still bad"): HAVOK_CLOTH_INSTANCE::vfunc_13 (STATIC
// 0x140c52a60, LIVE fpwatch reader of her bones on the game thread) anchors / collides the cloth
// from the bone worlds at data+0x80 (data[0x10]). For her array it simulates against the edited pose
// for the call, so the dress follows the turned body and arms; the field is put back afterwards.
using ClothUpdateFn = void (*)(void*, float, uint64_t*);
ClothUpdateFn g_clothUpdateOriginal = nullptr;
bool SwapClothBones(uint64_t* data, uint64_t from, uint64_t to) {
    __try {
        if (data[0x10] != from) return false;
        data[0x10] = to;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t ClothBones(uint64_t* data) {
    __try {
        return data[0x10];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void HookClothUpdate(void* self, float dt, uint64_t* data) {
    if (!data || !g_active.load(std::memory_order_acquire)) return g_clothUpdateOriginal(self, dt, data);
    const Mat4 *src = nullptr, *use = nullptr;
    {   // not held across the game call: the cloth update may reach other hooked functions
        std::lock_guard<std::mutex> lock(g_mutex);
        src = reinterpret_cast<const Mat4*>(ClothBones(data));
        use = src ? RenderSource(src) : src;
        if (use != src) use = g_physicsPose.data();  // anchors: never the collapsed head / neck
        if (use == src || !Aligned16(use) ||
            !SwapClothBones(data, reinterpret_cast<uint64_t>(src), reinterpret_cast<uint64_t>(use)))
            use = nullptr;
        else
            ++g_status.clothWrites;
    }
    g_clothUpdateOriginal(self, dt, data);
    if (use) SwapClothBones(data, reinterpret_cast<uint64_t>(use), reinterpret_cast<uint64_t>(src));
}

// Static meshes (no per-frame update; the renderer reads *(elem+0x18) when drawing) attached to
// one of Jodie's bones through a node chain, e.g. a held item (owner, 2026-10-08: "the item is on
// my body"). FUN_1409dc450 runs for every element every frame: such an element is pointed at a
// mod-owned matrix holding the edited world (render structure only; the game's node is untouched)
// and put back as soon as first person is off or the chain no longer ends on her.
using FinalizeFn = void (*)(int*, float*, float*, uint8_t*);
FinalizeFn g_finalizeOriginal = nullptr;
struct Repoint {
    void* elem = nullptr;
    const Mat4* orig = nullptr;
    const uint8_t* orig28 = nullptr;  // +0x28 when it pointed into the parent world (types 8 / 40 ...)
    uint64_t lastSeen = 0;
    alignas(16) Mat4 world{};
};
// Some element types (LIVE 0.16.0: types 8 and 40 at her head — hair, earrings) are not transformed
// from +0x18 each frame; they draw from +0x28, which points into the same parent node world. When the
// finalizer redirects +0x18, +0x28 follows into the redirected matrix (and is put back with it).
bool ReadElement28(void* elem, const uint8_t** out) {
    __try {
        *out = *reinterpret_cast<const uint8_t**>(static_cast<char*>(elem) + 0x28);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteElement28(void* elem, const uint8_t* value) {
    __try {
        *reinterpret_cast<const uint8_t**>(static_cast<char*>(elem) + 0x28) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint32_t g_loggedTypes = 0;
std::array<Repoint, 256> g_repoints;
std::atomic<int> g_repointCount{0};
std::atomic<int> g_dumpElements{0};
uint64_t g_dumpSerial = 0;
bool ReadElementHeader(void* elem, int* type, const Mat4** world, const Mat4** bones, uint32_t* count) {
    __try {
        *type = *static_cast<int*>(elem);
        *world = *reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x18);
        *bones = *type == 3 ? *reinterpret_cast<const Mat4**>(static_cast<char*>(elem) + 0x70) : nullptr;
        *count = *type == 3 ? *reinterpret_cast<uint32_t*>(static_cast<char*>(elem) + 0x78) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadPosition(const Mat4* m, Vec3* out) {
    __try {
        *out = Position(*m);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Repoint* FindRepoint(void* elem) {
    for (auto& r : g_repoints) if (r.elem == elem) return &r;
    return nullptr;
}
void HookFinalize(int* elem, float* a2, float* a3, uint8_t* a4) {
    if (elem && !g_active.load(std::memory_order_acquire) && g_repointCount.load() > 0) {
        // First person off: hand repointed elements their own world back.
        std::lock_guard<std::mutex> lock(g_mutex);
        if (Repoint* r = FindRepoint(elem)) {
            int type = 0;
            const Mat4 *cur = nullptr, *bones = nullptr;
            uint32_t n = 0;
            if (ReadElementHeader(elem, &type, &cur, &bones, &n) && cur == &r->world) {
                WriteElementParent(elem, r->orig);
                if (r->orig28) WriteElement28(elem, r->orig28);
            }
            r->orig28 = nullptr;
            r->elem = nullptr;
            --g_repointCount;
        }
    } else if (elem && g_active.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        int type = 0;
        const Mat4 *cur = nullptr, *bones = nullptr;
        uint32_t boneCount = 0;
        const Mat4 *ja = nullptr, *jb = nullptr;
        uint32_t jcount = 0;
        if (ReadElementHeader(elem, &type, &cur, &bones, &boneCount) &&
            SourceArrays(g_plan.skeleton, &ja, &jb, &jcount)) {
            Repoint* r = FindRepoint(elem);
            const Mat4* orig = r && cur == &r->world ? r->orig : cur;
            const bool live = g_editsOn.load() && g_plan.firstPerson && RenderSource(ja) != ja && g_renderReady;
            int dumpBone = -2;
            if (type != 3 && orig && live && PropWorldFromEdited(orig, jcount)) {
                if (!r) {
                    Repoint* slot = nullptr;
                    for (auto& e : g_repoints)
                        if (!e.elem || g_renderSerial - e.lastSeen > 600) { slot = &e; break; }
                    r = slot;
                    if (r) {
                        if (!r->elem) ++g_repointCount;
                        r->elem = elem;
                    }
                }
                if (r) {
                    r->orig = orig;
                    r->world = g_propWorld;
                    r->lastSeen = g_renderSerial;
                    if (WriteElementParent(elem, &r->world)) ++g_status.propDraws;
                    const uint8_t* p28 = nullptr;
                    const auto* lo = reinterpret_cast<const uint8_t*>(orig);
                    if (type != 0xe && type != 0x24 && ReadElement28(elem, &p28) && p28 >= lo && p28 < lo + sizeof(Mat4)) {
                        r->orig28 = p28;
                        WriteElement28(elem, reinterpret_cast<const uint8_t*>(&r->world) + (p28 - lo));
                        if (type < 32 && !(g_loggedTypes & (1u << type))) {
                            g_loggedTypes |= 1u << type;
                            Log("FPBODY element type %d draws from its parent world (+0x28 = parent+0x%llx): redirected with it",
                                type, static_cast<unsigned long long>(p28 - lo));
                        }
                    }
                    dumpBone = 1;
                }
            } else if (r && cur == &r->world) {
                WriteElementParent(elem, r->orig);  // first person off / detached: give it back
                if (r->orig28) WriteElement28(elem, r->orig28);
                r->orig28 = nullptr;
                r->elem = nullptr;
                --g_repointCount;
            }
            if (const uint64_t at = g_autoDumpAt.load(std::memory_order_relaxed); at && GetTickCount64() >= at) {
                g_autoDumpAt = 0;
                g_dumpElements = 1;
                Log("FPELEMS automatic dump (elements near her head are hair / headwear candidates)");
            }
            if (g_dumpElements.load() > 0) {
                if (g_dumpSerial == 0) g_dumpSerial = g_renderSerial;
                if (g_renderSerial != g_dumpSerial) {
                    g_dumpElements = 0;
                    g_dumpSerial = 0;
                    Log("FPELEMS end");
                } else {
                    Vec3 p{}, hips = Position(g_overlay.clean[0]);
                    const Mat4* where = bones ? bones : orig;
                    if (where && ReadPosition(where, &p) && fp::Len(fp::Sub(p, hips)) < 2.f) {
                        const bool jodieBones = bones && (bones == ja || bones == jb);
                        const float toHead = g_rig.head < g_overlay.count
                                                 ? fp::Len(fp::Sub(p, Position(g_overlay.clean[g_rig.head])))
                                                 : -1.f;
                        Log("FPELEMS elem=%p type=%d world=%p bones=%p count=%u jodie=%d nodeChain=%s dist=%.2f head=%.2f", elem,
                            type, orig, bones, boneCount, int(jodieBones), dumpBone == 1 ? "her bone" : "no",
                            fp::Len(fp::Sub(p, hips)), toHead);
                    }
                }
            }
        }
    }
    g_finalizeOriginal(elem, a2, a3, a4);
}
void HookWorldUpdate(void* elem) { CallWithEditedParent(g_worldUpdateOriginal, elem); }
void HookTransform24(void* elem) { CallWithEditedParent(g_transform24Original, elem); }
void HookPaletteBounds(void* elem) { CallWithEdited(g_paletteBoundsOriginal, elem); }

void HookBuild(void* skeleton, const float* rootWorld) {
    void* const caller = _ReturnAddress();
    g_original(skeleton, rootWorld);
    if (!skeleton) return;
    void* const active = g_active.load(std::memory_order_acquire);
    void* const watched = g_watched.load(std::memory_order_acquire);
    if (skeleton != watched && skeleton != active) return;
    const bool final = caller == g_finalReturn;
    std::lock_guard<std::mutex> lock(g_mutex);
    // Clean head/root for the camera: always before any edit of this build.
    if (final && skeleton == watched) {
        ++g_status.selectedFinalCalls;
        CaptureAnchors(skeleton);
        g_framePlan = g_plan;
        g_framePlanSerial = g_planSerial;
        g_framePlanValid = true;
    }
    if (skeleton != active || !g_plan.firstPerson || skeleton != g_plan.skeleton || !g_inPlace.load()) return;
    SkeletonView v{};
    if (!ReadSkeleton(skeleton, &v)) return;
    if (final) {
        g_afterFinal = true;
        g_finalWorlds = v.worlds;
        g_overlay.ready = false;  // a fresh frame: the whole array is clean
    } else if (!g_afterFinal || v.worlds != g_finalWorlds) {
        // Intermediate builds (HumanIK solver inputs) stay clean; a build into the other world
        // array means the next frame's animation has started.
        if (v.worlds != g_finalWorlds) g_afterFinal = false;
        return;
    }
    ApplyInPlace(skeleton, !final);
}
}  // namespace

bool Install() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_original) return true;
    auto target = game::Addr(game::Anchor::SkeletonBuild);
    auto site = game::Addr(game::Anchor::FinalAnimationCallsite);
    auto palette = game::Addr(game::Anchor::MeshPaletteCopy);
    if (!target || !site || !palette || !bones::Install()) {
        Log("FPBODY guarded skeleton/final-callsite/palette unavailable; body path disabled");
        return false;
    }
    g_finalReturn = static_cast<char*>(site) + 5;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookBuild), reinterpret_cast<void**>(&g_original)) != MH_OK) {
        g_original = nullptr;
        Log("FPBODY build hook failed");
        return false;
    }
    if (MH_CreateHook(palette, reinterpret_cast<void*>(&HookPalette), reinterpret_cast<void**>(&g_paletteOriginal)) != MH_OK) {
        MH_RemoveHook(target);
        g_original = nullptr;
        g_paletteOriginal = nullptr;
        Log("FPBODY palette hook failed");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK || MH_EnableHook(palette) != MH_OK) {
        MH_RemoveHook(target);
        MH_RemoveHook(palette);
        g_original = nullptr;
        g_paletteOriginal = nullptr;
        Log("FPBODY EnableHook failed");
        return false;
    }
    if (void* publish = game::Addr(game::Anchor::SkeletonPublish)) {
        if (MH_CreateHook(publish, reinterpret_cast<void*>(&HookPublish), reinterpret_cast<void**>(&g_publishOriginal)) ==
                MH_OK &&
            MH_EnableHook(publish) == MH_OK)
            Log("FPBODY clean pose restored before each skeleton publish");
        else
            Log("FPBODY publish hook failed: edits are not restored (stateful systems see them)");
    }
    void* skinA = game::Addr(game::Anchor::HavokSkinIndexed);
    void* skinB = game::Addr(game::Anchor::HavokSkinContiguous);
    const bool skinOk = skinA && skinB &&
        MH_CreateHook(skinA, reinterpret_cast<void*>(&HookSkinIndexed), reinterpret_cast<void**>(&g_skinIndexedOriginal)) == MH_OK &&
        MH_CreateHook(skinB, reinterpret_cast<void*>(&HookSkinContiguous), reinterpret_cast<void**>(&g_skinContiguousOriginal)) == MH_OK &&
        MH_EnableHook(skinA) == MH_OK && MH_EnableHook(skinB) == MH_OK;
    Log("FPBODY render-only edits at Havok skinning: %s", skinOk ? "installed" : "FAILED");
    void* palA = game::Addr(game::Anchor::SkinPaletteBuild);
    void* palB = game::Addr(game::Anchor::SkinPaletteBounds);
    const bool palOk = palA && palB &&
        MH_CreateHook(palA, reinterpret_cast<void*>(&HookPaletteBuild), reinterpret_cast<void**>(&g_paletteBuildOriginal)) == MH_OK &&
        MH_CreateHook(palB, reinterpret_cast<void*>(&HookPaletteBounds), reinterpret_cast<void**>(&g_paletteBoundsOriginal)) == MH_OK &&
        MH_EnableHook(palA) == MH_OK && MH_EnableHook(palB) == MH_OK;
    Log("FPBODY render-only edits at the skin palette build: %s", palOk ? "installed" : "FAILED (body edits off)");
    void* rA = game::Addr(game::Anchor::ElementTransform);
    void* rB = game::Addr(game::Anchor::ElementWorldUpdate);
    void* rC = game::Addr(game::Anchor::ElementTransform24);
    const bool rigidOk = rA && rB && rC &&
        MH_CreateHook(rA, reinterpret_cast<void*>(&HookRigid), reinterpret_cast<void**>(&g_rigidOriginal)) == MH_OK &&
        MH_CreateHook(rB, reinterpret_cast<void*>(&HookWorldUpdate), reinterpret_cast<void**>(&g_worldUpdateOriginal)) == MH_OK &&
        MH_CreateHook(rC, reinterpret_cast<void*>(&HookTransform24), reinterpret_cast<void**>(&g_transform24Original)) == MH_OK &&
        MH_EnableHook(rA) == MH_OK && MH_EnableHook(rB) == MH_OK && MH_EnableHook(rC) == MH_OK;
    void* fin = game::Addr(game::Anchor::ElementFinalize);
    const bool finOk = fin &&
        MH_CreateHook(fin, reinterpret_cast<void*>(&HookFinalize), reinterpret_cast<void**>(&g_finalizeOriginal)) == MH_OK &&
        MH_EnableHook(fin) == MH_OK;
    Log("FPBODY static meshes on her bones (held items): %s", finOk ? "installed" : "FAILED");
    void* ts = game::Addr(game::Anchor::TransformSetsGet);
    const bool tsOk = ts &&
        MH_CreateHook(ts, reinterpret_cast<void*>(&HookTransformSets), reinterpret_cast<void**>(&g_transformSetsOriginal)) == MH_OK &&
        MH_EnableHook(ts) == MH_OK;
    Log("FPBODY cloth bone input (TRANSFORM_SETS): %s", tsOk ? "installed" : "FAILED");
    void* cu = game::Addr(game::Anchor::ClothInstanceUpdate);
    const bool cuOk = cu &&
        MH_CreateHook(cu, reinterpret_cast<void*>(&HookClothUpdate), reinterpret_cast<void**>(&g_clothUpdateOriginal)) == MH_OK &&
        MH_EnableHook(cu) == MH_OK;
    Log("FPBODY cloth simulation anchors (HAVOK_CLOTH_INSTANCE): %s", cuOk ? "installed" : "FAILED");
    Log("FPBODY render-only edits for rigidly attached pieces: %s (edited pose 16-byte aligned: %s)",
        rigidOk ? "installed" : "FAILED", Aligned16(g_renderPose.data()) ? "yes" : "NO");
    return true;
}

Status ReadStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Status out = g_status;
    out.handValid[0] = g_plan.handValid[0];
    out.handValid[1] = g_plan.handValid[1];
    out.wantedBodyYaw = g_plan.bodyYaw;
    out.node = reinterpret_cast<uintptr_t>(g_plan.node);
    return out;
}

void Watch(void* node, void* skeleton) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (skeleton != g_watched.load()) g_anchors.valid = false;
    g_watchedNode = node;
    g_watched.store(skeleton, std::memory_order_release);
}

void Publish(const Plan& plan) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (plan.node != g_plan.node || plan.skeleton != g_plan.skeleton) g_reportedIssues = 0;
    g_plan = plan;
    ++g_planSerial;
    g_active.store(plan.firstPerson ? plan.skeleton : nullptr, std::memory_order_release);
}

bool CleanHead(void* node, Mat4* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!out || !g_anchors.valid || node != g_watchedNode || g_anchors.skeleton != g_watched.load()) return false;
    *out = g_anchors.headWorld;
    return true;
}
bool CleanRoot(void* node, Mat4* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!out || !g_anchors.valid || node != g_watchedNode || g_anchors.skeleton != g_watched.load()) return false;
    *out = g_anchors.rootWorld;
    return true;
}

bool RenderedHead(void* node, Mat4* head, Vec3* eyeL, Vec3* eyeR) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto& o = g_overlay;
    if (!head || !eyeL || !eyeR || !g_renderReady || node != g_watchedNode || o.skeleton != g_watched.load() ||
        g_anchors.skeleton != o.skeleton || g_anchors.head >= o.count || g_anchors.eye[0] >= o.count ||
        g_anchors.eye[1] >= o.count)
        return false;
    *head = o.clean[g_anchors.head];
    *eyeL = Position(o.clean[g_anchors.eye[0]]);
    *eyeR = Position(o.clean[g_anchors.eye[1]]);
    return fp::ValidMatrix(*head) && fp::Finite(*eyeL) && fp::Finite(*eyeR);
}

bool HeadOffStanding(void* node, float* meters) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!meters || !g_anchors.valid || node != g_watchedNode || g_anchors.skeleton != g_watched.load() ||
        g_standingSkeleton != g_anchors.skeleton)
        return false;
    Mat4 rootInv;
    if (!detail::AffineInverse(g_anchors.rootWorld, rootInv)) return false;
    *meters = fp::Len(fp::Sub(pose::TransformPoint(Position(g_anchors.headWorld), rootInv), g_standingHead));
    return std::isfinite(*meters);
}

bool TPosed(void* node) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_anchors.valid && node == g_watchedNode && g_anchors.skeleton == g_watched.load() && g_anchors.tposed;
}

bool CleanEyes(void* node, Vec3* left, Vec3* right) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!left || !right || !g_anchors.valid || !g_anchors.eyesValid || node != g_watchedNode ||
        g_anchors.skeleton != g_watched.load())
        return false;
    *left = g_anchors.eyePos[0];
    *right = g_anchors.eyePos[1];
    return true;
}

bool PromptAnchors(Vec3* head, Vec3* leftHand) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!head || !leftHand || !g_plan.firstPerson || !g_plan.turnBody || !g_anchors.valid ||
        g_anchors.skeleton != g_plan.skeleton)
        return false;
    *head = Position(g_anchors.headWorld);
    if (g_plan.armsActive && g_plan.handValid[0] && fp::ValidMatrix(g_plan.handWorld[0])) {
        *leftHand = Position(g_plan.handWorld[0]);
    } else {
        const uint32_t h = g_rig.hand[0].hand;
        const auto& o = g_overlay;
        if (!g_rig.valid || !o.ready || o.skeleton != g_plan.skeleton || h >= o.count) return false;
        *leftHand = Position(o.changed[h] ? o.edited[h] : o.clean[h]);
    }
    return fp::Finite(*head) && fp::Finite(*leftHand);
}

bool PromptView(Vec3* head, Vec3* eye, Vec3* forward) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!head || !eye || !forward || !g_plan.firstPerson || !g_anchors.valid ||
        g_anchors.skeleton != g_plan.skeleton || !fp::Finite(g_plan.headPos) || fp::Len(g_plan.headFwd) < 0.5f)
        return false;
    *head = Position(g_anchors.headWorld);
    *eye = g_plan.headPos;
    *forward = g_plan.headFwd;
    return fp::Finite(*head);
}

uint32_t g_watchBone = kAbsent;  // fpwatchbone: bone to watch instead of M_Head
void SetWatchBone(uint32_t bone) { g_watchBone = bone; }
void SetRestoreAtPublish(bool on) { g_restoreAtPublish = on; }
void DumpElements() { g_dumpElements = 1; }
void SetEdits(bool on) { g_editsOn = on; }

namespace {
bool ReadHeadAddresses(void* skel, uint32_t head, void* out[2]) {
    __try {
        for (int i = 0; i < 2; ++i) {
            auto* arr = *reinterpret_cast<Mat4**>(static_cast<char*>(skel) + (i ? 0x18 : 0x10));
            out[i] = arr ? &arr[head].m[12] : nullptr;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

bool HeadWorldAddresses(void* out[2]) {
    void* skel = nullptr;
    uint32_t head = kAbsent;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_anchors.valid && g_anchors.head < g_anchors.count) {  // captured even with edits off
            skel = g_anchors.skeleton;
            head = g_watchBone < g_anchors.count ? g_watchBone : g_anchors.head;
        } else if (g_rig.valid && g_rig.head < g_rig.count) {
            skel = g_rig.skeleton;
            head = g_rig.head;
        } else {
            return false;
        }
    }
    return skel && ReadHeadAddresses(skel, head, out);
}

void Suspend() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_plan = Plan{};
    ++g_planSerial;
    g_active.store(nullptr, std::memory_order_release);
}
void Restore() {}
void BeginRender() {}
void EndRender() {}
}  // namespace b2s::fpbody
