# Installation guide

Ignition Vita is a homebrew application. It needs (1) the VPK, installed on your Vita, and (2) the data files of **your own legal copy** of the game, copied to `ux0:data/ignition/`. Nothing from the game is included with the port and none will be offered.

## 1. Installing the VPK

Requirements: a Vita or PlayStation TV with custom firmware (for example HENkaku / Ensō) and a VPK installer (VitaShell is the usual one).

1. Enable **unsafe homebrew** in the homebrew settings of your custom firmware. The port runs the game through a dynamic recompiler that needs executable memory, so its bubble is an *unsafe* application and will not install or start in "safe" mode.
2. Copy `IgnitionVita_v1.0.vpk` to the Vita (USB, FTP or VitaShell's own file transfer).
3. Open it in VitaShell and confirm the installation. A bubble named **Ignition Vita** appears in the LiveArea (Title ID `IGNVITA12`).
4. If you are updating from an older build, install over it; do **not** uninstall by deleting `ux0:data/ignition` (that folder holds your game data and saved options, see [section 6](#6-updating-to-a-future-version)).

If VitaShell refuses the package, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md#installation-errors).

## 2. Getting the original game data legally

You need the PC version of *Ignition* (1997), from a legal source (for example your original CD). The port is written for the **3dfx / Glide executable** `Ign_3dfx.exe` of 626,688 bytes, SHA-256:

```
9C40E7D2711C58A8EE11C8A16125BB5FE90179FFA693432D0E45793C9FC7B79B
```

(Compute it on your PC with `certutil -hashfile Ign_3dfx.exe SHA256` on Windows or `sha256sum Ign_3dfx.exe` on Linux/macOS.) Other builds of the executable are not supported.

If the game is installed on a PC, the files are in its installation folder. You only need the items below; you do **not** need the `*.dll` files, `Ign_d3d.exe`, uninstallers, the manual or the 3dfx / DirectDraw wrappers.

### Required items

Copy them to `ux0:data/ignition/` keeping names and folder structure (Windows file names are case-insensitive; keep them as they are in the original):

```
ux0:data/ignition/
├── Ign_3dfx.exe
├── MenuBkg.dat
├── sys.col
├── H_pan1.pic   H_pan2.pic   H_signs.pic
├── N_pangfx.pic N_Signs.pic  N_sysgfx.pic  N_sysg_2.pic
├── Pokal.pic    S_pangfx.pic S_Signs.pic
├── Baltazar/    (whole folder, including data/)
├── CARS/        (whole folder)
├── fonts/       (whole folder)
├── GENERAL/     (whole folder)
├── Ghosts/      (whole folder; the game keeps best-lap ghosts there)
├── LEVELS/      (whole folder: Austria, Brazil, Canada, Carib, Iceland, Japan, USA)
└── Music/       (optional, see below)
```

The folders amount to about 37 MB in the version the port was tested with.

### The music (optional, but strongly recommended)

The original game plays its music from **CD audio tracks**, which cannot be read from a Vita. The port reproduces a "virtual CD": if you place your own conversion of the CD's audio tracks in `ux0:data/ignition/Music/`, the game plays them.

- Rip tracks **2 to 8** of the original CD (track 1 is the data track) and convert each to **Ogg Vorbis**.
- Name them exactly `Track02.ogg`, `Track03.ogg`, `Track04.ogg`, `Track05.ogg`, `Track06.ogg`, `Track07.ogg`, `Track08.ogg` (the number is the CD track number).
- Any normal Vorbis settings work; the port reads the sample rate and length from the files. The author's set is about 70 MB in total.
- Without these files the game is expected to run without music (sound effects do not depend on them); this case has not been tested on a console. The log lists the music tracks that were found or are missing.

Do not share these files: they are copyrighted material.

## 3. Copying the data to the Vita

- With VitaShell: connect by USB (or enable FTP), then copy the items above to `ux0:data/ignition/`. Create the folder if it does not exist.
- Check that `Ign_3dfx.exe` is directly inside `ux0:data/ignition/` (not in a sub-folder) and that folder names are spelled as in the list.
- Total used space: about 110 MB with music.

## 4. First start

1. Start **Ignition Vita** from the LiveArea. After the loading screens the game's language selector appears.
2. Choose your language with the D-pad and confirm with ✕. The choice is remembered between runs.
3. Navigate the menus with the D-pad/left stick, ✕ to confirm and ○ (or Start) to go back. In the player options you can change the name with the Vita keyboard (see [CONTROLS.md](CONTROLS.md#changing-the-player-name)).
4. Leave the game with the *Quit* entry of its menu so that your options are saved and the Vita's clocks are restored.

## 5. First run and checking the logs

After the first run the folder `ux0:data/ignition/logs/` exists. Open `ignitionvita.log` (any text editor on the PC, after copying it) and check:

- `[G160-CLOCKS] ... result=OK` (or `UNCHANGED` if your clocks were already high enough): the clock profile was applied.
- `[RELEASE-FPS-LIMITER] configured=30 ...`: the 30 FPS limiter is active.
- `[G157-SPLITSCREEN-NOTICE]` and `[G158-ONLINE-NOTICE]`: the two disabled multiplayer entries are in place.
- `[G159-NAME-IME-NOTICE] player name keyboard armed ...`: the keyboard for the name is available.
- At exit: `[RELEASE-FPS-SUMMARY] ...` and `[G160-CLOCKS-RESTORE] ...`.

If the log reports a music track as MISSING or INVALID, that file was not found or cannot be read (check its name and format).

## 6. Updating to a future version

- Install the new `IgnitionVita_*.vpk` over the old one.
- Keep `ux0:data/ignition/` untouched: it contains your game files, `ign_3dfx.btz` (the game's own saved options and progress) and the optional `g146_geo.txt` settings file.
- Read the release notes: if a version changes the data layout it will say so.

## Uninstalling

Delete the bubble with the LiveArea's usual method. If you also want to remove the data and saved options, delete `ux0:data/ignition/` yourself.
