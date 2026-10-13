#include "timerwindow.h"

#include <windows.h>

#include <cstdio>

#include "log.h"

namespace b2s::timerwindow {
namespace {

constexpr UINT_PTR kTick = 1;
HFONT g_font = nullptr;

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // clicking it never takes focus from the game
        case WM_NCHITTEST:
            return HTCAPTION;  // drag it anywhere
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            RECT rc;
            GetClientRect(wnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16));
            FillRect(mem, &rc, bg);
            DeleteObject(bg);
            HGDIOBJ oldFont = SelectObject(mem, g_font);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(255, 210, 60));
            wchar_t text[64];
            swprintf_s(text, L"%.1f s", LogSeconds());
            DrawTextW(mem, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldFont);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(wnd, msg, wp, lp);
    }
}

DWORD WINAPI WindowThread(LPVOID) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = L"B2SVR_Timer";
    RegisterClassW(&wc);
    g_font = CreateFontW(-40, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"B2SVR timer",
                               WS_POPUP | WS_BORDER, 20, 20, 240, 64, nullptr, nullptr, inst, nullptr);
    if (!wnd) {
        Log("TIMER window could not be created (%lu)", GetLastError());
        return 0;
    }
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    SetTimer(wnd, kTick, 100, nullptr);
    Log("TIMER window shown: its seconds are the log's timestamps");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

void Start() {
    wchar_t path[MAX_PATH]{}, value[16]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"b2svr.ini");
    GetPrivateProfileStringW(L"b2svr", L"timer_window", L"off", value, 16, path);
    if (_wcsicmp(value, L"on") != 0) return;
    if (HANDLE t = CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace b2s::timerwindow
