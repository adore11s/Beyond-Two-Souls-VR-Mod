#include "probe.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <string>

#include "devctl.h"
#include "game.h"
#include "log.h"
#include "pacing.h"
#include "stereo.h"
#include "xr.h"

extern "C" void B2SVR_TraceThunk();

namespace b2s::probe {
namespace {

using namespace b2s::game;

// Globals (VA for preferred base). Reached only after every code anchor guard passed.
constexpr uintptr_t kRenderStateVa = 0x141c78958;   // ptr; +0xBF4 = frame counter
constexpr uintptr_t kCurCameraVa = 0x141c79030;     // ptr to current M3D::CAMERA
constexpr uintptr_t kFrameCounterOff = 0xBF4;

template <typename T>
T* Global(uintptr_t va) {
    return reinterpret_cast<T*>(game::Va(va));
}

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t FrameCounter() {
    void* rs = *Global<void*>(kRenderStateVa);
    uint32_t v = 0;
    if (rs) SafeCopy(&v, static_cast<uint8_t*>(rs) + kFrameCounterOff, sizeof(v));
    return v;
}

// Bounded logging: everything for the first frames, then a sample.
bool Verbose(uint64_t n) { return n < 240 || n % 600 == 0; }

std::atomic<uint64_t> g_presents{0}, g_listDisplays{0}, g_camSetups{0}, g_sceneCalls{0};
std::atomic<uint64_t> g_listSincePresent{0}, g_setupSincePresent{0}, g_sceneSincePresent{0};
FILE* g_camDump = nullptr;

// EXP-003 draw census state (see CountDraw).
std::atomic<DWORD> g_listThread{0};
std::atomic<bool> g_inList{false};
std::atomic<uint64_t> g_drawInList{0}, g_drawOutList{0}, g_drawOtherThread{0}, g_drawDeferred{0};
std::atomic<uint64_t> g_execCmdList{0}, g_finishCmdList{0}, g_rtBinds{0}, g_rtBindsInList{0};
void** g_immediateVt = nullptr;

// Frame trace (devctl "frametrace [events]"): render-thread event log of markers, target
// binds, clears and draw counts, in order. Budget counts down per logged event.
std::atomic<int> g_traceBudget{0};
std::atomic<uint64_t> g_traceLastDraws{0};

void DescribeView(ID3D11View* v, char* out, size_t cap) {
    if (!v) {
        snprintf(out, cap, "-");
        return;
    }
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        tex->GetDesc(&d);
        tex->Release();
    }
    snprintf(out, cap, "%p(%ux%u f%d)", static_cast<void*>(res), d.Width, d.Height, int(d.Format));
    if (res) res->Release();
}

bool TraceEvent() {
    if (g_traceBudget.load() <= 0) return false;
    return g_traceBudget.fetch_sub(1) > 0;
}

uint64_t DrawsSinceLastTrace();

// ---- VIEWPORT_LIST::Display -------------------------------------------------------
using ListDisplayFn = void (*)(void* list, void* cmdbuf, uint32_t mode, uint8_t flag);
ListDisplayFn g_origListDisplay = nullptr;

void DumpCamera(uint64_t n, void* cam) {
    if (!g_camDump || !cam) return;
    struct Header {
        char magic[4];
        uint32_t frame;
        uint64_t call;
        uint64_t camPtr;
        uint64_t nodePtr;
    } h{{'C', 'A', 'M', 'D'}, FrameCounter(), n, reinterpret_cast<uint64_t>(cam), 0};
    uint8_t camBytes[0x600] = {};
    uint8_t nodeBytes[0x300] = {};
    SafeCopy(camBytes, cam, sizeof(camBytes));
    void* nodeHandle = Read<void*>(camBytes, off::CamNode);
    if (nodeHandle) {
        void* node = nullptr;
        if (SafeCopy(&node, static_cast<uint8_t*>(nodeHandle) + 0x30, sizeof(node)) && node) {
            h.nodePtr = reinterpret_cast<uint64_t>(node);
            SafeCopy(nodeBytes, node, sizeof(nodeBytes));
        }
    }
    fwrite(&h, sizeof(h), 1, g_camDump);
    fwrite(camBytes, sizeof(camBytes), 1, g_camDump);
    fwrite(nodeBytes, sizeof(nodeBytes), 1, g_camDump);
    fflush(g_camDump);
}

constexpr float kDelta = 0.064f;
constexpr bool kLensShiftExperiment = false;  // EXP-002 (passed); off while EXP-005 runs
std::atomic<int> g_expPhase{0};
constexpr uint32_t kExpStart = 1500;   // menu scene is stable by then
std::atomic<int> g_capturePhase{0};

// 0 = not in experiment, 1 = zero-delta capture, 2 = delta capture.
constexpr bool kExperimentsEnabled = false;  // passive build for owner playthroughs

int ExperimentPhase(uint32_t frame) {
    if (!kExperimentsEnabled) return 0;
    if (frame < kExpStart || frame >= kExpStart + 4) return 0;
    return (frame - kExpStart) == 2 ? 2 : 1;
}

[[maybe_unused]] void* MainCamera(void* list) {
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    for (uint32_t i = 0; handles && i < count; ++i) {
        void* vp = ResolveHandle(handles[i]);
        if (!vp || !(Read<uint32_t>(vp, off::VpFlags) & 1)) continue;
        if (void* cam = ResolveHandle(Read<void*>(vp, off::VpCamera))) return cam;
    }
    return nullptr;
}

// Scene telemetry for owner playthroughs: log whenever the enabled-viewport set, a
// viewport's camera, or its fov/aspect changes (split layouts, camera cuts).
struct VpState {
    void* vp;
    void* cam;
    uint32_t flags;
    float fov, aspect;
};
VpState g_lastVp[8] = {};
uint32_t g_lastCount = ~0u;

void LogSceneChanges(void* list) {
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    VpState now[8] = {};
    const uint32_t n = count < 8 ? count : 8;
    bool changed = count != g_lastCount;
    for (uint32_t i = 0; handles && i < n; ++i) {
        now[i].vp = ResolveHandle(handles[i]);
        if (!now[i].vp) continue;
        now[i].flags = Read<uint32_t>(now[i].vp, off::VpFlags);
        now[i].cam = ResolveHandle(Read<void*>(now[i].vp, off::VpCamera));
        if (now[i].cam) {
            now[i].fov = Read<float>(now[i].cam, off::CamFov);
            now[i].aspect = Read<float>(now[i].cam, off::CamAspect);
        }
        const VpState& o = g_lastVp[i];
        changed |= o.vp != now[i].vp || o.cam != now[i].cam || ((o.flags ^ now[i].flags) & 0x20001) ||
                   std::abs(o.fov - now[i].fov) > 0.05f || o.aspect != now[i].aspect;
    }
    if (!changed) return;
    g_lastCount = count;
    std::memcpy(g_lastVp, now, sizeof(now));
    Log("SCENE-CHANGE frame=%u viewports=%u", FrameCounter(), count);
    for (uint32_t i = 0; i < n; ++i)
        Log("  vp[%u]=%p flags=0x%08x cam=%p fov=%.4f aspect=%.4f", i, now[i].vp, now[i].flags, now[i].cam,
            now[i].fov, now[i].aspect);
}

// Raw VIEWPORT bytes for offline layout analysis (b2svr_vpdump.bin: u32 index, 0x900 bytes).
void DumpViewports(void* list) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/")) + L"\\b2svr_logs\\b2svr_vpdump.bin";
    FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!f) return;
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    for (uint32_t i = 0; handles && i < count; ++i) {
        uint8_t bytes[0x900] = {};
        if (void* vp = ResolveHandle(handles[i])) SafeCopy(bytes, vp, sizeof(bytes));
        fwrite(&i, sizeof(i), 1, f);
        fwrite(bytes, sizeof(bytes), 1, f);
    }
    fclose(f);
    Log("dumped %u viewports", count);
}

