#!/usr/bin/env bash
# Relink Ignition Vita against the libraries that are installed in your VitaSDK
# right now, WITHOUT recompiling any source file.
#
# Use it after replacing or modifying one of the statically linked libraries
# (vitaGL, vitaShaRK, math-neon, SceShaccCgExt, taiHEN stubs) in
# $VITASDK/arm-vita-eabi/lib, or the engine library external/WinVita/build-vita/
# libwinx86_vita.a. It needs a previous complete build (scripts/build.sh), whose
# object files it reuses. See docs/LGPL_COMPLIANCE.md.
#
# Usage:  VITASDK=/usr/local/vitasdk ./scripts/relink.sh
set -euo pipefail

: "${VITASDK:?VITASDK must point to your VitaSDK (for example /usr/local/vitasdk)}"
export PATH="$VITASDK/bin:$PATH"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"

if [ ! -f "$BUILD/Makefile" ]; then
  echo "relink.sh: $BUILD has no previous build. Run ./scripts/build.sh once first." >&2
  exit 1
fi

# Remove only the link products. The object files (*.obj) stay, so make just
# links again and repackages eboot.bin and the VPK.
rm -f "$BUILD/ignitionvita" "$BUILD/ignitionvita.velf" \
      "$BUILD/eboot.bin" "$BUILD/eboot.bin.out" \
      "$BUILD"/IgnitionVita_v1.1*.vpk "$BUILD"/IgnitionVita_v1.1*.vpk.out \
      "$BUILD"/IgnitionVita_v1.1*.vpk_param.sfo

make -C "$BUILD" -j"$(nproc)"

echo
echo "Relinked. Output:"
ls -l "$BUILD"/IgnitionVita_v1.1*.vpk
sha256sum "$BUILD"/IgnitionVita_v1.1*.vpk
