#include "fpcam.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <MinHook.h>

#include "bones.h"
#include "camdirector.h"
#include "game.h"
#include "heapscan.h"
#include "hotkeys.h"
#include "log.h"
#include "player.h"
#include "watchpoint.h"
#include "fpmode.h"
#include "fp_math.h"
#include "bodykind.h"
#include "fpbody.h"
#include "prompts.h"
#include "vrinput.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <cwchar>

namespace b2s::fpcam {
namespace {

bool g_ready = false;
// RecordList, camera builds, Present and dev commands can cross threads.
// Recursive because Prepare's failure paths use the public Suspend operation.
std::recursive_mutex g_stateMutex;
bool g_f1 = false, g_f3 = false, g_arms = true;
bool g_f3NeedsCalibration = true;  // F3 head -> view calibration must be redone (see Prepare)
hotkeys::Edge g_f1Key, g_f3Key;
// EXP-059 (owner 0.24.0, 2003 Navajo Day 2: "when I got off the horse the view stayed ~1 m too high"). LIVE: from the
// dismount on the view sat 0.70-0.85 m above her drawn head (FPDIAG headDy, 2366-2412 s, through 7 F1 toggles), on the
// horse at it (-0.20..+0.09). The one per-body eye had been relearned on the horse (a > 3 m base jump at 2280-2283 s,
// then the 2.5 s "take what there is" fallback: a rider's head is out of the standing range above her root) and
// nothing relearned it on foot. Now a second eye is learned while her motion kit is synced to another character's
// (player::State::synced, the engine's SyncMotionKit state), and the on-foot eye is kept for when she gets off.
fp::MountedEyes g_eyes;
void* g_actorNode = nullptr;
void* g_actorSkeleton = nullptr;
void* g_cameraNode = nullptr;
pose::Mat4 g_base = pose::Identity();
bool g_baseValid = false;
// Eye point relative to the M_Head joint (the top of the neck), in the view's yaw frame: the
// eyes sit about 9 cm in front of it and 7 cm above it (design spec 2026-09-28; 0.8.0 used
// 6 cm / 0 and put the view at the base of the skull). b2svr.ini fp_eye_fwd / fp_eye_up.
float g_viewYaw = 0.f, g_eyeForward = 0.09f, g_eyeUp = 0.07f;
float g_wristPitch = 0.f;  // radians; b2svr.ini fp_wrist_pitch (degrees)
// Per headset family (playbook 08.3, fp::HeadsetFamily of the OpenXR system name): b2svr.ini wristpitch_psvr2,
// wristpitch_quest, wristpitch_other (degrees); each falls back to fp_wrist_pitch, which was tuned on PS VR2.
// B2S's grip basis is not GoW's, so GoW's 25 / 25 / 5 degrees do not carry over: the defaults stay 0 until a
// headset calibration per family.
float g_wristPitchFamily[3] = {0.f, 0.f, 0.f};  // radians
int g_family = -1;  // the family last used (logged when it changes)
// Drawn-hand smoothing (playbook 08.1) in F1 with motion controls: b2svr.ini hand_smoothing (on; the VR settings
// panel's "Hand smoothing" row, read live), hand_smooth_cutoff (3 Hz), hand_smooth_beta (6). Gestures, the panel
// pointer and the game's input keep the raw controller samples (the pointer has its own filter).
bool g_handSmoothing = true;
float g_handCutoff = 3.f, g_handBeta = 6.f;
fp::HandFilter g_handFilter[2];
// F1 right-stick turning: smooth (default) or snap steps. b2svr.ini turn=smooth|snap and
// snap_turn_degrees (the launcher asks); dev verb "turn smooth|snap [degrees]".
// The headset eye in the world (Jodie's eye base + your head), for the room the eye is in (zonefix).
float g_eyeWorld[3] = {};
bool g_eyeWorldValid = false;
bool g_snapTurn = false;
float g_snapStep = 0.78539816f;  // radians (45 deg)
// Smooth turning speed (VR settings panel "Turning speed", GoW presets 20 .. 100 %): a fraction of 120 deg/s.
float g_turnSpeed = 1.f;
fp::SnapTurn g_snap;
uint64_t g_lastFrame = 0, g_lastAnchor = 0;
int g_implausibleEyeFrames = 0;
fp::Mode g_lastMode = fp::Mode::Cinematic;
fp::BehaviourClass g_lastKind = fp::BehaviourClass::Unknown;

bool GameForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

std::wstring IniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1) + L"b2svr.ini";
}
// A first-person choice changed in the game (keys, the head switch, the VR settings panel): kept in b2svr.ini.
void Save(const wchar_t* key, const wchar_t* value) { WritePrivateProfileStringW(L"b2svr", key, value, IniPath().c_str()); }

void SetF1(bool on, const char* why) {
    g_f1 = on;
    Save(L"firstperson", on ? L"on" : L"off");
    Log("FP F1=%d (%s)", int(on), why);
}
void SetF3(bool on, const char* why) {
    g_f3 = on;
    g_f3NeedsCalibration = true;
    Save(L"fpcutscenes", on ? L"on" : L"off");
    Log("FP F3=%d (%s)", int(on), why);
}

void* SkeletonOf(void* node) {
    __try {
        return node ? *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + 0x170) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void ReadSettings() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"b2svr.ini");
    wchar_t value[32];
    GetPrivateProfileStringW(L"b2svr", L"firstperson", L"off", value, 32, path);
    g_f1 = _wcsicmp(value, L"on") == 0;
    GetPrivateProfileStringW(L"b2svr", L"fpcutscenes", L"off", value, 32, path);
    g_f3 = _wcsicmp(value, L"on") == 0;
    GetPrivateProfileStringW(L"b2svr", L"fparms", L"on", value, 32, path);
    g_arms = _wcsicmp(value, L"off") != 0;
    auto number = [&](const wchar_t* key, float fallback, float lo, float hi) {
        GetPrivateProfileStringW(L"b2svr", key, L"", value, 32, path);
        if (!value[0]) return fallback;
        const float v = static_cast<float>(_wtof(value));
        return std::isfinite(v) && v >= lo && v <= hi ? v : fallback;
    };
    g_eyeForward = number(L"fp_eye_fwd", g_eyeForward, -0.3f, 0.4f);
    g_eyeUp = number(L"fp_eye_up", g_eyeUp, -0.3f, 0.4f);
    g_wristPitch = number(L"fp_wrist_pitch", 0.f, -90.f, 90.f) * 0.017453293f;
    const wchar_t* familyKeys[3] = {L"wristpitch_psvr2", L"wristpitch_quest", L"wristpitch_other"};
    for (int f = 0; f < 3; ++f)
        g_wristPitchFamily[f] = number(familyKeys[f], g_wristPitch * 57.29578f, -90.f, 90.f) * 0.017453293f;
    // 0.23.0 (owner: "dont hide head in first person ... i want to see mirror reflections"): show. 0.24.0 (owner: "make
    // the head (teeth, hair, head) hide in first person cutscenes, but keep it ... in first person gameplay"): cutscene =
    // hidden in F3 only, the default.
    GetPrivateProfileStringW(L"b2svr", L"fp_head", L"cutscene", value, 32, path);
    fpbody::SetHeadMode(_wcsicmp(value, L"show") == 0 ? fpbody::kHeadShown
                        : _wcsicmp(value, L"hide") == 0 ? fpbody::kHeadHidden
                                                        : fpbody::kHeadHiddenInCutscenes);
    Log("FP head in first person: %s (b2svr.ini fp_head)", fpbody::HeadModeName(fpbody::GetHeadMode()));
    // EXP-067 (playbook 14.12): fingers on the controllers, from the rig's rest shape.
    GetPrivateProfileStringW(L"b2svr", L"fp_fingers", L"on", value, 32, path);
    const bool fingersOn = !(_wcsicmp(value, L"off") == 0 || _wcsicmp(value, L"0") == 0);
    GetPrivateProfileStringW(L"b2svr", L"fp_fingers_rest_shape", L"on", value, 32, path);
    fpbody::SetFingers(fingersOn, !(_wcsicmp(value, L"off") == 0 || _wcsicmp(value, L"0") == 0));
    GetPrivateProfileStringW(L"b2svr", L"hand_smoothing", L"on", value, 32, path);
    g_handSmoothing = !(_wcsicmp(value, L"off") == 0 || _wcsicmp(value, L"0") == 0);
    g_handCutoff = number(L"hand_smooth_cutoff", 3.f, 0.1f, 30.f);
    g_handBeta = number(L"hand_smooth_beta", 6.f, 0.f, 100.f);
    GetPrivateProfileStringW(L"b2svr", L"turn", L"smooth", value, 32, path);
    g_snapTurn = _wcsicmp(value, L"snap") == 0;
    g_snapStep = number(L"snap_turn_degrees", 45.f, 10.f, 90.f) * 0.017453293f;
    g_turnSpeed = number(L"turn_speed", 1.f, 0.1f, 1.f);
    Log("FP turning in F1: %s (speed %.0f %%, snap %.0f deg)", g_snapTurn ? "snap" : "smooth", g_turnSpeed * 100.f,
        g_snapStep * 57.29578f);
}