void HookListDisplay(void* list, void* cmdbuf, uint32_t mode, uint8_t flag) {
    const uint64_t n = g_listDisplays++;
    g_listSincePresent++;
    const uint32_t count = list ? Read<uint32_t>(list, off::VpListCount) : 0;
    if (Verbose(n) || xr::DarkTraceActive()) {
        Log("LIST n=%llu frame=%u list=%p cmdbuf=%p mode=%u flag=%u viewports=%u", n, FrameCounter(), list,
            cmdbuf, mode, flag, count);
        auto** handles = list ? Read<void**>(list, off::VpListArray) : nullptr;
        for (uint32_t i = 0; handles && i < count && i < 8; ++i) {
            void* vp = ResolveHandle(handles[i]);
            if (!vp) {
                Log("  vp[%u] handle=%p unresolved", i, handles[i]);
                continue;
            }
            const uint32_t flags = Read<uint32_t>(vp, off::VpFlags);
            void* cam = ResolveHandle(Read<void*>(vp, off::VpCamera));
            if (cam) {
                Log("  vp[%u]=%p flags=0x%08x cam=%p fov=%.4f aspect=%.4f scale=%.3f near=%.4f far=%.1f mask=0x%x",
                    i, vp, flags, cam, Read<float>(cam, off::CamFov), Read<float>(cam, off::CamAspect),
                    Read<float>(cam, off::CamScale), Read<float>(cam, off::CamNear), Read<float>(cam, off::CamFar),
                    Read<uint32_t>(cam, off::CamLayerMask));
            } else {
                Log("  vp[%u]=%p flags=0x%08x cam=null", i, vp, flags);
            }
        }
    }
    if (n == 300) DumpViewports(list);
    g_expPhase = ExperimentPhase(FrameCounter());
    g_listThread = GetCurrentThreadId();
    // EXP-005: custom viewport rect (VIEWPORT+0xF0, NDC x0,y0,x1,y1) enabled by +0x3D8A and +0xD3.
    uint8_t* rectVp = nullptr;
    uint8_t savedRect[16], savedA = 0, savedB = 0;
    if (g_expPhase == 2) {
        if (void* cam = MainCamera(list)) {
            (void)cam;
            const uint32_t vpCount = Read<uint32_t>(list, off::VpListCount);
            auto** handles = Read<void**>(list, off::VpListArray);
            for (uint32_t i = 0; i < vpCount && !rectVp; ++i) {
                auto* vp = static_cast<uint8_t*>(ResolveHandle(handles[i]));
                if (vp && ResolveHandle(Read<void*>(vp, off::VpCamera))) rectVp = vp;
            }
        }
        if (rectVp) {
            std::memcpy(savedRect, rectVp + 0xF0, 16);
            savedA = rectVp[0x3D8A];
            savedB = rectVp[0xD3];
            const float left[4] = {-1.f, -1.f, 0.f, 1.f};
            std::memcpy(rectVp + 0xF0, left, 16);
            rectVp[0x3D8A] = 1;
            rectVp[0xD3] = 1;
            Log("EXP-005 frame=%u vp=%p rect->left half (saved flags %u %u)", FrameCounter(), rectVp, savedA, savedB);
        }
    }
    g_inList = true;
    stereo::RecordList(g_origListDisplay, list, cmdbuf, mode, flag);
    g_inList = false;
    if (rectVp) {
        std::memcpy(rectVp + 0xF0, savedRect, 16);
        rectVp[0x3D8A] = savedA;
        rectVp[0xD3] = savedB;
    }
    if (g_expPhase) g_capturePhase = g_expPhase.load();
    if (n % 60 == 0) DumpCamera(n, *Global<void*>(kCurCameraVa));
    LogSceneChanges(list);
}

// ---- CAMERA::Build: RE-08 mutation at the engine's own view builder (EXP-002) ------
using CamBuildFn = void (*)(void* cam, float aspect, float p3, float p4);
CamBuildFn g_origCamBuild = nullptr;
std::atomic<uint64_t> g_camBuilds{0};

