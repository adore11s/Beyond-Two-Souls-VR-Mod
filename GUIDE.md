# Beyond: Two Souls VR Mod — Guide

Everything beyond the basics in the [README](README.md).

- [What the mod does](#what-the-mod-does)
- [Supported game versions](#supported-game-versions)
- [Controls](#controls)
- [VR settings panel](#vr-settings-panel)
- [Settings file (b2svr.ini)](#settings-file-b2svrini)
- [What the mod changes on your PC](#what-the-mod-changes-on-your-pc)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)

---

## What the mod does

- **Real stereo 3D.** Every frame is drawn twice, once per eye, with full head tracking (turning and leaning).
- **A square picture for each eye.** While VR runs the game draws a square picture (the size you pick in the VR settings, `render_size`) instead of its 16:9 one, whatever your monitor's resolution. Needs the game in **Windowed** mode; in its other display modes the mod leaves the game's own resolution.
- **The game's own cameras stay in charge.** In third person your head moves on top of the camera the game chose, so scenes keep their framing.
- **The game's interface** (prompts, subtitles, HUD) floats in front of you as a separate layer.
- **Menus, videos and loading screens** show on a big screen floating in front of you.
- **No frame cap and no VSync while VR runs.** The headset sets the pace (for example 90 frames per second on PlayStation VR2).
- **First person (F1).** While you control Jodie you see through her eyes. Her head and hair are drawn while you walk her (you see them in mirrors) and hidden in first-person cutscenes (`fp_head`; since 1.0.0 her hair too, which the game simulates like cloth), your VR controllers move her arms and hands (and whatever she holds), her body turns with you, and the left stick walks where you look. While she stands still her body holds still too, instead of swaying with the idle animation. Whether you walk her (F1) or the game animates her (F3) comes from the game itself (0.23.0): while the game plays one of its own full-body animations on her it is a cutscene, otherwise you walk her - also the walks right after some prompts, where the game takes away her free walking.
- **First person in cutscenes (F3).** Cutscenes are shown from Jodie's eyes too, facing where she looks.
- **Prompts you can see.** In first person, words and dialogue choices the game puts at Jodie's head (a name to call out, a reply) appear in front of you instead.
- **Steady lighting.** Lights near you stay on wherever you look, so characters do not flicker between lit and unlit as you turn your head.
- **Aiden** keeps the game's own first-person view: the mod never changes it.
- **The main menu** always shows the game's own view, even with first person on (F1 / F3); first person starts once you are in the game. Right hand at your head + B / ○ does nothing there.
- **Aiden's possessions in first person (off by default, not yet tested in the game).** Turn on **Aiden possessions (first person)** in the VR settings (or `fp_possession=on`): while Aiden possesses someone, first person (F1) follows that person instead of Jodie, while you walk them (body turned with you, arms on your controllers with motion controls) and while the game animates them (the view on their head). When the possession ends it goes back to Jodie.
- **Jodie's models.** The game has four sets of Jodie models (young, 14, 16 and adult). The mod knows which one a scene uses and applies the young-Jodie hand rules only to young Jodie; every other model gets the rules that scale with the size of her arms. When a scene holds more than one set (the Black Sun chapter), her arm length decides.
- **VR controllers work as a gamepad.** No extra drivers are needed.
- **VR settings in the headset.** In the game's pause menu a VR settings panel appears beside the screen (see [VR settings panel](#vr-settings-panel)).
- **Sound follows your head.** 3D sounds are heard from where your head is: in third person turning your head turns the sound with it, in first person they come from Jodie's head instead of the game's camera (`audio_head` in `b2svr.ini`).
- **Steady hands.** In first person the drawn hands are smoothed a little so they do not tremble; fast moves still follow at once, and the game always gets your raw controller movement.

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
| `F1` | First person when you move as Jodie, on / off |
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
| **All four face buttons** together (A B X Y / ✕ ○ □ △), then let go | Start (pause menu, with the VR settings): for controllers without a menu button on the right hand. While you hold them none of the four reaches the game |
| Left grip **+** menu button | Back |
| Left grip **+** left stick | D-pad |
| **Right hand at your head + B / ○** | First person on / off: while you walk as Jodie it switches F1, otherwise (a cutscene) F3, and while Aiden possesses someone the VR setting **Aiden possessions (first person)** (turning it on also turns F1 on). That press never reaches the game; B / ○ with the hand anywhere else is the game's B / ○ as usual |

A regular gamepad keeps working at the same time.

On Steam copies the controllers appear as a Steam Input gamepad. On copies without Steam (Epic Games Store) they appear as Xbox controller 1 instead, so the game shows Xbox button prompts there and `F11` does not change them.

### In first person (F1)

| Control | Does |
|---|---|
| Left stick | Walk where you are looking |
| Right stick left / right | Turn: smoothly (Turning speed in the VR settings), or in steps with snap turning (Snap angle). Not while a prompt that uses the right stick is on screen (white dots, and since 0.23.0 the stick / swipe prompts too), nor while the game animates her: then the stick only does the prompt |
| Your hands | Jodie's hands follow your controllers |

When the game switches to a filtered camera view (for example the security cameras in the DPA rooms when Jodie stands still), first person steps aside and shows the game's camera, then comes back by itself. F3 does the same while Jodie is not animated (standing in a T-pose between scenes).

---

## VR settings panel

Open the game's pause menu (the menu button, or all four face buttons) and the **VR SETTINGS** panel appears to the right of the screen. Point either controller at an option and pull its trigger; while a controller points at the panel its trigger belongs to the panel, not the game. Every choice is saved in `b2svr.ini` at once.

| Row | Choices | `b2svr.ini` |
|---|---|---|
| First person (walking) | On / Off (same as F1 or the head switch) | `firstperson` |
| First person (cutscenes) | On / Off (same as F3) | `fpcutscenes` |
| Aiden possessions (first person) | On: while Aiden possesses someone, first person (F1) follows that person instead of Jodie (walking: body turned with you, arms on your controllers with motion controls; animated by the game: the view on their head). **Off** (the default): first person stays with Jodie | `fp_possession` |
| Motion controls (first person) | On: your controllers move Jodie's arms. Off: her arms stay animated by the game | `fparms` |
| Hand smoothing (first person) | On: with motion controls, Jodie's hands do not tremble when you hold still (fast moves still follow at once). Off: raw hands. The game and the gestures always get your raw movement | `hand_smoothing` |
| Turning (first person) | Smooth / Snap | `turn` |
| Turning speed / Snap angle | 20 .. 100 % / 15 .. 90 degrees | `turn_speed` / `snap_turn_degrees` |
| Controller buttons | Xbox / PlayStation (same as F10 / F11) | `controller` |
| VR picture size (per eye) | 1440 .. 2816, then **Auto** (the size your headset asks for). The game resizes at once | `render_size` |
| Rendering method | **Native**, the default (both eyes every frame) / AFR (one eye per frame, alternating: lighter on the graphics card; the other eye shows its last picture, so fast motion can look less smooth) / Mono (one picture per frame from the middle of your head, shown to both eyes: the fastest, no eye-to-eye differences, but no 3D depth - the world looks far away; for slow graphics cards or a problem that only shows with two eyes) | `rendering_method` |
| VR on / off | Off = back to your monitor; F9 turns VR on again | — |

The panel shows with the game's pause menu, never over a video. (The main menu is a 3D scene, so it does not show there.)

---

## Settings file (b2svr.ini)

`b2svr.ini` sits next to the game's exe (`BeyondTwoSouls_Steam.exe` on Steam).
- Every setting has a short note explaining it.
- Changes made in the file take effect the next time the game starts; the VR settings panel changes them at once.
- The installer never overwrites your copy. Once (marked by `installer_defaults=3`) it switches first person while walking on and in cutscenes off and the rendering method to Native; after that your own choices stay.

| Setting | Default | Meaning |
|---|---|---|
| `xr` | on | off = start on the monitor and press F9 for VR |
| `fps` | 0 | Frame rate cap; 0 = none (recommended in VR) |
| `vsync` | off | Monitor VSync; always off while VR runs |
| `render_size` | 2048 | VR picture size: a square of this many pixels per side for each eye while VR runs (the VR settings: 1440 / 1728 / 2048 / 2304 / 2560 / 2816 / auto); auto = the size your headset asks for (the larger of its two eye sizes, up to a multiple of 8); off = the game's own resolution. Needs the game in Windowed mode |
| `desktop_window` | 720 | While the square VR picture runs, the game window on your monitor is this many pixels square (the headset still gets the full picture); 0 = the game's own window size |
| `turn` | smooth | Right-stick turning in first person (F1): smooth or snap |
| `turn_speed` | 1 | Smooth turning speed: 1 = 120 degrees a second (the VR settings: 0.2 .. 1) |
| `snap_turn_degrees` | 45 | Snap turning step in degrees (the VR settings: 15 / 30 / 45 / 60 / 90) |
| `firstperson` | on | First person while you walk as Jodie (F1; saved whenever you switch it) |
| `fpcutscenes` | off | First person in cutscenes (F3; saved whenever you switch it) |
| `fp_possession` | off | Aiden's possessions in first person: on = first person follows the person Aiden possesses (the VR settings: Aiden possessions) |
| `fparms` | on | Motion controls: off = Jodie's arms stay animated by the game in first person |
| `rendering_method` | native | native = both eyes every frame; afr = one eye per frame, alternating; mono = one picture from the middle of the head for both eyes (the VR settings: Rendering method) |
| `audio_head` | on | 3D sound heard from your head (off = from the game's camera) |
| `hand_smoothing` | on | Smooth Jodie's hands with motion controls in first person (off = raw; the VR settings: Hand smoothing); `hand_smooth_cutoff` 3 and `hand_smooth_beta` 6 tune it |
| `fp_eye_fwd` / `fp_eye_up` | 0.09 / 0.07 | First-person view position in metres, forward of / above the top of Jodie's neck |
| `fp_wrist_pitch` | 0 | Tilts Jodie's hands in first person (degrees; positive = fingers further down) |
| `wristpitch_psvr2` / `wristpitch_quest` / `wristpitch_other` | `fp_wrist_pitch` | The same tilt for one headset family only (chosen from the headset's name in the log line `FP wrist angle for ...`) |
| `fp_prompts_on_hand` | on | In first person, words and choices the game shows at Jodie's head appear in front of you (off = where the game puts them) |
| `fp_head` | cutscene | First person: Jodie's head, face, teeth and hair. cutscene = hidden in first-person cutscenes (F3), drawn in F1 (seen in mirrors); show = always drawn; hide = always hidden |
| `texture_refill` | on | Troubleshooting only: off = the fix for black furniture / beds / ground (0.21.7) is not used |
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

The installer also sets three of the game's own options for VR, in the game's options file (`Saved Games\Beyond Two Souls Steam\Save\Options\0`): display mode **Windowed**, **Motion blur** off and **Depth of field** off. Those two are only kept by the game's **Custom** graphics preset, so a Low / Medium / High preset becomes Custom with exactly that preset's other choices. The original values go into `settings_changed.txt` in the `B2S_VR_MOD` folder (and a copy of the file into `game_options_backup.bin`); the uninstaller puts each one back unless you changed it yourself since. If the file is missing or not in the form the installer knows, it changes nothing and lists the options to set by hand.

---

## Known limitations

- **First person still has drawing problems.** Parts of the picture can go missing or turn black for a moment (for example when you walk into another room), and characters can look badly lit: the game lights them for its own cameras. If an object stays black, `texture_refill=off` in `b2svr.ini` is a test switch for the fix of black furniture / beds / ground; please send the log either way.
- **The Party level:** the right eye no longer loses objects when you move your head, but the house you can walk into (as Jodie and as Aiden) is not checked yet. If objects go missing or the picture turns black there, press **F9** to play that part on your monitor and **F9** again afterwards to go back to VR.
- **Not yet checked in a headset:** many of the newest fixes (Jodie's first-person body from the game's own rig, held items and bags, rooms near doors, the horse ride's eye height, the submarine, her hair hidden in first-person cutscenes). If something still looks wrong, note the scene and send the log.
- **First person is still being tuned.** The view height, hand positions and wrist angles may be off in some scenes: adjust them with `fp_eye_fwd`, `fp_eye_up` and `fp_wrist_pitch` (or `wristpitch_<headset>`) in `b2svr.ini`. Press F1 / F3 (or B / ○ at your head) again to go back to the game's cameras at any time.
- **Cinematic framing.** Some cutscenes keep the game's widescreen framing.
- **Performance.** Native draws the game twice per frame, AFR and Mono once; if the headset stutters, pick **Mono** (or AFR) or a smaller VR picture size in the VR settings, or lower the game's graphics settings.
- **The game window on your monitor is small** while VR runs (`desktop_window`, 720 x 720 by default): the headset gets the full square picture, the monitor only a preview.

---

## Troubleshooting

**The game runs flat, not in the headset**
- Is SteamVR (or your OpenXR runtime) running and the headset tracking? Wait a few seconds after the game starts, or press **F9**.
- Open `b2svr_logs\b2svr.log` in the game folder:
  - `FAIL-CLOSED` means the mod could not find the game's core VR addresses in your build (see Supported game versions); the `SIGDB` lines above it name what is missing;
  - `xrCreateInstance failed` means no VR runtime answered. The mod keeps trying every 5 seconds.

**The headset picture is blurry or the game renders 16:9**
In the game's graphics settings, set the display mode to **Windowed**. In its other modes the game sizes its picture from your desktop, so the mod keeps the game's own resolution (`RENDERSIZE ... needs the game in Windowed mode` in `b2svr_logs\b2svr.log`).

**SteamVR restarted or crashed while playing**
The mod falls back to the monitor and keeps trying to reconnect every 5 seconds. If VR does not come back, press **F9** twice or restart the game.

**F1 / F3 do nothing**
Click the game window (or the SteamVR window) first: the keys only work while one of them is in front. F1 only acts while you control Jodie; it waits through loading screens, menus and Aiden sections. With the controllers, hold your right hand at the side of your head and press B / ○.

**I can't find the VR settings**
Open the game's pause menu (the menu button, or all four face buttons together and let go): the panel is to the right of the screen. Turn your head to the right if it is outside your view.

**The game crashed**
- A crash report is saved in `b2svr_logs\crash_reports\crash_<date>.txt`: **one file** with everything in it - the report, the mod's whole log of that run and a small memory dump at the end (it lets the developer see exactly where every part of the game was when it crashed).
- It holds your PC and headset details, the mod's version and state, the level you were in and your story decisions (what the game holds in its story variables, plus the ones the chapter asked for and did not find), where the crash happened and what the mod did before it. It contains no user name or personal files (Windows filters the memory dump: no folders, no personal memory).
- Please send that one file, along with a few words on what you were doing (see **Something went wrong?** in README.md).

**The installer shows an error or closes**
- Take a screenshot of the installer window before pressing Enter and send it.
- `It is running` messages: close Beyond: Two Souls first (the game writes its options when it quits).
