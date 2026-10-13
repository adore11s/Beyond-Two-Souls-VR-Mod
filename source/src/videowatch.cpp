#include "videowatch.h"

#include <windows.h>

#include <atomic>

#include <MinHook.h>

#include "log.h"

namespace b2s::videowatch {
namespace {

// Bink 2 SDK signatures (x64 has one calling convention).
using OpenFn = void*(WINAPI*)(const char* name, uint32_t flags);
using NextFrameFn = void(WINAPI*)(void* bink);
using DoFrameAsyncFn = int32_t(WINAPI*)(void* bink, uint32_t yThread, uint32_t otherThread);
using CloseFn = void(WINAPI*)(void* bink);
OpenFn g_open = nullptr;
NextFrameFn g_nextFrame = nullptr;
DoFrameAsyncFn g_doFrameAsync = nullptr;
CloseFn g_close = nullptr;

constexpr uint32_t kBinkFileHandle = 0x00800000;  // BINKFILEHANDLE: `name` is a HANDLE
constexpr uint64_t kPlayingMs = 400;
std::atomic<uint64_t> g_lastFrame{0};

void Copy(const char* s, char* out, size_t cap) {
    out[0] = 0;
    __try {
        size_t i = 0;
        for (; s && i + 1 < cap && s[i]; ++i) out[i] = s[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}
// HBINK starts with U32 Width, U32 Height, U32 Frames (Bink SDK BINK struct).
bool Size(void* bink, uint32_t* w, uint32_t* h, uint32_t* frames) {
    __try {
        const uint32_t* b = static_cast<const uint32_t*>(bink);
        *w = b[0];
        *h = b[1];
        *frames = b[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* WINAPI HookOpen(const char* name, uint32_t flags) {
    void* bink = g_open(name, flags);
    char shown[160] = "(file handle)";
    if (!(flags & kBinkFileHandle)) Copy(name, shown, sizeof(shown));
    uint32_t w = 0, h = 0, frames = 0;
    if (bink && Size(bink, &w, &h, &frames))
        Log("VIDEO open %s %ux%u, %u frames", shown, w, h, frames);
    else
        Log("VIDEO open %s failed", shown);
    return bink;
}
void WINAPI HookNextFrame(void* bink) {
    g_lastFrame.store(GetTickCount64(), std::memory_order_relaxed);
    g_nextFrame(bink);
}
int32_t WINAPI HookDoFrameAsync(void* bink, uint32_t yThread, uint32_t otherThread) {
    g_lastFrame.store(GetTickCount64(), std::memory_order_relaxed);
    return g_doFrameAsync(bink, yThread, otherThread);
}
void WINAPI HookClose(void* bink) {
    Log("VIDEO close");
    g_close(bink);
}

}  // namespace

void Install() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
    if (!bink) {
        Log("VIDEO bink2w64.dll not loaded: flat-cutscene detection off");
        return;
    }
    auto hook = [&](const char* name, void* detour, void** original) {
        void* target = reinterpret_cast<void*>(GetProcAddress(bink, name));
        return target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    };
    const bool open = hook("BinkOpen", reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void**>(&g_open));
    const bool next = hook("BinkNextFrame", reinterpret_cast<void*>(&HookNextFrame), reinterpret_cast<void**>(&g_nextFrame));
    const bool async = hook("BinkDoFrameAsync", reinterpret_cast<void*>(&HookDoFrameAsync),
                            reinterpret_cast<void**>(&g_doFrameAsync));
    const bool close = hook("BinkClose", reinterpret_cast<void*>(&HookClose), reinterpret_cast<void**>(&g_close));
    Log("VIDEO Bink hooks: open=%d nextFrame=%d doFrameAsync=%d close=%d", int(open), int(next), int(async), int(close));
}

bool Playing(uint64_t nowMs) {
    const uint64_t last = g_lastFrame.load(std::memory_order_relaxed);
    return last && nowMs >= last && nowMs - last < kPlayingMs;
}

}  // namespace b2s::videowatch
