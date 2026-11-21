#include "player.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "bodykind.h"
#include "crashreport.h"
#include "ctrlprobe.h"
#include "menuwatch.h"
#include "bones.h"
#include "game.h"
#include "heapscan.h"
#include "log.h"

namespace b2s::player {
namespace {

constexpr uintptr_t kAnaelGameplayVt = 0x1417a9190;  // QDT::SINT::ANAEL_GAMEPLAY
constexpr uintptr_t kCharacterVt = 0x1417447a0;      // QDT::SINT::CHARACTER (primary vtable)
constexpr size_t kAnaelActive = 0xa8;                // byte: player controls Aiden (LIVE)
constexpr size_t kAnaelJodie = 0xb8;                 // KCOM handle -> Jodie's CHARACTER (LIVE; script "LinkedCharacter")
constexpr size_t kAnaelAiden = 0xc0;                 // KCOM handle -> Aiden's CHARACTER (LIVE; script "Character")
constexpr size_t kCharPhysics = 0x508;               // HAVOK_CHARACTER* while controller attached
// EXP-057 (owner 2026-11-04: marked F1 / F3 mistakes in 2001 Navajo; "decompiled engine state, never guesses"):
// CHARACTER+0x340 motion kit (GMK, the locomotion the movement stick drives; +0x40 byte 1 = stopped, EXP-055) and
// CHARACTER+0x348 animation player: table +0x18, 16 entries of 0x18 bytes (+8 animation, +0x10 layer), count +0x180.
// STATIC: every authored animation reaches that table through SCRIPT_ENTITY_ABC::vfunc_161 / vfunc_165
// (FUN_14083d4f0 -> FUN_140981a40 -> FUN_140981930): sequence actions (PLAY_ANIM_ACTION_INSTANCE::vfunc_04), the
// script controller (SCRIPT_ENTITY_CONTROLLER_PLAY_ANIM::vfunc_125), cinematics (PLAYABLE_ANIMO::vfunc_105) and the
// script method PlayAnim (FUN_140dd1fa0 -> FUN_140ddd370: request+0x40 layer, +0x70 = "layer == 0"; FUN_140efc4c0
// marks layer < 1 as the base layer). Locomotion is not in it: LIVE 0.22.0 the table was empty through every walk.
constexpr size_t kCharMotionKit = 0x340;
constexpr size_t kCharAnimPlayer = 0x348;
// EXP-059 (STATIC, see State::synced): motion kit +0x50 = its motion graph; graph +0x788 = the graph it is synced to
// (SyncMotionKit), +0x6c0 = its owner's SCRIPT_ENTITY_ABC sub-object, which sits at CHARACTER+0x128.
constexpr size_t kMotionKitGraph = 0x50;
constexpr size_t kGraphSyncedTo = 0x788;
constexpr size_t kGraphOwnerSub = 0x6c0;
constexpr size_t kCharSyncSub = 0x128;
// EXP-064 (STATIC FUN_140a1afa0): a master graph's +0x780 = its latest follower graph, a follower's +0x780 = the next.
constexpr size_t kGraphFollowers = 0x780;
// CHARACTER +0x170 = its skeleton (STATIC: HIERARCHY_MAPPING update FUN_1411dd920 reads *(handle+0x30)+0x170; the same
// object carries the motion kit at +0x340, FUN_140dce550).
constexpr size_t kCharSkeleton = 0x170;
// Between two authored base-layer animations the table can be empty for a moment (LIVE 0.22.0: 1.0 s at 233 s and 60 ms
// at 476 s, both inside cutscenes). Without a physics controller the motion kit only counts as the player's walk once
// no base-layer animation has played for this long; with the physics controller it counts at once.
constexpr ULONGLONG kHandOverMs = 1500;
// EXP-047 (STATIC): POSSESSION_CENTRE_CONTROLLER, one per body Aiden can possess (see player.h).
constexpr uintptr_t kPccVt = 0x14178b5d0;            // QDT::SINT::POSSESSION_CENTRE_CONTROLLER (primary vtable)
constexpr uint64_t kPccUpdateVa = 0x14110c4f0;       // its vfunc_124: the per-frame controller update (this, dt)
constexpr size_t kPccCharacter = 0x108;              // KCOM handle: script "Character", the body it possesses
constexpr size_t kPccState = 0x130;                  // int32: -1 new, 0 possessed, 1 ending, 2 ended
constexpr DWORD kPccSeenMs = 500;                    // a possession counts while its controller updated this recently
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
// The same back-pointer check without a class (a possessed body: a CHARACTER, or another script entity).
bool IsLiveHandleObject(void* obj) {
    void* handle = nullptr;
    return obj && Field(obj, 0x20, handle) && CachedObject(handle) == obj;
}

// ---- possessions (EXP-047) ----
std::atomic<bool> g_possessionFP{false};  // owner 2026-10-28: off by default
using PccUpdateFn = void (*)(void*, float, uintptr_t, uintptr_t);
PccUpdateFn g_pccUpdate = nullptr;
// The controller whose state was 0 (possessed) at its last update, and when (game thread writes, Poll reads).
std::atomic<void*> g_possessedPcc{nullptr};
std::atomic<DWORD> g_possessedAt{0};

void LogPccState(void* pcc, int32_t state);

// POSSESSION_CENTRE_CONTROLLER::vfunc_124 (this, dt in xmm1): the engine's controller loop (FUN_140b391a0) passes
// only rcx and xmm1; two integer registers are passed through untouched.
void HookPccUpdate(void* pcc, float dt, uintptr_t a3, uintptr_t a4) {
    g_pccUpdate(pcc, dt, a3, a4);
    int32_t state = -1;
    if (!Field(pcc, kPccState, state)) return;
    if (state == 0) {
        g_possessedPcc.store(pcc, std::memory_order_relaxed);
        g_possessedAt.store(GetTickCount(), std::memory_order_relaxed);
    }
    LogPccState(pcc, state);
}

bool HandleName(void* handle, char* out, size_t cap) {
    out[0] = 0;
    __try {
        if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000) return false;
        const char* const* holder = *reinterpret_cast<const char* const* const*>(static_cast<uint8_t*>(handle) + 0x18);
        if (!holder || !*holder) return false;
        size_t i = 0;
        for (const char* s = *holder; i + 1 < cap && s[i]; ++i) out[i] = s[i];
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

void* ModelOf(void* entity) {
    __try {
        return bones::EntityModel(entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The body Aiden possesses: the "Character" of the controller that was in state 0 at an update within kPccSeenMs,
// still live and still possessed, and not Jodie herself.
void* PossessedBody(void* jodie, void** pccOut) {
    void* pcc = g_possessedPcc.load(std::memory_order_relaxed);
    if (!pcc || GetTickCount() - g_possessedAt.load(std::memory_order_relaxed) > kPccSeenMs) return nullptr;
    int32_t state = -1;
    void* handle = nullptr;
    if (!IsLiveObject(pcc, kPccVt) || !Field(pcc, kPccState, state) || state != 0 || !Field(pcc, kPccCharacter, handle))
        return nullptr;
    void* c = CachedObject(handle);
    if (!c || c == jodie || !IsLiveHandleObject(c)) return nullptr;
    if (pccOut) *pccOut = pcc;
    return c;
}

// Every possession controller's state: when first seen and on every change (game thread, from the update hook).
void LogPccState(void* pcc, int32_t state) {
    struct Seen {
        void* pcc;
        int32_t state;
    };
    static Seen logged[64];
    static int nLogged = 0, next = 0, sightings = 0;
    Seen* known = nullptr;
    for (int i = 0; i < nLogged && !known; ++i)
        if (logged[i].pcc == pcc) known = &logged[i];
    if (known && known->state == state) return;
    const bool first = !known;
    if (first) {
        known = &logged[next];
        next = (next + 1) % 64;
        if (nLogged < 64) ++nLogged;
    }
    const int32_t was = known->state;
    *known = {pcc, state};
    // Every change; first sightings (one per possessable body in a scene) up to 200 lines a run.
    if (first && ++sightings > 200) return;
    void* handle = nullptr;
    void* body = Field(pcc, kPccCharacter, handle) ? CachedObject(handle) : nullptr;
    char name[96] = "";
    if (body) game::ObjectName(body, name, sizeof(name));
    if (first)
        Log("PLAYER possession centre %p: body %p '%s', state %d (-1 new, 0 possessed, 1 ending, 2 ended)", pcc, body,
            name, int(state));
    else
        Log("PLAYER possession centre %p: body %p '%s', state %d -> %d", pcc, body, name, int(was), int(state));
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

// EXP-057: who drives her body, from the engine's own state (see kCharAnimPlayer).
//   an authored animation on the base layer (layer 0) in her animation player -> the game animates her: cutscene rules
//   no such animation, her physics character controller attached               -> the player walks her
//   no such animation, no physics controller, her motion kit running           -> the player walks her (the walks after
//                                                                                 a prompt: 2001 Navajo 354-371 s and
//                                                                                 731-744 s, stick 1.00, 1.6-2.9 m / 5 s)
//   otherwise (no motion kit, or it is stopped)                                 -> cutscene rules
// Unreadable table: the physics controller alone decides (the rule before 0.23.0).
struct Driving {
    std::atomic<void*> who{nullptr};
    std::atomic<ULONGLONG> lastAuthored{0};
    std::atomic<const char*> why{""};
};
Driving g_driving;
std::mutex g_lastMutex;
State g_last;

Control EngineControl(void* character) {
    void* physics = nullptr;
    Field(character, kCharPhysics, physics);
    void* player = nullptr;
    void* table = nullptr;
    uint32_t count = 0;
    bool tableRead = Field(character, kCharAnimPlayer, player) && player && Field(player, 0x18, table) && table &&
                     Field(table, 0x180, count) && count <= 16;
    uint32_t base = 0;
    for (uint32_t i = 0; tableRead && i < count; ++i) {
        uint32_t layer = 0;
        void* anim = nullptr;
        if (!Field(table, 0x10 + i * 0x18, layer) || !Field(table, 0x8 + i * 0x18, anim)) {
            tableRead = false;
            break;
        }
        if (anim && layer == 0) ++base;
    }
    void* kit = nullptr;
    uint8_t stopped = 1;
    const bool kitRuns = Field(character, kCharMotionKit, kit) && kit && Field(kit, 0x40, stopped) && stopped == 0;
    const ULONGLONG now = GetTickCount64();
    if (g_driving.who.exchange(character, std::memory_order_relaxed) != character) g_driving.lastAuthored = 0;
    if (!tableRead) {
        g_driving.why = physics ? "physics controller (animation table unreadable)" : "no physics controller (animation table unreadable)";
        return physics ? Control::Walking : Control::Cutscene;
    }
    if (base) {
        g_driving.lastAuthored.store(now, std::memory_order_relaxed);
        g_driving.why = physics ? "authored base-layer animation, physics controller attached" : "authored base-layer animation";
        return Control::Cutscene;
    }
    if (physics) {
        g_driving.why = "physics controller, no authored base-layer animation";
        return Control::Walking;
    }
    if (!kitRuns) {
        g_driving.why = kit ? "motion kit stopped, no physics controller" : "no motion kit, no physics controller";
        return Control::Cutscene;
    }
    const ULONGLONG last = g_driving.lastAuthored.load(std::memory_order_relaxed);
    if (last && now - last < kHandOverMs) {
        g_driving.why = "base-layer animation just ended (hand-over)";
        return Control::Cutscene;
    }
    g_driving.why = "motion kit, no authored base-layer animation, no physics controller";
    return Control::Walking;
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
    if (anael) Field(anael, kAnaelActive, aiden);
    if (aiden) s.control = Control::Aiden;
    else if (s.jodie) s.control = EngineControl(s.jodie);
    // The main menu (owner 2026-10-28: no first person there). The game has no C++ main-menu flag (its menus are
    // scripts; the only menu string in the exe is the in-game menu's UpdateInGameMenu_); the main menu is the scene
    // MENU_GAME (LIVE: "CallOnDestroy sur MENU_GAME" / AREA_MENU when a game starts) with its own Jodie, and the
    // engine names characters "<scene>_<who>" (LIVE: "MENU_GAME_Jodie", "_0401_JODIE_AND_ANAEL_Jodie"). A character
    // named "MENU_..." belongs to a menu scene. Read again when the character changes and once a second.
    {
        static void* checked = nullptr;
        static DWORD checkedAt = 0;
        static bool isMenu = false;
        const DWORD now = GetTickCount();
        if (s.jodie != checked || now - checkedAt >= 1000) {
            char name[96] = "";
            const bool menu = s.jodie && ObjectName(s.jodie, name, sizeof(name)) && std::strncmp(name, "MENU_", 5) == 0;
            if (menu && !isMenu) Log("PLAYER '%s' belongs to a menu scene: no first person", name);
            if (!menu && isMenu) Log("PLAYER out of the menu scene: first person as set");
            checked = s.jodie;
            checkedAt = now;
            isMenu = menu;
        }
        s.menu = isMenu;
    }
    if (s.jodie != g_loggedJodie || s.control != g_loggedControl) {
        if (s.jodie && s.jodie != g_loggedJodie) {  // EXP-046: her model family, with the evidence
            char why[256];
            const int family = Family(s.jodie, why, sizeof(why));
            Log("PLAYER Jodie %p is %s: %s", s.jodie, bodykind::Name(static_cast<bodykind::Family>(family)), why);
            char name[96] = "";
            ObjectName(s.jodie, name, sizeof(name));
            crash::NoteLevel(name, bodykind::Name(static_cast<bodykind::Family>(family)));  // 1.0.0: the level, for reports
        }
        g_loggedJodie = s.jodie;
        g_loggedControl = s.control;
        float h[16];
        const bool head = HeadWorld(s.jodie, h);
        Log("PLAYER anael=%p jodie=%p (%s) control=%s (%s) head=%s", anael, s.jodie,
            fromAnael ? "Aiden's link" : s.jodie ? "by name" : "none", Name(s.control),
            s.control == Control::Aiden ? "Aiden's control byte" : g_driving.why.load(std::memory_order_relaxed),
            head ? "found" : "missing");
    }
    // Possessions (EXP-047): while a possession controller is in state 0, its body is the first-person character,
    // whatever Aiden's control byte says: walking while the body has a physics character controller (the player
    // walks it: F1), otherwise as in a cutscene (F3), the same signal as Jodie's. Logged on every change.
    if (g_pccUpdate) {
        void* pcc = nullptr;
        void* body = PossessedBody(s.jodie, &pcc);
        const bool on = g_possessionFP.load(std::memory_order_relaxed);
        void* bodyPhysics = nullptr;
        const Control bodyControl =
            body && IsLiveObject(body, kCharacterVt) && Field(body, kCharPhysics, bodyPhysics) && bodyPhysics
                ? Control::Walking
                : Control::Cutscene;
        static void* loggedBody = nullptr;
        static bool loggedOn = false;
        static Control loggedBodyControl = Control::Unknown;
        if (body != loggedBody || (body && (on != loggedOn || bodyControl != loggedBodyControl))) {
            char name[96] = "";
            if (body) game::ObjectName(body, name, sizeof(name));
            float h[16];
            if (body)
                Log("PLAYER possession: Aiden possesses %p '%s' (centre %p; Aiden control byte %d, %s, head %s) -> %s",
                    body, name, pcc, int(aiden), Name(bodyControl), HeadWorld(body, h) ? "found" : "MISSING",
                    on ? "first person follows that body" : "option off: first person stays with Jodie");
            else if (loggedBody)
                Log("PLAYER possession over: back to Jodie (%s)", Name(s.control));
            loggedBody = body;
            loggedOn = on;
            loggedBodyControl = bodyControl;
        }
        s.possessing = body != nullptr;
        if (body && on) {
            s.jodie = body;
            s.possessed = true;
            s.control = bodyControl;
        }
    }
    {
        void* physics = nullptr;
        s.physics = s.jodie && Field(s.jodie, kCharPhysics, physics) && physics;
    }
    // EXP-059: her motion kit synced to another character's (SyncMotionKit: riding a horse), see State::synced.
    {
        void* kit = nullptr;
        void* graph = nullptr;
        s.kitRead = s.jodie && Field(s.jodie, kCharMotionKit, kit) && kit && Field(kit, kMotionKitGraph, graph) && graph &&
                    Field(graph, kGraphSyncedTo, s.synced);
        if (!s.kitRead) s.synced = nullptr;
        // EXP-064: the objects synced to her (see State::syncedProps).
        if (s.kitRead && !s.synced) {
            void* g = nullptr;
            Field(graph, kGraphFollowers, g);
            for (int k = 0; g && k < 8 && s.syncedPropCount < 8; ++k) {
                void* master = nullptr;
                uint8_t* sub = nullptr;
                const void* skel = nullptr;
                if (!Field(g, kGraphSyncedTo, master) || master != graph) break;
                if (Field(g, kGraphOwnerSub, sub) && reinterpret_cast<uintptr_t>(sub) > 0x10000 + kCharSyncSub &&
                    Field(sub - kCharSyncSub, kCharSkeleton, skel) && skel)
                    s.syncedProps[s.syncedPropCount++] = skel;
                if (!Field(g, kGraphFollowers, g)) break;
            }
            static uint32_t loggedCount = 0;
            static const void* loggedFirst = nullptr;
            if (s.syncedPropCount != loggedCount || (s.syncedPropCount && s.syncedProps[0] != loggedFirst)) {
                loggedCount = s.syncedPropCount;
                loggedFirst = s.syncedPropCount ? s.syncedProps[0] : nullptr;
                Log("PLAYER %u object(s) played in step with her (SyncMotionKit to her motion graph %p): carried with "
                    "her drawn hands", s.syncedPropCount, graph);
            }
        }
        static void* loggedSynced = nullptr;
        static void* loggedOwner = nullptr;
        if (s.kitRead && (s.synced != loggedSynced || s.jodie != loggedOwner)) {
            if (s.synced) {
                // The graph's owner (INFERENCE, log only): vfunc_03 calls slot 3 of the object at each synced graph's
                // +0x6c0, and slot 3 of CHARACTER's sub-object at +0x128 is that unsync, so +0x6c0 = owner + 0x128.
                char name[96] = "";
                uint8_t* sub = nullptr;
                if (!(Field(s.synced, kGraphOwnerSub, sub) && reinterpret_cast<uintptr_t>(sub) > 0x10000 + kCharSyncSub &&
                      ObjectName(sub - kCharSyncSub, name, sizeof(name))))
                    strcpy_s(name, "?");
                Log("PLAYER her motion kit is synced to graph %p of '%s' (SyncMotionKit: riding): first-person eye height "
                    "of the ride", s.synced, name);
            } else if (loggedSynced && s.jodie == loggedOwner) {
                Log("PLAYER her motion kit is no longer synced (UnsyncMotionKit): first-person eye height on foot");
            }
            loggedSynced = s.synced;
            loggedOwner = s.jodie;
        }
    }
    // EXP-055: the engine's own control signals of this character, logged on change (read-only, every 50 ms).
    {
        static ULONGLONG probeAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - probeAt >= 50) {
            probeAt = now;
            ctrlprobe::Frame(s.menu ? nullptr : s.jodie);
        }
    }
    menuwatch::Frame(s.menu);  // 1.0.3: the main menu's Options page (the VR panel beside it, playbook 10.6)
    {
        std::lock_guard lock(g_lastMutex);
        g_last = s;
    }
    return s;
}

State Last() {
    std::lock_guard lock(g_lastMutex);
    return g_last;
}

void Install() {
    wchar_t path[MAX_PATH]{}, v[16]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"b2svr.ini";
    GetPrivateProfileStringW(L"b2svr", L"fp_possession", L"off", v, 16, ini.c_str());
    g_possessionFP = _wcsicmp(v, L"on") == 0;
    void* target = reinterpret_cast<void*>(game::Va(kPccUpdateVa));
    if (!target || !game::Va(kPccVt)) {
        Log("PLAYER possession controller not found in this build: no first person in possessions");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookPccUpdate), reinterpret_cast<void**>(&g_pccUpdate)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_pccUpdate = nullptr;
        Log("PLAYER possession controller hook FAILED: no first person in possessions");
        return;
    }
    Log("PLAYER possession controller hook installed (POSSESSION_CENTRE_CONTROLLER::vfunc_124); possessions in first "
        "person %s", g_possessionFP.load() ? "on" : "off");
}

bool PossessionFirstPerson() { return g_possessionFP.load(std::memory_order_relaxed); }
void SetPossessionFirstPerson(bool on) {
    if (g_possessionFP.exchange(on) == on) return;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"b2svr.ini";
    WritePrivateProfileStringW(L"b2svr", L"fp_possession", on ? L"on" : L"off", ini.c_str());
    Log("FP Aiden possessions in first person %s", on ? "on" : "off");
}

int Family(void* character, char* why, size_t cap) {
    char name[96] = "", model[128] = "";
    ObjectName(character, name, sizeof(name));
    void* handle = nullptr;
    Field(character, 0x300, handle);
    if (!HandleName(handle, model, sizeof(model))) {
        if (void* obj = ModelOf(character)) ObjectName(obj, model, sizeof(model));
    }
    bodykind::Family f = bodykind::FromModelName(model);
    if (f != bodykind::Family::Unknown) {
        snprintf(why, cap, "model '%s' (character '%s')", model, name);
        return int(f);
    }
    const int scene = bodykind::SceneOf(name);
    f = bodykind::FromScene(scene);
    snprintf(why, cap, "scene %04d of character '%s' (model name '%s')", scene < 0 ? 0 : scene, name, model);
    return int(f);
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
