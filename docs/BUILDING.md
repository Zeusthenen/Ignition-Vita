# Building from source

This guide rebuilds Ignition Vita from scratch: the WinVita engine library, the executable and the VPK. It was written from, and checked against, a build of this exact tree made in an empty directory (see [Verification](#7-verification-of-the-released-package-and-of-a-source-build)).

## 1. What you need

| Item | Tested with |
|---|---|
| Host OS | Windows 10 with **MSYS2** (the plain `msys` shell). Linux should work the same way but was not tested |
| VitaSDK | built 2026-04-25 (vita-toolchain `71f3789`, vita-headers `de02de2`), GCC `arm-vita-eabi-gcc` 15.2.0, installed at `/usr/local/vitasdk` |
| CMake | 4.3.2 (any recent 3.x or 4.x should do) |
| GNU Make | 4.4.1 |
| Python 3 | 3.12 (only for `scripts/check_vpk.py`) |
| bash, coreutils, sed, tr | the ones of MSYS2 (WinVita's `build.sh` is a bash script) |

Libraries installed in the VitaSDK (they are linked into the executable):

| Library | vdpm package |
|---|---|
| vitaGL | `vitaGL` |
| vitaShaRK (runtime shader compiler support) | `vitaShaRK` |
| math-neon | `libmathneon` |
| SceShaccCgExt | `SceShaccCgExt` |
| taiHEN import stubs (needed by vitaGL / vitaShaRK) | `taihen` |

The port itself calls no taiHEN function; the stub library is only a link dependency of vitaGL / vitaShaRK. The licenses of these libraries, the exact files the tested VPK was linked with (SHA-256) and the versions are in [LGPL_COMPLIANCE.md](LGPL_COMPLIANCE.md).

## 2. Installing the tools (MSYS2 + VitaSDK)

Install [MSYS2](https://www.msys2.org/), open the **MSYS** shell and install the basic tools:

```bash
pacman -Syu
pacman -S --needed make git cmake python
```

Install the VitaSDK as described on <https://vitasdk.org/> (summary):

```bash
git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
./vdpm vitaGL vitaShaRK libmathneon SceShaccCgExt taihen   # or ./install-all.sh
```

Put `VITASDK` and the `PATH` line in your shell profile so they are set every time. Check with:

```bash
arm-vita-eabi-gcc --version
vita-pack-vpk 2>&1 | head -1      # prints the usage
ls $VITASDK/arm-vita-eabi/lib/libvitaGL.a
```

The vitaGL build in the VitaSDK package manager is the one the port was developed with (`libvitaGL.a` of 603,588 bytes, installed on 2026-04-25; SHA-256 in [LGPL_COMPLIANCE.md](LGPL_COMPLIANCE.md)). **A fresh `vdpm` installation today may download a newer vitaGL** (the VitaSDK recipe now pins a later upstream commit), which has not been tested with this port and may behave differently. To stay as close as possible to the tested build, compare the SHA-256 of your `libvitaGL.a` with the one in that page and, if it differs, build vitaGL from the commit identified there (`make HAVE_SBRK=1 NO_DEBUG=1`; the commit is established by evidence, not recorded by the package, see the page). The same page gives the versions of vitaShaRK (v1.5), SceShaccCgExt (v1.0.1) and taiHEN (v0.11) of the tested build.

## 3. Getting the source

Clone or unpack this repository to a path **without spaces**, for example `~/IgnitionVita`. Everything needed is inside: `source/` (the port), `external/WinVita/` (the vendored engine), `third_party/` (stb_vorbis), `sce_sys/` (LiveArea resources), `CMakeLists.txt` and `scripts/`.

## 4. The quick way

```bash
cd ~/IgnitionVita
export VITASDK=/usr/local/vitasdk
./scripts/build.sh            # LIGHT (release) build
```

It builds the engine library (about 163 translation units; allow several minutes), configures and builds the executable, and writes `build/IgnitionVita_v1.0.vpk`. Use `./scripts/build.sh FULL` for the diagnostic build (heavy logging, for developers only).

## 5. The same, step by step

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

# 1) the engine static library  ->  external/WinVita/build-vita/libwinx86_vita.a
cd external/WinVita
TARGET=vita ./build.sh
cd ../..

# 2) configure
mkdir -p build && cd build
cmake -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DIGNITION_RENDERER_3DFX=ON \
      -DIGN_DIAGNOSTICS=LIGHT \
      ..

# 3) build: executable -> eboot.bin -> VPK
make -j"$(nproc)"
```

Notes:

- `-DIGNITION_RENDERER_3DFX=ON` selects the 3dfx/Glide game (`Ign_3dfx.exe`), which is the supported one. Without it a legacy DirectDraw diagnostic variant is built; do not use it.
- `-DIGN_DIAGNOSTICS=LIGHT` is the release logging level (default); `FULL` adds heavy diagnostics.
- If the engine library is missing, CMake stops with a message telling you to run `build.sh` in `external/WinVita` first.
- Rebuilding is incremental (WinVita's script only recompiles what changed).
- Warnings: the executable itself compiles without warnings; the engine library build prints a few harmless ones from the vendored sources (for example `'USE_MMAP' redefined`). The `param.sfo` produced is identical to the one of the released package.

Output: `build/IgnitionVita_v1.0.vpk` (also `build/eboot.bin`, `build/ignitionvita` and `build/ignitionvita.velf`).

## 6. The LiveArea resources inside the VPK

The VPK carries four files besides `eboot.bin` and `param.sfo`; CMake takes them from `sce_sys/` and stores them at these **exact** paths (case and separators matter):

```
sce_sys/icon0.png                          128 x 128
sce_sys/livearea/contents/template.xml
sce_sys/livearea/contents/bg0.png          840 x 500
sce_sys/livearea/contents/startup.png      280 x 158
```

Rules learnt the hard way:

- The three PNGs **must be 8-bit indexed (palette) PNGs: PNG colour type 3, bit depth 8, not interlaced**. 24-bit RGB or RGBA files make VitaShell refuse the package with error `0x8010113D`.
- `template.xml` must reference the images **by name only** (`bg0.png`, `startup.png`), because it sits in the same folder as them. It is stored as UTF-8 without a BOM:

```xml
<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="2">
    <livearea-background>
        <image>bg0.png</image>
    </livearea-background>
    <gate>
        <startup-image>startup.png</startup-image>
    </gate>
</livearea>
```

- If you change the artwork, keep the exact dimensions. A tool such as `pngquant` or ImageMagick (`magick in.png PNG8:out.png`) can produce an indexed PNG; then **check** it (next section).

Checking the dimensions and the format of a PNG without installing anything:

```bash
python3 - <<'EOF'
import struct, sys
for f in ("sce_sys/icon0.png", "sce_sys/livearea/contents/bg0.png", "sce_sys/livearea/contents/startup.png"):
    d = open(f, "rb").read()
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", d[16:29])
    print(f, w, "x", h, "bit depth", depth, "colour type", ctype, "interlace", interlace)   # want: 8 and 3 and 0
EOF
```

## 7. Verification of the released package and of a source build

Inspect any VPK (this does not install it) and print its hashes:

```bash
python3 scripts/check_vpk.py build/IgnitionVita_v1.0.vpk --extract /tmp/ign_vpk_check
```

It checks that the package contains exactly the six expected members at the exact paths, that the three PNGs have the right dimensions, 8-bit depth and colour type 3, that `template.xml` references `bg0.png` and `startup.png`, and prints the size, SHA-256 / SHA-1 / MD5 and the SHA-256 of every member. `--extract` also unpacks the VPK so you can look at the files.

What was verified for this release (see also the release notes):

- The tree in this repository was copied to an **empty directory** and built with the commands of section 4. It built without warnings and produced a VPK with the correct structure.
- The machine code of all 22 object files of the executable and of all 91 members of `libwinx86_vita.a` built from this tree is **identical** to the code of the build that was tested on the console (compared section by section, ignoring only addresses).
- A source build is therefore functionally equivalent to the released `IgnitionVita_v1.0.vpk` but **not byte-identical**: the game's start-up line embeds the compile date and time, and the packaged `eboot.bin` therefore differs by a few bytes. The released VPK is the file that was tested; use its SHA-256 (in the release notes) to confirm a download.

## 8. Relinking with a modified library

If you change one of the statically linked libraries (vitaGL, vitaShaRK, ...), you do not need to recompile the port: install your build of the library in `$VITASDK/arm-vita-eabi/lib` and run

```bash
./scripts/relink.sh
```

It relinks the existing object files and rebuilds the VPK. Details and the license terms: [LGPL_COMPLIANCE.md](LGPL_COMPLIANCE.md).

## 9. Troubleshooting a build

| Problem | Hint |
|---|---|
| `libwinx86_vita.a not found` | run `TARGET=vita ./build.sh` in `external/WinVita` first (or use `scripts/build.sh`) |
| `arm-vita-eabi-gcc: command not found` | `VITASDK` / `PATH` not exported in this shell |
| errors about a missing vitaGL / vitashark / `-lmathneon` / `-lSceShaccCgExt` / `-ltaihen_stub` | install the packages of section 1 with `vdpm` |
| C errors in the engine about K&R declarations | WinVita's script already passes `-std=gnu17`; make sure you did not override the compiler flags |
| `bash: ./build.sh: bad interpreter` | the scripts need LF line endings (`.gitattributes` enforces them; fix with `dos2unix`) |
| the VPK is refused by VitaShell with `0x8010113D` | the LiveArea PNGs are not 8-bit indexed |
