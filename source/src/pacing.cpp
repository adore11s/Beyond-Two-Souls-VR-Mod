#include "pacing.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"
#include "log.h"

namespace b2s::pacing {
namespace {

// ADDR-021 frame-rate mode setter (mode): stores the mode at 0x141b996fc and the limiter's
// minimum frame time in ms at 0x141c8b350: 0 -> 33.4 (30 FPS), 1 -> 16.7 (60 FPS),
// 2 -> 0 (limiter off). Called from the options loader and a Lua binding. The guard covers
// the whole function, so both RIP-relative globals below are verified by it.
constexpr uintptr_t kSetterVa = 0x1403b3770;
constexpr uint8_t kSetterGuard[] = {
    0x89, 0x0d, 0x86, 0x5f, 0x7e, 0x01, 0x85, 0xc9, 0x74, 0x31, 0x83, 0xe9, 0x01, 0x74, 0x22, 0x83,
    0xf9, 0x01, 0x75, 0x48, 0x0f, 0x57, 0xc0, 0x48, 0x8b, 0x05, 0xfa, 0x4f, 0x8f, 0x01, 0xf3, 0x0f,
    0x11, 0x05, 0xba, 0x7b, 0x8d, 0x01, 0xc7, 0x80, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x42,
    0xc3, 0xf3, 0x0f, 0x10, 0x05, 0x5f, 0x98, 0x3f, 0x01, 0xeb, 0xdc, 0x48, 0x8b, 0x05, 0xd6, 0x4f,
    0x8f, 0x01, 0xf3, 0x0f, 0x10, 0x05, 0x8a, 0x98, 0x3f, 0x01, 0xf3, 0x0f, 0x11, 0x05, 0x8e, 0x7b,
    0x8d, 0x01, 0xc7, 0x80, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x41, 0xc3};
// ADDR-022 the limiter (0x140c7b6f0) reads the frame time here and skips its Sleep/spin
// loop when the value is <= 0.
constexpr uintptr_t kLimiterReadVa = 0x140c7b735;
constexpr uint8_t kLimiterReadGuard[] = {0xf3, 0x0f, 0x10, 0x05, 0x13, 0xfc, 0x00, 0x01,
                                         0x0f, 0x57, 0xff, 0x0f, 0x2f, 0xc7, 0xf2, 0x44};
constexpr uintptr_t kModeVa = 0x141b996fc;       // int: 0 = 30, 1 = 60, 2 = unlimited
constexpr uintptr_t kFrameTimeVa = 0x141c8b350;  // float ms, <= 0 disables the limiter

// fps setting: kGame leaves the game's limiter alone (except while XR runs); 0 = off.
constexpr int kGame = -1;
std::atomic<int> g_fps{0};
std::atomic<bool> g_vsyncOff{true};
bool g_ok = false;
float* g_frameTime = nullptr;
const int* g_mode = nullptr;
std::atomic<int> g_lastLogged{-2};

template <typename T>
T* At(uintptr_t va) {
    return reinterpret_cast<T*>(game::Va(va));
}

// The limiter value the game itself would use for its current mode.
float GameFrameTime(float current) {
    switch (*g_mode) {
        case 0: return 33.4f;
        case 1: return 16.7f;
        case 2: return 0.0f;
        default: return current;
    }
}

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\b2svr.ini";
}

int ParseFps(const char* s) {
    if (!s || !*s || _stricmp(s, "game") == 0) return kGame;
    if (_stricmp(s, "off") == 0 || _stricmp(s, "unlimited") == 0) return 0;
    const int v = std::atoi(s);
    return v > 0 ? v : 0;
}

}  // namespace

void Install() {
    const std::wstring ini = IniPath();
    char buf[32];
    wchar_t wbuf[32];
    GetPrivateProfileStringW(L"b2svr", L"fps", L"0", wbuf, 32, ini.c_str());
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
    g_fps = ParseFps(buf);
    GetPrivateProfileStringW(L"b2svr", L"vsync", L"off", wbuf, 32, ini.c_str());
    g_vsyncOff = _wcsicmp(wbuf, L"game") != 0 && _wcsicmp(wbuf, L"on") != 0;

    // The setter and limiter read are located by signature (sigdb) in any build; the full-function
    // guards below also cover the RIP displacements, so they are only required on the analysed build.
    (void)kSetterGuard;
    (void)kLimiterReadGuard;
    g_ok = At<uint8_t>(kSetterVa) && At<uint8_t>(kLimiterReadVa) && At<float>(kFrameTimeVa) && At<const int>(kModeVa);
    if (g_ok) {
        g_frameTime = At<float>(kFrameTimeVa);
        g_mode = At<const int>(kModeVa);
    }
    Log("PACING limiter guards %s; fps=%d (-1=game, 0=off) vsync=%s", g_ok ? "OK" : "MISMATCH (limiter left alone)",
        g_fps.load(), g_vsyncOff ? "off" : "game");
}

unsigned OnPresent(unsigned gameSync, bool xrRunning) {
    if (g_ok) {
        const int fps = g_fps;
        const float current = *g_frameTime;
        float want;
        if (fps == kGame) want = xrRunning ? 0.0f : GameFrameTime(current);  // headset paces XR
        else want = fps > 0 ? 1000.0f / float(fps) : 0.0f;
        if (current != want) {
            *g_frameTime = want;
            if (g_lastLogged.exchange(fps) != fps)
                Log("PACING limiter %.2fms -> %.2fms (mode=%d xr=%d)", current, want, *g_mode, xrRunning);
        }
    }
    return (xrRunning || g_vsyncOff) ? 0u : gameSync;
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "fps") == 0) {
        g_fps = ParseFps(arg);
        g_lastLogged = -2;
        Log("PACING fps=%d (-1=game, 0=off)", g_fps.load());
        return true;
    }
    if (std::strcmp(verb, "vsync") == 0) {
        g_vsyncOff = !(arg && (_stricmp(arg, "game") == 0 || _stricmp(arg, "on") == 0));
        Log("PACING vsync=%s", g_vsyncOff ? "off" : "game");
        return true;
    }
    return false;
}

}  // namespace b2s::pacing