// Diagnostic (0.16.34, owner: black second when entering a room in F1 / F3; LIVE 0.16.33: the drawn
// viewport list never changed, and in one spell the main camera kept being built). While the XR view
// is dark, each frame's camera builds are summed per camera and logged at Present with the frame's
// lists, scene calls and draws (DARKTRACE); LIST / SCENE lines are logged for every call meanwhile.
struct TraceCam {
    void* cam;
    float aspect;
    uint32_t builds;
};
TraceCam g_traceCams[6]{};
uint32_t g_traceCamCount = 0, g_traceCamOther = 0;
void TraceCamBuild(void* cam, float aspect) {
    for (uint32_t i = 0; i < g_traceCamCount; ++i)
        if (g_traceCams[i].cam == cam) {
            g_traceCams[i].builds++;
            return;
        }
    if (g_traceCamCount < std::size(g_traceCams)) g_traceCams[g_traceCamCount++] = {cam, aspect, 1};
    else g_traceCamOther++;
}

void HookCamBuild(void* cam, float aspect, float p3, float p4) {
    const uint64_t n = g_camBuilds++;
    const int phase = g_expPhase;
    auto* shift = reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + off::CamLensShiftX);
    const float saved = *shift;
    if (phase == 2 && kLensShiftExperiment) *shift = saved + kDelta;
    *shift += stereo::EyeOffset();
    stereo::BeforeCameraBuild(cam);
    aspect = stereo::BuildAspect(cam, aspect);
    g_origCamBuild(cam, aspect, p3, p4);
    *shift = saved;
    stereo::AfterCameraBuild(cam);
    if (xr::DarkTraceActive()) TraceCamBuild(cam, aspect);
    if (phase || Verbose(n))
        Log("CAMBUILD n=%llu frame=%u cam=%p aspect=%.4f p3=%.4f p4=%.4f shift=(%.4f,%.4f) phase=%d viewTx=%.5f",
            n, FrameCounter(), cam, aspect, p3, p4, saved, Read<float>(cam, off::CamLensShiftY), phase,
            Read<float>(cam, off::CamView + 12 * 4));
}

// ---- VIEWPORT::CameraSetup (current-camera publish + layer mask) -------------------
using CamSetupFn = void (*)(void* vp);
CamSetupFn g_origCamSetup = nullptr;

void HookCamSetup(void* vp) {
    g_camSetups++;
    g_setupSincePresent++;
    g_origCamSetup(vp);
}

// ---- once-per-frame scene update guard ---------------------------------------------
using SceneFn = void (*)(void* scene, void* vp, uint32_t mask, uint8_t slot);
SceneFn g_origScene = nullptr;

void HookScene(void* scene, void* vp, uint32_t mask, uint8_t slot) {
    const uint64_t n = g_sceneCalls++;
    g_sceneSincePresent++;
    if (Verbose(n) || xr::DarkTraceActive())
        Log("SCENE n=%llu frame=%u scene=%p vp=%p mask=0x%x slot=%u", n, FrameCounter(), scene, vp, mask, slot);
    g_origScene(scene, vp, mask, slot);
}

// ---- engine trace revival -------------------------------------------------------------
// ENGINE::Trace is an empty variadic printf in retail (/OPT:ICF may fold other empty
// variadics onto it), so only format strings inside the EXE's .rdata are logged.
uintptr_t g_rdataBegin = 0, g_rdataEnd = 0;
struct TraceCount {
    const char* fmt;
    uint32_t n;
};
TraceCount g_traceCounts[256] = {};
std::mutex g_traceMutex;

bool FormatTrace(char* out, size_t cap, const char* fmt, va_list args) {
    __try {
        vsnprintf(out, cap, fmt, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Called from B2SVR_TraceThunk (trace_thunk.asm), which preserves every volatile register the
// engine's callers rely on; args is the caller's spilled variadic area.
void TraceV(const char* fmt, va_list args) {
    const auto f = reinterpret_cast<uintptr_t>(fmt);
    if (f < g_rdataBegin || f >= g_rdataEnd) return;
    {
        std::lock_guard lock(g_traceMutex);
        size_t slot = (f >> 3) % 256;
        for (size_t probe = 0; probe < 256; ++probe, slot = (slot + 1) % 256) {
            if (g_traceCounts[slot].fmt == fmt || g_traceCounts[slot].fmt == nullptr) break;
        }
        TraceCount& c = g_traceCounts[slot];
        c.fmt = fmt;
        if (++c.n > 20 && (c.n & (c.n - 1)) != 0) return;  // after 20, log only powers of two
    }
    char buf[1024];
    const bool ok = FormatTrace(buf, sizeof(buf), fmt, args);
    size_t len = std::strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    Log("ENGINE %s", ok ? buf : fmt);
}

void FindRdata() {
    auto* base = reinterpret_cast<uint8_t*>(ModuleBase());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".rdata", 7) == 0) {
            g_rdataBegin = reinterpret_cast<uintptr_t>(base + sec->VirtualAddress);
            g_rdataEnd = g_rdataBegin + sec->Misc.VirtualSize;
        }
    }
}

// ---- EXP-006: engine command-marker census (render-thread callback seam) --------------
// Engine commands are 0x28-byte records dispatched through a handler table (ADDR-011,
// 0x1414f9d30, indexed by record+4). Type 0x1B is a named marker whose retail handler only
// advances the argument cursor (`inc [cmd+0x18]; ret`). Replacing that table slot gives a
// callback on whichever thread replays the buffer.
constexpr uintptr_t kCmdTableVa = 0x1414f9d30;
constexpr uintptr_t kMarkerHandlerVa = 0x1407e3530;
constexpr int kMarkerCmd = 0x1B;
std::atomic<uint64_t> g_markers{0};
std::atomic<DWORD> g_markerThread{0};

// EXP-024: per-eye-pass counts over many frames (`shcount <frames>`): shadow-atlas binds and
// occlusion read-backs between consecutive eye markers, one log line per frame.
std::atomic<int> g_shCountFrames{0};
std::atomic<int> g_renderEyePass{0};  // 0 until the EYE0 marker executes, then 1 until EYE1
std::atomic<uint32_t> g_passAtlas{0}, g_passOccl{0}, g_passDraws{0};
uint32_t g_shPrev[2] = {};

