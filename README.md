<p align="center">
  <img src="docs/images/banner.png" alt="Ignition Vita" width="640">
</p>

<h1 align="center">Ignition Vita</h1>

<p align="center">
  <b>A native-feeling port of the 1997 arcade racer <i>Ignition</i> to the PlayStation Vita.</b><br>
  Version <b>v1.0</b> &nbsp;·&nbsp; Platform: <b>PlayStation Vita / PlayStation TV</b> (homebrew, unsafe mode) &nbsp;·&nbsp; Port by <b>Zeusthenen (Zeus)</b>
</p>

<p align="center">
  <img alt="Version v1.0" src="https://img.shields.io/badge/version-v1.0-blue">
  <img alt="Platform PS Vita" src="https://img.shields.io/badge/platform-PS%20Vita-informational">
  <img alt="Tested on a real PS Vita; PS TV not tested" src="https://img.shields.io/badge/tested%20on-PS%20Vita%20only-lightgrey">
  <br>
  <a href="https://github.com/Zeusthenen/Ignition-Vita/releases">Releases</a> &nbsp;·&nbsp;
  <a href="https://github.com/Zeusthenen/Ignition-Vita/issues">Issues</a> &nbsp;·&nbsp;
  <a href="docs/INSTALLATION.md">Installation</a> &nbsp;·&nbsp;
  <a href="docs/FAQ.md">FAQ</a>
</p>

<p align="center">
  <img src="docs/images/livearea.jpg" alt="Ignition Vita in the PS Vita LiveArea (real console photo)" width="420"><br>
  <sub>The LiveArea of the finished port, photographed on a real PS Vita.</sub>
</p>

