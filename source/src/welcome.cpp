// First-start welcome windows (welcome.h): when and where they show, pointing, and the keyboard / gamepad close.
// Logic ported from the God of War VR mod's vr/panel.cpp WelcomeFrame (same owner, MIT; GoW D-318 / D-331).
#include "welcome.h"

#include <windows.h>
#include <Xinput.h>
#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

#include "log.h"
#include "player.h"
#include "welcome_draw.h"

namespace b2s::welcome {
namespace {

constexpr float kWelW = 1.3f, kWelDist = 1.6f;  // metres wide, metres in front of the head

int g_page = 0, g_drawnPage = -1;
bool g_drawnHover = false;
std::atomic<bool> g_active{false};
bool g_done = false, g_placed = false;
float g_yaw = 0.f, g_y = 0.f;
double g_stereoSince = 0.0, g_lastT = 0.0;
bool g_altArmed = false;     // the keyboard / gamepad press counts only once released since the window showed
bool g_armed[2] = {true, true};
std::atomic<bool> g_padHeld{false};
std::atomic<uint64_t> g_padAt{0};
std::atomic<bool> g_enterHeldAfter{false}, g_padHeldAfter{false};
float g_delay = 3.f;
bool g_settingsRead = false;

std::wstring IniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1) + L"b2svr.ini";
}

float IniFloat(const wchar_t* key, float fallback) {
    wchar_t v[32]{};
    GetPrivateProfileStringW(L"b2svr", key, L"", v, 32, IniPath().c_str());
    if (!v[0]) return fallback;
    const float f = static_cast<float>(_wtof(v));
    return std::isfinite(f) ? f : fallback;
}

double NowS() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// The game's window (or another window of the game process) is in front: keys typed elsewhere never count.
bool GameInFront() {
    DWORD pid = 0;
    const HWND f = GetForegroundWindow();
    return f && GetWindowThreadProcessId(f, &pid) && pid == GetCurrentProcessId();
}

// A real gamepad's A / Cross: what the game's own pad reads reported (NotePadConfirm, fresh within 200 ms) or a
// direct XInput user 0 read (every 50 ms; slow on empty slots).
bool PadConfirmHeld() {
    const uint64_t now = GetTickCount64();
    if (g_padHeld.load() && now - g_padAt.load() < 200) return true;
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetStateFn getState = [] {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = GetModuleHandleW(name)) return reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        return m ? reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState")) : nullptr;
    }();
    static uint64_t at = 0;
    static bool held = false;
    if (getState && now - at >= 50) {
        at = now;
        XINPUT_STATE st{};
        held = getState(0, &st) == ERROR_SUCCESS && (st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    }
    return held;
}

// The next window (CONTINUE) or the end (START). False = all windows seen, nothing shown any more.
bool Next(const char* how) {
    if (++g_page >= kWelPages) {
        WritePrivateProfileStringW(L"b2svr", L"welcome_seen", std::to_wstring(kWelcomeVersion).c_str(), IniPath().c_str());
        g_done = true;
        g_active = false;
        g_enterHeldAfter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        g_padHeldAfter = PadConfirmHeld();
        Log("WELCOME START (%s) - all windows seen, b2svr.ini welcome_seen %d", how, kWelcomeVersion);
        return false;
    }
    Log("WELCOME CONTINUE (%s) - window %d of %d", how, g_page + 1, kWelPages);
    return true;
}

// The pointer ray through a One Euro filter (as the VR panel's: still at rest, a fast sweep passes).
void SmoothAim(int hand, pose::Vec3& p, pose::Vec3& d) {
    static bool have[2] = {false, false};
    static pose::Vec3 sp[2], sd[2];
    static double last[2] = {0.0, 0.0};
    const double now = NowS();
    const float dt = last[hand] > 0.0 ? float(now - last[hand]) : 0.f;
    last[hand] = now;
    if (!have[hand] || dt <= 0.f || dt > 0.1f) {
        sp[hand] = p;
        sd[hand] = d;
        have[hand] = true;
        return;
    }
    const float dp = std::sqrt((p.x - sp[hand].x) * (p.x - sp[hand].x) + (p.y - sp[hand].y) * (p.y - sp[hand].y) +
                               (p.z - sp[hand].z) * (p.z - sp[hand].z));
    const float dd = std::sqrt((d.x - sd[hand].x) * (d.x - sd[hand].x) + (d.y - sd[hand].y) * (d.y - sd[hand].y) +
                               (d.z - sd[hand].z) * (d.z - sd[hand].z));
    const float cutoff = 3.f + 6.f * (dp + 2.f * dd) / dt;
    const float a = 1.f / (1.f + 1.f / (6.2831853f * cutoff * dt));
    sp[hand] = {sp[hand].x + (p.x - sp[hand].x) * a, sp[hand].y + (p.y - sp[hand].y) * a, sp[hand].z + (p.z - sp[hand].z) * a};
    sd[hand] = {sd[hand].x + (d.x - sd[hand].x) * a, sd[hand].y + (d.y - sd[hand].y) * a, sd[hand].z + (d.z - sd[hand].z) * a};
    const float n = std::sqrt(sd[hand].x * sd[hand].x + sd[hand].y * sd[hand].y + sd[hand].z * sd[hand].z);
    if (n > 1e-6f) sd[hand] = {sd[hand].x / n, sd[hand].y / n, sd[hand].z / n};
    p = sp[hand];
    d = sd[hand];
}

