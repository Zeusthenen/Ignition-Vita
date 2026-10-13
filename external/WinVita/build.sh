#!/bin/bash
# build.sh — build libwinx86.a: the Box86-derived x86->ARMv7 dynarec core
# (third_party/box86-dynarec + src/dynarec86) plus the generic runtime layer
# (src/runtime, src/platform) that any Windows-PE32-on-PS-Vita port links
# against. Two targets:
#
#   ./build.sh              qemu-arm/Linux test target -> build-arm/libwinx86.a
#   TARGET=vita ./build.sh  real PS Vita target         -> build-vita/libwinx86_vita.a
#
# This is the "standalone library" half of the hybrid architecture: winx86
# must build on its own, with zero knowledge of any specific game. A
# consumer (e.g. d2vita) links this archive and plugs in its own game-specific
# native hooks and Win32 shims purely through the public API in
# src/runtime/cpu.h (Cpu::set_alternate) and src/runtime/bridge.h
# (Bridge::register_shim / shim_trap) — see README.md.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
B86="$ROOT/third_party/box86-dynarec"
SHIM="$ROOT/src/dynarec86/shim"
DYN86="$ROOT/src/dynarec86"
RT="$ROOT/src/runtime"
PLAT="$ROOT/src/platform"
RENDER="$ROOT/src/render"

VITA_EXTRA=""
if [ "${TARGET:-}" = vita ]; then
    OUT="$ROOT/build-vita"
    LIBNAME=libwinx86_vita.a
    CC=arm-vita-eabi-gcc
    CXX=arm-vita-eabi-g++
    # -std=gnu17: box86 uses K&R-style `void f()` decls that C23 (gcc>=14
    # default) rejects against `void f(int)` definitions.
    VITA_EXTRA="-I$SHIM/vita -std=gnu17 -D__vita__ -D__clear_cache(b,e)=dyn86_vita_clear_cache(b,e)"
    CXX_VITA_EXTRA="-D__vita__ -I$SHIM/vita"
else
    OUT="$ROOT/build-arm"
    LIBNAME=libwinx86.a
    CC=arm-linux-gnueabihf-gcc
    CXX=arm-linux-gnueabihf-g++
fi

# LIBTAG/EXTRA : construire une SAVEUR de la bibliotheque (objets et archive
# separes) pour un A/B a la compilation — ex. LIBTAG=_prof EXTRA="-DPROF_COUNTERS",
# ou LIBTAG=_callret EXTRA="-DD2_CALLRET=1". Meme convention que l'ancien
# tools/build_dynarec86.sh de d2vita (dont ce script prend la releve) : EXTRA
# s'applique aux DEUX cotes (C du dynarec, C++ de la couche runtime), un define
# non reconnu par l'un des deux cotes est simplement ignore. Sans eux, rien ne
# change (mêmes noms de sortie qu'avant l'ajout de ce mecanisme).
OBJ="$OUT/obj"
if [ -n "${LIBTAG:-}" ]; then
    LIBNAME="${LIBNAME%.a}${LIBTAG}.a"
    OBJ="$OBJ$LIBTAG"
fi
LIB="$OUT/$LIBNAME"

CFLAGS="-O2 -g -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DDYNAREC -DARM -DUSE_MMAP -DTRACE_MEMSTAT $VITA_EXTRA \
  -I$SHIM -I$DYN86 -I$B86/include -I$B86 -I$B86/dynarec \
  -Wno-unused-variable -Wno-unused-parameter -Wno-unused-function \
  -Wno-unused-but-set-variable -Wno-pointer-sign ${EXTRA:-}"

CXXFLAGS="-std=gnu++17 -O2 -g -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard \
  -DD2RT_CPU_BOX86 $CXX_VITA_EXTRA \
  -I$ROOT/src -I$DYN86 -I$DYN86/shim -I$B86/include -I$B86 \
  -Wno-unused-variable -Wno-unused-parameter ${EXTRA:-}"