// ---- EXP-031 discovery: which gameplay controller updates run (walking vs cutscene vs Aiden) ----
extern "C" {
extern void* B2SVR_CountThis[8];
extern uint64_t B2SVR_CountCalls[8];
extern void* B2SVR_CountOrig[8];
void B2SVR_CountThunk0();
void B2SVR_CountThunk1();
void B2SVR_CountThunk2();
void B2SVR_CountThunk3();
void B2SVR_CountThunk4();
void B2SVR_CountThunk5();
void B2SVR_CountThunk6();
void B2SVR_CountThunk7();
}
struct CountTarget {
    uintptr_t va;
    const char* name;
};
constexpr CountTarget kCountTargets[8] = {
    {0x140fe2880, "TPGCTRL::v124"}, {0x140fe2a10, "TPGCTRL::v125"}, {0x140f5b420, "RAIL::v118"},
    {0x140f5b390, "RAIL::v117"},    {0x140f592a0, "RAIL::v112"},    {0x140e2afe0, "TPG::v105"},
    {0x140e37a40, "TPG::v108"},     {0x141271660, "ANAEL::v06"},
};
uint64_t g_countPrev[8] = {};
bool g_countersOn = false;

void InstallCounters() {
    void* thunks[8] = {reinterpret_cast<void*>(&B2SVR_CountThunk0), reinterpret_cast<void*>(&B2SVR_CountThunk1),
                       reinterpret_cast<void*>(&B2SVR_CountThunk2), reinterpret_cast<void*>(&B2SVR_CountThunk3),
                       reinterpret_cast<void*>(&B2SVR_CountThunk4), reinterpret_cast<void*>(&B2SVR_CountThunk5),
                       reinterpret_cast<void*>(&B2SVR_CountThunk6), reinterpret_cast<void*>(&B2SVR_CountThunk7)};
    // Development probe on recorded addresses: the verified build only.
    if (!game::KnownBuild()) {
        Log("FPPROBE call counters: verified build only");
        return;
    }
    int ok = 0;
    for (int i = 0; i < 8; ++i) {
        void* target = reinterpret_cast<void*>(game::ModuleBase() + (kCountTargets[i].va - 0x140000000));
        if (MH_CreateHook(target, thunks[i], &B2SVR_CountOrig[i]) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
    }
    g_countersOn = true;
    Log("FPPROBE call counters installed %d/8", ok);
}

// SEH-guarded KCOM handle -> object, cached handles only (no resolver side effects off-thread).
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

void* FieldPtr(void* obj, size_t off) {
    __try {
        return obj ? *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + off) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int FieldByte(void* obj, size_t off) {
    __try {
        return obj ? *(static_cast<uint8_t*>(obj) + off) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uintptr_t VtableVa(void* obj);
bool ProbeHeadAt(void* obj, size_t off, float out[3]);

void ProbeTpgObject(void* tpg);

// TPG controller +0x108 -> THIRD_PERSON_GAMEPLAY; +0xdc IsActive (STATIC: script IsActive/OnActivate);
// candidate character handles +0x1e0 / +0x1e8.
void ProbeTpg(void* ctrl) {
    if (!ctrl) return;
    void* tpg = CachedObject(FieldPtr(ctrl, 0x108));
    Log("FPTPG ctrl=%p vt=%llx tpg=%p vt=%llx active=%d c110=%d c111=%d c112=%d a271=%d", ctrl,
        static_cast<unsigned long long>(VtableVa(ctrl)), tpg, static_cast<unsigned long long>(VtableVa(tpg)),
        FieldByte(tpg, 0xdc), FieldByte(ctrl, 0x110), FieldByte(ctrl, 0x111), FieldByte(ctrl, 0x112),
        FieldByte(tpg, 0x271));
    ProbeTpgObject(tpg);
}

void ProbeTpgObject(void* tpg) {
    if (!tpg) return;
    Log("FPTPG obj=%p vt=%llx active=%d a271=%d h268=%p", tpg, static_cast<unsigned long long>(VtableVa(tpg)),
        FieldByte(tpg, 0xdc), FieldByte(tpg, 0x271), FieldPtr(tpg, 0x268));
    for (size_t off : {size_t(0x1e0), size_t(0x1e8), size_t(0x268)}) {
        void* obj = CachedObject(FieldPtr(tpg, off));
        float p[3] = {};
        const bool head = ProbeHeadAt(tpg, off, p);
        Log("FPTPG   tpg+0x%zx -> %p vt=%llx head=%d (%.3f %.3f %.3f)", off, obj,
            static_cast<unsigned long long>(VtableVa(obj)), int(head), p[0], p[1], p[2]);
    }
}

void LogCounters() {
    char line[512];
    int n = snprintf(line, sizeof(line), "FPCOUNT");
    for (int i = 0; i < 8; ++i) {
        const uint64_t c = B2SVR_CountCalls[i];
        n += snprintf(line + n, sizeof(line) - n, " %s=%llu(%p)", kCountTargets[i].name,
                      static_cast<unsigned long long>(c - g_countPrev[i]), B2SVR_CountThis[i]);
        g_countPrev[i] = c;
    }
    Log("%s", line);
    ProbeTpg(B2SVR_CountThis[0]);
}
std::atomic<int> g_probeFrames{0};
const uint32_t kHeadHash = bones::Crc32("M_Head");

// SEH-guarded read of a behaviour field as a KCOM handle whose object is a script entity with a
// head bone (Task 4 discovery). No C++ objects with destructors in this function (__try rule).
bool ProbeHeadAt(void* obj, size_t off, float out[3]) {
    __try {
        void* handle = *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + off);
        if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000) return false;
        const uint32_t flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(handle) + 0x20);
        if (((flags >> 29) & 1) == 0) return false;  // only cached handles: no resolver side effects
        void* entity = *reinterpret_cast<void**>(static_cast<uint8_t*>(handle) + 0x30);
        if (!entity) return false;
        void* node = bones::EntityNode(entity);
        const float* m = bones::BoneWorld(node, kHeadHash);
        if (!m) return false;
        out[0] = m[12];
        out[1] = m[13];
        out[2] = m[14];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t VtableVa(void* obj) {
    __try {
        if (!obj) return 0;
        const auto vt = reinterpret_cast<uintptr_t>(*static_cast<void**>(obj));
        return vt - game::ModuleBase() + 0x140000000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// `peek <hexaddr> [bytes]`: SEH-guarded hex dump from inside the game (external reads are denied).
void Peek(uintptr_t addr, int len) {
    char line[160];
    for (int off = 0; off < len; off += 16) {
        int n = snprintf(line, sizeof(line), "PEEK %016llx:", static_cast<unsigned long long>(addr + off));
        bool ok = true;
        __try {
            for (int i = 0; i < 16 && off + i < len; ++i)
                n += snprintf(line + n, sizeof(line) - n, " %02x", reinterpret_cast<const uint8_t*>(addr)[off + i]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        Log("%s%s", line, ok ? "" : " <unreadable>");
        if (!ok) break;
    }
}

// `findvt <hexva>`: scan committed private read-write memory for objects whose vtable is the given
// static VA (discovery only; runs on its own thread, SEH-guarded, logs at most 32 hits).
std::atomic<uintptr_t> g_findVt{0};
void* g_watchTpg = nullptr;

DWORD WINAPI FindVtThread(LPVOID) {
    void* hits[32];
    size_t mb = 0;
    const int n = heapscan::FindByVtable(g_findVt.load(), hits, 32, &mb);
    for (int i = 0; i < n; ++i) Log("FINDVT hit %p", hits[i]);
    Log("FINDVT done vt=%llx hits=%d scanned=%zu MB", static_cast<unsigned long long>(g_findVt.load()), n, mb);
    return 0;
}

void Probe() {
    void* b = camdirector::Active();
    void* c = camdirector::Child(b);
    Log("FPPROBE behaviour=%s (%p vt=%llx) child=%s (%p vt=%llx)", camdirector::Name(b), b,
        static_cast<unsigned long long>(VtableVa(b)), camdirector::Name(c), c,
        static_cast<unsigned long long>(VtableVa(c)));
    for (void* obj : {b, c}) {
        if (!obj) continue;
        for (size_t off = 0; off < 0x800; off += 8) {
            float p[3];
            if (ProbeHeadAt(obj, off, p))
                Log("FPPROBE   %s+0x%zx -> entity with M_Head at (%.3f %.3f %.3f)", camdirector::Name(obj), off, p[0],
                    p[1], p[2]);
        }
    }
}

void ProbePlayer(const player::State& st) {
    float h[16], b[16];
    const bool head = player::HeadWorld(st.jodie, h), body = player::BodyWorld(st.jodie, b);
    pose::Mat4 bm{};
    if (body) std::memcpy(bm.m, b, sizeof(b));
    Log("FPPLAYER jodie=%p control=%s head=%d (%.3f %.3f %.3f) body=%d (%.3f %.3f %.3f) yaw=%.1f", st.jodie,
        player::Name(st.control), int(head), head ? h[12] : 0.f, head ? h[13] : 0.f, head ? h[14] : 0.f, int(body),
        body ? b[12] : 0.f, body ? b[13] : 0.f, body ? b[14] : 0.f, body ? fp::YawOf(bm) * 57.29578f : 0.f);
}

}  // namespace

void Install() {
    std::lock_guard lock(g_stateMutex);
    const bool bonesOk = bones::Install();
    const bool dirOk = camdirector::Install();
    g_ready = bonesOk && dirOk;
    fpbody::Install();
    prompts::Install();
    player::Install();  // EXP-047: the possession-controller hook (possessions)
    ReadSettings();
    vrinput::Install();
    Log("FPCAM install: bones=%d director=%d eye fwd=%.3f up=%.3f wrist pitch=%.1f", int(bonesOk), int(dirOk),
        g_eyeForward, g_eyeUp, g_wristPitch * 57.29578f);
}


void Frame() {
    std::lock_guard lock(g_stateMutex);
    if (g_f1Key.Feed(VK_F1)) SetF1(!g_f1, "F1 key");
    if (g_f3Key.Feed(VK_F3)) SetF3(!g_f3, "F3 key");
    // F10 / F11 (owner): Xbox / PlayStation controller type for the VR controllers' virtual pad.
    static hotkeys::Edge f10, f11;
    if (f10.Feed(VK_F10)) vrinput::SetPadType(2);
    if (f11.Feed(VK_F11)) vrinput::SetPadType(5);
    if (!g_ready) return;
    const player::State st = player::Poll();
    // Right hand at the head + B / O (GoW D-252, playbook 28.2; vrinput HeadSwitch): while the player walks as
    // Jodie it switches F1, otherwise (a cutscene, a scripted scene) F3. Polled here, before any first-person code,
    // so it also turns first person ON from both modes off. It replaces the both-thumbsticks combo.
    // EXP-047 (owner: "when you press O it will switch off the first person Aiden possessions, like it does with
    // first person gameplay and cutscenes"): while Aiden possesses someone it switches "Aiden possessions (first
    // person)"; shown (option on, F1 on) -> off, otherwise on (with F1, which the possession view needs).
    if (vrinput::ConsumeHeadSwitch()) {
        if (st.menu) {
            // The main menu has no first person: the press would only change F1 / F3 unseen (LIVE 0.19.1: F3 on / off
            // twice at the main menu).
            Log("FP right hand at the head + B / O at the main menu: no first person there, F1 / F3 unchanged");
        } else if (st.possessing) {
            const bool show = !(player::PossessionFirstPerson() && g_f1);
            player::SetPossessionFirstPerson(show);
            if (show && !g_f1) SetF1(true, "right hand at the head + B / O, possession");
        } else if (st.control == player::Control::Walking) {
            SetF1(!g_f1, "right hand at the head + B / O, walking");
        } else {
            SetF3(!g_f3, "right hand at the head + B / O, cutscene");
        }
    }
    const int n = g_probeFrames.load();
    if (n > 0) {
        if (n % 90 == 0) {
            if (g_watchTpg) ProbeTpgObject(g_watchTpg);
            else if (g_countersOn) LogCounters();
            else ProbePlayer(st);
        }
        g_probeFrames--;
    }
}

std::atomic<bool> g_cutsceneFirstPerson{false}, g_firstPersonDrawn{false};
bool CutsceneFirstPerson() { return g_cutsceneFirstPerson.load(std::memory_order_relaxed); }
bool FirstPersonDrawn() { return g_firstPersonDrawn.load(std::memory_order_relaxed); }
bool EyeWorld(float out[3]) {
    std::lock_guard lock(g_stateMutex);
    if (!g_eyeWorldValid) return false;
    out[0] = g_eyeWorld[0];
    out[1] = g_eyeWorld[1];
    out[2] = g_eyeWorld[2];
    return true;
}


bool GameplayFP() { std::lock_guard lock(g_stateMutex); return g_f1; }
bool CutsceneFP() { std::lock_guard lock(g_stateMutex); return g_f3; }
void SetGameplayFP(bool on) { std::lock_guard lock(g_stateMutex); if (g_f1 != on) SetF1(on, "VR settings"); }
void SetCutsceneFP(bool on) { std::lock_guard lock(g_stateMutex); if (g_f3 != on) SetF3(on, "VR settings"); }
bool MotionControls() { std::lock_guard lock(g_stateMutex); return g_arms; }
void SetMotionControls(bool on) {
    std::lock_guard lock(g_stateMutex);
    if (g_arms == on) return;
    g_arms = on;
    Save(L"fparms", on ? L"on" : L"off");
    Log("FP motion controls (VR controllers move Jodie's arms) %s", on ? "on" : "off");
}
bool HandSmoothing() { std::lock_guard lock(g_stateMutex); return g_handSmoothing; }
void SetHandSmoothing(bool on) {
    std::lock_guard lock(g_stateMutex);
    if (g_handSmoothing == on) return;
    g_handSmoothing = on;
    Save(L"hand_smoothing", on ? L"on" : L"off");
    Log("FP hand smoothing (Jodie's hands with motion controls) %s", on ? "on" : "off");
}
bool PossessionFP() { return player::PossessionFirstPerson(); }
void SetPossessionFP(bool on) { player::SetPossessionFirstPerson(on); }
bool SnapTurn() { std::lock_guard lock(g_stateMutex); return g_snapTurn; }
void SetSnapTurn(bool on) {
    std::lock_guard lock(g_stateMutex);
    g_snapTurn = on;
    g_snap = {};
    Save(L"turn", on ? L"snap" : L"smooth");
    Log("FP turning in F1: %s", on ? "snap" : "smooth");
}
float SnapDegrees() { std::lock_guard lock(g_stateMutex); return g_snapStep * 57.29578f; }
void SetSnapDegrees(float degrees) {
    std::lock_guard lock(g_stateMutex);
    g_snapStep = std::clamp(degrees, 10.f, 90.f) * 0.017453293f;
    wchar_t v[16];
    swprintf_s(v, L"%.0f", g_snapStep * 57.29578f);
    Save(L"snap_turn_degrees", v);
    Log("FP snap angle %.0f deg", g_snapStep * 57.29578f);
}
float TurnSpeed() { std::lock_guard lock(g_stateMutex); return g_turnSpeed; }
void SetTurnSpeed(float fraction) {
    std::lock_guard lock(g_stateMutex);
    g_turnSpeed = std::clamp(fraction, 0.1f, 1.f);
    wchar_t v[16];
    swprintf_s(v, L"%.2f", g_turnSpeed);
    Save(L"turn_speed", v);
    Log("FP turning speed %.0f %%", g_turnSpeed * 100.f);
}

void Suspend() {
    std::lock_guard lock(g_stateMutex);
    g_cutsceneFirstPerson = false;
    g_firstPersonDrawn = false;
    g_baseValid = false;
    g_eyeWorldValid = false;
    g_cameraNode = nullptr;
    g_lastMode = fp::Mode::Cinematic;
    g_lastFrame = 0;
    fpbody::Suspend();
}

// F3: the view follows Jodie's head (yaw and pitch, no roll). headToView is the fixed rotation from
// her head bone's frame to "looking straight ahead", calibrated on upright walking frames (eyes level)
// or, before any walking, when a cutscene starts.
pose::Mat4 g_headToView = pose::Identity();
bool g_headToViewValid = false;
bool g_f3Provisional = false;        // calibrated from her body heading, not her eyes
float g_f3DriftTime = 0.f;           // seconds her face has been off the calibrated view
float g_f3YawOffTime = 0.f;          // seconds the calibrated head forward has been 50+ deg off her facing
// F3 up / down from her face (0.16.40): elevations of head bone -> between her eyes, per skeleton.
constexpr int kLevelSamples = 600;
float g_levelSamples[kLevelSamples] = {};
int g_levelCount = 0, g_levelNext = 0;
uint64_t g_levelSampleAt = 0;
void* g_levelSkeleton = nullptr;
int g_eyeStreak = 0;                 // consecutive level, steady eye frames (F3 calibration)
float g_eyeStreakLook = 0.f;
void* g_eyeCalSkeleton = nullptr;    // the skeleton g_eyeCal belongs to
pose::Mat4 g_eyeCal = pose::Identity();  // last calibration from her eyes
float g_viewPitch = 0.f;

pose::Mat4 RotationOnly(const pose::Mat4& m) {
    pose::Mat4 r = pose::Identity();
    for (int row = 0; row < 3; ++row) {
        const pose::Vec3 a = fp::Norm({m.m[row * 4], m.m[row * 4 + 1], m.m[row * 4 + 2]});
        r.m[row * 4] = a.x; r.m[row * 4 + 1] = a.y; r.m[row * 4 + 2] = a.z;
    }
    return r;
}
void CalibrateHeadToView(const pose::Mat4& head, float lookYaw) {
    pose::Mat4 inv;
    if (!fpbody::detail::AffineInverse(RotationOnly(head), inv)) return;
    g_headToView = pose::Mul(pose::FromPose(pose::YawQuat(lookYaw), {0, 0, 0}), inv);
    g_headToViewValid = true;
}

// The same, with the view also pitched (keeps the current up / down when redone mid-cutscene).
void CalibrateHeadToViewPitched(const pose::Mat4& head, float lookYaw, float pitch) {
    pose::Mat4 inv;
    if (!fpbody::detail::AffineInverse(RotationOnly(head), inv)) return;
    const pose::Quat pq{std::sin(pitch * 0.5f), 0.f, 0.f, std::cos(pitch * 0.5f)};
    g_headToView = pose::Mul(pose::Mul(pose::FromPose(pq, {0, 0, 0}), pose::FromPose(pose::YawQuat(lookYaw), {0, 0, 0})), inv);
    g_headToViewValid = true;
}

// EXP-054's guided-walk rule (stick + head movement, off since 0.21.7) and 0.16.20's interaction rule (her head
// 12 cm from standing within 8 s of a prompt) are gone (EXP-057, 0.23.0): the owner's marked run (2001 Navajo) had both
// walks after a prompt and the misplaced F3 explained by the engine's own state, which player::Poll now reads (an
// authored base-layer animation in her animation player, her physics controller, her motion kit).

void Prepare(void* cameraNode, const pose::Mat4& authoredCamera, const xr::FrameInput& input) {
    std::lock_guard lock(g_stateMutex);
    const uint64_t now = GetTickCount64();
    if (!g_ready || !cameraNode || !fp::ValidMatrix(authoredCamera)) {
        Suspend();
        return;
    }
    // Headset not focused (SteamVR overlay, Windows key): keep the last first-person view instead
    // of dropping to the game camera and re-entering afterwards (owner, 2026-10-08: the overlay
    // showed the non-F1 view, and F3 came back flipped). The body keeps its last plan.
    if (!xr::FrameInputUsable(input, now)) {
        if (!(g_baseValid && cameraNode == g_cameraNode && g_lastMode == fp::Mode::FirstPerson)) Suspend();
        return;
    }
    const player::State st = player::Poll();
    fp::BehaviourClass kind = fp::BehaviourClass::Unknown;
    // The main menu (owner: "it doesn't switch to first person during that, even with first person on"): the game
    // camera, whatever F1 / F3 say (player::State::menu).
    if (st.menu) kind = fp::BehaviourClass::Unknown;
    else if (st.control == player::Control::Walking) kind = fp::BehaviourClass::Gameplay;
    else if (st.control == player::Control::Cutscene) kind = fp::BehaviourClass::Cutscene;
    if (kind != fp::BehaviourClass::Cutscene) g_f3NeedsCalibration = true;  // also with F1 off
    else if (st.control == player::Control::Aiden) kind = fp::BehaviourClass::NativeFirstPerson;
    // EXP-047 (owner: "it should switch from following Jodie to following the possessed NPC"): with the possession
    // option on, F1 alone puts the view in the possessed body, also while the scripts animate it (cutscene rules:
    // the view on its animated head), not only while the player walks it.
    const fp::Mode mode = fp::Classify(kind, g_f1, g_f3 || (st.possessed && g_f1));
    {
        static int lastLogged = -1;
        const int state = int(kind) * 4 + int(mode) + (st.jodie ? 0 : 64);
        if (state != lastLogged) {
            lastLogged = state;
            Log("FP state: control=%s F1=%d F3=%d -> %s%s", player::Name(st.control),
                int(g_f1), int(g_f3),
                mode == fp::Mode::FirstPerson ? "first person" :
                mode == fp::Mode::NativeFirstPerson ? "game's own first person (Aiden)" : "game camera",
                !st.jodie ? " (Jodie not found yet)" : st.menu ? " (main menu)" : "");
        }
    }
    // Owner 0.16.12: in the DPA rooms the game cuts to a security-camera view with a filter while
    // Jodie stands still. While a camera filter is on, first person hands the view to the game
    // camera; it comes back by itself when the filter goes off. Owner 0.16.13: F3 too (LIVE: filter
    // `..._Entity2659_RT` on during a cutscene at 60.6-63.6 s, F3 stayed on).
    {
        static bool reported = false;
        const bool filtered = (kind == fp::BehaviourClass::Gameplay || kind == fp::BehaviourClass::Cutscene) &&
                              mode == fp::Mode::FirstPerson && camdirector::FilterActive();
        if (filtered != reported) {
            reported = filtered;
            Log(filtered ? "FP camera filter on: first person paused, game camera" : "FP camera filter off: first person resumes");
        }
        if (filtered) {
            Suspend();
            return;
        }
    }
    if (mode != fp::Mode::FirstPerson || !st.jodie) {
        Suspend();
        return;
    }
    void* node = bones::EntityNode(st.jodie);
    void* skeleton = SkeletonOf(node);
    if (!node || !skeleton) { Suspend(); return; }
    const bool newActor = node != g_actorNode || skeleton != g_actorSkeleton;
    if (newActor) {
        // EXP-046: which body this is (Jodie's model family, or a possessed body) decides the body rules.
        char why[256] = "a body Aiden possesses";
        const int family = st.possessed ? int(bodykind::Family::Other) : player::Family(st.jodie, why, sizeof(why));
        fpbody::Identify(skeleton, family, why);
        Log("FP first-person body %p: %s (%s)", st.jodie, bodykind::Name(static_cast<bodykind::Family>(family)), why);
    }
    fpbody::Watch(node, skeleton);
    if (newActor) {
        fpbody::Suspend();
        g_actorNode = node;
        g_actorSkeleton = skeleton;
        g_eyes.Reset(reinterpret_cast<uintptr_t>(skeleton));
        g_implausibleEyeFrames = 0;
        g_lastAnchor = 0;
        g_f3NeedsCalibration = true;  // EXP-047: another body (a possession): its own head -> view calibration
    }
    // Owner 0.16.11: F3 put the view inside a T-posed Jodie at a scene change (LIVE: rootYaw 0,
    // no skinning writes). Her bind pose means the game is not animating her: the game camera
    // takes over until she moves again, then first person resumes with a fresh calibration.
    {
        static bool reported = false;
        const bool tposed = fpbody::TPosed(node);
        if (tposed != reported) {
            reported = tposed;
            Log(tposed ? "FP Jodie is in her bind pose (T-pose): game camera until she is animated again"
                       : "FP Jodie is animated again: first person resumes");
        }
        if (tposed) {
            g_f3NeedsCalibration = true;
            Suspend();
            return;
        }
    }
    const bool entering = newActor || g_lastMode != mode || g_lastKind != kind || !g_baseValid;
    g_lastKind = kind;
    if (entering) g_viewYaw = fp::YawOf(authoredCamera);
    // High-resolution frame step (owner: body and hands "go back and forth" while turning with the
    // stick). GetTickCount64 ticks every ~15.6 ms against ~11 ms VR frames, so the stick turn
    // advanced 0 and then ~2 frames' worth on alternate frames.
    static LARGE_INTEGER qpcFreq{}, qpcLast{};
    if (!qpcFreq.QuadPart) QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    const float dt = g_lastFrame && qpcLast.QuadPart ?
        std::min(float(double(qpcNow.QuadPart - qpcLast.QuadPart) / double(qpcFreq.QuadPart)), 0.1f) : 0.f;
    qpcLast = qpcNow;
    g_lastFrame = now;
    // Right stick steers the detached view; HMD yaw remains an independent transform.
    // Owner 0.16.18 (young Jodie playing with a doll, white dots): "the view is ~45 deg too far left".
    // The right stick both turned the F1 view and did the white-dot gestures, so every gesture also
    // turned the view. While the game shows in-world prompts the stick is the game's only.
    const auto& right = input.hand[1];
    const bool promptsShowing = prompts::Showing(now);
    if (kind == fp::BehaviourClass::Gameplay && right.active && !promptsShowing) {
        if (!g_snapTurn) {
            if (std::fabs(right.stick[0]) > 0.25f) g_viewYaw -= right.stick[0] * 2.0943951f * g_turnSpeed * dt;
        } else if (const int step = g_snap.Step(right.stick[0], dt)) {
            g_viewYaw -= float(step) * g_snapStep;
        }
    } else {
        g_snap.Block();  // a stick held through prompts or a cutscene does not snap when it ends
    }
    {
        static bool reported = false;
        if (promptsShowing != reported && kind == fp::BehaviourClass::Gameplay) {
            reported = promptsShowing;
            Log(promptsShowing ? "FP prompts showing: right stick goes to the game only (no view turn)"
                               : "FP prompts gone: right stick turns the view again");
        }
    }

    pose::Mat4 head{}, root{};
    const bool cleanHead = fpbody::CleanHead(node, &head) || player::HeadWorld(st.jodie, head.m);
    const bool cleanRoot = fpbody::CleanRoot(node, &root) || player::BodyWorld(st.jodie, root.m);
    const bool anchors = cleanHead && cleanRoot && fp::ValidMatrix(head) && fp::ValidMatrix(root);
    if (anchors) {
        if (kind == fp::BehaviourClass::Gameplay) {
            // EXP-059: on foot or riding (her motion kit synced to the horse's), each with its own eye.
            if (g_eyes.Update(reinterpret_cast<uintptr_t>(skeleton), st.kitRead, reinterpret_cast<uintptr_t>(st.synced))) {
                if (g_eyes.Riding())
                    Log("FP eye: riding (motion kit synced to graph %p): the ride's own eye height%s", st.synced,
                        g_eyes.Ride().Ready() ? " (kept)" : " (learning)");
                else
                    Log("FP eye: on foot again (motion kit not synced): the on-foot eye height%s",
                        g_eyes.Foot().Ready() ? " (kept from before the ride)" : " (learning)");
            }
            const bool riding = g_eyes.Riding();
            fp::StableEye& eye = g_eyes.Active();
            // Calibrate on normal standing / walking frames only; after ~2.5 s without one
            // (a long crouch or climb) take what there is rather than stay in third person.
            // A rider sits on the horse: the standing range does not apply (her head ~2.3 m above her root; the scripts
            // give the horse's controller a 2.4 m height, SetHeight(2.4) in fControlHorse).
            const bool plausible = riding || fp::PlausibleStandingHead(head, root) || g_implausibleEyeFrames > 225;
            if (!plausible && !eye.Ready()) ++g_implausibleEyeFrames;
            const bool wasReady = eye.Ready();
            eye.Sample(reinterpret_cast<uintptr_t>(skeleton), head, root, plausible);
            if (!eye.HasSample()) { Suspend(); return; }
            const pose::Mat4 next = eye.Base(root, fp::YawOf(root), g_viewYaw, g_eyeForward, g_eyeUp);
            // Large authored moves start a fresh base rather than blending across a teleport.
            if (g_baseValid && !fp::PlausibleStep({g_base.m[12], g_base.m[13], g_base.m[14]},
                                                {next.m[12], next.m[13], next.m[14]}, 3.f)) {
                Log("FP eye relearned (%s): the view base jumped %.1f m in one frame", riding ? "riding" : "on foot",
                    fp::Len(fp::Sub({g_base.m[12], g_base.m[13], g_base.m[14]}, {next.m[12], next.m[13], next.m[14]})));
                eye.Reset(reinterpret_cast<uintptr_t>(skeleton));
                if (!riding) g_implausibleEyeFrames = 0;
                eye.Sample(reinterpret_cast<uintptr_t>(skeleton), head, root, riding || fp::PlausibleStandingHead(head, root));
                // No usable sample yet: an empty calibration would put the view at her feet.
                if (!eye.HasSample()) { Suspend(); return; }
            }
            if (!wasReady && eye.Ready())
                Log("FP eye height learned (%s): her head %.2f m above her root (%d frames)",
                    riding ? "riding" : "on foot", eye.Offset().y, eye.Samples());
            g_base = eye.Base(root, fp::YawOf(root), g_viewYaw, g_eyeForward, g_eyeUp);
            // Walking upright: her eyes look roughly level, so this is the head -> view calibration.
            pose::Vec3 eyeL{}, eyeR{};
            float face = 0.f;
            g_f3NeedsCalibration = true;  // the next cutscene starts with a fresh F3 calibration
            // (No head -> view calibration from walking: LIVE it came out 180 deg off for cutscenes,
            // owner 2026-10-08. F3 calibrates from the cutscene itself, on each entry.)
            (void)eyeL; (void)eyeR; (void)face;
        } else {
            // Cutscene: face where Jodie is looking (owner, 2026-10-06): head bone -> eye midpoint
            // of the face rig, smoothed (~0.15 s) so animation twitches do not shake the view.
            // Without eyes: her body heading. (The authored camera often looks AT her.)
            // Her front is the root's +Z (LIVE: head->eyes = root yaw + 185..190 deg, EXP-036 0.12.5, 0.16.9).
            const float bodyFront = std::remainder(fp::YawOf(root) + 3.14159265f + fp::HeadToEyesFlip(), 6.28318531f);
            float look = bodyFront, pitch = 0.f;
            pose::Vec3 eyeL{}, eyeR{};
            // 0.16.26 (owner 0.16.25: "when she turns her body my view turns, when she moves her head it
            // doesn't, up / down maybe"). The camera read her head / eyes at the skeleton build taken as
            // final; head turns added after it (look-at) only reach the array Havok skins, which the drawn
            // body already uses (RenderSource). Her facing (the eye line below) comes from that drawn copy
            // (one frame old). 0.16.27 (owner: "the view is a bit too high always" with 0.16.26 also moving
            // the camera and its pitch to the drawn head): position, pitch and calibration stay on the
            // built head as in 0.16.25; only the left / right facing uses the drawn eyes.
            const bool eyesRead = fpbody::CleanEyes(node, &eyeL, &eyeR);
            pose::Vec3 faceL = eyeL, faceR = eyeR;
            bool faceRead = eyesRead;
            // The head bone the face points are measured against (same copy as the eyes: built or drawn).
            pose::Vec3 faceHead{head.m[12], head.m[13], head.m[14]};
            {
                pose::Mat4 drawnHead{};
                pose::Vec3 drawnL{}, drawnR{};
                if (fpbody::RenderedHead(node, &drawnHead, &drawnL, &drawnR)) {
                    if (eyesRead) {
                        const pose::Vec3 a = fp::Sub(eyeR, eyeL), b = fp::Sub(drawnR, drawnL);
                        const float diff = std::remainder(std::atan2(-b.z, b.x) - std::atan2(-a.z, a.x), 6.28318531f);
                        static uint64_t lastDiffLog = 0;
                        if (std::fabs(diff) > 0.1745f && now - lastDiffLog >= 2000) {
                            lastDiffLog = now;
                            Log("FP F3 head turn after the animation build: drawn face %.0f deg from the built one", diff * 57.29578f);
                        }
                    }
                    faceL = drawnL;
                    faceR = drawnR;
                    faceHead = {drawnHead.m[12], drawnHead.m[13], drawnHead.m[14]};
                    faceRead = true;
                }
            }
            const bool eyes = eyesRead && fp::LookYaw({head.m[12], head.m[13], head.m[14]}, eyeL, eyeR, &look);
            if (eyes) look += fp::HeadToEyesFlip();
            // LIVE 0.16.9 (prologue, bald Jodie): on two F3 entries the eyes sat behind her head
            // (look = root -Z, 3 deg from it: the view faced backwards); a head turned more than
            // ~100 deg from her body is not a pose but stale face-rig data: use her body front.
            const bool eyesAgree = eyes && std::fabs(std::remainder(look - bodyFront, 6.28318531f)) < 1.75f;
            if (eyes && !eyesAgree) look = bodyFront;
            // Fresh head calibration every time F3 first person is entered (owner, 2026-10-08:
            // toggling F3 off/on reused a stale calibration -> flipped view). Entering covers F3
            // presses, new cutscenes and coming back from walking; a focus loss (overlay) no longer
            // exits first person, so it does not recalibrate.
            if (entering || g_f3NeedsCalibration) g_headToViewValid = false;
            g_f3NeedsCalibration = false;
            // Calibration history (all owner reports on young Jodie / bald Jodie):
            // - 2026-10-08: a stale calibration reused on F3 re-entry flipped the view -> calibrate per entry.
            // - 0.16.18: an entry with unusable eyes calibrated her body heading while her head was turned.
            // - 0.16.22: interaction entries calibrated on their first frame, where the face rig still held
            //   the previous pose (look=103 / rootYaw=-143 every time): 45 deg left.
            // - 0.16.23: correcting from the eyes every frame chased noise: looking down at the toys the
            //   head -> eyes vector is near vertical and its heading jumps (10 -> 14 -> 27 -> 93 -> 13 deg).
            // So: her eyes are trusted only on reliable frames (eyes near level, within 25 deg; 10 frames
            // in a row within 3 deg of each other; agreeing with her body front). A calibration from such
            // frames is kept for her skeleton (head bone -> face is fixed) and used at once on later entries;
            // the view then follows her head bone. Without one yet, her body heading is used provisionally.
            // A kept calibration is corrected only on reliable frames 15 deg off for 0.5 s.
            bool reliable = false;
            if (eyes && eyesAgree) {
                const pose::Vec3 mid{(eyeL.x + eyeR.x) * 0.5f - head.m[12], (eyeL.y + eyeR.y) * 0.5f - head.m[13],
                                     (eyeL.z + eyeR.z) * 0.5f - head.m[14]};
                const float len = fp::Len(mid);
                const bool level = len > 1e-3f && std::sqrt(mid.x * mid.x + mid.z * mid.z) >= 0.9f * len;
                const bool steady = level && std::fabs(std::remainder(look - g_eyeStreakLook, 6.28318531f)) < 0.0524f;
                g_eyeStreak = level ? (steady ? g_eyeStreak + 1 : 1) : 0;
                g_eyeStreakLook = look;
                reliable = g_eyeStreak >= 10;
            } else {
                g_eyeStreak = 0;
            }
            if (!g_headToViewValid) {
                if (g_eyeCalSkeleton == skeleton) {
                    g_headToView = g_eyeCal;
                    g_headToViewValid = true;
                    g_f3Provisional = false;
                    Log("FP F3 calibrated: her kept eye calibration (recenterEpoch=%u)", input.recenterEpoch);
                } else if (reliable) {
                    CalibrateHeadToView(head, look);
                    g_eyeCalSkeleton = skeleton;
                    g_eyeCal = g_headToView;
                    g_f3Provisional = false;
                    Log("FP F3 calibrated from her eyes: look=%.0f rootYaw=%.0f recenterEpoch=%u", look * 57.29578f,
                        fp::YawOf(root) * 57.29578f, input.recenterEpoch);
                } else {
                    CalibrateHeadToView(head, eyesAgree ? look : bodyFront);
                    g_f3Provisional = true;
                    Log("FP F3 calibrated provisionally (%s) look=%.0f rootYaw=%.0f recenterEpoch=%u",
                        eyesAgree ? "eyes not steady yet" : "eyes behind her head: body front",
                        (eyesAgree ? look : bodyFront) * 57.29578f, fp::YawOf(root) * 57.29578f, input.recenterEpoch);
                }
                g_f3DriftTime = 0.f;
            } else if (reliable) {
                const pose::Mat4 v = pose::Mul(g_headToView, RotationOnly(head));
                const float viewLook = std::atan2(v.m[8], v.m[10]);  // forward = -row2 -> atan2(-fx, -fz)
                const float drift = std::remainder(look - viewLook, 6.28318531f);
                g_f3DriftTime = g_f3Provisional || std::fabs(drift) > 0.2618f ? g_f3DriftTime + dt : 0.f;
                if (g_f3Provisional || g_f3DriftTime > 0.5f) {
                    CalibrateHeadToView(head, look);
                    g_eyeCalSkeleton = skeleton;
                    g_eyeCal = g_headToView;
                    Log("FP F3 %s from her eyes: look=%.0f (view was %.0f deg off)",
                        g_f3Provisional ? "calibrated" : "recalibrated", look * 57.29578f, drift * 57.29578f);
                    g_f3Provisional = false;
                    g_f3DriftTime = 0.f;
                }
            }
            // Owner: "the head doesn't align with the view Jodie is watching" -> follow the head's
            // pitch too: her head frame carried through the calibrated head -> view rotation.
            if (g_headToViewValid) {
                const pose::Mat4 view = pose::Mul(g_headToView, RotationOnly(head));
                const pose::Vec3 f{-view.m[8], -view.m[9], -view.m[10]};
                if (fp::Finite(f) && fp::Len(f) > 0.5f) {
                    look = std::atan2(-f.x, -f.z);
                    pitch = std::asin(std::clamp(f.y / fp::Len(f), -0.95f, 0.95f));
                }
            }
            // 0.16.25, left / right from her face itself (owner 0.16.24: "in the cutscene she turns her head
            // 90 deg left and in F3 I didn't see it"; white-dot animations still drift left). The head
            // bone (M_Head) is not her face on these rigs - the face is the LC_M_Head group, and LIVE the two
            // disagree ("eyes behind her head") - so a view carried by M_Head can miss a turn of the face. Her
            // facing is square to the line between her eyes: that line stays level when she looks down (where
            // head -> eyes went vertical and noisy) and needs no calibration. Which of its two sides is her
            // front comes from the face itself (fp::FaceFront, 0.17.1: "the side nearer the current view"
            // turned the view to her back once the view had drifted). Up / down still comes from the head bone.
            if (faceRead) {
                const float ref = entering ? look : g_viewYaw;
                float face = 0.f;
                bool fromFace = false;
                if (fp::FaceFront(faceL, faceR, faceHead, ref, bodyFront, &face, &fromFace)) {
                    look = face;
                    if (entering)
                        Log("FP F3 facing from her eye line: %.0f deg (%s; calibrated view %.0f)", look * 57.29578f,
                            fromFace ? "front from her face" : "side nearer the view", ref * 57.29578f);
                    static uint64_t lastFlipLog = 0;
                    if (!entering && std::fabs(std::remainder(look - g_viewYaw, 6.28318531f)) > 1.5708f &&
                        now - lastFlipLog >= 2000) {
                        lastFlipLog = now;
                        Log("FP F3 her face is %.0f deg from the view: the view turns to her front (the old rule kept her back)",
                            std::remainder(look - g_viewYaw, 6.28318531f) * 57.29578f);
                    }
                }
            }
            // 0.16.40 (owner on 0.16.34 - 0.16.39: up / down still inverted at times in F3). Up / down comes
            // from her face itself now: head bone -> the point between her eyes, made square to the line
            // between her eyes, is a fixed direction of her skull (head bone and eyes are rigid), so its
            // elevation goes down when she looks down, whatever the head -> view calibration says. (That
            // calibration's left / right part was provisional on every LIVE entry, off by up to 70 deg plus up
            // to 70 deg of look-at turn; a forward more than 90 deg off her face rose when she looked down.)
            // Its level value - a property of the rig - is the median of what has been seen on this skeleton
            // (5 samples a second, last 2 minutes; a face looks about level most of the time), seeded with
            // the value at the first entry. Without a face reading: the old head-bone pitch, sign-corrected.
            bool facePitch = false;
            if (faceRead) {
                const pose::Vec3 mid{(faceL.x + faceR.x) * 0.5f, (faceL.y + faceR.y) * 0.5f, (faceL.z + faceR.z) * 0.5f};
                const pose::Vec3 a = fp::Norm(fp::Sub(faceR, faceL));
                pose::Vec3 n = fp::Sub(mid, faceHead);
                n = fp::Sub(n, fp::Mul(a, fp::Dot(n, a)));
                const float nl = fp::Len(n), nflat = std::sqrt(n.x * n.x + n.z * n.z);
                // In front of her (stale face-rig data once put the eyes behind her head, 0.16.9).
                const bool front = fp::Finite(n) && nl > 0.01f && -n.x * std::sin(look) - n.z * std::cos(look) > 0.f;
                if (front) {
                    const float e = std::atan2(n.y, nflat);
                    if (g_levelSkeleton != skeleton) {
                        g_levelSkeleton = skeleton;
                        g_levelCount = 0;
                        g_levelNext = 0;
                        g_levelSampleAt = 0;
                    }
                    if (g_levelCount == 0 || now - g_levelSampleAt >= 200) {
                        g_levelSampleAt = now;
                        g_levelSamples[g_levelNext] = e;
                        g_levelNext = (g_levelNext + 1) % kLevelSamples;
                        g_levelCount = std::min(g_levelCount + 1, kLevelSamples);
                    }
                    float sorted[kLevelSamples];
                    std::copy(g_levelSamples, g_levelSamples + g_levelCount, sorted);
                    std::nth_element(sorted, sorted + g_levelCount / 2, sorted + g_levelCount);
                    const float level = sorted[g_levelCount / 2];
                    pitch = std::clamp(e - level, -1.25f, 1.25f);
                    facePitch = true;
                    static uint64_t lastLog = 0;
                    if (entering || now - lastLog >= 10000) {
                        lastLog = now;
                        Log("FP F3 up / down from her face: %.0f deg (face %.0f, level %.0f from %d samples)", pitch * 57.29578f,
                            e * 57.29578f, level * 57.29578f, g_levelCount);
                    }
                }
            }
            // 0.16.34 (owner on 0.16.33: "during F3 cutscenes sometimes the head up and down movement is
            // reversed"). Up / down is her head bone's forward through the head -> view calibration, whose
            // left / right part is provisional on most entries (LIVE 0.16.33: every entry provisional, up to
            // 70 deg from her eye line; look-at turns add up to 70 deg more). Turned more than 90 deg from
            // where she faces, that forward rises when she looks down. The elevation is now measured against
            // her facing (the direction above): its sign follows the side the forward points to, it holds while
            // the forward is nearly edge-on to her facing, and a calibration 50 deg or more off her facing for
            // 0.3 s is redone around the current pitch (no jump).
            if (entering) g_f3YawOffTime = 0.f;
            if (!facePitch && g_headToViewValid) {
                const pose::Mat4 view = pose::Mul(g_headToView, RotationOnly(head));
                const pose::Vec3 f{-view.m[8], -view.m[9], -view.m[10]};
                const float len = fp::Len(f), flat = std::sqrt(f.x * f.x + f.z * f.z);
                if (fp::Finite(f) && len > 0.5f && flat > 1e-3f) {
                    const float c = -f.x * std::sin(look) - f.z * std::cos(look);  // along her facing
                    pitch = std::fabs(c) >= 0.3f * len ? std::atan2(c < 0.f ? -f.y : f.y, std::fabs(c))
                                                       : (entering ? 0.f : g_viewPitch);
                    pitch = std::clamp(pitch, -1.25f, 1.25f);
                    g_f3YawOffTime = c < 0.643f * flat ? g_f3YawOffTime + dt : 0.f;  // cos 50 deg
                    if (g_f3YawOffTime > 0.3f) {
                        CalibrateHeadToViewPitched(head, look, pitch);
                        g_f3YawOffTime = 0.f;
                        static uint64_t lastLog = 0;
                        if (now - lastLog >= 2000) {
                            lastLog = now;
                            Log("FP F3 head forward %.0f deg off her facing: calibration redone (pitch %.0f deg kept)",
                                std::acos(std::clamp(c / flat, -1.f, 1.f)) * 57.29578f, pitch * 57.29578f);
                        }
                    }
                }
            }
            const float t = 1.f - std::exp(-dt / 0.15f);
            g_viewYaw = entering ? look : fp::ApproachAngle(g_viewYaw, look, t);
            g_viewPitch = entering ? pitch : g_viewPitch + (pitch - g_viewPitch) * t;
            g_base = fp::FirstPersonBase(head, g_viewYaw, g_eyeForward, g_eyeUp);
            // Pitch about the eye: v * Pitch * Yaw, keeping the eye position.
            const pose::Vec3 eye{g_base.m[12], g_base.m[13], g_base.m[14]};
            const pose::Quat pq{std::sin(g_viewPitch * 0.5f), 0.f, 0.f, std::cos(g_viewPitch * 0.5f)};
            g_base = pose::Mul(pose::FromPose(pq, {0, 0, 0}), pose::FromPose(pose::YawQuat(g_viewYaw), eye));
        }
        g_lastAnchor = now;
    } else if (!g_baseValid || !g_lastAnchor || now - g_lastAnchor > 500) {
        Suspend();
        return;
    }
    g_baseValid = true;
    g_cameraNode = cameraNode;
    g_lastMode = mode;
    fpbody::Plan plan{};
    plan.node = node;
    plan.skeleton = skeleton;
    plan.firstPerson = anchors;
    plan.recenterEpoch = input.recenterEpoch;
    plan.armsActive = g_arms && kind == fp::BehaviourClass::Gameplay && anchors;
    plan.turnBody = kind == fp::BehaviourClass::Gameplay;
    plan.moveStick = input.hand[0].active && std::hypot(input.hand[0].stick[0], input.hand[0].stick[1]) > 0.2f;
    {
        const int family = fp::HeadsetFamily(xr::SystemName());
        if (family != g_family) {
            g_family = family;
            Log("FP wrist angle for %s (OpenXR system \"%s\"): %.1f deg; hands %s", family == 0 ? "PS VR2" :
                family == 1 ? "Meta Quest" : "other headsets", xr::SystemName(), g_wristPitchFamily[family] * 57.29578f,
                g_handSmoothing ? "smoothed" : "raw");
        }
        plan.wristPitch = g_wristPitchFamily[family];
    }
    const pose::Mat4 headWorld = pose::Mul(input.headRel, g_base);
    g_eyeWorld[0] = headWorld.m[12];
    g_eyeWorld[1] = headWorld.m[13];
    g_eyeWorld[2] = headWorld.m[14];
    g_eyeWorldValid = fp::Finite({headWorld.m[12], headWorld.m[13], headWorld.m[14]});
    vrinput::Publish(input, kind == fp::BehaviourClass::Gameplay,
                     fp::YawOf(headWorld) - fp::YawOf(authoredCamera));
    // Smoothed body heading (owner: "stuttering of body when I turn"): the drawn body used to snap
    // to the head yaw every frame, and its pose is built on worker threads up to a frame behind the
    // camera, so turning made it judder against the view. Now: while walking it follows the view
    // smoothly (~0.12 s); standing, the head turns freely within +-35 deg and beyond that the body
    // catches up smoothly (the usual first-person / VRIK body follow).
    {
        static float bodyYaw = 0.f;
        static bool have = false;
        const float target = kind == fp::BehaviourClass::Gameplay ? fp::YawOf(headWorld) : fp::YawOf(root);
        if (!have || entering || kind != fp::BehaviourClass::Gameplay) {
            bodyYaw = target;
            have = true;
        } else {
            const float d = std::remainder(target - bodyYaw, 6.28318531f);
            const bool moving = std::fabs(input.hand[0].stick[0]) > 0.2f || std::fabs(input.hand[0].stick[1]) > 0.2f;
            if (moving || std::fabs(d) > 0.61f) {
                const float tau = moving ? 0.12f : 0.2f;
                bodyYaw = fp::ApproachAngle(bodyYaw, target, 1.f - std::exp(-dt / tau));
            }
        }
        plan.bodyYaw = bodyYaw;
    }
    plan.eyePivot = {g_base.m[12], g_base.m[13], g_base.m[14]};
    plan.headPos = {headWorld.m[12], headWorld.m[13], headWorld.m[14]};
    plan.headFwd = fp::Norm(pose::Vec3{-headWorld.m[8], -headWorld.m[9], -headWorld.m[10]});
    plan.eyeForward = g_eyeForward;
    plan.eyeUp = g_eyeUp;
    for (int i = 0; i < 2; ++i) {
        plan.handValid[i] = plan.armsActive && input.hand[i].active && input.hand[i].gripValid;
        pose::Mat4 grip = input.hand[i].gripRel;
        if (!plan.handValid[i] || !g_handSmoothing) g_handFilter[i].Reset();
        else grip = g_handFilter[i].Feed(grip, input.displayTime, input.recenterEpoch, g_handCutoff, g_handBeta);
        plan.handWorld[i] = pose::Mul(grip, g_base);
        plan.trigger[i] = input.hand[i].trigger;
        plan.grip[i] = input.hand[i].squeeze;
        // EXP-067: a hand reports finger touch once any of its touch sensors has said "touched" (a controller without
        // touch sensing never does and keeps the resting bases, playbook 14.12).
        static bool touchSeen[2]{};
        if (input.hand[i].triggerTouch || input.hand[i].thumbTouch) {
            if (!touchSeen[i])
                Log("FINGERS %s controller reports finger touch: point and fist follow the fingers, the thumb the grip",
                    i ? "right" : "left");
            touchSeen[i] = true;
        }
        plan.triggerTouch[i] = input.hand[i].triggerTouch;
        plan.thumbTouch[i] = input.hand[i].thumbTouch;
        plan.touchSeen[i] = touchSeen[i];
    }
    fpbody::Publish(plan);
    g_cutsceneFirstPerson = plan.firstPerson && !plan.turnBody;
    g_firstPersonDrawn = plan.firstPerson;
    // Once a second in first person: what the body edits did (EXP-035).
    static uint64_t lastDiag = 0;
    static uint64_t lastWrites = 0;
    if (now - lastDiag >= 1000) {
        lastDiag = now;
        const auto body = fpbody::ReadStatus();
        Log("FPDIAG control=%s arms=%d hands=%d,%d armState=%u,%u reach=%.2f/%.2f%s,%.2f/%.2f%s scale=%.2f wrist=%s%.0f,%s%.0f hidden=%u helpers=%u rigid=%llu cloth=%llu props=%llu writes/s=%llu failure=%u bodyYaw=%.0f headDy=%.2f bodyDy=%.2f idleHeld=%d",
            player::Name(st.control), int(plan.armsActive), int(plan.handValid[0]), int(plan.handValid[1]),
            body.armState[0], body.armState[1], body.armReach[0], body.armLength[0], body.armClamped[0] ? "!" : "",
            body.armReach[1], body.armLength[1], body.armClamped[1] ? "!" : "", body.armScale, body.handRetargeted[0] ? "" : "anim:", body.handTurn[0], body.handRetargeted[1] ? "" : "anim:",
            body.handTurn[1], body.hiddenBones, body.attachedBones, static_cast<unsigned long long>(body.attachedHidden),
            static_cast<unsigned long long>(body.clothWrites), static_cast<unsigned long long>(body.propDraws),
            static_cast<unsigned long long>(body.applyWrites - lastWrites), body.lastFailure, plan.bodyYaw * 57.29578f,
            body.headDeltaY, body.bodyShiftY, int(body.idleHeld));
        lastWrites = body.applyWrites;
    }
}

bool Base(void* cameraNode, pose::Mat4* out) {
    std::lock_guard lock(g_stateMutex);
    if (!out || !g_baseValid || cameraNode != g_cameraNode) return false;
    *out = g_base;
    return true;
}

void BeginRender() { std::lock_guard lock(g_stateMutex); fpbody::BeginRender(); }
void EndRender() { std::lock_guard lock(g_stateMutex); fpbody::EndRender(); }

void DescribeForCrash(char* out, size_t cap) {
    // Unlocked on purpose: the crashing thread may hold g_stateMutex; a torn read only mislabels.
    snprintf(out, cap, " F1 %s, F3 %s, arms %s, now %s", g_f1 ? "on" : "off", g_f3 ? "on" : "off",
             g_arms ? "on" : "off", g_lastMode == fp::Mode::FirstPerson ? "first person" : "game camera");
}

bool Command(const char* verb, const char* arg) {
    std::lock_guard lock(g_stateMutex);
    if (std::strcmp(verb, "fpstatus") == 0) {
        const auto st = player::Poll();
        const auto body = fpbody::ReadStatus();
        Log("FPSTATUS ready=%d foreground=%d F1=%d F3=%d arms=%d mode=%d base=%d control=%s jodie=%p camera=%p actor=%p skeleton=%p eyeSamples=%d viewYaw=%.1f",
            int(g_ready), int(GameForeground()), int(g_f1), int(g_f3), int(g_arms),
            int(g_lastMode), int(g_baseValid), player::Name(st.control), st.jodie,
            g_cameraNode, g_actorNode, g_actorSkeleton, int(g_eyes.Active().HasSample()), g_viewYaw * 57.29578f);
        Log("FPSTATUS body final=%llu capture=%llu writes=%llu failure=%u first=%d node=%p skeleton=%p worlds=%p count=%u head=%u yaw=%.1f->%.1f hands=%d,%d chains=%u/%u/%u,%u/%u/%u",
            body.selectedFinalCalls, body.captureValid, body.applyWrites, body.lastFailure,
            int(body.firstSuccess), reinterpret_cast<void*>(body.node), reinterpret_cast<void*>(body.skeleton),
            reinterpret_cast<void*>(body.worlds), body.bonesCount, body.lastHeadIndex,
            body.authoredRootYaw * 57.29578f, body.wantedBodyYaw * 57.29578f,
            int(body.handValid[0]), int(body.handValid[1]), body.upper[0], body.fore[0], body.hand[0],
            body.upper[1], body.fore[1], body.hand[1]);
        vrinput::LogStatus();
        return true;
    }
    if (std::strcmp(verb, "fp") == 0 || std::strcmp(verb, "fpcutscenes") == 0 ||
        std::strcmp(verb, "fparms") == 0) {
        bool* setting = std::strcmp(verb, "fp") == 0 ? &g_f1 :
                        std::strcmp(verb, "fparms") == 0 ? &g_arms : &g_f3;
        *setting = arg && std::strcmp(arg, "on") == 0;
        Log("FP %s=%d", verb, int(*setting));
        return true;
    }
    if (std::strcmp(verb, "fpprompts") == 0) {
        prompts::SetEnabled(!arg || std::strcmp(arg, "off") != 0);
        return true;
    }
    if (std::strcmp(verb, "fpflip") == 0) {
        fp::HeadToEyesFlipRef() = arg && std::atoi(arg) ? 3.14159265f : 0.f;
        Log("FP head->eyes flip: %d", arg && std::atoi(arg) ? 1 : 0);
        return true;
    }
    if (std::strcmp(verb, "fpbodyflip") == 0) {
        fp::BodyFlipRef() = arg && std::atoi(arg) ? 3.14159265f : 0.f;
        Log("FP body flip: %d", arg && std::atoi(arg) ? 1 : 0);
        return true;
    }
    if (std::strcmp(verb, "fpelems") == 0) {
        fpbody::DumpElements();
        Log("FPELEMS dump requested (next frame)");
        return true;
    }
    if (std::strcmp(verb, "fpbody") == 0) {
        const bool on = !arg || std::strcmp(arg, "off") != 0;
        fpbody::SetEdits(on);
        Log("FP body edits: %s", on ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpclothpose") == 0) {  // EXP-056 A/B: one cloth pose (default) or the earlier mix
        const bool one = !arg || std::strcmp(arg, "old") != 0;
        fpbody::SetClothOnePose(one);
        Log("FP cloth pose: %s", one ? "one physics pose for every cloth input (named rigs)" : "earlier mix");
        return true;
    }
    if (std::strcmp(verb, "fpitemcull") == 0) {  // 0.24.0 A/B: held items tested where drawn (on) or engine only (off)
        const bool on = !arg || std::strcmp(arg, "off") != 0;
        fpbody::SetItemCull(on);
        Log("FP held items in the engine's visibility: %s", on ? "also tested where they are drawn" : "engine result only");
        return true;
    }
    if (std::strcmp(verb, "fphead") == 0) {  // fphead show|hide|cutscene
        fpbody::SetHeadMode(arg && std::strcmp(arg, "show") == 0   ? fpbody::kHeadShown
                            : arg && std::strcmp(arg, "hide") == 0 ? fpbody::kHeadHidden
                                                                   : fpbody::kHeadHiddenInCutscenes);
        Log("FP head in first person: %s", fpbody::HeadModeName(fpbody::GetHeadMode()));
        return true;
    }
    if (std::strcmp(verb, "fpclothjump") == 0) {  // EXP-056 A/B: engine cloth teleport on first-person pose jumps
        const bool on = !arg || std::strcmp(arg, "off") != 0;
        fpbody::SetClothTeleports(on);
        Log("FP cloth teleport on pose jumps: %s", on ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpheadcloth") == 0) {  // 0.26.0 A/B: her hair cloth hidden with her head
        const bool on = !arg || std::strcmp(arg, "off") != 0;
        fpbody::SetHeadCloth(on);
        Log("FP hair cloth hidden with her head: %s", on ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fprestore") == 0) {
        const bool on = !arg || std::strcmp(arg, "off") != 0;
        fpbody::SetRestoreAtPublish(on);
        Log("FP restore before publish: %s", on ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpwatchbone") == 0 && arg) {
        fpbody::SetWatchBone(uint32_t(std::atoi(arg)));
        Log("FP watch bone index: %d", std::atoi(arg));
        return true;
    }
    if (std::strcmp(verb, "fpwatch") == 0) {
        void* addr[2] = {};
        const bool ok = fpbody::HeadWorldAddresses(addr) && watchpoint::Start(addr[0], addr[1], arg ? std::atoi(arg) : 5);
        Log("FP watch head bone: %s", ok ? "started" : "unavailable (no rig yet, or already running)");
        return true;
    }
    if (std::strcmp(verb, "fpeye") == 0 && arg) {
        // "fpeye <forward>" (metres); the up offset keeps its value. Both: b2svr.ini.
        g_eyeForward = std::clamp(static_cast<float>(std::atof(arg)), -0.3f, 0.4f);
        Log("FP eye forward=%.3f up=%.3f", g_eyeForward, g_eyeUp);
        return true;
    }
    if (std::strcmp(verb, "fpeyeup") == 0 && arg) {
        g_eyeUp = std::clamp(static_cast<float>(std::atof(arg)), -0.3f, 0.4f);
        Log("FP eye forward=%.3f up=%.3f", g_eyeForward, g_eyeUp);
        return true;
    }
    if (std::strcmp(verb, "fpwrist") == 0 && arg) {
        g_wristPitch = std::clamp(static_cast<float>(std::atof(arg)), -90.f, 90.f) * 0.017453293f;
        for (float& f : g_wristPitchFamily) f = g_wristPitch;
        Log("FP wrist pitch=%.1f deg (every headset family, until the next start)", g_wristPitch * 57.29578f);
        return true;
    }
    if (std::strcmp(verb, "turn") == 0 && arg) {
        // "turn smooth" / "turn snap [degrees]"
        g_snapTurn = std::strncmp(arg, "snap", 4) == 0;
        if (g_snapTurn && arg[4] == ' ') g_snapStep = std::clamp(float(std::atof(arg + 5)), 10.f, 90.f) * 0.017453293f;
        g_snap = {};
        Log("FP turning in F1: %s (%.0f deg steps)", g_snapTurn ? "snap" : "smooth", g_snapStep * 57.29578f);
        return true;
    }
    if (std::strcmp(verb, "fpcount") == 0) {
        if (!g_countersOn) InstallCounters();
        g_probeFrames = arg && std::atoi(arg) > 0 ? std::atoi(arg) : 900;
        return true;
    }
    if (std::strcmp(verb, "fpprobe") == 0) {
        g_probeFrames = arg && std::atoi(arg) > 0 ? std::atoi(arg) : 900;
        Log("FPPROBE armed for %d frames", g_probeFrames.load());
        return true;
    }
    if (std::strcmp(verb, "findvt") == 0 && arg) {
        g_findVt = std::strtoull(arg, nullptr, 16);
        if (HANDLE t = CreateThread(nullptr, 0, FindVtThread, nullptr, 0, nullptr)) CloseHandle(t);
        return true;
    }
    if (std::strcmp(verb, "tpgwatch") == 0 && arg) {
        g_watchTpg = reinterpret_cast<void*>(std::strtoull(arg, nullptr, 16));
        g_probeFrames = 90 * 600;
        Log("FPTPG watching %p", g_watchTpg);
        return true;
    }
    if (std::strcmp(verb, "peek") == 0 && arg) {
        char* end = nullptr;
        const uintptr_t addr = std::strtoull(arg, &end, 16);
        const int len = end && *end ? std::atoi(end) : 64;
        Peek(addr, len > 0 && len <= 4096 ? len : 64);
        return true;
    }
    return false;
}

}  // namespace b2s::fpcam
