cmake_minimum_required(VERSION 3.16)

# --- Toolchain -----------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(ignitionvita CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Experimental 3dfx/Glide bring-up branch (see audit/NFS2_GLIDE_REFERENCE.md
# and the G0 delivery notes). OFF by default -- the stable DirectDraw build
# (Ign_d3d.exe) is completely unaffected when this is off; every 3dfx-only
# code path is guarded by the IGNITION_RENDERER_3DFX compile definition
# this option adds, never a runtime branch. Separate TITLEID/VPK name so
# the two can coexist as distinct apps on the Vita -- this never replaces
# or overwrites the stable ignitionvita-diag.vpk.
option(IGNITION_RENDERER_3DFX "Build the experimental Ign_3dfx.exe/Glide bring-up branch instead of the stable DirectDraw branch" OFF)

# iteration-G23-BIS (kept through G24+): G23B's real-hardware log turned out
# to come from a stale pre-G23 install still sitting on the Vita under
# IGNVITA02 (confirmed by binary string search -- the delivered G23 VPK did
# not contain the old tags that log showed), and the cause of that stale
# install was never root-caused (LiveArea's own update-detection, most
# likely). IGNVITA03 subsequently proved reliable -- G24's real-hardware run
# confirmed the vertex-array pipeline via this exact TITLEID. Deliberately
# NOT reverted back to IGNVITA02: doing so would reintroduce the same
# update-ambiguity risk this TITLEID exists to eliminate, for no benefit.
# Purely packaging identity, no Glide/Win32 logic implied by this choice.

# iteration-G84, Parte 16-19: controlled diagnostic A/B test -- for the
# first 30 real-race frames, force-clears the rotated-in GL backbuffer
# immediately after each vglSwapBuffers, before the guest draws anything
# into it, to test whether that alone makes the stale loading/menu curtain
# disappear. Declared here (before VITA_VPKNAME is set) so the two builds
# get distinct VPK names/app names and can be delivered side by side for
# a real-hardware A/B, per the user's own explicit request. Default OFF --
# the normal build's behavior is completely unchanged either way.
# iteration-G138: compile-time diagnostics level. LIGHT is the default and the only level of the normal VPK; FULL is for future diagnostic VPKs (and the G137 baseline build).
# Never selected at run time.
set(IGN_DIAGNOSTICS "LIGHT" CACHE STRING "Diagnostics level: LIGHT (default, normal VPK) or FULL (all historical hot-path diagnostics)")
set_property(CACHE IGN_DIAGNOSTICS PROPERTY STRINGS LIGHT FULL)
if(NOT (IGN_DIAGNOSTICS STREQUAL "LIGHT" OR IGN_DIAGNOSTICS STREQUAL "FULL"))
  message(FATAL_ERROR "IGN_DIAGNOSTICS must be LIGHT or FULL (got '${IGN_DIAGNOSTICS}')")
endif()
option(G84_CLEAR_PROBE"Diagnostic: force-clear the rotated-in GL backbuffer for the first 30 real-race frames, before any guest draw (3dfx branch only, G84 controlled stale-buffer elimination test)" OFF)
option(G101_LFB_AB_PROBE "G101 controlled A/B: suppress unchanged final LFB presentation during the PROBE periods" OFF)
# iteration-G104: G103's own zero-dirty guard is promoted from an implicit
# always-on default to an explicit, still-buildable option -- its real-
# hardware VPK must remain reproducible (the user's own "do not overwrite
# G101-G103 VPKs" rule) even though G104's frame-aware compositor below is
# now the new default for a normal build.
option(G103_ZERO_DIRTY_LFB_GUARD_BUILD "Build G103's own global zero-dirty LFB suppression guard instead of G104/G105's frame-aware compositor (3dfx branch only)" OFF)

