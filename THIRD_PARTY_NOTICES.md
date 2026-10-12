# Third-party notices and license map

Ignition Vita contains, links to or was informed by the third-party works below. Each keeps its own license; this file lists them with their copyright holders. Copies of the license texts are in [`licenses/`](licenses/). How the libraries are linked, their versions and how to relink: [`docs/LGPL_COMPLIANCE.md`](docs/LGPL_COMPLIANCE.md).

## License map

| Component | Where | License | Copyright |
|---|---|---|---|
| **Original code and documentation of Ignition Vita** | `source/` (except the two files below), `scripts/`, `CMakeLists.txt`, docs | **MIT** ([`LICENSE`](LICENSE)) | (c) 2026 Zeusthenen (Zeus) |
| **Glide 2.x layer** | `source/win32/glide2x_ign.cpp`, `source/win32/glide2x_ign.h` | **LGPL-2.1-only** ([`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt)) | OpenGLide authors for the material taken from it (below); (c) 2026 Zeusthenen (Zeus) for the original parts |
| **WinVita / winx86** engine | `external/WinVita/` | MIT ([`licenses/WinVita-MIT.txt`](licenses/WinVita-MIT.txt)) | (c) 2026 Franck Roustan |
| **Box86 dynarec** | `external/WinVita/third_party/box86-dynarec/` | MIT ([`licenses/Box86-MIT.txt`](licenses/Box86-MIT.txt)) | (c) 2018-2021 Sebastien Chevalier (ptitSeb) |
| khash | `…/box86-dynarec/include/khash.h` | MIT (text in the file) | (c) 2008, 2009, 2011 Attractive Chaos |
| Realmode X86 Emulator Library | `…/box86-dynarec/emu/x86primop.*` | permissive BSD-like (text in the files and in `external/WinVita/THIRD-PARTY-NOTICES.md`) | see below |
| DejaVu Sans Mono pixel data | `external/WinVita/src/platform/vita_kb_font.h` | Bitstream Vera / DejaVu license (free use and redistribution; see <https://dejavu-fonts.github.io/>) | the DejaVu fonts project and its contributors |
| **stb_vorbis** v1.22 | `third_party/stb_vorbis/` | MIT or public domain (Unlicense), at your choice ([`licenses/stb_vorbis-MIT-or-Unlicense.txt`](licenses/stb_vorbis-MIT-or-Unlicense.txt)) | (c) 2017 Sean Barrett |
| vitaGL | linked (VitaSDK) | LGPL-3.0-or-later; also offered under GPL-3.0 ([`licenses/LGPL-3.0.txt`](licenses/LGPL-3.0.txt), [`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt)) | (c) 2017-2020 Rinnegatamante and contributors |
| vitaShaRK | linked (VitaSDK) | LGPL-3.0-or-later | (c) 2017-2020 Rinnegatamante |
| SceShaccCgExt | linked (VitaSDK) | **GPL-3.0**, according to its original license ([`licenses/GPL-3.0.txt`](licenses/GPL-3.0.txt)); no "or later" grant has been verified, so none is assumed | bythos14 (see its repository) |
| math-neon | linked (VitaSDK) | MIT ([`licenses/math-neon-MIT.txt`](licenses/math-neon-MIT.txt)) | (c) 2015 Lachlan Tychsen-Smith (the VitaSDK package is built from Rinnegatamante's repository of it) |
| taiHEN import stubs | linked (VitaSDK) | MIT ([`licenses/taiHEN-MIT.txt`](licenses/taiHEN-MIT.txt)) | (c) 2016 Yifan Lu |
| libpthread (pthread-embedded of the VitaSDK) | linked statically | LGPL-2.1-or-later ("the library as a whole is licensed under LGPLv2 or later" per its README; text [`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt)); Vita-specific patches MIT | pthreads-win32 / pthread-embedded authors and the VitaSDK contributors |
| VitaSDK libc (newlib), libgcc, libstdc++, system stubs | linked statically | their own licenses (libgcc and libstdc++ under the GPL with the GCC Runtime Library Exception) | the respective projects and the VitaSDK contributors |
| **The released VPK as a whole** | the executable inside the VPK | distributed under the applicable terms of the **GPL-3.0**, because SceShaccCgExt (GPL-3.0) is linked into it (explained in [`docs/LGPL_COMPLIANCE.md`](docs/LGPL_COMPLIANCE.md#1-who-is-under-which-license)) | — |

The original code of Ignition Vita stays MIT; the two Glide-layer files stay LGPL-2.1-only; vitaGL and vitaShaRK keep their LGPL licenses; SceShaccCgExt is GPL-3.0. The GPL-3.0 terms of the VPK come only from the combination of these in one executable and do not relicense any individual source file.
| *Ignition* (1997): executable, data, music, artwork, name, logos | **not included** | belongs to its owners | — |

## Not part of this project

**Ignition** (1997), its executable, data, music, artwork, names and logos belong to their respective owners. None of it is included in this repository or in the released VPK, and this project claims no right over it. The LiveArea artwork of the port (`sce_sys/`, `docs/images/`) shows the game's logo for identification only.

> Ignition Vita is an unofficial, non-commercial fan port. It is not affiliated with or endorsed by Interplay, UDS, or any current rights holder. Ignition, its name, logo and original game assets belong to their respective owners. Original game files are not included and must be supplied legally by the user. This notice does not grant any right over the logo or the original resources.
>
> Ignition Vita es un port de aficionados, no oficial y sin fines comerciales. No está afiliado ni respaldado por Interplay, UDS ni por ningún titular actual de los derechos. Ignition, su nombre, logotipo y los recursos originales del juego pertenecen a sus respectivos propietarios. Los archivos originales del juego no se incluyen y deben ser aportados legalmente por el usuario. Este aviso no constituye una concesión de derechos sobre el logotipo ni sobre los recursos originales.

Ignition Vita is not affiliated with or endorsed by Sony either.

## Code included in this repository

### WinVita / winx86 — `external/WinVita/`
- Copyright (c) 2026 Franck Roustan. License: **MIT** ([`licenses/WinVita-MIT.txt`](licenses/WinVita-MIT.txt)). Upstream: <https://github.com/Franckrst/WinVita>.
- A generic x86 → ARMv7 execution engine and Win32 runtime for PlayStation Vita, extracted from the D2Vita project. It is vendored as source (only `src/`, `third_party/`, `build.sh`, the `tools/` that document the Box86 patches, and its own `README.md`, `LICENSE` and `THIRD-PARTY-NOTICES.md`) so that v1.0 can be rebuilt exactly.
- **Local modifications** made for Ignition Vita (kept under the same MIT license): the DirectSound emulation `src/runtime/ds_emul.cpp` / `ds_emul.h`, the audio sink header `src/runtime/audio_sink.h` and the Vita audio output `src/platform/vita_audio.cpp`. Everything else is the WinVita snapshot taken for this project; whoever updates the vendored copy from upstream must reapply or upstream those four files.
- Its own third-party notices are in [`external/WinVita/THIRD-PARTY-NOTICES.md`](external/WinVita/THIRD-PARTY-NOTICES.md) and summarised here.

### Box86 dynarec core — `external/WinVita/third_party/box86-dynarec/`
- Copyright (c) 2018-2021 Sebastien Chevalier ("ptitSeb"). License: **MIT** ([`licenses/Box86-MIT.txt`](licenses/Box86-MIT.txt)). Upstream: <https://github.com/ptitSeb/box86>.
- Verbatim extraction of the ARMv7 dynarec plus WinVita's mechanically generated local patch set (`external/WinVita/tools/box86_local_patches.diff`).
- `khash.h`: Copyright (c) 2008, 2009, 2011 by Attractive Chaos, MIT (text embedded in the file).
- `emu/x86primop.c` / `x86primop.h`: from the *Realmode X86 Emulator Library*: Copyright (c) 1996-1999 SciTech Software, Inc.; (c) David Mosberger-Tang; (c) 1999 Egbert Eich; (c) 2007-2017 SUSE LINUX GmbH (Steffen Winterfeldt). A permissive BSD-like license with a non-endorsement clause; the full text is in `external/WinVita/THIRD-PARTY-NOTICES.md` and in the file headers.
- `src/platform/vita_kb_font.h`: pixel data generated from **DejaVu Sans Mono**. No font file is embedded.

### stb_vorbis — `third_party/stb_vorbis/`
- Ogg Vorbis decoder v1.22, Copyright (c) 2017 Sean Barrett. License: **MIT or public domain (Unlicense), at your choice**. Used unmodified by `source/music/ogg_source.cpp`. See `third_party/stb_vorbis/ORIGIN.txt`.

## The Glide layer: OpenGLide, NFSIISE and the Glide API

`source/win32/glide2x_ign.cpp` and `glide2x_ign.h` implement the Glide 2.x API that `Ign_3dfx.exe` imports, on top of vitaGL. They contain material from other projects and are therefore **LGPL-2.1-only**, not MIT (each file carries the same notice at its top):

- **OpenGLide** — <https://github.com/voyageur/openglide>, a git mirror of the original CVS repository; its `LICENSE` file is the GNU LGPL version 2.1. Its source headers say: "OpenGLide - Glide to OpenGL Wrapper (http://openglide.sourceforge.net) … OpenGLide is OpenSource under LGPL license. Originally made by Fabio Barros. Modified by Paul for Glidos (http://www.glidos.net). Linux version by Simon White." Tables, translation logic and semantics taken or adapted from it — in particular the translations of `guColorCombineFunction` and `guAlphaSource` (from OpenGLide's `grguColorAlpha.cpp`) and the behaviour of the `grgu*` draw functions — belong to those authors and are used under the LGPL. This project claims no copyright over that material. Zeusthenen (Zeus) claims copyright only over his own original contributions to the two files and licenses them under the same LGPL-2.1-only terms. The exact OpenGLide revision that was consulted was not recorded.
- **NFSIISE** — <https://github.com/zaps166/NFSIISE>, MIT, Copyright (c) 2014-2024 Błażej Szczygieł ([`licenses/NFSIISE-MIT.txt`](licenses/NFSIISE-MIT.txt)). Its Glide 2.x wrapper was studied; the table of Glide screen-resolution sizes in `glide2x_ign.cpp` is taken from its headers, and a few design decisions follow it (cited in the source comments).
- **The Glide 2.x API** (function names, parameter lists, numeric constants) is the public interface of 3dfx Interactive's Glide, which any compatible implementation reproduces. It was cross-checked against `sdk2_glide.h` as distributed in the OpenGLide repository; that header carries a 3dfx Interactive copyright notice. Only the names, signatures, constants and facts needed to implement a compatible API were consulted; **no 3dfx header or source file (`sdk2_glide.h` or any other) is included in this repository** (checked by file name and by content). "Glide" and "3dfx" are trademarks of their owners. The `sezero/glide` repository (3dfx GLIDE Source Code General Public License) was consulted for structure sizes and enum values only.

Some comments in `source/runtime/ignition_boot.cpp` and in the Glide files cite these projects as the evidence for decisions; they contain no code of them.

## Libraries linked into the executable (not included in the repository)

They come from the VitaSDK installation and are linked statically into `eboot.bin`. Versions, file hashes, upstream addresses and the way to replace them: [`docs/LGPL_COMPLIANCE.md`](docs/LGPL_COMPLIANCE.md).

- **vitaGL** and **vitaShaRK** — Rinnegatamante and contributors, LGPL-3.0-or-later (per the headers `vitaGL.h` and `vitashark.h`). <https://github.com/Rinnegatamante/vitaGL>, <https://github.com/Rinnegatamante/vitaShaRK>.
- **SceShaccCgExt** — bythos14, GPL-3.0. <https://github.com/bythos14/SceShaccCgExt>. It is pulled in by vitaShaRK; the port's own code never calls it.
- **math-neon** — Lachlan Tychsen-Smith, MIT.
- **taiHEN** import stubs — Yifan Lu, MIT. <https://github.com/yifanlu/taiHEN>. The port calls no taiHEN function; the stub library is a link dependency of vitaShaRK/vitaGL.
- **VitaSDK** — <https://vitasdk.org/>.

Because these libraries are statically linked, anyone who receives the VPK may obtain the source and relink the application with a modified version of them. This repository contains the complete source and the scripts of the application; [`docs/LGPL_COMPLIANCE.md`](docs/LGPL_COMPLIANCE.md) explains the procedure (`scripts/relink.sh`) and the license terms of the combined binary.

## Tools

Built with the VitaSDK toolchain (GCC) under MSYS2. The build scripts only call tools of the VitaSDK; none of them is distributed here.

## Acknowledgments

Development and documentation assistance: Anthropic Claude and OpenAI ChatGPT.
