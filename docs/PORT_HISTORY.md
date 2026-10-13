# Port history

A readable summary of how Ignition Vita was made, from the first line of code to v1.0. It leaves out the hundreds of small internal iterations; each paragraph is a milestone. (Internal iteration labels such as "G138" still appear in log tags and source comments; they are the numbering of the private development steps.)

The method never changed: **analyse → implement one small step → build a VPK → test on a real Vita → read the log → decide the next step.** Nothing was assumed to work until a real console log showed it.

## 1. First boot: loading a Windows executable

The project began as a diagnostic: load the original PE32 executable on the Vita through the WinVita engine, wire up enough of Win32 to reach the entry point, and log exactly where execution stopped (every unimplemented import became a clean, logged stop instead of a crash). Each stop told what to implement next: kernel32 file access mapped to `ux0:data/ignition`, timers, window messages, DirectInput and DirectSound entry points. The first target was the DirectDraw build of the game; it was soon abandoned for the 3dfx/Glide build (`Ign_3dfx.exe`), whose rendering is a thin, well-understood API.

## 2. First images: Glide on vitaGL

The 34 Glide functions imported by the game were implemented over vitaGL one at a time, each confirmed by the next log: window open, buffer clear and swap, texture memory, colour combine, alpha blend, depth and clipping, and finally triangle submission. A vitaGL self-test proved the context first. The first recognisable frames came from the language selector and the intro; the first race track followed once vertex layouts, colour/alpha compatibility and a four-vertex quad reconstruction used by the track were understood.

## 3. The compositor: the game's software-drawn screens

Most of the game's 2D (menus, HUD, panels, sprites) is drawn by the game's own software renderer into the Glide linear frame buffer. Turning that buffer into correct pictures took many iterations: uploading it as a texture, only when it changes, only the changed regions, with the right alpha, in the right order relative to the 3D scene, keeping it correct across pause and menu transitions. A persistent overlay layer, tile alpha and a stable-pixel mask were the final form.

## 4. HUD and in-race overlays

The race HUD needed its own lifecycle (when to show, clear and refresh it) and region tracking so that stale pixels never stay on screen and the HUD does not shimmer between frames. The game's pause and race states are detected from its own variables, never from pictures.

## 5. Controls

The Vita pad is turned into DirectInput keyboard events with a small deterministic mapper (see `docs/CONTROLS.md`). Finding how the game reads confirmation, cancel and Yes/No questions (the Yes key depends on the language) was part of this, as was making the quit prompt and the exit from the menu clean.

## 6. Language selector and persistence

The language selector initially misrendered and kept the wrong language after the first screen. It was fixed in three steps (rendering, the exit from the selector, the flag region) and the chosen language is remembered across runs and sessions through the game's own configuration.

## 7. Car selection

The rotating-car carousel of the car-selection screen was invisible or wrongly coloured at first. Replaying the carousel draws, fixing flat colours and the raster stage, and re-scoping the "true model" regions produced the correct, animated car selection.

## 8. Audio

Sound effects came from emulating DirectSound and playing the game's own mix through the Vita audio output, which needed timing fixes (starved threads, priorities, buffer cursors) and a reconciliation of audio and visuals. Music was the last piece: the original plays CD audio, so a **virtual CD** was built, mapping MCI commands to Ogg Vorbis tracks supplied by the user (decoded with stb_vorbis), with an exact table of contents so that the game's track numbering and timing logic work unchanged.

## 9. Smoke and transparency

Smoke and some effects use a **chroma-key** colour in textures. Handling it in 3D required alpha-tested keyed texture variants, built only when needed and cached, with the correct texture environment and later with memoised key resolution to cost almost nothing per draw.

## 10. Stability

Real-console runs of minutes and several races exposed rare problems (ring buffers, GL state mismatches, scans, budget exhaustion of the recompiler). Defensive checks, counters in the logs and a "resume after budget" loop were added until a full session produced zero errors of each kind. The logging system was rebuilt to be crash-safe, rotating and cheap (it had been costing frames).

## 11. Fast paths and optimisation

Performance work was measured, not guessed: a per-frame profile (guest vs host time) showed that the recompiled game logic dominates. The port added a deferred, batched GL state path, trivial rejection of off-screen triangles, an incremental vectorised compositor, native replacements of two hot pixel loops of the game, a display-list cull and keyed-texture memoisation. Together they took heavy races from roughly 19 FPS to the mid-to-high twenties, and lighter scenes to about 30 FPS or slightly above before the limiter.

## 12. Multiplayer decisions

Split screen is not usable with one console and online play needs a network stack the port does not provide (the original calls into DirectPlay and a dialog). Both entries were left visible but made inert by a structural hook in the menu code, so they cannot end the game.

## 13. Player name with the Vita keyboard

The game's own name entry needs a PC keyboard. The port opens the official Vita keyboard from the name item and hands the answer to the game's original commit routine; the structure of the menu node, the name buffer (3 characters + terminator) and the character table were established by analysis and checked by running the game's real menu code on a PC.

## 14. Clock profile and 30 FPS limiter

The port now requests, once at start-up, at least ARM 444 / BUS 222 / GPU 222 / XBAR 166 MHz without fighting an external 500 MHz plugin, and restores only what it changed. A presentation limiter paces frames to 30 per second with a single sleep per frame and no catch-up. Because the game is wall-clock driven, limiting presentation does not change game speed.

## 15. LiveArea and release

Custom LiveArea art (bubble icon, background, start-up image) was added. The first package was refused by VitaShell (`0x8010113D`) because the images were 24-bit; indexed 8-bit PNGs fixed the installation, and a corrected template (revision 2) made the background and start-up image appear. The physical test of the final package passed, and it became **v1.0**.
