#pragma once
// The player's character and control state (EXP-031 task 4).
//
// LIVE (young Jodie, 0401 JODIE_AND_ANAEL): ANAEL_GAMEPLAY (Aiden's gameplay object, present whenever
// Aiden is tethered to Jodie) holds KCOM handles to Jodie's CHARACTER at +0xb8 and Aiden's at +0xc0.
// Its byte +0xa8 is 1 while the player controls Aiden. Jodie's CHARACTER+0x508 holds her
// HAVOK_CHARACTER while her physics character controller is attached (STATIC: FUN_140ed5240 clears
// it on detach). It is null in cutscenes and while Aiden is controlled, so it is the "can walk" signal.
//
// Possessions (EXP-047, STATIC; replaces EXP-046's THIRD_PERSON_GAMEPLAY::vfunc_108 hook, which never ran: LIVE
// 0.19.0, not even while Jodie walked). Every body Aiden can possess has a POSSESSION_CENTRE_CONTROLLER (vtable
// 0x14178b5d0): +0x108 KCOM handle to that body ("Character"; FUN_14110c350 compares its position with Aiden's
// character ANAEL_GAMEPLAY +0xc0 against the selection radius), +0x130 state: -1 from the constructor
// (FUN_14110dc10), 0 when the scripts call its method PossessionStarted (FUN_14110cd70), 2 on PossessionEnded
// (FUN_14110cbb0), 1 while ending (vfunc_124 fires OnPossessionEnd, then 2). Its vfunc_124 (0x14110c4f0) is the
// per-frame update the engine's controller loop (FUN_140b391a0, vtable slot 0x3e0, dt in xmm1) calls on every
// active controller; it is hooked to see the controller whose state is 0, and that body is the possessed one.
namespace b2s::player {

enum class Control { Unknown, Walking, Cutscene, Aiden };

struct State {
    // The first-person character: Jodie (SINT::CHARACTER), or the body Aiden possesses while
    // `possessed` is set (only with the "Aiden possessions" option on). nullptr when unknown.
    void* jodie = nullptr;
    bool possessed = false;
    // Aiden possesses someone now, whether or not first person follows that body (the option): the head switch
    // then toggles the option (fpcam::Frame).
    bool possessing = false;
    // The character belongs to a menu scene (the main menu's "MENU_GAME_Jodie"): no first person there.
    bool menu = false;
    Control control = Control::Unknown;
    // EXP-059 (riding): the motion graph her motion kit is synced to (the horse's), nullptr when it is not synced.
    // STATIC: the script method SyncMotionKit (FUN_140dce790 -> FUN_140dce550 -> FUN_140a1afa0) sets
    // CHARACTER+0x340 (motion kit) -> +0x50 (motion graph) -> +0x788 to the other character's graph; UnsyncMotionKit
    // (SCRIPT_ENTITY_ABC::vfunc_03 0x140dce1a0) clears it. 2003 Navajo Day 2: fControlHorse calls
    // Jodie:SyncMotionKit(HorseAshkii, "IW_JHO20_HUNTINT01_SKELETON") when the player takes the reins.
    // `kitRead`: she has a motion kit to read (cutscenes have none; then `synced` says nothing).
    void* synced = nullptr;
    bool kitRead = false;
    // Her physics character controller is attached (CHARACTER+0x508, see the header comment). EXP-060: story cutscenes
    // run with it detached; interaction animations (the 2003 corral, mounting) keep it.
    bool physics = false;
};

// Installs the POSSESSION_CENTRE_CONTROLLER update hook (possessions); without it there are none.
void Install();

// Game thread, once per frame. Keeps the cached ANAEL_GAMEPLAY valid; when it is not, starts a
// background heap scan (rate-limited) and returns Control::Unknown until one is found.
State Poll();
// The last Poll result (any thread; EXP-060 tour). Empty before the first Poll.
State Last();

// "Aiden possessions (first person)": a possessed body becomes the first-person character.
// b2svr.ini fp_possession=on|off (default off, owner 2026-10-28); saved by the setter.
bool PossessionFirstPerson();
void SetPossessionFirstPerson(bool on);

// The Jodie model family of a character (bodykind.h): from its model's resource name when the game
// keeps one, else from its scene (the character's name). `why` gets the evidence for the log.
int Family(void* character, char* why, size_t cap);

// World matrices (row-major 4x4, translation at [12..14]) of Jodie's head bone and scene node.
bool HeadWorld(void* jodie, float out[16]);
bool BodyWorld(void* jodie, float out[16]);

const char* Name(Control c);

}  // namespace b2s::player
