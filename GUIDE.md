# Beyond: Two Souls VR Mod — Guide

Everything beyond the basics in the [README](README.md).

- [What the mod does](#what-the-mod-does)
- [Supported game versions](#supported-game-versions)
- [Controls](#controls)
- [Settings file (b2svr.ini)](#settings-file-b2svrini)
- [What the mod changes on your PC](#what-the-mod-changes-on-your-pc)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)

---

## What the mod does

- **Real stereo 3D.** Every frame is drawn twice, once per eye, with full head tracking (turning and leaning).
- **The game's own cameras stay in charge.** In third person your head moves on top of the camera the game chose, so scenes keep their framing.
- **The game's interface** (prompts, subtitles, HUD) floats in front of you as a separate layer.
- **Menus, videos and loading screens** show on a big screen floating in front of you.
- **No frame cap and no VSync while VR runs.** The headset sets the pace (for example 90 frames per second on PlayStation VR2).
- **First person (F1).** While you control Jodie you see through her eyes. Her head, hair and earrings are hidden so they never block your view, your VR controllers move her arms and hands (and whatever she holds), her body turns with you, and the left stick walks where you look. While she stands still her body holds still too, instead of swaying with the idle animation.
- **First person in cutscenes (F3).** Cutscenes are shown from Jodie's eyes too, facing where she looks.
- **Prompts you can see.** In first person, words and dialogue choices the game puts at Jodie's head (a name to call out, a reply) appear in front of you instead.
- **Steady lighting.** Lights near you stay on wherever you look, so characters do not flicker between lit and unlit as you turn your head.
- **Aiden** keeps the game's own first-person view: the mod never changes it.
- **VR controllers work as a gamepad.** No extra drivers are needed.

---

## Supported game versions

The mod works on any PC build of Beyond: Two Souls: Steam, Epic Games Store, or a copy from anywhere else.

| Build | Status |
|---|---|
| Steam (`BeyondTwoSouls_Steam.exe`, build stamp `0x5eb531da`) | Verified: every game address was recorded on this build |
| Any other build (Epic Games Store, other Steam updates, other copies) | Unverified: the mod finds its game addresses by signature |

Every time the game starts, the mod looks at the game's code:
- On the verified build it checks each recorded address against its signature.
- On any other build it searches for each address by signature. `b2svr_logs\b2svr.log` lists every address as `verified`, `MOVED` (found at a new place), `MISSING` or `AMBIGUOUS`, then one summary line.
- A feature whose addresses are missing or ambiguous switches itself off; the rest keeps working.
- If the core VR view cannot be found, the whole mod switches off (`FAIL-CLOSED` in the log) and the game runs normally on your monitor, so nothing breaks.

Steam copies: the installer also writes `steam_appid.txt` (the game's Steam app id) into the game folder when there is none, so the exe can be started directly. The uninstaller removes it only if the installer created it. Epic Games copies need nothing extra: the installer starts them through the Epic Games Launcher.

Two `b2svr.ini` switches, both off by default:
- `verified_build_only=on`: stay off on any build except the verified one.
- `signature_scan=force`: search by signature even on the verified build (testing).

---

## Controls

### Keyboard

These work while the game window or a SteamVR window is in front.

| Key | What it does |
|---|---|
| `F9` | VR on / off |
| `F2` | Recenter your view (SteamVR's own recenter works too) |
| `F1` | First person while you play as Jodie, on / off |
| `F3` | First person in cutscenes, on / off |
| `F10` | Xbox button prompts (saved in `b2svr.ini`) |
| `F11` | PlayStation button prompts (saved in `b2svr.ini`). Prompts that ask you to tilt or move the controller (motion sensor) use your right VR controller: tilt it or move it up / down / left / right |

F1 and F3 are independent: F1 alone keeps cutscenes in the game's cameras, F3 alone keeps gameplay in third person, and both together give first person throughout.

### VR controllers (they act as a gamepad)

| Controller | Gamepad |
|---|---|
| Left stick / right stick | Left stick / right stick |
| Right lower / upper face button (A / B) | A / B |
| Left lower / upper face button (X / Y) | X / Y |
| Left trigger / right trigger | Left trigger / right trigger |
| Left grip / right grip | Left bumper / right bumper |
| Stick clicks | Stick clicks |
| Menu button | Start (pause menu) |
| Left grip **+** menu button | Back |
| Left grip **+** left stick | D-pad |
| Click **both** thumbsticks | F1 (first person) on / off |
| Hold **both** thumbsticks (about a second) | F3 (first person in cutscenes) on / off |

A regular gamepad keeps working at the same time.

On Steam copies the controllers appear as a Steam Input gamepad. On copies without Steam (Epic Games Store) they appear as Xbox controller 1 instead, so the game shows Xbox button prompts there and `F11` does not change them.

### In first person (F1)

| Control | Does |
|---|---|
| Left stick | Walk where you are looking |
| Right stick left / right | Turn (not while white-dot prompts are on screen: then the stick only does the prompt) |
| Your hands | Jodie's hands follow your controllers |

When the game switches to a filtered camera view (for example the security cameras in the DPA rooms when Jodie stands still), first person steps aside and shows the game's camera, then comes back by itself. F3 does the same while Jodie is not animated (standing in a T-pose between scenes).

---

## Settings file (b2svr.ini)

`b2svr.ini` sits next to the game's exe (`BeyondTwoSouls_Steam.exe` on Steam).
- Every setting has a short note explaining it.
- Changes take effect the next time the game starts.
- The installer never overwrites your copy.

| Setting | Default | Meaning |
|---|---|---|
| `xr` | on | off = start on the monitor and press F9 for VR |
| `fps` | 0 | Frame rate cap; 0 = none (recommended in VR) |
| `vsync` | off | Monitor VSync; always off while VR runs |
| `firstperson` | off | on = start with F1 first person switched on |
| `fpcutscenes` | off | on = start with F3 first person in cutscenes switched on |
| `fparms` | on | off = Jodie's arms stay animated by the game in first person |
| `fp_eye_fwd` / `fp_eye_up` | 0.09 / 0.07 | First-person view position in metres, forward of / above the top of Jodie's neck |
| `fp_wrist_pitch` | 0 | Tilts Jodie's hands in first person (degrees; positive = fingers further down) |
| `fp_prompts_on_hand` | on | In first person, words and choices the game shows at Jodie's head appear in front of you (off = where the game puts them) |
| `light_cull_margin` | 1.35 | Lights are chosen for a view this much wider than both eyes |
| `light_cull_back` | 10 | Lights near you stay on wherever you look (0 = the game's own view-based light check) |
| `controller` | xbox | Button prompts: xbox or playstation (F10 / F11 switch it and save it) |
| `verified_build_only` | off | on = stay off on any game build except the verified one |
| `signature_scan` | off | force = find game addresses by signature even on the verified build (testing) |

---

## What the mod changes on your PC

Nothing in the game is replaced, patched or edited on disk.
- The mod is one file, `dinput8.dll`, next to the game's exe. Windows loads it into the game when it starts, and it passes the game's DirectInput calls on to Windows' own `dinput8.dll`.
- If another mod's `dinput8.dll` is already there (an ASI loader, an overlay), the installer stops and leaves it alone.

| File / folder | What it is |
|---|---|
| `dinput8.dll` | The mod |
| `b2svr.ini` | The mod's settings |
| `b2svr_logs\` | The mod's log (`b2svr.log`, the previous run in `b2svr.prev.log`) and crash reports (`crash_reports\`) |

---

## Known limitations

- **First person is still being tuned.** The view height, hand positions and wrist angles may be off in some scenes: adjust them with `fp_eye_fwd`, `fp_eye_up` and `fp_wrist_pitch` in `b2svr.ini`. Press F1 / F3 again to go back to the game's cameras at any time.
- **Cinematic framing.** Some cutscenes keep the game's widescreen framing.
- **Performance.** The game is drawn twice per frame; lower the game's graphics settings if the headset stutters.

---

## Troubleshooting

**The game runs flat, not in the headset**
- Is SteamVR (or your OpenXR runtime) running and the headset tracking? Wait a few seconds after the game starts, or press **F9**.
- Open `b2svr_logs\b2svr.log` in the game folder:
  - `FAIL-CLOSED` means the mod could not find the game's core VR addresses in your build (see Supported game versions); the `SIGDB` lines above it name what is missing;
  - `xrCreateInstance failed` means no VR runtime answered. The mod keeps trying every 5 seconds.

**SteamVR restarted or crashed while playing**
The mod falls back to the monitor and keeps trying to reconnect every 5 seconds. If VR does not come back, press **F9** twice or restart the game.

**F1 / F3 do nothing**
Click the game window (or the SteamVR window) first: the keys only work while one of them is in front. F1 only acts while you control Jodie; it waits through loading screens, menus and Aiden sections.

**The game crashed**
- A crash report is saved in `b2svr_logs\crash_reports\crash_<date>.txt`, with the whole log of that run beside it (`crash_<date>_log.txt`).
- They hold your PC and headset details, the mod's version and state, where the crash happened and what the mod did before it. They contain no user name or personal files.
- Please send both, along with a few words on what you were doing (see **Something went wrong?** in README.md).

**The installer / launcher shows an error or closes**
- Everything it printed is in `setup_log.txt` in the `B2S_VR_MOD` folder (the run before: `setup_log.prev.txt`). Please send that file.
