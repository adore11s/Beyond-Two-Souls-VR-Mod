#include "audio.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "game.h"
#include "log.h"

namespace b2s::audio {
namespace {

constexpr uint64_t kSetPositionVa = 0x140ce55b0;     // SOUND_LISTENER::SetPosition (sig_targets.json)
constexpr uint64_t kSetOrientationVa = 0x140ce57c0;  // SOUND_LISTENER::SetOrientation

using SetPosFn = void (*)(void* listener, const float* pos);
using SetOriFn = void (*)(void* listener, const float* front, const float* up);
SetPosFn g_origPos = nullptr;
SetOriFn g_origOri = nullptr;

std::atomic<bool> g_enabled{true};
std::mutex g_pubMutex;
HeadPublish g_pub;

// The listener whose position call just got the head: its orientation call (which follows it in the same
// feed) gets the head too. Game thread only (the feeds run in the game's update).
void* g_following = nullptr;
std::atomic<uint64_t> g_fromHead{0}, g_fromGame{0};

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\b2svr.ini";
}

bool Snapshot(HeadPublish* out) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_pubMutex);
    *out = g_pub;
    return Fresh(*out, GetTickCount64());
}

void Report(bool head) {
    (head ? g_fromHead : g_fromGame).fetch_add(1, std::memory_order_relaxed);
    static uint64_t next = 0, shown = 0;
    const uint64_t now = GetTickCount64();
    if (head && !shown++) Log("AUDIO the sound listener follows the headset from now on");
    if (now >= next) {
        const uint64_t h = g_fromHead.exchange(0), g = g_fromGame.exchange(0);
        if (next && h)  // silent while VR is off (only the game's own listener)
            Log("AUDIO listener updates: %llu from the head, %llu the game's own (last 30 s)",
                static_cast<unsigned long long>(h), static_cast<unsigned long long>(g));
        next = now + 30000;
    }
}

void HookSetPosition(void* listener, const float* pos) {
    HeadPublish p;
    if (pos && Snapshot(&p) && FollowsCamera(pos, p.authored)) {
        float head[4], front[4], up[4];
        HeadVectors(p.head, head, front, up);
        g_following = listener;
        Report(true);
        g_origPos(listener, head);
        return;
    }
    if (g_following == listener) g_following = nullptr;
    Report(false);
    g_origPos(listener, pos);
}

void HookSetOrientation(void* listener, const float* front, const float* up) {
    HeadPublish p;
    if (listener && listener == g_following && Snapshot(&p)) {
        float head[4], f[4], u[4];
        HeadVectors(p.head, head, f, u);
        g_origOri(listener, f, u);
        return;
    }
    g_origOri(listener, front, up);
}

}  // namespace

bool Install() {
    wchar_t v[16]{};
    GetPrivateProfileStringW(L"b2svr", L"audio_head", L"on", v, 16, IniPath().c_str());
    g_enabled = _wcsicmp(v, L"off") != 0;
    void* pos = reinterpret_cast<void*>(game::Va(kSetPositionVa));
    void* ori = reinterpret_cast<void*>(game::Va(kSetOrientationVa));
    if (!pos || !ori) {
        Log("AUDIO listener functions not found in this build: sound stays with the game camera");
        return false;
    }
    if (MH_CreateHook(pos, reinterpret_cast<void*>(&HookSetPosition), reinterpret_cast<void**>(&g_origPos)) != MH_OK ||
        MH_CreateHook(ori, reinterpret_cast<void*>(&HookSetOrientation), reinterpret_cast<void**>(&g_origOri)) != MH_OK ||
        MH_EnableHook(pos) != MH_OK || MH_EnableHook(ori) != MH_OK) {
        MH_DisableHook(pos);
        MH_DisableHook(ori);
        Log("AUDIO listener hooks FAILED: sound stays with the game camera");
        return false;
    }
    Log("AUDIO listener hooks installed (b2svr.ini audio_head=%s)", g_enabled.load() ? "on" : "off");
    return true;
}

void Publish(const pose::Mat4& head, const pose::Mat4& authored, bool valid) {
    std::lock_guard lock(g_pubMutex);
    g_pub.valid = valid;
    g_pub.tickMs = GetTickCount64();
    if (valid) {
        g_pub.head = head;
        g_pub.authored = authored;
    }
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "audiohead") != 0) return false;
    g_enabled = !arg || std::strcmp(arg, "off") != 0;
    Log("AUDIO head-tracked listener %s", g_enabled.load() ? "on" : "off");
    return true;
}

}  // namespace b2s::audio
