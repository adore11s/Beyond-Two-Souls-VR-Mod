#include "prompts.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "fp_math.h"
#include "fpbody.h"
#include "fpcam.h"
#include "game.h"
#include "log.h"

namespace b2s::prompts {
namespace {

// STATIC (EXP-038): FUN_140e41750 updates one iconic MPAR GUI record per frame. The record's first
// qword is the MPAR element; the element's world position (+0x230/+0x234/+0x238, recomputed by the
// element each frame) is copied into the record's PositionX/Y/Z (+0x10..+0x18), then projected
// with the game camera into ScreenX/Y, IsBehind, IsOnRight, ScreenAngle (all bound to MenusMaster
// parameters). Moving the element's position for the duration of the call keeps every derived
// value consistent; game logic only ever sees its own position again afterwards.
constexpr uint64_t kUpdateRecordVa = 0x140e41750;
constexpr uint64_t kSetAnchorVa = 0x1411ff230;      // dialogue / thoughts display: per-frame world anchor
constexpr size_t kElementPos = 0x230;
// Prompts in a column around / above her head are hers (owner, 0.15.0: "CLAYTON" above the head
// was not caught by a 0.6 m sphere): within 0.5 m horizontally, from 0.7 m below to 1.5 m above.
constexpr float kNearHeadXZ = 0.5f, kBelowHead = 0.7f, kAboveHead = 1.5f;
constexpr float kAboveHand = 0.12f;  // metres above the left hand
constexpr float kSpread = 0.5f;      // several choices keep their arrangement, half as wide

using UpdateFn = void (*)(void* record);
UpdateFn g_original = nullptr;
std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_moved{0}, g_calls{0}, g_inF1{0};
std::atomic<uint64_t> g_lastUpdateMs{0};  // last iconic record update (Showing)
uint64_t g_lastSummaryTick = 0, g_lastSummaryCalls = 0;

// Diagnostic: the first frames of every element seen in F1 (offset from her head, pinned or not).
void* g_seen[64]{};
uint32_t g_seenCount = 0;
void LogElementOnce(void* element, pose::Vec3 fromHead, bool pinned) {
    for (uint32_t i = 0; i < g_seenCount; ++i)
        if (g_seen[i] == element) return;
    if (g_seenCount < 64) g_seen[g_seenCount++] = element;
    else return;
    Log("PROMPTS element %p in F1: from her head right/up/fwd-world (%.2f %.2f %.2f), %.2f m horizontally -> %s", element,
        fromHead.x, fromHead.y, fromHead.z, std::sqrt(fromHead.x * fromHead.x + fromHead.z * fromHead.z),
        pinned ? "pinned above the left hand" : "left where the game put it");
}

// Pinned position for something the game anchors at offset `fromHead` from her head.
pose::Vec3 Pinned(pose::Vec3 hand, pose::Vec3 fromHead) {
    // Keep the arrangement of several choices (half as wide); never float more than 25 cm off the hand.
    const pose::Vec3 spread{fromHead.x * kSpread, std::fmin(std::fmax(fromHead.y * kSpread, -0.15f), 0.25f),
                            fromHead.z * kSpread};
    return fp::Add(fp::Add(hand, pose::Vec3{0.f, kAboveHand, 0.f}), spread);
}
bool NearHead(pose::Vec3 fromHead) {
    const float horizontal = std::sqrt(fromHead.x * fromHead.x + fromHead.z * fromHead.z);
    return fp::Finite(fromHead) && horizontal <= kNearHeadXZ && fromHead.y >= -kBelowHead && fromHead.y <= kAboveHead;
}

// STATIC (EXP-038): dialogue choices / thoughts (e.g. a name to call out) are a MenusMaster display
// anchored at a world matrix. FUN_140e26c80 (per frame while a dialogue is shown, tail-called from
// 0x140d5ca10) builds it from DialogCharacter's bone named DialogOffsetBoneName (globals
// 0x141ca7520 / 0x141ca7500; her head) plus DialogGlobal/LocalOffset (0x141ca74e0 / 0x141ca74f0),
// THOUGHTS_GUI (FUN_1410b29c0) likewise, and both hand it to FUN_1411ff230(display, matrix, dt),
// which stores it at display+0x170 for drawing. A copy with the translation moved is passed on.
using SetAnchorFn = void (*)(void* display, const float* matrix, uint32_t dt);
SetAnchorFn g_setAnchorOriginal = nullptr;

// The pinned anchor stays put (owner, 0.15.2 / 0.15.3: following the hand every frame made the
// words shake hard; smoothing only made them drift; 0.15.5: a deadband glide was not wanted either).
// When a dialogue appears it is placed above the left hand with the game's rotation of that moment
// and then left fixed in the world; it moves again only if the hand gets 40 cm away.
constexpr float kRelockDistance = 0.4f;
struct Lock {
    void* display = nullptr;
    float m[16]{};
    uint64_t lastTick = 0;
};
Lock g_locks[4];
Lock& LockFor(void* display) {
    for (auto& e : g_locks)
        if (e.display == display) return e;
    Lock* oldest = &g_locks[0];
    for (auto& e : g_locks)
        if (e.lastTick < oldest->lastTick) oldest = &e;
    *oldest = Lock{};
    oldest->display = display;
    return *oldest;
}
std::atomic<uint64_t> g_anchorsPinned{0};
bool g_anchorLogged = false;
bool CopyMatrix(const float* m, float out[16]) {
    __try {
        std::memcpy(out, m, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The display's word wobble (FUN_1411f83e0: per-word sine offsets x display+0x340, clamped to 0.3 by
// FUN_1411faa80) ran visibly fast on the pinned words (owner, 0.15.2 / 0.16.0: "shaking"). While a
// dialogue is pinned its factor is 0 (the words hold still); the game's value is put back after.
constexpr size_t kDisplayNoise = 0x340;
struct NoiseSave {
    void* display = nullptr;
    float value = 0.f;
};
NoiseSave g_noise[4];
bool SwapNoise(void* display, float set, float* old) {
    __try {
        float* f = reinterpret_cast<float*>(static_cast<uint8_t*>(display) + kDisplayNoise);
        *old = *f;
        *f = set;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void SilenceNoise(void* display) {
    for (auto& e : g_noise)
        if (e.display == display) {
            float old;
            SwapNoise(display, 0.f, &old);
            return;
        }
    for (auto& e : g_noise)
        if (!e.display) {
            if (SwapNoise(display, 0.f, &e.value)) e.display = display;
            return;
        }
}
void RestoreNoise(void* display) {
    for (auto& e : g_noise)
        if (e.display == display) {
            float old;
            SwapNoise(display, e.value, &old);
            e.display = nullptr;
        }
}

// First person (F1 and F3): the dialogue goes in front of the view (owner, 2026-10-11: F3 first, then
// F1 too instead of the left hand), 70 cm ahead along the gaze, then stays put; placed again for a new
// dialogue, when the gaze turns more than 35 deg away from it, or when it ends up nearer than 30 /
// further than 120 cm (she moved).
constexpr float kViewDistance = 0.7f, kViewDrop = 0.08f;

void HookSetAnchorView(void* display, float m[16], uint32_t dt, pose::Vec3 head, pose::Vec3 eye, pose::Vec3 fwd) {
    const pose::Vec3 fromHead = fp::Sub(pose::Vec3{m[12], m[13], m[14]}, head);
    if (!NearHead(fromHead)) {
        RestoreNoise(display);
        return g_setAnchorOriginal(display, m, dt);
    }
    SilenceNoise(display);
    const uint64_t now = GetTickCount64();
    Lock& lock = LockFor(display);
    const pose::Vec3 target = fp::Sub(fp::Add(eye, fp::Mul(fwd, kViewDistance)), pose::Vec3{0.f, kViewDrop, 0.f});
    const pose::Vec3 locked{lock.m[12], lock.m[13], lock.m[14]};
    const pose::Vec3 toLocked = fp::Sub(locked, eye);
    const float dist = fp::Len(toLocked);
    const bool looking = dist > 1e-3f && fp::Dot(fp::Mul(toLocked, 1.f / dist), fwd) > 0.82f;  // within ~35 deg
    if (!lock.lastTick || now - lock.lastTick > 500 || !looking || dist < 0.3f || dist > 1.2f) {
        std::memcpy(lock.m, m, sizeof(lock.m));
        lock.m[12] = target.x;
        lock.m[13] = target.y;
        lock.m[14] = target.z;
    }
    lock.lastTick = now;
    std::memcpy(m, lock.m, 64);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("PROMPTS first dialogue placed in front of the view (anchor was %.2f m above her head)", fromHead.y);
    }
    g_setAnchorOriginal(display, m, dt);
}

void HookSetAnchor(void* display, const float* matrix, uint32_t dt) {
    float m[16];
    pose::Vec3 head, eye, fwd;
    if (!g_enabled.load(std::memory_order_relaxed) || !matrix || !CopyMatrix(matrix, m) ||
        !fpbody::PromptView(&head, &eye, &fwd)) {
        if (display) RestoreNoise(display);
        return g_setAnchorOriginal(display, matrix, dt);
    }
    if (g_anchorsPinned.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("PROMPTS dialogue anchors in first person go in front of the view");
    HookSetAnchorView(display, m, dt, head, eye, fwd);
}

bool ReadElement(void* record, uint8_t** element, float pos[3]) {
    __try {
        *element = *static_cast<uint8_t**>(record);
        if (!*element) return false;
        std::memcpy(pos, *element + kElementPos, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WritePosition(uint8_t* element, const float pos[3]) {
    __try {
        std::memcpy(element + kElementPos, pos, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// STATIC (FUN_140e41750): the record projects its anchor through the game camera; outside the
// screen (|x| or |y| >= 1, or behind) it sets +0x1f2 and takes its display down, and the prompt
// cannot be done. In first person the game camera is the player's view (owner 0.16.14, F3 cutscene:
// "a controller showing me to move my joystick up" worked only when looking down at it). An anchor
// more than 25 deg off the view is put, for the game's update only, 0.8 m out at the edge of that
// cone on its side, so it stays on screen and can be done; the real anchor is restored after.
void UpdateInView(void* record, uint8_t* element, const float original[3]) {
    pose::Vec3 head, eye, fwd;
    const pose::Vec3 at{original[0], original[1], original[2]};
    if (!fp::Finite(at) || !fpbody::PromptView(&head, &eye, &fwd)) return g_original(record);
    fwd = fp::Norm(fwd);
    const pose::Vec3 to = fp::Sub(at, eye);
    const float dist = fp::Len(to);
    const pose::Vec3 dir = dist > 1e-3f ? fp::Mul(to, 1.f / dist) : fwd;
    constexpr float kCone = 0.4363323f;  // 25 deg
    const float cosA = fp::Dot(dir, fwd);
    if (cosA >= std::cos(kCone)) return g_original(record);
    pose::Vec3 side = fp::Sub(dir, fp::Mul(fwd, cosA));
    side = fp::Len(side) > 1e-3f ? fp::Norm(side) : pose::Vec3{0.f, -1.f, 0.f};  // straight behind: below
    const pose::Vec3 placed = fp::Add(eye, fp::Mul(fp::Add(fp::Mul(fwd, std::cos(kCone)), fp::Mul(side, std::sin(kCone))), 0.8f));
    const float moved[3] = {placed.x, placed.y, placed.z};
    if (!WritePosition(element, moved)) return g_original(record);
    g_original(record);
    WritePosition(element, original);
    static std::atomic<uint64_t> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("PROMPTS first prompt outside the first-person view (%.0f deg off, %.2f m) kept on screen at the view's edge",
            std::acos(std::clamp(cosA, -1.f, 1.f)) * 57.29578f, dist);
}

void HookUpdateRecord(void* record) {
    uint8_t* element = nullptr;
    float original[3];
    pose::Vec3 head, hand;
    const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t now = GetTickCount64();
    g_lastUpdateMs.store(now, std::memory_order_relaxed);
    if (now - g_lastSummaryTick > 10000) {
        if (calls != g_lastSummaryCalls)
            Log("PROMPTS iconic prompt updates: %llu (in F1 %llu, pinned %llu)", static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(g_inF1.load()), static_cast<unsigned long long>(g_moved.load()));
        g_lastSummaryTick = now;
        g_lastSummaryCalls = calls;
    }
    if (!g_enabled.load(std::memory_order_relaxed) || !record || !ReadElement(record, &element, original))
        return g_original(record);
    if (!fpbody::PromptAnchors(&head, &hand)) return UpdateInView(record, element, original);
    g_inF1.fetch_add(1, std::memory_order_relaxed);
    const pose::Vec3 at{original[0], original[1], original[2]};
    const pose::Vec3 fromHead = fp::Sub(at, head);
    const bool hers = fp::Finite(at) && NearHead(fromHead);
    LogElementOnce(element, fromHead, hers);
    if (!hers) return UpdateInView(record, element, original);
    const pose::Vec3 moved = Pinned(hand, fromHead);
    const float pinned[3] = {moved.x, moved.y, moved.z};
    if (!WritePosition(element, pinned)) return g_original(record);
    g_original(record);
    WritePosition(element, original);
    if (g_moved.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("PROMPTS first in-world prompt at Jodie's head pinned above her left hand (%.2f m from the head)",
            fp::Len(fromHead));
}

// Button prompts in F3 (0.17.2, owner live in _1401_EMBASSY: "the HOLD and LB prompt should face in front of me
// in F3 like the Clayton prompt"; it is drawn in the 3D pass, the 2D layer was empty). STATIC (EXP-040 0.16.34,
// re-verified): the button / stick prompts (MPAR_ELEMENT_HOLD, STANDARD, MASH, AXIS_*) are each a small
// MenusMaster menu (FUN_140fa4e00(element)+0x148), drawn in the world while its byte +6 is set at its matrix
// +0x30..+0x6c (rows X / Y / Z / position). FUN_140e46850 (MPAR_GUI, per element; entry +0x10 element, +0x18
// that menu) builds that matrix at the object or bone the prompt is about, turned toward the GAME camera - in F3
// the shot camera, not the eye - and stores a copy in the element for game logic (+0x200). In F3 the menu's
// copy only is replaced after the call: in front of the view (5 deg above the gaze; the white dots sit below it),
// at the distance it had from the eye (0.7 - 2 m, so it keeps its drawn size), facing the view, world-locked and
// placed again when new, unseen for 0.5 s, 35 deg off the gaze or much nearer / further. The white dots
// (MPAR_ELEMENT_ICONIC) keep their own handling (HookUpdateRecord).
constexpr uint64_t kPlaceElementVa = 0x140e46850;
constexpr uint64_t kIconicVtableVa = 0x14179d130;  // QDT::SINT::MPAR_ELEMENT_ICONIC
constexpr size_t kEntryElement = 0x10, kEntryMenu = 0x18, kMenu3D = 6, kMenuMatrix = 0x30;
constexpr float kPromptMinDistance = 0.7f, kPromptMaxDistance = 2.0f, kPromptLift = 0.0873f;  // 5 deg
using PlaceElementFn = void (*)(void* gui, void* entry);
PlaceElementFn g_placeOriginal = nullptr;
uintptr_t g_iconicVt = 0;
struct Placed {
    uint8_t* menu = nullptr;
    float m[16]{};
    float distance = 0.f;
    uint64_t seen = 0;
};
Placed g_placed[8];
std::atomic<uint64_t> g_promptsPlaced{0};

// Rows X = view right, Y = up, Z = X x Y (toward the eye; engine right-handed, -Z forward), each keeping its
// length (the menu's scale). FUN_140c27690 builds the prompts' turn-to-camera matrix the same way (Z row =
// from the prompt toward what it faces).
void FaceView(float m[16], pose::Vec3 fwd) {
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (!(len > 1e-3f)) return;
    const pose::Vec3 right{-fwd.z / len, 0.f, fwd.x / len}, up{0.f, 1.f, 0.f};
    const pose::Vec3 rows[3] = {right, up, fp::Cross(right, up)};
    for (int r = 0; r < 3; ++r) {
        float scale = fp::Len(pose::Vec3{m[r * 4], m[r * 4 + 1], m[r * 4 + 2]});
        if (!std::isfinite(scale) || scale < 1e-3f) scale = 1.f;
        m[r * 4] = rows[r].x * scale;
        m[r * 4 + 1] = rows[r].y * scale;
        m[r * 4 + 2] = rows[r].z * scale;
    }
}

// The entry's menu (only while drawn in 3D) and the element's class.
bool ReadPrompt(void* entry, uint8_t** menu, float m[16], uintptr_t* elementVt) {
    __try {
        const uint8_t* e = static_cast<uint8_t*>(entry);
        const uint8_t* element = *reinterpret_cast<uint8_t* const*>(e + kEntryElement);
        *elementVt = element ? *reinterpret_cast<const uintptr_t*>(element) : 0;
        *menu = *reinterpret_cast<uint8_t* const*>(e + kEntryMenu);
        if (!*menu || !(*menu)[kMenu3D]) return false;
        std::memcpy(m, *menu + kMenuMatrix, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WritePrompt(uint8_t* menu, const float m[16]) {
    __try {
        std::memcpy(menu + kMenuMatrix, m, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Diagnostic: every 3D prompt menu seen in first person, once (its class, where it was).
uint8_t* g_promptsSeen[32]{};
uint32_t g_promptsSeenCount = 0;
bool FirstSight(uint8_t* menu) {
    for (uint32_t i = 0; i < g_promptsSeenCount; ++i)
        if (g_promptsSeen[i] == menu) return false;
    if (g_promptsSeenCount >= std::size(g_promptsSeen)) return false;
    g_promptsSeen[g_promptsSeenCount++] = menu;
    return true;
}

void HookPlaceElement(void* gui, void* entry) {
    g_placeOriginal(gui, entry);
    uint8_t* menu = nullptr;
    float m[16];
    uintptr_t vt = 0;
    pose::Vec3 head, eye, fwd;
    if (!g_enabled.load(std::memory_order_relaxed) || !entry || !ReadPrompt(entry, &menu, m, &vt) ||
        !fpbody::PromptView(&head, &eye, &fwd))
        return;
    const pose::Vec3 anchor{m[12], m[13], m[14]};
    if (!fp::Finite(anchor)) return;
    const bool iconic = g_iconicVt && vt == g_iconicVt;
    const bool f3 = fpcam::CutsceneFirstPerson();
    if (FirstSight(menu))
        Log("PROMPTS 3D prompt %p (%s, class vt 0x%llx) %.2f m from the eye in %s -> %s", static_cast<void*>(menu),
            iconic ? "white dot" : "button prompt",
            static_cast<unsigned long long>(vt ? vt - game::ModuleBase() + 0x140000000ull : 0),
            fp::Len(fp::Sub(anchor, eye)), f3 ? "F3" : "F1",
            !iconic && f3 ? "in front of the view, facing it" : "left where the game puts it");
    if (iconic || !f3) return;
    fwd = fp::Norm(fwd);
    const uint64_t now = GetTickCount64();
    Placed* slot = nullptr;
    for (auto& p : g_placed)
        if (p.menu == menu) {
            slot = &p;
            break;
        }
    if (!slot)
        for (auto& p : g_placed)
            if (!slot || p.seen < slot->seen) slot = &p;  // free or least recently seen
    bool place = slot->menu != menu || now - slot->seen > 500;
    if (!place) {
        const pose::Vec3 to = fp::Sub(pose::Vec3{slot->m[12], slot->m[13], slot->m[14]}, eye);
        const float d = fp::Len(to);
        place = d < 0.45f * slot->distance || d > 1.7f * slot->distance ||
                fp::Dot(fp::Mul(to, 1.f / std::max(d, 1e-3f)), fwd) < std::cos(0.6109f);  // 35 deg
    }
    if (place) {
        const float fromEye = fp::Len(fp::Sub(anchor, eye));
        const float viewYaw = std::atan2(-fwd.x, -fwd.z);
        const float pitch = std::asin(std::clamp(fwd.y, -0.95f, 0.95f)) + kPromptLift;
        const pose::Vec3 dir{-std::sin(viewYaw) * std::cos(pitch), std::sin(pitch), -std::cos(viewYaw) * std::cos(pitch)};
        slot->menu = menu;
        slot->distance = std::clamp(fromEye, kPromptMinDistance, kPromptMaxDistance);
        std::memcpy(slot->m, m, sizeof(slot->m));
        FaceView(slot->m, fwd);
        const pose::Vec3 at = fp::Add(eye, fp::Mul(dir, slot->distance));
        slot->m[12] = at.x;
        slot->m[13] = at.y;
        slot->m[14] = at.z;
        if (g_promptsPlaced.fetch_add(1, std::memory_order_relaxed) < 8)
            Log("PROMPTS button prompt %p placed in front of the view, facing it: %.2f m (it was %.2f m from the eye)",
                static_cast<void*>(menu), slot->distance, fromEye);
    }
    slot->seen = now;
    WritePrompt(menu, slot->m);
}

}  // namespace

void SetEnabled(bool on) {
    g_enabled = on;
    Log("PROMPTS on the left hand in F1: %s", on ? "on" : "off");
}
bool Enabled() { return g_enabled.load(); }
uint64_t LastShownMs() { return g_lastUpdateMs.load(std::memory_order_relaxed); }
bool Showing(uint64_t nowMs) {
    const uint64_t last = g_lastUpdateMs.load(std::memory_order_relaxed);
    return last && nowMs >= last && nowMs - last < 250;
}

bool Install() {
    wchar_t path[MAX_PATH]{}, value[16]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"b2svr.ini");
        GetPrivateProfileStringW(L"b2svr", L"fp_prompts_on_hand", L"on", value, 16, path);
        g_enabled = _wcsicmp(value, L"off") != 0;
    }
    void* target = reinterpret_cast<void*>(game::Va(kUpdateRecordVa));
    if (!target) {
        Log("PROMPTS iconic prompt update not found in this build: prompts stay where the game puts them");
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookUpdateRecord), reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("PROMPTS hook FAILED");
        return false;
    }
    Log("PROMPTS hook installed (in F1, prompts at Jodie's head go above her left hand: %s)",
        g_enabled.load() ? "on" : "off");
    if (void* anchor = reinterpret_cast<void*>(game::Va(kSetAnchorVa))) {
        if (MH_CreateHook(anchor, reinterpret_cast<void*>(&HookSetAnchor), reinterpret_cast<void**>(&g_setAnchorOriginal)) ==
                MH_OK &&
            MH_EnableHook(anchor) == MH_OK)
            Log("PROMPTS dialogue / thoughts anchor hook installed");
        else
            Log("PROMPTS dialogue / thoughts anchor hook FAILED");
    } else {
        Log("PROMPTS dialogue anchor not found in this build: dialogue choices stay where the game puts them");
    }
    g_iconicVt = game::Va(kIconicVtableVa);
    if (void* place = reinterpret_cast<void*>(game::Va(kPlaceElementVa)); place && g_iconicVt) {
        if (MH_CreateHook(place, reinterpret_cast<void*>(&HookPlaceElement), reinterpret_cast<void**>(&g_placeOriginal)) ==
                MH_OK &&
            MH_EnableHook(place) == MH_OK)
            Log("PROMPTS button prompt placement hook installed (F3: in front of the view)");
        else
            Log("PROMPTS button prompt placement hook FAILED");
    } else {
        Log("PROMPTS button prompt placement not found in this build: button prompts stay where the game puts them");
    }
    return true;
}

}  // namespace b2s::prompts
