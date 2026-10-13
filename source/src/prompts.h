#pragma once
// In-world prompts (iconic MPAR: interaction words / dialogue choices such as a name to call out)
// that the game anchors at Jodie's head are drawn above her left hand in F1 first person, where
// the player can see them (at her head they sit at or behind the first-person eye).
#include <cstdint>

namespace b2s::prompts {

bool Install();
// Dev verb / b2svr.ini fp_prompts_on_hand: on (default) or off.
void SetEnabled(bool on);
bool Enabled();
// The game is showing in-world prompts (white dots, words) right now: one was updated in the last 250 ms.
bool Showing(uint64_t nowMs);
// GetTickCount64 of the last in-world prompt update, 0 if none yet.
uint64_t LastShownMs();

}  // namespace b2s::prompts
