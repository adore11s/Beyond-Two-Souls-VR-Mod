# Beyond: Two Souls VR Mod

Play **Beyond: Two Souls (PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Jodie's eyes, with your VR controllers as her hands. Works with the Steam and Epic Games Store versions of the game.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person is still being tuned.

## Important: read before playing

- **Only playing in third person?** Skip the first-person points below. Your controllers work like a gamepad and the game keeps its own cameras, drawn in 3D around you.
- **Game settings:** no need to change them. The installer sets what VR needs (Windowed, Motion blur off, Depth of field off).
- **First person on / off:** hold your right hand next to your head and press **B / ○**. Press it while you walk as Jodie to switch first person for walking, or during a cutscene to switch it for cutscenes. By default it is on for walking and off for cutscenes. You can also change both in the VR settings panel.
- **Recommended:** play with first person for walking and switch on first person in cutscenes only from time to time. The game was not made for first person, so not every cutscene animation looks good through Jodie's eyes.
- **Menus:** press **all four face buttons** (A B X Y / ✕ ○ □ △) together and let go for the game's pause menu. The menu button works too.
- **VR settings:** in the game's pause menu, a panel appears on the right. Point a controller at it and pull the trigger.

## What the mod does

- **Native stereo rendering.** The game is drawn separately for each eye, with full head tracking.
- **Two ways to play.** Keep the game's **own cameras**, drawn in 3D around you, or play in **first person** through Jodie's eyes: while you walk her and in cutscenes. Whether you walk her or the game animates her is read from the game itself.
- **Motion controls in first person.** Your VR controllers are Jodie's hands: her arms, wrists and fingers follow them, and whatever she holds stays in her hand. Her body turns with you and the left stick walks where you look.
- **Xbox or PlayStation controls**, with matching button icons in the game (VR settings panel).
- **Rendering method:** Native (both eyes every frame, the default), AFR (one eye per frame, lighter on the graphics card) or Mono (one picture for both eyes, the fastest), set in the VR settings panel.

## Install

1. Start the game normally once and quit it (so it has made its options file).
2. Open your Beyond: Two Souls folder. In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon.
3. Copy this whole **`B2S_VR_MOD`** folder into it.
4. Double-click **`Install VR mod.cmd`** in the `B2S_VR_MOD` folder. You only do this **once** (and again after downloading a new version).
5. From now on, start **SteamVR**, put your headset on and start the game as usual, from Steam, Epic or the exe.

VR turns on by itself a few seconds after the game starts. Using Virtual Desktop (**VDXR**) or another OpenXR runtime instead? Connect your headset to it first. The mod's settings file (`b2svr.ini`) sits next to the game's exe.

## Controls

| Button / key | What it does |
|---|---|
| **B / ○ with your right hand next to your head** | First person on / off - press it while walking for walking, during a cutscene for cutscenes |
| **All four face buttons** together, then let go | The game's pause menu, with the **VR settings** panel next to it |
| **F9** | VR off / on (play on your monitor, press again to go back to VR) |

➡️ All controls, settings and fixes: **[GUIDE.md](GUIDE.md)**

## Uninstall

Double-click **`Uninstall VR mod.cmd`** in the `B2S_VR_MOD` folder. It moves the mod's own files to the **Recycle Bin** (nothing is deleted for good) and puts back the game options the installer changed, unless you changed them yourself since. **Your saves and the game's files are never touched.**

## Something went wrong?

Open an issue on the mod's GitHub page (**Issues** → **New issue**) with a few words on what you were doing, and attach:

| What happened | Send |
|---|---|
| **The game crashed** | The newest **`crash_<date>.txt`** from **`b2svr_logs\crash_reports`** in your game folder - just this one file, it holds everything |
| **Something looks or works wrong, the game froze, or VR did not start** | **`b2svr_logs\b2svr.log`** from your game folder, right after quitting the game (if you have started the game again since, send **`b2svr.prev.log`** too) |
| **The install or uninstall window showed an error** | A screenshot of that window |

The mod writes these files itself while the game runs (`b2svr.log` is started fresh each time; the run before is kept as `b2svr.prev.log`).

None of these files contain your user name or other personal information.

## Known bugs (fixes coming soon)

- **In some levels the shadows are janky.**
- **First person mode is still being worked on:** the motion controls, holding items and similar things aren't perfect yet.

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.
The mod includes the official OpenXR loader and MinHook: see **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

*Not affiliated with Quantic Dream or Sony Interactive Entertainment. No game files are included.*
