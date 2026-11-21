#pragma once
// First-start welcome windows (playbook 11.4, REQ-27; GoW D-318 / D-331): WELCOME, KO-FI SUPPORTER, VR CONTROLS, one at
// a time, the first time a player starts this welcome version: ~3 s (welcome_delay_s) into the 3D view (the main menu
// is a 3D scene, so usually there), not while a story cutscene plays, hidden on 2D screens (loading, videos, pause) and
// shown again with the page reached. 1.6 m ahead, following the head slowly, mandatory: while one is up the VR
// controllers drive only it (vrinput gets nothing); its button takes a controller's trigger, Enter (the game's window in
// front) or a gamepad's A / Cross (Steam Input or XInput), which are kept from the game meanwhile. START writes
// b2svr.ini welcome_seen = kWelcomeVersion; the shipped b2svr.ini has no welcome_seen. Drawing: welcome_draw.h.
#include <cstdint>

#include "posemath.h"
#include "xr.h"

namespace b2s::welcome {

constexpr int kWelcomeVersion = 1;

struct Layer {
    pose::Vec3 center{};  // seated reference frame (as panel::Placement)
    float yaw = 0.f;      // the window's front (+Z) faces the head
    float widthM = 0.f, heightM = 0.f;
    const uint32_t* pixels = nullptr;  // kWW x kWH, 0xAARRGGBB premultiplied
    bool dirty = false;                // new pixels since the last frame
    bool cursor = false;               // a controller points at the window
    pose::Vec3 cursorAt{};
    float cursorSize = 0.035f;
};

// Installs the keyboard seam (the game reads keys with GetKeyboardState: INPUT_DEVICE_WIN_KEYBOARD::vfunc_08
// 0x140c026b0, STATIC) that keeps Enter from the game while a window takes it.
void Install();
// Render thread, a stereo (3D) frame: true with the window to draw this frame.
bool Frame(const xr::FrameInput& in, Layer* out);
// A 2D frame (loading, video, pause): the window hides and the 3 s count starts again (the page reached is kept).
void FlatFrame();
// Any thread: a window is up (the VR controllers drive only it).
bool Active();
// Any thread: Enter / a gamepad's confirm belongs to the window now (one is up, or the closing press is still held).
bool TakesEnter();
bool TakesPadConfirm();
// Input paths report a real gamepad's A / Cross (Steam Input digital "button_a", XInput user 0) as the game reads it.
void NotePadConfirm(bool held);

}  // namespace b2s::welcome
