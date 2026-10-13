#include "vrinput.h"
#include <windows.h>
#include <Xinput.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include "game.h"
#include "log.h"
#include "steam_shadow.h"

namespace b2s::vrinput {
namespace {
constexpr uint64_t kController = 0xB25A000000000001ull;
constexpr uint64_t kDigitalBase = 0xB25A100000000000ull;
constexpr uint64_t kAnalogBase = 0xB25A200000000000ull;
constexpr int kMaxControllers = 16;
constexpr const char* kDigitalNames[] = {
    "button_start", "button_back", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "button_a", "button_b", "button_x", "button_y", "left_bumper",
    "left_trigger", "left_stick_click", "right_bumper", "right_trigger", "right_stick_click"
};
struct DigitalData { uint8_t state, active; };
struct AnalogData { int mode; float x, y; uint8_t active; uint8_t padding[3]; };
// Steam InputMotionData_t: rotQuat x y z w, posAccel x y z, rotVel x y z.
struct MotionData { float rotQuat[4], posAccel[3], rotVel[3]; };
static_assert(sizeof(DigitalData)==2 && sizeof(AnalogData)==16 && sizeof(MotionData)==40);

using ApiUpdate = void (*)(void*);
using PadCreate = void* (*)(void*, uint64_t);
using Connected = int (*)(void*, uint64_t*);
using HandleByName = uint64_t (*)(void*, const char*);
using Activate = void (*)(void*, uint64_t, uint64_t);
// STATIC: game callers supply explicit result buffers in RDX, then controller
// and action in R8/R9, including the two-byte digital result.
using Digital = DigitalData* (*)(void*, DigitalData*, uint64_t, uint64_t);
using Analog = AnalogData* (*)(void*, AnalogData*, uint64_t, uint64_t);
using Motion = MotionData* (*)(void*, MotionData*, uint64_t);
using Vibration = void (*)(void*, uint64_t, uint16_t, uint16_t);
using InputType = int (*)(void*, uint64_t);
ApiUpdate g_apiUpdate = nullptr;
PadCreate g_padCreate = nullptr;
Connected g_connected = nullptr;
HandleByName g_digitalHandle = nullptr, g_analogHandle = nullptr;
Activate g_activate = nullptr;
Digital g_digital = nullptr;
Analog g_analog = nullptr;
Motion g_motion = nullptr;
Vibration g_vibration = nullptr;
InputType g_inputType = nullptr;
void* g_context = nullptr;
detail::ShadowTable g_shadow;
std::atomic<bool> g_contextReady{false};
std::atomic<uint64_t> g_digitalQueries{0}, g_digitalActive{0};
std::atomic<uint64_t> g_analogQueries{0}, g_analogNonzero{0};
std::mutex g_installMutex, g_sampleMutex;
Sample g_sample{};
bool g_gameplay = false;
std::atomic<int> g_padType{2};
std::atomic<uint64_t> g_unplugUntil{0};
uint64_t g_sampleTick = 0;
thread_local bool g_enumerating = false, g_virtualConstructor = false;

bool ExecutableSteamFunction(void* function) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!function || !VirtualQuery(function, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
        (mbi.Protect & PAGE_GUARD)) return false;
    wchar_t path[MAX_PATH]{};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(function), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH)) return false;
    const wchar_t* name = wcsrchr(path,L'\\');
    name = name ? name+1 : path;
    return _wcsicmp(name,L"steam_api64.dll")==0 || _wcsicmp(name,L"steamclient64.dll")==0;
}

