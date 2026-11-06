#pragma once
// Developer tools of the mod's maintainer (owner 2026-11-06: "those are just mine"). They are compiled only into the
// developer build (CMake target b2svr_dev, B2SVR_DEV_TOOLS=1, sources in src/devtools/ which the public release does
// not carry). The release build links devtools_off.cpp instead: every function below does nothing, installs no hook,
// reads no key and never touches the player's files or saves.
#include <cstdint>

#include "xr.h"

namespace b2s::devtools {

// False in the release build.
bool Available();

// Once, at startup (hooks of the developer tools).
void Install();

// Game thread, once per Present (their hotkeys).
void Frame();

// Game thread, wherever controller input reaches the game: the input the game should get.
xr::FrameInput Filter(const xr::FrameInput& in);

// An overlay image in front of the head (any thread): false while none is shown. `version` changes with every redraw.
constexpr int kOverlayWidthPx = 1000, kOverlayHeightPx = 1300;
constexpr float kOverlayWidthM = 1.0f, kOverlayHeightM = kOverlayWidthM * float(kOverlayHeightPx) / float(kOverlayWidthPx);
struct Overlay {
    pose::Vec3 center{};  // seated reference frame (yaw-only, recentered head at the origin)
    float yaw = 0.f;
    uint64_t version = 0;
};
bool GetOverlay(Overlay* out);
// Copies the current image (kOverlayWidthPx x kOverlayHeightPx, B8G8R8A8, opaque) into dst; returns its version.
uint64_t CopyPixels(uint32_t* dst);

// Dev verbs (b2svr_cmd.txt); false when the verb is not theirs.
bool Command(const char* verb, const char* arg);

}  // namespace b2s::devtools