// Per-eye draws (0.17.3, EXP-041): render-thread draws up to the EYE0 marker (left pass, plus the previous
// frame's UI) and from EYE0 to EYE1 (right pass). `eyedraws <frames>` logs their average once. Not a
// missing-geometry detector: the first pass also holds once-per-frame work (LIVE: ~2000 more draws with
// both eyes rendered from the same pose).
std::atomic<uint32_t> g_eyeDraws{0};
int g_measureFrames = 0, g_measureLeftFrames = 0;  // render thread (set by the verb between frames)
uint64_t g_measureLeft = 0, g_measureRight = 0;
char g_measureTag[32] = {};
uint32_t g_leftDraws = 0;
// While the headset view is dark: each eye's draws next to the bright-view average, to tell a scene
// that drew nothing (visibility found nothing) from one drawn and then blacked out.
uint64_t g_brightLeft16 = 0, g_brightRight16 = 0;  // running averages x16
uint64_t g_darkDiagAt = 0;
int g_darkDiagLogs = 0;

void EyeMarker(int eye) {
    const uint32_t draws = g_eyeDraws.exchange(0, std::memory_order_relaxed);
    if (eye == 0) {
        g_leftDraws = draws;
        return;
    }
    if (xr::ViewDark()) {
        const uint64_t now = GetTickCount64();
        if (now - g_darkDiagAt >= 500 && g_darkDiagLogs < 120) {
            g_darkDiagAt = now;
            ++g_darkDiagLogs;
            Log("DARK eyes drew left %u, right %u (bright view averages left %llu, right %llu)", g_leftDraws, draws,
                static_cast<unsigned long long>(g_brightLeft16 >> 4), static_cast<unsigned long long>(g_brightRight16 >> 4));
        }
    } else {
        g_brightLeft16 += g_leftDraws - (g_brightLeft16 >> 4);
        g_brightRight16 += draws - (g_brightRight16 >> 4);
    }
    if (g_measureLeftFrames > 0) {
        g_measureLeft += g_leftDraws;
        g_measureRight += draws;
        if (--g_measureLeftFrames == 0)
            Log("EYES measure %s: left %llu, right %llu draws per frame over %d frames", g_measureTag,
                static_cast<unsigned long long>(g_measureLeft / g_measureFrames),
                static_cast<unsigned long long>(g_measureRight / g_measureFrames), g_measureFrames);
    }
}

bool IsAtlasSized(ID3D11View* v) {
    if (!v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    ID3D11Texture2D* t = nullptr;
    bool yes = false;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        yes = d.Width == 2048 && d.Height == 2048;
        t->Release();
    }
    r->Release();
    return yes;
}

void MarkerHandler(void* /*cmdbuf*/, uint8_t* cmd) {
    uint32_t& cursor = *reinterpret_cast<uint32_t*>(cmd + 0x18);
    auto** args = *reinterpret_cast<const char***>(cmd + 0x08);
    const char* name = args ? args[cursor] : nullptr;
    ++cursor;  // the original handler's whole behaviour
    if (TraceEvent()) {
        char safe[64] = {};
        if (name) SafeCopy(safe, name, sizeof(safe) - 1);
        Log("TRACE draws+%llu marker %s", DrawsSinceLastTrace(), safe);
    }
    stereo::RenderThreadTick();
    if (name && std::strncmp(name, "B2SVR:EYE0", 10) == 0) {
        g_renderEyePass = 1;
        EyeMarker(0);
    } else if (name && std::strncmp(name, "B2SVR:EYE1", 10) == 0) {
        g_renderEyePass = 2;
        EyeMarker(1);
    }
    else if (name && std::strncmp(name, "B2SVR:UIB", 9) == 0) g_renderEyePass = 0;
    if (g_shCountFrames > 0 && name && std::strncmp(name, "B2SVR:EYE", 9) == 0) {
        const int eye = name[9] - '0';
        static uint32_t left[3];
        const uint32_t a = g_passAtlas.exchange(0), o = g_passOccl.exchange(0), d = g_passDraws.exchange(0);
        if (eye == 0) {
            left[0] = a; left[1] = o; left[2] = d;
        } else {
            Log("SHC L atlas=%u occl=%u draws=%u | R atlas=%u occl=%u draws=%u", left[0], left[1], left[2], a, o, d);
            g_shCountFrames--;
        }
    }
    if (name && name[0] == 'B' && stereo::OnMarker(name)) return;
    const uint64_t n = g_markers++;
    g_markerThread = GetCurrentThreadId();
    if (n < 400) {
        char safe[64] = {};
        if (name) SafeCopy(safe, name, sizeof(safe) - 1);
        Log("MARKER n=%llu thread=%lu drawsSoFar(render)=%llu name=%s", n, GetCurrentThreadId(),
            g_drawOtherThread.load(), safe);
    }
}

bool InstallMarkerSeam() {
    auto** slot = reinterpret_cast<void**>(game::Va(kCmdTableVa)) + kMarkerCmd;
    void* expected = reinterpret_cast<void*>(game::Va(kMarkerHandlerVa));
    if (*slot != expected) {
        Log("marker seam: table slot mismatch (%p != %p); not installed", *slot, expected);
        return false;
    }
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = reinterpret_cast<void*>(&MarkerHandler);
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("marker seam installed at table slot %p", slot);
    return true;
}

// ---- DXGI Present ------------------------------------------------------------------
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_origPresent = nullptr;
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Present1Fn g_origPresent1 = nullptr;

void HookGameContext(IDXGISwapChain* sc);
void DumpTargetCensus(uint64_t frames);

// Saves the backbuffer as a 32-bit BMP next to the game (b2svr_cap_<frame>_<phase>.bmp).
void CaptureBackbuffer(IDXGISwapChain* sc, uint32_t frame, int phase) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.SampleDesc = {1, 0};
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path(exe);
            path = path.substr(0, path.find_last_of(L"\\/")) + L"\\b2svr_cap_" + std::to_wstring(frame) + L"_" +
                   std::to_wstring(phase) + L".bmp";
            if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR)) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = LONG(d.Width);
                ih.biHeight = -LONG(d.Height);
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (UINT y = 0; y < d.Height; ++y)
                    fwrite(static_cast<uint8_t*>(m.pData) + y * m.RowPitch, 4, d.Width, f);
                fclose(f);
            }
            ctx->Unmap(st, 0);
            Log("CAPTURE frame=%u phase=%d fmt=%d %ux%u", frame, phase, int(d.Format), d.Width, d.Height);
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
    bb->Release();
}

