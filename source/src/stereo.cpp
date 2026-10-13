#include "stereo.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string>
#include <utility>

#include "crashreport.h"
#include "game.h"
#include "hotkeys.h"
#include "fpcam.h"
#include "zonefix.h"
#include "fpbody.h"
#include "watchpoint.h"
#include "vrinput.h"
#include "videowatch.h"
#include "log.h"
#include "posemath.h"
#include "xr.h"

namespace b2s::stereo {

void InstallCullView();  // shared cull view (EXP-013), defined below
namespace {

using namespace b2s::game;

// Marker names must outlive the command buffer, so they are static strings.
constexpr int kMaxPasses = 3;  // 3 = diagnostic: a third recording exposes accumulating state
const char* const kEyeMarker[kMaxPasses] = {"B2SVR:EYE0", "B2SVR:EYE1", "B2SVR:EYE2"};
std::atomic<int> g_passes{2};

// Eye markers that also carry the XR frame slot: "B2SVR:EYE<eye>:<slot>".
struct XrMarkerNames {
    char names[2][xr::kSlots][16];
    char uiBegin[xr::kSlots][16];
    char uiEnd[xr::kSlots][16];
    XrMarkerNames() {
        for (int s = 0; s < xr::kSlots; ++s) {
            for (int e = 0; e < 2; ++e) snprintf(names[e][s], sizeof(names[e][s]), "B2SVR:EYE%d:%d", e, s);
            snprintf(uiBegin[s], sizeof(uiBegin[s]), "B2SVR:UIB:%d", s);
            snprintf(uiEnd[s], sizeof(uiEnd[s]), "B2SVR:UIE:%d", s);
        }
    }
};
const XrMarkerNames kXrMarkers;

// XR state for the pair being recorded (game thread) and the last located eyes.
std::atomic<int> g_curSlot{-1};
xr::EyeView g_xrEyes[2];
std::atomic<bool> g_haveXrEyes{false};
std::atomic<void*> g_mainCam{nullptr};
// Every camera of an enabled viewport in the recorded list (EXP-014: the menu's second viewport,
// a backdrop, stayed mono and screen-locked when only the first camera got the eye pose).
constexpr int kMaxStereoCams = 4;
void* g_stereoCams[kMaxStereoCams] = {};
int g_stereoCamCount = 0;
bool IsStereoCam(const void* cam) {
    for (int i = 0; i < g_stereoCamCount; ++i)
        if (g_stereoCams[i] == cam) return true;
    return false;
}
std::atomic<float> g_sceneAspect{2.3501f};
std::atomic<bool> g_xrWanted{false};  // b2svr.ini xr=on or the "xr on" verb
// XR frame whose UI (MenusMaster, drawn after VIEWPORT_LIST::Display) is lifted at Present.
std::atomic<int> g_pendingUiSlot{-1};
// Set by RecordList; a Present without it is a 2D-only frame (pause menu, splash, loading).
std::atomic<bool> g_recordedSincePresent{false};
// Stereo was switched on by a running XR session (not by the "stereo on" dev verb).
bool g_xrOwnsStereo = false;

std::atomic<bool> g_on{false};
std::atomic<bool> g_capture{true};  // "capture off": record per eye but emit no eye markers
std::atomic<bool> g_suppressParticles{false};
std::atomic<float> g_ipd{0.064f};
std::atomic<int> g_recordingEye{-1};  // game thread: eye being recorded, -1 when mono
std::atomic<uint64_t> g_eyeCopies[kMaxPasses] = {};
std::atomic<uint64_t> g_pairsRecorded{0};


// ---- engine command emission (game thread) ------------------------------------------

using AllocFn = uint8_t* (*)(void* cmdbuf, uint32_t type);
using CommitFn = void (*)(void* cmdbuf, void* rec);
using GrowFn = void (*)(void* array);
constexpr uint32_t kMarkerCmd = 0x1B;

// Mirrors the engine's own marker emission (VIEWPORT_LIST::Display): one string argument in
// the record's {ptr,count,cap} array at +0x08. The string is ours, so no arena accounting.
void EmitMarker(void* cmdbuf, const char* name) {
    auto alloc = reinterpret_cast<AllocFn>(Addr(Anchor::CmdAlloc));
    auto commit = reinterpret_cast<CommitFn>(Addr(Anchor::CmdCommit));
    auto grow = reinterpret_cast<GrowFn>(Addr(Anchor::ArrayGrow));
    uint8_t* rec = alloc(cmdbuf, kMarkerCmd);
    auto& count = *reinterpret_cast<uint32_t*>(rec + 0x10);
    const auto cap = *reinterpret_cast<uint32_t*>(rec + 0x14);
    if (count == cap) grow(rec + 0x08);
    auto** args = *reinterpret_cast<const char***>(rec + 0x08);
    args[count++] = name;
    commit(cmdbuf, rec);
}

// ---- game-thread hooks ------------------------------------------------------------------

using Display2DFn = void (*)(void* list, void* cmdbuf);
Display2DFn g_origDisplay2D = nullptr;
using ListStageFn = void (*)(void* list);
ListStageFn g_origPreParticles = nullptr, g_origPostParticles = nullptr;

void HookDisplay2D(void* list, void* cmdbuf) {
    const int eye = g_recordingEye;
    if (eye < 0) {
        g_origDisplay2D(list, cmdbuf);
        return;
    }
    // The 3D scene for this eye is complete in the command stream here.
    if (g_capture) {
        const int slot = g_curSlot;
        EmitMarker(cmdbuf, slot >= 0 && eye < 2 ? kXrMarkers.names[eye][slot] : kEyeMarker[eye]);
    }
    // UI is recorded once, with the last pass, so it reaches the flat window unchanged.
    if (eye == g_passes - 1) {
        g_origDisplay2D(list, cmdbuf);
        return;
    }
    // Display2D's first loop is per-viewport end-of-display cleanup (FUN_140867f30). It resets
    // per-recording state such as PROJECTOR_LIGHT+0x410 (views registered on a light); skipping it
    // made the next eye render every light twice (EXP-007/008 watchpoint). Run it for every pass.
    // Signature from the Display2D call site (0x14088873a): rcx=vp, rdx=cmdbuf, r8d=list index.
    using EndDisplayFn = void (*)(void* vp, void* cmdbuf, uint32_t index);
    auto endDisplay = reinterpret_cast<EndDisplayFn>(Addr(Anchor::ViewportEndDisplay));
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    for (uint32_t i = 0; handles && i < count; ++i) {
        void* vp = ResolveHandle(handles[i]);
        if (vp && (Read<uint32_t>(vp, off::VpFlags) & 1)) endDisplay(vp, cmdbuf, i);
    }
    // Second loop: reset each camera scene's light set (*(scene+0x190)+0x60). Without it the next
    // recording re-registered every light on top of this one (light binds 26 -> 52).
    using LightResetFn = void (*)(void* lightSet);
    auto lightReset = reinterpret_cast<LightResetFn>(Addr(Anchor::LightSetReset));
    for (uint32_t i = 0; handles && i < count; ++i) {
        void* vp = ResolveHandle(handles[i]);
        void* cam = vp ? ResolveHandle(Read<void*>(vp, off::VpCamera)) : nullptr;
        void* node = cam ? Read<void*>(cam, off::CamSceneNode) : nullptr;
        void* scene = node ? Read<void*>(node, 0x30) : nullptr;
        void* holder = scene ? Read<void*>(scene, 0x190) : nullptr;
        if (holder) lightReset(Read<void*>(holder, 0x60));
    }
}

// CLOSEUP_LIGHTING_MANAGER registers its rig lights into the scene on every viewport render
// setup, and the scene's light list is only reset once per frame, so each extra recording
// added the rig again (EXP-007: HDR light binds 26 -> 52 -> 72, face brighter per pass).
// Lights registered by the first pass stay valid for the frame, so later passes skip it.
using CloseupAttachFn = void (*)(void* manager, void* scene);
CloseupAttachFn g_origCloseupAttach = nullptr;
std::atomic<bool> g_fixCloseupLights{false};  // refuted premise (EXP-007); kept as a diagnostic toggle

std::atomic<bool> g_closeupOff{false};  // EXP-016: no close-up rig lights at all (discriminator)

// Diagnostic (owner, 2026-10-11: characters lose their shadows for a few seconds now and then):
// logs whenever the number of close-up rig lights the manager attaches changes, so a dropout can
// be matched to the rig (or ruled out) from the log. Same walk as FUN_140953d50: entries at
// +0x210 (stride 0x20), count at +0x218, light at entry+0x30.
void LogCloseupLightCount(void* manager) {
    static int last = -1, logs = 0;
    const auto* m = static_cast<const uint8_t*>(manager);
    const uint32_t n = *reinterpret_cast<const uint32_t*>(m + 0x218);
    const auto* entries = *reinterpret_cast<const uint8_t* const*>(m + 0x210);
    int lights = 0;
    for (uint32_t i = 0; entries && i < n && i < 64; ++i) {
        const auto* e = *reinterpret_cast<const uint8_t* const*>(entries + size_t(i) * 0x20);
        lights += e && *reinterpret_cast<void* const*>(e + 0x30);
    }
    if (lights != last && logs < 400) {
        ++logs;
        Log("CLOSEUP rig lights %d -> %d (slots %u)", last, lights, n);
        last = lights;
    }
}

void HookCloseupAttach(void* manager, void* scene) {
    if (g_recordingEye <= 0) LogCloseupLightCount(manager);
    if (g_closeupOff) return;
    // The close-up rig is placed relative to the shot camera. In F3 the view is at Jodie's eyes, so
    // the rig sat right beside the characters near her (owner, 2026-10-11: "characters near Jodie
    // get brighter and get more shadows, only in F3"). In F3 the scene's own lighting is used alone.
    if (fpcam::CutsceneFirstPerson()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("STEREO close-up character lights skipped while F3 first person is on");
        }
        return;
    }
    if (g_recordingEye >= 1 && g_fixCloseupLights) return;
    g_origCloseupAttach(manager, scene);
}

void HookPreParticles(void* list) {
    if (g_recordingEye >= 1 && g_suppressParticles) return;
    g_origPreParticles(list);
}

void HookPostParticles(void* list) {
    if (g_recordingEye >= 1 && g_suppressParticles) return;
    g_origPostParticles(list);
}

// ---- diagnostic: bracket VIEWPORT_LIST::Display's recording stages with markers -------
// "stagemarks on" emits B2SVR:STG:<rva> before each stage so a frametrace attributes every
// render-thread event to the game-thread stage that recorded it. Exact-build diagnostic:
// the addresses are only used after all guarded anchors matched (same .text).
constexpr uintptr_t kStageVa[] = {0x14088a0c0, 0x1408884f0, 0x140889ec0, 0x140889c60, 0x14088a170, 0x140889ba0, 0x140889e00, 0x140889d40, 0x140889ae0, 0x140889a20, 0x1408890e0, 0x140889860, 0x140889770, 0x1408895b0, 0x1408891a0, 0x140888dd0, 0x140888af0, 0x140888a00, 0x1408894e0, 0x140889410, 0x140888940, 0x140889340, 0x140888880};
constexpr int kStageCount = int(sizeof(kStageVa) / sizeof(kStageVa[0]));
const char* const kStageMarker[kStageCount] = {
    "B2SVR:STG:88a0c0",
    "B2SVR:STG:8884f0",
    "B2SVR:STG:889ec0",
    "B2SVR:STG:889c60",
    "B2SVR:STG:88a170",
    "B2SVR:STG:889ba0",
    "B2SVR:STG:889e00",
    "B2SVR:STG:889d40",
    "B2SVR:STG:889ae0",
    "B2SVR:STG:889a20",
    "B2SVR:STG:8890e0",
    "B2SVR:STG:889860",
    "B2SVR:STG:889770",
    "B2SVR:STG:8895b0",
    "B2SVR:STG:8891a0",
    "B2SVR:STG:888dd0",
    "B2SVR:STG:888af0",
    "B2SVR:STG:888a00",
    "B2SVR:STG:8894e0",
    "B2SVR:STG:889410",
    "B2SVR:STG:888940",
    "B2SVR:STG:889340",
    "B2SVR:STG:888880"};
using StageFn = void (*)(void*, void*, void*, void*);
StageFn g_origStage[kStageCount] = {};
std::atomic<bool> g_stageMarks{false};
std::atomic<void*> g_curCmdbuf{nullptr};

// EXP-016 discriminators: skip a list stage (bit per index in kStageVa).
std::atomic<uint32_t> g_skipStages{0};
constexpr int kStageReflection = 11;  // 0x140889860 "DisplayReflection"
constexpr int kStageMirrors = 13;     // 0x1408895b0 "DisplayMirrors"

template <int I>
void HookStage(void* a, void* b, void* c, void* d) {
    if (g_stageMarks && g_curCmdbuf) EmitMarker(g_curCmdbuf, kStageMarker[I]);
    if (g_skipStages.load() & (1u << I)) return;
    g_origStage[I](a, b, c, d);
}

template <int... I>
void HookAllStages(std::integer_sequence<int, I...>);

// ---- diagnostic: light renderer census per pass ("lightcensus <frames>") -------------------
using LightRenderFn = void (*)(void* scene, void* cmdbuf, uint32_t idx, void* vp);
using LightVolumeFn = void (*)(void* scene, void* cmdbuf, void* light, uint8_t flag);
LightRenderFn g_origLightRender = nullptr;
LightVolumeFn g_origLightVolume = nullptr;
std::atomic<int> g_lightCensusFrames{0};
std::atomic<int> g_volumeCalls{0};

std::wstring GameDir();

bool LightSafeRead(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Writes lights[0..3] (0x800) + their +0x240 job objects (0x300) before and after the render
// call into b2svr_lightdump.bin: u32 pass, u32 phase(0 before/1 after), then the bytes.
FILE* g_lightDump = nullptr;
void DumpLights(void* set, int pass, int phase) {
    if (!g_lightDump || !set) return;
    static uint8_t buf[4 * (0x800 + 0x300)];
    std::memset(buf, 0, sizeof(buf));
    void** arr = Read<void**>(set, 0x50);
    const uint32_t n = Read<uint32_t>(set, 0x58);
    for (uint32_t i = 0; arr && i < 4 && i < n; ++i) {
        void* light = arr[i];
        LightSafeRead(buf + i * 0xB00, light, 0x800);
        void* job = light ? Read<void*>(light, 0x240) : nullptr;
        if (job) LightSafeRead(buf + i * 0xB00 + 0x800, job, 0x300);
    }
    const uint32_t hdr[2] = {uint32_t(pass), uint32_t(phase)};
    fwrite(hdr, sizeof(hdr), 1, g_lightDump);
    fwrite(buf, sizeof(buf), 1, g_lightDump);
}

// ---- diagnostic: hardware write-watchpoint on PROJECTOR_LIGHT+0x410 ("watchlight") --------
// The game thread's DR0 is armed from a helper thread (a thread cannot reliably set its own
// debug registers); a vectored handler logs the writing instruction and its callers, then
// disarms after a few hits.
constexpr uintptr_t kProjectorLightVtableVa = 0x1417097c0;
std::atomic<bool> g_watchRequest{false};
std::atomic<uintptr_t> g_watchAddr{0};
std::atomic<DWORD> g_watchThread{0};
std::atomic<int> g_watchHits{0};

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = ModuleBase();
    const int hit = g_watchHits++;
    uint32_t value = 0;
    LightSafeRead(&value, reinterpret_cast<void*>(g_watchAddr.load()), 4);
    Log("WATCH hit=%d pass=%d rip=exe+0x%llx value=%u", hit, g_recordingEye.load(),
        static_cast<unsigned long long>(c->Rip - base), value);
    const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 10; ++i) {
        uintptr_t v = 0;
        if (!LightSafeRead(&v, sp + i, sizeof(v))) break;
        if (v > base + 0x1000 && v < base + 0x13bc000) {
            Log("WATCH   [rsp+0x%03x] exe+0x%llx", i * 8, static_cast<unsigned long long>(v - base));
            ++shown;
        }
    }
    c->Dr6 = 0;
    if (hit >= 7) c->Dr7 = 0;  // disarm
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI ArmWatch(LPVOID) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_watchThread);
    if (!th) return 0;
    SuspendThread(th);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(th, &c);
    c.Dr0 = g_watchAddr;
    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
    SetThreadContext(th, &c);
    ResumeThread(th);
    CloseHandle(th);
    Log("WATCH armed on thread %lu addr=%p", g_watchThread.load(), reinterpret_cast<void*>(g_watchAddr.load()));
    return 0;
}

