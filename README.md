# Beyond: Two Souls VR Mod

Play **Beyond: Two Souls (PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through Jodie's eyes.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person (F1 / F3) is still being tuned.

## What you need

- **Beyond: Two Souls on PC** (Steam, Epic Games Store or another copy). The Steam build is verified; other builds are supported by signature (see GUIDE.md).
- **A VR headset with SteamVR** (or another OpenXR runtime such as Virtual Desktop). Tested on PlayStation VR2.
- **A strong graphics card.** The game is drawn twice, once per eye.

## Install

1. Open your Beyond: Two Souls folder. In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon.
2. Copy this whole **`B2S_VR_MOD`** folder into it.
3. Start **SteamVR** and put your headset on.
4. Double-click **`Play Beyond Two Souls in VR.cmd`** in the `B2S_VR_MOD` folder.

VR turns on by itself a few seconds after the game starts. From then on you can also start the game straight from Steam or the Epic Games Launcher; it stays in VR until you uninstall.

## Keys

| Key | What it does |
|---|---|
| **F9** | VR on / off |
| **F2** | Recenter your view |
| **F1** | First person while you play as Jodie |
| **F3** | First person in cutscenes |
| **F10** / **F11** | Xbox / PlayStation button prompts (remembered) |

The keys work while the game window or a SteamVR window is in front.
On the VR controllers: **click both thumbsticks** for F1, **hold both thumbsticks** (about a second) for F3.

➡️ All controls, settings and fixes: **[GUIDE.md](GUIDE.md)**

## Uninstall

Double-click **`Uninstall VR mod.cmd`**. It puts the game folder back as it was. **Your saves are never touched.**

## It crashed?

A crash report is saved in **`b2svr_logs\crash_reports`** inside your game folder. It contains no personal information. Please send it along with a few words on what you were doing.

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
Anyone can fix, improve or build on it: see **[CONTRIBUTING.md](CONTRIBUTING.md)**.

*Not affiliated with Quantic Dream or Sony Interactive Entertainment. No game files are included.*