LARGE_INTEGER g_qpf{}, g_lastPresent{};
double g_dtMin = 1e9, g_dtMax = 0, g_dtSum = 0;
uint32_t g_dtCount = 0;

void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags, const char* which) {
    const uint64_t n = g_presents++;
    if (n == 0) HookGameContext(sc);
    stereo::OnPresent(sc);
    devctl::OnFrame(sc);
    if (n == 600) DumpTargetCensus(599);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_lastPresent.QuadPart) {
        const double dt = 1000.0 * double(now.QuadPart - g_lastPresent.QuadPart) / double(g_qpf.QuadPart);
        g_dtMin = dt < g_dtMin ? dt : g_dtMin;
        g_dtMax = dt > g_dtMax ? dt : g_dtMax;
        g_dtSum += dt;
        if (++g_dtCount == 120) {
            Log("PACING last120: avg=%.2fms (%.1f fps) min=%.2f max=%.2f sync=%u", g_dtSum / 120, 120000.0 / g_dtSum,
                g_dtMin, g_dtMax, sync);
            g_dtMin = 1e9, g_dtMax = 0, g_dtSum = 0, g_dtCount = 0;
        }
    }
    g_lastPresent = now;
    if (int phase = g_capturePhase.exchange(0)) CaptureBackbuffer(sc, FrameCounter(), phase);
    const uint64_t lists = g_listSincePresent.exchange(0);
    const uint64_t setups = g_setupSincePresent.exchange(0);
    const uint64_t scenes = g_sceneSincePresent.exchange(0);
    const uint64_t dIn = g_drawInList.exchange(0), dOut = g_drawOutList.exchange(0);
    const uint64_t dOther = g_drawOtherThread.exchange(0), dDef = g_drawDeferred.exchange(0);
    const uint64_t exec = g_execCmdList.exchange(0), fin = g_finishCmdList.exchange(0);
    const uint64_t rt = g_rtBinds.exchange(0), rtIn = g_rtBindsInList.exchange(0);
    if (xr::DarkTraceActive()) {
        char cams[400];
        int len = 0;
        cams[0] = 0;
        for (uint32_t i = 0; i < g_traceCamCount && len < 340; ++i)
            len += snprintf(cams + len, sizeof(cams) - len, " %p(%.2f)x%u", g_traceCams[i].cam, g_traceCams[i].aspect,
                            g_traceCams[i].builds);
        Log("DARKTRACE frame=%u lists=%llu camSetups=%llu sceneCalls=%llu draws render=%llu list=%llu cams:%s%s",
            FrameCounter(), lists, setups, scenes, dOther, dIn + dOut, cams, g_traceCamOther ? " +others" : "");
    }
    g_traceCamCount = 0;
    g_traceCamOther = 0;
    if (!Verbose(n)) return;
    Log("DRAWS n=%llu inList=%llu outList=%llu otherThread=%llu deferred=%llu execCL=%llu finishCL=%llu "
        "rtBinds=%llu (inList %llu) presentThread=%s",
        n, dIn, dOut, dOther, dDef, exec, fin, rt, rtIn, GetCurrentThreadId() == g_listThread ? "list" : "OTHER");
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    Log("%s n=%llu frame=%u sc=%p sync=%u flags=0x%x bb=%ux%u fmt=%d buffers=%u windowed=%d "
        "lists=%llu camSetups=%llu sceneCalls=%llu",
        which, n, FrameCounter(), sc, sync, flags, d.BufferDesc.Width, d.BufferDesc.Height,
        int(d.BufferDesc.Format), d.BufferCount, d.Windowed, lists, setups, scenes);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    sync = pacing::OnPresent(sync, xr::Running());  // under XR the headset (xrWaitFrame) paces
    OnPresent(sc, sync, flags, "PRESENT");
    stereo::BeforePresent();
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* p) {
    sync = pacing::OnPresent(sync, xr::Running());
    OnPresent(sc, sync, flags, "PRESENT1");
    stereo::BeforePresent();
    return g_origPresent1(sc, sync, flags, p);
}

// ---- EXP-003 draw census: where do D3D11 draws happen relative to the list display? ----

void CountDraw(ID3D11DeviceContext* ctx) {
    if (*reinterpret_cast<void***>(ctx) != g_immediateVt) {
        g_drawDeferred++;
    } else if (GetCurrentThreadId() != g_listThread) {
        g_drawOtherThread++;
        g_eyeDraws.fetch_add(1, std::memory_order_relaxed);
    } else if (g_inList) {
        g_drawInList++;
    } else {
        g_drawOutList++;
    }
}

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
DrawIndexedFn g_origDrawIndexed = nullptr;
DrawFn g_origDraw = nullptr;
DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;
DrawInstancedFn g_origDrawInstanced = nullptr;
ExecFn g_origExec = nullptr;
FinishFn g_origFinish = nullptr;
OMSetRTFn g_origOMSetRT = nullptr;

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
    CountDraw(c);
    g_origDrawIndexed(c, a, b, d);
}
void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT a, UINT b) {
    CountDraw(c);
    g_origDraw(c, a, b);
}
void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    CountDraw(c);
    g_origDrawIndexedInstanced(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    CountDraw(c);
    g_origDrawInstanced(c, a, b, d, e);
}
void STDMETHODCALLTYPE HookExec(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL r) {
    g_execCmdList++;
    g_origExec(c, l, r);
}
HRESULT STDMETHODCALLTYPE HookFinish(ID3D11DeviceContext* c, BOOL r, ID3D11CommandList** l) {
    g_finishCmdList++;
    return g_origFinish(c, r, l);
}
struct RtKey {
    UINT w, h;
    int fmt;
    UINT count;
    void* tex;
};
RtKey g_rtSeen[64];
std::atomic<int> g_rtSeenCount{0};
std::atomic<bool> g_rtCensusOn{true};