void MaybeArmWatch(void* set) {
    if (!g_watchRequest || !set) return;
    void** arr = Read<void**>(set, 0x50);
    const uint32_t n = Read<uint32_t>(set, 0x58);
    const uintptr_t vt = game::Va(kProjectorLightVtableVa);
    for (uint32_t i = 0; arr && i < n; ++i) {
        if (arr[i] && *reinterpret_cast<uintptr_t*>(arr[i]) == vt) {
            g_watchRequest = false;
            g_watchAddr = reinterpret_cast<uintptr_t>(arr[i]) + 0x410;
            g_watchThread = GetCurrentThreadId();
            static bool vehAdded = false;
            if (!vehAdded) {
                AddVectoredExceptionHandler(1, WatchHandler);
                vehAdded = true;
            }
            if (HANDLE h = CreateThread(nullptr, 0, ArmWatch, nullptr, 0, nullptr)) CloseHandle(h);
            return;
        }
    }
}

// Diagnostic (owner, 2026-10-11: in F3 cutscenes a light keeps switching on and off on the characters
// near Jodie; LIVE 0.15.6 at those times the scene light list flips 13 <-> 14 with no camera cut).
// While F3 first person is drawn: every light that enters / leaves the scene's light list, with its
// class (RTTI), its node position (+0xb0, if it is a node) and its distance from the eye camera.
const char* RttiName(const void* obj, char* out, size_t n) {
    out[0] = 0;
    __try {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(obj);
        const auto* col = *reinterpret_cast<const uint32_t* const*>(vt - 8);
        const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
        size_t i = 0;
        for (; i + 1 < n && name[i]; ++i) out[i] = name[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    return out;
}

void LightMonitor(void* set, uint32_t lights) {
    static void* last[64]{};
    static uint32_t lastCount = 0;
    static ULONGLONG window = 0;
    static int lines = 0;
    void* now[64]{};
    uint32_t n = 0;
    void** arr = set ? Read<void**>(set, 0x50) : nullptr;
    for (uint32_t i = 0; arr && i < lights && n < 64; ++i) {
        void* l = nullptr;
        if (LightSafeRead(&l, &arr[i], sizeof(l)) && l) now[n++] = l;
    }
    const ULONGLONG t = GetTickCount64();
    if (t - window >= 1000) {
        window = t;
        lines = 0;
    }
    pose::Vec3 head{}, eye{}, fwd{};
    const bool haveEye = fpbody::PromptView(&head, &eye, &fwd);
    const float cam[3] = {eye.x, eye.y, eye.z};
    const bool haveCam = haveEye;
    auto report = [&](void* l, const char* what) {
        if (lines >= 12) return;
        ++lines;
        char cls[96];
        float p[3]{};
        const bool hasPos = LightSafeRead(p, static_cast<uint8_t*>(l) + 0xb0, 12) && std::isfinite(p[0]);
        const float d = hasPos && haveCam ? std::sqrt((p[0] - cam[0]) * (p[0] - cam[0]) + (p[1] - cam[1]) * (p[1] - cam[1]) +
                                                      (p[2] - cam[2]) * (p[2] - cam[2]))
                                          : -1.f;
        float ahead = 0.f;  // how far in front of the view (cosine of the angle to it)
        if (hasPos && haveCam && d > 1e-3f)
            ahead = ((p[0] - cam[0]) * fwd.x + (p[1] - cam[1]) * fwd.y + (p[2] - cam[2]) * fwd.z) / d;
        Log("LIGHTS F3: %s %p %s pos(+0xb0)=(%.2f %.2f %.2f) %.2f m from the eye, view cos %.2f (%u -> %u lights)", what, l,
            RttiName(l, cls, sizeof(cls)), p[0], p[1], p[2], d, ahead, lastCount, n);
    };
    for (uint32_t i = 0; i < n; ++i) {
        bool was = false;
        for (uint32_t k = 0; k < lastCount && !was; ++k) was = last[k] == now[i];
        if (!was && lastCount) report(now[i], "+");
    }
    for (uint32_t k = 0; k < lastCount; ++k) {
        bool is = false;
        for (uint32_t i = 0; i < n && !is; ++i) is = now[i] == last[k];
        if (!is) report(last[k], "-");
    }
    std::memcpy(last, now, sizeof(last));
    lastCount = n;
}

// Light list (owner, 2026-10-11: in F3 lights keep switching on / off on the characters near Jodie).
// STATIC: SCENE::BuildLightList (FUN_14089a900) queries the scene's lights per pass (FUN_140993ab0:
// published view 0x141c8f100 / projection 0x141c8f180, or the alternate set 0x141c8ef50 / 0x141c8ef90
// when the byte at 0x141c8ef3e is set; camera position; a 13 m range; a minimum screen size) and the
// query walks the scene by area, so a lamp's lights come and go together. LIVE 0.16.0: SPOT_LIGHTs
// and a group of 7 lights entering / leaving as the eye moves, no camera cut. In VR the published
// projection is one eye's own frustum, which moves with the head; the query now gets both eyes'
// frustum widened by `light_cull_margin` (b2svr.ini, default 1.35), the real projection back after.
using BuildLightListFn = void (*)(void*, void*, void*, void*, void*);
BuildLightListFn g_origBuildLights = nullptr;
std::atomic<float> g_lightCullMargin{1.35f};
void WideLightProjection(float* p) {
    const xr::EyeView &a = g_xrEyes[0], &b = g_xrEyes[1];
    const float m = g_lightCullMargin.load();
    const float l = std::min(a.tanLeft, b.tanLeft) * m, rt = std::max(a.tanRight, b.tanRight) * m;
    const float u = std::max(a.tanUp, b.tanUp) * m, d = std::min(a.tanDown, b.tanDown) * m;
    p[0] = 2.f / (rt - l);
    p[5] = 2.f / (u - d);
    p[8] = (rt + l) / (rt - l);
    p[9] = (u + d) / (u - d);
}
void HookBuildLights(void* scene, void* a2, void* vp, void* a4, void* a5) {
    const int eye = g_recordingEye.load();
    float* proj = reinterpret_cast<float*>(game::Va(0x141c8f180));
    float* alt = reinterpret_cast<float*>(game::Va(0x141c8ef90));
    static std::atomic<int> calls{0};
    if (calls.fetch_add(1) < 4)
        Log("STEREO light list build #%d: eye=%d thread=%lu xr=%d", calls.load(), eye, GetCurrentThreadId(),
            int(g_haveXrEyes.load()));
    // LIVE 0.16.2: never widened — the list is not always built inside an eye pass; in VR it is
    // widened wherever it is built (outside the passes the published camera is the left eye).
    if (!g_haveXrEyes || g_lightCullMargin.load() < 1.f || !proj) return g_origBuildLights(scene, a2, vp, a4, a5);
    float saved[16], savedAlt[16];
    std::memcpy(saved, proj, sizeof(saved));
    if (alt) std::memcpy(savedAlt, alt, sizeof(savedAlt));
    WideLightProjection(proj);
    if (alt) WideLightProjection(alt);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("STEREO light list built for both eyes' frustum x%.2f (lights no longer flip with head motion)",
            g_lightCullMargin.load());
    }
    g_origBuildLights(scene, a2, vp, a4, a5);
    std::memcpy(proj, saved, sizeof(saved));
    if (alt) std::memcpy(alt, savedAlt, sizeof(savedAlt));
}

// Light visibility, round 2 (owner: in F3 the characters' lighting / shadows change with where the
// game's camera looks). LIVE 0.16.3: SCENE::BuildLightList is never called; 0.16.4's hardware watch
// on the light list found the real path: VIEWPORT (FUN_14086b410) -> FUN_140867750 ->
// FUN_140994d40 -> FUN_140990c40 tests every light against the view's frustum planes and
// FUN_1409987d0 adds the survivors; FUN_140996f80 then picks the shadow-casting light from that list.
// The frustum is vp+0x3DD0 (VpSetup FUN_14086c760: FUN_140c21b20(frustum, projection, near, far)
// then FUN_140c21950(frustum, camera node world)). INFERENCE: in VR the player looks around, so a
// light beside / behind the eye that lights the characters in view is culled and comes back as the
// head moves. During FUN_140867750 only, vp+0x3DD0 becomes an all-around frustum: 152 deg wide with
// its apex `light_cull_back` (10 m) behind the eye, so every light near the player stays; the
// engine's own range / screen-size limits and its 30-light cap still apply. Restored right after
// (the frustum also rebuilds positions from depth, EXP-018).
using LightVisFn = void (*)(void*, void*, void*, void*, uint32_t, void*);
using FrustumFromProjFn = void (*)(float*, const float*, float, float);
using FrustumTransformFn = void (*)(float*, const float*);
LightVisFn g_origLightVis = nullptr;
FrustumFromProjFn g_frustumFromProj = nullptr;
FrustumTransformFn g_frustumTransform = nullptr;
std::atomic<float> g_lightCullBack{10.f};
constexpr size_t kVpFrustum = 0x3dd0, kFrustumBytes = 0xf0;

void* CachedHandleObject(void* handle, int bit) {
    __try {
        if (!handle) return nullptr;
        const uint32_t flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(handle) + 0x20);
        if (((flags >> bit) & 1) == 0) return nullptr;
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(handle) + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool AllAroundFrustum(uint8_t* vp, float* frustum) {
    if (!g_frustumFromProj || !g_frustumTransform) return false;
    void* cam = CachedHandleObject(Read<void*>(vp, 0x8b8), 29);
    void* node = cam ? CachedHandleObject(Read<void*>(cam, 0xa0), 21) : nullptr;
    if (!cam || !node) return false;
    alignas(16) float world[16];  // the engine's frustum code loads these with MOVAPS
    if (!LightSafeRead(world, static_cast<uint8_t*>(node) + 0x80, sizeof(world))) return false;
    alignas(16) float proj[16];
    if (!LightSafeRead(proj, static_cast<uint8_t*>(cam) + off::CamProj, sizeof(proj))) return false;
    constexpr float kTan = 4.f;  // ~152 deg each way
    proj[0] = 1.f / kTan;
    proj[5] = 1.f / kTan;
    proj[8] = 0.f;
    proj[9] = 0.f;
    // The camera looks along -Z of its node: the apex goes back along +Z.
    const float back = g_lightCullBack.load();
    world[12] += world[8] * back;
    world[13] += world[9] * back;
    world[14] += world[10] * back;
    alignas(16) float saved[kFrustumBytes / 4];
    std::memcpy(saved, frustum, sizeof(saved));
    if (reinterpret_cast<uintptr_t>(frustum) & 15) return false;
    g_frustumFromProj(frustum, proj, saved[0x38], saved[0x39] + back);
    g_frustumTransform(frustum, world);
    return true;
}

void HookLightVis(void* vp, void* scene, void* a3, void* a4, uint32_t a5, void* a6) {
    if (!g_haveXrEyes || g_lightCullBack.load() <= 0.f || !vp) return g_origLightVis(vp, scene, a3, a4, a5, a6);
    auto* frustum = reinterpret_cast<float*>(static_cast<uint8_t*>(vp) + kVpFrustum);
    // With the alternate camera set on, the function first copies +0x3DD0 to +0x4000: keep both.
    auto* copy = reinterpret_cast<float*>(static_cast<uint8_t*>(vp) + 0x4000);
    float saved[kFrustumBytes / 4], savedCopy[kFrustumBytes / 4];
    if (!LightSafeRead(saved, frustum, sizeof(saved)) || !LightSafeRead(savedCopy, copy, sizeof(savedCopy)))
        return g_origLightVis(vp, scene, a3, a4, a5, a6);
    const bool swapped = AllAroundFrustum(static_cast<uint8_t*>(vp), frustum);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("STEREO light visibility: all-around frustum (apex %.1f m behind the eye): %s", g_lightCullBack.load(),
            swapped ? "on" : "camera unavailable");
    }
    g_origLightVis(vp, scene, a3, a4, a5, a6);
    if (swapped) {
        std::memcpy(frustum, saved, sizeof(saved));
        std::memcpy(copy, savedCopy, sizeof(savedCopy));
    }
}

void HookLightRender(void* scene, void* cmdbuf, uint32_t idx, void* vp) {
    if (g_watchRequest) {
        void* holder = Read<void*>(scene, 0x190);
        MaybeArmWatch(holder ? Read<void*>(holder, 0x60) : nullptr);
    }
    const bool log = g_lightCensusFrames > 0;
    uint32_t count = 0;
    void* set = nullptr;
    if (log) {
        void* holder = Read<void*>(scene, 0x190);
        set = holder ? Read<void*>(holder, 0x60) : nullptr;
        count = set ? Read<uint32_t>(set, 0x58) : 0;
        g_volumeCalls = 0;
        DumpLights(set, g_recordingEye, 0);
    }
    const bool monitor = !log && g_recordingEye <= 0 && fpcam::CutsceneFirstPerson();
    if (monitor) {
        void* holder = Read<void*>(scene, 0x190);
        set = holder ? Read<void*>(holder, 0x60) : nullptr;
        count = set ? Read<uint32_t>(set, 0x58) : 0;
        LightMonitor(set, count);
        // (0.16.5-0.16.21 armed a hardware watch on the light list once per session, 3 s into F3, for
        // the lighting discovery (done: VIEWPORT::LightVisibility). LIVE 0.16.21 crash: a game thread
        // took the watch's last trap as it was being disarmed, 0x80000004 at exe+0x997073. Removed; the
        // watch is a dev verb only.)
    }
    g_origLightRender(scene, cmdbuf, idx, vp);
    if (log) DumpLights(set, g_recordingEye, 1);
    if (log)
        Log("LIGHTCENSUS pass=%d scene=%p vp=%p set=%p count=%u volumeDraws=%d", g_recordingEye.load(), scene, vp, set,
            count, g_volumeCalls.load());
}

void HookLightVolume(void* scene, void* cmdbuf, void* light, uint8_t flag) {
    g_volumeCalls++;
    g_origLightVolume(scene, cmdbuf, light, flag);
}

// ---- render-thread eye capture ---------------------------------------------------------

// Backbuffer, device and context are fetched on the game thread and cached: calling
// IDXGISwapChain::GetBuffer on the render thread while the game thread is inside Present
// deadlocked the game (EXP-007 run 5). D3D11 keeps buffer 0 pointing at the current back
// buffer, so the cached texture stays valid until ResizeBuffers.
std::atomic<IDXGISwapChain*> g_swapchain{nullptr};
std::mutex g_bbMutex;
ID3D11Texture2D* g_bb = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

// ---- Cinema letterbox (docs/CINEMA_BAND.md) ----------------------------------------------------
// ADDR-023 render-thread handler of engine command 1 "set cinema ratio" (table 0x1414f9d30[1]):
// DisplayCfg(0x141c8b278)+0xB880 = flag; +0x28 = +0x20 (scene width = backbuffer width);
// +0x2C = flag ? min(width * 0.4255, +0x24) : +0x24 (scene height). The guard covers the RIP
// displacement of DisplayCfg.
constexpr uintptr_t kCinemaHandlerVa = 0x1407e4750;
constexpr uint8_t kCinemaHandlerGuard[] = {
    0x8b, 0x4a, 0x18, 0x0f, 0x57, 0xc0, 0x8d, 0x41, 0x01, 0x89, 0x42, 0x18, 0x48, 0x8b, 0x42, 0x08, 0x48,
    0x8b, 0x15, 0x11, 0x6b, 0x4a, 0x01, 0x48, 0x8b, 0x0c, 0xc8, 0x0f, 0xb6, 0x01, 0x8b, 0x4a, 0x20, 0x89,
    0x4a, 0x28, 0x88, 0x82, 0x80, 0xb8, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x1e};
// ADDR-024 game-thread recorder of that command (cmdbuf, flag); the options loader calls it with
// *(RenderState+0x40) when runtime.ini UseCinemaRatio changes (cached at 0x141c7894f).
constexpr uintptr_t kCinemaSetVa = 0x1407e47c0;
constexpr uint8_t kCinemaSetGuard[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48,
                                       0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6,
                                       0xda, 0x48, 0x8b, 0xe9, 0xba, 0x01, 0x00, 0x00, 0x00, 0xe8};
constexpr uintptr_t kDisplayCfgVa = 0x141c8b278;
constexpr uintptr_t kCinemaCacheVa = 0x141c7894f;  // the game's own UseCinemaRatio value
constexpr uintptr_t kCinemaRenderStateVa = 0x141c78958;
constexpr uintptr_t kCfgBbHeight = 0x24, kCfgSceneWidth = 0x28, kCfgSceneHeight = 0x2C, kCfgCinema = 0xB880;
int g_cinemaOk = -1;                   // -1 unchecked, 0 guard mismatch, 1 ok
// Default 1: switching the letterbox off at runtime leaves the engine's scene render targets
// at the band height; the bottom quarter of the eye then smears (HEADSET FAIL 0.4.3,
// CINEMA_BAND.md). 0 stays available as a dev verb until the target owner is found.
std::atomic<int> g_cinemaMode{1};      // 0 = off in VR (full-height eyes), 1 = keep the game's
int g_cinemaForced = -1;               // flag value B2SVR last requested, -1 none

uint8_t* DisplayCfg() {
    return *reinterpret_cast<uint8_t**>(game::Va(kDisplayCfgVa));
}

// Game thread, while recording a frame: in VR the scene fills the whole backbuffer height
// (the letterbox band wastes ~25% of the eye's vertical resolution); outside VR the game's own
// UseCinemaRatio value is restored.
void UpdateCinema(bool vr) {
    if (g_cinemaOk < 0) {
        g_cinemaOk = game::GuardAt(game::Va(kCinemaHandlerVa), kCinemaHandlerGuard, sizeof(kCinemaHandlerGuard)) &&
                     game::GuardAt(game::Va(kCinemaSetVa), kCinemaSetGuard, sizeof(kCinemaSetGuard));
        Log("CINEMA guards %s", g_cinemaOk ? "OK" : "MISMATCH (letterbox left to the game)");
    }
    if (!g_cinemaOk) return;
    uint8_t* cfg = DisplayCfg();
    void* rs = *reinterpret_cast<void**>(game::Va(kCinemaRenderStateVa));
    if (!cfg || !rs) return;
    const uint8_t game = *reinterpret_cast<uint8_t*>(game::Va(kCinemaCacheVa));
    int want = -1;
    if (vr && g_cinemaMode == 0) want = 0;
    else if (g_cinemaForced >= 0) want = game;  // hand the game's value back once
    if (want < 0) return;
    if (cfg[kCfgCinema] == uint8_t(want)) {
        if (want == int(game) && !(vr && g_cinemaMode == 0)) g_cinemaForced = -1;
        return;
    }
    if (g_cinemaForced == want) return;  // already recorded; waiting for the render thread
    using SetFn = void (*)(void* cmdbuf, uint8_t flag);
    reinterpret_cast<SetFn>(game::Va(kCinemaSetVa))(*reinterpret_cast<void**>(static_cast<uint8_t*>(rs) + 0x40),
                                                 uint8_t(want));
    g_cinemaForced = want;
    Log("CINEMA letterbox %s (scene %ux%u, game setting %u)", want ? "on" : "off",
        *reinterpret_cast<uint32_t*>(cfg + kCfgSceneWidth), *reinterpret_cast<uint32_t*>(cfg + kCfgSceneHeight),
        unsigned(game));
}

// Rows [top, top + height) of a backbuffer of `bbHeight` rows that hold the 3D scene.
void SceneBand(uint32_t bbWidth, uint32_t bbHeight, uint32_t& top, uint32_t& height) {
    if (g_cinemaOk == 1) {
        if (const uint8_t* cfg = DisplayCfg()) {
            const uint32_t w = *reinterpret_cast<const uint32_t*>(cfg + kCfgSceneWidth);
            const uint32_t h = *reinterpret_cast<const uint32_t*>(cfg + kCfgSceneHeight);
            const uint32_t bh = *reinterpret_cast<const uint32_t*>(cfg + kCfgBbHeight);
            if (w == bbWidth && bh == bbHeight && h > 0 && h <= bbHeight) {
                top = (bbHeight - h) / 2;
                height = h;
                return;
            }
        }
    }
    // Fallback: the scene camera's aspect (cinema band centred).
    const float aspect = g_sceneAspect.load();
    height = aspect > 0.f ? uint32_t(float(bbWidth) / aspect + 0.5f) : bbHeight;
    if (height > bbHeight) height = bbHeight;
    top = (bbHeight - height) / 2;
}

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ResizeBuffersFn g_origResizeBuffers = nullptr;

void ReleaseCachedTargets() {
    std::lock_guard lock(g_bbMutex);
    if (g_bb) g_bb->Release();
    g_bb = nullptr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    ReleaseCachedTargets();
    Log("STEREO ResizeBuffers %ux%u", w, h);
    return g_origResizeBuffers(sc, n, w, h, f, flags);
}
ID3D11Texture2D* g_eyeTex[kMaxPasses] = {};
D3D11_TEXTURE2D_DESC g_eyeDesc{};
std::mutex g_shotMutex;
std::string g_shotName;  // non-empty: save both eyes after the next EYE1 copy

std::wstring GameDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/"));
}

void SaveTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& name) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) return;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const UINT scale = 2, w = d.Width / scale, h = d.Height / scale;
        const std::wstring path = GameDir() + L"\\" + std::wstring(name.begin(), name.end()) + L".bmp";
        if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR)) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            const UINT stride = (w * 3 + 3) & ~3u;
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(w);
            ih.biHeight = -LONG(h);
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + stride * h;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            std::string row(stride, '\0');
            for (UINT y = 0; y < h; ++y) {
                const uint8_t* src = static_cast<uint8_t*>(m.pData) + size_t(y) * scale * m.RowPitch;
                for (UINT x = 0; x < w; ++x) {
                    const uint8_t* p = src + size_t(x) * scale * 4;  // R8G8B8A8
                    row[x * 3 + 0] = char(p[2]);
                    row[x * 3 + 1] = char(p[1]);
                    row[x * 3 + 2] = char(p[0]);
                }
                fwrite(row.data(), 1, stride, f);
            }
            fclose(f);
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    Log("STEREO saved %s", name.c_str());
}

// EXP-024 `lumtrace <frames>`: per eye and frame, mean luminance of a 4x4 grid of the eye image
// (staging readback, sampled every 16th pixel). Detects periodic one-eye lighting flips.
std::atomic<int> g_lumFrames{0};
ID3D11Texture2D* g_lumStaging = nullptr;

void LumTrace(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, int eye) {
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (g_lumStaging) {
        D3D11_TEXTURE2D_DESC sd{};
        g_lumStaging->GetDesc(&sd);
        if (sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format) {
            g_lumStaging->Release();
            g_lumStaging = nullptr;
        }
    }
    if (!g_lumStaging) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_lumStaging))) return;
    }
    ctx->CopyResource(g_lumStaging, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_lumStaging, 0, D3D11_MAP_READ, 0, &m))) return;
    double cell[16] = {};
    int cnt[16] = {};
    for (UINT y = 0; y < d.Height; y += 16) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        const int cy = int(y * 4 / d.Height);
        for (UINT x = 0; x < d.Width; x += 16) {
            const uint8_t* p = row + size_t(x) * 4;
            const int c = cy * 4 + int(x * 4 / d.Width);
            cell[c] += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            cnt[c]++;
        }
    }
    ctx->Unmap(g_lumStaging, 0);
    char line[256];
    int n = snprintf(line, sizeof(line), "LUM %c", eye == 0 ? 'L' : 'R');
    for (int i = 0; i < 16; ++i) n += snprintf(line + n, sizeof(line) - n, " %.1f", cnt[i] ? cell[i] / cnt[i] : 0.0);
    Log("%s", line);
    if (eye == 1) g_lumFrames--;
}

