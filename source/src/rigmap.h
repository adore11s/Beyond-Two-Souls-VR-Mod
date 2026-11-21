#pragma once
// EXP-056: the rig convention of Quantic Dream's character skeletons, read from the game's data instead of guessed
// from motion or distance.
//
// SOURCE (BigFile packages, every JHO9 / JHO14 / JHO16 / JHO20 and possession rig dumped so far, 15 skeletons): bone
// names are CRC32 hashes in the skeleton's table (+0x270, STATIC FUN_140977950); the names come from the rigs'
// expression and node tables in the BigFile archives (tools/rig_names.py builds the dictionary).
//   LC_<X>, LC_EX_<X>, LC_EA_<X>      constraint copies of the bone <X> (LC_L_Hand -> L_Hand, LC_EX_R_ForeArm ->
//                                     R_ForeArm), parented to M_Hips or a group (LC_EX_M_GRP, LC_EA_M_GRP); the
//                                     engine keeps them on <X>, their children hang off them with authored locals.
//   PX_*                              fingers (under LC_<S>_Hand).
//   EX_*                              expression-driven skin helpers under an LC_EX_ group (rig expressions such as
//                                     `EX_R_Arm_Roll2.rotateX = 0.6667 * BB_R_Arm_TwistX.twist`).
//   EX_<S>_ForeArm_Roll1..3           forearm roll helpers: they share the wrist twist out along the forearm.
//   EA_<S>_ATTACH01 / 02, EA_M_ATTACH* item sockets (the scripts' AttachObjectToBone targets, STATIC FUN_140edafc0 ->
//                                     FUN_140dcbd60: a bone looked up by the CRC of its name).
//   FA_* under LC_M_Head              the face rig; LC_EX_M_Head holds the eye-blink and jaw helpers.
// The head that first person hides is exactly M_Neck (with M_Neck1 / M_Head) plus the LC_M_Head and LC_EX_M_Head
// groups. The old distance rule (EXP-034) also hid the shoulder and arm helpers that were near her face when the rig
// was built (LIVE dumps: EX_*_UpShoulder, LC_EX_*_Shoulder, EX_*_BackArm / FrontArm, and in one adult rig the whole
// right arm helper chain), which collapsed that skin onto her neck.
//
// EXP-063 (tools/rig_extract.py + rig_viz.py: all 289 character rigs read from the BigFile archives and drawn):
//   * The hierarchy stores a class word per bone next to its name hash (NODEAR table, runtime +0x270 second dword,
//     STATIC FUN_14097d5c0): 1 body, 2 face, 4 fingers, 8 sockets, 16 expression helpers, +32 = copy bone. On every
//     rig each flagged copy bone that has a same-named bone rests exactly on it (bind pose, +0x300), so a flagged
//     copy bone whose name the list below does not cover is matched to the body bone it rests on: LC_EA_<S>_ATTACH01
//     (the item socket's parent on some outfits, resting on the hand), unnamed head copies carrying a hat / hood /
//     mask socket, forearm copies on possession bodies.
//   * A copy of a hidden bone is hidden with it (head-mounted sockets and what hangs below them).
//   * EX_<S>_ForeArm_Roll<n> are numbered from the WRIST back to the elbow: Roll1 rests at 84 % of the forearm,
//     Roll2 61 %, Roll3 40 %, Roll4 18 % (adult Jodie; background bodies have two at 50 % and 96 %, some one).
//     Each takes the share of the wrist twist of its place along the forearm (RollShares). The earlier 1/3, 2/3, 1
//     by number twisted the middle of the forearm more than both ends (owner 2026-11-11: "twisted like balloons").
#include "bones_crc.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace b2s::rigmap {

constexpr uint32_t kMax = 1024;
constexpr uint32_t kNone = kMax;
constexpr int kMaxRolls = 6;
constexpr uint32_t kFlagBody = 1, kFlagCopy = 32;

