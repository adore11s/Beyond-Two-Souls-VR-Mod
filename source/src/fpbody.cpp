#include "fpbody.h"
#include "bodykind.h"
#include "bones.h"
#include "fpmode.h"
#include "game.h"
#include "log.h"
#include "rigmap.h"
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

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
    // EXP-046: the model family (bodykind.h) decides it when known (young Jodie = JHO9 only); the arm length
    // only when it is not.
    bodykind::Family family = bodykind::Family::Unknown;
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
    std::array<int16_t, detail::kMaxBones> candOf{};        // -1, else the bone's index in cand
    // EXP-046 (Heavy Rain 0.11.0 hand stretch): skin helpers ON the wrist joint (within 1.5 cm of the hand bone,
    // scaled) that are not under the hand turn with it through the game's constraints. Until the learning has
    // decided them they are carried with the solved hand (adult rules only; young Jodie keeps her rules).
    std::array<bool, detail::kMaxBones> wristCarry[2]{};
    bool attachLearned = false;
    uint32_t hidden = 0, collapseAt = kAbsent;  // hidden bones shrink onto collapseAt (neck base)
    HandRig hand[2];
    // EXP-056: the game's own rig convention (rigmap.h). Every rig that is not on young-Jodie rules and has the named
    // copy groups uses it: copy bones follow the bone they copy, the head set is the named head groups, the forearm roll
    // helpers share the wrist twist, item sockets stay live in the hand. Young Jodie keeps her accepted rules.
    rigmap::Map map;
    bool nameRules = false;
    std::array<uint32_t, detail::kMaxBones> crc{};  // her bone name hashes (+0x270), for hierarchy-mapped objects
    std::array<bool, detail::kMaxBones> handSide[2]{};  // bones of each hand (hand subtree and its copy groups)
    bool valid = false;
} g_rig;
// EXP-056: the rigid transform the drawn body got this frame (turn about the eye pivot plus the head-follow shift):
// drawn = clean x g_bodyDelta for everything that hangs off her body node rather than a bone.
alignas(16) Mat4 g_bodyDelta = pose::Identity();
bool g_bodyDeltaValid = false;
// Last plan serial at which an item node was seen linked to each of her bones (her hands hold something).
std::array<uint64_t, detail::kMaxBones> g_socketSeen{};
float g_wristRoll[2]{};  // this frame's extra wrist twist about each forearm (EXP-056 roll helpers)

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
const Mat4* PhysicsPose();  // EXP-056: the one cloth pose (defined with g_physicsPose)
const Mat4* HeadClothCurrent();  // 0.26.0: the head cloth pose while a head cloth instance updates here, else null
bool ClothOnePose();
float RotationBetween(const Mat4& a, const Mat4& b);
Mat4 Rotation(const Mat4& m);
extern bool g_renderReady;
extern std::atomic<bool> g_editsOn;

alignas(16) Mat4 g_propWorld;
bool g_loggedItem = false;
// Bones that scene nodes hang off (prop sockets), seen by PropWorldFromEdited: candidates for the
// arm-helper learning even when they are not near an arm at rig build time.
std::array<bool, detail::kMaxBones> g_socketBone{};
bool ReadElementHeader(void* elem, int* type, const Mat4** world, const Mat4** bones, uint32_t* count);
bool PropWorldFromEdited(const Mat4* parent, uint32_t count);
void CheckHeldItemsDrawn();

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

uint32_t BoneCrc(void* skel, uint32_t bone) {
    __try {
        const uint8_t* table = skel ? *reinterpret_cast<uint8_t**>(static_cast<char*>(skel) + 0x270) : nullptr;
        return table && bone < detail::kMaxBones ? *reinterpret_cast<const uint32_t*>(table + size_t(bone) * 8) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// EXP-046: who the watched skeleton belongs to (fpcam, from player::Family or a possession), for BuildRig.
struct Identity {
    void* skeleton = nullptr;
    bodykind::Family family = bodykind::Family::Unknown;
    char why[256] = "";
} g_identity;

const char* FamilyTag(bodykind::Family f) {
    switch (f) {
        case bodykind::Family::Child: return "child";
        case bodykind::Family::Teen: return "teen";
        case bodykind::Family::Adult: return "adult";
        case bodykind::Family::Other: return "other";
        default: return "unknown";
    }
}

bool CloseAffine(const Mat4& a, const Mat4& b) {
    float d = 0.f;
    for (int k = 0; k < 16; ++k)
        if (k % 4 != 3) d = std::max(d, std::fabs(a.m[k] - b.m[k]));
    return d < 2e-3f;
}

// The game's composition (STATIC SKELETON::BuildWorld 0x14098d370: world[i] = local[i] (skeleton +0x08, stride
// 0x40) x world[parent[i]] (+0x30, u32; top bones use the root slot world[count]), FUN_1402ae170 per bone, in
// index order), checked on this rig's clean pose. Diagnostic only: bones that the game turns after the build
// (look-at, constraints, IK) do not match.
alignas(16) std::array<Mat4, detail::kMaxBones> g_locals;
bool CheckHierarchy(void* skel, uint32_t count, const uint32_t* parents, const Mat4* clean) {
    const Mat4* locals = nullptr;
    if (!CopyMem(&locals, static_cast<char*>(skel) + 0x08, sizeof(locals)) || !locals ||
        !CopyMem(g_locals.data(), locals, size_t(count) * sizeof(Mat4))) {
        Log("FPBODY hierarchy check: local matrices (skeleton +0x08) unreadable");
        return false;
    }
    int checked = 0, rowOk = 0, colOk = 0;
    const uint32_t step = std::max(1u, count / 24);
    for (uint32_t i = 0; i < count && checked < 24; i += step) {
        const uint32_t p = parents[i];
        if (p > count) continue;
        ++checked;
        rowOk += CloseAffine(pose::Mul(g_locals[i], clean[p]), clean[i]);
        colOk += CloseAffine(pose::Mul(clean[p], g_locals[i]), clean[i]);
    }
    Log("FPBODY hierarchy (parents +0x30, locals +0x08): local x parent world = world on %d of %d bones checked "
        "(parent x local: %d)%s", rowOk, checked, colOk, rowOk * 5 >= checked * 4 ? "" : " - LOW, see the dump");
    return true;
}

// One CSV per new rig in b2svr_logs (overwritten per bone count and family): the clean pose with the parent
// hierarchy and what the rig made of each bone, for offline checks (EXP-046).
void DumpSkeleton(const Rig& r, const Mat4* clean, float armLen, bool localsRead);

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
        // EXP-046: the model family decides; the 0.42 m arm test only when the family is unknown.
        r.family = g_identity.skeleton == skel ? g_identity.family : bodykind::Family::Unknown;
        r.adult = !bodykind::ChildRules(r.family, armLen);
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
        Log("FPBODY rig arms %.2f m, %s (%s): %s", armLen, bodykind::Name(r.family),
            g_identity.skeleton == skel ? g_identity.why : "no identity", r.adult ? "adult" : "child (unchanged rules)");
        if ((r.family == bodykind::Family::Child) != (armLen <= 0.42f) && r.family != bodykind::Family::Unknown &&
            r.family != bodykind::Family::Other)
            Log("FPBODY note: the model family and the arm length disagree (%.2f m); the family decides", armLen);
        if (r.adult)
            Log("FPBODY adult rig: arm scale %.2f; %u hidden bone(s) below her neck base stay in the cloth pose", r.armScale,
                kept);
    }
    // EXP-056: the rig as the game names it (copy groups, head groups, roll helpers, item sockets).
    rigmap::Build(r.map, v.count, r.parents.data(), [&](uint32_t hash) { return HashIndex(skel, v.count, hash); });
    r.nameRules = r.adult && r.map.named;
    for (uint32_t b = 0; b < v.count; ++b) r.crc[b] = BoneCrc(skel, b);
    r.handSide[0].fill(false);
    r.handSide[1].fill(false);
    if (r.nameRules) {
        uint32_t oldOnly = 0, logged = 0;
        char list[256] = "";
        for (uint32_t b = 0; b < v.count; ++b) {
            if (r.hideTree[b] && !r.map.hide[b]) {
                ++oldOnly;
                if (logged < 24) {
                    char one[12];
                    sprintf_s(one, "%s%u", logged ? " " : "", b);
                    strcat_s(list, one);
                    ++logged;
                }
            }
            r.hideTree[b] = r.map.hide[b];
        }
        r.hidden = r.map.hidden;
        if (r.map.neck < v.count) r.collapseAt = r.map.neck;
        r.clothKeep.fill(false);
        for (unsigned h = 0; h < 2; ++h) {
            const HandRig& hr = r.hand[h];
            if (hr.hand >= v.count) continue;
            for (uint32_t b = 0; b < v.count; ++b) {
                bool side = detail::Descendant(r.parents.data(), v.count, b, hr.hand);
                for (uint32_t c = 0; c < v.count && !side; ++c)
                    side = r.map.driver[c] == hr.hand && detail::Descendant(r.parents.data(), v.count, b, c);
                r.handSide[h][b] = side;
            }
        }
        uint32_t rolls[2]{};
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 3; ++k) rolls[s] += r.map.rolls[s][k] < v.count;
        Log("FPBODY rig map (game naming, EXP-056): %u copy bones follow the bone they copy; head = M_Neck + LC_M_Head + "
            "LC_EX_M_Head groups, %u bones (the old distance rule also hid %u: %s); forearm roll helpers L %u/3 R %u/3; "
            "item sockets %u; finger groups L %u R %u",
            r.map.drivers, r.map.hidden, oldOnly, oldOnly ? list : "none", rolls[0], rolls[1], r.map.sockets,
            r.map.lcHand[0], r.map.lcHand[1]);
    } else {
        Log("FPBODY rig map: %s (copy groups %s): earlier rules kept", r.adult ? "named groups missing" : "young-Jodie rules",
            r.map.named ? "named" : "not named");
    }
    g_autoDumpAt = GetTickCount64() + 4000;
    r.attachDriver.fill(-1);
    r.candOf.fill(-1);
    r.attachLearned = r.nameRules;  // EXP-056: nothing is learned from motion on a named rig
    r.candCount = 0;
    for (uint32_t b = 0; b < v.count && r.candCount < r.cand.size() && !r.nameRules; ++b) {
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
                r.candOf[b] = int16_t(r.candCount);
                auto& c = r.cand[r.candCount++];
                c = {};
                c.bone = b;
                c.hand = uint8_t(h);
                break;
            }
        }
    }
    r.wristCarry[0].fill(false);
    r.wristCarry[1].fill(false);
    uint32_t wristCarried[2]{};
    if (r.adult && !r.nameRules)
        for (uint32_t b = 0; b < v.count; ++b) {
            if (r.candOf[b] < 0) continue;
            for (unsigned h = 0; h < 2; ++h) {
                const HandRig& hr = r.hand[h];
                if (hr.hand >= v.count || fp::Len(fp::Sub(Position(clean[b]), Position(clean[hr.hand]))) >= 0.015f * r.armScale)
                    continue;
                r.wristCarry[h][b] = true;
                r.cand[r.candOf[b]].hand = uint8_t(h);  // learned against the arm it sits on
                ++wristCarried[h];
                break;
            }
        }
    Log("FPBODY wrist-joint helpers (within %.1f cm of the hand bone, not under it) ride on the hands until learned: "
        "left %u, right %u%s", 1.5f * r.armScale, wristCarried[0], wristCarried[1], r.adult ? "" : " (young-Jodie rules: none)");
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
    {
        float armLen = 0.f;
        for (unsigned h = 0; h < 2; ++h) {
            const HandRig& hr = r.hand[h];
            if (hr.upper >= v.count || hr.fore >= v.count || hr.hand >= v.count) continue;
            armLen = std::max(armLen, fp::Len(fp::Sub(Position(clean[hr.fore]), Position(clean[hr.upper]))) +
                                          fp::Len(fp::Sub(Position(clean[hr.hand]), Position(clean[hr.fore]))));
        }
        const bool locals = CheckHierarchy(skel, v.count, r.parents.data(), clean);
        DumpSkeleton(r, clean, armLen, locals);
    }
    return true;
}

