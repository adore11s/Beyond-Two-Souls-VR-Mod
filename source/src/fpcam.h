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

// The player's first-person choices (VR settings panel, F1 / F3, the head switch). Every setter saves to
// b2svr.ini at once: firstperson, fpcutscenes, fparms (motion controls: the VR controllers move Jodie's arms),
// hand_smoothing (the drawn hands of motion controls through the playbook 08.1 filter), turn (smooth / snap),
// snap_turn_degrees, turn_speed (smooth turning, a fraction of 120 deg/s).
bool GameplayFP();
bool CutsceneFP();
void SetGameplayFP(bool on);
void SetCutsceneFP(bool on);
bool MotionControls();
void SetMotionControls(bool on);
bool HandSmoothing();
void SetHandSmoothing(bool on);
// "Aiden possessions (first person)" (EXP-046, player::PossessionFirstPerson): b2svr.ini fp_possession.
bool PossessionFP();
void SetPossessionFP(bool on);
bool SnapTurn();
void SetSnapTurn(bool on);
float SnapDegrees();
void SetSnapDegrees(float degrees);
float TurnSpeed();
void SetTurnSpeed(float fraction);

// True while F3 first person is drawn in a cutscene (the view is at Jodie's eyes, not the shot camera).
bool CutsceneFirstPerson();
// True while first person (F1 or F3) is drawn.
bool FirstPersonDrawn();
// The headset eye's world position in first person (last prepared frame); false outside first person.
bool EyeWorld(float out[3]);


// Crash report line: F1/F3 settings and the current mode. Lock-free (may run in a crashed process).
void DescribeForCrash(char* out, size_t cap);

// Dev verbs: fpprobe [frames]. Returns true when handled.
bool Command(const char* verb, const char* arg);

}  // namespace b2s::fpcam
