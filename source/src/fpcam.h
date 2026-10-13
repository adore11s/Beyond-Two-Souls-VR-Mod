#pragma once
#include <cstddef>

#include "posemath.h"
#include "xr.h"
// F1 first-person camera glue (EXP-031): keys, mode, first-person base pose, probes.

namespace b2s::fpcam {

// Installs the engine seams (bones, camera director). Safe to call once after anchors resolve.
void Install();

// Game thread, start of every recorded list (before the eye passes).
void Frame();

// One game-thread snapshot, before stereo changes the primary camera node.
void Prepare(void* cameraNode, const pose::Mat4& authoredCamera, const xr::FrameInput& input);
bool Base(void* cameraNode, pose::Mat4* out);
void Suspend();
void BeginRender();
void EndRender();

// True while F3 first person is drawn in a cutscene (the view is at Jodie's eyes, not the shot camera).
bool CutsceneFirstPerson();
// True while first person (F1 or F3) is drawn.
bool FirstPersonDrawn();


// Crash report line: F1/F3 settings and the current mode. Lock-free (may run in a crashed process).
void DescribeForCrash(char* out, size_t cap);

// Dev verbs: fpprobe [frames]. Returns true when handled.
bool Command(const char* verb, const char* arg);

}  // namespace b2s::fpcam