struct Map {
    uint32_t count = 0;
    bool named = false;                 // both hands have a named copy group (LC_<S>_Hand or LC_EX_<S>_Hand)
    uint16_t driver[kMax];              // a copy bone -> the bone it copies; kNone otherwise
    bool hide[kMax];                    // head, neck and face
    uint8_t roll[kMax];                 // EX_<S>_ForeArm_Roll<n>: n (1..kMaxRolls), 0 otherwise
    bool socket[kMax];                  // EA_*_ATTACH* item sockets
    uint32_t neck = kNone;              // M_Neck: where the hidden bones collapse
    uint32_t lcHand[2]{kNone, kNone};   // LC_L_Hand, LC_R_Hand (finger groups)
    uint32_t rolls[2][kMaxRolls];       // [side][n-1], 0 = left
    float rollShare[2][kMaxRolls]{};    // share of the wrist twist each roll helper takes (RollShares)
    uint32_t fore[2]{kNone, kNone}, hand[2]{kNone, kNone};  // <S>_ForeArm, <S>_Hand
    uint32_t drivers = 0, hidden = 0, sockets = 0;
    uint32_t restDrivers = 0;           // copy bones matched by their rest place (not in the name list)
    uint32_t hiddenCopies = 0;          // bones hidden because they hang off a copy of a hidden bone
    Map() {
        for (auto& side : rolls)
            for (uint32_t& r : side) r = kNone;
    }
};

namespace detail {
inline bool Under(const uint32_t* parents, uint32_t count, uint32_t bone, uint32_t ancestor) {
    for (uint32_t steps = 0; bone < count && steps <= count; ++steps) {
        if (bone == ancestor) return true;
        bone = parents[bone];
    }
    return false;
}
}  // namespace detail