void NoteTarget(ID3D11View* view, bool depth) {
    if (!view || !g_rtCensusOn) return;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        const int fmt = depth ? -int(d.Format) : int(d.Format);
        int n = g_rtSeenCount;
        bool found = false;
        for (int i = 0; i < n; ++i) {
            if (g_rtSeen[i].tex == tex) {
                g_rtSeen[i].count++;
                found = true;
                break;
            }
        }
        if (!found && n < 64) {
            g_rtSeen[n] = {d.Width, d.Height, fmt, 1, tex};
            g_rtSeenCount = n + 1;
        }
        tex->Release();
    }
    if (res) res->Release();
}

void DumpTargetCensus(uint64_t frames) {
    g_rtCensusOn = false;
    const int n = g_rtSeenCount;
    Log("RT census over %llu frames: %d distinct textures (fmt<0 = depth)", frames, n);
    for (int i = 0; i < n; ++i)
        Log("  rt[%02d] tex=%p %4ux%-4u fmt=%4d binds=%u", i, g_rtSeen[i].tex, g_rtSeen[i].w, g_rtSeen[i].h,
            g_rtSeen[i].fmt, g_rtSeen[i].count);
}

uint64_t DrawsSinceLastTrace() {
    const uint64_t now = g_drawOtherThread.load() + g_drawOutList.load() + g_drawInList.load();
    return now - g_traceLastDraws.exchange(now);
}

using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
ClearRtvFn g_origClearRtv = nullptr;
ClearDsvFn g_origClearDsv = nullptr;

void STDMETHODCALLTYPE HookClearRtv(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT col[4]) {
    if (TraceEvent()) {
        char d[96];
        DescribeView(rtv, d, sizeof(d));
        Log("TRACE draws+%llu clearRTV %s (%.2f %.2f %.2f %.2f)", DrawsSinceLastTrace(), d, col[0], col[1], col[2],
            col[3]);
    }
    g_origClearRtv(c, rtv, col);
}

void STDMETHODCALLTYPE HookClearDsv(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
                                    UINT8 stencil) {
    if (TraceEvent()) {
        char d[96];
        DescribeView(dsv, d, sizeof(d));
        Log("TRACE draws+%llu clearDSV %s flags=%u depth=%.2f stencil=%u", DrawsSinceLastTrace(), d, flags, depth,
            stencil);
    }
    g_origClearDsv(c, dsv, flags, depth, stencil);
}

void STDMETHODCALLTYPE HookOMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv,
                                   ID3D11DepthStencilView* dsv) {
    if (g_shCountFrames > 0 && (IsAtlasSized(dsv) || (n > 0 && rtv && IsAtlasSized(rtv[0])))) g_passAtlas++;
    if ((n > 0 || dsv) && TraceEvent()) {
        char a[96], b[96], z[96];
        DescribeView(n > 0 && rtv ? rtv[0] : nullptr, a, sizeof(a));
        DescribeView(n > 1 && rtv ? rtv[1] : nullptr, b, sizeof(b));
        DescribeView(dsv, z, sizeof(z));
        Log("TRACE draws+%llu bind n=%u rt0=%s rt1=%s ds=%s", DrawsSinceLastTrace(), n, a, b, z);
    }
    if (*reinterpret_cast<void***>(c) == g_immediateVt) {
        if (n && rtv && rtv[0]) NoteTarget(rtv[0], false);
        NoteTarget(dsv, true);
        g_rtBinds++;
        if (g_inList && GetCurrentThreadId() == g_listThread) g_rtBindsInList++;
    }
    g_origOMSetRT(c, n, rtv, dsv);
}

// EXP-023 (playbook FAIL-STR-025): occlusion query results decided in one eye's view and reused
// by the other. `occl force` reports every occlusion query as visible (can only add work, never
// delete geometry) and drops predication; `occl census` counts GetData by query type.
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
using SetPredFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Predicate*, BOOL);
GetDataFn g_origGetData = nullptr;
SetPredFn g_origSetPred = nullptr;
std::atomic<bool> g_occlForce{false};
// 0.17.9 (owner, "The Party": objects / the whole scene missing in the second eye; LIVE A/B with head offsets:
// 9 of 12 captures broken with the game's results, 2 of 12 with every query reported visible; frame time
// unchanged at 90 fps). Default: report every occlusion query visible while VR runs (`occl vr`); `occl off`
// = the game's results, `occl force` = always.
std::atomic<bool> g_occlForceVr{true};
bool OcclForced() { return g_occlForce || (g_occlForceVr && xr::Running()); }
std::atomic<int> g_occlCensus{0};
std::atomic<uint64_t> g_qOccl{0}, g_qPred{0}, g_qOther{0}, g_qPredSet{0}, g_qNotReady{0};

HRESULT STDMETHODCALLTYPE HookGetData(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* data, UINT size,
                                      UINT flags) {
    const HRESULT hr = g_origGetData(c, a, data, size, flags);
    if (!a || !data) return hr;
    ID3D11Query* q = nullptr;
    if (FAILED(a->QueryInterface(__uuidof(ID3D11Query), reinterpret_cast<void**>(&q))) || !q) return hr;
    D3D11_QUERY_DESC d{};
    q->GetDesc(&d);
    q->Release();
    if (d.Query == D3D11_QUERY_OCCLUSION) {
        g_qOccl++;
        if (hr == S_FALSE) g_qNotReady++;
        if (g_shCountFrames > 0) g_passOccl++;
        // A result the GPU has not finished ("not ready", S_FALSE) also counts as visible: the second eye
        // pass asks right after the first one recorded the queries (0.17.11).
        if (OcclForced() && (hr == S_OK || hr == S_FALSE) && size >= sizeof(UINT64)) {
            *static_cast<UINT64*>(data) = 1u << 20;
            return S_OK;
        }
    } else if (d.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
        g_qPred++;
        if (OcclForced() && (hr == S_OK || hr == S_FALSE) && size >= sizeof(BOOL)) {
            *static_cast<BOOL*>(data) = TRUE;
            return S_OK;
        }
    } else {
        g_qOther++;
    }
    return hr;
}