// ---- Enter kept from the game (D-331b: reading the key with GetAsyncKeyState does not stop the game getting it) ----
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
GetKeyboardStateFn g_origGetKeyboardState = nullptr;
uintptr_t g_exeLo = 0, g_exeHi = 0;

BOOL WINAPI HookGetKeyboardState(PBYTE keys) {
    const BOOL r = g_origGetKeyboardState(keys);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (r && keys && caller >= g_exeLo && caller < g_exeHi && TakesEnter()) keys[VK_RETURN] = 0;
    return r;
}

}  // namespace

void Install() {
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    g_exeLo = exe;
    g_exeHi = exe + nt->OptionalHeader.SizeOfImage;
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetKeyboardState"));
    const bool ok = target &&
                    MH_CreateHook(target, reinterpret_cast<void*>(&HookGetKeyboardState),
                                  reinterpret_cast<void**>(&g_origGetKeyboardState)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
    const float seen = IniFloat(L"welcome_seen", 0.f);
    Log("WELCOME %s (b2svr.ini welcome_seen %.0f, this version %d); Enter seam %s", seen >= float(kWelcomeVersion)
        ? "already seen - not shown" : "windows will show ~3 s into the first 3D view", seen, kWelcomeVersion,
        ok ? "installed" : "NOT installed (Enter also reaches the game)");
    if (seen >= float(kWelcomeVersion)) g_done = true;
}

bool Frame(const xr::FrameInput& in, Layer* out) {
    const double now = NowS();
    const float dt = g_lastT > 0.0 ? float(std::fmin(now - g_lastT, 0.1)) : 0.f;
    g_lastT = now;
    if (g_done) return false;
    if (!g_settingsRead) {
        g_settingsRead = true;
        g_delay = std::fmax(IniFloat(L"welcome_delay_s", 3.f), 0.f);
    }
    if (!xr::FrameInputUsable(in, GetTickCount64())) return false;  // no head pose this frame: nothing drawn
    if (!g_active.load()) {
        // ~3 s into the 3D view, not while a story cutscene plays (the main menu, a 3D scene, counts as 3D)
        const player::State st = player::Last();
        if (!st.menu && st.control == player::Control::Cutscene) {
            g_stereoSince = 0.0;
            return false;
        }
        if (g_stereoSince == 0.0) g_stereoSince = now;
        if (now - g_stereoSince < g_delay) return false;
        if (!MakeD2D()) {
            g_done = true;
            return false;
        }
        g_active = true;
        g_drawnPage = -1;
        g_altArmed = false;  // a key / pad button already held does not count
        g_armed[0] = g_armed[1] = false;  // a trigger already pulled does not count
        Log("WELCOME window %d of %d shown", g_page + 1, kWelPages);
    }
    // Follows the view slowly: its direction eases toward the head's (yaw, 0.6 s), its height with the head's (0.4 s).
    const pose::Vec3 head{in.headRel.m[12], in.headRel.m[13], in.headRel.m[14]};
    float fx = -in.headRel.m[8], fz = -in.headRel.m[10];
    const float fn = std::sqrt(fx * fx + fz * fz);
    if (fn > 0.2f) {
        fx /= fn;
        fz /= fn;
    } else {
        fx = -std::sin(g_yaw);
        fz = -std::cos(g_yaw);
    }
    const float target = std::atan2(-fx, -fz);  // the direction the head looks (0 = -Z)
    if (!g_placed) {
        g_yaw = target;
        g_y = head.y - 0.08f;
        g_placed = true;
    } else {
        float d = target - g_yaw;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        g_yaw += d * (1.f - std::exp(-dt / 0.6f));
        g_y += (head.y - 0.08f - g_y) * (1.f - std::exp(-dt / 0.4f));
    }
    const pose::Vec3 c{head.x - std::sin(g_yaw) * kWelDist, g_y, head.z - std::cos(g_yaw) * kWelDist};
    const float yaw = std::atan2(head.x - c.x, head.z - c.z);  // the quad's +Z toward the head
    const float h = kWelW * float(kWH) / float(kWW);
    const float nx = std::sin(yaw), nz = std::cos(yaw), rx = std::cos(yaw), rz = -std::sin(yaw);
    bool hover = false, cursor = false;
    pose::Vec3 hitP{};
    for (int hand = 1; hand >= 0; --hand) {  // the right hand first
        const xr::HandInput& hs = in.hand[hand];
        bool hit = false;
        if (hs.active && hs.aimValid) {
            pose::Vec3 o{hs.aimRel.m[12], hs.aimRel.m[13], hs.aimRel.m[14]};
            pose::Vec3 d{-hs.aimRel.m[8], -hs.aimRel.m[9], -hs.aimRel.m[10]};  // aim -Z
            SmoothAim(hand, o, d);
            const float dn = d.x * nx + d.z * nz;
            if (dn < -1e-3f) {
                const float t = ((c.x - o.x) * nx + (c.z - o.z) * nz) / dn;
                if (t > 0.f && t < 20.f) {
                    const pose::Vec3 q{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
                    const float lx = (q.x - c.x) * rx + (q.z - c.z) * rz, ly = q.y - c.y;
                    const float u = lx / kWelW + 0.5f, v = 0.5f - ly / h;
                    if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f) {
                        hit = true;
                        if (!cursor) {
                            cursor = true;
                            hitP = q;
                            const int px = int(u * kWW), py = int(v * kWH);
                            hover = px >= g_wBtn.left && px < g_wBtn.right && py >= g_wBtn.top && py < g_wBtn.bottom;
                            if (hs.trigger < 0.35f) g_armed[hand] = true;
                            if (g_armed[hand] && hs.trigger > 0.75f) {
                                g_armed[hand] = false;
                                if (hover && !Next("trigger")) return false;
                            }
                        }
                    }
                }
            }
        }
        if (!hit && hs.trigger < 0.35f) g_armed[hand] = true;
        if (!hit && hs.trigger > 0.75f) g_armed[hand] = false;  // a pull begun elsewhere never clicks
    }
    // D-331: Enter (the game in front; not Space or another key the game uses in play) or a gamepad's A / Cross press
    // the button too - nobody without VR controllers in hand is stuck. A press counts once released since the window
    // showed.
    {
        const bool key = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 && GameInFront();
        const bool pad = PadConfirmHeld();
        if (!key && !pad) {
            g_altArmed = true;
        } else if (g_altArmed) {
            g_altArmed = false;
            if (!Next(key ? "keyboard" : "gamepad")) return false;
        }
    }
    out->dirty = false;
    if (g_page != g_drawnPage || hover != g_drawnHover) {
        WelcomeDraw(g_page, hover);
        g_drawnPage = g_page;
        g_drawnHover = hover;
        out->dirty = true;
    }
    out->center = c;
    out->yaw = yaw;
    out->widthM = kWelW;
    out->heightM = h;
    out->pixels = g_wOut;
    out->cursor = cursor;
    if (cursor) out->cursorAt = {hitP.x + nx * 0.01f, hitP.y, hitP.z + nz * 0.01f};
    return true;
}

void FlatFrame() {
    g_stereoSince = 0.0;  // the 3 s count starts again at the next 3D frames
    if (g_active.exchange(false)) Log("WELCOME a 2D screen - window %d hidden until the 3D view is back", g_page + 1);
    g_placed = false;
}

bool Active() { return g_active.load(); }

bool TakesEnter() {
    if (g_active.load()) return true;
    if (g_enterHeldAfter.load()) {
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) return true;
        g_enterHeldAfter = false;
    }
    return false;
}

bool TakesPadConfirm() {
    if (g_active.load()) return true;
    if (g_padHeldAfter.load()) {
        if (g_padHeld.load() && GetTickCount64() - g_padAt.load() < 200) return true;
        g_padHeldAfter = false;
    }
    return false;
}

void NotePadConfirm(bool held) {
    g_padHeld = held;
    g_padAt = GetTickCount64();
}

}  // namespace b2s::welcome