> **Legal notice.** Ignition Vita is an unofficial, non-commercial fan port. It is not affiliated with or endorsed by Interplay, UDS, or any current rights holder. Ignition, its name, logo and original game assets belong to their respective owners. Original game files are not included and must be supplied legally by the user. This notice does not grant any right over the logo or the original resources.
>
> **Aviso legal.** Ignition Vita es un port de aficionados, no oficial y sin fines comerciales. No está afiliado ni respaldado por Interplay, UDS ni por ningún titular actual de los derechos. Ignition, su nombre, logotipo y los recursos originales del juego pertenecen a sus respectivos propietarios. Los archivos originales del juego no se incluyen y deben ser aportados legalmente por el usuario. Este aviso no constituye una concesión de derechos sobre el logotipo ni sobre los recursos originales.
>
> **This repository and the released VPK do not contain any game data**: no executable, resources, music, videos, tracks, models or other files of the commercial game. You must provide your own files from your own legal copy of *Ignition* (see [Original data required](#7-original-data-required)). The project is not affiliated with or endorsed by Sony either.

[Leer en español → README_ES.md](README_ES.md)

---

## Contents

1. [About the project](#1-about-the-project)
2. [Features](#2-features)
3. [Current status](#3-current-status)
4. [Performance](#4-performance)
5. [Requirements](#5-requirements)
6. [Installation](#6-installation)
7. [Original data required](#7-original-data-required)
8. [Controls](#8-controls)
9. [Configuration](#9-configuration)
10. [Saves and logs](#10-saves-and-logs)
11. [Disabled features / known limitations](#11-disabled-features--known-limitations)
12. [Building from source](#12-building-from-source)
13. [Troubleshooting](#13-troubleshooting)
14. [Reporting bugs](#14-reporting-bugs)
15. [Credits](#15-credits)
16. [Licenses](#16-licenses)
17. [Screenshots](#17-screenshots)
18. [Version history](#18-version-history)

---

## 1. About the project

*Ignition* (1997) is a PC arcade racing game. Ignition Vita lets you play the **original Windows game** on a PlayStation Vita.

It is **not** a rewrite or a remake. The port loads the original `Ign_3dfx.exe` (the 3dfx/Glide version of the game), runs its x86 code on the Vita through a dynamic recompiler (x86 → ARMv7), and provides, in native Vita code, everything the executable expects from Windows: files, timers, DirectInput, DirectSound, the Windows message loop, MCI CD audio and the Glide 3D API (translated to OpenGL calls on the Vita's GPU with [vitaGL](https://github.com/Rinnegatamante/vitaGL)).

Because of that, the port needs the game's own data files, which are **not** included. How it all fits together is described in [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md) and the story of how it was made in [docs/PORT_HISTORY.md](docs/PORT_HISTORY.md).

## 2. Features

- Runs the original game executable and data: campaign and single-player modes, tracks, cars, menus and options as in the PC game.
- 3D rendering through vitaGL; the 2D HUD, menus and effects of the original software renderer are composited correctly.
- **Corrected HUD, transparencies and smoke** (chroma-key handling of the original textures).
- **Complete audio**: music (the CD audio tracks, played from your own Ogg files), engines, turbo, skids, crashes and all sound effects.
- **Vita controls**: D-pad / left stick, face buttons and shoulder buttons are mapped to the keyboard commands the game expects (see [Controls](#8-controls)).
- **Working language selector**, with the chosen language remembered between runs.
- **Fixed car-selection screen.**
- **Official Vita keyboard** to change the three-character player name (opened from the name option of the player options menu).
- **Game data and options are saved** by the game's own save routine in `ux0:data/ignition/`.
- **Clean exit** back to the LiveArea ("Quit" in the game's menu).
- **Custom LiveArea**: bubble icon, background and start-up image.
- **30 FPS limiter** (configurable) for even frame pacing.
- **Automatic clock profile**: the port asks the system for at least ARM 444 MHz / BUS 222 MHz / GPU 222 MHz / GPU Xbar 166 MHz at start-up (see [Performance](#4-performance)).

## 3. Current status

**v1.0 — first public release.** The complete game loop (menus → car selection → race → results → menus → quit) has been played on real PS Vita hardware: normal races, Championship races and the Yodel Peaks track, with sound, HUD, smoke, language selector, name entry, saving and clean exit.

Two menu entries are intentionally inactive: [split screen and online multiplayer](#11-disabled-features--known-limitations).

## 4. Performance

In races you can usually expect **about 25–30 frames per second**. The port includes a limiter that presents at most 30 frames per second, evenly spaced; that is a cap, not a promise.

Honest expectations:

- Performance **varies with the track, the game mode, the number of vehicles and the scene, and with the clock frequencies in use**. It is fluid in most situations, but it is **not a locked 30 FPS and it is not constant**: busy scenes run below 30 (in the author's final test on a real PS Vita, race scenes averaged roughly 25–27 FPS; lighter scenes can reach the 30 FPS cap).
- The game's logic is driven by real time, so a lower frame rate makes the picture less smooth but does **not** make cars, timers or lap times slower.
- By default the port requests these clock frequencies at start-up (details below).
- If you already use **500 MHz through an external plugin**, the port respects that setting and does not lower it. Using such a plugin is **optional, entirely your decision and your responsibility** (higher clocks mean more heat and battery use); the port never asks for 500 MHz by itself.

**Clock profile** (a *minimum*, applied once at start-up, per component only if the current value is lower):

| Component | Frequency requested |
|---|---|
| ARM / CPU | 444 MHz |
| BUS | 222 MHz |
| GPU | 222 MHz |
| GPU Xbar | 166 MHz |

- It is applied when the game starts, before the graphics are initialised.
- It never lowers a higher frequency that something else (for example a plugin at 500 MHz) has already set.
- On exit, the port restores only the frequencies it changed itself, and only if nobody changed them afterwards.
- An external plugin can still change the clocks after start-up; the port does not fight it (it never re-applies the profile).
- The result is written to the log (`[G160-CLOCKS]` at start, `[G160-CLOCKS-RESTORE]` at exit).

## 5. Requirements

- A PlayStation Vita (or PlayStation TV) with custom firmware allowing homebrew, with **unsafe homebrew enabled** (the port needs executable memory for its recompiler, so it is installed as an *unsafe* application).
- About **110 MB** of free space for the game data (about 37 MB of data plus about 70 MB for the music if you provide it; the music size depends on how you encode it) and a couple of MB for the VPK.
- A VPK installer such as VitaShell.
- Your **own legal copy of Ignition** to take the data from (see [below](#7-original-data-required)).

> **Not verified:** whether `libshacccg.suprx` (the Vita shader compiler module) is required. The port uses vitaGL's fixed-function pipeline and compiles no shaders of its own, but it is linked with vitaGL/vitaShaRK, which can use that module, and the game has not been run on a console that lacks it. If the game does not start and you do not have it installed, install it as described by the vitaGL documentation and report the result.

**PlayStation TV: not tested.** The port was tested on a real PS Vita only. The code reads the standard controller input, so it may work on a PS TV with a paired controller, but that is unconfirmed.

## 6. Installation

Full step-by-step guide: [docs/INSTALLATION.md](docs/INSTALLATION.md). Short version:

1. Install `IgnitionVita_v1.0.vpk` with VitaShell (the bubble *Ignition Vita* appears in the LiveArea).
2. Copy your own *Ignition* data to `ux0:data/ignition/` ([exact list](#7-original-data-required)).
3. Start the game. The first run creates `ux0:data/ignition/logs/`; check `ignitionvita.log` (see [docs/INSTALLATION.md](docs/INSTALLATION.md#5-first-run-and-checking-the-logs)).

Updating to a future version: install the new VPK over the old one; **do not delete `ux0:data/ignition/`** (your game data and saved options live there).

## 7. Original data required

The port needs files from a legal copy of *Ignition* for PC (Windows). Nothing from the game is distributed here and none will be provided or linked.

Copy these items, keeping the same names and folder structure, into **`ux0:data/ignition/`**:

| Item | Type |
|---|---|
| `Ign_3dfx.exe` | the game executable (the 3dfx/Glide version; 626,688 bytes, SHA-256 `9C40E7D2711C58A8EE11C8A16125BB5FE90179FFA693432D0E45793C9FC7B79B`) |
| `Baltazar\`, `CARS\`, `fonts\`, `GENERAL\`, `Ghosts\`, `LEVELS\` | the game's data folders (whole folders) |
| `MenuBkg.dat`, `sys.col` | data files |
| `H_pan1.pic`, `H_pan2.pic`, `H_signs.pic`, `N_pangfx.pic`, `N_Signs.pic`, `N_sysgfx.pic`, `N_sysg_2.pic`, `Pokal.pic`, `S_pangfx.pic`, `S_Signs.pic` | the HUD / panel picture files |
| `Music\Track02.ogg` … `Music\Track08.ogg` | **optional** but needed for music: the CD audio tracks 2–8 of your own copy, converted by you to Ogg Vorbis |

Not needed: the original `*.dll` files, `Ign_d3d.exe`, installers, manuals and the 3dfx / DirectDraw wrapper tools: the port provides its own replacements.

The executable listed above is the one the port was written and tested against; other builds of the game are not supported.

Details, including how to prepare the music, are in [docs/INSTALLATION.md](docs/INSTALLATION.md#2-getting-the-original-game-data-legally).

## 8. Controls

| Vita | Action |
|---|---|
| D-pad or left stick | Move / navigate menus / steer and accelerate-brake in the race (arrow keys of the PC game) |
| ✕ Cross | Confirm / "Yes" (Enter; in Yes/No prompts it sends the *yes* key of the game's current language) |
| ○ Circle or Start | Back / cancel in menus; pause and quit prompt in a race (Esc) |
| △ Triangle | "No" in Yes/No prompts; shift up in a race (`/` key) |
| □ Square | Shift down in a race (`.` key) |
| L | Rear view (`'` key) |
| R | Turbo / boost (Right Shift) |
| Select (tap) | Change view after a race (Tab, sent when you release the button) |
| Select held + □ | Toggle the race results (`` ` `` key) |

The Yes/No behaviour of ✕ and △ only applies while the game's own Yes/No question is on screen during a race (for example the quit-race prompt); everywhere else ✕ is Enter. Full table and notes (from the port's input code): [docs/CONTROLS.md](docs/CONTROLS.md). To change the player name: select the *name* option (first item of the player options menu) and press ✕; the official Vita keyboard opens (up to 3 characters).

## 9. Configuration

An optional text file, **`ux0:data/ignition/g146_geo.txt`**, holds settings. Put one setting per word, on any line:

| Setting | Meaning |
|---|---|
| `fps-limit=30` | default: present at most 30 frames per second, evenly spaced |
| `fps-limit=0` or `fps-limit=off` | unlimited, for comparison and diagnosis |

Other values between 10 and 60 are accepted; anything else is rejected and the default stays (the log says so). With no file, the default (30) is used. More technical switches are listed in [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md#10-control-file-tokens).

## 10. Saves and logs

- **Saved data**: the game writes its own configuration and progress (language, player names, options) to `ux0:data/ignition/ign_3dfx.btz`, exactly as the PC version does when you leave the game with its *Quit* option. Quit from the game's menu (not with the PS button) to be sure it is written.
- **Logs**: `ux0:data/ignition/logs/` contains `boot.txt`, `ignitionvita.log`, `ignition_critical.log` and `ignition_session_id.txt` (the previous run's files are kept with a `.previous` suffix). They are small and contain no personal data.

## 11. Disabled features / known limitations

- **Split screen**: visible in the *Multi player* menu but **intentionally disabled**, because it is not usable with a single Vita. Selecting it does nothing.
- **Online / network multiplayer**: visible but **disabled in v1.0**. Selecting it does nothing.

Both entries stay in the menu for fidelity to the original, they do not run any action and they do not close the game. They are limitations of this release, not errors.

Other things to know: the performance caveats of [Performance](#4-performance); music requires the Ogg files you provide; only the supported executable of the game is guaranteed to work; PlayStation TV and the need for `libshacccg.suprx` are [not verified](#5-requirements).

## 12. Building from source

All the source needed is in this repository (including the vendored engine under `external/WinVita`). The build uses MSYS2 (or Linux) with VitaSDK. Complete instructions: [docs/BUILDING.md](docs/BUILDING.md).

```bash
export VITASDK=/usr/local/vitasdk
./scripts/build.sh          # builds the engine library, the executable and build/IgnitionVita_v1.0.vpk
python3 scripts/check_vpk.py build/IgnitionVita_v1.0.vpk
```

A source build is functionally equivalent to the released VPK but not byte-identical to it (the build embeds its compile date).

The program links vitaGL, vitaShaRK and other libraries statically. Which versions, their source, the license terms of the VPK and how to relink the program with a modified library (`scripts/relink.sh`, no recompilation needed): [docs/LGPL_COMPLIANCE.md](docs/LGPL_COMPLIANCE.md).

## 13. Troubleshooting

See [docs/FAQ.md](docs/FAQ.md) for the common questions and [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md): installation errors (including `0x8010113D`), black screen, missing music, LiveArea not showing the background, clocks, logs and how to start from a clean state.

## 14. Reporting bugs

Open an [issue](https://github.com/Zeusthenen/Ignition-Vita/issues/new/choose) using the *Bug report* template (`.github/ISSUE_TEMPLATE/bug_report.yml`) and attach `boot.txt`, `ignitionvita.log` and `ignition_critical.log` from `ux0:data/ignition/logs/`. Please include the port version, your Vita model, firmware, plugins, the clock frequencies, game mode / track / car, steps to reproduce, and a photo or video for visual problems. Reports without enough information may need clarification. Do not attach game files.

Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).

## 15. Credits

- **Port by Zeusthenen (Zeus).**
- **WinVita / winx86** by Franck Roustan (MIT): the generic x86 → ARMv7 execution engine and Win32 runtime on which the port is built, derived from the D2Vita project and from **Box86** by Sebastien "ptitSeb" Chevalier (MIT).
- **OpenGLide** (LGPL-2.1; originally by Fabio Barros, modified by Paul for Glidos, Linux version by Simon White): parts of the Glide layer of the port (`source/win32/glide2x_ign.*`) are taken or adapted from it, and those two files are therefore licensed LGPL-2.1-only.
- **NFSIISE** by Błażej Szczygieł (MIT): reference for the Glide 2.x wrapper and a table taken from it.
- **vitaGL**, **vitaShaRK** (LGPL-3.0-or-later) and **SceShaccCgExt** (GPL-3.0, by bythos14): graphics, shader and compiler-extension libraries linked into the program; **taiHEN** by Yifan Lu (MIT) and **math-neon** by Lachlan Tychsen-Smith (MIT).
- **VitaSDK** and its contributors.
- **stb_vorbis** by Sean Barrett (MIT / public domain): Ogg Vorbis decoding.
- The people who documented the Vita's homebrew scene, and everyone who tested.

Full details, copyright lines and licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

> Development and documentation assistance: Anthropic Claude and OpenAI ChatGPT.

## 16. Licenses

This repository is **not** under a single license. In short (the authoritative texts are [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); copies of the license texts are in [licenses/](licenses/)):

| What | License |
|---|---|
| Original code and documentation of Ignition Vita (Copyright (c) 2026 Zeusthenen (Zeus)) | **MIT** ([LICENSE](LICENSE)) |
| `source/win32/glide2x_ign.cpp` and `glide2x_ign.h` (contain material from OpenGLide) | **LGPL-2.1-only** ([licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt)) — an exception to the MIT license |
| `external/WinVita/` (Franck Roustan; includes Box86 by ptitSeb) | MIT |
| `third_party/stb_vorbis/` | MIT or public domain |
| vitaGL and vitaShaRK (linked statically into the executable, not in this repository) | keep their own **LGPL** licenses (LGPL-3.0-or-later according to their file headers) |
| SceShaccCgExt (linked statically, pulled in by vitaShaRK; not in this repository) | **GPL-3.0**, according to its original license |
| math-neon, taiHEN stubs (MIT); libpthread of the VitaSDK (LGPL-2.1-or-later); other VitaSDK runtime libraries | their own licenses |
| **The released VPK (the combined binary)** | distributed under the applicable terms of the **GPL-3.0**, because SceShaccCgExt (GPL-3.0) is linked into it. Corresponding source and relinking: [docs/LGPL_COMPLIANCE.md](docs/LGPL_COMPLIANCE.md) |
| The game *Ignition*, its name, logo and data | **not covered, not included**; they belong to their owners |

Anyone can rebuild the program from this repository and relink it with modified versions of the LGPL libraries: [docs/LGPL_COMPLIANCE.md](docs/LGPL_COMPLIANCE.md).

## 17. Screenshots

| | |
|---|---|
| ![LiveArea on a real Vita](docs/images/livearea.jpg) | ![LiveArea background](docs/images/banner.png) |
| LiveArea of the port on a PS Vita | LiveArea background used by the bubble |
| ![Bubble icon](docs/images/icon.png) | ![Start-up image](docs/images/startup.png) |
| Bubble icon | Start-up image of the "Start" button |

> In-game screenshots are intentionally not included yet.

## 18. Version history

See [CHANGELOG.md](CHANGELOG.md). The technical story of the port is in [docs/PORT_HISTORY.md](docs/PORT_HISTORY.md).

| Version | Date | Notes |
|---|---|---|
| v1.0 | 2026-10 | First public release (see [Releases](https://github.com/Zeusthenen/Ignition-Vita/releases)) |
