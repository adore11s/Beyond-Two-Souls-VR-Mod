#pragma once
// Which body the first-person rig is: one of Jodie's model families, or someone else (a body Aiden possesses).
//
// STATIC (EXP-046, the game's own resource tables BigFile_PC*.dbg): every Jodie model is a package
// "IW_JHO<age>_<outfit>" (JHO = Jodie Holmes, package class 0x12d6). 76 model packages in four age families:
//   JHO9  (13: EXT1, INT1, INT1BS, INT2..INT6, PAJ1, PAJ1RIP, PAJ2, PAJ2RIP, PAJ3)   young (small) Jodie
//   JHO14 (2: base, BS)                                                                 the party, 14
//   JHO16 (19: ABDO, BAR, BAR2, BARBS, CON_EXT1/EXT2/INT1, DLC_01, JOG*, KRAV*, ...)   teenager, 16
//   JHO20 (42: PRO, EMB, DIN*, HUNTINT01, SOM*, HOM*, HOSP*, COW_*, SEN, CHI*, ...)    adult
//   plus IW_JHOB and IW_JHOWITHBIKE (adult extras). Animation sets name the same ages (MK_JODIE_09_*, MK_JODIE20_*).
// Which chapter file holds which family gives the scene table below: every scene of a chapter file loads only one
// family, except the Black Sun file (2501-2617), which holds all four (memories): there the arm length decides.
//
// The young-Jodie rules (owner 2026-10-13: "these changes shouldn't affect the small Jodie model") are kept for the
// JHO9 family only; every other body uses the general (adult) rules, which scale with the rig's own arm length.
#include <cstdlib>
#include <cstring>

namespace b2s::bodykind {

enum class Family { Unknown, Child, Teen, Adult, Other };

inline const char* Name(Family f) {
    switch (f) {
        case Family::Child: return "young Jodie (JHO9)";
        case Family::Teen: return "teenage Jodie (JHO14 / JHO16)";
        case Family::Adult: return "adult Jodie (JHO20)";
        case Family::Other: return "another body (possessed)";
        default: return "unknown";
    }
}

inline Family FromAge(int age) {
    if (age >= 5 && age <= 12) return Family::Child;
    if (age >= 13 && age <= 17) return Family::Teen;
    if (age >= 18 && age <= 40) return Family::Adult;
    return Family::Unknown;
}

// A model / resource name holding "JHO<age>" (IW_JHO9_INT2, IW_JHO20_PRO, ...). "JHOB" / "JHOWITHBIKE" are adult.
inline Family FromModelName(const char* name) {
    if (!name) return Family::Unknown;
    for (const char* p = std::strstr(name, "JHO"); p; p = std::strstr(p + 1, "JHO")) {
        const char* q = p + 3;
        if (*q >= '0' && *q <= '9') {
            int age = 0;
            for (int k = 0; k < 3 && *q >= '0' && *q <= '9'; ++k, ++q) age = age * 10 + (*q - '0');
            // "JHO2020" (a shared material set) reads as 20: the first two digits are the age.
            if (age > 99) age /= 10;
            const Family f = FromAge(age);
            if (f != Family::Unknown) return f;
        } else if (std::strncmp(q, "WITHBIKE", 8) == 0 || (q[0] == 'B' && (q[1] == '_' || q[1] == 0))) {
            return Family::Adult;
        }
    }
    return Family::Unknown;
}

// Scene number of a script object's name: "_0102_PROLOGUE_ARRESTED_Jodie" -> 102, "_0401_JODIE_AND_ANAEL_Jodie"
// -> 401; -1 when the name does not start with "_dddd_".
inline int SceneOf(const char* name) {
    if (!name || name[0] != '_') return -1;
    int scene = 0;
    for (int k = 1; k <= 4; ++k) {
        if (name[k] < '0' || name[k] > '9') return -1;
        scene = scene * 10 + (name[k] - '0');
    }
    return name[5] == '_' ? scene : -1;
}

// The Jodie family of a scene (chapter files of the analysed build, see the header comment).
inline Family FromScene(int scene) {
    if (scene < 0) return Family::Unknown;
    if (scene >= 101 && scene <= 102) return Family::Adult;   // BigFile_PC_0101_0102: JHO20_PRO, JHO20_UND_NAVV1
    if (scene >= 401 && scene <= 705) return Family::Child;   // 0401, 0501_0503, 0601, 0701, 0703, 0704, 0705: JHO9 only
    if (scene >= 801 && scene <= 802) return Family::Teen;    // 0801_0802: JHO14
    if (scene >= 901 && scene <= 1209) return Family::Teen;   // 0901_0903, 1001_1002, 1101_1102, 1201_1209: JHO16
    if (scene >= 1401 && scene <= 2408) return Family::Adult; // 1401 .. 2401_2408: JHO20 (and JHOWITHBIKE)
    if (scene >= 4000 && scene <= 4099) return Family::Teen;  // 4000 (the DLC): JHO16_DLC_01
    return Family::Unknown;                                    // 2501_2617 holds every family; anything else unknown
}

// The young-Jodie rules apply: her family is known to be JHO9, or unknown and the rig's arms are a child's
// (shoulder -> elbow -> wrist up to 0.42 m; LIVE young Jodie 0.34 m, adult 0.48 m, the 0.16.41 rule).
inline bool ChildRules(Family f, float armLength) {
    if (f == Family::Child) return true;
    if (f == Family::Unknown) return armLength <= 0.42f;
    return false;
}

}  // namespace b2s::bodykind