void CopyEye(int eye, int slot) {
    std::lock_guard lock(g_bbMutex);
    ID3D11Texture2D* bb = g_bb;
    ID3D11Device* dev = g_dev;
    ID3D11DeviceContext* ctx = g_ctx;
    if (!bb || !dev || !ctx) return;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    if (d.Width != g_eyeDesc.Width || d.Height != g_eyeDesc.Height || d.Format != g_eyeDesc.Format) {
        for (auto*& t : g_eyeTex) {
            if (t) t->Release();
            t = nullptr;
        }
        g_eyeDesc = d;
        D3D11_TEXTURE2D_DESC ed = d;
        ed.Usage = D3D11_USAGE_DEFAULT;
        ed.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ed.CPUAccessFlags = 0;
        ed.MiscFlags = 0;
        for (auto*& t : g_eyeTex) dev->CreateTexture2D(&ed, nullptr, &t);
        Log("STEREO eye textures %ux%u fmt=%d", d.Width, d.Height, int(d.Format));
    }
    if (g_eyeTex[eye]) ctx->CopyResource(g_eyeTex[eye], bb);
    if (g_lumFrames > 0 && g_eyeTex[eye] && eye <= 1) LumTrace(dev, ctx, g_eyeTex[eye], eye);
    if (slot >= 0 && eye < 2 && g_eyeTex[eye]) {
        // The 3D scene occupies a centred band of the backbuffer (full height with the
        // letterbox off in VR).
        uint32_t top = 0, h = d.Height;
        SceneBand(d.Width, d.Height, top, h);
        xr::OnEye(eye, slot, ctx, g_eyeTex[eye], top, h, /*uiFollows=*/eye == 1 && g_capture);
    }
    const uint64_t n = g_eyeCopies[eye]++;
    if (n < 4) Log("STEREO copied eye %d (#%llu) on thread %lu", eye, n, GetCurrentThreadId());
    if (eye == g_passes - 1) {
        std::string shot;
        {
            std::lock_guard shotLock(g_shotMutex);
            shot.swap(g_shotName);
        }
        if (!shot.empty()) {
            SaveTexture(dev, ctx, g_eyeTex[0], shot + "_L");
            SaveTexture(dev, ctx, g_eyeTex[1], shot + "_R");
            if (g_passes > 2) SaveTexture(dev, ctx, g_eyeTex[2], shot + "_P2");
        }
    }
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %-24s %p %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

template <int... I>
void HookAllStages(std::integer_sequence<int, I...>) {
    (Hook(reinterpret_cast<void*>(game::Va(kStageVa[I])), reinterpret_cast<void*>(&HookStage<I>),
          reinterpret_cast<void**>(&g_origStage[I]), kStageMarker[I]),
     ...);
}

}  // namespace

void Install() {
    // b2svr.ini next to the game: "xr=on" starts the OpenXR session automatically at launch.
    {
        wchar_t ini[MAX_PATH];
        const std::wstring path = GameDir() + L"\\b2svr.ini";
        GetPrivateProfileStringW(L"b2svr", L"xr", L"off", ini, MAX_PATH, path.c_str());
        if (_wcsicmp(ini, L"on") == 0) {
            g_xrWanted = true;
            Log("b2svr.ini: xr=on (session starts on the first rendered frame)");
        }
    }
    HookAllStages(std::make_integer_sequence<int, kStageCount>{});
    {
        const uint8_t kRenderGuard[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x44, 0x89, 0x44};
        const uint8_t kVolumeGuard[] = {0x48, 0x89, 0x5c, 0x24, 0x08};
        auto* r = reinterpret_cast<uint8_t*>(game::Va(0x140897780));
        auto* v = reinterpret_cast<uint8_t*>(game::Va(0x1408955c0));
        if (game::GuardAt(reinterpret_cast<uintptr_t>(r), kRenderGuard, sizeof(kRenderGuard)))
            Hook(r, reinterpret_cast<void*>(&HookLightRender), reinterpret_cast<void**>(&g_origLightRender),
                 "SCENE::RenderLights");
        if (game::GuardAt(reinterpret_cast<uintptr_t>(v), kVolumeGuard, sizeof(kVolumeGuard)))
            Hook(v, reinterpret_cast<void*>(&HookLightVolume), reinterpret_cast<void**>(&g_origLightVolume),
                 "SCENE::DrawLightVolume");
        wchar_t margin[16];
        GetPrivateProfileStringW(L"b2svr", L"light_cull_margin", L"1.35", margin, 16, (GameDir() + L"\\b2svr.ini").c_str());
        const float mv = static_cast<float>(_wtof(margin));
        g_lightCullMargin = std::isfinite(mv) && mv >= 0.f && mv <= 3.f ? mv : 1.35f;
        GetPrivateProfileStringW(L"b2svr", L"light_cull_back", L"10", margin, 16, (GameDir() + L"\\b2svr.ini").c_str());
        const float back = static_cast<float>(_wtof(margin));
        g_lightCullBack = std::isfinite(back) && back >= 0.f && back <= 100.f ? back : 10.f;
        g_frustumFromProj = reinterpret_cast<FrustumFromProjFn>(game::Va(0x140c21b20));
        g_frustumTransform = reinterpret_cast<FrustumTransformFn>(game::Va(0x140c21950));
        if (void* lv = reinterpret_cast<void*>(game::Va(0x140867750)))
            Hook(static_cast<uint8_t*>(lv), reinterpret_cast<void*>(&HookLightVis), reinterpret_cast<void**>(&g_origLightVis),
                 "VIEWPORT::LightVisibility");
        if (void* b = reinterpret_cast<void*>(game::Va(0x14089a900)))
            Hook(static_cast<uint8_t*>(b), reinterpret_cast<void*>(&HookBuildLights),
                 reinterpret_cast<void**>(&g_origBuildLights), "SCENE::BuildLightList");
    }
    Hook(Addr(Anchor::ListDisplay2D), reinterpret_cast<void*>(&HookDisplay2D),
         reinterpret_cast<void**>(&g_origDisplay2D), "VIEWPORT_LIST::Display2D");
    videowatch::Install();  // full-screen videos in the 2D layer (xr::OnUiEnd)
    InstallCullView();
    fpcam::Install();
    Hook(Addr(Anchor::CloseupLightsAttach), reinterpret_cast<void*>(&HookCloseupAttach),
         reinterpret_cast<void**>(&g_origCloseupAttach), "CLOSEUP_LIGHTING::Attach");
    Hook(Addr(Anchor::PreUpdateParticles), reinterpret_cast<void*>(&HookPreParticles),
         reinterpret_cast<void**>(&g_origPreParticles), "VIEWPORT_LIST::PreUpdParts");
    Hook(Addr(Anchor::PostUpdateParticles), reinterpret_cast<void*>(&HookPostParticles),
         reinterpret_cast<void**>(&g_origPostParticles), "VIEWPORT_LIST::PostUpdParts");
}

// ---- diagnostic: snapshot engine state around each pass (find accumulators) ----------
std::atomic<bool> g_diffDump{false};
constexpr uintptr_t kRenderStateVa = 0x141c78958;
constexpr uintptr_t kDataWinVa = 0x141c78000, kDataWinSize = 0x3000;
constexpr size_t kRenderStateSize = 0x2000;

bool SafeRead(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(dst, 0, n);
        return false;
    }
}

constexpr size_t kObjSize = 0x4400;  // VIEWPORT is 0x41F0; scene size unknown

// Region order per snapshot: render state, .data window, 3D viewport, its camera's scene.
void Snapshot(FILE* f, uint32_t stage, void* list) {
    void* rs = *reinterpret_cast<void**>(game::Va(kRenderStateVa));
    void* vp3d = nullptr;
    void* scene = nullptr;
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    for (uint32_t i = 0; handles && i < count && !vp3d; ++i) {
        void* vp = ResolveHandle(handles[i]);
        void* cam = vp ? ResolveHandle(Read<void*>(vp, off::VpCamera)) : nullptr;
        if (!cam) continue;
        vp3d = vp;
        if (void* node = Read<void*>(cam, off::CamSceneNode)) scene = Read<void*>(node, 0x30);
    }
    static uint8_t buf[kRenderStateSize + kDataWinSize + 2 * kObjSize];
    SafeRead(buf, rs, kRenderStateSize);
    SafeRead(buf + kRenderStateSize, reinterpret_cast<void*>(game::Va(kDataWinVa)), kDataWinSize);
    SafeRead(buf + kRenderStateSize + kDataWinSize, vp3d, kObjSize);
    // Second object region: light holder (scene+0x190) 0x800 bytes, then scene 0x3C00.
    // Second object region: 4 light objects (0x800 each) from the scene's light set, then
    // each light's +0x240 job object (0x300 each).
    void* holder = scene ? Read<void*>(scene, 0x190) : nullptr;
    void* set = holder ? Read<void*>(holder, 0x60) : nullptr;
    void** lightArr = set ? Read<void**>(set, 0x50) : nullptr;
    const uint32_t nLights = set ? Read<uint32_t>(set, 0x58) : 0;
    uint8_t* objB = buf + kRenderStateSize + kDataWinSize + kObjSize;
    std::memset(objB, 0, kObjSize);
    for (uint32_t i = 0; lightArr && i < 4 && i < nLights; ++i) {
        void* light = nullptr;
        SafeRead(&light, lightArr + i, sizeof(light));
        if (!light) continue;
        SafeRead(objB + i * 0x800, light, 0x800);
        void* job = nullptr;
        SafeRead(&job, static_cast<uint8_t*>(light) + 0x240, sizeof(job));
        if (job) SafeRead(objB + 0x2000 + i * 0x300, job, 0x300);
    }
    fwrite(&stage, 4, 1, f);
    fwrite(buf, sizeof(buf), 1, f);
    if (stage == 0) Log("STEREO snapshot regions: rs=%p vp3d=%p holder=%p scene=%p", rs, vp3d, holder, scene);
}

// ---- per-pass light list restore ----------------------------------------------------------
// Each recording's visibility stages append the scene's visible lights to
// *(*(scene+0x190)+0x60)+0x50 {ptr, count@+0x58}, which is only reset once per frame, so the
// second eye rendered both eyes' lights (EXP-007: light binds 14 -> 28 in FUN_140888dd0).
// Restoring the count before each extra pass makes every eye gather its own visible lights.
constexpr uintptr_t kSceneLightsHolder = 0x190, kHolderLightSet = 0x60, kLightSetCount = 0x58;
std::atomic<bool> g_lightFix{false};  // superseded by the Display2D cleanup replay (EXP-008)
std::atomic<int> g_lightLogBudget{12};

uint32_t* SceneLightCount(void* list) {
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    for (uint32_t i = 0; handles && i < count; ++i) {
        void* vp = ResolveHandle(handles[i]);
        void* cam = vp ? ResolveHandle(Read<void*>(vp, off::VpCamera)) : nullptr;
        void* node = cam ? Read<void*>(cam, off::CamSceneNode) : nullptr;
        void* scene = node ? Read<void*>(node, 0x30) : nullptr;
        void* holder = scene ? Read<void*>(scene, kSceneLightsHolder) : nullptr;
        void* set = holder ? Read<void*>(holder, kHolderLightSet) : nullptr;
        if (set) return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(set) + kLightSetCount);
    }
    return nullptr;
}

void RestoreNode();  // eye pose on the camera node, defined below
std::atomic<bool> g_prime{true};  // EXP-022
std::atomic<bool> g_tailLeft{false};  // EXP-026: refuted (owner: still unstable), toggle kept
void RestoreEngineFov();
void PrimeEyeCamera();
void PrimeViewports(void* list, void* cmdbuf);
std::atomic<bool> g_engineFov{true};  // EXP-016: owner saw culling cut at top/bottom/sides
void ApplyEngineFov(void* camp);
std::atomic<bool> g_nodePass{true};  // EXP-018
uint8_t* CameraNode(void* cam);
void SetNodeRelative(uint8_t* node, const pose::Mat4& rel);
pose::Mat4 AuthoredCameraWorld(uint8_t* node);
pose::Mat4 EyeRel(int eye);
pose::Mat4 NodeRel(int eye);
std::atomic<bool> g_aoOff{false};  // EXP-016
void RestoreEngineFov();
// EXP-013 discriminators (off by default).
std::atomic<bool> g_eyeSame{false};   // both passes rendered with eye 0's pose
// Each eye pass sees its own frame key, so caches keyed on RenderState+0xBF4 ("once per frame")
// are computed per eye. EXP-030: required together with `passsync` — without the sync the eyes
// raced and re-keying produced flashing (EXP-023); with the sync but without re-keying the right
// eye reused the left eye's results and both eyes came out identical.
std::atomic<bool> g_frameKey{true};
constexpr uintptr_t kFrameCounterOff = 0xBF4;  // RenderState(0x141c78958)+0xBF4

std::atomic<bool> g_passSync{true};
std::atomic<bool> g_restoreAtPresent{false};
HANDLE g_syncEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
std::atomic<uint64_t> g_syncWaits{0}, g_syncTimeouts{0};
std::atomic<double> g_syncMsMax{0.0};