// `find(crc)` returns the bone with that name hash, or any value >= count when there is none.
// `flags` (count dwords, the hierarchy's class word per bone) and `rest` (count x 16 floats, bind-pose WORLD matrices,
// row-major, translation at 12..14) are optional: with both, flagged copy bones outside the name list are matched by
// their rest place.
template <class Find>
void Build(Map& m, uint32_t count, const uint32_t* parents, Find find, const uint32_t* flags = nullptr,
           const float* rest = nullptr) {
    m = Map{};
    if (!parents || !count || count > kMax) return;
    m.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        m.driver[i] = uint16_t(kNone);
        m.hide[i] = m.socket[i] = false;
        m.roll[i] = 0;
    }
    auto bone = [&](const char* name) {
        const uint32_t b = find(bones::Crc32(name));
        return b < count ? b : kNone;
    };
    static const char* const kBase[] = {
        "M_Hips", "M_Spine", "M_Spine1", "M_Spine2", "M_Spine3", "M_Neck", "M_Neck1", "M_Head",
        "L_Shoulder", "L_Arm", "L_ForeArm", "L_Hand", "L_UpLeg", "L_Leg", "L_Foot", "L_ToeBase",
        "R_Shoulder", "R_Arm", "R_ForeArm", "R_Hand", "R_UpLeg", "R_Leg", "R_Foot", "R_ToeBase"};
    static const char* const kCopy[] = {"LC_", "LC_EX_", "LC_EA_", "LC_PX_", "LC_FA_"};
    char name[64];
    for (const char* base : kBase) {
        const uint32_t x = bone(base);
        if (x == kNone) continue;
        for (const char* prefix : kCopy) {
            int n = 0;
            for (const char* s = prefix; *s && n < 60; ++s) name[n++] = *s;
            for (const char* s = base; *s && n < 63; ++s) name[n++] = *s;
            name[n] = 0;
            const uint32_t c = bone(name);
            if (c != kNone && c != x && !detail::Under(parents, count, x, c)) {  // never a copy of its own descendant
                m.driver[c] = uint16_t(x);
                ++m.drivers;
            }
        }
    }
    if (flags && rest) {
        auto same = [&](uint32_t a, uint32_t b) {
            const float* x = rest + size_t(a) * 16;
            const float* y = rest + size_t(b) * 16;
            for (int k = 0; k < 15; ++k) {
                if (k == 3 || k == 7 || k == 11) continue;
                if (!(std::fabs(x[k] - y[k]) < 1e-3f)) return false;  // 1 mm, and the same axes
            }
            return true;
        };
        // The step is only trusted when the named copies confirm the rest pose (they rest on their bones).
        uint32_t named = 0, agree = 0;
        for (uint32_t c = 0; c < count; ++c)
            if (m.driver[c] < count) {
                ++named;
                agree += same(c, m.driver[c]);
            }
        if (named && agree * 10 >= named * 9)
            for (uint32_t c = 0; c < count; ++c) {
                if (!(flags[c] & kFlagCopy) || m.driver[c] < count) continue;
                for (uint32_t x = 0; x < count; ++x) {
                    if (x == c || !(flags[x] & kFlagBody) || (flags[x] & kFlagCopy)) continue;
                    // a group root (a child of the bone it rests on, no offset) is not a copy
                    if (parents[c] == x || detail::Under(parents, count, x, c) || !same(c, x)) continue;
                    m.driver[c] = uint16_t(x);
                    ++m.drivers;
                    ++m.restDrivers;
                    break;
                }
            }
    }
    m.lcHand[0] = bone("LC_L_Hand");
    m.lcHand[1] = bone("LC_R_Hand");
    m.named = (m.lcHand[0] != kNone || bone("LC_EX_L_Hand") != kNone) &&
              (m.lcHand[1] != kNone || bone("LC_EX_R_Hand") != kNone);
    // Head: M_Neck (else M_Neck1, else M_Head) and the head copy groups, never an arm or a hand group.
    m.neck = bone("M_Neck");
    if (m.neck == kNone) m.neck = bone("M_Neck1");
    if (m.neck == kNone) m.neck = bone("M_Head");
    const uint32_t roots[] = {m.neck, bone("LC_M_Head"), bone("LC_EX_M_Head"), bone("LC_EA_M_Head"), bone("LC_FA_M_Head")};
    const uint32_t keep[] = {bone("L_Arm"), bone("R_Arm"), bone("L_Shoulder"), bone("R_Shoulder"), m.lcHand[0], m.lcHand[1]};
    for (uint32_t b = 0; b < count; ++b) {
        bool head = false;
        for (uint32_t r : roots) head |= r != kNone && detail::Under(parents, count, b, r);
        for (uint32_t k : keep) head &= !(k != kNone && (detail::Under(parents, count, b, k) || detail::Under(parents, count, k, b)));
        m.hide[b] = head;
        m.hidden += head;
    }
    // A copy of a hidden bone (head / neck) is hidden with everything below it: head-mounted sockets, hats, masks.
    for (uint32_t c = 0; c < count; ++c) {
        if (m.driver[c] >= count || !m.hide[m.driver[c]] || m.hide[c]) continue;
        for (uint32_t b = 0; b < count; ++b) {
            if (m.hide[b] || !detail::Under(parents, count, b, c)) continue;
            bool kept = false;
            for (uint32_t k : keep)
                kept |= k != kNone && (detail::Under(parents, count, b, k) || detail::Under(parents, count, k, b));
            if (kept) continue;
            m.hide[b] = true;
            ++m.hidden;
            ++m.hiddenCopies;
        }
    }
    static const char kSide[2] = {'L', 'R'};
    for (int s = 0; s < 2; ++s) {
        char nm[32] = "EX_?_ForeArm_Roll?";
        nm[3] = kSide[s];
        for (int k = 0; k < kMaxRolls; ++k) {
            nm[17] = char('1' + k);
            const uint32_t r = bone(nm);
            m.rolls[s][k] = r;
            if (r != kNone) m.roll[r] = uint8_t(k + 1);
        }
        char fore[16] = "?_ForeArm";
        fore[0] = kSide[s];
        m.fore[s] = bone(fore);
        char hand[16] = "?_Hand";
        hand[0] = kSide[s];
        m.hand[s] = bone(hand);
    }
    static const char* const kSocket[] = {"EA_L_ATTACH01", "EA_L_ATTACH02", "EA_R_ATTACH01", "EA_R_ATTACH02",
                                          "EA_M_ATTACH", "EA_M_ATTACH01", "EA_M_ATTACH02"};
    for (const char* s : kSocket) {
        const uint32_t b = bone(s);
        if (b != kNone && !m.socket[b]) { m.socket[b] = true; ++m.sockets; }
    }
}

