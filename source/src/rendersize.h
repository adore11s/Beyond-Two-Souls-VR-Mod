#pragma once
// Square render size in VR (owner 2026-10-15, EXP-041). The player picks N in the launcher
// (b2svr.ini render_size=N); while VR is on and the game runs in its windowed mode, the game renders
// at N x N instead of its 16:9 resolution, whatever the desktop's resolution. The headset's eyes are
// close to square, so a 16:9 picture wasted width and stretched its rows.
namespace b2s::rendersize {

void Install();

// devctl: "rendersize <N|off>" (applied at the next frame).
bool Command(const char* verb, const char* arg);

}  // namespace b2s::rendersize
