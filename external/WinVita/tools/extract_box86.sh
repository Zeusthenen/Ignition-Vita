#!/bin/bash
# Reproducible extraction of the Box86 x86->ARMv7 dynarec core into
# third_party/box86-dynarec/.
# Box86 is (c) ptitSeb, MIT license (the LICENSE file is preserved in the
# extracted tree). Source revision expected: 39d3ed2.
#
# Usage: tools/extract_box86.sh [path-to-box86-checkout]
#
# Idempotent: re-copies verbatim then re-applies tools/box86_local_patches.diff
# (the FULL local delta — fastmmu, memintrin, emitprof, signtag, the
# scheduler preemption seam, the SMC write-protection switch, and the
# original 6-item patch this script used to carry inline). That patch file
# is generated and mechanically validated by tools/regen_box86_patch.sh —
# never hand-edit it; regenerate it from the live tree before touching
# anything under third_party/box86-dynarec/, or a re-run of THIS script will
# silently revert your changes to whatever the patch file currently says.
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
PATCHFILE="$ROOT/tools/box86_local_patches.diff"

[ -f "$B/dynarec/dynarec_arm.c" ] || { echo "Box86 source not found at $BOX86"; exit 1; }
[ -f "$PATCHFILE" ] || { echo "Missing $PATCHFILE — run tools/regen_box86_patch.sh first"; exit 1; }

# Safety check: if $D already exists and diverges from what re-extraction
# would produce, refuse to clobber it silently. Extract into a scratch dir
# first and diff before touching the real tree.
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

mkdir -p "$SCRATCH"/{dynarec,emu,include,tools}
cp "$BOX86/LICENSE" "$SCRATCH/LICENSE"

# --- dynarec core (verbatim) ---
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
; do cp "$B/dynarec/$f" "$SCRATCH/dynarec/"; done
# NOTE: arm_printer.{c,h} deliberately NOT copied (shimmed to a no-op)

# --- public headers (verbatim) ---
for f in regs.h custommem.h rbtree.h dynarec.h dynablock.h dynarec_arm.h khash.h; do
    cp "$B/include/$f" "$SCRATCH/include/"
done

# --- emu support (verbatim) ---
for f in x86emu_private.h x86run_private.h x87emu_private.h x87emu_private.c \
         x87emu_setround.h x86primop.h x86primop.c x86compstrings.h x86compstrings.c; do
    cp "$B/emu/$f" "$SCRATCH/emu/"
done

# --- tools + custom memory manager (verbatim) ---
cp "$B/tools/bridge_private.h" "$SCRATCH/tools/"
cp "$B/tools/rbtree.c" "$SCRATCH/tools/"
cp "$B/custommem.c" "$SCRATCH/"

# --- the full local patch set (fastmmu, memintrin, emitprof, signtag,
#     preemption seam, SMC switch — see tools/regen_box86_patch.sh) ---
LICENSE_BACKUP="$SCRATCH/LICENSE"
patch -d "$SCRATCH" -p1 --forward -s < "$PATCHFILE"
cp "$LICENSE_BACKUP" "$SCRATCH/LICENSE" 2>/dev/null || true

if [ -d "$D" ]; then
    if ! diff -rq "$SCRATCH" "$D" > /dev/null 2>&1; then
        echo "REFUS: la ré-extraction ne reproduit pas l'arbre $D à l'identique." >&2
        echo "Le patch stocké (tools/box86_local_patches.diff) est en retard sur des" >&2
        echo "modifications faites directement dans $D. Lance d'abord:" >&2
        echo "    tools/regen_box86_patch.sh" >&2
        echo "pour capturer ces modifications, PUIS relance cet outil." >&2
        echo "Différences détectées:" >&2
        diff -rq "$SCRATCH" "$D" >&2
        exit 1
    fi
    echo "Arbre déjà à jour, rien à faire ($(find "$D" -type f | wc -l) fichiers)."
    exit 0
fi

mkdir -p "$(dirname "$D")"
mv "$SCRATCH" "$D"
trap - EXIT

echo "Extracted Box86 dynarec ($(git -C "$BOX86" rev-parse --short HEAD 2>/dev/null || echo '?')) into $D"
find "$D" -type f | wc -l
