# Licenses of the binary, corresponding source and relinking

Ignition Vita links several libraries **statically** into `eboot.bin` (the executable inside the VPK). Some are under the GNU LGPL, one is under the GNU GPL. This page states, unambiguously, which license applies to what, which exact source corresponds to the released `IgnitionVita_v1.0.vpk`, how sure we are of each version, and how to rebuild or relink the program with modified libraries. License texts are in [licenses/](../licenses/); copyright lines in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## 1. Who is under which license

| Part | License |
|---|---|
| Original code of Zeusthenen (Zeus): `source/` (except the two files below), `scripts/`, `CMakeLists.txt`, the documentation | **MIT** |
| `source/win32/glide2x_ign.cpp`, `source/win32/glide2x_ign.h` (contain material from OpenGLide) | **LGPL-2.1-only** |
| `external/WinVita/` (Franck Roustan; Box86 by ptitSeb inside), `third_party/stb_vorbis/` | MIT (stb_vorbis: MIT or public domain) |
| **vitaGL**, **vitaShaRK** (linked, not in this repository) | keep their own **LGPL** licenses — LGPL-3.0-or-later according to the headers of their files (`vitaGL.h`, `vitashark.h`); the vitaGL repository also provides the GPL-3.0 text |
| **SceShaccCgExt** (linked, not in this repository) | **GPL-3.0**, according to its original license (the `LICENSE` file of its repository is the GPL version 3 text; no "or later" grant was found there, so none is assumed) |
| math-neon, taiHEN import stubs (linked) | MIT |
| libpthread (pthread-embedded of the VitaSDK, linked) | LGPL-2.1-or-later (its README: "the library as a whole is licensed under LGPLv2 or later"); Vita-specific patches MIT |
| newlib (libc), libgcc, libstdc++ (linked) | their own licenses; libgcc and libstdc++ are under the GPL with the GCC Runtime Library Exception |
| **The released VPK / the combined executable** | distributed under the **applicable terms of the GPL-3.0**, because the GPL-3.0 library SceShaccCgExt is statically linked into it |