void STDMETHODCALLTYPE HookSetPred(ID3D11DeviceContext* c, ID3D11Predicate* p, BOOL value) {
    if (p) g_qPredSet++;
    g_origSetPred(c, OcclForced() ? nullptr : p, value);
}

// EXP-025: occlusion queries are read once per frame, after the right-eye pass overwrote them, so
// the left eye was lit from the right eye's (previous-frame) visibility. `occl eye0`: queries are
// only issued in the left-eye pass (render thread between frame start and the EYE0 marker); the
// right-eye pass leaves them untouched, so the results always describe the left eye (the right eye
// is 64 mm away). g_renderEyePass is set by the XR eye markers on the render thread.
using BeginEndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
BeginEndFn g_origBegin = nullptr, g_origEnd = nullptr;
std::atomic<bool> g_occlEye0{false};  // EXP-025 refuted (owner: not fixed), kept as a toggle
std::atomic<uint64_t> g_occlSkipped{0};

bool IsOcclusionQuery(ID3D11Asynchronous* a) {
    ID3D11Query* q = nullptr;
    if (!a || FAILED(a->QueryInterface(__uuidof(ID3D11Query), reinterpret_cast<void**>(&q))) || !q) return false;
    D3D11_QUERY_DESC d{};
    q->GetDesc(&d);
    q->Release();
    return d.Query == D3D11_QUERY_OCCLUSION || d.Query == D3D11_QUERY_OCCLUSION_PREDICATE;
}

bool SkipQuery(ID3D11Asynchronous* a) {
    if (!g_occlEye0 || g_renderEyePass != 1 || !xr::Running()) return false;
    if (!IsOcclusionQuery(a)) return false;
    g_occlSkipped++;
    return true;
}

void STDMETHODCALLTYPE HookBegin(ID3D11DeviceContext* c, ID3D11Asynchronous* a) {
    if (SkipQuery(a)) return;
    g_origBegin(c, a);
}

void STDMETHODCALLTYPE HookEnd(ID3D11DeviceContext* c, ID3D11Asynchronous* a) {
    if (SkipQuery(a)) return;
    g_origEnd(c, a);
}

// EXP-014 trace: viewports and scissor rects (screen-locked clip rectangles).
using RsViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RsScissorsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
RsViewportsFn g_origRsViewports = nullptr;
RsScissorsFn g_origRsScissors = nullptr;

void STDMETHODCALLTYPE HookRsViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (n && v && TraceEvent())
        Log("TRACE draws+%llu viewport n=%u (%.0f,%.0f %.0fx%.0f)", DrawsSinceLastTrace(), n, v[0].TopLeftX,
            v[0].TopLeftY, v[0].Width, v[0].Height);
    g_origRsViewports(c, n, v);
}

void STDMETHODCALLTYPE HookRsScissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (n && r && TraceEvent())
        Log("TRACE draws+%llu scissor n=%u (%ld,%ld)-(%ld,%ld)", DrawsSinceLastTrace(), n, r[0].left, r[0].top,
            r[0].right, r[0].bottom);
    g_origRsScissors(c, n, r);
}

struct D3DSlots {
    void* present = nullptr;
    void* present1 = nullptr;
    void** immediateVt = nullptr;
    void** deferredVt = nullptr;
};

