# Frequently asked questions

Spanish readers: the README is available in Spanish ([README_ES.md](../README_ES.md)); this FAQ is in English.

## About the game and the files

**Does this include the game?**
No. The repository and the VPK contain **no data of the commercial game**: no executable, resources, music, videos, tracks or models. You need your own legal copy of *Ignition* (1997) for PC and copy the files listed in [INSTALLATION.md](INSTALLATION.md) to `ux0:data/ignition/`. No download links are provided or will be provided.

**Where do I get the game?**
From a legal source you own, for example the original CD. This project cannot help with obtaining the game.

**Which version of the game works?**
The 3dfx/Glide executable `Ign_3dfx.exe` of 626,688 bytes (SHA-256 `9C40E7D2711C58A8EE11C8A16125BB5FE90179FFA693432D0E45793C9FC7B79B`). Other builds of the executable (for example `Ign_d3d.exe`) are not supported.

**Is this an official release or affiliated with the game's owners?**
No. It is an unofficial, non-profit fan project, not affiliated with or endorsed by the owners of *Ignition* or by Sony. The game's name, logos and resources belong to their owners.

**There is no music.**
The original plays CD audio, which the Vita cannot read. Convert tracks 2–8 of your own CD to Ogg Vorbis and name them `Track02.ogg` … `Track08.ogg` in `ux0:data/ignition/Music/`. See [INSTALLATION.md](INSTALLATION.md#the-music-optional-but-strongly-recommended).

## Console and installation

**Which consoles work?**
Tested on a real **PS Vita**. **PlayStation TV: not tested.**

**Do I need unsafe homebrew?**
Yes. The port runs the game through a dynamic recompiler that needs executable memory, so it is an *unsafe* application.

**Do I need `libshacccg.suprx`?**
Not verified. The port compiles no shaders of its own, but it links libraries that can use that module, and it has not been run on a console without it. If the game does not start and you do not have it, install it as described in the vitaGL documentation and tell us the result.

**How do I update?**
Install the new VPK over the old one and keep `ux0:data/ignition/` (your files and saved options).

**VitaShell says `0x8010113D`.**
See [TROUBLESHOOTING.md](TROUBLESHOOTING.md#installation-errors). The released VPK does not have that problem.

## Playing

**What are the controls?**
See [CONTROLS.md](CONTROLS.md). In short: D-pad / left stick to move, ✕ confirm, ○ or Start back and pause, △ / □ shift gears, L rear view, R turbo.

**How do I change the player name?**
Select the name option in the player options menu and press ✕: the Vita keyboard opens (up to 3 characters).

**Why are split screen and online multiplayer disabled?**
Split screen needs two players on one screen, which is not usable with a single Vita, and the online mode needs a network stack the port does not provide. In v1.0 both entries are visible but do nothing and do not close the game.

**Where are my saves?**
The game writes its own save/configuration file `ign_3dfx.btz` in `ux0:data/ignition/` when you leave with its *Quit* option.

## Performance

**What frame rate should I expect?**
Usually about 25–30 FPS in races. It varies with the track, the mode, the number of vehicles, the scene and the clock frequencies. It is **not** a locked or constant 30 FPS.

**Which clock frequencies does the port use?**
At start-up it requests at least ARM 444 / BUS 222 / GPU 222 / GPU Xbar 166 MHz. It never lowers a higher frequency set by something else and, at exit, restores only what it changed itself.

**I use a 500 MHz plugin. Is that a problem?**
No. The port respects it. Using such a plugin is optional and your own responsibility (heat, battery).

**How do I change or disable the 30 FPS limiter?**
Create `ux0:data/ignition/g146_geo.txt` with `fps-limit=30` (default), `fps-limit=0` / `fps-limit=off` (unlimited) or another value from 10 to 60. The game's speed does not depend on the frame rate.

## Problems and logs

**Where are the logs?**
`ux0:data/ignition/logs/` (`boot.txt`, `ignitionvita.log`, `ignition_critical.log`). Attach them when you report a problem; see [TROUBLESHOOTING.md](TROUBLESHOOTING.md#logs-and-what-to-send).

**How do I report a bug?**
Open an issue with the *Bug report* template: <https://github.com/Zeusthenen/Ignition-Vita/issues>. Incomplete reports may need clarification.

## Source code and licenses

**Where is the source code?**
In this repository, including the vendored engine (`external/WinVita`). Build instructions: [BUILDING.md](BUILDING.md).

**Which license does the project use?**
Not a single one: MIT for the original code of Zeusthenen (Zeus), LGPL-2.1-only for two files that contain OpenGLide material, LGPL for vitaGL and vitaShaRK, GPL-3.0 for SceShaccCgExt, and the licenses of each other third-party component. The VPK as a combined binary is distributed under the applicable terms of the GPL-3.0 because SceShaccCgExt is linked into it. See the table in the README, [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and [LGPL_COMPLIANCE.md](LGPL_COMPLIANCE.md).

**Can I rebuild it with my own version of vitaGL?**
Yes: [LGPL_COMPLIANCE.md](LGPL_COMPLIANCE.md#3-how-to-rebuild-or-relink-with-a-modified-library) explains how, and `scripts/relink.sh` relinks without recompiling.

**What are the "G138", "G160" labels in the code and the logs?**
Numbers of the private development steps. They are kept for technical traceability.

**Was AI used?**
The acknowledgments of the README state it: development and documentation assistance by Anthropic Claude and OpenAI ChatGPT.