void RecordList(ListDisplayFn orig, void* list, void* cmdbuf, uint32_t mode, uint8_t flag) {
    g_curCmdbuf = cmdbuf;
    g_curSlot = -1;
    fpcam::Frame();
    g_recordedSincePresent = true;
    xr::FrameInput frameInput{};
    {
        // Always called: it polls session events even before the session runs (READY -> begin).
        xr::EyeView eyes[2];
        const int slot = xr::BeginGameFrame(eyes, &frameInput);
        if (xr::Running()) {
            g_on = true;
            g_passes = 2;
            g_xrOwnsStereo = true;
        } else if (g_xrOwnsStereo) {
            // Session lost or stopped (RECOVERY_MATRIX.md): fall back to the game's own mono
            // camera instead of rendering two eyes from a stale head pose.
            g_xrOwnsStereo = false;
            g_on = false;
            g_haveXrEyes = false;
            Log("STEREO XR session no longer running; back to flat mono rendering");
        }
        if (slot >= 0) {
            // Diagnostic (EXP-013): both eye passes use eye 0's pose.
            if (g_eyeSame) eyes[1] = eyes[0];
            std::memcpy(g_xrEyes, eyes, sizeof(eyes));
            g_haveXrEyes = true;
            g_curSlot = slot;
        }
    }
    UpdateCinema(g_xrOwnsStereo);
    vrinput::Publish(frameInput, false, 0.f);
    {
        // The cameras of this list: every enabled viewport's camera gets the eye pose; the first
        // one is the scene camera (aspect / letterbox).
        void* cam = nullptr;
        void* cams[kMaxStereoCams] = {};
        int n = 0;
        const uint32_t count = Read<uint32_t>(list, off::VpListCount);
        auto** handles = Read<void**>(list, off::VpListArray);
        for (uint32_t i = 0; handles && i < count; ++i) {
            void* vp = ResolveHandle(handles[i]);
            if (!vp || !(Read<uint32_t>(vp, off::VpFlags) & 1)) continue;
            void* c = ResolveHandle(Read<void*>(vp, off::VpCamera));
            if (!c) continue;
            if (!cam) cam = c;
            bool dup = false;
            for (int k = 0; k < n; ++k) dup |= cams[k] == c;
            if (!dup && n < kMaxStereoCams) cams[n++] = c;
        }
        // Diagnostic (owner 0.16.30: "the view goes black for a second when entering a room: the room's
        // camera changes"; LIVE: for 0.85 s only two square cameras were built, the main one not). Every
        // change of the drawn list and its viewports' flags / cameras is logged (at most 40 per second).
        {
            static void* lastList = nullptr;
            static uint64_t lastSig = 0, windowStart = 0;
            static int inWindow = 0;
            uint64_t sig = reinterpret_cast<uintptr_t>(list) * 1000003ull;
            for (uint32_t i = 0; handles && i < count; ++i) {
                void* vp = ResolveHandle(handles[i]);
                sig = sig * 31 + reinterpret_cast<uintptr_t>(vp);
                if (vp) sig = sig * 31 + Read<uint32_t>(vp, off::VpFlags) + reinterpret_cast<uintptr_t>(Read<void*>(vp, off::VpCamera)) * 7;
            }
            const uint64_t nowMs = GetTickCount64();
            if (nowMs - windowStart >= 1000) { windowStart = nowMs; inWindow = 0; }
            if ((sig != lastSig || list != lastList) && inWindow++ < 40) {
                char line[512];
                int len = snprintf(line, sizeof(line), "STEREO list %p (%u vp):", list, count);
                for (uint32_t i = 0; handles && i < count && i < 6 && len < 440; ++i) {
                    void* vp = ResolveHandle(handles[i]);
                    void* c = vp ? ResolveHandle(Read<void*>(vp, off::VpCamera)) : nullptr;
                    len += snprintf(line + len, sizeof(line) - len, " vp%u=%p flags=0x%x cam=%p aspect=%.2f", i, vp,
                                    vp ? Read<uint32_t>(vp, off::VpFlags) : 0u, c, c ? Read<float>(c, 0x11C) : 0.f);
                }
                Log("%s", line);
            }
            lastSig = sig;
            lastList = list;
        }
        bool changed = n != g_stereoCamCount;
        for (int k = 0; k < n && !changed; ++k) changed = cams[k] != g_stereoCams[k];
        if (changed) {
            for (int k = 0; k < n; ++k)
                Log("STEREO viewport camera %d/%d cam=%p fov=%.3f aspect=%.3f", k + 1, n, cams[k],
                    Read<float>(cams[k], off::CamFov), Read<float>(cams[k], 0x11C));
        }
        std::memcpy(g_stereoCams, cams, sizeof(cams));
        g_stereoCamCount = n;
        g_mainCam = cam;
        if (cam) g_sceneAspect = Read<float>(cam, 0x11C);
    }
    if (g_curSlot >= 0 && g_on) {
        if (uint8_t* node = g_mainCam ? CameraNode(g_mainCam) : nullptr)
            fpcam::Prepare(node, AuthoredCameraWorld(node), frameInput);
        else fpcam::Suspend();
    } else fpcam::Suspend();
    // EXP-016: `ao off` clears the engine's per-viewport AO switch (VIEWPORT+0x890 bit9, checked
    // by FUN_1408695f0 before ComputeAmbientOcclusion) for this recording; restored afterwards.
    uint32_t* aoFlags[8] = {};
    int aoCount = 0;
    if (g_aoOff) {
        const uint32_t cnt = Read<uint32_t>(list, off::VpListCount);
        auto** hs = Read<void**>(list, off::VpListArray);
        for (uint32_t i = 0; hs && i < cnt && aoCount < 8; ++i) {
            auto* vp = static_cast<uint8_t*>(ResolveHandle(hs[i]));
            if (!vp) continue;
            auto* f = reinterpret_cast<uint32_t*>(vp + off::VpFlags);
            if (*f & (1u << 9)) {
                *f &= ~(1u << 9);
                aoFlags[aoCount++] = f;
            }
        }
    }
    struct AoRestore {
        uint32_t** f;
        int* n;
        ~AoRestore() {
            for (int i = 0; i < *n; ++i) *f[i] |= (1u << 9);
        }
    } aoRestore{aoFlags, &aoCount};
    if (!g_on) {
        orig(list, cmdbuf, mode, flag);
        return;
    }
    fpcam::BeginRender();
    zonefix::Frame();
    const int passes = g_passes;
    FILE* dump = nullptr;
    if (g_diffDump.exchange(false)) {
        const std::wstring path = GameDir() + L"\\b2svr_passdump.bin";
        dump = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    }
    if (dump) Snapshot(dump, 0, list);
    uint32_t* lights = SceneLightCount(list);
    const uint32_t lightsBefore = lights ? *lights : 0;
    uint32_t lightsAfter[kMaxPasses] = {};
    for (int eye = 0; eye < passes; ++eye) {
        if (eye > 0 && lights && g_lightFix) *lights = lightsBefore;
        g_recordingEye = eye;
        // EXP-018: the eye pose is on every stereo camera node from the START of the pass, so the
        // viewport setup's reads before CAMERA::Build see the same pose as everything after it.
        if (g_nodePass && g_haveXrEyes && eye <= 1)
            for (int k = 0; k < g_stereoCamCount; ++k) {
                if (uint8_t* node = CameraNode(g_stereoCams[k])) SetNodeRelative(node, NodeRel(eye));
                // EXP-019: the headset FOV/aspect from the pass start: "Compute visibilities" runs
                // before the first CAMERA::Build and culled with the game's ~25 deg camera.
                if (g_engineFov) ApplyEngineFov(g_stereoCams[k]);
            }
        // EXP-022: before any stage of this pass runs, the engine's camera globals still hold the
        // previous pass (for the left eye: the right eye of the PREVIOUS frame). Build this eye's
        // camera now and publish its matrices so pass-start consumers see the current eye.
        if (g_prime && g_haveXrEyes && eye <= 1) {
            PrimeEyeCamera();
            PrimeViewports(list, cmdbuf);
        }
        uint32_t* frameCounter = nullptr;
        uint32_t savedCounter = 0;
        // EXP-021: EVERY eye pass gets its own frame key. Caches keyed on the counter can be filled
        // before the passes (game update, plain game camera); with only pass 2 re-keyed, the left
        // eye reused those and showed shadows for the wrong camera while the right eye was clean.
        if (g_frameKey && eye <= 1 && g_haveXrEyes) {
            if (void* rs = *reinterpret_cast<void**>(game::Va(kCinemaRenderStateVa))) {
                frameCounter = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rs) + kFrameCounterOff);
                savedCounter = *frameCounter;
                *frameCounter = savedCounter ^ (eye == 0 ? 0x20000000u : 0x40000000u);
            }
        }
        // 0.16.35: the visibility pass starts from the room the eye is in (zonefix.h).
        zonefix::BeginPass(g_stereoCams, g_stereoCamCount);
        orig(list, cmdbuf, mode, flag);
        zonefix::EndPass();
        if (frameCounter) *frameCounter = savedCounter;
        if (g_passSync && eye == 0 && passes >= 2 && g_haveXrEyes) {
            ResetEvent(g_syncEvent);
            EmitMarker(cmdbuf, "B2SVR:SYNC");
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            const bool ok = WaitForSingleObject(g_syncEvent, 40) == WAIT_OBJECT_0;
            QueryPerformanceCounter(&t1);
            const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
            const uint64_t n = ++g_syncWaits;
            if (!ok) g_syncTimeouts++;
            if (ms > g_syncMsMax) g_syncMsMax = ms;
            if (n <= 3 || n % 900 == 0)
                Log("STEREO pass sync #%llu: %.2f ms (max %.2f, timeouts %llu)", n, ms, g_syncMsMax.load(),
                    g_syncTimeouts.load());
        }
        // EXP-030: the render thread reads the camera node / FOV scalars when it EXECUTES the pass.
        // Left eye: restored only after the sync wait above (its commands have executed). Last eye:
        // kept until Present, where the render thread is idle (EXP-011), then restored there.
        if (g_passSync && g_haveXrEyes && passes >= 2 && eye == passes - 1) {
            g_restoreAtPresent = true;
        } else {
            RestoreNode();  // the next pass (and game logic after the last one) sees the original node
            RestoreEngineFov();
        }
        if (lights) lightsAfter[eye] = *lights;
        if (dump) Snapshot(dump, uint32_t(eye + 1), list);
    }
    if (dump) {
        fclose(dump);
        Log("STEREO pass dump written (%d passes, rs=0x%zx + data 0x%zx bytes each)", passes, kRenderStateSize,
            size_t(kDataWinSize));
    }
    // EXP-026: after the last (right-eye) pass the camera object and the camera globals hold the
    // RIGHT eye; the engine's work between frames then fed the next frame's LEFT eye with right-eye
    // state (eyesame fixed it). Leave the camera in the left-eye state instead.
    if (g_tailLeft && g_haveXrEyes && passes >= 2) {
        g_recordingEye = 0;
        for (int k = 0; k < g_stereoCamCount; ++k) {
            if (uint8_t* node = CameraNode(g_stereoCams[k])) SetNodeRelative(node, EyeRel(0));
            if (g_engineFov) ApplyEngineFov(g_stereoCams[k]);
        }
        PrimeEyeCamera();
        RestoreNode();
        RestoreEngineFov();
    }
    g_recordingEye = -1;
    g_pairsRecorded++;
    // UI boundary (EXP-011): MenusMaster records its draws after VIEWPORT_LIST::Display returns
    // and before Present. The render thread clears the backbuffer to transparent here, the UI
    // lands on it alone, and BeforePresent lifts it onto the XR quad and ends the XR frame.
    if (g_capture && g_curSlot >= 0) {
        EmitMarker(cmdbuf, kXrMarkers.uiBegin[g_curSlot]);
        g_pendingUiSlot = g_curSlot.load();
    }
    if (g_lightCensusFrames > 0) g_lightCensusFrames--;
    if (lights && g_lightLogBudget > 0 && g_lightLogBudget-- > 0)
        Log("STEREO lights: before=%u after pass0=%u pass1=%u (fix=%d)", lightsBefore, lightsAfter[0],
            lightsAfter[1], int(g_lightFix.load()));
}

float EyeOffset() {
    const int eye = g_recordingEye;
    if (eye < 0 || g_haveXrEyes) return 0.f;
    // CAMERA+0x154 is a view-space X translation added after the view inverse, so a positive
    // value moves the image right, i.e. the eye left (EXP-002). Left eye = +ipd/2.
    const float half = g_ipd.load() * 0.5f;
    return eye == 0 ? half : -half;  // diagnostic pass 2 reuses the right eye
}

// Render thread, every engine marker. XR lifecycle now runs at Present (xr::Service).
void RenderThreadTick() {}

// EXP-030: the render thread executes recorded commands while the game thread keeps recording.
// Commands that read engine data by pointer (camera node, light/shadow matrices) at EXECUTION time
// could see the right-eye pass's values while the left-eye pass was still executing — a timing
// race (left eye flips ~1 s, right eye always right, `eyesame` hides it). `passsync`: after
// recording the left-eye pass, emit B2SVR:SYNC and wait for the render thread to reach it.

bool OnMarker(const char* name) {
    if (std::strcmp(name, "B2SVR:SYNC") == 0) {
        SetEvent(g_syncEvent);
        return true;
    }
    if (std::strncmp(name, "B2SVR:UI", 8) == 0 && (name[8] == 'B' || name[8] == 'E') && name[9] == ':') {
        const int slot = std::atoi(name + 10);
        std::lock_guard lock(g_bbMutex);
        if (g_bb && g_ctx) {
            if (name[8] == 'B')
                xr::OnUiBegin(slot, g_ctx, g_bb);
            else
                xr::OnUiEnd(slot, g_ctx, g_bb, g_eyeTex[1]);
        }
        return true;
    }
    if (std::strncmp(name, "B2SVR:EYE", 9) != 0) return false;
    const int eye = name[9] - '0';
    const int slot = name[10] == ':' ? std::atoi(name + 11) : -1;
    if (eye >= 0 && eye < g_passes) CopyEye(eye, slot);
    return true;
}

