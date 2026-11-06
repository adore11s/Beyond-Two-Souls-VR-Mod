#pragma once
// Active camera behaviour of the game's CAMERA_DIRECTOR (EXP-031 task 3). STATIC: the director
// keeps a behaviour stack (+0xE0 array of KCOM handles, +0xF0 count; element 0 is active, empty ->
// default handle at 0x141c9b308); each behaviour's class is identified by its vtable.
#include "fpmode.h"

namespace b2s::camdirector {

// Hooks CAMERA_DIRECTOR::vfunc_110 (per-frame) to learn the director instance; guarded.
bool Install();

// Active behaviour object, or nullptr when unknown.
void* Active();

// Behaviour -> class group for fp::Classify.
fp::BehaviourClass Classify(void* behaviour);

// Short class name for logs ("CHASE", "SHOT", ...), "?" when unknown.
const char* Name(void* behaviour);

// Sub-behaviour at +0x588 (selectors/containers), or nullptr.
void* Child(void* behaviour);

// A camera filter (CAMERA_MODIFIER_CAMERA_SHADER: a screen shader on the game camera, e.g. the
// security-camera look) that belongs to a camera is switched on right now. Filters owned by the camera
// director itself (level-wide looks such as the submarine's underwater shader, EXP-061) do not count.
bool FilterActive();

}  // namespace b2s::camdirector
