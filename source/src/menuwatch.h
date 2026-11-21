#pragma once
// The VR panel in the main menu's Options (playbook 10.6, 1.0.3). Beyond's main menu is a 3D scene (MENU_GAME, LIVE
// 0.18.0) whose pages are MenusMaster screens driven by the scene script MENU_GAME_fMenu. STATIC (the decoded script,
// tools/lua51_calls.py over BigFile_PC): the menu's event handler (MENU_GAME_fMenu/fn224) answers each page's camera
// event by killing the other page controllers and playing that page's camera sequence on a SEQUENCE_CONTROLLER_PLAY:
//   MM_CAM_1 -> MENU_GAME_sNormal (object 6346)    MM_CAM_2 -> MENU_GAME_sChapters (6341)
//   MM_CAM_3 -> MENU_GAME_sContinue (6342)         MM_CAM_4 -> MENU_GAME_sOptions (6345)
//   MM_CAM_5 -> MENU_GAME_sExtra (6343)            MMS_Button_Validation -> MENU_GAME_sValidation (6340)
// LIVE 1.0.3 (the owner in the main menu's Options, read with the mod's peek): SEQUENCE_CONTROLLER_PLAY +0x110 is the
// KCOM handle of the SEQUENCE it plays (+0 class 4288, +4 the script's object id, +0x18 the name holder), and with
// Options open one reads 4288 / 6345 / "MENU_GAME_sOptions" (next to the menu's background sequences MENU_GAME_sSmooth
// and MENU_GAME_s26Epilogues). The engine updates every live script controller each frame from g_activeControllers
// (0x141ca6d50; EXP-055) and MM_CAM_x kills the other pages' controllers, so a live controller playing
// MENU_GAME_sOptions = the Options page is shown. The page is a setting (b2svr.ini options_menu_page, default
// MENU_GAME_sOptions; id 6345 when the name is missing); every page change is logged ("MENU main menu page ...").
namespace b2s::menuwatch {

// Game thread, once per frame (player::Poll): `menuScene` = the player character belongs to a menu scene.
void Frame(bool menuScene);
// Any thread: the main menu's Options page is shown now (the VR panel stands beside it).
bool OptionsOpen();

}  // namespace b2s::menuwatch
