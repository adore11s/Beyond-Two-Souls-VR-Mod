#pragma once
// VR settings panel, profile B2S_ELEVEN_ROW_V6 (1.0.3; panel_math.h): the GoW panel's GDI renderer, layout, fonts and
// pointing in the playbook's V6 style (revision 20/23) with the B2S light-blue accent. Drawn with GDI into a 1000 x 1690
// DIB (redrawn only when a value or the hovered button changes) and shown by xr.cpp as its own quad, 0.15 m to the right
// of the floating game screen: on the game's 2D-only screens (the pause menu) and, at the main menu (a 3D scene), while
// its Options page is open (menuwatch.h, playbook 10.6). Point with either controller and pull its trigger; every
// choice is saved in b2svr.ini at once.
#include <cstdint>

#include "panel_math.h"
#include "xr.h"

namespace b2s::panel {

struct Frame {
    Placement placement;               // seated reference frame
    const uint32_t* pixels = nullptr;  // kWidthPx x kHeightPx, 0xAARRGGBB (sRGB bytes), opaque
    bool dirty = false;                // new pixels since the last frame
    bool cursor = false;               // a controller points at the panel
    pose::Vec3 cursorAt{};             // the cursor quad's centre (1 cm in front of the panel)
    float cursorSize = 0.035f;
};

// The thread that submits the frame's layers (game thread at Present): the panel next to the screen centred at
// `screen` (width `screenW`, reference frame) for this input. Handles pointing and clicks.
void Update(const xr::FrameInput& in, const pose::Vec3& screen, float screenW, Frame* out);
// No panel this frame: pointing state reset (the trigger belongs to the game again).
void Hidden();
// The 64 x 64 cursor dot (0xAARRGGBB, premultiplied, sRGB).
const uint32_t* CursorPixels();
constexpr int kCursorPx = 64;
// Hand 0 left / 1 right points at the panel: its trigger belongs to the panel, not the game.
bool CapturesTrigger(int hand);

}  // namespace b2s::panel
