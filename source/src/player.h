#pragma once
// The player's character and control state (EXP-031 task 4).
//
// LIVE (young Jodie, 0401 JODIE_AND_ANAEL): ANAEL_GAMEPLAY (Aiden's gameplay object, present whenever
// Aiden is tethered to Jodie) holds KCOM handles to Jodie's CHARACTER at +0xb8 and Aiden's at +0xc0.
// Its byte +0xa8 is 1 while the player controls Aiden. Jodie's CHARACTER+0x508 holds her
// HAVOK_CHARACTER while her physics character controller is attached (STATIC: FUN_140ed5240 clears
// it on detach). It is null in cutscenes and while Aiden is controlled, so it is the "can walk" signal.
namespace b2s::player {

enum class Control { Unknown, Walking, Cutscene, Aiden };

struct State {
    void* jodie = nullptr;  // SINT::CHARACTER, nullptr when unknown
    Control control = Control::Unknown;
};

// Game thread, once per frame. Keeps the cached ANAEL_GAMEPLAY valid; when it is not, starts a
// background heap scan (rate-limited) and returns Control::Unknown until one is found.
State Poll();

// World matrices (row-major 4x4, translation at [12..14]) of Jodie's head bone and scene node.
bool HeadWorld(void* jodie, float out[16]);
bool BodyWorld(void* jodie, float out[16]);

const char* Name(Control c);

}  // namespace b2s::player
