#pragma once
// Bink video activity. The game decodes every video through bink2w64.dll, whose functions it
// imports by name on every build (Steam, Steam demo, Epic), so no game address is needed.
// xr uses it to tell a full-screen video in the 2D layer (a "flat cutscene", owner 0.16.9: shown
// as a see-through panel over the 3D scene) from ordinary UI drawn over the scene.
#include <cstdint>

namespace b2s::videowatch {

// Hooks the Bink exports once (MinHook must be initialised). Safe to call again.
void Install();

// True while a video frame was decoded in the last 400 ms.
bool Playing(uint64_t nowMs);

}  // namespace b2s::videowatch