// Slots come from a throwaway device; the game's objects share d3d11/dxgi's vtables.
bool FindD3DSlots(D3DSlots* out) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"B2SVR_Dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        void** vt = *reinterpret_cast<void***>(sc);
        out->present = vt[8];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            out->present1 = (*reinterpret_cast<void***>(sc1))[22];
            sc1->Release();
        }
        out->immediateVt = *reinterpret_cast<void***>(ctx);
        ID3D11DeviceContext* deferred = nullptr;
        if (SUCCEEDED(dev->CreateDeferredContext(0, &deferred))) {
            out->deferredVt = *reinterpret_cast<void***>(deferred);
            deferred->Release();
        }
        sc->Release();
        dev->Release();
        ctx->Release();
    } else {
        Log("dummy swapchain failed hr=0x%08lx", hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool Hook(void* target, void* detour, void** orig, const char* name);

// D3D11 context vtables are per device, so hook the game's own immediate context.
void HookGameContext(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    void** v = *reinterpret_cast<void***>(ctx);
    g_immediateVt = v;
    Log("game device=%p immediate ctx=%p vtable=%p featureLevel=0x%x", dev, ctx, v, dev->GetFeatureLevel());
    Hook(v[12], reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_origDrawIndexed),
         "Ctx::DrawIndexed");
    Hook(v[13], reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_origDraw), "Ctx::Draw");
    Hook(v[20], reinterpret_cast<void*>(&HookDrawIndexedInstanced),
         reinterpret_cast<void**>(&g_origDrawIndexedInstanced), "Ctx::DrawIndexedInstanced");
    Hook(v[21], reinterpret_cast<void*>(&HookDrawInstanced), reinterpret_cast<void**>(&g_origDrawInstanced),
         "Ctx::DrawInstanced");
    Hook(v[33], reinterpret_cast<void*>(&HookOMSetRT), reinterpret_cast<void**>(&g_origOMSetRT),
         "Ctx::OMSetRenderTargets");
    Hook(v[27], reinterpret_cast<void*>(&HookBegin), reinterpret_cast<void**>(&g_origBegin), "Ctx::Begin");
    Hook(v[28], reinterpret_cast<void*>(&HookEnd), reinterpret_cast<void**>(&g_origEnd), "Ctx::End");
    Hook(v[29], reinterpret_cast<void*>(&HookGetData), reinterpret_cast<void**>(&g_origGetData),
         "Ctx::GetData");
    Hook(v[30], reinterpret_cast<void*>(&HookSetPred), reinterpret_cast<void**>(&g_origSetPred),
         "Ctx::SetPredication");
    Hook(v[44], reinterpret_cast<void*>(&HookRsViewports), reinterpret_cast<void**>(&g_origRsViewports),
         "Ctx::RSSetViewports");
    Hook(v[45], reinterpret_cast<void*>(&HookRsScissors), reinterpret_cast<void**>(&g_origRsScissors),
         "Ctx::RSSetScissorRects");
    Hook(v[58], reinterpret_cast<void*>(&HookExec), reinterpret_cast<void**>(&g_origExec), "Ctx::ExecuteCommandList");
    Hook(v[50], reinterpret_cast<void*>(&HookClearRtv), reinterpret_cast<void**>(&g_origClearRtv),
         "Ctx::ClearRenderTargetView");
    Hook(v[53], reinterpret_cast<void*>(&HookClearDsv), reinterpret_cast<void**>(&g_origClearDsv),
         "Ctx::ClearDepthStencilView");
    ID3D11DeviceContext* deferred = nullptr;
    if (SUCCEEDED(dev->CreateDeferredContext(0, &deferred))) {
        void** dv = *reinterpret_cast<void***>(deferred);
        Log("game deferred ctx vtable=%p (draw slot %s)", dv, dv[12] == v[12] ? "shared" : "distinct");
        Hook(dv[114], reinterpret_cast<void*>(&HookFinish), reinterpret_cast<void**>(&g_origFinish),
             "DeferredCtx::FinishCommandList");
        deferred->Release();
    }
    ctx->Release();
    dev->Release();
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %-24s %p %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace

void TraceVEntry(const char* fmt, va_list args) { TraceV(fmt, args); }

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "eyedraws") == 0) {
        // "eyedraws <frames> [tag]": average left / right eye draws over the next frames, one line.
        int n = arg ? std::atoi(arg) : 0;
        if (n <= 0) n = 90;
        const char* space = arg ? std::strchr(arg, ' ') : nullptr;
        snprintf(g_measureTag, sizeof(g_measureTag), "%s", space ? space + 1 : "now");
        g_measureLeft = g_measureRight = 0;
        g_measureFrames = n;
        g_measureLeftFrames = n;
        return true;
    }
    if (std::strcmp(verb, "shcount") == 0) {
        g_shCountFrames = arg && std::atoi(arg) > 0 ? std::atoi(arg) : 270;
        return true;
    }
    if (std::strcmp(verb, "occl") == 0) {
        if (arg && std::strcmp(arg, "force") == 0) g_occlForce = true;
        else if (arg && std::strcmp(arg, "off") == 0) g_occlForce = g_occlForceVr = false;
        else if (arg && std::strcmp(arg, "vr") == 0) { g_occlForce = false; g_occlForceVr = true; }
        else if (arg && std::strcmp(arg, "eye0") == 0) g_occlEye0 = true;
        else if (arg && std::strcmp(arg, "botheyes") == 0) g_occlEye0 = false;
        Log("OCCL eye0=%d skipped(right-eye pass)=%llu", int(g_occlEye0.load()), g_occlSkipped.load());
        Log("OCCL force=%d (in VR %d) GetData: occlusion=%llu (not ready %llu) predicate=%llu other=%llu "
            "SetPredication(non-null)=%llu",
            int(g_occlForce.load()), int(g_occlForceVr.load()), g_qOccl.load(), g_qNotReady.load(), g_qPred.load(), g_qOther.load(), g_qPredSet.load());
        return true;
    }
    if (std::strcmp(verb, "frametrace") != 0) return false;
    const int n = arg && arg[0] ? std::atoi(arg) : 1500;
    g_traceLastDraws = g_drawOtherThread.load() + g_drawOutList.load() + g_drawInList.load();
    g_traceBudget = n;
    Log("TRACE armed for %d events", n);
    return true;
}

void Install() {
    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed");
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dump(exe);
    const std::wstring dir = dump.substr(0, dump.find_last_of(L"\\/"));
    // Camera dumps are for reverse engineering only (about 50 MB an hour): b2svr.ini devdumps=on.
    wchar_t dumps[16] = L"";
    GetPrivateProfileStringW(L"b2svr", L"devdumps", L"off", dumps, 16, (dir + L"\\b2svr.ini").c_str());
    if (_wcsicmp(dumps, L"on") == 0) {
        dump = dir + L"\\b2svr_logs\\b2svr_camdump.bin";
        g_camDump = _wfsopen(dump.c_str(), L"wb", _SH_DENYWR);
    }
    Log("camera dump %s (b2svr.ini devdumps)", g_camDump ? "on" : "off");

    FindRdata();
    pacing::Install();
    InstallMarkerSeam();
    stereo::Install();
    void* traceOrig = nullptr;
    Hook(Addr(Anchor::EngineTrace), reinterpret_cast<void*>(&B2SVR_TraceThunk), &traceOrig, "ENGINE::Trace");
    Hook(Addr(Anchor::ViewportListDisplay), reinterpret_cast<void*>(&HookListDisplay),
         reinterpret_cast<void**>(&g_origListDisplay), "VIEWPORT_LIST::Display");
    Hook(Addr(Anchor::CameraBuild), reinterpret_cast<void*>(&HookCamBuild),
         reinterpret_cast<void**>(&g_origCamBuild), "CAMERA::Build");
    Hook(Addr(Anchor::ViewportCameraSetup), reinterpret_cast<void*>(&HookCamSetup),
         reinterpret_cast<void**>(&g_origCamSetup), "VIEWPORT::CameraSetup");
    Hook(Addr(Anchor::SceneDisplay), reinterpret_cast<void*>(&HookScene), reinterpret_cast<void**>(&g_origScene),
         "SCENE::FrameUpdate");

    D3DSlots s;
    if (FindD3DSlots(&s)) {
        Hook(s.present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent),
             "IDXGISwapChain::Present");
        Hook(s.present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_origPresent1),
             "IDXGISwapChain1::Present1");
    }
    Log("probe installed");
}

}  // namespace b2s::probe

extern "C" void B2SVR_TraceV(const char* fmt, va_list args) { b2s::probe::TraceVEntry(fmt, args); }