// ---- Eye pose on the camera node (FAIL-STR-004, docs/EXP-012) --------------------------------
// FUN_14086c760 builds world-space frustums, the camera position and a view direction from the
// camera NODE's world matrix (+0x80) after CAMERA::Build; composing the eye only onto the view
// (+0x280) left those mono, so screen-space shadow/lighting reconstruction followed the game
// camera instead of the eye. Mode 0 composes the eye onto the node world for the duration of the
// eye pass (view and every node consumer agree), restored after each pass so game logic never
// sees the head pose. Mode 1 is the previous view-only composition (A/B control).
constexpr uintptr_t kNodeWorld = 0x80, kNodeFlags = 0x120;
// 0 node = eye pose (EXP-012), 1 view-only (old), 2 CENTRE (EXP-029, default): the camera node
// carries the head-centre pose for both passes (every position-derived engine decision is identical
// for both eyes, as with `eyesame`, which the owner confirmed fixes the left-eye flip), and only the
// view matrix is shifted per eye (V_eye = V_centre * centreRel * inverse(eyeRel)).
std::atomic<int> g_eyePoseMode{0};  // EXP-029: centre mode gives identical eyes (render reads node-derived data)
// The node keeps the ORIGINAL world in `saved`; every value B2SVR writes is derived from it, and
// `written` is the last value written (so an engine recompute mid-pass is detected and re-saved).
struct NodeOverride {
    uint8_t* node = nullptr;
    float saved[16];
    float written[16];
} g_nodeOv[kMaxStereoCams];

void RestoreOne(NodeOverride& o) {
    if (!o.node) return;
    float* w = reinterpret_cast<float*>(o.node + kNodeWorld);
    // Only undo our own write; if the engine recomputed the world meanwhile, its value stands.
    if (std::memcmp(w, o.written, sizeof(o.written)) == 0) std::memcpy(w, o.saved, sizeof(o.saved));
    o.node = nullptr;
}

void RestoreNode() {
    for (auto& o : g_nodeOv) RestoreOne(o);
}

pose::Mat4 AuthoredCameraWorld(uint8_t* node) {
    pose::Mat4 world;
    const float* current = reinterpret_cast<const float*>(node + kNodeWorld);
    for (const auto& o : g_nodeOv) {
        if (o.node == node && std::memcmp(current, o.written, sizeof(o.written)) == 0) {
            std::memcpy(world.m, o.saved, sizeof(world.m));
            return world;
        }
    }
    std::memcpy(world.m, current, sizeof(world.m));
    return world;
}

uint8_t* CameraNode(void* cam) {
    auto* node = static_cast<uint8_t*>(ResolveHandle(Read<void*>(cam, off::CamNode)));
    if (!node) return nullptr;
    if (((Read<uint32_t>(node, kNodeFlags) >> 1) & 1) == 0) {
        using UpdateFn = void (*)(void*);
        auto update = reinterpret_cast<UpdateFn>(Addr(Anchor::NodeUpdateWorld));
        if (!update) return nullptr;
        update(node);
    }
    return node;
}

// Writes rel * originalWorld into the node (row-vector: rel is the pose in camera space).
void SetNodeRelative(uint8_t* node, const pose::Mat4& rel) {
    float* w = reinterpret_cast<float*>(node + kNodeWorld);
    NodeOverride* o = nullptr;
    for (auto& e : g_nodeOv)
        if (e.node == node) o = &e;
    if (!o)
        for (auto& e : g_nodeOv)
            if (!e.node && !o) o = &e;
    if (!o) return;  // more cameras than slots: leave this one mono
    const bool ours = o->node == node && std::memcmp(w, o->written, sizeof(o->written)) == 0;
    if (!ours) {
        RestoreOne(*o);
        o->node = node;
        std::memcpy(o->saved, w, sizeof(o->saved));
    }
    pose::Mat4 world;
    std::memcpy(world.m, o->saved, sizeof(world.m));
    fpcam::Base(node, &world);
    const pose::Mat4 m = pose::Mul(rel, world);
    std::memcpy(o->written, m.m, sizeof(m.m));
    std::memcpy(w, m.m, sizeof(m.m));
}

pose::Mat4 EyeRel(int eye) {
    pose::Mat4 offset;
    std::memcpy(offset.m, g_xrEyes[eye].viewOffset, sizeof(offset.m));
    return pose::RigidInverse(offset);  // eyeRel = inverse(viewOffset)
}

// Shared cull camera (EXP-013): midway between the eyes, eye 0's orientation.
pose::Mat4 CenterRel() {
    pose::Mat4 c = EyeRel(0);
    const pose::Mat4 r = EyeRel(1);
    for (int i = 12; i < 15; ++i) c.m[i] = 0.5f * (c.m[i] + r.m[i]);
    return c;
}

pose::Mat4 NodeRel(int eye) { return g_eyePoseMode == 2 ? CenterRel() : EyeRel(eye); }

bool g_viewCarriesEye = false;  // this build's eye pose went through the node

// EXP-014: the engine's own FOV scalar (+0x104, full vertical angle, rad) and aspect (+0x108)
// still describe the game's narrow camera while the matrix renders the headset's FOV; consumers
// of the scalar (culling cones, effects) disagree with the pixels. `enginefov on` writes values
// covering both eyes for the pass and restores them after (playbook: forced projection).

constexpr uintptr_t kCamEffAspect = 0x11C;   // aspect CAMERA::Build stores and consumers use
constexpr uintptr_t kCamAutoExposure = 0x1C1;  // EXP-019 candidate (bytes +0x1C0..0x1C3 = HDR bools)
std::atomic<int> g_autoExposure{-1};          // -1 game, 0 force off, 1 force on (EXP-019)
// EXP-028: tonemap key range (auto exposure between MinKey +0x17C and MaxKey +0x180, operator
// +0x1B0 = 3 in gameplay). `keylock <v>` pins both to v for the eye passes; `keylock off` restores.
constexpr uintptr_t kCamMinKey = 0x17C, kCamMaxKey = 0x180;
std::atomic<bool> g_keyLock{false};
std::atomic<float> g_keyValue{0.f};
struct FovSave {
    uint8_t* cam = nullptr;
    float fov, aspect, effAspect, nearClip;
    uint8_t autoExp;
    float minKey, maxKey;
} g_fovSave[kMaxStereoCams];

// Headset frustum (both eyes; symmetric under symfov) as the engine's FOV/aspect scalars.
void HeadsetFovScalars(float& fov, float& aspect) {
    const xr::EyeView &a = g_xrEyes[0], &b = g_xrEyes[1];
    const float u = std::max(a.tanUp, b.tanUp), d = std::min(a.tanDown, b.tanDown);
    const float l = std::min(a.tanLeft, b.tanLeft), r = std::max(a.tanRight, b.tanRight);
    fov = std::atan(u) + std::atan(-d);
    aspect = (r - l) / (u - d);
}

void ApplyEngineFov(void* camp) {
    auto* cam = static_cast<uint8_t*>(camp);
    FovSave* slot = nullptr;
    for (auto& f : g_fovSave)
        if (f.cam == cam) return;  // already applied this pass
    for (auto& f : g_fovSave)
        if (!f.cam && !slot) slot = &f;
    if (!slot) return;
    auto* fov = reinterpret_cast<float*>(cam + off::CamFov);
    auto* aspect = reinterpret_cast<float*>(cam + off::CamAspect);
    auto* eff = reinterpret_cast<float*>(cam + kCamEffAspect);
    auto* nearClip = reinterpret_cast<float*>(cam + off::CamNear);
    *slot = {cam, *fov, *aspect, *eff, *nearClip, cam[kCamAutoExposure], *reinterpret_cast<float*>(cam + kCamMinKey),
             *reinterpret_cast<float*>(cam + kCamMaxKey)};
    if (g_keyLock) {
        *reinterpret_cast<float*>(cam + kCamMinKey) = g_keyValue.load();
        *reinterpret_cast<float*>(cam + kCamMaxKey) = g_keyValue.load();
    }
    float f, asp;
    HeadsetFovScalars(f, asp);
    *fov = f;
    *aspect = asp;
    *eff = asp;
    if (g_autoExposure >= 0) cam[kCamAutoExposure] = uint8_t(g_autoExposure.load());
}

void RestoreEngineFov() {
    for (auto& f : g_fovSave) {
        if (!f.cam) continue;
        *reinterpret_cast<float*>(f.cam + off::CamFov) = f.fov;
        *reinterpret_cast<float*>(f.cam + off::CamAspect) = f.aspect;
        *reinterpret_cast<float*>(f.cam + kCamEffAspect) = f.effAspect;
        *reinterpret_cast<float*>(f.cam + off::CamNear) = f.nearClip;
        f.cam[kCamAutoExposure] = f.autoExp;
        *reinterpret_cast<float*>(f.cam + kCamMinKey) = f.minKey;
        *reinterpret_cast<float*>(f.cam + kCamMaxKey) = f.maxKey;
        f.cam = nullptr;
    }
}

float BuildAspect(void* cam, float gameAspect) {
    const int eye = g_recordingEye;
    if (!g_engineFov || eye < 0 || eye > 1 || !g_haveXrEyes || !IsStereoCam(cam)) return gameAspect;
    float f, asp;
    HeadsetFovScalars(f, asp);
    return asp;
}

void BeforeCameraBuild(void* cam) {
    g_viewCarriesEye = false;
    const int eye = g_recordingEye;
    if (eye < 0 || eye > 1 || !g_haveXrEyes || !IsStereoCam(cam) || g_eyePoseMode == 1) return;
    uint8_t* node = CameraNode(cam);
    if (!node) return;
    SetNodeRelative(node, NodeRel(eye));
    g_viewCarriesEye = g_eyePoseMode == 0;  // mode 2: AfterCameraBuild shifts the centre view per eye
    if (g_engineFov) ApplyEngineFov(cam);
    static int logs = 0;
    if (logs++ < 2) Log("STEREO eye %d pose composed on camera node %p", eye, node);
}

