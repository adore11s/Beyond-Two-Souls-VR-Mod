// EXP-056 desk tests for the game's rig convention (src/rigmap.h), on a reduced copy of adult Jodie's 255-bone
// rig (names and parents as in b2svr_logs/skeleton_255_adult.csv).
#include "../src/rigmap.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// EXP-063: a whole rig from the game files with the picture tool's results (tools/rig_fixture.py).
#include "rigmap_fixture.inc"

using namespace b2s;
namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}
struct Bone { const char* name; int parent; };
// Index order and parents follow the dump where it matters (copy groups under M_Hips, helpers under them).
const Bone kRig[] = {
    {"M_Hips", -1},           // 0
    {"M_Spine", 0},           // 1
    {"M_Spine2", 1},          // 2
    {"R_Shoulder", 2},        // 3
    {"R_Arm", 3},             // 4
    {"R_ForeArm", 4},         // 5
    {"R_Hand", 5},            // 6
    {"L_Shoulder", 2},        // 7
    {"L_Arm", 7},             // 8
    {"L_ForeArm", 8},         // 9
    {"L_Hand", 9},            // 10
    {"M_Neck", 2},            // 11
    {"M_Neck1", 11},          // 12
    {"M_Head", 12},           // 13
    {"LC_M_Head", 0},         // 14
    {"FA_M_FacialGRP", 14},   // 15
    {"FA_L_Eye", 15},         // 16
    {"LC_EX_M_GRP", 0},       // 17
    {"LC_EX_M_Spine2", 17},   // 18
    {"EX_L_UpShoulder", 18},  // 19  near the neck: must stay drawn
    {"LC_EX_R_Shoulder", 17}, // 20
    {"EX_R_BackArm", 20},     // 21  near the neck: must stay drawn
    {"LC_EX_R_ForeArm", 17},  // 22
    {"EX_R_ForeArm_Roll1", 22},  // 23
    {"EX_R_ForeArm_Roll2", 22},  // 24
    {"EX_R_ForeArm_Roll3", 22},  // 25
    {"LC_EX_R_Hand", 17},     // 26
    {"EX_R_UpWrist", 26},     // 27
    {"LC_R_Hand", 0},         // 28
    {"PX_R_HandIndex1", 28},  // 29
    {"EA_R_ATTACH01", 28},    // 30
    {"LC_L_Hand", 0},         // 31
    {"PX_L_HandIndex1", 31},  // 32
    {"EA_L_ATTACH01", 31},    // 33
    {"LC_EX_M_Head", 17},     // 34
    {"EX_L_LidIntC_eyeBlink", 34},  // 35
    {"EX_L_ForeArm_Roll1", 17},     // 36 (left forearm group missing in this reduced rig: the roll still counts)
};
constexpr uint32_t kCount = sizeof(kRig) / sizeof(kRig[0]);
uint32_t g_crc[kCount];
uint32_t Find(uint32_t hash) {
    for (uint32_t i = 0; i < kCount; ++i)
        if (g_crc[i] == hash) return i;
    return rigmap::kNone;
}
uint32_t Index(const char* name) {
    for (uint32_t i = 0; i < kCount; ++i)
        if (std::strcmp(kRig[i].name, name) == 0) return i;
    return rigmap::kNone;
}
}  // namespace