// Guarded RIP-relative instructions prove both context-data and import operands.
void* ReadSteamContext() {
    __try {
        auto* code = static_cast<uint8_t*>(game::Addr(game::Anchor::SteamContextAccess));
        if (!code) return nullptr;
        void* data = code+7+*reinterpret_cast<int32_t*>(code+3);
        auto* import = code+13+*reinterpret_cast<int32_t*>(code+9);
        using Init = void** (*)(void*);
        auto init = *reinterpret_cast<Init*>(import);
        if (!ExecutableSteamFunction(reinterpret_cast<void*>(init))) return nullptr;
        void** wrapper = init(data);
        return wrapper ? *wrapper : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool ReadMethods(void* context, void** methods, void*** originalTable) {
    __try {
        void** vt = *reinterpret_cast<void***>(context);
        *originalTable=vt;
        constexpr int slots[]{3,5,11,12,14,15,20,23,26};
        for (size_t i=0;i<std::size(slots);++i) {
            methods[i] = vt[slots[i]];
            if (!ExecutableSteamFunction(methods[i])) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildAndPublishTable(void* context, void** expected, void* const* detours) {
    __try {
        // This copy is bounded to 520 bytes, with all pointer faults contained.
        // Preserve the RTTI entry and unmodified slots; only this object changes.
        g_shadow.Build(expected,detours);
        DWORD oldProtect=0;
        if (!VirtualProtect(context,sizeof(void*),PAGE_READWRITE,&oldProtect)) return false;
        auto* slot=reinterpret_cast<void* volatile*>(context);
        void* old=InterlockedCompareExchangePointer(slot,g_shadow.Table(),expected);
        DWORD ignored=0;
        VirtualProtect(context,sizeof(void*),oldProtect,&ignored);
        return old==expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Synthetic(void* context, uint64_t handle) {
    return g_contextReady.load(std::memory_order_acquire) && handle==kController && context==g_context;
}

int HookConnected(void* context, uint64_t* handles) {
    const int count = g_connected(context, handles);
    if (!g_enumerating || !g_contextReady.load(std::memory_order_acquire) || context!=g_context ||
        !xr::Running() || !handles || count<0 || count>=kMaxControllers) return count;
    if (GetTickCount64() < g_unplugUntil.load()) return count;  // type switch: let the game drop the pad
    for (int i=0;i<count;++i) if (handles[i]==kController) return count;
    handles[count]=kController;
    return count+1;
}
void HookActivate(void* context, uint64_t handle, uint64_t set) {
    if (!Synthetic(context,handle)) g_activate(context,handle,set);
}
uint64_t HookDigitalHandle(void* context, const char* name) {
    if (g_virtualConstructor && context==g_context && name)
        for (size_t i=0;i<std::size(kDigitalNames);++i)
            if (std::strcmp(name,kDigitalNames[i])==0) return kDigitalBase+i+1;
    return g_digitalHandle(context,name);
}
uint64_t HookAnalogHandle(void* context, const char* name) {
    if (g_virtualConstructor && context==g_context && name) {
        if (std::strcmp(name,"left_stick")==0) return kAnalogBase+1;
        if (std::strcmp(name,"right_stick")==0) return kAnalogBase+2;
    }
    return g_analogHandle(context,name);
}
bool Button(const LegacyState& state, uint64_t action) {
    if (action<=kDigitalBase || action>kDigitalBase+std::size(kDigitalNames)) return false;
    switch (action-kDigitalBase-1) {
        // Touch-style controllers (Quest, PSVR2 through SteamVR) have one menu button, on the
        // left hand: it is Start (pause). Held left grip + menu is Back.
        case 0: return (state.buttons&kRightMenu)!=0 ||
                       ((state.buttons&kLeftMenu)!=0 && (state.buttons&kLeftSqueeze)==0);
        case 1: return (state.buttons&kLeftMenu)!=0 && (state.buttons&kLeftSqueeze)!=0;
        case 2: return (state.buttons&kDpadUp)!=0;
        case 3: return (state.buttons&kDpadDown)!=0;
        case 4: return (state.buttons&kDpadLeft)!=0;
        case 5: return (state.buttons&kDpadRight)!=0;
        case 6: return (state.buttons&kRightA)!=0;
        case 7: return (state.buttons&kRightB)!=0;
        case 8: return (state.buttons&kLeftA)!=0;
        case 9: return (state.buttons&kLeftB)!=0;
        case 10: return (state.buttons&(kLeftGrip|kLeftSqueeze))!=0;
        case 11: return (state.buttons&kLeftTriggerButton)!=0;
        case 12: return (state.buttons&kLeftStickClick)!=0;
        case 13: return (state.buttons&(kRightGrip|kRightSqueeze))!=0;
        case 14: return (state.buttons&kRightTriggerButton)!=0;
        case 15: return (state.buttons&kRightStickClick)!=0;
        default: return false;
    }
}
DigitalData* HookDigital(void* context, DigitalData* output, uint64_t handle, uint64_t action) {
    if (!Synthetic(context,handle)) return g_digital(context,output,handle,action);
    g_digitalQueries.fetch_add(1, std::memory_order_relaxed);
    const bool pressed = Button(Snapshot(),action);
    if (pressed) g_digitalActive.fetch_add(1, std::memory_order_relaxed);
    *output = {uint8_t(pressed),1};
    return output;
}
AnalogData* HookAnalog(void* context, AnalogData* output, uint64_t handle, uint64_t action) {
    if (!Synthetic(context,handle)) return g_analog(context,output,handle,action);
    g_analogQueries.fetch_add(1, std::memory_order_relaxed);
    const LegacyState state = Snapshot();
    *output = {6,0.f,0.f,1,{0,0,0}}; // legacy joystick-move mode; X/Y at +4/+8
    if (action==kAnalogBase+1) { output->x=state.moveX; output->y=state.moveY; }
    else if (action==kAnalogBase+2) { output->x=state.viewX; output->y=state.viewY; }
    if (output->x != 0.f || output->y != 0.f) g_analogNonzero.fetch_add(1, std::memory_order_relaxed);
    return output;
}
// PlayStation motion (0.16.41, owner: "when you switch to PlayStation controls it expects gyro in the
// controller; some prompts need me to move my controller down / up / left / right"). STATIC: the game's
// Steam pad (INPUT_DEVICE_STEAM_PAD, FUN_140c01390) reads GetMotionData every frame; for a pad Steam calls
// a PS4 controller (GetInputTypeForHandle 5 -> its kind 1, FUN_140c01d80) it turns posAccel into the
// SIXAXIS-style channels SensorX / Y / Z / G (20 - 23): x = -ax, y = 2 * -ay + 16384, z = -az, each
// * 113 / 16384 (113 counts per g, PS3 style), centred on 512 and divided by 512 (y also - 30): DS4 raw
// units, 8192 per g, the pad reading +1 g on +Y when it lies flat (y then sits at its centre). Here the
// right VR controller is that pad: its aim frame (X right, Y up, Z toward the player, as a DS4 held in
// front of you) reads the specific force - gravity's opposite plus its own acceleration - so tilting it or
// moving it up / down / left / right reads as on a DS4. Its orientation goes in rotQuat; rotVel stays 0
// (this path does not read it). Only while the pad is PlayStation (F11); Xbox keeps the still pad.
struct MotionState {
    bool valid = false;
    uint64_t tick = 0;
    float pos[3]{}, vel[3]{}, acc[3]{};  // LOCAL space, metres / s / s^2 (smoothed)
    float force[3]{};                    // specific force in the controller frame, g
    float quat[4]{0.f, 0.f, 0.f, 1.f};
} g_motionState;
std::mutex g_motionMutex;
std::atomic<uint64_t> g_motionReads{0};

void UpdateMotion(const xr::FrameInput& frame) {
    const xr::HandInput& h = frame.hand[1].aimValid ? frame.hand[1] : frame.hand[0];
    std::lock_guard lock(g_motionMutex);
    MotionState& m = g_motionState;
    if (!h.aimValid || !frame.tickMs) {
        m.valid = false;
        return;
    }
    const float* a = h.aimRel.m;
    const float p[3] = {a[12], a[13], a[14]};
    const float dt = m.valid && frame.tickMs > m.tick ? std::min(float(frame.tickMs - m.tick) / 1000.f, 0.1f) : 0.f;
    if (dt > 0.f) {
        // velocity and acceleration from the positions, each smoothed over ~30 ms
        const float k = 1.f - std::exp(-dt / 0.03f);
        for (int i = 0; i < 3; ++i) {
            const float v = (p[i] - m.pos[i]) / dt;
            const float acc = (v - m.vel[i]) / dt;
            m.vel[i] += (v - m.vel[i]) * k;
            m.acc[i] += (std::clamp(acc, -30.f, 30.f) - m.acc[i]) * k;
        }
    } else {
        for (int i = 0; i < 3; ++i) m.vel[i] = m.acc[i] = 0.f;
    }
    for (int i = 0; i < 3; ++i) m.pos[i] = p[i];
    m.tick = frame.tickMs;
    m.valid = true;
    // specific force (what an accelerometer reads), g: up 1 g when still, plus the hand's acceleration
    const float f[3] = {m.acc[0] / 9.81f, 1.f + m.acc[1] / 9.81f, m.acc[2] / 9.81f};
    // into the controller frame (row-major rows = its X / Y / Z axes in LOCAL space)
    for (int r = 0; r < 3; ++r) {
        const float ax = a[r * 4], ay = a[r * 4 + 1], az = a[r * 4 + 2];
        const float len = std::sqrt(ax * ax + ay * ay + az * az);
        m.force[r] = len > 1e-4f ? (f[0] * ax + f[1] * ay + f[2] * az) / len : 0.f;
    }
    // rotation matrix -> quaternion (row-vector convention: rows are the rotated axes)
    const float r00 = a[0], r01 = a[1], r02 = a[2], r10 = a[4], r11 = a[5], r12 = a[6], r20 = a[8], r21 = a[9], r22 = a[10];
    const float tr = r00 + r11 + r22;
    float q[4];
    if (tr > 0.f) {
        const float sq = std::sqrt(tr + 1.f) * 2.f;
        q[3] = 0.25f * sq; q[0] = (r12 - r21) / sq; q[1] = (r20 - r02) / sq; q[2] = (r01 - r10) / sq;
    } else if (r00 > r11 && r00 > r22) {
        const float sq = std::sqrt(1.f + r00 - r11 - r22) * 2.f;
        q[3] = (r12 - r21) / sq; q[0] = 0.25f * sq; q[1] = (r01 + r10) / sq; q[2] = (r20 + r02) / sq;
    } else if (r11 > r22) {
        const float sq = std::sqrt(1.f + r11 - r00 - r22) * 2.f;
        q[3] = (r20 - r02) / sq; q[0] = (r01 + r10) / sq; q[1] = 0.25f * sq; q[2] = (r12 + r21) / sq;
    } else {
        const float sq = std::sqrt(1.f + r22 - r00 - r11) * 2.f;
        q[3] = (r01 - r10) / sq; q[0] = (r20 + r02) / sq; q[1] = (r12 + r21) / sq; q[2] = 0.25f * sq;
    }
    const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 1e-4f && std::isfinite(ql))
        for (int i = 0; i < 4; ++i) m.quat[i] = q[i] / ql;
}

MotionData* HookMotion(void* context, MotionData* output, uint64_t handle) {
    if (!Synthetic(context,handle)) return g_motion(context,output,handle);
    *output = {};
    output->rotQuat[3]=1.f;
    if (g_padType.load(std::memory_order_relaxed) != 5) return output;
    std::lock_guard lock(g_motionMutex);
    const MotionState& m = g_motionState;
    const uint64_t now = GetTickCount64();
    if (!m.valid || now < m.tick || now - m.tick > 250) {
        output->posAccel[1] = 8192.f;  // a pad lying still and flat
        return output;
    }
    for (int i = 0; i < 4; ++i) output->rotQuat[i] = m.quat[i];
    for (int i = 0; i < 3; ++i) output->posAccel[i] = std::clamp(m.force[i] * 8192.f, -32767.f, 32767.f);
    if (g_motionReads.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("VRINPUT PlayStation motion: the right controller is the pad's motion sensor (accel %.0f %.0f %.0f)",
            output->posAccel[0], output->posAccel[1], output->posAccel[2]);
    return output;
}
void HookVibration(void* context, uint64_t handle, uint16_t right, uint16_t left) {
    if (!Synthetic(context,handle)) g_vibration(context,handle,right,left);
}
int HookInputType(void* context, uint64_t handle) {
    // The game maps type 1 to Steam-controller gyro/flick processing and 5 to
    // PlayStation processing. Type 2 uses its ordinary gamepad path.
    return Synthetic(context,handle) ? g_padType.load() : g_inputType(context,handle);
}

bool InstallContextHooks() {
    if (g_contextReady.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(g_installMutex);
    if (g_contextReady.load(std::memory_order_relaxed)) return true;
    void* context = ReadSteamContext();
    void* targets[9]{};
    void** originalTable=nullptr;
    if (!context || !ReadMethods(context,targets,&originalTable)) return false;
    void* detours[]{reinterpret_cast<void*>(&HookConnected),reinterpret_cast<void*>(&HookActivate),
        reinterpret_cast<void*>(&HookDigitalHandle),reinterpret_cast<void*>(&HookDigital),
        reinterpret_cast<void*>(&HookAnalogHandle),reinterpret_cast<void*>(&HookAnalog),
        reinterpret_cast<void*>(&HookMotion),reinterpret_cast<void*>(&HookVibration),reinterpret_cast<void*>(&HookInputType)};
    void** originals[]{reinterpret_cast<void**>(&g_connected),reinterpret_cast<void**>(&g_activate),
        reinterpret_cast<void**>(&g_digitalHandle),reinterpret_cast<void**>(&g_digital),
        reinterpret_cast<void**>(&g_analogHandle),reinterpret_cast<void**>(&g_analog),
        reinterpret_cast<void**>(&g_motion),reinterpret_cast<void**>(&g_vibration),reinterpret_cast<void**>(&g_inputType)};
    for (size_t i=0;i<std::size(targets);++i) *originals[i]=targets[i];
    if (!BuildAndPublishTable(context,originalTable,detours)) {
        static bool reported=false;
        if (!reported) {
            Log("VRINPUT Steam interface table unavailable; native input retained");
            reported=true;
        }
        return false;
    }
    g_context=context;
    g_contextReady.store(true,std::memory_order_release);
    Log("VRINPUT Steam controller interface virtualized with instance table; headset input checks pending");
    return true;
}
void HookApiUpdate(void* api) {
    const bool previous=g_enumerating;
    g_enumerating=InstallContextHooks();
    g_apiUpdate(api);
    g_enumerating=previous;
}
void* HookPadCreate(void* pad, uint64_t handle) {
    const bool previous=g_virtualConstructor;
    g_virtualConstructor=handle==kController && g_contextReady.load(std::memory_order_acquire);
    void* result=g_padCreate(pad,handle);
    g_virtualConstructor=previous;
    if (handle==kController) Log("VRINPUT native Steam pad created for motion controllers");
    return result;
}
} // namespace

// ---- XInput path (builds without Steam, e.g. the Epic Games Store copy) ----------------------------
// LIVE Epic demo: the Steam input seams do not exist ("Steam anchors unavailable"); the exe imports
// XINPUT1_3 (by ordinal) for gamepads. The VR controllers are merged into XInput user 0 with the
// same button mapping as the virtual Steam pad. Only installed when the Steam path is unavailable,
// so a Steam copy never sees the controllers twice.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
XInputGetStateFn g_xiGetState = nullptr, g_xiGetStateEx = nullptr;
XInputGetCapsFn g_xiGetCaps = nullptr;
std::atomic<DWORD> g_xiPacket{0};
std::atomic<uint64_t> g_xiReads{0};

WORD XInputButtons(const LegacyState& st) {
    WORD b = 0;
    auto set = [&](uint64_t action, WORD bit) { if (Button(st, kDigitalBase + 1 + action)) b |= bit; };
    set(0, XINPUT_GAMEPAD_START);
    set(1, XINPUT_GAMEPAD_BACK);
    set(2, XINPUT_GAMEPAD_DPAD_UP);
    set(3, XINPUT_GAMEPAD_DPAD_DOWN);
    set(4, XINPUT_GAMEPAD_DPAD_LEFT);
    set(5, XINPUT_GAMEPAD_DPAD_RIGHT);
    set(6, XINPUT_GAMEPAD_A);
    set(7, XINPUT_GAMEPAD_B);
    set(8, XINPUT_GAMEPAD_X);
    set(9, XINPUT_GAMEPAD_Y);
    set(10, XINPUT_GAMEPAD_LEFT_SHOULDER);
    set(12, XINPUT_GAMEPAD_LEFT_THUMB);
    set(13, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    set(15, XINPUT_GAMEPAD_RIGHT_THUMB);
    return b;
}
SHORT Axis(float v) { return SHORT(Clamp(v) * 32767.f); }
// The VR stick wins when it is pushed further than the real pad's.
void MergeStick(SHORT& x, SHORT& y, float vx, float vy) {
    const float rx = x / 32767.f, ry = y / 32767.f;
    if (vx * vx + vy * vy > rx * rx + ry * ry) { x = Axis(vx); y = Axis(vy); }
}
DWORD Merge(DWORD user, DWORD result, XINPUT_STATE* state) {
    if (user != 0 || !state || !xr::Running()) return result;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        Log("VRINPUT XInput: the game polls pad 1 (real pad there: %s); VR controllers merged in",
            result == ERROR_SUCCESS ? "yes" : "no");
    const LegacyState st = Snapshot();
    const WORD buttons = XInputButtons(st);
    const bool vrActive = buttons || st.moveX || st.moveY || st.viewX || st.viewY || st.leftTrigger || st.rightTrigger;
    if (result != ERROR_SUCCESS) {
        *state = {};
        result = ERROR_SUCCESS;  // the VR controllers are a connected pad of their own
    }
    XINPUT_GAMEPAD& g = state->Gamepad;
    g.wButtons |= buttons;
    g.bLeftTrigger = std::max<BYTE>(g.bLeftTrigger, BYTE(Clamp(st.leftTrigger, 0.f, 1.f) * 255.f));
    g.bRightTrigger = std::max<BYTE>(g.bRightTrigger, BYTE(Clamp(st.rightTrigger, 0.f, 1.f) * 255.f));
    MergeStick(g.sThumbLX, g.sThumbLY, st.moveX, st.moveY);
    MergeStick(g.sThumbRX, g.sThumbRY, st.viewX, st.viewY);
    // A new packet number whenever the VR controllers are in use: games skip unchanged packets.
    if (vrActive) state->dwPacketNumber += g_xiPacket.fetch_add(1, std::memory_order_relaxed) + 1;
    g_xiReads.fetch_add(1, std::memory_order_relaxed);
    return result;
}
DWORD WINAPI HookXInputGetState(DWORD user, XINPUT_STATE* state) { return Merge(user, g_xiGetState(user, state), state); }
DWORD WINAPI HookXInputGetStateEx(DWORD user, XINPUT_STATE* state) { return Merge(user, g_xiGetStateEx(user, state), state); }
DWORD WINAPI HookXInputGetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    const DWORD r = g_xiGetCaps(user, flags, caps);
    if (user != 0 || r == ERROR_SUCCESS || !caps || !xr::Running()) return r;
    *caps = {};
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = SHORT(0x7FFF);
    return ERROR_SUCCESS;
}
bool InstallXInput() {
    static bool done = false;
    if (done) return g_xiGetState != nullptr;
    done = true;
    HMODULE xi = nullptr;
    for (const wchar_t* name : {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"})
        if ((xi = GetModuleHandleW(name)) != nullptr) break;
    if (!xi) xi = LoadLibraryW(L"xinput1_3.dll");
    if (!xi) { Log("VRINPUT XInput not available: VR controllers cannot reach the game"); return false; }
    auto hook = [](void* target, void* detour, void** original) {
        return target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    };
    const bool state = hook(reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState")),
                            reinterpret_cast<void*>(&HookXInputGetState), reinterpret_cast<void**>(&g_xiGetState));
    const bool caps = hook(reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetCapabilities")),
                           reinterpret_cast<void*>(&HookXInputGetCaps), reinterpret_cast<void**>(&g_xiGetCaps));
    // Ordinal 100: XInputGetStateEx (adds the guide button), used by some games.
    const bool ex = hook(reinterpret_cast<void*>(GetProcAddress(xi, MAKEINTRESOURCEA(100))),
                         reinterpret_cast<void*>(&HookXInputGetStateEx), reinterpret_cast<void**>(&g_xiGetStateEx));
    Log("VRINPUT XInput path: GetState=%d GetCapabilities=%d GetStateEx=%d (VR controllers act as XInput pad 1)",
        int(state), int(caps), int(ex));
    return state;
}

std::wstring IniPath();
bool Install() {
    std::lock_guard lock(g_installMutex);
    if (g_apiUpdate && g_padCreate) return true;
    {   // the controller type chosen last time (F10 Xbox / F11 PlayStation)
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"b2svr", L"controller", L"xbox", value, 32, IniPath().c_str());
        g_padType = _wcsicmp(value, L"playstation") == 0 ? 5 : 2;
        Log("VRINPUT controller type from b2svr.ini: %s", g_padType.load() == 5 ? "PlayStation" : "Xbox");
    }
    void* api=game::Addr(game::Anchor::SteamApiUpdate);
    void* create=game::Addr(game::Anchor::SteamPadCreate);
    if (!api || !create || !game::Addr(game::Anchor::SteamContextAccess)) {
        Log("VRINPUT Steam anchors unavailable (not a Steam build): using XInput");
        return InstallXInput();
    }
    if (MH_CreateHook(api,reinterpret_cast<void*>(&HookApiUpdate),reinterpret_cast<void**>(&g_apiUpdate))!=MH_OK)
        return false;
    if (MH_CreateHook(create,reinterpret_cast<void*>(&HookPadCreate),reinterpret_cast<void**>(&g_padCreate))!=MH_OK ||
        MH_EnableHook(create)!=MH_OK || MH_EnableHook(api)!=MH_OK) {
        MH_DisableHook(create); MH_RemoveHook(create); MH_RemoveHook(api);
        g_apiUpdate=nullptr; g_padCreate=nullptr;
        return false;
    }
    Log("VRINPUT guarded Steam enumeration/create hooks installed");
    return true;
}
void Suspend() {
    std::lock_guard lock(g_sampleMutex);
    g_sample={}; g_sampleTick=0;
}
void Publish(const xr::FrameInput& frame, bool firstPersonGameplay, float cameraRelativeHeadYaw) {
    std::lock_guard lock(g_sampleMutex);
    g_sample=SampleFromFrame(frame,GetTickCount64());
    g_sample.headYaw=cameraRelativeHeadYaw;
    g_sampleTick=frame.tickMs;
    g_gameplay=firstPersonGameplay;
    UpdateMotion(frame);
}
LegacyState Snapshot() {
    std::lock_guard lock(g_sampleMutex);
    const uint64_t now=GetTickCount64();
    if (!xr::Running() || !g_sampleTick || now<g_sampleTick || now-g_sampleTick>250) return {};
    return Map(g_sample,g_gameplay);
}
// b2svr.ini next to the game exe: the F10 / F11 choice is kept there (owner: it was forgotten on exit).
std::wstring IniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1) + L"b2svr.ini";
}
void SetPadType(int type) {
    if (type != 2 && type != 5) return;
    g_padType = type;
    g_unplugUntil = GetTickCount64() + 1200;
    WritePrivateProfileStringW(L"b2svr", L"controller", type == 5 ? L"playstation" : L"xbox", IniPath().c_str());
    Log("VRINPUT controller type -> %s (virtual pad re-plugged, saved to b2svr.ini)", type == 5 ? "PlayStation" : "Xbox");
}
int PadType() { return g_padType.load(); }
void LogStatus() {
    static std::atomic<uint64_t> last{0};
    const uint64_t now = GetTickCount64();
    uint64_t expected = last.load(std::memory_order_relaxed);
    if (expected && now - expected < 250) return;
    if (!last.compare_exchange_strong(expected, now, std::memory_order_relaxed)) return;
    Sample sample{}; uint64_t tick = 0; bool gameplay = false;
    {
        std::lock_guard lock(g_sampleMutex);
        sample = g_sample; tick = g_sampleTick; gameplay = g_gameplay;
    }
    char left[128] = "<none>", right[128] = "<none>";
    xr::GetInteractionProfile(0, left, sizeof(left));
    xr::GetInteractionProfile(1, right, sizeof(right));
    const uint64_t age = tick && now >= tick ? now - tick : UINT64_MAX;
    Log("VRINPUT status focus=%d fresh=%d age=%llu gameplay=%d L(grip=%d stick=%.2f,%.2f trig=%.2f) R(grip=%d stick=%.2f,%.2f trig=%.2f) profileL=%s profileR=%s steam(ctx=%d dq=%llu da=%llu aq=%llu an=%llu)",
        sample.focused ? 1 : 0, sample.fresh ? 1 : 0, static_cast<unsigned long long>(age), gameplay ? 1 : 0,
        sample.leftGrip ? 1 : 0, sample.leftX, sample.leftY, sample.leftTrigger,
        sample.rightGrip ? 1 : 0, sample.rightX, sample.rightY, sample.rightTrigger,
        left, right, g_contextReady.load() ? 1 : 0,
        static_cast<unsigned long long>(g_digitalQueries.load()), static_cast<unsigned long long>(g_digitalActive.load()),
        static_cast<unsigned long long>(g_analogQueries.load()), static_cast<unsigned long long>(g_analogNonzero.load()));
}
} // namespace b2s::vrinput
