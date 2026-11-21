#pragma once
// First-start welcome windows (playbook 11.4, REQ-27) - their DRAWING only: the VR panel's V6 pattern, the built-in
// Montserrat (RCDATA 3101..3103, assets/fonts, SIL Open Font License 1.1) and the three pages' text. Shared by the mod
// (welcome.cpp: when and where the windows show) and tests/welcome_preview.cpp (PNGs of every window from this exact
// code, to check a new or changed window before the headset). Owner 2026-11-15: no game-specific third window; the
// last window names the VR panel's settings ("tinker in VR settings (Aiden possessions etc.)"); the accent is the
// panel's light blue from the Beyond: Two Souls cover, not GoW's red.
#include <windows.h>
#include <cstdint>

namespace b2s::welcome {
constexpr int kWW = 1200, kWH = 820;  // the window's pixels (1.3 m wide in the headset)
constexpr int kWelPages = 3;          // WELCOME, KO-FI SUPPORTER, VR CONTROLS
extern uint32_t g_wOut[kWW * kWH];    // the last drawn window, 0xAARRGGBB premultiplied
extern RECT g_wBtn;                   // the CONTINUE / START bubble in g_wOut's pixels
uint32_t PatternPixel(int px, int py);  // one pixel of the VR panel's V6 background pattern (0x00RRGGBB)
bool LoadPanelFonts();                  // the built-in Montserrat added for GDI (this process only); true = all three
bool MakeD2D();                         // Direct2D / DirectWrite / WIC and the fonts (once); false = unavailable
void WelcomeDraw(int page, bool hover); // page 0..kWelPages-1 into g_wOut (hover = the bubble pointed at)
}  // namespace b2s::welcome
