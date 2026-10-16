# Beyond: Two Souls VR Mod

Play **Beyond: Two Souls (PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Jodie's eyes.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person (F1 / F3) is still being tuned.

## What the mod does

- **Two ways to play.** Keep the game's **normal cameras**, drawn in 3D around you, or switch to **first person** and see everything through Jodie's eyes: **F1** when you move as Jodie, **F3** in cutscenes.
- **Motion controls in first person.** Your VR controllers become Jodie's hands. This is still being worked on: so far it is tested with two of Jodie's looks, **young Jodie in the green shirt** and **adult Jodie in the red dress**. Other outfits may look off for now; I am slowly making them all work.
- **Prompts you can see.** In first person (F1 / F3) the game's prompts appear in front of you.
- **Recommended: play in F1 and switch F3 on only from time to time.** The game was not made for first person, so not every cutscene animation looks good through Jodie's eyes.

## Before you install

Start the game normally once and, in its graphics settings:

- set the display mode to **Windowed** (the mod needs it to draw the square VR picture),
- turn **Motion blur** off,
- turn **Depth of field** off.

## Install

1. Open your Beyond: Two Souls folder. In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon.
2. Copy this whole **`B2S_VR_MOD`** folder into it.
3. Start **SteamVR** and put your headset on.
4. Double-click **`Play Beyond Two Souls in VR.cmd`** in the `B2S_VR_MOD` folder. It asks two things each time (just press Enter to keep your last choice):
   - the **VR picture size** (1 to 6; bigger is sharper but needs a faster graphics card),
   - **smooth or snap turning** in first person (1 or 2).

VR turns on by itself a few seconds after the game starts. From then on you can also start the game straight from Steam or the Epic Games Launcher; it stays in VR until you uninstall.

## Controls

| Key | What it does |
|---|---|
| **Press both thumbsticks** (F1) | First person when you move as Jodie |
| **Hold both thumbsticks** (F3) | First person in cutscenes |
| **F10** | Xbox button prompts |
| **F11** | PlayStation button prompts |
| **F9** | VR off / on (play on your monitor, press again to go back to VR) |

To quickly switch between the first-person modes, use both thumbsticks on your VR controllers, or, if you're playing on keyboard without VR controllers, the keys **F1** and **F3**.

## Uninstall

Double-click **`Uninstall VR mod.cmd`**. It puts the game folder back as it was. **Your saves are never touched.**

## Something went wrong?

Open an issue on the mod's GitHub page (**Issues** → **New issue**) with a few words on what you were doing, and attach:

| What happened | Send |
|---|---|
| **The game crashed** | The newest **`crash_<date>.txt`** and **`crash_<date>_log.txt`** from **`b2svr_logs\crash_reports`** in your game folder |
| **Something looks or works wrong, the game froze, or VR did not start** | **`b2svr_logs\b2svr.log`** from your game folder, right after quitting the game (if you have started the game again since, send **`b2svr.prev.log`** too) |
| **The installer / launcher window showed an error** | **`setup_log.txt`** from this `B2S_VR_MOD` folder |

None of these files contain your user name or other personal information.

## Known bugs

- **The Party level is drawn wrong in VR.** Objects, shadows and the door go missing, and sometimes the whole picture turns plain blue or black, mostly around the house you can walk into (as Jodie and as Aiden). For now, press **F9** to switch VR off and play this level on your monitor, then press **F9** again to go back to VR.
- **First person (F1 / F3) still has drawing problems.** Parts of the picture can go missing or turn black for a moment (for example when you walk into another room), and the lighting on characters can look bad: the game was not made for first person, so characters are lit for the game's normal camera.

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.

*Not affiliated with Quantic Dream or Sony Interactive Entertainment. No game files are included.*
