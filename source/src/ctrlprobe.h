#pragma once
// EXP-055 probe (owner 2026-11-04: "the first person and third person gameplay distinction should be by decompiled
// animation lists and more decompilation of the game"): every engine-side control signal of the first-person
// character, logged whenever one of them changes, so a run through walking, guided walks, sequences and cutscenes shows
// which one the game itself uses. STATIC sources (EXP-055):
//   CHARACTER+0x508  HAVOK character (script EnableController; the mod's current walking signal)
//   CHARACTER+0x340  motion kit: +0x40 stopped (EnableMotionKit / PauseGMK / StopGMK), +0x58 runtime +0xd8 running
//   CHARACTER+0x348  animation player: +0x18 table, 16 entries of 0x18 (+8 animation, +0x10 layer), count +0x180
//                    (StopAllAnim -> FUN_1402aa5e0 walks it)
//   g_activeControllers (0x141ca6d50: array, count +8) - every script controller the engine updates this frame
//                    (FUN_140b391a0, slot 0x3e0); a controller's +0x100 / +0x108 KCOM handles name its entities;
//                    SEQUENCE_CONTROLLER_PLAY: +0x148 playing, +0x118 instance (+0x188 the entity it plays on).
namespace b2s::ctrlprobe {

// Game thread, once per frame (player::Poll). Read-only.
void Frame(void* character);

}  // namespace b2s::ctrlprobe