mkdir -p "$OBJ"/{core,pass0,pass1,pass2,pass3,rt}

# --- C: dynarec core (compiled once) ---
CORE_SRC="
$B86/dynarec/dynarec.c
$B86/dynarec/dynablock.c
$B86/dynarec/dynarec_arm.c
$B86/dynarec/dynarec_arm_functions.c
$B86/dynarec/dynarec_arm_jmpnext.c
$B86/custommem.c
$B86/tools/rbtree.c
$B86/emu/x87emu_private.c
$B86/emu/x86primop.c
$B86/emu/x86compstrings.c
$SHIM/shim_impl.c
$SHIM/x86run_flags.c
$DYN86/dyn86.c
$DYN86/alt_table.c
$DYN86/dyn86_emitprof.c
$DYN86/dyn86_memintrin.c
$DYN86/dyn86_intrin.c
"
[ "${TARGET:-}" = vita ] && CORE_SRC="$CORE_SRC $SHIM/vita/mman_vita.c"

ASM_SRC="
$B86/dynarec/arm_prolog.S
$B86/dynarec/arm_epilog.S
$B86/dynarec/arm_next.S
$B86/dynarec/arm_lock_helper.S
"

# --- C: the pass sources, compiled 4x with -DSTEP=0..3 (like Box86 itself) ---
PASS_SRC="
$B86/dynarec/dynarec_arm_helper.c
$B86/dynarec/dynarec_arm_pass.c
$B86/dynarec/dynarec_arm_emit_tests.c
$B86/dynarec/dynarec_arm_emit_math.c
$B86/dynarec/dynarec_arm_emit_logic.c
$B86/dynarec/dynarec_arm_emit_shift.c
$B86/dynarec/dynarec_arm_00.c
$B86/dynarec/dynarec_arm_0f.c
$B86/dynarec/dynarec_arm_64.c
$B86/dynarec/dynarec_arm_65.c
$B86/dynarec/dynarec_arm_66.c
$B86/dynarec/dynarec_arm_67.c
$B86/dynarec/dynarec_arm_d8.c
$B86/dynarec/dynarec_arm_d9.c
$B86/dynarec/dynarec_arm_da.c
$B86/dynarec/dynarec_arm_db.c
$B86/dynarec/dynarec_arm_dc.c
$B86/dynarec/dynarec_arm_dd.c
$B86/dynarec/dynarec_arm_de.c
$B86/dynarec/dynarec_arm_df.c
$B86/dynarec/dynarec_arm_f0.c
$B86/dynarec/dynarec_arm_660f.c
$B86/dynarec/dynarec_arm_66f0.c
$B86/dynarec/dynarec_arm_f20f.c
$B86/dynarec/dynarec_arm_f30f.c
"

# --- C++: the generic runtime layer ---
RT_SRC="
$RT/pe_image.cpp
$RT/bridge.cpp
$RT/cpu.cpp
$RT/cpu_box86.cpp
$RT/sched_cooperative.cpp
$RT/gil.cpp
$RT/sched_native.cpp
$RT/prof.cpp
$RT/audio_sink_host.cpp
$RT/guest_atomics.cpp
$RT/guest_files.cpp
$RT/guest_toolhelp.cpp
$RT/guest_sync.cpp
$RT/win32_shims_sync.cpp
$RT/guest_thread_ctx.cpp
$RT/win32_shims_kernel32.cpp
$RT/win32_shims_locale.cpp
$RT/win32_shims_memory.cpp
$RT/win32_shims_wait.cpp
$RT/win32_shims_shell32.cpp
$RT/win32_shims_advapi32.cpp
$RT/win32_shims_user32.cpp
$RT/win32_shims_misc.cpp
$RT/win32_shims_psapi.cpp
$RT/win32_shims_version.cpp
$RT/authenticode.cpp
$RT/win32_shims_wintrust.cpp
$RT/win32_shims_gdi32.cpp
$RT/win32_shims_window.cpp
$RT/win32_shims_wsock32.cpp
$RT/poll_gil.cpp
$RT/net_nonblock.cpp
$RT/net_guard.cpp
$RT/guest_scratch.cpp
$RT/guest_region.cpp
$RT/host_clock.cpp
$RT/guest_str.cpp
$RT/guest_locale.cpp
$RT/ds_emul.cpp
$PLAT/vita_audio.cpp
$PLAT/vita_host.cpp
$PLAT/vita_gpumem.cpp
$PLAT/present_scale.cpp
$RENDER/render_null.cpp
"