if(IGNITION_RENDERER_3DFX)
  if(G101_LFB_AB_PROBE)
    set(VITA_APP_NAME  "IgnitionVita (3dfx G101 LFB A-B)")
    set(VITA_VPKNAME   "IgnitionVita_G101_LFB_AB_PROBE")
  elseif(G84_CLEAR_PROBE)
    # iteration-G85, Parte 26: retired from active use (G84's own A/B test
    # was visually IDENTICAL to normal -- clearprobe_visual_change=NO,
    # simple_missing_clear_root_cause=REJECTED). Source and CMake option
    # kept behind this compile-time OFF-by-default switch rather than
    # deleted, per this iteration's own "keep source if desired" allowance.
    set(VITA_APP_NAME  "IgnitionVita (3dfx G84-clearprobe)")
    set(VITA_VPKNAME   "ignitionvita-3dfx-g84-clearprobe")
  elseif(G103_ZERO_DIRTY_LFB_GUARD_BUILD)
    set(VITA_APP_NAME  "IgnitionVita (3dfx G103 zero-dirty guard)")
    set(VITA_VPKNAME   "IgnitionVita_G103_ZERO_DIRTY_LFB_GUARD")
  else()
    # iteration-G104: frame-aware LFB/GL compositor. Classifies each
    # completed LFB lock/unlock group by real per-frame Glide draw activity
    # and a persistent 16x16 dirty-tile map instead of G103's single global
    # "skip if globally unchanged" rule (which both hid the LFB-only
    # language selector and still let the stale fullscreen LFB blit
    # overwrite the actively-rendering race world most of the time).
    #
    # iteration-G105: G104's own first real-hardware run showed its
    # `contract` field wrongly required an exact lock/unlock CALLER address
    # match, so any one of Ignition's several legitimate lock/unlock call-
    # site families inside a swap's ~33-35 transactions flipped the WHOLE
    # interval to UNKNOWN -- RACE frames=60, unknown=60, sparse=0 on real
    # hardware. G105 fixes the contract in place to validate pure
    # STRUCTURE (type/buffer/mode/pointer/stride/geometry), never caller
    # identity -- see glide2x_ign.cpp's own G105 block comment for the
    # full real-hardware evidence. Same G104_FRAME_AWARE_LFB_COMPOSITOR
    # compile path/define (the fix is internal to it, not a separate
    # toggle -- the old caller-matching logic was not preserved behind a
    # switch since it was a confirmed bug, not a design this project ever
    # wants to reproduce), just a new VPK identity for this iteration.
    # iteration-G106: G105's own real-hardware run proved two further
    # problems: (1) PERFORMANCE -- G103's own hot-path analyzer ran IN
    # PARALLEL with G104's, each doing a full 512000-byte compare/copy per
    # LOCK/UNLOCK TRANSACTION (33-60 of them per race swap), on the order
    # of 100ms+/frame of pure CPU buffer-touching versus the GPU overlay's
    # own 0.12-0.25ms -- so G106 moves ALL of that to ONE read+diff+CRC per
    # SWAP (not per transaction) and compiles G103's own hot-path functions
    # out of this build entirely (see G103_HOT_PATH_ACTIVE below); and (2)
    # COMPOSITION MODEL -- "content unchanged" was wrongly treated as "skip
    # the overlay", which hid the menu and the race HUD (both persistent,
    # near-static LFB content) even though the animated GL background
    # underneath was visible proof the compositor itself was alive. G106
    # keeps a PERSISTENT overlay-tile mask, separate from the per-swap
    # changed-tile set, so persistent UI redraws every GL-active frame
    # regardless of whether its own bytes changed that particular swap --
    # see glide2x_ign.cpp's own G106 block comment for the full real-
    # hardware evidence and the exact epoch-reset rules. Same
    # G104_FRAME_AWARE_LFB_COMPOSITOR compile path/define as G104/G105.
    #
    # iteration-G107: the G106 VPK (icon0.png added speculatively, after
    # the fact, as an unconfirmed guess at SCE error 0x80105A03) was
    # rejected outright by real hardware and is NOT reused. G107 returns
    # to the exact minimal packaging recipe that G101-G105 all installed
    # successfully with (param.sfo + eboot.bin, nothing else -- see the
    # vita_create_vpk call below, which no longer adds any FILE). It also
    # moves to a NEW TITLEID (IGNVITA04, see below) so this install can
    # never collide with whatever IGNVITA03 state is already on the
    # device. Confirmed before making this change: no C++ source in this
    # project references IGNVITA03/IGNVITA04 at all -- every game-data/log
    # path is the hardcoded literal "ux0:data/ignition/..." (see
    # source/runtime/ignition_paths.h's own kDataRoot and log.cpp's own
    # kLogPath/kCriticalLogPath/kSessionIdPath), so changing the packaging
    # TITLEID has zero effect on where the guest's data lives.
    # iteration-G108: G107's real-hardware run was the first genuinely
    # playable session (menu, race, HUD, ~10-13 FPS, ~39s of play) but
    # ended in a DELIBERATE wrapper shutdown (request_halt on a single
    # non-finite triangle, not a Vita crash) and still showed the G106
    # overlay model's own over-coverage problem (~252/1000 tiles, ~25% of
    # the screen, opaque 16x16 blocks). G108 replaces request_halt for
    # that one recoverable case with DROP_TRIANGLE_AND_CONTINUE, and
    # replaces the overlay's union-of-ever-changed-tiles model with a
    # current-vs-epoch-base diff at a finer 8x8 granularity -- see
    # glide2x_ign.cpp's own G108 block comment for the full rationale.
    # New TITLEID again (IGNVITA05), per this iteration's own explicit
    # instruction, so this install cannot collide with whatever IGNVITA03/
    # IGNVITA04 state is already on the device.
    # iteration-G109: G108's real-hardware run kept the stability fix (no
    # more spontaneous LiveArea exit) but regressed performance severely,
    # 13-14 FPS down to 3-4 FPS, root-caused to g104_classify_and_decide's
    # own snapshot helper losing G107's zero-copy hostptr fast path (an
    # unconditional extra 512000-byte memcpy) plus a bit-serial CRC32 call
    # silently folded into the same timed "snapshot" window -- see
    # glide2x_ign.cpp's own G109 comment on g104_snapshot_lfb and
    # g_g104LastUploadedLfb for the confirmed fix (zero-copy restored,
    # CRC32 replaced with a direct memcmp). Also adds bounded hysteresis
    # around epoch-base capture and broad-change re-basing, aimed at the
    # still-reported stuck/pegged menu images, without touching the
    # structural contract, the fast-path architecture, or the halt policy.
    # iteration-G110: G109's real-hardware run confirmed the snapshot fix
    # worked in isolation (avg_snapshot_us dropped from 65501.28 to 24.26)
    # but the overall experience stayed "totally unplayable" with no HUD at
    # all in race and the menu still showing stuck images -- the same
    # symptoms G108 already produced. The user's own explicit, evidence-
    # based conclusion: every compositor change made AFTER G107 (G108's
    # current-vs-epoch-base model, 8x8 tiles, and G109's own stabilization/
    # cooldown heuristics on top of it) made things worse, not better, and
    # G107 (the union-of-ever-changed-tiles model, 16x16 tiles, CRC-based
    # upload check) remains the best-performing version actually tested on
    # hardware. G110 is a full, deliberate ROLLBACK of glide2x_ign.cpp's
    # entire G104_FRAME_AWARE_LFB_COMPOSITOR block to that exact G107 logic
    # (reconstructed from this project's own prior verbatim source reads,
    # since no git repository or other backup of G107's source exists --
    # only its compiled build-3dfx-g107 output does). The ONLY functional
    # change kept on top of G107 is G108's own safe-drop fix (non-finite
    # required vertex fields are discarded via DROP_TRIANGLE_AND_CONTINUE
    # instead of request_halt, in guDrawTriangleWithClip_impl/
    # grDrawPoint_impl) -- that code lives in a structurally separate part
    # of the file (near gu_vertex_required_finite) and was never part of
    # the compositor revert.
    # iteration-G111: G110's real-hardware run confirmed the visual/stability
    # rollback worked (menu state matches G107, game keeps moving behind the
    # menu, race entry works, no crash/halt/LiveArea exit) but FPS stayed at
    # G108/G109 levels (~3-4fps) instead of recovering G107's ~13-14fps, even
    # though a forensic byte-for-byte .text comparison against the real
    # G107 build artifacts (build-3dfx-g107/) proved G110's compositor and
    # every untouched object file are code-identical to G107 -- so the FPS
    # gap was never in this project's own source. The user's own explicit
    # hint, confirmed in external/WinVita/third_party/box86-dynarec: markers
    # 11 ("[RUN27]") and 14 ("[RUN29]") in dynarec_arm_66.c's case 0x9C/0x9D
    # (16-bit PUSHF/POPF) had gone UNCONDITIONAL since that file's own
    # iteration-31 -- a raw fopen/fprintf/fclose on literally EVERY PUSHFW/
    # POPFW in the entire translated game, bypassing this project's own
    # logger entirely (invisible to log_get_perf_stats' overhead accounting)
    # and present identically in every prior G-iteration build, since none
    # before G111 had authorization to touch external/WinVita. Removed those
    # two call sites plus six narrower, EIP-gated "[RUN25]"/"[RUN27]"/
    # "[RUN29]" markers in dynarec_arm_00.c (markers 3,4,5,10,13,15); left
    # the out-of-scope RUN26/RUN28/RUN30/IDLE34/IDLE35 diagnostics and the
    # functional REP STOSD override (marker 12's own logic, not its passive
    # marker) completely untouched, per the user's own explicit scope limit.
    # The real dynarec fixes these sat beside (MMU_PUSH16/MMU_POP16) are
    # unmodified.
    # iteration-G112: G111's real-hardware run confirmed the hot-trace
    # removal recovered ~3-4fps -> ~6-8fps, a full race completed (first
    # time ever on this port), safe-drop discarded thousands of primitives
    # without a halt, but FPS stayed well below G107's 13-14fps, and a NEW
    # defect appeared: the HUD progressively duplicates during a long race
    # (confirmed by the user's own hardware photo -- stale POS/lap-time/
    # results text all visible simultaneously). Root-caused BOTH from the
    # user's own real [G106-PERF] log, not guessed: (1) a ~44ms/swap
    # UNTIMED gap inside g104_classify_and_decide's own measured span,
    # tracked down by direct code reading to `g104_crc32()` -- a bit-serial
    # CRC32 (8 shift/xor ops/byte) run over the full 512000-byte LFB every
    # swap, in the gap BETWEEN the snapshot and diff timers, invisible to
    # [G106-PERF] yet fully counted in avg_compositor_total_us (~50.2ms
    # measured, ~5.8ms of it ever attributed) -- removed entirely, replaced
    # with the already-computed contentChangedThisFrame bool, this
    # iteration's own single highest-confidence fix; (2) the persistent
    # union-of-ever-changed-tiles mask (unchanged since G106/G107) has its
    # own failure mode, distinct from G108/G109's epoch-base bug: a tile
    # that EVER held HUD content never leaves the mask except on a broad-
    # coverage reset, so moved/cleared HUD elements keep being redrawn from
    # whatever the (now-current, since the texture is always fully re-
    # uploaded) LFB content is at that tile -- fixed with a content-based
    # retire/activate rule using the game's own chromakey value as the
    # "this pixel is background" test (g112_chromakey_key_rgb565(), a
    # documented, hardware-UNCONFIRMED hypothesis that fails safe to G111's
    # current behavior if wrong, never toward G108/G109's HUD-disappearing
    # class of bug), computed in the SAME per-pixel pass as change
    # detection -- no current-vs-epoch-base, no second 512000-byte pass.
    # Also eliminated a second, independent full-buffer re-read inside
    # lfb_upload_texture (its own std::vector allocated fresh every call)
    # by reusing the compositor's own already-read buffer. Partial/sub-
    # rectangle texture upload was investigated but deliberately DEFERRED
    # (see lfb_upload_texture's own comment) pending this iteration's own
    # new instrumentation actually showing the full upload as the
    # remaining bottleneck on real hardware -- "solo después de medirlo"
    # per this iteration's own explicit sequencing. New TITLEID again
    # (IGNVITA09), so this install cannot collide with IGNVITA03 through
    # IGNVITA08 already on the device.
    # iteration-G113: G112's real-hardware run was the most stable yet (full
    # races, a second race on another track, ~9-11fps, compositor ~9-10ms)
    # but the HUD still duplicated progressively and a small stale image
    # stuck on screen at the start of the second race. G112 retired tiles
    # (2892) yet the stale pixels were INSIDE the shadow LFB the game keeps
    # writing into (it persists; on a Voodoo the per-frame 3D pass rewrites
    # the framebuffer and the game redraws the whole HUD every frame -- ~42
    # lock/unlock pairs per race frame in the log). G113 clears that shadow
    # to the key once per frame in race/results GL scenes (swap end, plus
    # grBufferClear when it arrives before the first lock), derives the mask
    # from the rebuilt layer, latches the LFB chromakey at the write lock
    # (G112's 0x1083/0x0000 flip was the live 3D key changing between draw
    # passes, not a substitution by grChromakeyMode), and resets the overlay
    # at every scene boundary. New TITLEID (IGNVITA10).
    # iteration-G114: G113 removed the HUD duplication but (1) painted black
    # rectangles: its key latched only at write locks with pixelPipeline&&
    # chromakey-enabled, and the log showed every HUD lock has pipe=0/en=0
    # (lfb_key_valid=NO), so the layer was cleared to 0x0000 and the opaque
    # tile overlay drew it; (2) lost ~3-5fps to a full 512000-byte clear per
    # frame (plus a full prev copy per snapshot). G114: dedicated overlay key
    # learned from grChromakeyValue (zero never accepted, confirmed by the value
    # preceding the write locks, 0x1082/0x1083 bit-0 tolerance), never clearing
    # without it; per-pixel transparency by drawing spans of non-key pixels
    # (no texture-format/blend change); sparse clear of the written tiles only;
    # per-tile prev update; faster key-row scan. New TITLEID (IGNVITA11).
    # iteration-G115: G114's picture was right (HUD clean, transparent, results
    # clean, post-race menu clean) but its per-pixel transparency drew ~1,950
    # span quads (~11,700 vertices) per frame. G115 keeps the clear/key/mask
    # contract and replaces the spans by ONE QUAD PER ACTIVE TILE (6 vertices,
    # ~1,500 per frame) sampling a dedicated RGBA5551 overlay texture (alpha 0 =
    # overlay key, 1 = everything else incl. legit black; vitaGL copies
    # GL_RGBA/GL_UNSIGNED_SHORT_5_5_5_1 without conversion) with GL_ALPHA_TEST;
    # conversion is limited to active changed/new tiles inside the compositor
    # pass. The key is available early (previous-session cache, else the value
    # observed in the G114 log, always provisional until this session's lock
    # samples confirm or replace it) and scenes are managed whenever a key is
    # available, so the initial menu gets the same treatment as the post-race
    # one. Adds a host-vs-guest frame breakdown. New TITLEID (IGNVITA12).
    # iteration-G116: input-only phase (controls first pass); visible name and
    # VPK file name only -- Title ID deliberately unchanged (IGNVITA12).
    # iteration-G117: controls menu/exit fix (input + exit path only); same
    # rule -- visible name and VPK file name only, Title ID unchanged.
    # iteration-G118: car-selection preview investigation build (diagnostic
    # probe only, no behaviour change); visible name and VPK file name only.
    # iteration-G119: car-selection visibility fix (selection node only); visible
    # name and VPK file name only.
    # iteration-G120: car-selection ordering probe + one A/B diagnostic (selection
    # node only); visible name and VPK file name only.
    # iteration-G121: car-selection visibility probe (read-only, selection node
    # only; G119/G120 overlay experiments removed); visible name and VPK name only.
    # iteration-G122: car-selection flat-colour audit + one causal colour test
    # (selection node, caller 0x00450DEF only); visible name and VPK name only.
    # iteration-G125: car-selection carousel replayed in its real state after the LFB compositor
    # (clean build, no probes); visible name and VPK name only.
    # iteration-G126: language-selection screen (initial English, no accumulated highlight rings);
    # visible name and VPK name only.
    # iteration-G127: localized flag-rectangle clear (no whole-shadow fill) + compositor exit-state log;
    # visible name and VPK name only.
    # iteration-G128: flag rectangle corrected to the real 61x53 sprite size; visible name and VPK name only.
    # iteration-G129: first DirectSound sound-effects output (22050 Hz mono mixed by the game itself, played
    # through the existing engine + Vita sceAudioOut sink); MCI/OGG music NOT implemented; graphics untouched;
    # visible name and VPK name only (Title ID unchanged).
    # iteration-G130: DirectSound timing (audio thread priority, stale stream-ring content played as silence,
    # real-gap metric and bounded timing/feed/thread summaries); graphics, input and MCI untouched; visible
    # name and VPK name only (Title ID unchanged).
    # iteration-G131: reconciliation build = G128 graphics/input/kernel modules (compared byte for byte at
    # configure time and at boot) + G130 DirectSound timing; stream-table integrity canaries, overlay-key cache
    # reconciliation and an optional on-device control file; music (MCI/CD/OGG) still pending; visible name and
    # VPK name only (Title ID unchanged).
    # iteration-G132: music first pass: mciSendStringA -> virtual CD-audio (8-track TOC) -> Music/TrackNN.ogg
    # (stb_vorbis, streamed) -> a SECOND audio port (MAIN, 48 kHz) with its own thread. DirectSound, graphics,
    # input, save and exit untouched (frozen modules compared with the G131 snapshot at configure time and at
    # boot). Visible name and VPK name only (Title ID unchanged).
    # iteration-G134: recovery build. Starts again from G132_COMPLETE_STABLE_BASE (G133's buffered logger engine is NOT used):
    # the simple open/append/close logger behind a short static allowlist, logs in ux0:data/ignition/logs/ (created by the first
    # statement of main()), one previous generation, and explicit IGN_CP checkpoints. Every frozen module is unchanged. Visible name
    # and VPK name only (Title ID unchanged).
    # iteration-G135: DIAGNOSTIC build for the blurred in-race pause menu. Exactly the proven G134 tree plus (1) a read-only pause-menu probe
    # in glide2x_ign.cpp (3 log tags, bounded, no rendering change) and (2) the matching allowlist rows in diagnostics/log.cpp. Visible name
    # and VPK name only (Title ID unchanged).
    # iteration-G136: G135 + a PASSIVE, structurally gated pause probe (a real pause = the guest's own condition; a menu-like picture is only counted),
    # no periodic GL fingerprint, no pixel-only episodes, invalid-triangle correlation. No rendering change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G137: G136 + track-geometry integrity: the three display-list polygon helpers never write the alpha field of their stack vertices, so a NaN
    # leftover there no longer drops the triangle while nothing can read the source alpha (every other case keeps G108); bounded burst diagnostics.
    # Visible name and VPK name only (Title ID unchanged).
    # iteration-G138: performance baseline + first safe fast path. IGN_DIAGNOSTICS=LIGHT (default, the normal VPK) removes the per-call / per-triangle diagnostics from the hot paths;
    # IGN_DIAGNOSTICS=FULL is the G137 behaviour (all historical diagnostics, G137 ring/bursts, per-triangle timers) plus the same G138 frame-window telemetry, for a same-session baseline.
    # iteration-G139: geometry submission fast path (GL state shadow, draw session, consecutive-triangle batching) in the LIGHT build; the FULL build keeps the individual G138-FULL path and verifies the
    # shadow against real GL every race window. Visible name and VPK name only (Title ID unchanged).
    # iteration-G140: G139 + language persistence for the session (the confirmed language is given back where the original code reloads it from ign_3dfx.btz; the English start is
    # forced only on the first selector entry of a process). No rendering / audio / pacing change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G141: G140 + a passive, bounded comparison of the race HUD state of the first race against the second ([G141-HUD-*] lines, at most 20 per race) and a shadow-vs-GL check just before the
    # first HUD frame of every race. No rendering change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G142: G140 behaviour + cheap per-frame accumulators of the HUD overlay, transition / spike / pause-marker lines ([G142-*]), an evidence-gated accumulation guard and the track identification
    # (the heavy G141 per-frame capture is removed). Visible name and VPK name only (Title ID unchanged).
    # iteration-G143: G140 behaviour + HUD region lifetime: a mode machine (race / pause / other) with one boundary clear per change, the G142 recovery clear made self-verified per region (with a
    # BOTTOM_RIGHT scrub as fallback), a palette-proven key guard, per-region instrument ([G143-*]; no GPU read-back) and the track identification of G142. Visible name and VPK name only (Title ID unchanged).
    # iteration-G144: G143 behaviour + the incremental LFB compositor fast path for the race (fast scan, a ring of overlay textures written in place, only the tiles that changed; the G143 path stays as the
    # fallback whenever the contract does not hold) + the cheap windowed scene-profile counters prepared for G145. No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G145: G144 behaviour + the explicit race lifecycle (the incremental compositor is armed in EVERY race, not only the first), trivial rejection of triangles completely outside the scissor (decided before
    # anything is decoded), a fast validated copy of the guest vertices, windowed geometry counters; the heavy G144/G145 profiler is FULL-only. No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G146: G145 behaviour + the geometry fast paths made to work: first-failing-condition eligibility accounting, a viewport predicate and a post-decode rejection that need neither the memory fast path nor the scissor
    # record, a three-tier guest read (host span, bulk read, dword path), frame-time percentiles per window, an optional control file to measure the paths on / off. No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G148 (clean copy of G146, nothing of the G147 tree): the deferred STATE PACKET (12 families, a dirty mask, per-family counters), lean state handlers (no clock read per call), blend / scissor applied INSIDE the open draw session in the race,
    # and a batch key without the logical fields that never reach GL state. No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G149 (clean copy of G148): the cost profiler of the guest->host boundary (gap pairs by call site, triangle segments, crossing census, swap stages, race-frame accounting that the pauses no longer pollute),
    # a one-entry texture dimension cache and the compositor scan on 128-bit vectors (bit-identical, self-checked, falls back to the G148 scan). No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G150 (clean copy of G149): the LIGHT build compiles the G149 profilers out, the memory-card log is written in one batch per swap (11-20 ms per open/close, 13 lines per window), the shim-side guest reads no longer pay getProtection
    # (WX86_MEMGUARD is not armed in LIGHT), the texture dimensions are recorded at guTexSource, and the guest run is resumed when its 2^31-1 block budget ends instead of ending the game. No visual change. Visible name and VPK name only (Title ID unchanged).
    # iteration-G151 (clean copy of G150): the two native HUD loops that the G150 real run proved (hooks=0x3: 14,004 frames, 32.1 FPS weighted, no visual difference) are ON by default, with hook-hud-draw=off / hook-hud-sprite=off as
    # emergency switches in ux0:data/ignition/g146_geo.txt; release telemetry (race windows written every fourth window and when slow, one session summary line). No new optimisation. Visible name and VPK name only (Title ID unchanged).
    # iteration-G152 (clean copy of G151): the Championship race is recognised as a race (the car-selection node stays current while the race loop runs; the carousel flag is held off while the game's race loop word is 1), so it takes the same
    # fast paths as any race; evidence lines [G152-CHAMP-*]. No new optimisation. Visible name and VPK name only (Title ID unchanged).
    # iteration-G153 (clean copy of G152): the Glide chroma key is applied to the 3D textured draws (smoke / particles drew their keyed texels as opaque black): keyed texture variants with alpha, alpha test in the draw session.
    # No new optimisation. Visible name and VPK name only (Title ID unchanged).
    # iteration-G154 (clean copy of G153): the G153 chroma key resolved from the palette in effect: one mask per (palette, key), an O(1) decision per texture, variants only for textures that have a keyed texel and rebuilt only when the mask
    # content or the texture content really changes; aggregated counters instead of probe lines. Same pixels as G153. Visible name and VPK name only (Title ID unchanged).
    # iteration-G155 (clean copy of G154): the keyed resolution memoised per change, variants selected by (used AND keyed) indices with a bounded pool, the alpha test and the texture environment kept across the draws that cannot
    # tell the difference (fewer GL state calls per keyed run), no clock in the triangle path. Same pixels as G154. Visible name and VPK name only (Title ID unchanged).
    # iteration-G156 (clean copy of G155): the display-list cull (win32/g156_cull.h): triangle commands wholly beyond the GL viewport are skipped by the game's own dispatcher before the guest builds their Glide calls. Same pixels as G155.
    # Visible name and VPK name only (Title ID unchanged).
    # iteration-G157 (clean copy of G156): the split-screen option of the multiplayer menu is disabled for v1.0 (win32/g157_split.h): its confirmation is swallowed in the menu input proc, nothing else changes.
    # Visible name and VPK name only (Title ID unchanged).
    # iteration-G158 (clean copy of G157; "ONLINE_BLOCK_FIX" supersedes the VPK named DISABLE_ONLINE): the network-game option of the multiplayer menu is disabled for v1.0 (win32/g158_online.h): its confirmation is swallowed at the same menu seam,
    # Visible name and VPK name only (Title ID unchanged).
    # iteration-G159 (clean copy of G158): the player name (the option of the player options menu) is entered with the Vita's system keyboard (win32/g159_name.h; links the Ime and Sysmodule stubs). Visible name and VPK name only (Title ID unchanged).
    # iteration-G160 (clean copy of G159): the default clock profile (win32/g160_clocks.h, runtime/g160_clock_runtime.cpp): the port itself asks, once, for at least ARM 444 / BUS 222 / GPU 222 / XBAR 166 MHz (ScePower_stub, already linked) and restores
    # what it changed at the normal end. Visible name and VPK name only (Title ID unchanged).
    # iteration-G161 = Ignition Vita v1.0 RC1 (clean copy of G160) with exactly two independent changes: (1) the final LiveArea resources (resources/livearea: icon0.png, bg0.png, startup.png + template.xml, packed into the VPK) and (2) a 30 FPS
    # presentation limiter (win32/g161_pacing.h, runtime/g161_pacer.cpp; `fps-limit=` in the existing control file). The visible bubble name is "Ignition Vita", the version 01.00; the Title ID is unchanged.
    if(IGN_DIAGNOSTICS STREQUAL "FULL")
      set(VITA_APP_NAME  "Ignition Vita (diag)")
      set(VITA_VPKNAME   "IgnitionVita_v1.0_DIAGNOSTIC_FULL")
    else()
      set(VITA_APP_NAME  "Ignition Vita")
      set(VITA_VPKNAME   "IgnitionVita_v1.0")   # the LiveArea PNGs are the 8-bit indexed (colour type 3) set; the console refused the 24-bit RGB set with 0x8010113D
    endif()
  endif()
  if(NOT G101_LFB_AB_PROBE AND NOT G84_CLEAR_PROBE AND NOT G103_ZERO_DIRTY_LFB_GUARD_BUILD)
    set(VITA_TITLEID   "IGNVITA12")
  else()
    set(VITA_TITLEID   "IGNVITA03")
  endif()
