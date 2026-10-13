#!/bin/bash
# Regenerate tools/box86_local_patches.diff FROM THE LIVE TREE.
#
# extract_box86.sh used to carry a hand-maintained 6-item patch that had
# fallen 29 files behind the real divergence (fastmmu, memintrin, emitprof,
# signtag, the d2vita_progress_c hook...). Re-running it would have silently
# reverted third_party/box86-dynarec/ to vanilla-Box86-plus-6-old-patches,
# erasing everything else. See docs/audit for the incident.
#
# This script closes that hole mechanically: it diffs a *fresh, verbatim*
# copy of the upstream Box86 sources against the live third_party tree and
# writes the exact delta as a patch file. No hand-transcription, so no risk
# of mistranscribing dynarec logic. Run this BEFORE committing any change
# under third_party/box86-dynarec/, so extract_box86.sh always has an
# accurate patch to replay.
#
# Usage: tools/regen_box86_patch.sh [path-to-box86-checkout]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Pas de chemin machine par defaut : un moteur generique ne connait pas
# l'arborescence de la machine qui l'a construit. Sans argument, on
# cherche un checkout Box86 fraternel du depot (../box86), et sinon on
# demande explicitement le chemin plutot que de deviner un compte utilisateur.
BOX86="${1:-}"
if [ -z "$BOX86" ]; then
  CAND="$(cd "$ROOT/.." 2>/dev/null && pwd)/box86"
  [ -f "$CAND/src/dynarec/dynarec_arm.c" ] && BOX86="$CAND"
fi
[ -n "$BOX86" ] || { echo "Chemin Box86 introuvable : passe-le en argument ($0 <chemin>)"; exit 1; }
B="$BOX86/src"
D="$ROOT/third_party/box86-dynarec"
OUT="$ROOT/tools/box86_local_patches.diff"

[ -f "$B/dynarec/dynarec_arm.c" ] || { echo "Box86 source not found at $BOX86"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/a" "$WORK/b"

# --- side a: fresh verbatim copy of upstream, same layout as $D ---
mkdir -p "$WORK/a"/{dynarec,emu,include,tools}
for f in \
    arm_emitter.h arm_lock_helper.h arm_lock_helper.S \
    arm_prolog.S arm_epilog.S arm_next.S \
    dynarec_arm_jmpnext.c \
    dynarec_private.h dynarec_arm_private.h dynablock_private.h \
    dynarec_arm_pass0.h dynarec_arm_pass1.h dynarec_arm_pass2.h dynarec_arm_pass3.h \
    dynarec_arm_helper.c dynarec_arm_helper.h \
    dynarec_arm_functions.c dynarec_arm_functions.h \
    dynarec_arm_pass.c \
    dynarec_arm_emit_math.c dynarec_arm_emit_logic.c \
    dynarec_arm_emit_shift.c dynarec_arm_emit_tests.c \
    dynarec_arm_00.c dynarec_arm_0f.c dynarec_arm_64.c dynarec_arm_65.c \
    dynarec_arm_66.c dynarec_arm_660f.c dynarec_arm_66f0.c dynarec_arm_67.c \
    dynarec_arm_d8.c dynarec_arm_d9.c dynarec_arm_da.c dynarec_arm_db.c \
    dynarec_arm_dc.c dynarec_arm_dd.c dynarec_arm_de.c dynarec_arm_df.c \
    dynarec_arm_f0.c dynarec_arm_f20f.c dynarec_arm_f30f.c \
    dynarec_arm.c dynablock.c dynarec.c \
; do cp "$B/dynarec/$f" "$WORK/a/dynarec/"; done
for f in regs.h custommem.h rbtree.h dynarec.h dynablock.h dynarec_arm.h khash.h; do
    cp "$B/include/$f" "$WORK/a/include/"
done
for f in x86emu_private.h x86run_private.h x87emu_private.h x87emu_private.c \
         x87emu_setround.h x86primop.h x86primop.c x86compstrings.h x86compstrings.c; do
    cp "$B/emu/$f" "$WORK/a/emu/"
done
cp "$B/tools/bridge_private.h" "$WORK/a/tools/"
cp "$B/tools/rbtree.c" "$WORK/a/tools/"
cp "$B/custommem.c" "$WORK/a/"

# --- side b: the live tree, minus LICENSE (verbatim, never part of the diff) ---
cp -r "$D"/. "$WORK/b/"
rm -f "$WORK/b/LICENSE"

# --- diff, then validate mechanically before trusting it ---
( cd "$WORK" && LC_ALL=C diff -ruN a b > "$OUT" || true )

VALIDATE="$(mktemp -d)"
trap 'rm -rf "$WORK" "$VALIDATE"' EXIT
cp -r "$WORK/a" "$VALIDATE/a"
patch -d "$VALIDATE/a" -p1 --forward -s < "$OUT"
if ! diff -rq "$VALIDATE/a" "$WORK/b" > /dev/null; then
    echo "REFUS: le patch régénéré ne reproduit pas l'arbre vivant à l'identique." >&2
    diff -rq "$VALIDATE/a" "$WORK/b" >&2
    exit 1
fi

echo "OK: $OUT régénéré et validé mécaniquement ($(grep -c '^--- a/' "$OUT") fichiers, $(wc -l < "$OUT") lignes)."
