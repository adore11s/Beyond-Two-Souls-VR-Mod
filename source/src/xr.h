#pragma once
#include <cstdint>
#include "posemath.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// OpenXR session on the game's own D3D11 device (docs/STEREO_ROUTE.md, playbook PORT-08).
//
// Frame contract (one XR frame per recorded stereo pair):
//   game thread   BeginGameFrame(): poll events, xrWaitFrame, xrLocateViews -> eye transforms
//   render thread OnEye(0): xrBeginFrame (strictly after the previous xrEndFrame), copy eye 0
//   render thread OnEye(1): copy eye 1, xrEndFrame with the poses the pair was rendered with
namespace b2s::xr {

// XR frame slots; the slot of a recorded stereo pair travels in its eye marker names.
constexpr int kSlots = 8;

// Per-eye data for the camera build, in the engine's convention (row-major, row-vector,
// right-handed, Y up, -Z forward, metres).
struct EyeView {
    float viewOffset[16];  // post-multiply onto the engine view: V_eye = V * viewOffset
    float tanLeft, tanRight, tanUp, tanDown;
};

// Immutable game-thread snapshot from the same predicted time and reference as both eyes.
// A missing/untracked hand is neutral; unlike the head, controller poses are never held.
struct HandInput {
    pose::Mat4 gripRel = pose::Identity(), aimRel = pose::Identity();
    bool active = false, gripValid = false, aimValid = false;
    float stick[2] = {0.f, 0.f};
    float trigger = 0.f, squeeze = 0.f;
    bool a = false, b = false, stickClick = false, menu = false;
};
struct FrameInput {
    uint64_t serial = 0, tickMs = 0;
    int64_t displayTime = 0;
    uint32_t recenterEpoch = 0;
    bool focused = false;  // usable live head tracking in a focused session
    pose::Mat4 headRel = pose::Identity();
    HandInput hand[2];  // left, right; a/b = lower/upper face button on either hand
};
inline bool FrameInputUsable(const FrameInput& input, uint64_t now) {
    return input.serial != 0 && input.focused && input.tickMs <= now && now - input.tickMs <= 250;
}

// Game thread at Present (render thread idle): starts the session when wanted, shuts it down
// fully when not wanted or when the runtime exits/loses it, and retries a failed start
// every 5 s (docs/RECOVERY_MATRIX.md).
void Service(ID3D11Device* device, bool wanted);
bool Running();
// Diagnostic: true while the left eye image is dark (and 0.3 s after), for the first 6 dark spells.
bool DarkTraceActive();
// True while the left eye image is darker than 3% (read back one or two frames late).
bool ViewDark();
// Headset / runtime names from the last successful start ("" before); for crash reports.
const char* SystemName();
const char* RuntimeName();
// XR frames ended so far (stereo and flat); lets Present detect a frame that never completed.
uint64_t FramesEnded();

// Game thread, once per frame before the stereo recording. Returns the XR frame slot this
// recording belongs to (eyes[] then holds the poses to render with), or -1 for no XR frame.
// Optional input is always overwritten, including a neutral snapshot on an early exit.
int BeginGameFrame(EyeView eyes[2], FrameInput* input = nullptr);

// Render thread, at each eye marker. src is the full backbuffer-sized eye image;
// the scene occupies rows [srcTop, srcTop + srcHeight).
// uiFollows: the frame is ended by OnUiEnd (UI quad) instead of after eye 1.
void OnEye(int eye, int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t srcTop,
           uint32_t srcHeight, bool uiFollows);

// Render thread, around the last pass's Display2D: the UI is drawn over transparent black,
// lifted onto a floating quad layer, and the flat mirror (scene + UI) is restored.
void OnUiBegin(int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer);
void OnUiEnd(int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11Texture2D* eyeImage);

// Game thread at Present, for frames with no stereo recording (pause menu, splash and loading
// screens): runs a whole XR frame that shows the flat backbuffer on the floating panel.
// showPanel true: a 2D-only screen, shown alone on the floating panel in black space.
// showPanel false: a stalled stereo pair; lastEyes (the last pair's images) are re-shown.
// Optional input is sampled live at this frame's display time, independent of held eyes.
bool FlatFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11Texture2D* const lastEyes[2],
               bool showPanel, FrameInput* input = nullptr);

// Recenters the seated reference on the current head yaw and position.
void Recenter();
// Monotonic recenter generation (PORT-06): every consumer of a frame reads the same one.
uint32_t RecenterEpoch();
bool GetInteractionProfile(int hand, char* output, uint32_t capacity);

// Diagnostics: save the next UI capture (colour + alpha); put an opaque known image on the quad.
void RequestUiShot();
// Game thread, right before ResizeBuffers (render thread idle): drop the backbuffer view.
void ReleaseBackbufferViews();
void RequestFlatShot();  // next 2D-only frame: save the panel source and the backdrop eye image
void SetUiTestPattern(int mode);
// UI panel alpha: 3 (default) brightness coverage, drops black overlays such as cutscene
// letterbox bars; 2 keeps every drawn pixel.
void SetUiAlphaMode(int mode);
// EXP-014 discriminator: render and submit symmetric per-eye frustums.
void SetSymmetricFov(bool on);
// EXP-018: offset the virtual head (metres up, degrees pitch) for capture tests; 0 0 = off.
void SetHeadTestOffset(float dy, float pitchDeg);

}  // namespace b2s::xr