// For a head set made by another rule (young Jodie's distance rule): a bone outside the named head set that hangs
// below a copy bone whose own bone is not hidden belongs to that bone (shoulder, upper-arm and collar skin helpers
// under the copies of the spine, shoulders and arms) and is taken out. The nearest copy bone above it decides; a bone
// under no copy bone is left as the other rule decided. `notHead[b]` is set for every bone so decided, hidden before
// or not, so a later rule cannot hide it again. Returns the number of bones taken out.
inline uint32_t ReleaseNonHead(const Map& m, const uint32_t* parents, bool* hide, bool* notHead) {
    uint32_t released = 0;
    for (uint32_t b = 0; b < m.count; ++b) {
        notHead[b] = false;
        if (m.hide[b]) continue;
        uint32_t a = b;
        for (uint32_t steps = 0; a < m.count && steps <= m.count && m.driver[a] >= m.count; ++steps) a = parents[a];
        if (a >= m.count || m.driver[a] >= m.count || m.hide[m.driver[a]]) continue;
        notHead[b] = true;
        if (hide[b]) {
            hide[b] = false;
            ++released;
        }
    }
    return released;
}

// The share of the extra wrist twist each forearm roll helper takes: its place along the elbow-wrist line in the given
// pose (count x 16 floats, world matrices), 0 at the elbow and 1 at the wrist. Returns the number of helpers per side
// packed as left | right << 8.
inline uint32_t RollShares(Map& m, const float* world) {
    uint32_t found = 0;
    for (int s = 0; s < 2; ++s) {
        uint32_t n = 0;
        for (int k = 0; k < kMaxRolls; ++k) m.rollShare[s][k] = 0.f;
        if (!world || m.fore[s] >= m.count || m.hand[s] >= m.count) continue;
        const float* e = world + size_t(m.fore[s]) * 16 + 12;
        const float* w = world + size_t(m.hand[s]) * 16 + 12;
        const float ax = w[0] - e[0], ay = w[1] - e[1], az = w[2] - e[2];
        const float len2 = ax * ax + ay * ay + az * az;
        if (!(len2 > 1e-6f)) continue;
        for (int k = 0; k < kMaxRolls; ++k) {
            const uint32_t b = m.rolls[s][k];
            if (b >= m.count) continue;
            const float* p = world + size_t(b) * 16 + 12;
            float t = ((p[0] - e[0]) * ax + (p[1] - e[1]) * ay + (p[2] - e[2]) * az) / len2;
            if (!(t == t)) t = 0.f;
            m.rollShare[s][k] = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            ++n;
        }
        found |= n << (8 * s);
    }
    return found;
}

// Name of a well-known bone for log lines (nullptr when the hash is not one of them).
inline const char* KnownName(uint32_t crc) {
    static const char* const kNames[] = {
        "M_Hips", "M_Spine", "M_Spine1", "M_Spine2", "M_Neck", "M_Neck1", "M_Head", "L_Shoulder", "L_Arm",
        "L_ForeArm", "L_Hand", "R_Shoulder", "R_Arm", "R_ForeArm", "R_Hand", "LC_L_Hand", "LC_R_Hand",
        "LC_EX_L_Hand", "LC_EX_R_Hand", "LC_EX_L_ForeArm", "LC_EX_R_ForeArm", "LC_EX_L_Arm", "LC_EX_R_Arm",
        "LC_EX_M_Spine2", "LC_EA_M_Spine2", "EA_L_ATTACH01", "EA_L_ATTACH02", "EA_R_ATTACH01", "EA_R_ATTACH02",
        "EA_M_ATTACH", "EA_M_ATTACH01", "EX_L_ForeArm_Roll1", "EX_L_ForeArm_Roll2", "EX_L_ForeArm_Roll3",
        "EX_R_ForeArm_Roll1", "EX_R_ForeArm_Roll2", "EX_R_ForeArm_Roll3", "EX_L_ForeArm_Roll4", "EX_R_ForeArm_Roll4",
        "LC_EA_L_ATTACH01", "LC_EA_R_ATTACH01", "LC_EA_M_ATTACH01", "LC_EA_M_Head", "LC_M_Head", "LC_EX_M_Head",
        "PX_L_HandIndex1", "PX_R_HandIndex1", "PX_L_HandMiddle1", "PX_R_HandMiddle1"};
    for (const char* n : kNames)
        if (bones::Crc32(n) == crc) return n;
    return nullptr;
}

}  // namespace b2s::rigmap
