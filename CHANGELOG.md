# Changelog

All notable changes to Ignition Vita. The format follows [Keep a Changelog](https://keepachangelog.com/).

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