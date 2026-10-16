#pragma once
#include <cstdint>

struct IDXGISwapChain;

// Native stereo, rung R1 variant B (docs/STEREO_ROUTE.md, experiments/EXP-007):
// the game thread records VIEWPORT_LIST::Display once per eye with the eye offset applied
// through CAMERA::Build's lens shift, and appends a named marker before each eye's 2D pass.
// The render thread reaches that marker after the eye's 3D scene is finished and copies the
// backbuffer into that eye's texture.
namespace b2s::stereo {

using ListDisplayFn = void (*)(void* list, void* cmdbuf, uint32_t mode, uint8_t flag);

void Install();

// Game thread: replaces a single VIEWPORT_LIST::Display call when stereo is on.
void RecordList(ListDisplayFn orig, void* list, void* cmdbuf, uint32_t mode, uint8_t flag);

// Game thread, inside CAMERA::Build: extra view-space X offset for the eye being recorded.
float EyeOffset();

// Game thread, right after CAMERA::Build: apply the headset eye view/projection (XR only).
void BeforeCameraBuild(void* cam);
// EXP-019: the aspect CAMERA::Build should use (headset aspect for stereo cameras in an eye pass).
float BuildAspect(void* cam, float gameAspect);
void AfterCameraBuild(void* cam);

// Render thread: called for every engine marker (cheap); performs deferred XR startup.
void RenderThreadTick();

// Render thread: called for markers named "B2SVR:..."; returns true when consumed.
bool OnMarker(const char* name);

// Game thread, once per Present.
void OnPresent(IDXGISwapChain* sc);

// Game thread, immediately before the game's Present: lift the UI onto the XR quad, restore
// the flat mirror and end the XR frame.
void BeforePresent();

// VR requested (b2svr.ini xr=on, F9 or the "xr on" verb), whether or not the session runs yet.
bool XrWanted();

// devctl verbs: "stereo on|off", "ipd <m>", "suppress on|off", "sshot <name>", "xr on|off", "recenter".
bool Command(const char* verb, const char* arg);

}  // namespace b2s::stereo
