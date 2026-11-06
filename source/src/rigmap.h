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
#include "bones_crc.h"
#include <cstdint>

namespace b2s::rigmap {

constexpr uint32_t kMax = 1024;
constexpr uint32_t kNone = kMax;

struct Map {
    uint32_t count = 0;
    bool named = false;                 // both hands have a named copy group (LC_<S>_Hand or LC_EX_<S>_Hand)
    uint16_t driver[kMax];              // a copy bone -> the bone it copies; kNone otherwise
    bool hide[kMax];                    // head, neck and face
    uint8_t roll[kMax];                 // EX_<S>_ForeArm_Roll<n>: n (1..3), 0 otherwise
    bool socket[kMax];                  // EA_*_ATTACH* item sockets
    uint32_t neck = kNone;              // M_Neck: where the hidden bones collapse
    uint32_t lcHand[2]{kNone, kNone};   // LC_L_Hand, LC_R_Hand (finger groups)
    uint32_t rolls[2][3]{{kNone, kNone, kNone}, {kNone, kNone, kNone}};  // [side][n-1], 0 = left
    uint32_t drivers = 0, hidden = 0, sockets = 0;
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
template <class Find>
void Build(Map& m, uint32_t count, const uint32_t* parents, Find find) {
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
    static const char* const kRoll[2][3] = {{"EX_L_ForeArm_Roll1", "EX_L_ForeArm_Roll2", "EX_L_ForeArm_Roll3"},
                                            {"EX_R_ForeArm_Roll1", "EX_R_ForeArm_Roll2", "EX_R_ForeArm_Roll3"}};
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 3; ++k) {
            const uint32_t r = bone(kRoll[s][k]);
            m.rolls[s][k] = r;
            if (r != kNone) m.roll[r] = uint8_t(k + 1);
        }
    static const char* const kSocket[] = {"EA_L_ATTACH01", "EA_L_ATTACH02", "EA_R_ATTACH01", "EA_R_ATTACH02",
                                          "EA_M_ATTACH", "EA_M_ATTACH01", "EA_M_ATTACH02"};
    for (const char* s : kSocket) {
        const uint32_t b = bone(s);
        if (b != kNone && !m.socket[b]) { m.socket[b] = true; ++m.sockets; }
    }
}

// Name of a well-known bone for log lines (nullptr when the hash is not one of them).
inline const char* KnownName(uint32_t crc) {
    static const char* const kNames[] = {
        "M_Hips", "M_Spine", "M_Spine1", "M_Spine2", "M_Neck", "M_Neck1", "M_Head", "L_Shoulder", "L_Arm",
        "L_ForeArm", "L_Hand", "R_Shoulder", "R_Arm", "R_ForeArm", "R_Hand", "LC_L_Hand", "LC_R_Hand",
        "LC_EX_L_Hand", "LC_EX_R_Hand", "LC_EX_L_ForeArm", "LC_EX_R_ForeArm", "LC_EX_L_Arm", "LC_EX_R_Arm",
        "LC_EX_M_Spine2", "LC_EA_M_Spine2", "EA_L_ATTACH01", "EA_L_ATTACH02", "EA_R_ATTACH01", "EA_R_ATTACH02",
        "EA_M_ATTACH", "EA_M_ATTACH01", "EX_L_ForeArm_Roll1", "EX_L_ForeArm_Roll2", "EX_L_ForeArm_Roll3",
        "EX_R_ForeArm_Roll1", "EX_R_ForeArm_Roll2", "EX_R_ForeArm_Roll3", "LC_M_Head", "LC_EX_M_Head",
        "PX_L_HandIndex1", "PX_R_HandIndex1", "PX_L_HandMiddle1", "PX_R_HandMiddle1"};
    for (const char* n : kNames)
        if (bones::Crc32(n) == crc) return n;
    return nullptr;
}

}  // namespace b2s::rigmap
