#include "player.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "bones.h"
#include "game.h"
#include "heapscan.h"
#include "log.h"

namespace b2s::player {
namespace {

constexpr uintptr_t kAnaelGameplayVt = 0x1417a9190;  // QDT::SINT::ANAEL_GAMEPLAY
constexpr uintptr_t kCharacterVt = 0x1417447a0;      // QDT::SINT::CHARACTER (primary vtable)
constexpr size_t kAnaelActive = 0xa8;                // byte: player controls Aiden (LIVE)
constexpr size_t kAnaelJodie = 0xb8;                 // KCOM handle -> Jodie's CHARACTER (LIVE)
constexpr size_t kCharPhysics = 0x508;               // HAVOK_CHARACTER* while controller attached
constexpr DWORD kRescanMs = 1000;  // owner: F1/F3 unavailable for the first ~15 s
// Jodie found by name only: look again now and then, an ANAEL_GAMEPLAY (Aiden control) may appear.
constexpr DWORD kNamedRescanMs = 10000;
constexpr int kMaxCharacters = 96;

const uint32_t kHeadHash = bones::Crc32("M_Head");

std::atomic<void*> g_anael{nullptr};
// LIVE 0.16.8 (owner, bald adult Jodie): scenes without Aiden have no ANAEL_GAMEPLAY at all
// ("0 ANAEL_GAMEPLAY candidates" for minutes), so F3 never found her. Fallback: her CHARACTER by name.
std::atomic<void*> g_named{nullptr};
std::atomic<bool> g_scanning{false};
DWORD g_lastScan = 0;
void* g_loggedJodie = nullptr;
Control g_loggedControl = Control::Unknown;
DWORD g_lastNamesLog = 0;

// KCOM handle -> cached object (handle+0x20 bit29, object at +0x30). Uncached handles are not
// resolved: the resolver has side effects.
void* CachedObject(void* handle) {
    __try {
        if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000) return nullptr;
        const uint32_t flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(handle) + 0x20);
        if (((flags >> 29) & 1) == 0) return nullptr;
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(handle) + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

template <typename T>
bool Field(void* obj, size_t off, T& out) {
    __try {
        if (!obj) return false;
        std::memcpy(&out, static_cast<uint8_t*>(obj) + off, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A live KCOM object: expected vtable, and its own handle (+0x20) points back at it. A freed copy
// that still holds the vtable fails the back-pointer check once the handle is reused.
bool IsLiveObject(void* obj, uintptr_t vtStaticVa) {
    uintptr_t vt = 0;
    void* handle = nullptr;
    if (!Field(obj, 0, vt) || vt != game::Va(vtStaticVa)) return false;
    if (!Field(obj, 0x20, handle)) return false;
    return CachedObject(handle) == obj;
}

// LIVE 0401: "_0401_JODIE_AND_ANAEL_Jodie" (names via game::ObjectName).
using game::ObjectName;

// Character names are "<chapter>_<who>"; chapter parts are upper case ("JODIE_AND_ANAEL"), so the
// case-sensitive "Jodie" is her own part. Her own name part exactly ("..._Jodie") ranks first, then
// a name ending in "Jodie" (LIVE 0.16.12: "_0401_JODIE_AND_ANAEL_AuraJodie" was picked over her),
// then "Jodie..." variants.
int JodieNameScore(const char* name) {
    const char* p = std::strstr(name, "Jodie");
    if (!p) return 0;
    const size_t n = std::strlen(name);
    if (n >= 6 && std::strcmp(name + n - 6, "_Jodie") == 0) return 3;
    const char* last = p;
    while (const char* next = std::strstr(last + 1, "Jodie")) last = next;
    return std::strcmp(last, "Jodie") == 0 ? 2 : 1;
}

DWORD WINAPI ScanThread(LPVOID) {
    static void* hits[2 * kMaxCharacters];
    const uintptr_t vts[2] = {kAnaelGameplayVt, kCharacterVt};
    int counts[2] = {};
    size_t mb = 0;
    heapscan::FindByVtables(vts, 2, hits, kMaxCharacters, counts, &mb);
    // Prefer a live object that already has Jodie (LIVE 0.15.5: right after a load a live
    // ANAEL_GAMEPLAY without her was taken and kept for 30 s, so F3 could not find Jodie).
    void* live = nullptr;
    bool withJodie = false;
    for (int i = 0; i < counts[0] && !withJodie; ++i) {
        if (!IsLiveObject(hits[i], kAnaelGameplayVt)) continue;
        void* handle = nullptr;
        const bool has = Field(hits[i], kAnaelJodie, handle) && CachedObject(handle);
        if (!live || has) {
            live = hits[i];
            withJodie = has;
        }
    }
    g_anael = live;
    // Without Aiden's link: the live character named Jodie (physics-driven first, then one with a head).
    void* named = nullptr;
    char namedName[96] = "";
    int best = 0, characters = 0, jodies = 0;
    if (!withJodie) {
        for (int i = 0; i < counts[1]; ++i) {
            void* c = hits[kMaxCharacters + i];
            if (!IsLiveObject(c, kCharacterVt)) continue;
            ++characters;
            char name[96];
            if (!ObjectName(c, name, sizeof(name))) continue;
            const int byName = JodieNameScore(name);
            if (!byName) continue;
            ++jodies;
            void* physics = nullptr;
            float h[16];
            const int score = byName * 4 + (Field(c, kCharPhysics, physics) && physics ? 2 : 0) + (HeadWorld(c, h) ? 1 : 0);
            if (score > best) {
                best = score;
                named = c;
                std::memcpy(namedName, name, sizeof(name));
            }
        }
    }
    g_named = named;
    Log("PLAYER scan: %d ANAEL_GAMEPLAY candidates in %zu MB, live=%p%s; %d characters, %d named Jodie%s%s",
        counts[0], mb, live, live && !withJodie ? " (no Jodie yet)" : "", characters, jodies,
        named ? " -> " : "", namedName);
    // No Jodie anywhere: list the names (at most every 30 s) so the next miss says what to match.
    if (!withJodie && !named && characters) {
        const DWORD now = GetTickCount();
        if (!g_lastNamesLog || now - g_lastNamesLog >= 30000) {
            g_lastNamesLog = now;
            for (int i = 0, shown = 0; i < counts[1] && shown < 24; ++i) {
                void* c = hits[kMaxCharacters + i];
                char name[96];
                if (IsLiveObject(c, kCharacterVt) && ObjectName(c, name, sizeof(name))) {
                    Log("PLAYER   character %p %s", c, name);
                    ++shown;
                }
            }
        }
    }
    g_scanning = false;
    return 0;
}

void StartScan(DWORD minGapMs) {
    const DWORD now = GetTickCount();
    if (g_scanning || now - g_lastScan < minGapMs) return;
    g_lastScan = now;
    g_scanning = true;
    if (HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr)) CloseHandle(t);
    else g_scanning = false;
}

}  // namespace

State Poll() {
    State s;
    void* anael = g_anael.load();
    if (anael && !IsLiveObject(anael, kAnaelGameplayVt)) {
        g_anael = nullptr;
        anael = nullptr;
    }
    void* named = g_named.load();
    if (named && !IsLiveObject(named, kCharacterVt)) {
        g_named = nullptr;
        named = nullptr;
    }
    void* handle = nullptr;
    void* fromAnael = anael && Field(anael, kAnaelJodie, handle) ? CachedObject(handle) : nullptr;
    s.jodie = fromAnael ? fromAnael : named;
    // Keep looking while Aiden's link to her is missing: every second with no Jodie at all, every
    // 10 s while she is only known by name.
    if (!fromAnael) StartScan(s.jodie ? kNamedRescanMs : kRescanMs);
    uint8_t aiden = 0;
    void* physics = nullptr;
    if (anael) Field(anael, kAnaelActive, aiden);
    if (aiden) s.control = Control::Aiden;
    else if (s.jodie && Field(s.jodie, kCharPhysics, physics)) s.control = physics ? Control::Walking : Control::Cutscene;
    if (s.jodie != g_loggedJodie || s.control != g_loggedControl) {
        g_loggedJodie = s.jodie;
        g_loggedControl = s.control;
        float h[16];
        const bool head = HeadWorld(s.jodie, h);
        Log("PLAYER anael=%p jodie=%p (%s) control=%s head=%s", anael, s.jodie,
            fromAnael ? "Aiden's link" : s.jodie ? "by name" : "none", Name(s.control), head ? "found" : "missing");
    }
    return s;
}

bool HeadWorld(void* jodie, float out[16]) {
    if (!jodie) return false;
    __try {  // engine reads on a possibly mid-teardown character
        const float* m = bones::BoneWorld(bones::EntityNode(jodie), kHeadHash);
        if (!m) return false;
        std::memcpy(out, m, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BodyWorld(void* jodie, float out[16]) {
    if (!jodie) return false;
    __try {
        void* node = bones::EntityNode(jodie);
        if (!node) return false;
        // As the engine does before reading +0x80 (FUN_14126d1b0): refresh a stale world matrix.
        if (((*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(node) + 0x120) >> 1) & 1) == 0)
            reinterpret_cast<void(__fastcall*)(void*)>(game::Addr(game::Anchor::NodeUpdateWorld))(node);
        std::memcpy(out, static_cast<uint8_t*>(node) + 0x80, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* Name(Control c) {
    switch (c) {
        case Control::Walking: return "walking";
        case Control::Cutscene: return "cutscene";
        case Control::Aiden: return "aiden";
        default: return "unknown";
    }
}

}  // namespace b2s::player
