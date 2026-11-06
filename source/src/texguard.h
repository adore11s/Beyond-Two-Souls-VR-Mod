#pragma once
#include <cstddef>

// EXP-054: textures used after the game destroyed them (owner crash 2026-11-04, 0.21.2, 2001 Navajo Arrival).
//
// LIVE (WER dump): the render thread called a pure virtual function on a KRENDERER::TEXTURE_DX11 whose destructor had
// already run (its vtable was the abstract I_TEXTURE one, 0x141734f28): RENDERER_SHADER_CUSTOM::vfunc_01 ->
// FUN_140816f00 (binds a shader's textures) -> FUN_140cbeb00 (texture view) -> slot +0x88 = _purecall -> abort().
// STATIC: a renderer shader keeps raw texture pointers (+0x47a count, +0x480 array; the engine SHADER_CUSTOM fills them
// from its engine-texture handles at +0x5b0, FUN_14090fe80 / FUN_14090fdc0, once per frame in SHADER_CUSTOM::vfunc_03
// and when a streamed texture swaps). Old textures go through FUN_140c7b080 (deferred: a list flushed by
// FUN_140c7b360 once the render thread has finished the frame, FUN_140828650) - so a slot that still names one after
// the flush is a dangling pointer the engine did not refresh. Which path left it is not known yet.
//
// Guard: before a shader's textures are bound (FUN_140816f00, and the one slot RENDERER_SHADER_SKIN::vfunc_01 reads), a
// slot whose texture carries the I_TEXTURE vtable (destroyed, memory not reused yet) is repaired: it gets what the
// engine's refill would give it now (its engine texture's current texture), else the game's not-ready texture
// (RenderState+0xb60); 0.21.3 left it empty, which draws black (owner, 0.21.3). Each catch is logged with the
// shader, the texture's name and who released and destroyed it (a ring of the last releases / destructions, with their
// game call stacks), so the next catch names the engine path.
namespace b2s::texguard {

// Render / game threads hooks (sigdb addresses; a missing one leaves its part off). After game::ResolveAll.
void Install();
// The 30 s statistics line (stereo's frame stats): releases, destructions and caught dead textures.
void LogStats();
// 0.21.7, game thread, once per frame before the eye passes are recorded: the engine's own per-frame refill
// (SHADER_CUSTOM::vfunc_03) for the shaders drawn since the last call that the engine did not refill itself.
void RefillDrawn();
// Dev command "texguard test": runs the guard on a fake shader in the mod's own memory (one destroyed texture, one live
// one, one with a loaded engine texture) and logs whether each was repaired right.
void SelfTest();
// For a crash report: the last caught dead textures and the most recent destructions (one line each).
void DescribeForCrash(void (*add)(const char* line, void* ctx), void* ctx);

}  // namespace b2s::texguard