void DumpSkeleton(const Rig& r, const Mat4* clean, float armLen, bool localsRead) {
    char name[96];
    sprintf_s(name, "\\skeleton_%u_%s.csv", r.count, FamilyTag(r.family));
    const std::wstring path = std::wstring(LogDir()) + std::wstring(name, name + std::strlen(name));
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    const uint8_t* table = nullptr;
    CopyMem(&table, static_cast<char*>(r.skeleton) + 0x270, sizeof(table));
    fprintf(f, "# B2S first-person rig: %u bones, %s (%s), arms %.3f m, %s rules\n", r.count, bodykind::Name(r.family),
            g_identity.skeleton == r.skeleton ? g_identity.why : "no identity", armLen, r.adult ? "adult" : "young-Jodie");
    fprintf(f, "# world matrices row-major, metres, Y up, row vectors; l0..l15 = local (skeleton +0x08)%s\n",
            localsRead ? "" : " unreadable");
    fprintf(f, "bone,crc,parent,role,driver,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,m12,m13,m14,m15");
    if (localsRead)
        for (int k = 0; k < 16; ++k) fprintf(f, ",l%d", k);
    fprintf(f, "\n");
    for (uint32_t i = 0; i < r.count; ++i) {
        uint32_t crc = 0;
        if (table) CopyMem(&crc, table + size_t(i) * 8, sizeof(crc));
        const char* role = i == r.head                                ? "head"
                           : r.hideTree[i]                             ? "hidden"
                           : r.nameRules && r.map.socket[i]            ? "socket"
                           : r.nameRules && r.map.roll[i]              ? "roll"
                           : r.nameRules && r.map.driver[i] < r.count  ? "copy"
                           : r.carried[0][i] || r.carried[1][i]        ? "lc_finger"
                           : r.wristCarry[0][i] || r.wristCarry[1][i]  ? "wrist_helper"
                           : r.candOf[i] >= 0                          ? "candidate"
                                                                       : "";
        for (unsigned h = 0; h < 2; ++h) {
            const HandRig& hr = r.hand[h];
            if (i == hr.upper) role = h ? "R_upper" : "L_upper";
            if (i == hr.fore) role = h ? "R_fore" : "L_fore";
            if (i == hr.hand) role = h ? "R_hand" : "L_hand";
        }
        fprintf(f, "%u,%08x,%d,%s,%d", i, crc, r.parents[i] >= r.count ? -1 : int(r.parents[i]), role,
                r.nameRules ? (r.map.driver[i] < r.count ? int(r.map.driver[i]) : -1) : int(r.attachDriver[i]));
        for (int k = 0; k < 16; ++k) fprintf(f, ",%.5f", clean[i].m[k]);
        if (localsRead)
            for (int k = 0; k < 16; ++k) fprintf(f, ",%.5f", g_locals[i].m[k]);
        fprintf(f, "\n");
    }
    fclose(f);
    Log("FPBODY skeleton dump written: b2svr_logs%s (bone, crc, parent, role, world, local)", name);
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
extern uint64_t g_renderSerial;
// EXP-056: after the arm chain is solved, every copy bone of this arm (LC_EX_<S>_Arm / _ForeArm / _Hand, LC_<S>_Hand)
// is put back on the bone it copies the way the game composes it (its offset from that bone in this frame's pose),
// and its children follow with their authored locals: skin helpers, roll bones, fingers, item sockets. Then the three
// forearm roll helpers take 1/3, 2/3 and all of the extra wrist twist about the forearm (the game's roll expressions
// share a joint twist out like this, e.g. `EX_R_Arm_Roll2.rotateX = 0.6667 * BB_R_Arm_TwistX.twist`). While the hand
// holds nothing, the fingers keep the pose captured once (owner: no animated fingers on the controller); while an item
// hangs off one of its bones, the fingers keep the animation's grip so the item sits in them as in third person.
bool CarryNamedCopies(unsigned h, Vec3 foreAxis) {
    Rig& r = g_rig;
    const HandRig& hr = r.hand[h];
    const uint32_t chain[3] = {hr.upper, hr.fore, hr.hand};
    for (uint32_t c = 0; c < r.count; ++c) {
        const uint32_t d = r.map.driver[c];
        if (d >= r.count || (d != chain[0] && d != chain[1] && d != chain[2])) continue;
        Mat4 inv;
        if (!detail::AffineInverse(g_body[d], inv)) continue;
        const Mat4 replacement = pose::Mul(pose::Mul(g_body[c], inv), g_arm[d]);
        if (!detail::RebuildDescendants(g_body.data(), r.parents.data(), r.count, c, replacement, g_arm.data())) return false;
    }
    if (fp::Finite(foreAxis) && fp::Len(foreAxis) > 0.5f && std::isfinite(g_wristRoll[h]))
        for (int k = 0; k < 3; ++k) {
            const uint32_t b = r.map.rolls[h][k];
            if (b >= r.count) continue;
            const float angle = g_wristRoll[h] * float(k + 1) / 3.f, half = angle * 0.5f;
            const pose::Quat q{foreAxis.x * std::sin(half), foreAxis.y * std::sin(half), foreAxis.z * std::sin(half),
                               std::cos(half)};
            Mat4 turned = pose::Mul(g_arm[b], pose::FromPose(q, {0, 0, 0}));
            SetPosition(turned, Position(g_arm[b]));
            if (!detail::RebuildDescendants(g_body.data(), r.parents.data(), r.count, b, turned, g_arm.data())) return false;
        }
    bool holding = false;
    for (uint32_t i = 0; i < r.count && !holding; ++i)
        holding = r.handSide[h][i] && g_socketSeen[i] && g_renderSerial - g_socketSeen[i] < 90;
    if (!holding && hr.lc < r.count) {
        Mat4 inverse;
        if (!detail::AffineInverse(g_body[hr.hand], inverse)) return true;
        if (!r.fingerRelValid[h]) {
            for (uint32_t i = 0; i < r.count; ++i)
                if (r.carried[h][i]) r.fingerRel[h][i] = pose::Mul(g_body[i], inverse);
            r.fingerRelValid[h] = true;
        }
        for (uint32_t i = 0; i < r.count; ++i) {
            if (!r.carried[h][i] || i == hr.lc) continue;
            bool socket = false;  // sockets and what hangs below them stay live
            for (uint32_t s = i, steps = 0; s < r.count && steps <= r.count && !socket; s = r.parents[s], ++steps)
                socket = r.map.socket[s];
            if (!socket) g_arm[i] = pose::Mul(r.fingerRel[h][i], g_arm[hr.hand]);
        }
    }
    return true;
}
bool SolveArmImpl(unsigned h) {
    const Rig& r = g_rig;
    const HandRig& hr = r.hand[h];
    const Mat4& grip = g_plan.handWorld[h];
    g_wristRoll[h] = 0.f;
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
            g_wristRoll[h] = fp::Finite(a) && fp::Finite(b) && std::isfinite(roll) ? roll : 0.f;
            // EXP-056: a named rig with its three forearm roll helpers shares the twist out along them (the game's
            // rig: the forearm bone itself does not twist), so the forearm keeps its solved basis.
            const bool rollHelpers = r.nameRules && r.map.rolls[h][0] < r.count && r.map.rolls[h][1] < r.count &&
                                     r.map.rolls[h][2] < r.count;
            if (!rollHelpers && fp::Finite(a) && fp::Finite(b) && std::isfinite(roll)) {
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
    if (r.nameRules) {
        if (!CarryNamedCopies(h, fp::Norm(fp::Sub(solve.wrist, solve.elbow)))) return false;
    } else if (hr.lc < r.count) {
    // The finger group (LC_*_Hand) follows the hand rigidly, in a pose captured once (not the
    // animation's current finger motion).
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
    // EXP-046: wrist-joint helpers not decided yet ride on the solved hand (adult rules only).
    if (r.adult) {
        Mat4 handInv;
        if (detail::AffineInverse(g_body[hr.hand], handInv))
            for (uint32_t i = 0; i < r.count; ++i) {
                if (!r.wristCarry[h][i] || r.attachDriver[i] >= 0) continue;
                const int16_t ci = r.candOf[i];
                if (ci >= 0 && r.cand[ci].decided) continue;  // decided: carried above, or it stays with the body
                g_arm[i] = pose::Mul(pose::Mul(g_body[i], handInv), g_arm[hr.hand]);
            }
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
std::atomic<int> g_headMode{2};  // b2svr.ini fp_head=show|hide|cutscene (0.24.0 default: hidden in F3 cutscenes only)
std::atomic<bool> g_headCloth{true};  // 0.26.0 dev A/B `fpheadcloth on|off`: her hair cloth hidden with her head
// The head set is collapsed now: always (hide), or while first person is a cutscene (plan.turnBody is F1 gameplay).
bool HideHeadNow() {
    const int m = g_headMode.load(std::memory_order_relaxed);
    return m == 1 || (m == 2 && !g_plan.turnBody);
}

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
    g_bodyDelta = delta;
    g_bodyDeltaValid = g_plan.turnBody;
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
        if (fp::Finite(shift) && fp::Len(shift) < 1.5f) {
            for (uint32_t i = 0; i < count; ++i) SetPosition(g_body[i], fp::Add(Position(g_body[i]), shift));
            SetPosition(g_bodyDelta, fp::Add(Position(g_bodyDelta), shift));
        }
    }
    g_work = g_body;
    // Helpers are learned in any first person, F3 cutscenes included, so they are known before the
    // arms first follow the controllers (owner 0.16.20: "when I first got into F1 the wrists broke for
    // a second, then they were good" = the learning time). 0.16.14 learned in F1 only because in an F3
    // cutscene two foot bones were taken for hand helpers by position alone (9.3 cm away); the 0.16.15+
    // rules (exact rotation / position within 0.6 cm, never beyond 8 cm) refuse those.
    // EXP-056: a named rig learns nothing from motion; its copy bones follow their bones by the game's naming.
    if (g_plan.firstPerson && !r.nameRules) LearnAttachments(g_body.data());
    if (g_plan.armsActive) {
        SolveArm(0);
        SolveArm(1);
    } else {
        g_status.armState[0] = g_status.armState[1] = 0;
    }
    // (0.16.18-0.16.20 re-checked the hide set over time and around her eyes. LIVE 0.16.20: it only ever
    // added arm / shoulder helpers that came near her face in cutscene poses (175, 181, 188, 240, 244),
    // never hair: young Jodie's hair is not her skeleton's bones. Removed; the set is the build-time one.)
    if (!r.nameRules) LearnHeadFollowers(o.clean.data(), count);  // EXP-056: the head set is named on these rigs
    // Collapse the head, face and hair bones (detail::HeadHideSet). Keeping the translation leaves
    // a valid affine matrix for the skinning code; the tiny basis shrinks everything bound to them.
    // EXP-036: the edited pose only feeds Havok's skinning matrices (render), never the skeleton,
    // so no game system reads these bones and a near-zero basis is safe again.
    // 0.23.0 (owner 2026-11-04: "dont hide head in first person, i want it to appear ... cuz i want to see mirror
    // reflections"): b2svr.ini fp_head=show (default) draws the head set as posed; fp_head=hide collapses it as before.
    const bool hideHead = HideHeadNow();
    Mat4 hidden = g_work[r.collapseAt < count ? r.collapseAt : r.head];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) hidden.m[row * 4 + col] *= 0.001f;
    for (uint32_t i = 0; i < count; ++i) {
        o.edited[i] = hideHead && r.hideTree[i] ? hidden : g_work[i];
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
    if (g_rig.nameRules && ClothOnePose()) {  // EXP-056: the one cloth pose
        static const std::array<bool, detail::kMaxBones> all = [] { std::array<bool, detail::kMaxBones> a{}; a.fill(true); return a; }();
        const Mat4* head = HeadClothCurrent();  // 0.26.0: inside a head cloth instance's update
        if (WriteClothPalette(palette, count, head ? head : PhysicsPose(), all.data())) ++g_status.clothWrites;
        return;
    }
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
// 0.26.0 (owner 2026-11-06: "the head is hidden but not the hair" in first-person cutscenes): her hair is Havok cloth
// (SOURCE, the scene scripts: Jodie:SetClothState("HAIR01".."HAIR05", "dynamic"), SimulateCloth("HAIREXT01/02")),
// and on a named rig every cloth input gets the one physics pose with her head NOT collapsed (EXP-056, so dresses and
// necklaces are not stretched). LIVE (owner's 0.25 run, every adult-Jodie scene 2101-2404): 1-3 of her cloth
// instances have their engine reference bone (HAVOK_CLOTH_INSTANCE: *(*(this+0xf8)+8), STATIC vfunc_13) on M_Head,
// M_Neck1 or M_Neck - bones of the hidden head set - so the hair was simulated and drawn at the head the render had
// collapsed. While the head is hidden, a cloth instance whose reference bone is in the head set gets this pose: the
// physics pose with every head-set bone 30 m down at full size (the rule young Jodie's hair has had since 0.16.24),
// and the engine's cloth teleport moves its particles there and back with the jump. Other cloth (dress, coat, bag:
// reference bones M_Hips / M_Spine / M_Spine2) keeps the one physics pose.
alignas(16) std::array<Mat4, detail::kMaxBones + 1> g_headClothPose;
bool g_headClothReady = false;           // g_headClothPose matches this frame's physics pose with the head hidden
thread_local const Mat4* t_clothPose = nullptr;  // set while a head cloth instance updates on this thread
struct HeadClothStats {
    uint64_t since = 0, updates = 0, inside = 0, outside = 0;  // skin / bone-input calls inside its update, else
    std::array<void*, 16> seen{};
    uint32_t seenCount = 0;
} g_headClothStats;
const Mat4* HeadClothCurrent() {
    if (t_clothPose) ++g_headClothStats.inside;
    return t_clothPose;
}
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
    const bool hideHead = HideHeadNow();
    const uint32_t neck1 = rig.head < count ? rig.parents[rig.head] : kAbsent;
    const uint32_t neck0 = neck1 < count ? rig.parents[neck1] : kAbsent;
    // EXP-056 (owner: clothing "stretched to oblivion", a necklace upside down): on a named rig nothing is dropped; the
    // cloth simulation gets her drawn pose without the head collapse, the same pose for every cloth input.
    for (uint32_t i = 0; i <= count; ++i) {
        g_renderPose[i] = i < count && o.changed[i] ? o.edited[i] : o.clean[i];
        g_physicsPose[i] = i < count ? g_work[i] : o.clean[i];
        // (Only while the head is hidden: a shown head keeps young Jodie's simulated hair on it, fp_head=show.)
        if (hideHead && !rig.nameRules && i < count && rig.hideTree[i] && i != neck1 && i != neck0 && !rig.clothKeep[i])
            SetPosition(g_physicsPose[i], fp::Add(Position(g_physicsPose[i]), Vec3{0.f, -30.f, 0.f}));
        // 0.26.0: her head cloth (hair) on a named rig, see g_headClothPose. Young Jodie keeps her rules above.
        g_headClothPose[i] = g_physicsPose[i];
        if (hideHead && rig.nameRules && i < count && rig.hideTree[i])
            SetPosition(g_headClothPose[i], fp::Add(Position(g_headClothPose[i]), Vec3{0.f, -30.f, 0.f}));
    }
    g_headClothReady = hideHead && rig.nameRules && g_headCloth.load(std::memory_order_relaxed);
    g_renderSource = src;
    g_renderSerial = g_planSerial;
    g_renderReady = true;
    CheckHeldItemsDrawn();
    ++g_status.captureValid;
    if (!g_status.firstSuccess) {
        g_status.firstSuccess = true;
        Log("FPBODY LIVE first-person pose drawn through Havok skinning: bones=%u hidden=%u arms=%d turn=%d", count,
            g_rig.hidden, int(g_plan.armsActive), int(g_plan.turnBody));
    }
    return g_renderPose.data();
}

// EXP-056: the pose for Havok cloth (its skinning operators, transform sets and the cloth matrix map). EXP-035/036 gave
// those the render pose (head collapsed to a point at her neck base) while the cloth update got the physics pose (head
// and hair moved 30 m down): three different poses for one simulation, so cloth skinned to a hidden bone was pulled to
// her neck base or 30 m away. A named rig gives every cloth input the one physics pose; young Jodie keeps hers.
std::atomic<bool> g_clothOnePose{true};  // dev: fpclothpose old|one
const Mat4* PhysicsPose() { return g_physicsPose.data(); }
bool ClothOnePose() { return g_clothOnePose.load(std::memory_order_relaxed); }
const Mat4* ClothSource(const Mat4* src) {
    const Mat4* use = RenderSource(src);
    if (use == src || !g_rig.nameRules || !g_clothOnePose.load(std::memory_order_relaxed)) return use;
    // 0.26.0: a head cloth instance updating on this thread (its skinning / bone input inside its own update) gets
    // the same head-dropped pose as its simulation anchors.
    if (const Mat4* head = HeadClothCurrent()) return head;
    if (g_headClothReady) ++g_headClothStats.outside;
    return g_physicsPose.data();
}
void HookSkinIndexed(float* xf, const Mat4* src, const uint16_t* idx, const float* invBind, uint64_t count, float* out) {
    if (!g_active.load(std::memory_order_acquire)) return g_skinIndexedOriginal(xf, src, idx, invBind, count, out);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* use = ClothSource(src);
    if (!Aligned16(use)) use = src;
    if (use != src) ++g_status.applyWrites;
    g_skinIndexedOriginal(xf, use, idx, invBind, count, out);
}
void HookSkinContiguous(float* xf, const Mat4* src, const float* invBind, uint64_t count, float* out) {
    if (!g_active.load(std::memory_order_acquire)) return g_skinContiguousOriginal(xf, src, invBind, count, out);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Mat4* use = ClothSource(src);
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
    // EXP-056 hierarchy mapping (MapHierarchy): bones paired with hers by name, verified on her bones.
    uint8_t nameState = 0;          // 0 not checked, 1 mapped onto her, 2 not
    uint64_t nameBuilt = 0;
    uint32_t nameMapped = 0;
    std::array<int16_t, 256> nameMap{};
    std::array<uint32_t, 256> parents{};
    bool nameLogged = false;
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

// ---- EXP-056: hierarchy-mapped objects (the Navajo backpack) ----
// SOURCE (decoded scene script): `_2001_NAVAJO_ARRIVAL_BackPack:MapHierarchy(_2001_NAVAJO_ARRIVAL_Jodie)` puts the
// backpack on her; nothing attaches it. STATIC: MapHierarchy (script method 0x9e, FUN_140dd4060 -> FUN_140dd3930)
// creates a QDT::SINT::HIERARCHY_MAPPING whose bone table (FUN_1411de020) pairs each bone of the object's skeleton with
// her bone of the same name hash (+0x270 tables); every frame FUN_1411dd920 writes her locals (composed over skipped
// bones) into the paired bones' locals and puts the object's root node on her node. So after its build the paired bones
// lie exactly on hers and its own bones (bag body, straps) hang off them with their authored locals. The mod's copy
// test needed 8 bones on hers; the backpack has fewer, so it fell to the motion-learned attachment (LIVE 0.21.6: glued
// to a finger bone, then released). Here: the same pairing by name, kept only when every name-paired bone lies on her
// bone (a mapping onto her, not another character with the same bone names), drawn from her drawn bones, and every
// other bone rebuilt from its parent with its clean local.
// Every skeleton's world arrays -> its skeleton (registered at SKELETON::BuildWorld while first person is on).
struct SkelReg {
    std::atomic<uintptr_t> worlds{0}, skel{0};
};
std::array<SkelReg, 4096> g_skelReg;
size_t SkelSlot(uintptr_t key) { return size_t(((key >> 4) * 0x9E3779B97F4A7C15ull) >> 52); }
void RegisterSkeleton(void* skel) {
    const Mat4 *a = nullptr, *b = nullptr;
    uint32_t n = 0;
    if (!SourceArrays(skel, &a, &b, &n)) return;
    for (const Mat4* w : {a, b}) {
        if (!w) continue;
        const uintptr_t key = reinterpret_cast<uintptr_t>(w);
        size_t i = SkelSlot(key);
        for (int probe = 0; probe < 8; ++probe, i = (i + 1) & 4095) {
            uintptr_t cur = g_skelReg[i].worlds.load(std::memory_order_relaxed);
            if (cur == key || (cur == 0 && g_skelReg[i].worlds.compare_exchange_strong(cur, key)) || probe == 7) {
                if (probe == 7) g_skelReg[i].worlds.store(key, std::memory_order_relaxed);
                g_skelReg[i].skel.store(reinterpret_cast<uintptr_t>(skel), std::memory_order_relaxed);
                break;
            }
        }
    }
}
void* SkeletonOf(const Mat4* worlds) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(worlds);
    size_t i = SkelSlot(key);
    for (int probe = 0; probe < 8; ++probe, i = (i + 1) & 4095) {
        if (g_skelReg[i].worlds.load(std::memory_order_relaxed) != key) continue;
        void* skel = reinterpret_cast<void*>(g_skelReg[i].skel.load(std::memory_order_relaxed));
        const Mat4 *a = nullptr, *b = nullptr;
        uint32_t n = 0;
        if (skel && SourceArrays(skel, &a, &b, &n) && (a == worlds || b == worlds)) return skel;  // still that skeleton
        return nullptr;
    }
    return nullptr;
}
bool ReadSkeletonNames(void* skel, uint32_t count, uint32_t* parents, uint32_t* crcs) {
    __try {
        const uint32_t* p = *reinterpret_cast<uint32_t**>(static_cast<char*>(skel) + 0x30);
        const uint8_t* table = *reinterpret_cast<uint8_t**>(static_cast<char*>(skel) + 0x270);
        if (!p || !table) return false;
        for (uint32_t i = 0; i < count; ++i) {
            parents[i] = p[i];
            crcs[i] = *reinterpret_cast<const uint32_t*>(table + size_t(i) * 8);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// 0.24.0 (owner 2026-11-05: "the bag on my back doesn't turn when I walk backwards, read how that works"). STATIC: the
// engine moves a hierarchy-mapped object (BackPack:MapHierarchy(Jodie)) in HIERARCHY_MAPPING's update FUN_1411dd920
// (a vfunc, no direct callers): source = *(*(mapping+0x70)+0x30)+0x170 (her skeleton), target =
// *(*(mapping+0x78)+0x30)+0x170 (the bag's); the target's node local is set to her skeleton node's world
// (FUN_14026a5f0(target+0x250 node, FUN_140268500(source) = source node +0x80)), then her locals (+0x08) are copied into
// the target bones paired by name (pair table mapping+0x80, count +0x88). 0.22.0 took a skeleton as mapped onto her only
// while every name-paired bone sat within 3 mm of hers, re-checked every ~45 plan frames; a failed check (a bone a frame
// behind, or changed after the copy) dropped it to the position copy, which draws the bag's own bones where the
// animation has them - unturned, which shows when the drawn body is turned far from the animated one (walking backwards
// it is ~180 deg). Now the engine's own relation decides: the update is hooked and a skeleton the engine maps from hers
// is taken as mapped, paired by name like the engine, with no position test.
struct EngineMapping {
    std::atomic<const void*> target{nullptr};
    std::atomic<const void*> source{nullptr};
    std::atomic<uint64_t> seen{0};
};
std::array<EngineMapping, 64> g_engineMappings;
std::atomic<uint64_t> g_mappingTick{0};
std::atomic<bool> g_mappingHooked{false};
using MappingFn = void (*)(void* mapping);
MappingFn g_mappingOriginal = nullptr;
const void* MappedSkeleton(uint8_t* handle) {
    if (!handle || !((*reinterpret_cast<uint32_t*>(handle + 0x20) >> 21) & 1)) return nullptr;
    uint8_t* obj = *reinterpret_cast<uint8_t**>(handle + 0x30);
    return obj ? *reinterpret_cast<const void**>(obj + 0x170) : nullptr;
}
bool MappingSkeletons(void* mapping, const void** source, const void** target) {
    __try {
        *source = MappedSkeleton(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mapping) + 0x70));
        *target = MappedSkeleton(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mapping) + 0x78));
        return *source && *target;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void HookMapping(void* mapping) {
    g_mappingOriginal(mapping);
    const void* source = nullptr;
    const void* target = nullptr;
    if (!mapping || !MappingSkeletons(mapping, &source, &target)) return;
    const uint64_t tick = g_mappingTick.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t slot = (reinterpret_cast<uintptr_t>(target) >> 4) % g_engineMappings.size();
    for (size_t probe = 0; probe < g_engineMappings.size(); ++probe, slot = (slot + 1) % g_engineMappings.size()) {
        auto& e = g_engineMappings[slot];
        const void* cur = e.target.load(std::memory_order_acquire);
        if (!cur) {
            if (!e.target.compare_exchange_strong(cur, target) && cur != target) continue;
        } else if (cur != target) {
            continue;
        }
        e.source.store(source, std::memory_order_release);
        e.seen.store(tick, std::memory_order_release);
        return;
    }
}
// The skeleton the engine maps `target` from (null when it maps none, or the mapping has not run lately).
const void* EngineMappingSource(const void* target) {
    const uint64_t now = g_mappingTick.load(std::memory_order_acquire);
    size_t slot = (reinterpret_cast<uintptr_t>(target) >> 4) % g_engineMappings.size();
    for (size_t probe = 0; probe < g_engineMappings.size(); ++probe, slot = (slot + 1) % g_engineMappings.size()) {
        const auto& e = g_engineMappings[slot];
        const void* cur = e.target.load(std::memory_order_acquire);
        if (!cur) return nullptr;
        if (cur != target) continue;
        // Stale after ~20000 mapping updates of any object (the target was unmapped or freed).
        return now - e.seen.load(std::memory_order_acquire) < 20000 ? e.source.load(std::memory_order_acquire) : nullptr;
    }
    return nullptr;
}

const Mat4* HierarchyMappedPose(Follower& f, const Mat4* src, uint32_t count) {
    const auto& o = g_overlay;
    const Rig& r = g_rig;
    const bool engineKnown = g_mappingHooked.load(std::memory_order_relaxed);
    if (f.nameState == 2 && g_planSerial - f.nameBuilt <= 90) return nullptr;
    if (f.nameState != 1 || g_planSerial - f.nameBuilt > 45) {
        const uint8_t wasState = f.nameState;
        f.nameState = 2;
        f.nameBuilt = g_planSerial;
        void* skel = SkeletonOf(src);
        std::array<uint32_t, 256> crcs{};
        uint32_t skelCount = 0;
        if (!skel || skel == r.skeleton || !CopyMem(&skelCount, skel, sizeof(skelCount)) || skelCount != count ||
            !ReadSkeletonNames(skel, count, f.parents.data(), crcs.data()))
            return nullptr;
        // The engine's relation (hooked) when available; the 0.22.0 position test only on a build without that hook.
        const bool engineMapped = engineKnown && EngineMappingSource(skel) == r.skeleton;
        uint32_t named = 0, onHers = 0;
        for (uint32_t j = 0; j < count; ++j) {
            f.nameMap[j] = -1;
            for (uint32_t k = 0; k < r.count && k < o.count; ++k) {
                if (r.crc[k] != crcs[j]) continue;
                ++named;
                const bool on = SameBone(g_current[j], o.clean[k]);
                if (on) ++onHers;
                if (engineMapped || on) f.nameMap[j] = int16_t(k);
                break;
            }
        }
        f.nameMapped = engineMapped ? named : onHers;
        if (engineMapped ? named >= 2 : (onHers >= 2 && onHers == named)) f.nameState = 1;
        if (f.nameState == 1 && wasState != 1 && engineMapped && g_characterLogs.fetch_add(1) < 40)
            Log("FPBODY skeleton %p is mapped from hers by the engine (HIERARCHY_MAPPING): %u of %u bones paired by name "
                "(%u of them on her bones this frame), the other %u follow them; drawn with her drawn pose", skel, named,
                count, onHers, count - named);
        if (!f.nameLogged && (f.nameState == 1 || (named >= 2 && onHers >= 2))) {
            f.nameLogged = true;
            if (f.nameState == 1 && !engineMapped)
                Log("FPBODY skeleton mapped onto hers (engine hierarchy mapping, bones paired by name): %u of %u bones copy "
                    "hers, the other %u follow them (drawn from her drawn pose)", onHers, count, count - onHers);
            else if (f.nameState != 1)
                Log("FPBODY skeleton with %u bones named like hers, %u of them on her bones: not taken as mapped onto her%s",
                    named, onHers, engineKnown ? " (the engine maps it from no skeleton of hers)" : "");
        }
        if (f.nameState != 1) return nullptr;
    }
    // Paired bones: her bone's own edit (clean -> drawn) applied to the paired bone (where it sits exactly on hers this
    // is her drawn bone); the rest from their parent, parents first (any index order); a root of its own hangs on its
    // node, which the engine puts on her node (FUN_14026a5f0): it turns with her drawn body.
    std::array<bool, 256> done{};
    uint32_t left = count;
    for (uint32_t j = 0; j < count; ++j)
        if (f.nameMap[j] >= 0) {
            const uint32_t k = uint32_t(f.nameMap[j]);
            Mat4 inv;
            f.pose[j] = SameBone(g_current[j], o.clean[k]) || !detail::AffineInverse(o.clean[k], inv)
                            ? g_renderPose[k]
                            : pose::Mul(pose::Mul(g_current[j], inv), g_renderPose[k]);
            if (!fp::ValidMatrix(f.pose[j])) f.pose[j] = g_renderPose[k];
            done[j] = true;
            --left;
        }
    for (uint32_t pass = 0; left && pass < count; ++pass) {
        bool progress = false;
        for (uint32_t j = 0; j < count; ++j) {
            if (done[j]) continue;
            const uint32_t p = f.parents[j];
            if (p < count && !done[p]) continue;
            Mat4 inv;
            if (p < count && detail::AffineInverse(g_current[p], inv))
                f.pose[j] = pose::Mul(pose::Mul(g_current[j], inv), f.pose[p]);
            else  // a root of its own: its node is on her node (FUN_1411dd920), so it turns with her drawn body
                f.pose[j] = g_bodyDeltaValid ? pose::Mul(g_current[j], g_bodyDelta) : g_current[j];
            if (!fp::ValidMatrix(f.pose[j])) f.pose[j] = g_current[j];
            done[j] = true;
            --left;
            progress = true;
        }
        if (!progress) break;
    }
    if (left) return nullptr;
    ++g_status.attachedHidden;
    return f.pose.data();
}

// EXP-056: `rigid` receives the follower when the element is not a bone copy of her (the caller tries the engine's own
// attachment, ItemSource, before the motion-learned RigidFollower).
const Mat4* FollowerSource(const Mat4* src, uint32_t count, Follower** rigid) {
    if (rigid) *rigid = nullptr;
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
    if (f->builtSerial && f->mapped < 8 && f->nameState != 1 && f->attachDecided && f->attachBone < 0 &&
        g_planSerial - f->builtSerial <= 45)
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
    if (g_rig.nameRules)
        if (const Mat4* mapped = HierarchyMappedPose(*f, src, count)) return mapped;
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
    if (rigid) *rigid = f;
    return src;
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
    // EXP-056 order: a bone copy of her skeleton (clothing with its own skeleton), then the engine's attachment of the
    // item's node to her (bone or body node), and only then the motion-learned attachment for unlinked objects.
    // Young Jodie keeps the earlier order (item link first).
    Follower* rigid = nullptr;
    const uint32_t count = src ? ElementCount(elem) : 0;
    if (use == src && src && !g_rig.nameRules) use = ItemSource(elem, src);
    if (use == src && src) use = FollowerSource(src, count, &rigid);
    if (use == src && src && g_rig.nameRules) use = ItemSource(elem, src);
    // (ItemSource may have recomputed her pose through g_current: the object's bones are read again.)
    if (use == src && src && rigid && CopyMem(g_current.data(), src, size_t(count) * sizeof(Mat4)))
        use = RigidFollower(*rigid, src, count);
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

// EXP-056 diagnostics for held items (owner: "make for every item a visualisation of how it stays in hand"): each item
// node on one of her hand bones is logged with where it sits in the animated hand's frame and in the drawn hand's frame
// (identical when the item stays exactly where the animation holds it), and how far the drawn hand is from the controller
// hand. tools/fp_items_report.py turns the FPITEM lines into one picture per item.
struct ItemLog {
    const void* node = nullptr;
    uint64_t lastLog = 0;
    uint32_t lines = 0;
    // 0.23.0 (owner: "in my hand i've had the backpack but sometimes it disappeared and appeared back"): the last frame
    // the item was drawn (any of its pieces, any pass), and where: drawn and animated world.
    int hand = -1;
    uint64_t seenSerial = 0, missingSince = 0;
    bool missing = false, gone = false;
    Mat4 clean{}, drawn{};
};
std::array<ItemLog, 64> g_itemLogs;

// Angle (degrees) between the view and the direction from the eye to p, and the distance (m).
void ViewAngle(const Vec3& p, float* deg, float* dist) {
    const Vec3 d = fp::Sub(p, g_plan.headPos);
    *dist = fp::Len(d);
    const float f = fp::Len(g_plan.headFwd);
    if (*dist < 1e-4f || f < 1e-4f) { *deg = 0.f; return; }
    const float c = std::clamp((d.x * g_plan.headFwd.x + d.y * g_plan.headFwd.y + d.z * g_plan.headFwd.z) / (*dist * f), -1.f, 1.f);
    *deg = std::acos(c) * 57.29578f;
}

// Once per drawn first-person frame: a hand item that was drawn and has not been for 3 frames is logged, with where it was
// last drawn (distance from the eyes: the near plane is 0.1 m; angle from the view) and where the animation holds it (the
// game's own visibility works on the animated scene). Logged again when it is drawn again. Not drawn for 10 s = gone (the
// game hid or detached it): no line.
void LogHeldCullStats();
void CheckHeldItemsDrawn() {
    LogHeldCullStats();
    static uint64_t window = 0;
    static int lines = 0;
    const uint64_t now = GetTickCount64();
    if (now - window >= 10000) { window = now; lines = 0; }
    for (auto& e : g_itemLogs) {
        if (!e.node || e.hand < 0 || e.gone || !e.seenSerial) continue;
        const uint64_t age = g_renderSerial - e.seenSerial;
        float dd = 0.f, da = 0.f, ad = 0.f, aa = 0.f;
        ViewAngle(Position(e.drawn), &da, &dd);
        ViewAngle(Position(e.clean), &aa, &ad);
        if (!e.missing && age >= 3) {
            e.missing = true;
            e.missingSince = e.seenSerial;
            if (++lines <= 20)
                Log("FPITEM item=%p (%s hand) not drawn for 3 frames: last drawn %.2f m from the eyes, %.0f deg from the view; "
                    "where the animation holds it %.2f m from the eyes, %.0f deg from the view (%.2f m from the drawn one)",
                    e.node, e.hand ? "right" : "left", dd, da, ad, aa, fp::Len(fp::Sub(Position(e.drawn), Position(e.clean))));
        } else if (e.missing && age == 0) {
            e.missing = false;
            if (++lines <= 20)
                Log("FPITEM item=%p (%s hand) drawn again after %llu frames: %.2f m from the eyes, %.0f deg from the view; "
                    "animated %.2f m, %.0f deg", e.node, e.hand ? "right" : "left",
                    static_cast<unsigned long long>(g_renderSerial - e.missingSince), dd, da, ad, aa);
        } else if (e.missing && age >= 900) {
            e.gone = true;  // ~10 s: hidden or detached by the game
        }
    }
}
// ---- 0.24.0: her held items are culled by the engine where the animation holds them ----
// Owner 2026-11-05: "my bag wasn't in my hands, then I switched and it was, and then it kept disappearing and
// appearing (sometimes walking, sometimes standing still)". LIVE 0.23.0: the bag on EA_L_ATTACH01 was drawn exactly in
// the drawn hand (drift 0.0 cm), while the animation held it 0.63 m away and 64 deg off the view. STATIC: a mesh is
// drawn only if its cull record survives the visibility filter FUN_1409e7bb0 (records 0xd0 bytes: +0x00 an oriented
// box - centre, half extents, X axis, Y axis, Z = X x Y; FUN_1402cda30 tests it against each visible room's frustum
// planes in FUN_1402ce540; +0x40 the same box in its room's space when +0x80 has flag 0x40, made by ENTITY_ABC::vfunc_133
// with FUN_140c36630(box, room+0x1c0) when room+0x200; +0x90 the MESH, whose node is MESH+0xc8; +0x98 the room). The
// box is the animated one; the mod moves only the drawn pieces. So an item drawn in her hand vanished whenever its
// animated place left the eye's view.
// Now: after the engine's filter, each of her items it dropped is filtered again by the same engine function on a copy
// of its record with the box moved to the drawn place (drawn = clean x move, move = clean^-1 x drawn from the last drawn
// frame, half extents + 5 cm for that frame of lag); kept there, its index is added to the output (the output has room
// for every input index). Engine records are never written.
struct HeldPlacement {
    std::atomic<const void*> node{nullptr};
    std::atomic<uint32_t> version{0};
    std::atomic<uint64_t> serial{0};
    Mat4 move{};
};
std::array<HeldPlacement, 32> g_heldPlacements;
std::atomic<uint64_t> g_heldKept{0}, g_heldChecked{0};
std::atomic<bool> g_itemCull{true};
using FilterFn = void (*)(const uint16_t* in, uint32_t n, uint8_t* records, uint16_t* out, uint32_t* outCount, void* query);
FilterFn g_filterOriginal = nullptr;
using ObbToRoomFn = void (*)(float* box, const float* matrix);
ObbToRoomFn g_obbToRoom = nullptr;
uintptr_t g_roomVt = 0;

// Render path, under g_mutex (one writer at a time): the item node's move from its animated to its drawn world.
void NoteHeldPlacement(const void* node, const Mat4& clean, const Mat4& drawn) {
    Mat4 inv;
    if (!node || !detail::AffineInverse(clean, inv)) return;
    const Mat4 move = pose::Mul(inv, drawn);
    if (!fp::ValidMatrix(move)) return;
    HeldPlacement* slot = nullptr;
    for (auto& e : g_heldPlacements)
        if (e.node.load(std::memory_order_relaxed) == node) { slot = &e; break; }
    if (!slot) {
        for (auto& e : g_heldPlacements)
            if (!e.node.load(std::memory_order_relaxed) || g_renderSerial - e.serial.load(std::memory_order_relaxed) > 900) {
                slot = &e;
                break;
            }
    }
    if (!slot) return;
    const uint32_t v = slot->version.load(std::memory_order_relaxed);
    slot->version.store(v + 1, std::memory_order_release);  // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    slot->node.store(node, std::memory_order_relaxed);
    slot->move = move;
    slot->serial.store(g_renderSerial, std::memory_order_relaxed);
    slot->version.store(v + 2, std::memory_order_release);
}
// Any thread: the move of a held item drawn in the last few frames.
bool HeldMove(const void* node, Mat4* move) {
    const uint64_t now = g_renderSerial;
    for (auto& e : g_heldPlacements) {
        if (e.node.load(std::memory_order_acquire) != node) continue;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t v0 = e.version.load(std::memory_order_acquire);
            if (v0 & 1) continue;
            const Mat4 m = e.move;
            const uint64_t serial = e.serial.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.version.load(std::memory_order_relaxed) != v0 || e.node.load(std::memory_order_relaxed) != node) continue;
            if (now - serial > 3) return false;  // not drawn in first person lately
            *move = m;
            return true;
        }
        return false;
    }
    return false;
}
bool CopyRecord(const uint8_t* rec, uint8_t* out, const void** node) {
    __try {
        std::memcpy(out, rec, 0xd0);
        const uint8_t* mesh = *reinterpret_cast<uint8_t* const*>(rec + 0x90);
        *node = mesh ? *reinterpret_cast<const void* const*>(mesh + 0xc8) : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const void* RecordNode(const uint8_t* rec) {
    __try {
        const uint8_t* mesh = *reinterpret_cast<uint8_t* const*>(rec + 0x90);
        return mesh ? *reinterpret_cast<const void* const*>(mesh + 0xc8) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// The box (centre, half extents, X axis, Y axis) moved by `move` (row vectors, translation in m[12..14]).
void MoveBox(float* box, const Mat4& move) {
    const Vec3 c = pose::TransformPoint({box[0], box[1], box[2]}, move);
    const Vec3 o = pose::TransformPoint({0.f, 0.f, 0.f}, move);
    const Vec3 ax = fp::Sub(pose::TransformPoint({box[8], box[9], box[10]}, move), o);
    const Vec3 ay = fp::Sub(pose::TransformPoint({box[12], box[13], box[14]}, move), o);
    box[0] = c.x; box[1] = c.y; box[2] = c.z;
    for (int i = 4; i < 7; ++i) box[i] += 0.05f;
    box[8] = ax.x; box[9] = ax.y; box[10] = ax.z;
    box[12] = ay.x; box[13] = ay.y; box[14] = ay.z;
}
bool RoomSpaceBox(uint8_t* rec) {
    __try {
        if (!((*reinterpret_cast<uint32_t*>(rec + 0x80)) & 0x40)) return true;  // the room copy is not used for it
        const uint8_t* room = *reinterpret_cast<uint8_t* const*>(rec + 0x98);
        if (!room || !g_obbToRoom || !g_roomVt || *reinterpret_cast<const uintptr_t*>(room) != g_roomVt ||
            !*(room + 0x200))
            return false;
        std::memcpy(rec + 0x40, rec, 0x40);
        g_obbToRoom(reinterpret_cast<float*>(rec + 0x40), reinterpret_cast<const float*>(room + 0x1c0));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallFilter(const uint16_t* in, uint32_t n, uint8_t* recs, uint16_t* out, uint32_t* outCount, void* query) {
    __try {
        g_filterOriginal(in, n, recs, out, outCount, query);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void HookFilter(const uint16_t* in, uint32_t n, uint8_t* records, uint16_t* out, uint32_t* outCount, void* query) {
    g_filterOriginal(in, n, records, out, outCount, query);
    // Adult rigs only (owner rule 2026-10-13: young Jodie stays exactly as accepted).
    if (!g_itemCull.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_acquire) || !in || !records ||
        !out || !outCount || n == 0 || !g_plan.firstPerson || !g_rig.adult)
        return;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t idx = in[i];
        uint8_t* rec = records + size_t(idx) * 0xd0;
        const void* node = RecordNode(rec);
        Mat4 move;
        if (!node || !HeldMove(node, &move)) continue;
        bool kept = false;
        for (uint32_t j = 0; j < *outCount && !kept; ++j) kept = out[j] == idx;
        if (kept) continue;
        g_heldChecked.fetch_add(1, std::memory_order_relaxed);
        alignas(16) uint8_t copy[0xd0];
        const void* copyNode = nullptr;
        if (!CopyRecord(rec, copy, &copyNode) || copyNode != node) continue;
        MoveBox(reinterpret_cast<float*>(copy), move);
        if (!RoomSpaceBox(copy)) continue;
        const uint16_t zero = 0;
        uint16_t one = 0;
        uint32_t m = 0;
        if (!CallFilter(&zero, 1, copy, &one, &m, query) || m != 1 || *outCount >= n) continue;
        out[(*outCount)++] = idx;
        const uint64_t k = g_heldKept.fetch_add(1, std::memory_order_relaxed);
        if (k < 10)
            Log("FPITEM item node %p: the engine's visibility dropped it where the animation holds it and keeps it where "
                "it is drawn (#%llu)", node, static_cast<unsigned long long>(k + 1));
    }
}
void LogHeldCullStats() {
    static uint64_t window = 0, lastKept = 0, lastChecked = 0;
    const uint64_t now = GetTickCount64();
    if (now - window < 30000) return;
    window = now;
    const uint64_t kept = g_heldKept.load(), checked = g_heldChecked.load();
    if (checked != lastChecked)
        Log("FPITEM 30 s: %llu visibility tests dropped one of her items at its animated place; %llu of them kept it at "
            "its drawn place", static_cast<unsigned long long>(checked - lastChecked),
            static_cast<unsigned long long>(kept - lastKept));
    lastKept = kept;
    lastChecked = checked;
}

void LogHeldItem(const char* itemNode, int bone, const Mat4& cleanWorld, const Mat4& drawnWorld) {
    NoteHeldPlacement(itemNode, cleanWorld, drawnWorld);
    const Rig& r = g_rig;
    const auto& o = g_overlay;
    int h = -1;
    if (bone >= 0 && uint32_t(bone) < r.count)
        for (int s = 0; s < 2 && h < 0; ++s)
            if (r.nameRules ? r.handSide[s][bone]
                            : (r.hand[s].hand < r.count && (detail::Descendant(r.parents.data(), r.count, uint32_t(bone), r.hand[s].hand) ||
                                                            r.carried[s][bone])))
                h = s;
    const uint64_t now = GetTickCount64();
    ItemLog* slot = nullptr;
    for (auto& e : g_itemLogs)
        if (e.node == itemNode) { slot = &e; break; }
    if (!slot) {
        slot = &g_itemLogs[0];
        for (auto& e : g_itemLogs)
            if (!e.node || e.lastLog < slot->lastLog) { slot = &e; if (!e.node) break; }
        *slot = ItemLog{};
        slot->node = itemNode;
    }
    slot->hand = h;
    slot->seenSerial = g_renderSerial;
    slot->gone = false;
    slot->clean = cleanWorld;
    slot->drawn = drawnWorld;
    // Hand items: every 10 s for 5 minutes per item; anything else on her: once.
    if (slot->lines && (h < 0 || slot->lines >= 30 || now - slot->lastLog < 10000)) return;
    slot->lastLog = now;
    ++slot->lines;
    const char* name = rigmap::KnownName(BoneCrc(r.skeleton, bone >= 0 ? uint32_t(bone) : kAbsent));
    if (h < 0) {
        Log("FPITEM item=%p on %s %d (%s): drawn %.1f cm from where the animation has it", itemNode,
            bone < 0 ? "her body node" : "bone", bone, bone < 0 ? "turned with her body" : name ? name : "unnamed",
            fp::Len(fp::Sub(Position(drawnWorld), Position(cleanWorld))) * 100.f);
        return;
    }
    const uint32_t hb = r.hand[h].hand;
    Mat4 cleanInv, drawnInv;
    if (hb >= o.count || !detail::AffineInverse(o.clean[hb], cleanInv) || !detail::AffineInverse(g_renderPose[hb], drawnInv)) return;
    const Vec3 a = pose::TransformPoint(Position(cleanWorld), cleanInv), d = pose::TransformPoint(Position(drawnWorld), drawnInv);
    const float turn = RotationBetween(pose::Mul(cleanWorld, cleanInv), pose::Mul(drawnWorld, drawnInv)) * 57.29578f;
    float controller = -1.f;
    if (g_plan.armsActive && g_plan.handValid[h]) controller = fp::Len(fp::Sub(Position(g_renderPose[hb]), Position(g_plan.handWorld[h])));
    Log("FPITEM item=%p on bone %d (%s) %s hand: animated hand frame %.1f %.1f %.1f cm, drawn hand frame %.1f %.1f %.1f cm, "
        "drift %.1f cm %.1f deg; drawn wrist %.1f cm from the controller grip%s",
        itemNode, bone, name ? name : "unnamed", h ? "right" : "left", a.x * 100.f, a.y * 100.f, a.z * 100.f, d.x * 100.f,
        d.y * 100.f, d.z * 100.f, fp::Len(fp::Sub(a, d)) * 100.f, turn, controller * 100.f,
        controller < 0.f ? " (arms not on the controllers)" : "");
}

// parent = the matrix pointer an element reads; returns true and fills g_propWorld when it is the
// world of a node chained to one of Jodie's bones, or (EXP-056) to her body node.
// STATIC NODE::vfunc_106 (0x140849590): a node's parent world for bone b is the skeleton's world b (+0x170 -> +0x10 +
// b * 0x40); a bone out of range (-1: AttachObjectToBodyNode, FUN_140dcbd60 with 0xffffffff) gives the node world
// (+0x80). Items on her body node (owner: "the bag on my back is not on my back when my body turns") were left where the
// animated body is; they now take the drawn body's rigid transform (clean x g_bodyDelta), and one at her head (within
// 25 cm of it, i.e. something worn on the head) goes with the hidden head instead.
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
            if (bone < 0 || uint32_t(bone) >= count) {
                if (!g_bodyDeltaValid || !g_rig.nameRules) return false;
                Mat4 clean;
                if (!ReadMatrix(parent, &clean) || !fp::ValidMatrix(clean)) return false;
                const auto& o = g_overlay;
                const uint32_t head = g_rig.head;
                Mat4 world = pose::Mul(clean, g_bodyDelta), headInv;
                if (head < o.count && fp::Len(fp::Sub(Position(clean), Position(o.clean[head]))) < 0.25f &&
                    detail::AffineInverse(o.clean[head], headInv))
                    world = pose::Mul(pose::Mul(clean, headInv), g_renderPose[head]);
                if (!fp::ValidMatrix(world)) return false;
                g_propWorld = world;
                LogHeldItem(chain[0].node, -1, clean, world);
                return true;
            }
            g_socketBone[bone] = true;
            g_socketSeen[bone] = g_renderSerial;
            Mat4 world = g_renderPose[bone];
            for (int i = n - 1; i >= 0; --i) world = pose::Mul(chain[i].local, world);
            if (!fp::ValidMatrix(world)) return false;
            g_propWorld = world;
            Mat4 clean;
            if (ReadMatrix(parent, &clean)) LogHeldItem(chain[0].node, bone, clean, world);
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
    const Mat4* use = ClothSource(*worlds);
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
// EXP-056: teleports. STATIC HAVOK_CLOTH_INSTANCE::vfunc_13: its reference bone is data[0x10][*(*(this+0xf8)+8)];
// the bone's previous world is kept at this+0x80. vfunc_07 (0x140c528b0, slot 7) sets this+0x16c |= 0x100, and the
// next vfunc_13 then moves every cloth particle rigidly from the previous to the current reference frame and copies the
// positions over the previous ones (FUN_140c6f460: no velocity), or resets the cloth to its skinned pose when the frame
// turned more than 45 deg (FUN_140c52550). The game uses it when it moves a character instantly. The first-person
// pose jumps the same way (F1 on / off, a snap turn, a recentre, the head-follow shift catching up): without the
// teleport the dress was flung by the jump (owner: "the bottom part breaks and gets lifted", the back of clothes inside
// her body). So whenever the first-person part of her cloth's reference frame jumps (more than 10 deg or 8 cm from
// the last update), the engine's teleport is requested before the update.
struct ClothTrack {
    void* inst = nullptr;
    Mat4 delta = pose::Identity();  // clean reference x delta = the reference frame this cloth was given
    bool posed = false;             // the first-person pose was given (delta not identity)
    uint64_t lastSeen = 0, teleports = 0, updates = 0;
    uint32_t ref = kAbsent;
    bool logged = false;
};
std::array<ClothTrack, 64> g_clothTrack;
std::atomic<int> g_clothPosed{0};       // tracked instances last given the first-person pose
uint64_t g_clothTick = 0;               // update counter (track ages)
struct ClothSteps {                     // diagnostic: how often her cloth is stepped per animation frame
    uint64_t since = 0, calls = 0, framesAt = 0;
    float dtMin = 1e9f, dtMax = 0.f;
    std::array<void*, 32> seen{};
    uint32_t seenCount = 0;
} g_clothSteps;
bool ReadClothRef(void* self, uint32_t* bone) {
    __try {
        const auto* info = *reinterpret_cast<const uint8_t* const*>(static_cast<char*>(self) + 0xf8);
        if (!info) return false;
        const auto* rec = *reinterpret_cast<const uint32_t* const*>(info + 8);
        if (!rec) return false;
        *bone = *rec;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using ClothTeleportFn = void (*)(void*, uint16_t);
bool RequestClothTeleport(void* self) {
    __try {
        void** vt = *reinterpret_cast<void***>(self);
        // Same class as the hooked update (vfunc_13 is the verified anchor): slot 7 is its teleport.
        if (!vt || vt[13] != game::Addr(game::Anchor::ClothInstanceUpdate)) return false;
        reinterpret_cast<ClothTeleportFn>(vt[7])(self, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float RotationBetween(const Mat4& a, const Mat4& b) {
    const Mat4 ra = Rotation(a), rb = Rotation(b);
    float tr = 0.f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tr += ra.m[i * 4 + j] * rb.m[i * 4 + j];
    return std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f));
}
std::atomic<bool> g_clothTeleports{true};  // dev: fpclothjump on|off
void HookClothUpdate(void* self, float dt, uint64_t* data) {
    const bool active = g_active.load(std::memory_order_acquire) != nullptr;
    if (!data || (!active && g_clothPosed.load(std::memory_order_relaxed) == 0)) return g_clothUpdateOriginal(self, dt, data);
    const Mat4 *src = nullptr, *use = nullptr;
    {   // not held across the game call: the cloth update may reach other hooked functions
        std::lock_guard<std::mutex> lock(g_mutex);
        src = reinterpret_cast<const Mat4*>(ClothBones(data));
        use = src && active ? RenderSource(src) : src;
        if (use != src) use = g_physicsPose.data();  // anchors: never the collapsed head / neck
        // 0.26.0: her head cloth (reference bone in the hidden head set: hair) is simulated 30 m down with the head.
        uint32_t headRef = kAbsent;
        if (use != src && g_headClothReady && ReadClothRef(self, &headRef) && headRef < g_rig.count &&
            g_rig.hideTree[headRef]) {
            use = g_headClothPose.data();
            HeadClothStats& h = g_headClothStats;
            ++h.updates;
            bool known = false;
            for (uint32_t i = 0; i < h.seenCount && !known; ++i) known = h.seen[i] == self;
            if (!known && h.seenCount < h.seen.size()) {
                h.seen[h.seenCount++] = self;
                const char* name = rigmap::KnownName(BoneCrc(g_rig.skeleton, headRef));
                Log("FPBODY head cloth %p (reference bone %u %s, in the hidden head set): simulated 30 m below while her "
                    "head is hidden (hair)", self, headRef, name ? name : "unnamed");
            }
            const uint64_t now = GetTickCount64();
            if (!h.since) h.since = now;
            if (now - h.since >= 30000) {
                Log("FPBODY head cloth (30 s): %u instance(s) hidden with her head, %llu updates; cloth skinning / bone "
                    "input calls inside their updates %llu, other cloth calls on her %llu", h.seenCount,
                    static_cast<unsigned long long>(h.updates), static_cast<unsigned long long>(h.inside),
                    static_cast<unsigned long long>(h.outside));
                h = HeadClothStats{};
                h.since = now;
            }
        }
        if (use == src || !Aligned16(use) ||
            !SwapClothBones(data, reinterpret_cast<uint64_t>(src), reinterpret_cast<uint64_t>(use)))
            use = nullptr;
        else
            ++g_status.clothWrites;
        ++g_clothTick;
        ClothTrack* t = nullptr;
        for (auto& e : g_clothTrack) {
            if (e.inst == self) { t = &e; continue; }
            if (e.inst && g_clothTick - e.lastSeen > 5000) {  // a destroyed cloth instance: forget it
                if (e.posed) g_clothPosed.fetch_sub(1, std::memory_order_relaxed);
                e = ClothTrack{};
            }
        }
        if (!t && use) {
            ClothTrack* oldest = &g_clothTrack[0];
            for (auto& e : g_clothTrack) {
                if (!e.inst) { oldest = &e; break; }
                if (e.lastSeen < oldest->lastSeen) oldest = &e;
            }
            if (oldest->posed) g_clothPosed.fetch_sub(1, std::memory_order_relaxed);
            *oldest = ClothTrack{};
            oldest->inst = self;
            t = oldest;
        }
        if (t) {
            t->lastSeen = g_clothTick;
            ++t->updates;
            uint32_t ref = kAbsent;
            Mat4 cleanRef{}, inv;
            Mat4 delta = pose::Identity();
            const bool haveRef = src && ReadClothRef(self, &ref) && ref < g_overlay.count && CopyMem(&cleanRef, src + ref, sizeof(Mat4));
            const bool posed = use && haveRef && detail::AffineInverse(cleanRef, inv);
            if (posed) delta = pose::Mul(inv, use[ref]);
            if (!t->logged && haveRef) {
                t->logged = true;
                t->ref = ref;
                const char* name = g_rig.valid ? rigmap::KnownName(BoneCrc(g_rig.skeleton, ref)) : nullptr;
                Log("FPBODY her cloth %p: reference bone %u (%s)%s, %s pose", self, ref, name ? name : "unnamed",
                    g_rig.valid && ref < g_rig.count && g_rig.hideTree[ref] ? ", a hidden head bone" : "",
                    g_rig.nameRules && ClothOnePose() ? "one cloth" : "earlier");
            }
            bool jump = false;
            float angle = 0.f, move = 0.f;
            if (haveRef && (posed || t->posed)) {
                const Mat4 was = pose::Mul(cleanRef, t->posed ? t->delta : pose::Identity());
                const Mat4 now = posed ? use[ref] : cleanRef;
                angle = RotationBetween(was, now);
                move = fp::Len(fp::Sub(Position(was), Position(now)));
                jump = posed != t->posed || angle > 0.1745f || move > 0.08f;
            }
            // Named rigs only: young Jodie keeps her accepted cloth behaviour (owner, 2026-10-13).
            if (jump && g_rig.nameRules && g_clothTeleports.load(std::memory_order_relaxed) && RequestClothTeleport(self)) {
                ++t->teleports;
                if (t->teleports <= 20 || t->teleports % 100 == 0)
                    Log("FPBODY cloth %p teleported with the first-person pose (%s, %.0f deg, %.2f m; %llu so far)", self,
                        posed == t->posed ? "pose jump" : posed ? "first person on" : "first person off",
                        angle * 57.29578f, move, static_cast<unsigned long long>(t->teleports));
            }
            if (posed != t->posed) g_clothPosed.fetch_add(posed ? 1 : -1, std::memory_order_relaxed);
            t->posed = posed;
            t->delta = delta;
        }
        // Diagnostic (EXP-019/020 risk: per-eye frame keys could step cloth twice per frame): her cloth updates per
        // animation frame, logged every 30 s while first person is on.
        if (use) {
            ClothSteps& s = g_clothSteps;
            const uint64_t now = GetTickCount64();
            if (!s.since) { s.since = now; s.framesAt = g_status.selectedFinalCalls; }
            ++s.calls;
            s.dtMin = std::min(s.dtMin, dt);
            s.dtMax = std::max(s.dtMax, dt);
            bool known = false;
            for (uint32_t i = 0; i < s.seenCount && !known; ++i) known = s.seen[i] == self;
            if (!known && s.seenCount < s.seen.size()) s.seen[s.seenCount++] = self;
            if (now - s.since >= 30000) {
                const uint64_t frames = g_status.selectedFinalCalls - s.framesAt;
                Log("FPBODY cloth steps (30 s): %u cloth instance(s) of hers, %.2f updates per instance per animation "
                    "frame (%llu updates, %llu frames), dt %.1f..%.1f ms", s.seenCount,
                    frames && s.seenCount ? double(s.calls) / double(frames) / double(s.seenCount) : 0.0,
                    static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(frames), s.dtMin * 1000.f,
                    s.dtMax * 1000.f);
                s = ClothSteps{};
            }
        }
    }
    // While a head cloth updates, its skinning / bone input on this thread gets the same pose (ClothSource).
    const Mat4* prevCloth = t_clothPose;
    t_clothPose = use == g_headClothPose.data() ? use : nullptr;
    g_clothUpdateOriginal(self, dt, data);
    t_clothPose = prevCloth;
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
    if (active && skeleton != active) RegisterSkeleton(skeleton);  // EXP-056: hierarchy-mapped objects
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
    // 0.24.0: the engine's own hierarchy mappings (the backpack follows her by HIERARCHY_MAPPING) and the visibility
    // filter of mesh records (held items culled where the animation holds them).
    if (void* hm = reinterpret_cast<void*>(game::Va(0x1411dd920))) {
        const bool ok = MH_CreateHook(hm, reinterpret_cast<void*>(&HookMapping), reinterpret_cast<void**>(&g_mappingOriginal)) ==
                            MH_OK &&
                        MH_EnableHook(hm) == MH_OK;
        g_mappingHooked = ok;
        Log("FPBODY engine hierarchy mappings (HIERARCHY_MAPPING update, e.g. the backpack): %s",
            ok ? "followed: a skeleton the engine maps from hers is drawn with her drawn pose" : "FAILED");
    } else {
        Log("FPBODY HIERARCHY_MAPPING update not found in this build: mapped skeletons by position as in 0.22.0");
    }
    g_obbToRoom = reinterpret_cast<ObbToRoomFn>(game::Va(0x140c36630));
    g_roomVt = game::Va(0x141710560);
    if (void* vf = reinterpret_cast<void*>(game::Va(0x1409e7bb0))) {
        const bool ok = MH_CreateHook(vf, reinterpret_cast<void*>(&HookFilter), reinterpret_cast<void**>(&g_filterOriginal)) ==
                            MH_OK &&
                        MH_EnableHook(vf) == MH_OK;
        Log("FPBODY her items in the engine's visibility filter: %s (room-space boxes %s)",
            ok ? "tested where they are drawn when dropped where the animation holds them" : "FAILED",
            g_obbToRoom && g_roomVt ? "converted by the engine" : "not available: such items keep the engine's result");
    } else {
        Log("FPBODY visibility filter not found in this build: held items keep the engine's own visibility");
    }
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

void Identify(void* skeleton, int family, const char* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto f = static_cast<bodykind::Family>(family);
    if (g_identity.skeleton == skeleton && g_identity.family == f) return;
    g_identity.skeleton = skeleton;
    g_identity.family = f;
    strncpy_s(g_identity.why, why ? why : "", _TRUNCATE);
    // A rig already described for this skeleton is rebuilt with the new identity.
    if (g_rig.skeleton == skeleton && g_rig.family != f) g_rig.skeleton = nullptr;
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
void SetClothOnePose(bool on) { g_clothOnePose = on; }
void SetClothTeleports(bool on) { g_clothTeleports = on; }
void SetHeadCloth(bool on) { g_headCloth = on; }
void SetItemCull(bool on) { g_itemCull = on; }
void SetHeadMode(int mode) { g_headMode = mode < 0 || mode > 2 ? 2 : mode; }
int GetHeadMode() { return g_headMode.load(std::memory_order_relaxed); }
const char* HeadModeName(int mode) {
    return mode == 0 ? "shown" : mode == 1 ? "hidden" : "hidden in first-person cutscenes (F3), shown in F1 gameplay";
}

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