int main() {
    uint32_t parents[kCount];
    for (uint32_t i = 0; i < kCount; ++i) {
        g_crc[i] = bones::Crc32(kRig[i].name);
        parents[i] = kRig[i].parent < 0 ? kCount : uint32_t(kRig[i].parent);
    }
    static rigmap::Map m;
    rigmap::Build(m, kCount, parents, Find);
    Check(m.named, "both hands have named copy groups");
    Check(m.driver[Index("LC_R_Hand")] == Index("R_Hand"), "LC_R_Hand copies R_Hand");
    Check(m.driver[Index("LC_L_Hand")] == Index("L_Hand"), "LC_L_Hand copies L_Hand");
    Check(m.driver[Index("LC_EX_R_ForeArm")] == Index("R_ForeArm"), "LC_EX_R_ForeArm copies R_ForeArm");
    Check(m.driver[Index("LC_EX_R_Hand")] == Index("R_Hand"), "LC_EX_R_Hand copies R_Hand");
    Check(m.driver[Index("LC_EX_M_Spine2")] == Index("M_Spine2"), "LC_EX_M_Spine2 copies M_Spine2");
    Check(m.driver[Index("LC_EX_R_Shoulder")] == Index("R_Shoulder"), "LC_EX_R_Shoulder copies R_Shoulder");
    Check(m.driver[Index("LC_M_Head")] == Index("M_Head"), "LC_M_Head copies M_Head");
    Check(m.driver[Index("LC_EX_M_GRP")] == rigmap::kNone, "a group root copies nothing");
    Check(m.driver[Index("EX_R_UpWrist")] == rigmap::kNone, "a helper is a child, not a copy");
    Check(m.drivers == 8, "eight copy bones");
    // Head set: M_Neck subtree, LC_M_Head subtree, LC_EX_M_Head subtree; nothing else.
    for (const char* n : {"M_Neck", "M_Neck1", "M_Head", "LC_M_Head", "FA_M_FacialGRP", "FA_L_Eye", "LC_EX_M_Head",
                          "EX_L_LidIntC_eyeBlink"})
        Check(m.hide[Index(n)], n);
    for (const char* n : {"EX_L_UpShoulder", "LC_EX_R_Shoulder", "EX_R_BackArm", "M_Spine2", "R_Shoulder", "L_Arm",
                          "LC_R_Hand", "EA_R_ATTACH01", "LC_EX_M_GRP"})
        Check(!m.hide[Index(n)], n);
    Check(m.hidden == 8, "eight head bones");
    Check(m.neck == Index("M_Neck"), "collapse at M_Neck");
    Check(m.rolls[1][0] == Index("EX_R_ForeArm_Roll1") && m.rolls[1][1] == Index("EX_R_ForeArm_Roll2") &&
              m.rolls[1][2] == Index("EX_R_ForeArm_Roll3"),
          "right forearm roll helpers in order");
    Check(m.roll[Index("EX_R_ForeArm_Roll2")] == 2, "roll index 2");
    Check(m.rolls[0][0] == Index("EX_L_ForeArm_Roll1") && m.rolls[0][1] == rigmap::kNone, "left: one roll found");
    Check(m.socket[Index("EA_R_ATTACH01")] && m.socket[Index("EA_L_ATTACH01")] && m.sockets == 2, "item sockets");
    Check(m.lcHand[0] == Index("LC_L_Hand") && m.lcHand[1] == Index("LC_R_Hand"), "finger groups");
    // A rig without copy groups (other naming): not named, nothing driven, head from M_Neck only.
    {
        const char* plain[] = {"M_Hips", "M_Spine2", "M_Neck", "M_Head", "L_Hand", "R_Hand"};
        uint32_t pp[6] = {6, 0, 1, 2, 1, 1};
        uint32_t crc[6];
        for (int i = 0; i < 6; ++i) crc[i] = bones::Crc32(plain[i]);
        static rigmap::Map p;
        rigmap::Build(p, 6, pp, [&](uint32_t h) {
            for (uint32_t i = 0; i < 6; ++i)
                if (crc[i] == h) return i;
            return rigmap::kNone;
        });
        Check(!p.named && p.drivers == 0, "plain rig is not named");
        Check(p.hide[2] && p.hide[3] && !p.hide[1] && p.hidden == 2, "plain rig head = M_Neck subtree");
    }
    // EXP-063: the real rig (IW_JHOB, 253 bones). The C++ rules and the picture tool's port must agree bone for
    // bone: copy bones by name and by rest place, the head set with the head-mounted socket, the roll shares.
    {
        static rigmap::Map f;
        rigmap::Build(f, kFxCount, kFxParents, [](uint32_t h) {
            for (uint32_t i = 0; i < kFxCount; ++i)
                if (kFxCrc[i] == h) return i;
            return rigmap::kNone;
        }, kFxFlags, kFxRest);
        int hideDiff = 0, driverDiff = 0;
        for (uint32_t i = 0; i < kFxCount; ++i) {
            hideDiff += f.hide[i] != kFxHide[i];
            driverDiff += uint32_t(f.driver[i]) != kFxDriver[i];
        }
        Check(hideDiff == 0, "real rig: head set equals the picture tool's");
        Check(driverDiff == 0, "real rig: copy drivers equal the picture tool's");
        Check(f.restDrivers == kFxRestDrivers && kFxRestDrivers == 3, "real rig: three copies matched by rest place");
        Check(f.hiddenCopies == kFxHiddenCopies && kFxHiddenCopies == 2, "real rig: head socket copy and socket hidden");
        auto fx = [](const char* n) {
            const uint32_t h = bones::Crc32(n);
            for (uint32_t i = 0; i < kFxCount; ++i)
                if (kFxCrc[i] == h) return i;
            return rigmap::kNone;
        };
        Check(f.driver[fx("LC_EA_L_ATTACH01")] == fx("L_Hand") && f.driver[fx("LC_EA_R_ATTACH01")] == fx("R_Hand"),
              "real rig: the item sockets' parents follow the hands");
        Check(f.hide[fx("EA_M_ATTACH01")] && !f.hide[fx("EA_L_ATTACH01")] && !f.hide[fx("EA_R_ATTACH01")],
              "real rig: the head socket is hidden, the hand sockets are not");
        Check(f.driver[fx("LC_EX_M_GRP")] == rigmap::kNone && f.driver[fx("LC_EA_M_GRP")] == rigmap::kNone,
              "real rig: group roots copy nothing");
        const uint32_t found = rigmap::RollShares(f, kFxRest);
        Check(found == (4u | 4u << 8), "real rig: four roll helpers per forearm");
        float worst = 0.f;
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < rigmap::kMaxRolls; ++k) worst = std::fmax(worst, std::fabs(f.rollShare[s][k] - kFxShare[s][k]));
        Check(worst < 1e-4f, "real rig: roll shares equal the picture tool's");
        // Roll1 is the helper nearest the wrist: the twist share falls with the number, and the last is near the elbow.
        Check(f.rollShare[1][0] > f.rollShare[1][1] && f.rollShare[1][1] > f.rollShare[1][2] &&
                  f.rollShare[1][2] > f.rollShare[1][3] && f.rollShare[1][0] > 0.6f && f.rollShare[1][3] < 0.3f,
              "real rig: Roll1 takes the most wrist twist, Roll4 the least");
        // Young Jodie's distance rule: helpers below a copy of the spine / shoulders / arms leave a head set made by
        // another rule; head bones and bones under no copy stay as that rule decided.
        {
            static bool other[kFxCount], notHead[kFxCount];
            for (uint32_t i = 0; i < kFxCount; ++i) other[i] = f.hide[i];
            const uint32_t up = fx("EX_L_UpShoulder"), back = fx("EX_R_BackArm"), lcs = fx("LC_EX_L_Shoulder");
            const uint32_t hips = fx("M_Hips");
            other[up] = other[back] = other[lcs] = other[hips] = true;
            const uint32_t n = rigmap::ReleaseNonHead(f, kFxParents, other, notHead);
            Check(n == 3 && !other[up] && !other[back] && !other[lcs], "distance rule: shoulder / arm helpers released");
            Check(other[hips] && !notHead[hips], "distance rule: a bone under no copy bone is left alone");
            Check(other[fx("FA_L_Eye")] && other[fx("M_Head")] && !notHead[fx("FA_L_Eye")], "distance rule: head stays hidden");
            Check(notHead[fx("EX_L_ForeArm_Roll1")], "arm helpers are marked as never head");
        }
        // Without the class words / rest pose the name rules still work and nothing is matched by place.
        static rigmap::Map g;
        rigmap::Build(g, kFxCount, kFxParents, [](uint32_t h) {
            for (uint32_t i = 0; i < kFxCount; ++i)
                if (kFxCrc[i] == h) return i;
            return rigmap::kNone;
        });
        Check(g.restDrivers == 0 && g.hiddenCopies == 0 && g.named, "real rig: names only when no rest pose is given");
        // A rest pose that contradicts the named copies (all bones at the origin but the hands' copies moved) is not
        // trusted.
        static float bad[kFxCount * 16];
        for (uint32_t i = 0; i < kFxCount * 16; ++i) bad[i] = kFxRest[i];
        for (uint32_t i = 0; i < kFxCount; ++i)
            if (kFxFlags[i] & rigmap::kFlagCopy) bad[i * 16 + 12] += 0.5f;
        static rigmap::Map b;
        rigmap::Build(b, kFxCount, kFxParents, [](uint32_t h) {
            for (uint32_t i = 0; i < kFxCount; ++i)
                if (kFxCrc[i] == h) return i;
            return rigmap::kNone;
        }, kFxFlags, bad);
        Check(b.restDrivers == 0, "real rig: an inconsistent rest pose matches nothing");
    }
    // Name lookups for log lines.
    Check(rigmap::KnownName(bones::Crc32("EA_R_ATTACH01")) != nullptr, "socket name known");
    Check(rigmap::KnownName(0x12345678u) == nullptr, "unknown hash");
    if (failures) {
        std::printf("rigmap_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("rigmap_tests: all passed\n");
    return 0;
}