// ---- Shared cull view (EXP-013) --------------------------------------------------------------
// Per-eye frustums made per-eye DISCRETE choices (which shadows/lights/objects) that differ
// between the eyes and change with head rotation (hazard atlas: per-view decisions). With
// `cullview on`, both passes build their visibility frustums, camera position and direction from
// one cull camera: midway between the eyes, union of both eyes' tangents (+5%). The render view
// and projection stay per-eye: they are published (FUN_14083bae0 + jitter FUN_14086bf30) before
// the cull camera is swapped in, and the per-eye projection is re-published afterwards.
// 0 off (per-eye), 1 centre (head-anchored union, EXP-013), 2 game (EXP-015, default): the
// cull camera is the game camera's own pose with a wide symmetric frustum that grows only when
// the eyes look beyond it, so head motion no longer refits shadow maps / culling.
// EXP-018: default OFF. The cull swap replaced the frustum at vp+0x3DD0 (also used to rebuild
// positions from depth) with a centre/union camera, so deferred shadows were reconstructed with a
// camera that differs from the eye's → screen-anchored error → shadows slide with head motion.
// The per-eye mismatch EXP-013 attributed to culling was fixed by the symmetric frustum (EXP-014).
std::atomic<int> g_cullMode{0};
std::atomic<bool> g_cullView{false};
constexpr float kCullTanH = 1.6f, kCullTanV = 1.4f;  // ~116 x 109 degrees around the game camera
constexpr uintptr_t kVpSetupVa = 0x14086c760, kVpJitterVa = 0x14086bf30, kPublishProj2Va = 0x14083b750;
constexpr uint8_t kVpSetupGuard[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55,
                                     0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
constexpr uint8_t kVpJitterGuard[] = {0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0xf8, 0x00,
                                      0x00, 0x00, 0x0f, 0x29, 0x70, 0xe8, 0x44, 0x0f};
constexpr uint8_t kPublishProj2Guard[] = {0x48, 0x83, 0xec, 0x48, 0x0f, 0x28, 0x01, 0xf3,
                                          0x0f, 0x10, 0x0d, 0x19, 0x3b, 0x45, 0x01, 0xf3};
constexpr uintptr_t kProj2GlobalVa = 0x141c8f280;
using VpSetupFn = uint64_t (*)(void* vp, void* cmdbuf);
using VpJitterFn = void (*)(void* vp);
using PublishFn = void (*)(float* m);
VpSetupFn g_origVpSetup = nullptr;
VpJitterFn g_origVpJitter = nullptr;
PublishFn g_publishProj2 = nullptr;
int g_inVpSetup = 0;
struct CullSwap {
    bool active = false;
    uint8_t* cam = nullptr;
    float proj[16], projFar[16];  // per-eye, restored after the setup
    float unionProj[16];
} g_cullSwap;

void UnionProjection(float* p) {
    const xr::EyeView &a = g_xrEyes[0], &b = g_xrEyes[1];
    const float m = 1.05f;
    const float l = std::min(a.tanLeft, b.tanLeft) * m, rt = std::max(a.tanRight, b.tanRight) * m;
    const float u = std::max(a.tanUp, b.tanUp) * m, d = std::min(a.tanDown, b.tanDown) * m;
    p[0] = 2.f / (rt - l);
    p[5] = 2.f / (u - d);
    p[8] = (rt + l) / (rt - l);
    p[9] = (u + d) / (u - d);
}

// Symmetric frustum in the GAME camera's frame containing kCullTan* and every eye frustum corner.
void GameAnchoredProjection(float* p) {
    float h = kCullTanH, v = kCullTanV;
    for (int e = 0; e < 2; ++e) {
        const xr::EyeView& ev = g_xrEyes[e];
        const pose::Mat4 rel = EyeRel(e);  // eye pose in camera space (row-vector)
        for (float tx : {ev.tanLeft, ev.tanRight})
            for (float ty : {ev.tanDown, ev.tanUp}) {
                // Direction (tx, ty, -1) in eye space -> camera space (rotation rows of rel).
                const float x = tx * rel.m[0] + ty * rel.m[4] - rel.m[8];
                const float y = tx * rel.m[1] + ty * rel.m[5] - rel.m[9];
                const float z = tx * rel.m[2] + ty * rel.m[6] - rel.m[10];
                if (z > -0.2f) {  // looking far to the side/back: widest the engine can take
                    h = std::max(h, 4.f);
                    v = std::max(v, 4.f);
                    continue;
                }
                h = std::max(h, std::fabs(x / -z) * 1.05f);
                v = std::max(v, std::fabs(y / -z) * 1.05f);
            }
    }
    p[0] = 1.f / h;
    p[5] = 1.f / v;
    p[8] = 0.f;
    p[9] = 0.f;
}

void HookVpJitter(void* vp) {
    g_origVpJitter(vp);  // render projection globals are final here
    const int eye = g_recordingEye;
    if (!g_inVpSetup || !g_cullView || eye < 0 || eye > 1 || !g_haveXrEyes || g_cullSwap.active) return;
    auto* cam = static_cast<uint8_t*>(*reinterpret_cast<void**>(game::Va(0x141c79030)));
    if (!cam || !IsStereoCam(cam) || !g_viewCarriesEye) return;
    uint8_t* node = CameraNode(cam);
    if (!node) return;
    auto* p = reinterpret_cast<float*>(cam + off::CamProj);
    auto* pf = reinterpret_cast<float*>(cam + off::CamProjFar);
    std::memcpy(g_cullSwap.proj, p, sizeof(g_cullSwap.proj));
    std::memcpy(g_cullSwap.projFar, pf, sizeof(g_cullSwap.projFar));
    if (g_cullMode == 2) {
        GameAnchoredProjection(p);
        GameAnchoredProjection(pf);
    } else {
        UnionProjection(p);
        UnionProjection(pf);
    }
    std::memcpy(g_cullSwap.unionProj, p, sizeof(g_cullSwap.unionProj));
    SetNodeRelative(node, g_cullMode == 2 ? pose::Identity() : CenterRel());
    g_cullSwap.cam = cam;
    g_cullSwap.active = true;
}

std::atomic<int> g_camSetProbe{0};
std::atomic<int> g_viewProbe{0};  // EXP-017: log the second camera matrix set vs the main one
// EXP-017: the second camera set (0x141c8f240 view / 0x141c8f280 proj, FUN_14083b910/b750) is
// refreshed by the viewport setup only when RenderState+0x97; otherwise it keeps a view from an
// earlier pass (LIVE: the OTHER eye's view). `set2 on` publishes the current per-eye view/proj
// into it after every eye-pass viewport setup.
std::atomic<bool> g_refreshSet2{true};
constexpr uintptr_t kPublishView2Va = 0x14083b910;
constexpr uint8_t kPublishView2Guard[] = {0x48, 0x83, 0xec, 0x48, 0x0f, 0x28, 0x01, 0x0f,
                                          0x28, 0x2d, 0x92, 0x39, 0x45, 0x01, 0x0f, 0x29};
PublishFn g_publishView2 = nullptr;

uint64_t HookVpSetup(void* vp, void* cmdbuf) {
    ++g_inVpSetup;
    const uint64_t r = g_origVpSetup(vp, cmdbuf);
    --g_inVpSetup;
    if (g_refreshSet2 && g_publishView2 && g_publishProj2 && g_recordingEye >= 0 && g_recordingEye <= 1 &&
        g_haveXrEyes) {
        auto* cam = static_cast<uint8_t*>(*reinterpret_cast<void**>(game::Va(0x141c79030)));
        if (cam && IsStereoCam(cam)) {
            g_publishView2(reinterpret_cast<float*>(cam + off::CamView));
            g_publishProj2(reinterpret_cast<float*>(cam + off::CamProj));
        }
    }
    if (g_viewProbe > 0 && g_recordingEye >= 0) {
        g_viewProbe--;
        auto* cam = static_cast<uint8_t*>(*reinterpret_cast<void**>(game::Va(0x141c79030)));
        const float* g = reinterpret_cast<const float*>(game::Va(0x141c8f100));
        if (cam) {
            const float* v = reinterpret_cast<const float*>(cam + off::CamView);
            uint8_t* node = CameraNode(cam);
            const float* w = node ? reinterpret_cast<const float*>(node + 0x80) : nullptr;
            Log("VIEWPROBE eye=%d stereoCam=%d camV.t=(%.4f %.4f %.4f) globalV.t=(%.4f %.4f %.4f) node.t=(%.4f %.4f %.4f)",
                g_recordingEye.load(), int(IsStereoCam(cam)), v[12], v[13], v[14], g[12], g[13], g[14], w ? w[12] : 0.f,
                w ? w[13] : 0.f, w ? w[14] : 0.f);
        }
    }
    if (g_camSetProbe > 0 && g_recordingEye >= 0) {
        g_camSetProbe--;
        const float* v1 = reinterpret_cast<const float*>(game::Va(0x141c8f100));
        const float* p1 = reinterpret_cast<const float*>(game::Va(0x141c8f180));
        const float* v2 = reinterpret_cast<const float*>(game::Va(0x141c8f240));
        const float* p2 = reinterpret_cast<const float*>(game::Va(0x141c8f280));
        const uint8_t* rs = *reinterpret_cast<uint8_t**>(game::Va(0x141c78958));
        Log("CAMSET eye=%d vp=%p rs97=%u rs99=%u vp3d89=%u view==view2:%d proj==proj2:%d  v1 fwd=(%.3f %.3f %.3f) t=(%.2f %.2f %.2f) | v2 fwd=(%.3f %.3f %.3f) t=(%.2f %.2f %.2f) | p1 %.3f %.3f %.3f %.3f p2 %.3f %.3f %.3f %.3f",
            g_recordingEye.load(), vp, rs ? rs[0x97] : 0, rs ? rs[0x99] : 0, static_cast<uint8_t*>(vp)[0x3d89],
            std::memcmp(v1, v2, 64) == 0, std::memcmp(p1, p2, 64) == 0, v1[2], v1[6], v1[10], v1[12], v1[13], v1[14],
            v2[2], v2[6], v2[10], v2[12], v2[13], v2[14], p1[0], p1[5], p1[8], p1[9], p2[0], p2[5], p2[8], p2[9]);
    }
    if (g_cullSwap.active) {
        uint8_t* cam = g_cullSwap.cam;
        auto* p = reinterpret_cast<float*>(cam + off::CamProj);
        std::memcpy(p, g_cullSwap.proj, sizeof(g_cullSwap.proj));
        std::memcpy(cam + off::CamProjFar, g_cullSwap.projFar, sizeof(g_cullSwap.projFar));
        // The setup's tail may have published the union projection as the second matrix set.
        auto* g2 = reinterpret_cast<float*>(game::Va(kProj2GlobalVa));
        if (g_publishProj2 && std::memcmp(g2, g_cullSwap.unionProj, sizeof(g_cullSwap.unionProj)) == 0)
            g_publishProj2(p);
        // The node stays at the cull camera for the rest of the pass (culling consumers); the next
        // main-camera build re-applies the eye, and the pass end restores the original.
        g_cullSwap.active = false;
        // Back to the eye pose after the cull section (EXP-018), not the cull camera.
        if (g_nodePass && g_recordingEye >= 0 && g_recordingEye <= 1)
            if (uint8_t* node = CameraNode(cam)) SetNodeRelative(node, NodeRel(g_recordingEye));
        static int logs = 0;
        if (logs++ < 2) Log("STEREO cull view mode %d applied for eye %d", g_cullMode.load(), g_recordingEye.load());
    }
    return r;
}

constexpr uintptr_t kPublishMainVa = 0x14083bae0;
constexpr uint8_t kPublishMainGuard[] = {0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0xb8, 0x00,
                                         0x00, 0x00, 0x0f, 0x29, 0x70, 0xe8, 0x0f, 0x29};
constexpr uintptr_t kProjFarGlobalVa = 0x141c8f1c0;  // FUN_14086c760 copies cam+0x2E0 here
using PublishMainFn = void (*)(float* view, float* proj);
PublishMainFn g_publishMain = nullptr;

void PrimeEyeCamera() {
    if (!g_publishMain || !g_publishView2 || !g_publishProj2 || !g_stereoCamCount) return;
    using BuildFn = void (*)(void* cam, float aspect, float p3, float p4);
    auto build = reinterpret_cast<BuildFn>(Addr(Anchor::CameraBuild));  // through our detour
    if (!build) return;
    float fov, aspect;
    HeadsetFovScalars(fov, aspect);
    for (int k = 0; k < g_stereoCamCount; ++k) build(g_stereoCams[k], aspect, 1.f, 1.f);
    auto* cam = static_cast<uint8_t*>(g_mainCam.load());
    if (!cam) return;
    auto* v = reinterpret_cast<float*>(cam + off::CamView);
    auto* p = reinterpret_cast<float*>(cam + off::CamProj);
    g_publishMain(v, p);
    g_publishView2(v);
    g_publishProj2(p);
    std::memcpy(reinterpret_cast<void*>(game::Va(kProjFarGlobalVa)), cam + off::CamProjFar, 64);
    static int logs = 0;
    if (logs++ < 2) Log("STEREO primed eye %d camera %p before the pass", g_recordingEye.load(), cam);
}

// EXP-027: run the engine's full per-viewport setup (FUN_14086c760: CAMERA::Build, matrix publish,
// frustums cam+0x400/+0x550, vp+0x3DD0, position/direction) for every enabled camera viewport of
// the list at the START of an eye pass. Shadow-caster/visibility decisions early in the pass
// otherwise used frustums the previous pass built (for the left eye: the right eye's, previous
// frame): LIVE `shcount` showed the left pass drawing a different caster set for ~1 s stretches.
std::atomic<bool> g_primeViewports{true};

void PrimeViewports(void* list, void* cmdbuf) {
    if (!g_primeViewports || !list || !cmdbuf) return;
    auto setup = reinterpret_cast<VpSetupFn>(game::Va(kVpSetupVa));  // via our detour
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    int n = 0;
    for (uint32_t i = 0; handles && i < count; ++i) {
        void* vp = ResolveHandle(handles[i]);
        if (!vp) continue;
        const uint32_t flags = Read<uint32_t>(vp, off::VpFlags);
        if (!(flags & 1) || (flags >> 17 & 1) || !Read<void*>(vp, off::VpCamera)) continue;
        setup(vp, cmdbuf);
        ++n;
    }
    static int logs = 0;
    if (logs++ < 2) Log("STEREO primed %d viewport(s) for eye %d", n, g_recordingEye.load());
}

void InstallCullView() {
    auto ok = [&](uintptr_t va, const uint8_t* g, size_t n) {
        return game::GuardAt(game::Va(va), g, n);
    };
    if (!ok(kVpSetupVa, kVpSetupGuard, sizeof(kVpSetupGuard)) || !ok(kVpJitterVa, kVpJitterGuard, sizeof(kVpJitterGuard)) ||
        !ok(kPublishProj2Va, kPublishProj2Guard, sizeof(kPublishProj2Guard))) {
        g_cullView = false;
        Log("STEREO cull view guards MISMATCH; per-eye culling kept");
        return;
    }
    g_publishProj2 = reinterpret_cast<PublishFn>(game::Va(kPublishProj2Va));
    if (ok(kPublishMainVa, kPublishMainGuard, sizeof(kPublishMainGuard)))
        g_publishMain = reinterpret_cast<PublishMainFn>(game::Va(kPublishMainVa));
    else
        Log("STEREO main publisher guard MISMATCH; prime off");
    if (ok(kPublishView2Va, kPublishView2Guard, sizeof(kPublishView2Guard)))
        g_publishView2 = reinterpret_cast<PublishFn>(game::Va(kPublishView2Va));
    else
        Log("STEREO second-view publisher guard MISMATCH; set2 refresh off");
    Hook(reinterpret_cast<void*>(game::Va(kVpSetupVa)), reinterpret_cast<void*>(&HookVpSetup),
         reinterpret_cast<void**>(&g_origVpSetup), "VIEWPORT::Setup");
    Hook(reinterpret_cast<void*>(game::Va(kVpJitterVa)), reinterpret_cast<void*>(&HookVpJitter),
         reinterpret_cast<void**>(&g_origVpJitter), "VIEWPORT::Jitter");
}

void AfterCameraBuild(void* cam) {
    const int eye = g_recordingEye;
    if (eye < 0 || eye > 1 || !g_haveXrEyes || !IsStereoCam(cam)) return;
    const xr::EyeView& ev = g_xrEyes[eye];
    if (g_eyePoseMode == 2 && !g_viewCarriesEye) {
        // Mode 2: the node carried the head centre; V_eye = V_centre * centreRel * inverse(eyeRel).
        const pose::Mat4 shift = pose::Mul(CenterRel(), pose::RigidInverse(EyeRel(eye)));
        auto* v = reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + off::CamView);
        pose::Mat4 vm;
        std::memcpy(vm.m, v, sizeof(vm.m));
        const pose::Mat4 r = pose::Mul(vm, shift);
        std::memcpy(v, r.m, sizeof(r.m));
        static int dbg[2] = {};
        if (dbg[eye]++ < 3) {
            const pose::Mat4 e = EyeRel(eye), c = CenterRel();
            Log("CENTRE eye=%d eyeRel.t=(%.4f %.4f %.4f) centreRel.t=(%.4f %.4f %.4f) shift.t=(%.4f %.4f %.4f)", eye,
                e.m[12], e.m[13], e.m[14], c.m[12], c.m[13], c.m[14], shift.m[12], shift.m[13], shift.m[14]);
        }
    } else if (!g_viewCarriesEye) {
        // Mode 1 / fallback: V_eye = V * offset (row-vector); node consumers stay mono.
        auto* v = reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + off::CamView);
        float r[16] = {};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) r[i * 4 + j] += v[i * 4 + k] * ev.viewOffset[k * 4 + j];
        std::memcpy(v, r, sizeof(r));
    }
    // Projection: the headset's asymmetric frustum; keep the engine's depth terms (m22/m32).
    const float l = ev.tanLeft, rt = ev.tanRight, u = ev.tanUp, d = ev.tanDown;
    for (uintptr_t o : {off::CamProj, off::CamProjFar}) {
        auto* p = reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + o);
        p[0] = 2.f / (rt - l);
        p[5] = 2.f / (u - d);
        p[8] = (rt + l) / (rt - l);
        p[9] = (u + d) / (u - d);
    }
}

