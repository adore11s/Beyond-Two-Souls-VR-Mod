#pragma once
#include "xr.h"
#include "vrinput_math.h"

namespace b2s::vrinput {
// Guarded native Steam seams virtualize one controller; real devices pass through.
bool Install();
void Suspend();
void Publish(const xr::FrameInput& frame, bool firstPersonGameplay, float cameraRelativeHeadYaw);
// A head-button first-person switch happened (vrinput_math.h HeadSwitch) and fpcam has not taken it yet;
// cleared by the call. A switch older than 1 s is dropped.
bool ConsumeHeadSwitch();
LegacyState Snapshot();
void LogStatus();
// Controller type the virtual pad reports: 2 = Xbox-style gamepad (default, F10),
// 5 = PlayStation (F11). Switching briefly unplugs the virtual pad so the game re-creates it.
void SetPadType(int type);
int PadType();
} // namespace b2s::vrinput
