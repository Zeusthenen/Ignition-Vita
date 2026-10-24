# Changelog

All notable changes to Ignition Vita. The format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.1] - 2026-10-24

Second public release. It fixes the crash at the end of a championship, repairs progression, unlocks and saving, improves the default controls and makes the controls configurable from the game's own menu. The game data and the music are still not included.

### Fixed
- **Crash at the end of a championship.** The game no longer closes on the final screen after the last race of a championship.
- **Progression after completing a championship** works again: the game continues normally after the championship.
- **Unlocks and persistence** of vehicles, difficulties and tracks: unlocked content is now stored and kept. Verified on a real PS Vita: the **Banana** car, the **Amateur** difficulty and one additional track.
- **Quick race and Chase** can be finished and return to the menu.
- **Save protection:** `ign_3dfx.btz` is written to a temporary file (`ign_3dfx.btz.tmp`) and replaces the real file only when the whole save has been written; if a write fails, the previous save is kept. This reduces the risk of a corrupt or incomplete save.
- Several stability fixes.

### Added
- **Configurable controls.** Any of the 12 Vita buttons (✕, ○, □, △, L, R, Start, Select and the four D-pad directions) can be assigned to any of the eight race actions from the game's own screen *Options → Player 1 → Keyboard config*. The screen shows the names `PAD X`, `PAD O`, `PAD SQ`, `PAD TRI`, `PAD L`, `PAD R`, `START`, `SELECT`, `PAD UP`, `PAD DN`, `PAD LT`, `PAD RT`. The assignments work in races, are stored with the game's other preferences and persist after restarting. The game's own duplicate detection is kept (a button already used by another action is refused).
- Documentation of the controls, including the technical Vita → virtual key table ([docs/CONTROLS.md](docs/CONTROLS.md), [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md)), and a more detailed bug-report template.

### Changed
- **Default race controls:** ✕ = accelerate, □ = brake / reverse.
- D-pad ↑ ↓ and the vertical axis of the left stick **no longer act during driving**, so that they cannot interfere with accelerating and braking. They keep navigating the menus; steering with D-pad / stick ← → is unchanged.
- □ no longer shifts down in a race (it is the brake); *GEAR DOWN* can be assigned to any button.
- Menus, Yes/No prompts, pause and results keep their previous controls.
- The build identifies itself in the log (`[G163-VERSION] build=Ignition Vita v1.1 ...`); the package version is 01.10.

### Known limitations
- Verified on a real PS Vita: the first championship from start to finish, Banana, Amateur and one additional track. Later championships use the same general progression path, but their rewards have **not** been tested individually from start to finish; please report any reproducible problem.
- Controls: the left stick cannot be assigned; if *TURN LEFT / RIGHT* are moved to buttons, the D-pad and the stick no longer steer; the original keyboard keys cannot be restored from the Vita; the key screen has no cancel or restore-defaults button; ○ and Start cannot both be assigned.
- **Split screen** and **online multiplayer** remain visible but disabled; this version adds no online features.
- The game data and music are not included and must come from the user's own legal copy; only `Ign_3dfx.exe` (626,688 bytes) is supported.
- Performance varies with track, mode, number of vehicles and clock frequency; the target is 30 FPS but drops can occur in demanding areas.
- Tested on a real PS Vita only; **PlayStation TV is not tested**, and whether `libshacccg.suprx` is required is not verified.
- More issues may appear as more users and scenarios are tested.

## [1.0] - 2026-10 — Initial release

First public release of Ignition Vita, a port of the 1997 racing game *Ignition* to the PlayStation Vita that runs the original game executable and data.

### Gameplay
- Single-player modes, normal races and the Championship are playable from the menus to the results screen and back; the Yodel Peaks track and the other tracks load and run.
- Car selection works (animated carousel), the HUD, transparencies and smoke render correctly.
- Language selector works and the chosen language is remembered between runs.
- Game options, player names and progress are saved by the game's own save routine in `ux0:data/ignition/`.
- Clean exit to the LiveArea from the game's *Quit* option.

### Audio
- Complete sound: engines, turbo, skids, crashes and all effects.
- Music through a virtual CD: the user supplies the CD audio tracks 2–8 as `Music/Track02.ogg` … `Track08.ogg`.

### Controls and interface
- Vita controls: D-pad / left stick, ✕ confirm, ○ / Start back and pause, △ and □ gear shifts (and "No" in Yes/No questions), L rear view, R turbo, Select for the view and results keys.
- Player name (three characters) entered with the official Vita keyboard.
- Custom LiveArea: bubble icon, background and start-up image.

### Performance
- Optimised rendering path (deferred and batched GL state, rejection of off-screen geometry, incremental compositor, native replacements of two hot pixel loops, display-list cull, keyed-texture caching).
- 30 FPS limiter with even frame pacing, enabled by default; `fps-limit=0` (or `off`) in `ux0:data/ignition/g146_geo.txt` unlocks it for comparison.
- Default clock profile requested at start-up: ARM 444 MHz, BUS 222 MHz, GPU 222 MHz, GPU Xbar 166 MHz, as a minimum: higher clocks set by external plugins are kept, and only the frequencies the port changed are restored at exit.
- Performance varies with track, number of vehicles and scene; 30 FPS is the target, not a guarantee.

### Licensing and documentation
- Original code under the MIT license; `source/win32/glide2x_ign.cpp` and `glide2x_ign.h` under LGPL-2.1-only (they contain material from OpenGLide); third-party components keep their licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
- The statically linked libraries, the license terms of the VPK and the relinking procedure (`scripts/relink.sh`) are documented in [docs/LGPL_COMPLIANCE.md](docs/LGPL_COMPLIANCE.md).
- Documentation in English and Spanish (README), installation, controls, building, troubleshooting, FAQ, technical notes and port history.

### Known limitations
- **Split screen** is visible in the menu but disabled (not usable with a single Vita).
- **Online multiplayer** is visible in the menu but disabled in this version.
- The game data and music are not included and must come from the user's own legal copy; only the 3dfx executable `Ign_3dfx.exe` (626,688 bytes) is supported.
- Tested on a real PS Vita only; **PlayStation TV is not tested**, and whether `libshacccg.suprx` is required is not verified.
- Frame rate is typically about 25–30 FPS in races and not constant; the 30 FPS limiter is a cap, not a guarantee.