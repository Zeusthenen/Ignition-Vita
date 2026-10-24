# Troubleshooting

Always start by looking at the logs in `ux0:data/ignition/logs/` (see the last section). Most problems are explained in the first lines of `ignitionvita.log` or in `boot.txt`.

## Installation errors

**VitaShell refuses the VPK with `0x8010113D`.**
This error was caused, during development, by LiveArea images stored as 24-bit RGB PNG files. The Vita requires the LiveArea PNG files (`icon0.png`, `bg0.png`, `startup.png`) to be **8-bit indexed (palette) PNGs**. The released VPK already contains correct files. If you build your own VPK with your own artwork, convert the images to 8-bit indexed PNG (PNG colour type 3), keep the exact sizes (128×128, 840×500, 280×158) and check them with `python3 scripts/check_vpk.py` (see [BUILDING.md](BUILDING.md)).

**The VPK installs but the game does not start / the bubble does nothing.**
Make sure **unsafe homebrew** is enabled in your custom firmware settings; the port needs executable memory for its recompiler and is installed as an unsafe application.

**Not enough memory / the game closes immediately at start.**
Close other applications (including suspended ones) and try again. Check `boot.txt`: it shows the memory blocks the port could allocate.

## The LiveArea does not show the custom background / start-up image

- The icon is read from the package, but the LiveArea page (background and start-up image) is read from the LiveArea database. If you installed an older build first, delete the bubble and install the VPK again (your data in `ux0:data/ignition/` is **not** touched by this). If it still shows the generic page, rebuild the database from the recovery menu.
- Check that your VPK contains `sce_sys/livearea/contents/template.xml`, `bg0.png` and `startup.png` (`scripts/check_vpk.py`).

## The game does not start properly

| Symptom | Possible cause |
|---|---|
| Returns to the LiveArea at once, `boot.txt` mentions the executable | `Ign_3dfx.exe` is missing or not directly inside `ux0:data/ignition/` |
| Black screen, then back to the LiveArea | Missing data folders or files (see the list in [INSTALLATION.md](INSTALLATION.md)); look in `ignitionvita.log` for the file that could not be opened |
| The game starts but misbehaves | A different build of the executable than the supported one (SHA-256 in the installation guide); other builds are not supported |
| The log mentions a missing shader compiler | Install `libshacccg.suprx` as described in the vitaGL documentation (whether the port needs it has not been verified on a console without it) |

## Music

- No music: the files `Music/Track02.ogg` … `Music/Track08.ogg` are missing, misnamed or not Ogg Vorbis. The port does not play the original CD; you must provide the tracks (see [INSTALLATION.md](INSTALLATION.md#the-music-optional-but-strongly-recommended)).
- Music of the wrong track: the file numbers must be the CD track numbers (track 2 is `Track02.ogg`).
- Sound effects are not affected by the music files.

## Performance

- The target is 30 FPS, and it is **not guaranteed in every scene** (see the README). Heavy tracks with many vehicles can run below 30 FPS.
- Check the clock line in the log (`[G160-CLOCKS]`). With no plugin it should show `after{arm=444 bus=222 gpu=222 xbar=166}`. If a plugin lowers the clocks after start-up, performance drops; the port does not fight external plugins.
- To compare against the unlimited mode, create `ux0:data/ignition/g146_geo.txt` containing `fps-limit=0`, play the same race, then delete the file (or write `fps-limit=30`) to go back.
- Using a plugin to raise the CPU to 500 MHz can give more headroom. This is optional and at your own risk (heat, battery); the port itself never asks for it.
- The game runs on real time: lower FPS makes the picture less smooth but does not slow cars or lap timers.

## The two multiplayer entries do nothing

Intentional: **split screen** and **online multiplayer** are disabled in v1.1 and do not close the game. See the README, "Disabled features".

## Controls

- **A button is not accepted in *Options → Player 1 → Keyboard config*:** the game refuses a button that is already assigned to another action (of either player), or you pressed ○ while Start is assigned (or the other way round). Press another button. Release every button before pressing the new one: buttons held when the screen starts waiting are ignored.
- **The car does not accelerate / brake / steer after changing the controls:** open the key screen and check what each action shows (`PAD X`, `PAD SQ` ...). *ACC.* must have the button you use to accelerate. If you moved *TURN LEFT / RIGHT* to buttons, the D-pad and the stick no longer steer (assign `PAD LT` / `PAD RT`).
- **D-pad ↑ ↓ / stick ↑ ↓ do nothing in a race:** intentional (see [CONTROLS.md](CONTROLS.md#1-default-controls-v11)).
- **There is no button for shifting down:** □ is the brake by default; assign *GEAR DOWN* to a button of your choice.
- **Back to the defaults:** see [CONTROLS.md](CONTROLS.md#2-configurable-controls-v11); the complete reset deletes `ign_3dfx.btz` (and your progress).

## The player name, language, controls or progress are not saved

The game writes its saved data at several moments and when you leave it with its own *Quit* option. If you leave with the PS button or by turning the console off, the changes made since the last save may be lost. Since v1.1 a save is written to `ign_3dfx.btz.tmp` and replaces `ign_3dfx.btz` only when complete: if the card is full or removed, the previous save is kept. The log line `[G163-SESSION-FINAL]` shows `commits` (completed saves), `kept_old` and `failed`. If the name entry does not open, check the log for `[G159-NAME-IME-...]` lines.

If a `ign_3dfx.btz.tmp` file stays in `ux0:data/ignition/`, a save was interrupted; the game recovers it the next time it opens the save. Keep a backup copy of `ign_3dfx.btz` and attach the logs if progress is lost.

## Starting from a clean state

To reset the game's saved options and progress, delete `ux0:data/ignition/ign_3dfx.btz` (the game creates a new one). To remove old logs, delete the `ux0:data/ignition/logs/` folder. Your game data stay untouched.

## Logs and what to send

The folder `ux0:data/ignition/logs/` contains:

- `boot.txt` — start-up checkpoints (memory, audio threads, exit).
- `ignitionvita.log` — the main log (clock profile, limiter, notices, per-window performance lines, exit summary).
- `ignition_critical.log` — the errors and the most important lines, kept even if the main log is rotated.
- `ignition_session_id.txt` — a small counter that identifies the run.
- The files of the previous run have the suffix `.previous`.

When reporting a problem attach `boot.txt`, `ignitionvita.log` and `ignition_critical.log` and fill in the issue template. The logs contain no personal data, only paths under `ux0:data/ignition`.