void BeforePresent() {
    fpcam::EndRender();
    if (g_restoreAtPresent.exchange(false)) {
        RestoreNode();
        RestoreEngineFov();
    }
    const bool recorded = g_recordedSincePresent.exchange(false);
    if (!recorded) fpcam::Suspend();
    const int slot = g_pendingUiSlot.exchange(-1);
    // At Present the render thread has executed and rewound the frame's command buffer
    // (EXP-011: recorded=0 replayed=0), so the immediate context is free on this thread.
    std::lock_guard lock(g_bbMutex);
    if (!g_bb || !g_ctx) return;
    if (slot >= 0) xr::OnUiEnd(slot, g_ctx, g_bb, g_eyeTex[1]);
    // Session start/stop/loss/retry, after this frame's XR work so a shutdown never cuts a
    // frame in half.
    xr::Service(g_dev, g_xrWanted);
    // Every Present must complete one XR frame. 2D-only screens (pause menu, splash, loading)
    // have no stereo pair, and a pair can stall (an eye copy skipped while ResizeBuffers had
    // released the backbuffer left a waited frame never begun, blocking all later waits).
    // FlatFrame begins any pending wait, or waits anew, and shows the flat image on the panel.
    static uint64_t lastEnded = 0;
    if (xr::Running() && xr::FramesEnded() == lastEnded) {
        static int stallLogs = 0;
        if (recorded && stallLogs < 10) Log("XR stereo pair did not complete; flat frame instead (#%d)", ++stallLogs);
        xr::FrameInput flatInput{};
        xr::FlatFrame(g_ctx, g_bb, g_eyeTex, /*showPanel=*/!recorded, &flatInput);
        vrinput::Publish(flatInput, false, 0.f);
    }
    if (!xr::Running()) vrinput::Suspend();
    lastEnded = xr::FramesEnded();
}

void OnPresent(IDXGISwapChain* sc) {
    if (!g_swapchain) {
        sc->AddRef();
        g_swapchain = sc;
        sc->GetDevice(IID_PPV_ARGS(&g_dev));
        if (g_dev) g_dev->GetImmediateContext(&g_ctx);
        crash::NoteDevice(g_dev);
        void** vt = *reinterpret_cast<void***>(sc);
        Hook(vt[13], reinterpret_cast<void*>(&HookResizeBuffers), reinterpret_cast<void**>(&g_origResizeBuffers),
             "IDXGISwapChain::ResizeBuffers");
    }
    // F9: VR on / off (applied at the next Present by xr::Service, like the "xr" verb).
    static hotkeys::Edge f9;
    if (f9.Feed(VK_F9)) {
        g_xrWanted = !g_xrWanted.load();
        Log("XR %s requested (F9)", g_xrWanted ? "on" : "off");
    }
    std::lock_guard lock(g_bbMutex);
    if (!g_bb) sc->GetBuffer(0, IID_PPV_ARGS(&g_bb));
    // Diagnostic: has the render thread consumed every recorded command by Present?
    // cmdbuf +0x28 = recorded count, +0x2c = replay cursor (FUN_1407e0a30).
    static int presentChecks = 0;
    if (void* cb = g_curCmdbuf.load(); cb && presentChecks < 40) {
        ++presentChecks;
        Log("PRESENT-SYNC recorded=%u replayed=%u", Read<uint32_t>(cb, 0x28), Read<uint32_t>(cb, 0x2c));
    }
}

bool Command(const char* verb, const char* arg) {
    const std::string v = verb;
    if (v == "stereo") {
        g_on = std::strcmp(arg, "on") == 0;
        Log("STEREO %s (ipd=%.4f suppressParticles=%d)", g_on ? "ON" : "OFF", g_ipd.load(),
            int(g_suppressParticles.load()));
    } else if (v == "ipd") {
        g_ipd = float(std::atof(arg));
        Log("STEREO ipd=%.4f", g_ipd.load());
    } else if (v == "suppress") {
        g_suppressParticles = std::strcmp(arg, "on") == 0;
        Log("STEREO suppressParticles=%d", int(g_suppressParticles.load()));
    } else if (v == "xr") {
        g_xrWanted = std::strcmp(arg, "on") == 0;  // applied at the next Present (xr::Service)
        Log("XR %s requested", g_xrWanted ? "on" : "off");
    } else if (v == "recenter") {
        xr::Recenter();
    } else if (v == "lumtrace") {
        g_lumFrames = std::atoi(arg) > 0 ? std::atoi(arg) : 360;
    } else if (v == "keylock") {
        if (std::strcmp(arg, "off") == 0) {
            g_keyLock = false;
        } else {
            g_keyValue = float(std::atof(arg));
            g_keyLock = true;
        }
        Log("STEREO key lock %s (%.3f)", g_keyLock ? "on" : "off", g_keyValue.load());
    } else if (v == "primevp") {
        g_primeViewports = std::strcmp(arg, "off") != 0;
        Log("STEREO prime viewports %s", g_primeViewports ? "on" : "off");
    } else if (v == "tail") {
        g_tailLeft = std::strcmp(arg, "off") != 0;
        Log("STEREO tail-left %s", g_tailLeft ? "on" : "off");
    } else if (v == "prime") {
        g_prime = std::strcmp(arg, "off") != 0;
        Log("STEREO prime %s", g_prime ? "on" : "off");
    } else if (v == "autoexp") {
        g_autoExposure = std::strcmp(arg, "off") == 0 ? 0 : std::strcmp(arg, "on") == 0 ? 1 : -1;
        Log("STEREO auto exposure %s", arg);
    } else if (v == "expo") {
        if (auto* cam = static_cast<uint8_t*>(g_mainCam.load())) {
            const float* f = reinterpret_cast<const float*>(cam);
            Log("EXPO cam=%p minKey=%.3f maxKey=%.3f tonemapOp=%d manualExp=%.3f middleGrey=%.3f 1bc=%.3f "
                "bytes1c0..1c3=%u %u %u %u 164=%u 220=%u 128..12a=%u %u %u fov=%.3f asp108=%.3f asp11c=%.3f",
                cam, f[0x17C / 4], f[0x180 / 4], *reinterpret_cast<const int*>(cam + 0x1B0), f[0x1B4 / 4],
                f[0x1B8 / 4], f[0x1BC / 4], cam[0x1C0], cam[0x1C1], cam[0x1C2], cam[0x1C3], cam[0x164], cam[0x220],
                cam[0x128], cam[0x129], cam[0x12A], f[0x104 / 4], f[0x108 / 4], f[0x11C / 4]);
        }
    } else if (v == "nodepass") {
        g_nodePass = std::strcmp(arg, "off") != 0;
        Log("STEREO nodepass %s", g_nodePass ? "on" : "off");
    } else if (v == "headoff") {
        float dy = 0, pitch = 0;
        if (arg) sscanf_s(arg, "%f %f", &dy, &pitch);
        xr::SetHeadTestOffset(dy, pitch);
        Log("STEREO head test offset dy=%.3f pitch=%.1f", dy, pitch);
    } else if (v == "set2") {
        g_refreshSet2 = std::strcmp(arg, "off") != 0;
        Log("STEREO set2 refresh %s", g_refreshSet2 ? "on" : "off");
    } else if (v == "passsync") {
        g_passSync = std::strcmp(arg, "off") != 0;
        Log("STEREO pass sync %s", g_passSync ? "on" : "off");
    } else if (v == "viewprobe") {
        g_viewProbe = std::atoi(arg) > 0 ? std::atoi(arg) : 12;
    } else if (v == "camset") {
        g_camSetProbe = std::atoi(arg) > 0 ? std::atoi(arg) : 8;
    } else if (v == "closeup") {
        g_closeupOff = std::strcmp(arg, "off") == 0;
        Log("STEREO close-up lighting rig %s", g_closeupOff ? "OFF" : "on");
    } else if (v == "ao") {
        g_aoOff = std::strcmp(arg, "off") == 0;
        Log("STEREO ambient occlusion %s", g_aoOff ? "OFF" : "on");
    } else if (v == "refl" || v == "mirrors") {
        const uint32_t bit = 1u << (v == "refl" ? kStageReflection : kStageMirrors);
        if (std::strcmp(arg, "off") == 0) g_skipStages |= bit;
        else g_skipStages &= ~bit;
        Log("STEREO %s %s", v.c_str(), (g_skipStages.load() & bit) ? "OFF" : "on");
    } else if (v == "enginefov") {
        g_engineFov = std::strcmp(arg, "on") == 0;
        Log("STEREO enginefov=%d", int(g_engineFov.load()));
    } else if (v == "symfov") {
        xr::SetSymmetricFov(std::strcmp(arg, "on") == 0);
        Log("STEREO symfov=%s", arg);
    } else if (v == "cullview") {
        g_cullView = std::strcmp(arg, "off") != 0;
        g_cullMode = std::strcmp(arg, "off") == 0 ? 0 : std::strcmp(arg, "center") == 0 ? 1 : 2;
        Log("STEREO cullview mode=%d (0 off, 1 centre, 2 game-anchored)", g_cullMode.load());
    } else if (v == "eyesame") {
        g_eyeSame = std::strcmp(arg, "on") == 0;
        Log("STEREO eyesame=%d", int(g_eyeSame.load()));
    } else if (v == "framekey") {
        g_frameKey = std::strcmp(arg, "on") == 0;
        Log("STEREO framekey=%d", int(g_frameKey.load()));
    } else if (v == "eyepose") {
        g_eyePoseMode = std::strcmp(arg, "view") == 0 ? 1 : std::strcmp(arg, "node") == 0 ? 0 : 2;
        Log("STEREO eye pose mode %d (0 node, 1 view only, 2 centre node + per-eye view)", g_eyePoseMode.load());
    } else if (v == "uialpha") {
        xr::SetUiAlphaMode(std::atoi(arg));
        Log("XR ui alpha mode %s", arg);
    } else if (v == "cinema") {
        g_cinemaMode = std::strcmp(arg, "game") == 0 ? 1 : 0;
        Log("CINEMA mode in VR: %s", g_cinemaMode ? "game setting" : "off (full height)");
    } else if (v == "flatshot") {
        xr::RequestFlatShot();
    } else if (v == "uishot") {
        xr::RequestUiShot();
    } else if (v == "uitest") {
        xr::SetUiTestPattern(std::strcmp(arg, "on") == 0 ? 1 : 0);
        Log("XR ui test pattern %s", arg);
    } else if (v == "watchlight") {
        g_watchHits = 0;
        g_watchRequest = true;
    } else if (v == "lightcensus") {
        if (!g_lightDump) {
            const std::wstring path = GameDir() + L"\\b2svr_lightdump.bin";
            g_lightDump = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        }
        g_lightCensusFrames = arg[0] ? std::atoi(arg) : 4;
    } else if (v == "lightfix") {
        g_lightFix = std::strcmp(arg, "off") != 0;
        g_lightLogBudget = 6;
        Log("STEREO lightfix=%d", int(g_lightFix.load()));
    } else if (v == "capture") {
        g_capture = std::strcmp(arg, "off") != 0;
        Log("STEREO capture=%d", int(g_capture.load()));
    } else if (v == "stagemarks") {
        g_stageMarks = std::strcmp(arg, "on") == 0;
        Log("STEREO stagemarks=%d", int(g_stageMarks.load()));
    } else if (v == "closeupfix") {
        g_fixCloseupLights = std::strcmp(arg, "off") != 0;
        Log("STEREO closeupfix=%d", int(g_fixCloseupLights.load()));
    } else if (v == "passdump") {
        g_diffDump = true;
    } else if (v == "passes") {
        // 3 passes is diagnostic only: it overflowed per-frame render data and crashed the
        // render thread (EXP-007, deterministic garbage camera pointer in FUN_1407fe6e0).
        g_passes = std::atoi(arg) == 3 ? 3 : 2;
        if (g_passes == 3) Log("STEREO WARNING: passes=3 is known to crash the render thread");
        Log("STEREO passes=%d", g_passes.load());
    } else if (v == "sshot") {
        std::lock_guard lock(g_shotMutex);
        g_shotName = arg[0] ? arg : "b2svr_eyes";
    } else {
        return false;
    }
    return true;
}

}  // namespace b2s::stereo
