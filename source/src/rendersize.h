#pragma once
#include <cstdint>
// Square render size in VR (owner 2026-10-15, EXP-041). The player picks N in the VR settings panel
// (b2svr.ini render_size=N | auto | off); while VR is on and the game runs in its windowed mode, the game
// renders at N x N instead of its 16:9 resolution, whatever the desktop's resolution. The headset's eyes are
// close to square, so a 16:9 picture wasted width and stretched its rows. Auto (GoW style) = the larger of the
// runtime's recommended eye width / height, up to a multiple of 8 (512 .. 8192); it is not frame-time adaptive.
namespace b2s::rendersize {

void Install();

// The setting: N (1440 .. 2816 or any 512 .. 8192 from b2svr.ini), 0 = the game's own size (off), -1 = Auto.
int Setting();
// Sets and saves it (b2svr.ini render_size); applied at the next frame.
void SetSetting(int setting);
// The Auto size once the headset's recommended eye size is known (0 before).
uint32_t AutoSize();
// The render size hooks are installed (false on a build where they were not found).
bool Available();

// The desktop window (playbook 08.4): when it is wider than the game's picture (the square in VR), the band of the
// picture's rows [v0, v1) that fills it - the 3D scene cropped to the window's 16:9 (no black bars). False = show the
// whole picture (not VR, a window as narrow as the picture, b2svr.ini desktop_crop=0).
bool DesktopCrop(uint32_t bbW, uint32_t bbH, float* v0, float* v1);

// devctl: "rendersize <N|auto|off>" (applied at the next frame).
bool Command(const char* verb, const char* arg);

}  // namespace b2s::rendersize
