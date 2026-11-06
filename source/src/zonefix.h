#pragma once
// First-person room visibility (0.16.35, EXP-040). The engine's portal visibility for a viewport starts
// from its camera's room (SCENE_ZONE at CAMERA+0x98, copied to VIEWPORT+0x41e0 by FUN_14086b540 /
// FUN_14086b410 / FUN_14086b300 every pass). In first person the drawn eye is in Jodie's head, but that
// room still follows the game's authored camera: walking through a door before the game cuts to the
// next room's camera (or a cutscene camera in another room) left the eye in a room the visibility pass
// never reached, and the view went black until the cut (owner: "black for a second when entering a
// room"; LIVE 0.16.34: render-thread draws ~1600 -> ~800 per frame for exactly the dark second).
#include <cstdint>

namespace b2s::zonefix {

// 0.24.0: resolves the engine's point -> room lookup (FUN_14092e2b0) and hooks the room query (FUN_1409d9ca0) for the
// all-rooms case. Call once with the other render hooks.
void Install();
// Every 30 s with the other statistics.
void LogStats();

// Game thread, once per frame (after player / first-person state is known).
void Frame();

// Game thread, around each eye pass of the drawn list: the stereo cameras' room is set to the room the
// engine puts the eye in for the pass, then put back; with the eye between room volumes the pass's room queries see
// every room instead (0.24.0).
void BeginPass(void* const* cams, int count);
void EndPass();

bool Command(const char* verb, const char* arg);

// Diagnostic (any thread): first person is drawn and the eye passes start from another room than the game camera's.
bool EyeRoomDiffers();

}  // namespace b2s::zonefix
