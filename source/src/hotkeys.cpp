#include "hotkeys.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <initializer_list>

namespace b2s::hotkeys {
namespace {

// F10 is a mod key, but Windows treats it like Alt: DefWindowProc turns its WM_SYSKEYUP into
// SC_KEYMENU and the window enters menu mode, stalling the game's message loop until the next key.
// STATIC: the game's window procedure (FUN_1407acb30) swallows VK_MENU but passes VK_F10 on.
// A message hook on the window's thread drops F10's system-key messages before dispatch;
// GetAsyncKeyState still sees the key.
LRESULT CALLBACK DropSystemF10(int code, WPARAM removed, LPARAM lp) {
    if (code == HC_ACTION) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if ((m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP) && m->wParam == VK_F10)
            m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, removed, lp);
}
void GuardF10(HWND gameWindow) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;
    const DWORD thread = GetWindowThreadProcessId(gameWindow, nullptr);
    if (!thread || !SetWindowsHookExW(WH_GETMESSAGE, &DropSystemF10, nullptr, thread)) installed = false;
}

bool IsSteamVrProcess(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != 0;
    CloseHandle(p);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    for (const wchar_t* vr : {L"vrmonitor.exe", L"vrcompositor.exe", L"vrserver.exe", L"vrwebhelper.exe",
                              L"vrdashboard.exe", L"vrstartup.exe"})
        if (_wcsicmp(name, vr) == 0) return true;
    return false;
}

}  // namespace

bool Active() {
    // The answer only changes with the foreground window: cache it per window handle.
    static HWND lastWindow = nullptr;
    static bool lastAnswer = false;
    HWND w = GetForegroundWindow();
    if (w == lastWindow) return lastAnswer;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    lastWindow = w;
    if (pid == GetCurrentProcessId()) GuardF10(w);
    lastAnswer = pid == GetCurrentProcessId() || (pid && IsSteamVrProcess(pid));
    return lastAnswer;
}

bool Edge::Feed(int vk) {
    const bool down = Active() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    return edge;
}

}  // namespace b2s::hotkeys
