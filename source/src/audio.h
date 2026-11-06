#pragma once
// Head-tracked 3D sound (playbook chapter 17 / REQ-16), STATIC 2026-10-28 (EXP-044).
//
// Beyond mixes its 3D sounds itself on XAudio2 (KSOUND, no X3DAudio): every positional sound buffer picks the
// nearest of up to ten listeners (table 0x141c8b550, FUN_140cd9590) and is panned against that listener's world
// matrix (+0x80) by FUN_140cea540. Each sound camera's per-frame task (FUN_1411d4210 / FUN_1411da390) feeds the
// CAMERA's node (CAMERA +0xA0) to its listener: SetPosition(listener, node world row 3) (FUN_140ce55b0, eased 9 %
// per call) then SetOrientation(listener, world(0,0,-1), world(0,1,0)) (FUN_140ce57c0).
//
// With the headset on, both calls get the head instead of the game camera: in third person the head's offset
// from the game camera (what the eyes show), in first person the head where first person places it. Only a
// listener fed with the game camera's own position is changed; VR off, flat screens and other listeners keep
// the game's own. b2svr.ini audio_head=on|off.
#include "audio_math.h"

namespace b2s::audio {

bool Install();
// Game thread, once per recorded stereo frame: the head of this frame and the game camera's own world, or
// valid=false (no VR, no main camera).
void Publish(const pose::Mat4& head, const pose::Mat4& authored, bool valid);
// Dev verb: "audiohead on|off".
bool Command(const char* verb, const char* arg);

}  // namespace b2s::audio