# ---------------------------------------------------------------------------
# Incrementale et parallele. AVANT ce mecanisme, les 163 unites (18 CORE +
# 4 ASM + 41 RT + 25 PASS x4 STEP) etaient recompilees a CHAQUE appel, meme
# quand rien n'avait change — la moitie du temps d'un build_rt_boot_vpk.sh
# complet (qui, lui, met en cache ses 19 unites depuis le 12/09). Meme
# mecanisme, deja eprouve la-bas : empreinte de commande (.cmd) + dependances
# .d de gcc (-MMD -MP), un fichier .rc PAR unite pour le code de retour — un
# echec derriere un `&` ne doit pas se perdre silencieusement sous `set -e`,
# contrairement a une simple commande en arriere-plan (c'etait la raison
# d'etre du mode sequentiel avant ce commit).
CXX_BANNER="$($CC --version | head -1) | $($CXX --version | head -1)"
DEPFLAGS="-MMD -MP"

need_rebuild() { # $1=obj $2=cmd -> imprime la raison, code 0 si recompilation requise
  local o="$1" cmd="$2" d="${1%.o}.d" c="${1%.o}.cmd" dep
  [ -f "$o" ] || { echo "objet absent"; return 0; }
  [ -f "$c" ] || { echo "empreinte absente"; return 0; }
  [ "$(cat "$c")" = "$cmd" ] || { echo "commande modifiee"; return 0; }
  [ -f "$d" ] || { echo "fichier .d absent"; return 0; }
  while read -r dep; do
    [ -n "$dep" ] || continue
    if [ ! -e "$dep" ]; then echo "dependance disparue: $dep"; return 0; fi
    if [ "$dep" -nt "$o" ]; then echo "plus recent: $dep"; return 0; fi
  done < <(tr -s ' \\\t' '\n\n\n' < "$d" | sed -e '/:$/d' -e '/^$/d')
  return 1
}

# id() disambigue deux objets de meme basename dans des dossiers differents —
# le cas des 25 PASS_SRC, compiles une fois par STEP dans pass0/../pass3/.
id() { printf '%s_%s' "$(basename "$(dirname "$1")")" "$(basename "${1%.o}")"; }

TODO_SRC=(); TODO_OBJ=(); TODO_CMD=(); TODO_KIND=(); TODO_EXTRA=(); TODO_WHY=(); SKIPPED=0
add_unit() { # $1=src $2=obj $3=kind(c|cxx) $4=extraflags
  local s="$1" o="$2" k="$3" extra="${4:-}" cmd why
  if [ "$k" = cxx ]; then cmd="[$CXX_BANNER] $CXX $CXXFLAGS $extra $DEPFLAGS -c $s -o $o"
  else                    cmd="[$CXX_BANNER] $CC $CFLAGS $extra $DEPFLAGS -c $s -o $o"; fi
  if why="$(need_rebuild "$o" "$cmd")"; then
    TODO_SRC+=("$s"); TODO_OBJ+=("$o"); TODO_CMD+=("$cmd"); TODO_KIND+=("$k"); TODO_EXTRA+=("$extra"); TODO_WHY+=("$why")
  else
    SKIPPED=$((SKIPPED+1))
  fi
}