How these fit together (the author's reading; seek legal advice if you need certainty):

* A binary that contains GPL-3.0 code is distributed under the GPL-3.0 as a whole. Everything else in the executable is under a license that permits that: MIT; LGPL-3.0-or-later (vitaGL, vitaShaRK); LGPL-2.1 for `glide2x_ign.*` and libpthread (section 3 of the LGPL-2.1 allows a recipient to apply the ordinary GNU GPL, of the same or a newer version, to a given copy); and the GCC Runtime Library Exception for libgcc/libstdc++.
* This does **not** relicense any source file of this repository: the MIT files stay MIT, the two Glide-layer files stay LGPL-2.1-only. The GPL-3.0 terms of the VPK come only from the combination in one executable.
* Redistributing the VPK means: include the GPL-3.0 text and the notices (done in the release package), do not add further restrictions, and keep the corresponding source available (section 2).
* Nothing stops anyone from installing a modified version on their own console: homebrew packages are installed freely and no signing key or secret is needed.

## 2. Corresponding source of `IgnitionVita_v1.0.vpk`

**In this repository** (tag of the release): the complete source of the port (`source/`), the vendored engine (`external/WinVita/`), `third_party/stb_vorbis/`, the LiveArea resources (`sce_sys/`), `CMakeLists.txt`, and the scripts to build, check and relink (`scripts/build.sh`, `scripts/check_vpk.py`, `scripts/relink.sh`). Build instructions: [BUILDING.md](BUILDING.md).

**Libraries linked from the VitaSDK installation**, with the source they were built from. The released VPK was linked on 2026-04-25 with the files identified here by size and SHA-256. The VitaSDK packages are prebuilt by the VitaSDK project and carry no commit id, so the source version is established from the package recipes that were in force in April 2026 (read from the history of <https://github.com/vitasdk/packages>; none of these recipes changed between their last commit before that date and 2026-04-25) and from the checks listed in the last column.

| Library | Linked file (size, SHA-256) | Source | How firmly identified |
|---|---|---|---|
| **vitaGL** | `libvitaGL.a`, 603,588 B, `0171211cb4afae035944432d830b14346804c60a9caa49c985549c588e460aa4` | <https://github.com/Rinnegatamante/vitaGL> at commit `c5d2ad6cc548d2191e92d765e06973f09712acbc` (2026-04-22, "Add vglLazyFree function."); built with `make HAVE_SBRK=1 NO_DEBUG=1` | **Identified by evidence, not certified byte for byte** — see below. The recipe followed the repository's default branch and did not pin a commit |
| **vitaShaRK** | `libvitashark.a`, 3,400 B, `0b85bc0a8cba76d016a4f84bdcdcd644fa2b52098ffb927d40c4ea36ef665f4e` | release **v.1.5**: <https://github.com/Rinnegatamante/vitaShaRK/archive/refs/tags/v.1.5.tar.gz> | The archive downloaded for the source package has the SHA-256 recorded in the April 2026 recipe (`07fd6b72c3cd9a7ce85b73474d85a3b10d6ca5e0d5e0eccb1715b68de895376c`), and the installed `vitashark.h` is byte-identical to its `source/vitashark.h`. The library binary itself was not rebuilt |
| **SceShaccCgExt** | `libSceShaccCgExt.a`, 60,518 B, `ddde5a6e75be658beaeb10d5912ea3f76c24dca02e1586565dfe65836793beb2` | release **v1.0.1** of <https://github.com/bythos14/SceShaccCgExt>: <https://github.com/bythos14/SceShaccCgExt/archive/refs/tags/v1.0.1.tar.gz> | Archive SHA-256 equals the recipe's (`a1cda9f3d637032cc597f2f4fdbc17813e5c7c90943a7d44c2e35f013c8dbc0b`); installed `shacccg_ext.h` byte-identical to its `include/shacccg_ext.h`. The library binary was not rebuilt |
| **taiHEN** stubs | `libtaihen_stub.a`, 19,892 B, `94161d5e869c2ea757528b42564b8acfb7c0fa40b7245a59852537e6b15943ed` | release **v0.11** of <https://github.com/yifanlu/taiHEN> (source tag archive <https://github.com/yifanlu/taiHEN/archive/refs/tags/v0.11.tar.gz>; prebuilt package of the release <https://github.com/yifanlu/taiHEN/releases/download/v0.11/taihen.tar.gz>) | The release package has the recipe's SHA-256 (`6a7d4592b983292e8b8c53c624fa800e33ff69fe06c9d267cafc33d0ded4d2a9`) and the installed `taihen.h` is byte-identical to it, **but the installed stub archives are not byte-identical to the ones in that package** (same sizes, different contents; the reason was not determined). They hold only import stubs; the port calls no taiHEN function |
| **math-neon** | `libmathneon.a`, 44,694 B, `fbac93bc0eea3737422b5e71429a3b0212ce01580cff5b3392b3db83afbdb006` | <https://github.com/Rinnegatamante/math-neon> at commit `0faab814782c071ff4015527f1ca955ab1ccc470` (2020-07-05, "Added Patreon support link.") | **Best supported by the evidence, not verified**: that commit is the last of the repository (no commit since 2020, so it was the default-branch head in April 2026) and the installed `math_neon.h` is byte-identical to its `source/math_neon.h`. The library binary was not rebuilt |
| **libpthread** | VitaSDK `libpthread.a` | <https://github.com/vitasdk/pthread-embedded> at commit `63e1cd9152082d9ccb1b38d67a4caf975562fbeb` | Exact: recorded by the installed VitaSDK (`version_info.txt`, built 2026-04-25) |
| **newlib** (libc, libm), system headers/stubs | VitaSDK `libc.a`, `libm.a`, `Sce*_stub.a` | <https://github.com/vitasdk/newlib> at `c1bd5fcb79612809d302b94f0f8e82d2f4da4c66`; <https://github.com/vitasdk/vita-headers> at `de02de215f36e427c6fd8c2fac9ed107d0210b53` (the stubs are generated from its database); <https://github.com/vitasdk/vita-toolchain> at `71f3789342f610a4eb3ba50fc33292c4900c6666` (a build tool, not linked; not bundled) | Exact: recorded by the installed VitaSDK |
| libgcc, libstdc++ | VitaSDK toolchain `arm-vita-eabi-gcc` 15.2.0 | the GCC sources as built by the VitaSDK, see <https://vitasdk.org/> and <https://github.com/vitasdk/buildscripts> | Version exact; **not bundled**: they are under the GPL with the GCC Runtime Library Exception, which lets the resulting program be conveyed under other terms |

### The corresponding-source package

The release carries **`IgnitionVita_v1.0_corresponding-source.tar.xz`** next to the VPK. It contains: a copy of this repository as it is at the release (source, scripts and documentation, including the build and relink instructions), the original upstream archives listed above (vitaGL, vitaShaRK, SceShaccCgExt, taiHEN, math-neon, pthread-embedded, newlib, vita-headers) exactly as downloaded, the license and attribution files of every component, and `SOURCE_MANIFEST.md` with the origin, version or commit, URL, SHA-256 and level of certainty of each source. The SHA-256 of the package and of every downloaded archive are in `SHA256SUMS_SOURCES.txt` (release assets). No game data, executable, music or other commercial material is in it.

The permanent links above remain the primary reference; the package makes the offer independent of those repositories staying online.

### How vitaGL was identified (and what is still not proven)

1. The installed `vitaGL.h` (70,342 B, installed with the library, 2026-04-25) is **byte-identical** to `source/vitaGL.h` at commit `c5d2ad6…`; that file did not change from that commit until the commit "Implement glReadBuffer" (`19ebdbd`, 2026-04-29), so every commit in between has the same header.
2. The installed library contains `vglLazyFree` (added by `c5d2ad6`) and does **not** contain `vglReadPixels` / `glReadBuffer` (added on 2026-04-29).
3. The April 2026 recipe built the repository's default branch at package build time, and the files carry the timestamp 2026-04-25 09:23 (+02:00). The last upstream commit before that moment is `c5d2ad6…`; the next one is from 2026-04-26.
4. The archive of commit `c5d2ad6…` downloaded from GitHub for the source package is file-for-file identical to the local git checkout of that commit used for the cross-check below.
5. As a cross-check, `c5d2ad6…` was built here with the VitaSDK toolchain and the flags of the recipe (`make HAVE_SBRK=1 NO_DEBUG=1`): the 904 strong (non-weak) global symbols of that build are exactly the same set as those of the installed library. Only weak/vague C++ template instantiations (29 in the local build, 42 in the installed one, with differences in both directions) differ, which depends on the compiler and standard library used (the installed library was built with GCC 10.3.0; the local one with 15.2.0).

This is strong evidence, but **not a proof**: the binary cannot be reproduced byte for byte with a different compiler, and the package does not record its commit. Treat `c5d2ad6…` as "identified by evidence", not as certified. A fresh `vdpm` installation today gives a **newer** vitaGL (the current recipe pins `2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8`, builds with `make NO_DEBUG=1`), vitaShaRK v1.7 and a changed package set; that combination has not been tested with this port.

## 3. How to rebuild or relink with a modified library

Everything needed is in this repository. Build instructions: [BUILDING.md](BUILDING.md).

1. Do a complete build once, so that the object files exist:

   ```bash
   export VITASDK=/usr/local/vitasdk
   ./scripts/build.sh
   ```

2. Obtain the source of the library you want to change (table above), modify it, build it with the VitaSDK toolchain and install the result over the file in the SDK. For vitaGL, with the recipe's flags:

   ```bash
   git clone https://github.com/Rinnegatamante/vitaGL
   cd vitaGL
   git checkout c5d2ad6cc548d2191e92d765e06973f09712acbc   # or any commit you want to try
   # ... your modifications ...
   export PATH="$VITASDK/bin:$PATH"
   make HAVE_SBRK=1 NO_DEBUG=1
   cp libvitaGL.a "$VITASDK/arm-vita-eabi/lib/"
   cp source/vitaGL.h "$VITASDK/arm-vita-eabi/include/"
   cd ..
   ```

   The other libraries (vitaShaRK, SceShaccCgExt, math-neon, taiHEN stubs, libpthread) are replaced the same way in `$VITASDK/arm-vita-eabi/lib`. The engine library `external/WinVita/build-vita/libwinx86_vita.a` can be rebuilt with `TARGET=vita ./build.sh` in `external/WinVita`.

3. Relink **without recompiling** any source file of the port:

   ```bash
   ./scripts/relink.sh
   ```

   It deletes only the link products (`build/ignitionvita`, `eboot.bin`, the VPK) and runs `make`, which links the existing object files (`build/CMakeFiles/ignitionvita.dir/`) with the libraries currently installed and repackages the VPK in `build/`. `make -C build VERBOSE=1` after a `relink.sh` shows the exact link command. If your change modifies the public headers of a library (an API/ABI change), run `./scripts/build.sh` instead so that the port is recompiled too.

4. Check the result with `python3 scripts/check_vpk.py build/IgnitionVita_v1.0.vpk` and install it on your console as any other VPK.

`scripts/relink.sh` was tested on a clean build of this tree (it relinked and repackaged the VPK without compiling anything).

## 4. The LGPL-2.1 files of the port

`source/win32/glide2x_ign.cpp` and `source/win32/glide2x_ign.h` are licensed LGPL-2.1-only because they contain material taken or adapted from **OpenGLide** (<https://github.com/voyageur/openglide>, LGPL 2.1; authors named in its headers: Fabio Barros, Paul for Glidos, Simon White). They are part of this source repository and are compiled with the rest of the port; anyone can modify them and rebuild with `scripts/build.sh`. See the notice at the top of each file and [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). No 3dfx header or source file is included in this repository; only the names, signatures, constants and facts needed to implement a compatible Glide 2.x API were consulted.

## 5. What is not known

* The upstream commit of vitaGL is identified by evidence, not certified byte for byte (section 2). The same applies, to a lesser degree, to math-neon (the head of its repository, unchanged since 2020; not verified) and to the taiHEN stubs (their archives are not byte-identical to the release package; only import stubs).
* The OpenGLide revision that was consulted was not recorded.
* The recipes of April 2026 were reconstructed from the public history of the VitaSDK package repository; the packages themselves cannot be re-downloaded in their April 2026 state. The upstream archives bundled in the source package were downloaded in October 2026.
* The license position of the combined binary is the author's reading, not legal advice.
