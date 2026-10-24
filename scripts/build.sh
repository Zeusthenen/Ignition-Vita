#!/usr/bin/env bash
# Builds the WinVita engine library and Ignition Vita, and produces build/IgnitionVita_v1.1.vpk
#
#   VITASDK=/usr/local/vitasdk ./scripts/build.sh          # release build (LIGHT diagnostics)
#   VITASDK=/usr/local/vitasdk ./scripts/build.sh FULL     # diagnostic build (much more logging; not for normal use)
#
# Run it from an MSYS2 / Linux shell. See docs/BUILDING.md.
set -euo pipefail
: "${VITASDK:?VITASDK must point to your VitaSDK installation (e.g. /usr/local/vitasdk)}"
export PATH="$VITASDK/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LEVEL="${1:-LIGHT}"
case "$LEVEL" in LIGHT|FULL) ;; *) echo "usage: $0 [LIGHT|FULL]" >&2; exit 2;; esac

echo "== 1/3: WinVita engine (x86 -> ARMv7 dynarec + Win32 runtime) =="
( cd "$ROOT/external/WinVita" && TARGET=vita ./build.sh )

echo "== 2/3: configure Ignition Vita ($LEVEL) =="
mkdir -p "$ROOT/build"
cd "$ROOT/build"
cmake -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
      -DIGNITION_RENDERER_3DFX=ON \
      -DIGN_DIAGNOSTICS="$LEVEL" \
      "$ROOT"

echo "== 3/3: build =="
make -j"$(nproc)"
ls -l ./*.vpk
sha256sum ./*.vpk