for s in $CORE_SRC; do add_unit "$s" "$OBJ/core/$(basename "${s%.*}").o" c; done
for s in $ASM_SRC;  do add_unit "$s" "$OBJ/core/$(basename "${s%.*}").o" c; done
for s in $RT_SRC;   do add_unit "$s" "$OBJ/rt/$(basename "${s%.*}").o" cxx; done
for step in 0 1 2 3; do
    for s in $PASS_SRC; do
        add_unit "$s" "$OBJ/pass$step/$(basename "${s%.*}").o" c "-DSTEP=$step"
    done
done

echo "== ${#TODO_OBJ[@]} unite(s) a (re)compiler, $SKIPPED a jour =="
for i in "${!TODO_OBJ[@]}"; do
  case "${TODO_KIND[$i]}" in cxx) t=CXX;; *) t=CC;; esac
  echo "  $t  $(basename "${TODO_OBJ[$i]}")  [${TODO_WHY[$i]}]"
done

JOBS="${WINX86_JOBS:-$(( $(nproc) - 1 ))}"
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
STATE="$OUT/.fastbuild${LIBTAG:-}"; rm -rf "$STATE"; mkdir -p "$STATE"

# Extra ${TODO_EXTRA[$i]:-} non protege par des guillemets : un define vide se
# dissout au lieu de devenir un argument fantome que gcc/g++ prendrait pour un
# fichier d'entree (meme raison que dans l'ancien compile_c/compile_cxx).
compile_one() { # $1=index
  local i="$1" s="${TODO_SRC[$i]}" o="${TODO_OBJ[$i]}" k="${TODO_KIND[$i]}" extra="${TODO_EXTRA[$i]}"
  local base; base="$(id "$o")"
  local rc=0
  if [ "$k" = cxx ]; then
    $CXX $CXXFLAGS $extra $DEPFLAGS -c "$s" -o "$o" > "$STATE/$base.log" 2>&1 || rc=$?
  else
    $CC $CFLAGS $extra $DEPFLAGS -c "$s" -o "$o" > "$STATE/$base.log" 2>&1 || rc=$?
  fi
  if [ "$rc" = 0 ]; then
    printf '%s' "${TODO_CMD[$i]}" > "${o%.o}.cmd"
    echo 0 > "$STATE/$base.rc"
  else
    rm -f "$o" "${o%.o}.cmd" "${o%.o}.d"
    echo "$rc" > "$STATE/$base.rc"
  fi
}

if [ "${#TODO_OBJ[@]}" -gt 0 ]; then
  running=0
  for i in "${!TODO_OBJ[@]}"; do
    while [ "$running" -ge "$JOBS" ]; do wait -n || true; running=$((running-1)); done
    compile_one "$i" &
    running=$((running+1))
  done
  wait
  fails=0
  for i in "${!TODO_OBJ[@]}"; do
    base="$(id "${TODO_OBJ[$i]}")"
    rc="$(cat "$STATE/$base.rc" 2>/dev/null || echo 127)"
    if [ "$rc" != "0" ]; then
      fails=$((fails+1))
      echo "---- ECHEC: ${TODO_SRC[$i]} (rc=$rc) ----" >&2
      cat "$STATE/$base.log" >&2 2>/dev/null || true
    else
      [ -s "$STATE/$base.log" ] && { echo "---- $(basename "${TODO_SRC[$i]}") ----"; cat "$STATE/$base.log"; } || true
    fi
  done
  [ "$fails" -eq 0 ] || { echo "FATAL: $fails unite(s) en echec" >&2; exit 1; }
fi

AR="${CC%-gcc}-ar"
rm -f "$LIB"
$AR rcs "$LIB" "$OBJ"/core/*.o "$OBJ"/pass*/*.o "$OBJ"/rt/*.o
echo "== $LIB =="
$AR t "$LIB" | wc -l