else()
  set(VITA_APP_NAME    "IgnitionVita (diag)")
  set(VITA_TITLEID     "IGNVITA01")
  set(VITA_VPKNAME     "ignitionvita-diag")
endif()
set(VITA_VERSION     "01.00")   # iteration-G161: Ignition Vita v1.0 RC1
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# --- WinVita dependency ----------------------------------------------------
# Vendored as plain source (not a submodule -- see README) under
# external/WinVita. It builds itself independently via its own build.sh
# (TARGET=vita ./build.sh -> build-vita/libwinx86_vita.a); this project only
# links the resulting archive and includes its public headers under src/.
set(WINVITA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/WinVita" CACHE PATH "WinVita checkout")
set(WINVITA_LIB "${WINVITA_DIR}/build-vita/libwinx86_vita.a")
# The engine archive is produced by external/WinVita/build.sh (see docs/BUILDING.md); the Ignition-specific changes to the vendored engine
# (DirectSound emulation and Vita audio output) are already part of external/WinVita/src.
if(NOT EXISTS "${WINVITA_LIB}")
  message(FATAL_ERROR
    "libwinx86_vita.a not found at ${WINVITA_LIB}.\n"
    "Build WinVita itself first:\n"
    "  export PATH=$ENV{VITASDK}/bin:$PATH\n"
    "  cd ${WINVITA_DIR} && TARGET=vita ./build.sh\n")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must match the flags libwinx86_vita.a itself was built with (WinVita's own
# build.sh): mixing float ABIs across objects in the final link is refused
# by the linker, not just a performance mismatch.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2 -Wall -Wl,-q -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard")

add_executable(ignitionvita
    source/main.cpp
    source/diagnostics/log.cpp
    source/diagnostics/phase_state.cpp
    source/diagnostics/call_trace.cpp
    source/runtime/ignition_paths.cpp
    source/runtime/ignition_memlayout.cpp
    source/runtime/ignition_guest_alloc.cpp
    source/runtime/ignition_boot.cpp
    source/runtime/g160_clock_runtime.cpp   # iteration-G160: the default clock profile
    source/runtime/g161_pacer.cpp           # iteration-G161 (v1.0 RC1): the 30 FPS presentation limiter
    source/win32/kernel32_ign.cpp
    source/win32/winmm_ign.cpp
    source/win32/user32_window.cpp
    source/win32/ddraw.cpp
    source/win32/dinput.cpp
    source/win32/glide2x_ign.cpp
    # iteration-G132: virtual CD-audio + OGG streaming (music/), isolated; stb_vorbis is included from third_party/ by ogg_source.cpp
    source/music/cd_toc.cpp
    source/music/mci_cd.cpp
    source/music/music_player.cpp
    source/music/ogg_source.cpp
    source/music/music_service.cpp
    source/music/music_vita.cpp
)

target_include_directories(ignitionvita PRIVATE
    source
    ${WINVITA_DIR}/src
)
# iteration-G132: ogg_source.cpp includes the unmodified third-party stb_vorbis.c; its own warnings are not ours to fix.
set_source_files_properties(source/music/ogg_source.cpp PROPERTIES COMPILE_FLAGS "-w")

# iteration-G131: proof, inside the build itself, that the graphics / input / kernel modules are the G128 ones.
# SHA-256 (first 16 hex) of each file as compiled into THIS build, next to the same file in the G128 snapshot
# (when the snapshot is present next to the sources); ignition_boot.cpp compares and logs them once at boot.
# (G134: main.cpp is no longer in this list -- it now creates the logs folder and writes the first checkpoints.)
# (G135: glide2x_ign.cpp is no longer in this list -- it carries the pause probe; its header and the overlay header stay frozen.)
set(G131_VIS_FILES
    source/win32/glide2x_ign.h source/win32/g113_overlay.h
    source/win32/ddraw.cpp source/win32/ddraw.h
    source/win32/dinput.cpp source/win32/dinput.h
    source/win32/user32_window.cpp source/win32/user32_window.h
    source/win32/kernel32_ign.cpp source/win32/kernel32_ign.h
    source/diagnostics/phase_state.cpp)
# (G138: call_trace.cpp is no longer in this list -- in the LIGHT build it arms nothing; the FULL build keeps the G137 code.)
set(G131_VIS_BASE "${CMAKE_CURRENT_SOURCE_DIR}/reference_tree")   # optional reference copy of the frozen modules; absent in the public repository -> reported as "unavailable"
set(G131_VIS_STR "")
foreach(f ${G131_VIS_FILES})
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${f}" g131_now)
  string(SUBSTRING "${g131_now}" 0 16 g131_now16)
  if(EXISTS "${G131_VIS_BASE}/${f}")
    file(SHA256 "${G131_VIS_BASE}/${f}" g131_ref)
    string(SUBSTRING "${g131_ref}" 0 16 g131_ref16)
  else()
    set(g131_ref16 "unavailable")
  endif()
  string(APPEND G131_VIS_STR "${f}=${g131_now16}/${g131_ref16}|")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
endforeach()
# iteration-G132: the DirectSound engine is frozen too (vendored sources; the G131 snapshot keeps them under external_WinVita_modified/)
foreach(pair "runtime/ds_emul.cpp|ds_emul.cpp" "runtime/ds_emul.h|ds_emul.h" "runtime/audio_sink.h|audio_sink.h" "platform/vita_audio.cpp|vita_audio.cpp")
  string(REPLACE "|" ";" pairl "${pair}")
  list(GET pairl 0 g132_rel)
  list(GET pairl 1 g132_snap)
  file(SHA256 "${WINVITA_DIR}/src/${g132_rel}" g131_now)
  string(SUBSTRING "${g131_now}" 0 16 g131_now16)
  if(EXISTS "${G131_VIS_BASE}/external_WinVita_modified/${g132_snap}")
    file(SHA256 "${G131_VIS_BASE}/external_WinVita_modified/${g132_snap}" g131_ref)
    string(SUBSTRING "${g131_ref}" 0 16 g131_ref16)
  else()
    set(g131_ref16 "unavailable")
  endif()
  string(APPEND G131_VIS_STR "external/WinVita/src/${g132_rel}=${g131_now16}/${g131_ref16}|")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WINVITA_DIR}/src/${g132_rel}")
endforeach()
target_compile_definitions(ignitionvita PRIVATE "G131_VIS_HASHES=\"${G131_VIS_STR}\"")
# iteration-G138: exactly one diagnostics-level definition, for every translation unit of the game
if(IGN_DIAGNOSTICS STREQUAL "FULL")
  target_compile_definitions(ignitionvita PRIVATE IGN_DIAGNOSTICS_FULL=1)
else()
  target_compile_definitions(ignitionvita PRIVATE IGN_DIAGNOSTICS_LIGHT=1)
endif()

if(IGNITION_RENDERER_3DFX)
  target_compile_definitions(ignitionvita PRIVATE IGNITION_RENDERER_3DFX=1)
  if(G101_LFB_AB_PROBE)
    target_compile_definitions(ignitionvita PRIVATE G101_LFB_AB_PROBE=1)
  elseif(G103_ZERO_DIRTY_LFB_GUARD_BUILD)
    # Standalone G103 build: its own hot-path analyzer
    # (g103_lfb_lock/unlock/should_suppress_lfb and friends) is ACTIVE here
    # -- G103_HOT_PATH_ACTIVE gates exactly those functions and their call
    # sites (see glide2x_ign.cpp), separately from G103_ZERO_DIRTY_LFB_GUARD
    # itself, which ALSO still gates the much older glReadPixels-based
    # G80/G81/G84/G92/G93 diagnostics that stay disabled in every build
    # from G104 onward regardless of which hot path is active.
    target_compile_definitions(ignitionvita PRIVATE G103_ZERO_DIRTY_LFB_GUARD=1 G103_HOT_PATH_ACTIVE=1)
  else()
    # iteration-G106: G103_ZERO_DIRTY_LFB_GUARD is still defined here
    # (disables the legacy glReadPixels-based diagnostics, as before) but
    # G103_HOT_PATH_ACTIVE is deliberately NOT defined -- G103's own
    # lock/unlock/should_suppress_lfb analyzer no longer runs in this
    # build at all (see glide2x_ign.cpp's own grLfbLock_impl/
    # grLfbUnlock_impl/grBufferSwap_impl, each now gated on
    # G103_HOT_PATH_ACTIVE specifically, not G103_ZERO_DIRTY_LFB_GUARD) --
    # this was G106's own required fix for G105's measured CPU cost
    # (G103's comparator alone averaged ~2.66ms/call x 33-60 calls/frame).
    target_compile_definitions(ignitionvita PRIVATE G103_ZERO_DIRTY_LFB_GUARD=1 G104_FRAME_AWARE_LFB_COMPOSITOR=1)
  endif()
endif()

# iteration-G4B/G4D: standalone vitaGL init/clear/swap diagnostic (Parte C
# of the G4B request) that replaces the guest entirely for this build --
# see main.cpp/glide2x_ign.cpp. Was ON by default through G4C while
# vitaGL's context validity was unresolved; G4C's real-hardware run proved
# it valid (context_valid=YES, GL_VERSION/VENDOR/RENDERER all present,
# glGetError=0), so G4D defaults this OFF and runs the guest with vitaGL
# pre-initialized once in main() instead (see glide2x_g4d_preinit_vitagl).
# Kept as a toggle for future isolated vitaGL diagnostics. Only meaningful
# when IGNITION_RENDERER_3DFX is also ON; never affects the stable DDraw
# build either way.
option(G4B_VITAGL_SELFTEST "Run a standalone vitaGL init/clear/swap selftest instead of Ignition (3dfx branch only)" OFF)
if(IGNITION_RENDERER_3DFX AND G4B_VITAGL_SELFTEST)
  target_compile_definitions(ignitionvita PRIVATE G4B_VITAGL_SELFTEST=1)
endif()

# iteration-G20C, Experiment A: makes grSstIdle_impl a complete GL no-op
# (no glFinish/glFlush/any GL call at all) to isolate "glFinish's own side
# effect" from "trap/bridge return interaction" as the cause of the
# post-grSstIdle crash G20/G20B observed. Diagnostic-only, default OFF --
# the normal/shipping build keeps calling glFinish (G20's own finding,
# unchanged).
option(G20C_IDLE_NOOP "Diagnostic: make grSstIdle a complete no-op, no GL sync call at all (3dfx branch only, G20C Experiment A)" OFF)
if(IGNITION_RENDERER_3DFX AND G20C_IDLE_NOOP)
  target_compile_definitions(ignitionvita PRIVATE G20C_IDLE_NOOP=1)
endif()

# iteration-G20C, Experiment B: NOPs out ONLY Ignition's own single CALL
# grSstIdle instruction at guest VA 0x00458A30 (confirmed byte-exact by
# disassembly across G14/G15/G20/G20B) so the guest falls straight through
# to 0x00458A35 without ever entering the grSstIdle trap at all -- isolates
# "trap/bridge return interaction specific to grSstIdle" from "guest code
# starting at 0x00458A35" as the crash cause. Does NOT touch grSstIdle's
# own registration, Bridge's trap_epilogue, or any other CALL instruction.
# Diagnostic-only, default OFF -- purely an A/B probe, never a real fix.
option(G20C_BYPASS_IDLE_CALLSITE "Diagnostic: NOP out Ignition's own CALL grSstIdle at guest VA 0x00458A30 ONLY (3dfx branch only, G20C Experiment B)" OFF)
if(IGNITION_RENDERER_3DFX AND G20C_BYPASS_IDLE_CALLSITE)
  target_compile_definitions(ignitionvita PRIVATE G20C_BYPASS_IDLE_CALLSITE=1)
endif()

# iteration-G84: wires the G84_CLEAR_PROBE option (declared above, near
# VITA_VPKNAME) into the actual compile define glide2x_ign.cpp checks.
if(IGNITION_RENDERER_3DFX AND G84_CLEAR_PROBE)
  target_compile_definitions(ignitionvita PRIVATE G84_CLEAR_PROBE=1)
endif()

# iteration-G4: vitaGL + its own link dependencies, 3dfx-branch only. No
# .pc/pkg-config file ships for vitaGL (checked this iteration), so this
# list was determined empirically: `arm-vita-eabi-nm -u libvitaGL.a` was
# inspected for undefined symbols, grouped by Sce module prefix
# (sceGxm/sceDisplay/sceCommonDialog/sceShaccCg/sceRtc), and each was
# matched against the stub .a actually present in the vitasdk lib dir --
# not copied from a sample project. Guarded so the stable DDraw build's
# link inputs stay exactly what they were before this iteration.
if(IGNITION_RENDERER_3DFX)
  set(IGNITION_VITAGL_LIBS
      vitaGL
      vitashark
      SceGxm_stub
      SceDisplay_stub
      SceCommonDialog_stub
      SceIme_stub
      SceSysmodule_stub
      SceShaccCg_stub
      SceShaccCgExt
      SceRtc_stub
      SceKernelDmacMgr_stub
      taihen_stub
      mathneon
  )
endif()

target_link_libraries(ignitionvita
    ${WINVITA_LIB}
    ${IGNITION_VITAGL_LIBS}
    SceLibKernel_stub
    SceKernelThreadMgr_stub
    SceKernelModulemgr_stub
    SceSysmem_stub
    SceIofilemgr_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceProcessmgr_stub
    # iteration-G28: sceCtrl* (win32/dinput.cpp's Vita-physical-input ->
    # DirectInput-buffered-keyboard bridge). dinput.cpp compiles into BOTH
    # the 3dfx and stable DDraw targets, so this is a common link input,
    # not gated by IGNITION_RENDERER_3DFX -- the stable DDraw build already
    # registers DirectInput shims defensively (see dinput.cpp's own header
    # comment) even though Ign_d3d.exe's import table has no DINPUT.dll
    # entry to link them to, so linking SceCtrl_stub here is harmless and
    # keeps both targets building from the exact same source list.
    SceCtrl_stub
    # iteration-G138: ScePower_stub, for the clock query in the telemetry start line ([G138-PERF-BASELINE]); iteration-G160: also for the four scePower*ClockFrequency setters, called only from runtime/g160_clock_runtime.cpp (once at start-up, once at the end).
    ScePower_stub
    pthread
    stdc++
    m
)

# iteration-G106-fix (REVERTED by G107): the G106 VPK, with a speculative
# sce_sys/icon0.png added after the fact as an unconfirmed guess at SCE
# error 0x80105A03, was REJECTED outright by real hardware -- the icon
# fix was not merely unproven, it made things worse or at best didn't
# help. G107 returns to the exact minimal recipe G101-G105 all installed
# successfully with: param.sfo + eboot.bin, nothing else. Do not add any
# FILE (icon/LiveArea) here without new real-hardware evidence that it
# specifically helps -- the previous attempt is the counter-evidence.
vita_create_self(eboot.bin ignitionvita UNSAFE)
# iteration-G161 (v1.0 RC1): the final LiveArea resources are packed byte for byte (the PNGs are copied unchanged from the prepared set; template.xml references bg0.png and startup.png).
# NOTE for the reader of the history above: the G106 VPK that was rejected by the console carried a speculative icon0.png added as a guess at an unrelated error; these files are the user's final set, and this is the first build to carry the
# LiveArea tree -- if an install were refused, the first thing to try is the same VPK without the FILE lines below.
if(IGNITION_RENDERER_3DFX)
  set(IGN_LIVEAREA_DIR "${CMAKE_SOURCE_DIR}/sce_sys")
  vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME}
      FILE ${IGN_LIVEAREA_DIR}/icon0.png sce_sys/icon0.png
      FILE ${IGN_LIVEAREA_DIR}/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
      FILE ${IGN_LIVEAREA_DIR}/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
      FILE ${IGN_LIVEAREA_DIR}/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
else()
  vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME}
  )
endif()
