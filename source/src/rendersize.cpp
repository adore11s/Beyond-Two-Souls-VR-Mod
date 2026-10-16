#include "rendersize.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "log.h"
#include "stereo.h"

namespace b2s::rendersize {
namespace {

// STATIC (EXP-041). FUN_140c7f5b0(unused, w, h) is the one place the game sets its render size:
// DisplayCfg (*0x141c8b278) +0x20/+0x24 = w/h, +0x28 = w, +0x2c = letterbox flag (+0xb880) ?
// min(w * 0.4255, h) : h; then ResizeBuffers(w, h) (FUN_140cbc2f0), the size listeners and the
// render targets. In the game's borderless and fullscreen modes (RenderState (*0x141c78958) +0xca8
// current mode, +0xcac requested: 0 windowed, 1 borderless, 2 fullscreen) the height is re-derived
// from the desktop's aspect and the swapchain is the desktop's size, so N x N needs windowed mode.
// FUN_140828a60(RenderState) runs once per frame on the game thread, inside FUN_140828650 under
// the renderer lock with the render thread idle, and calls FUN_140c7f5b0 when the resolution
// setting changes: the mod applies its own size from the same place.
// LIVE 0.17.0 (owner: "it's all stretched"): with only that, the scene targets stayed 2048x871 under a
// 2048x2048 viewport, so the top 43% of the view filled the eye. Each screen-sized TEXTURE_DX11
// records at creation whether it is letterboxed (+0x4c, vfunc_48); the resize code (FUN_140c7f460,
// RENDER_TARGET_DX11::vfunc_10) asks vfunc_49 and sizes such a texture w * 0.4255 high whatever the
// letterbox flag says now. While square, vfunc_49 answers "not letterboxed".
constexpr uint64_t kSetRenderSizeVa = 0x140c7f5b0;
constexpr uint64_t kResolutionUpdateVa = 0x140828a60;
constexpr uint64_t kTextureBandVa = 0x140cbdbc0;
constexpr uint64_t kDisplayCfgVa = 0x141c8b278;
constexpr uint64_t kCinemaCacheVa = 0x141c7894f;  // the game's own UseCinemaRatio value
constexpr size_t kModeNow = 0xca8, kModeWanted = 0xcac;
constexpr size_t kWidth = 0x20, kHeight = 0x24, kSceneWidth = 0x28, kSceneHeight = 0x2c, kLetterbox = 0xb880;
constexpr uint32_t kMinSize = 512, kMaxSize = 8192;

using SetRenderSizeFn = void (*)(void* unused, uint32_t w, uint32_t h);
using ResolutionUpdateFn = void (*)(void* renderState);
using TextureBandFn = uint8_t (*)(void* texture, uint8_t* letterboxed);
SetRenderSizeFn g_origSet = nullptr;
ResolutionUpdateFn g_origUpdate = nullptr;
TextureBandFn g_origBand = nullptr;
std::atomic<bool> g_noBand{false};  // textures are sized full height (square)

std::atomic<uint32_t> g_size{0};    // N, 0 = the game's own size
uint32_t g_gameW = 0, g_gameH = 0;  // the game's own render size (its last request)
bool g_inOwnCall = false;
bool g_square = false;              // the game renders N x N now
int g_reported = -1;
uint64_t g_bandRemovals = 0;

template <class T>
T& At(uint8_t* p, size_t off) {
    return *reinterpret_cast<T*>(p + off);
}

// The desktop window (0.17.5, owner: "it renders a big square for VR, on the desktop you just see a small
// window"). The swapchain's size is independent of the window's (DXGI scales on Present), and the game's
// window procedure (FUN_1407acb30) has no WM_SIZE handling, so the window can be small while the game
// renders N x N. While square: a `desktop_window` x `desktop_window` client area (b2svr.ini, default 720,
// 0 = leave the window alone) in the middle of its monitor, checked once a second (the game sets the window
// again only when its resolution or display mode changes); back to the game's own size, the window goes
// back where it was. DisplayCfg+0x18 = the game's window (FUN_140828a60).
constexpr size_t kWindow = 0x18;
int g_windowSize = 720;
RECT g_savedWindow{};
bool g_windowSaved = false;
uint64_t g_windowCheck = 0;

void SmallWindow(uint8_t* cfg) {
    HWND w = At<HWND>(cfg, kWindow);
    if (!g_windowSize || !w || !IsWindow(w)) return;
    RECT client{};
    GetClientRect(w, &client);
    if (client.right - client.left == g_windowSize && client.bottom - client.top == g_windowSize) return;
    if (!g_windowSaved) {
        GetWindowRect(w, &g_savedWindow);
        g_windowSaved = true;
    }
    RECT r{0, 0, g_windowSize, g_windowSize};
    AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(w, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(w, GWL_EXSTYLE)));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    SetWindowPos(w, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    static int logs = 0;
    if (logs++ < 5)
        Log("RENDERSIZE desktop window %dx%d (the headset still gets the full square)", g_windowSize, g_windowSize);
}

void RestoreWindow(uint8_t* cfg) {
    if (!g_windowSaved) return;
    g_windowSaved = false;
    HWND w = At<HWND>(cfg, kWindow);
    if (!w || !IsWindow(w)) return;
    SetWindowPos(w, nullptr, g_savedWindow.left, g_savedWindow.top, g_savedWindow.right - g_savedWindow.left,
                 g_savedWindow.bottom - g_savedWindow.top, SWP_NOZORDER | SWP_NOACTIVATE);
    Log("RENDERSIZE desktop window back to its size before VR");
}

uint8_t* DisplayCfg() {
    const uintptr_t a = game::Va(kDisplayCfgVa);
    return a ? *reinterpret_cast<uint8_t**>(a) : nullptr;
}

// Menus, subtitles and the 2D video rectangle (0.17.13, owner: "since square rendering the subtitles are
// cut off at the bottom", on the monitor too). STATIC: FUN_140d22cb0 builds each menu display with
// MM_DISPLAY+0x1e0 = height * (sceneH * 1080 / 817.0213) / renderH, and FUN_140c82120 makes the video
// rectangle sceneH * 780 / 817.0213 high. 817.02 is the letterbox band of a 1080-high screen, so with the
// game's band the factor is 1. Square has no band (scene = render height): 1080 / 817 = 1.32 times taller,
// and the bottom 16% of every menu (where the subtitles are) falls below the picture (LIVE 0.17.12: the main
// menu's lines 1.32 times farther from the middle than at 16:9). The constant (.rdata, read only by those
// two) is 1080 while square, so both lay out as with the game's band; menus built before keep theirs.
constexpr uint64_t kMenuBandVa = 0x1417ad180;
constexpr float kMenuBand = 817.0213f, kMenuBandSquare = 1080.f;

void SetMenuBand(bool square) {
    const uintptr_t a = game::Va(kMenuBandVa);
    if (!a) return;
    auto* f = reinterpret_cast<float*>(a);
    const float want = square ? kMenuBandSquare : kMenuBand;
    if (*f == want) return;
    if (std::fabs(*f - kMenuBand) > 0.01f && std::fabs(*f - kMenuBandSquare) > 0.01f) {
        static bool logged = false;
        if (!logged) Log("RENDERSIZE menu band constant is %.3f, not the game's: menus left alone", *f);
        logged = true;
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(f, sizeof(float), PAGE_READWRITE, &old)) return;
    *f = want;
    VirtualProtect(f, sizeof(float), old, &old);
    Log("RENDERSIZE menus and subtitles laid out for %s", square ? "the square picture" : "the game's own picture");
}

// Game thread, renderer lock held: the game's own size path with (w, h).
void Resize(uint8_t* cfg, uint32_t w, uint32_t h, bool square) {
    // Square: the scene fills the whole picture (no letterbox band), and the render targets are
    // made at full height. Switching the band off on targets made for it smeared the bottom of
    // the eye (CINEMA_BAND.md); here the targets are created after the flag changes.
    // Back to the game's size: its own letterbox setting again.
    if (square) {
        At<uint8_t>(cfg, kLetterbox) = 0;
    } else if (const uintptr_t cache = game::Va(kCinemaCacheVa)) {
        At<uint8_t>(cfg, kLetterbox) = *reinterpret_cast<const uint8_t*>(cache);
    }
    g_noBand = square;
    SetMenuBand(square);
    g_inOwnCall = true;
    g_origSet(nullptr, w, h);
    g_inOwnCall = false;
    g_square = square;
    Log("RENDERSIZE %s: render %ux%u, scene %ux%u", square ? "square" : "the game's own size", At<uint32_t>(cfg, kWidth),
        At<uint32_t>(cfg, kHeight), At<uint32_t>(cfg, kSceneWidth), At<uint32_t>(cfg, kSceneHeight));
}

uint8_t HookTextureBand(void* texture, uint8_t* letterboxed) {
    const uint8_t screenSized = g_origBand(texture, letterboxed);
    if (g_noBand.load(std::memory_order_relaxed) && letterboxed) *letterboxed = 0;
    return screenSized;
}

void HookSetRenderSize(void* unused, uint32_t w, uint32_t h) {
    if (!g_inOwnCall) {
        g_gameW = w;
        g_gameH = h;
        if (g_square) {
            // A resolution change in the game's menu while square: it stays square.
            const uint32_t n = g_size.load();
            Log("RENDERSIZE the game asked for %ux%u: kept at %ux%u", w, h, n, n);
            w = h = n;
        }
    }
    g_origSet(unused, w, h);
}

void HookResolutionUpdate(void* renderState) {
    g_origUpdate(renderState);
    auto* rs = static_cast<uint8_t*>(renderState);
    uint8_t* cfg = DisplayCfg();
    if (!rs || !cfg) return;
    if (!g_gameW && !g_square) {
        g_gameW = At<uint32_t>(cfg, kWidth);
        g_gameH = At<uint32_t>(cfg, kHeight);
    }
    const uint32_t n = g_size.load();
    const bool vr = stereo::XrWanted();
    const bool windowed = At<int32_t>(rs, kModeNow) == 0 && At<int32_t>(rs, kModeWanted) == 0;
    const int state = n == 0 ? 0 : !vr ? 1 : !windowed ? 2 : 3;
    if (state != g_reported) {
        g_reported = state;
        switch (state) {
            case 0: Log("RENDERSIZE off (b2svr.ini render_size): the game's own size"); break;
            case 1: Log("RENDERSIZE %ux%u waits for VR", n, n); break;
            case 2:
                Log("RENDERSIZE %ux%u needs the game in Windowed mode (window mode now %d, asked %d; 0 = windowed): "
                    "the game's own size until then",
                    n, n, At<int32_t>(rs, kModeNow), At<int32_t>(rs, kModeWanted));
                break;
            default: Log("RENDERSIZE %ux%u in VR", n, n); break;
        }
    }
    if (state == 3) {
        const uint64_t now = GetTickCount64();
        if (!g_square || At<uint32_t>(cfg, kWidth) != n || At<uint32_t>(cfg, kHeight) != n) {
            Resize(cfg, n, n, true);
            SmallWindow(cfg);
            g_windowCheck = now;
        } else if (now - g_windowCheck >= 1000) {
            g_windowCheck = now;
            SmallWindow(cfg);
        }
        if (At<uint8_t>(cfg, kLetterbox) || At<uint32_t>(cfg, kSceneHeight) != n) {
            // The game's letterbox command (runtime.ini UseCinemaRatio) came after the resize. The
            // targets were made at full height, so the band goes again, as that command would with 0.
            At<uint8_t>(cfg, kLetterbox) = 0;
            At<uint32_t>(cfg, kSceneWidth) = n;
            At<uint32_t>(cfg, kSceneHeight) = n;
            if (++g_bandRemovals <= 5) Log("RENDERSIZE letterbox band removed again (%llu)", g_bandRemovals);
        }
    } else if (g_square && g_gameW && g_gameH) {
        Resize(cfg, g_gameW, g_gameH, false);
        RestoreWindow(cfg);
    }
}

uint32_t ParseSize(const char* s) {
    const long v = s ? std::strtol(s, nullptr, 10) : 0;
    return v >= long(kMinSize) && v <= long(kMaxSize) ? uint32_t(v) & ~7u : 0;
}

}  // namespace

void Install() {
    wchar_t path[MAX_PATH]{}, value[32]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"b2svr.ini");
        GetPrivateProfileStringW(L"b2svr", L"render_size", L"off", value, 32, path);
        char narrow[32]{};
        for (int i = 0; i < 31 && value[i]; ++i) narrow[i] = char(value[i]);
        g_size = ParseSize(narrow);
        GetPrivateProfileStringW(L"b2svr", L"desktop_window", L"720", value, 32, path);
        const int dw = _wtoi(value);
        g_windowSize = dw >= 240 && dw <= 4096 ? dw : 0;
    }
    void* set = reinterpret_cast<void*>(game::Va(kSetRenderSizeVa));
    void* update = reinterpret_cast<void*>(game::Va(kResolutionUpdateVa));
    void* band = reinterpret_cast<void*>(game::Va(kTextureBandVa));
    if (!set || !update || !band || !DisplayCfg()) {
        Log("RENDERSIZE render size functions not found in this build: the game keeps its own size");
        g_size = 0;
        return;
    }
    if (MH_CreateHook(set, reinterpret_cast<void*>(&HookSetRenderSize), reinterpret_cast<void**>(&g_origSet)) != MH_OK ||
        MH_CreateHook(update, reinterpret_cast<void*>(&HookResolutionUpdate), reinterpret_cast<void**>(&g_origUpdate)) !=
            MH_OK ||
        MH_CreateHook(band, reinterpret_cast<void*>(&HookTextureBand), reinterpret_cast<void**>(&g_origBand)) != MH_OK ||
        MH_EnableHook(band) != MH_OK || MH_EnableHook(set) != MH_OK || MH_EnableHook(update) != MH_OK) {
        Log("RENDERSIZE hooks FAILED: the game keeps its own size");
        g_size = 0;
        return;
    }
    const uint32_t n = g_size.load();
    if (n)
        Log("RENDERSIZE hooks installed: %ux%u in VR (b2svr.ini render_size)", n, n);
    else
        Log("RENDERSIZE hooks installed: off (b2svr.ini render_size)");
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "rendersize") != 0) return false;
    if (!g_origSet) {
        Log("RENDERSIZE not available in this build");
        return true;
    }
    g_size = ParseSize(arg);
    g_reported = -1;
    Log("RENDERSIZE set to %u (applied at the next frame)", g_size.load());
    return true;
}

}  // namespace b2s::rendersize
