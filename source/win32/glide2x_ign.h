// SPDX-License-Identifier: LGPL-2.1-only
//
// LICENSE OF THIS FILE: GNU Lesser General Public License, version 2.1 only
// (full text: licenses/LGPL-2.1.txt), like its companion glide2x_ign.cpp, which
// carries the full notice about the third-party material (OpenGLide, LGPL;
// NFSIISE, MIT; the public Glide 2.x API) and about the ownership of it. This
// file is an exception to the MIT license that covers the rest of the original
// code of this repository (see LICENSE and THIRD_PARTY_NOTICES.md).
// Original parts: Copyright (c) 2026 Zeusthenen (Zeus), LGPL-2.1-only.
//
// Comments mention internal iteration labels ("G1", "G138", ...): they are the
// numbering of the private development steps, kept for technical traceability.
// win32/glide2x_ign.h — bring-up resolver for Ign_3dfx.exe's glide2x.dll
// imports. NOT a renderer. All 34 functions Ignition imports (confirmed by
// the Ign_3dfx.exe import-table audit, audit/glide_calls.csv) are
// registered:
//   - G0 (iteration): every one was a clean trap -- log name/args/caller,
//     then request_halt(). Proved on real hardware that execution reaches
//     grGlideInit as the FIRST_GLIDE_CALL.
//   - G1: grGlideInit now has a real, minimal implementation (sets an
//     internal GlideState.initialized flag, no vitaGL/framebuffer/hardware
//     emulation) and RETURNS to the guest normally instead of halting.
//   - G2: grSstQueryHardware also has a real implementation -- writes a
//     GrHwConfiguration whose fields were chosen from disassembling
//     Ignition's own post-call code (see glide2x_ign.cpp), not copied
//     from any reference project.
//   - G3: grSstSelect also has a real implementation -- validates
//     which_sst against the num_sst=1 config G2 wrote and records a
//     logical "selected SST" (no GPU/vitaGL context). Confirmed on real
//     hardware that the next call is grSstWinOpen (FOURTH_GLIDE_CALL,
//     args: hwnd=0, resolution=6/640x400, refresh=0/60Hz,
//     color_format=0/ARGB, origin=0/UPPER_LEFT, numColorBuffers=2,
//     numAuxBuffers=0).
//   - G4: grSstWinOpen also has a real implementation -- the first point
//     in the bring-up where a real graphics context is created. Populates
//     a minimal GlideContext (see glide2x_ign.cpp) and, 3dfx-build-only,
//     calls vitaGL's vglInitExtended() at the Vita's physical 960x544 plus
//     an aspect-correct glViewport() letterboxing the confirmed logical
//     640x400 content. Real hardware run: vglInitExtended returned
//     GL_FALSE.
//   - G4B: root-causing the G4 vglInitExtended failure. Fixed its
//     parameters to match 3 independently proven-working Vita ports found
//     on this machine (ram_threshold=6MB, MSAA_4X instead of G4's
//     8MB/MSAA_NONE) and added full before/after instrumentation
//     ([VGL-G4B]: thread id, free memory, exact args, return value). Added
//     a standalone selftest (glide2x_g4b_vitagl_selftest, gated by the
//     G4B_VITAGL_SELFTEST CMake option) that runs vglInitExtended from the
//     real process main thread, outside the guest/dynarec/trap context.
//     Real hardware: vglInitExtended returned 0, which G4B WRONGLY treated
//     as failure.
//   - G4C: corrected that misreading. Read the EXACT vitaGL source
//     revision matching the installed libvitaGL.a's build timestamp (not
//     master) and found vglInitWithCustomSizes()'s return value is a
//     resolution-fallback flag (GL_TRUE only if the requested size had to
//     be downgraded), not a success/failure boolean -- 0 for an exact-fit
//     960x544 request is the CORRECT, healthy result. Success is decided
//     independently via glGetString(GL_VERSION/VENDOR/RENDERER) +
//     glGetError() (vgl_g4c_verify_context). Real hardware confirmed
//     context_valid=YES (GL_VERSION="OpenGL ES 2.0 VitaGL",
//     GL_VENDOR="Rinnegatamante", GL_RENDERER="SGX543MP4+", glGetError=0),
//     clear+swap worked, no crash.
//   - G4D: tried initializing vitaGL exactly ONCE, from main() before
//     boot_run()/the guest even started (glide2x_g4d_preinit_vitagl,
//     since removed). Real hardware: the pre-init itself succeeded
//     (context_valid=YES), but the log then died right after
//     WINVITA_INIT, before "Creating ARM dynarec CPU backend" completed --
//     i.e. the early vitaGL init appears to interfere with CpuBox86's own
//     construction.
//   - G4E: removed the main()-before-boot_run() preinit entirely (see
//     main.cpp) to test whether that ordering was the problem. vitaGL is
//     initialized back inside grSstWinOpen_impl, same as G4C, keeping
//     G4C's correct (non-boolean) interpretation of vglInitExtended's
//     return value -- verified via vgl_g4c_verify_context (tag parameter
//     lets each iteration's calls show up under their own name). Added
//     [CPU-G4E] breadcrumbs and free-memory logging around
//     make_cpu_box86() in runtime/ignition_boot.cpp. Real hardware:
//     CPU/WinVita bring-up worked again, guest reached grSstWinOpen,
//     vitaGL context valid, FXTRUE, guest continued to FIFTH_GLIDE_CALL =
//     grRenderBuffer(buffer=1), confirmed via disassembly + import-table
//     cross-reference at caller_guest_eip=0x004586f2.
//   - G5: grRenderBuffer also has a real implementation -- logical state
//     only (GlideContext.render_buffer), no glBindFramebuffer/
//     vglSwapBuffers/glDraw* yet (that architecture decision is deferred
//     until grBufferClear/grBufferSwap/LFB/draw calls show how Ignition
//     actually uses front/back). buffer=1 decodes to GR_BUFFER_BACKBUFFER
//     per Glide's standard, stable public enum. Real hardware: FXTRUE,
//     guest continued to SIXTH_GLIDE_CALL = grDepthBufferMode(mode=0),
//     confirmed via disassembly + import-table cross-reference at
//     caller_guest_eip=0x004586f9.
//   - G6: grDepthBufferMode also has a real implementation -- logical
//     state only (GlideContext.depth_buffer_mode), no glEnable/
//     glDisable(GL_DEPTH_TEST) yet (Part D/Q: waiting to see the full
//     depth/cull state-setup sequence -- grDepthBufferFunction/
//     grDepthMask/grCullMode -- before mapping any of it to vitaGL).
//     mode=0 decodes to GR_DEPTHBUFFER_DISABLE per Glide's standard,
//     stable public enum. Real hardware: guest continued to
//     SEVENTH_GLIDE_CALL = grDepthMask(enable=0), confirmed via
//     disassembly + import-table cross-reference at
//     caller_guest_eip=0x00458700.
//   - G7: grDepthMask also has a real implementation -- logical state
//     only (GlideContext.depth_mask), no glDepthMask() call yet (Part
//     D/O/P: still capturing Ignition's full depth/cull state-setup
//     block). enable=0 decodes to FXFALSE per Glide's standard FxBool
//     convention. Real hardware: guest continued to EIGHTH_GLIDE_CALL =
//     grDepthBufferFunction(fnc=7), confirmed via disassembly +
//     import-table cross-reference at caller_guest_eip=0x00458707.
//   - G8: grDepthBufferFunction also has a real implementation -- logical
//     state only (GlideContext.depth_function), no glDepthFunc() call yet
//     (Part E/Q: still capturing Ignition's full depth/cull state-setup
//     block). fnc=7 decodes to GR_CMP_ALWAYS per Glide's standard, stable
//     public GrCmpFnc_t enum (shared between depth and alpha test). Real
//     hardware: guest continued to NINTH_GLIDE_CALL = grCullMode(mode=0),
//     confirmed via disassembly + import-table cross-reference at
//     caller_guest_eip=0x0045870e.
//   - G9: grCullMode also has a real implementation -- logical state only
//     (GlideContext.cull_mode), no glEnable/glDisable(GL_CULL_FACE)/
//     glCullFace() call yet (Part E/Q: still capturing Ignition's full
//     depth/cull state-setup block). mode=0 decodes to GR_CULL_DISABLE
//     per Glide's standard, stable public GrCullMode_t enum. Real
//     hardware: DirectInput init, message loop, opened
//     levels/japan/japan.tex (1032192 bytes), then guest continued to
//     TENTH_GLIDE_CALL = grBufferClear(color=0, alpha=0xff, depth=0xffff),
//     confirmed via disassembly + import-table cross-reference at
//     caller_guest_eip=0x00458731.
//   - G10: grBufferClear has the FIRST real vitaGL mapping in this
//     bring-up -- decodes GrColor_t per the GR_COLORFORMAT_ARGB format
//     selected in grSstWinOpen (alpha taken from grBufferClear's own
//     explicit alpha arg, not color's top byte), calls glClearColor +
//     glClear(GL_COLOR_BUFFER_BIT) (depth clear intentionally skipped --
//     GlideContext.depth_buffer_mode is still DISABLE and numAuxBuffers
//     is still 0), and logs glGetError() immediately after. Does NOT call
//     vglSwapBuffers -- grBufferClear is not presentation. Real hardware:
//     glGetError_after_clear=0, guest continued straight to
//     ELEVENTH_GLIDE_CALL = grBufferSwap(interval=0), confirmed via
//     disassembly + import-table cross-reference at
//     caller_guest_eip=0x00458738.
//   - G11: grBufferSwap has the FIRST real present in this bring-up --
//     calls vglSwapBuffers(GL_FALSE) exactly once per guest call (vitaGL's
//     has_commondialog parameter, unrelated to Glide's own swap_interval,
//     which has no vitaGL equivalent and is only logged, not acted on --
//     no vblank-wait/pacing logic added). Logs glGetError() after the
//     swap and a one-time FIRST_REAL_PRESENT marker, explicitly distinct
//     from FIRST_FRAME (still not claimed -- only a black clear has been
//     presented so far). TRAP POLICY CHANGE: from this iteration on,
//     kGlideFns[] only ever lists genuinely unimplemented imports
//     (grGlideInit through grBufferSwap all return to the guest normally
//     and can repeat, e.g. in a clear/swap frame loop, without
//     re-triggering the trap), so the generic trap reports
//     NEXT_UNIMPLEMENTED_GLIDE_CALL (with a monotonic runtime_call_index
//     counting every Glide call observed, implemented or trapped) instead
//     of an ordinal Nth-call name. grBufferSwap_impl also independently
//     caps at kMaxClearSwapCycles=4 completed swaps with no new API
//     observed, halting with FRAME_LOOP_NO_NEW_GLIDE_CALL so a
//     real-hardware run can't spin unbounded inside an idle clear/swap
//     loop. Real hardware: 2 complete clear/swap cycles
//     (completed_swap_count=2), both glGetError=0, then guest continued
//     to NEXT_UNIMPLEMENTED_GLIDE_CALL = grClipWindow(0,0,640,400),
//     confirmed via disassembly + import-table cross-reference at
//     caller_guest_eip=0x00458766 -- validating logical Glide resolution
//     is exactly 640x400, independent of grSstWinOpen's own resolution
//     decode.
//   - G12: grClipWindow has the FIRST real GL_SCISSOR_TEST mapping in this
//     bring-up -- stores the logical clip rect in GlideContext
//     (clip_min_x/y, clip_max_x/y) then converts it to a physical
//     glScissor() rectangle via a single centralized helper
//     (glide_logical_rect_to_physical_scissor) that queries the REAL
//     current GL viewport (glGetIntegerv(GL_VIEWPORT, ...)) rather than
//     assuming it's still vitaGL init's untouched full-physical default,
//     and explicitly flips Y to convert Glide's GR_ORIGIN_UPPER_LEFT
//     (top-down) coordinates into OpenGL's bottom-left-origin scissor
//     space. Uses GL_SCISSOR_TEST/glScissor, NOT glViewport. No
//     stretch/widescreen/final-letterboxing policy added -- logical
//     640x400 and physical 960x544 stay separate concepts. Real hardware:
//     logical_clip=0,0-640,400, physical_clip=x=0,y=0,w=960,h=544 (the
//     full viewport, as expected for a full-extent clip),
//     glGetError_after_clip=0, guest continued to
//     NEXT_UNIMPLEMENTED_GLIDE_CALL = grTexDownloadTable(tmu=0, type=2,
//     data=0x0061e720), confirmed via disassembly + import-table
//     cross-reference at caller_guest_eip=0x00458c50.
//   - G13: grTexDownloadTable has the FIRST real texture asset ingestion
//     in this bring-up -- decodes GrTexTable_t (type=2 =
//     GR_TEXTABLE_PALETTE, the only value this iteration implements
//     storage for; NCC0/NCC1 are recognized but explicitly halt rather
//     than fake support), validates the guest data pointer against
//     Ignition's own confirmed-mapped PE image range (0x00400000-
//     0x0066E000, NOT the guest stack region used elsewhere in this
//     file), then copies all 256 FxU32 palette entries (1024 bytes) into
//     GlideContext.palette[256] with a diagnostic wraparound checksum and
//     first-4-entries log. Explicitly does NOT create any GL texture yet.
//     Real hardware: checksum=0x00000000 (all-zero palette -- G13's own
//     comment flagged this as plausibly a not-yet-populated initial
//     palette, not a bug), stored_table=YES, but the guest then loaded
//     several more assets (Default.fil, n_Sysgfx.pic, n_Sysg_2.pic,
//     SYS.COL, MenuBkg.dat, ...) WITHOUT reaching a new Glide call, then
//     fell into the grBufferClear/grBufferSwap cycle already established
//     in G10/G11, tripping the (then-4-cycle) frame-loop watchdog.
//   - G14: DIAGNOSTIC ONLY (no new Glide API, grBufferClear/grBufferSwap/
//     grClipWindow/grTexDownloadTable byte-for-byte unchanged). Found the
//     init-time clear/swap/clipwindow block (0x458720-0x458766) is a
//     ONE-TIME unrolled sequence, NOT the repeating loop -- the real
//     per-frame present is a separate call site at 0x458a30-0x458a41
//     (`call grSstIdle` then `push 1; call grBufferSwap(interval=1)`, a
//     different swap_interval than the init block's 0), reached from a
//     tiny leaf function with no internal loop. Also found a nearby LFB
//     lock/unlock state-machine helper (0x458a60/0x458b50) that calls
//     grLfbLock/grLfbUnlock (IAT slots #28/#4) -- both STILL unimplemented
//     traps. Added the first frame-loop watchdog (cycle+wall-clock caps)
//     and Win32 activity counters. Real hardware: only 3 presents in ~18
//     real seconds (completed_swap_count=3), watchdog tripped on its
//     8-second time cap rather than the 120-cycle count cap -- i.e. the
//     guest is doing substantial legitimate work BETWEEN infrequent
//     presents, not spinning a tight clear/swap loop; PeekMessageA=1,
//     everything else (GetMessage/DispatchMessage/PostMessage/timers/
//     joystick) =0, palette checksum unchanged (still all-zero).
//   - G15 (this iteration, DIAGNOSTIC ONLY -- same freeze list as G14,
//     nothing GL-related touched): disassembled the real per-frame region
//     widely (0x458480-0x458c90) instead of just the known call sites,
//     and found the actual mechanism: at 0x458510-0x4585bc Ignition
//     builds a 14-slot FUNCTION-POINTER TABLE ("driver interface") at
//     0x4f3330-0x4f3367, and a dispatcher at ~0x454330-0x45445a calls
//     through it indirectly (`call DWORD PTR [0x4f33xx]`) -- which is why
//     G14's direct-call-target search found no caller for 0x458780/
//     0x458970/0x458a30: they're never called by a static near CALL, only
//     through this table. Identified slots: Init=0x458720 (the known
//     clear/swap/clipwindow block), Shutdown=0x458770 (grSstWinClose),
//     DrawSurface=0x458780 (locks the backbuffer WRITE via grLfbLock,
//     then does a REP MOVS-style blit -- Ignition's own SOFTWARE 2D
//     blitter, used for menu/UI content, NOT hardware draws),
//     PrepareSurface=0x458970 (ensures LFB lock state, zero-fills),
//     Present=0x458a30 (confirmed called from the dispatcher at
//     0x4543d5), LockSurface=0x458a60/UnlockSurface=0x458b50 (the state
//     machine G14 found), SetPalette=0x458ba0 (grTexDownloadTable's own
//     wrapper, confirmed the exact G13 call site), plus several no-op/
//     constant-return stub slots. This confirms Ignition's 2D/menu
//     rendering path goes through grLfbLock (software LFB), not hardware
//     texture/triangle draws -- if DrawSurface ever actually executes
//     (needs its object's [0] validity flag to be set, i.e. the resource
//     it draws must finish loading first), the existing grLfbLock trap
//     will catch it with zero new code. Did NOT find the exact
//     instruction/branch in the OUTER per-tick loop that decides
//     repeat-vs-advance -- that lives higher up than this dispatcher (the
//     dispatcher itself has no loop or repeat/exit branch, it is a plain
//     callable interface) and tracing it further was not completed this
//     iteration; reported honestly as unresolved rather than guessed.
//     Also: switched the frame-loop watchdog's PRIMARY trigger from
//     G14's 8-second wall-clock cap (falsified by real hardware -- see
//     G14's own entry) to a present-count cap (kMaxClearSwapCycles=10),
//     with the wall-clock cap raised to a 60-second safety net that
//     should not normally trigger; added real per-swap timing
//     ([G15-FRAME] swap #N t=.. delta_ms=..), per-category Win32/file/
//     DirectInput/Glide call-count deltas between swaps (aggregate
//     counters added centrally in each file's own shim-registration
//     closure -- user32_window.cpp's K(), winmm_ign.cpp's W(),
//     kernel32_ign.cpp's K(), dinput.cpp's make_vtable() -- rather than
//     touching each individual shim body), and a file-read-by-name
//     summary between swaps (kernel32_ign_g15_snapshot_file_activity(),
//     reusing the engine's own wx86_file_paths()).
//   - G16: NO GLIDE CHANGES (Part A: all 13 Glide APIs frozen, this file's
//     own logic untouched except raising the frame-loop cycle cap --
//     see below -- so it can never preempt the real issue). G15's
//     real-hardware run did not end in a normal diagnostic STOP: it ended
//     in a genuine CPU fault (fault_addr=0x0661ef01, inside the STACK
//     region) roughly 5 seconds after `mciSendStringA("open cdaudio")`.
//     Root-caused in win32/winmm_ign.cpp: that shim was registered with
//     argc=5, but the real Win32 ABI (and Ignition's own real call site,
//     disassembled this iteration at 0x4551f0-0x455208) has exactly 4
//     stdcall arguments -- the extra argc popped 4 bytes too many off the
//     guest stack on every call (bridge.cpp's trap_epilogue: esp_add =
//     4 + 4*argc), a real, mechanically-confirmed stack-imbalance bug,
//     fixed there as an ABI-correctness fix (mci_return semantics
//     unchanged -- still the same generic error). Also enriched
//     runtime/ignition_boot.cpp's top-level CPU-fault handler with full
//     register/stack-dump/classification logging (see its own G16
//     comment) since that is the only point after a fault where the Cpu
//     object is still queryable, given this project's Cpu backends expose
//     no per-instruction trace on the real ARM dynarec target (confirmed:
//     Cpu::set_code_hook is a no-op default only cpu_unicorn.cpp, the
//     desktop oracle, overrides). kMaxClearSwapCycles raised to 1000 so
//     the frame-loop watchdog can't fire before a real fault or a new
//     unimplemented Glide call does (Part S).
//   - G17 (this iteration): guTexMemReset also has a real implementation --
//     logical reset only (GlideContext.tex_mem_next_address/
//     tex_mem_allocation_count/tex_mem_generation), no glDeleteTextures or
//     any GPU-visible action. Confirmed (disassembly at 0x44a305-0x44a334)
//     that Ignition's own code, right before this call, sets a global
//     "texture memory size" of 0x200000 (2MB) and fills a 512-entry table
//     with 0xFFFFFFFF sentinels -- consistent with the well-established
//     public Glide semantic that guTexMemReset reinitializes the "gu"
//     utility layer's own logical texture-memory allocator (used
//     internally by guTexAllocateMemory), NOT an immediate GPU free and
//     NOT the palette (grTexDownloadTable's GR_TEXTABLE_PALETTE state is a
//     separate subsystem -- verified, palette_preserved is logged
//     explicitly every call). G16's real-hardware run (after the
//     mciSendStringA argc fix) confirmed: no CPU fault, MCI calls
//     (open/set/status cdaudio) all handled correctly by the guest despite
//     returning generic errors, a REAL non-zero palette was loaded via
//     grTexDownloadTable (checksum=0x69f793d7, no longer all-zero), then
//     guest continued to NEXT_UNIMPLEMENTED_GLIDE_CALL = guTexMemReset,
//     confirmed via disassembly + import-table cross-reference at
//     caller_guest_eip=0x0044a339. The other 20 imports remain clean
//     traps -- whichever fires first after guTexMemReset is the next
//     NEXT_UNIMPLEMENTED_GLIDE_CALL, still not implemented.
//
//     Separately (not a Glide change, but analyzed this iteration per
//     Part Q): the same run reported a checksum MISMATCH for
//     baltazar\data\menu.col (source_checksum=0x6159c1aa vs.
//     guest_checksum=0xd4be2bdb). Explained, not a bug: win32/
//     kernel32_ign.cpp's CloseHandle checksum diagnostic computes
//     source_checksum by re-reading the WHOLE host file from byte 0
//     (776 bytes), while guest_checksum is the running FNV-1a
//     accumulated ONLY over bytes actually handed to the guest via
//     ReadFile -- and menu.col's own loader calls SetFilePointer to
//     offset 8 before reading the remaining 768 bytes (skipping an
//     8-byte header). The two checksums are therefore computed over
//     DIFFERENT byte ranges by construction (0..775 vs. 8..775) --
//     any file read with a non-zero starting offset would produce this
//     same "mismatch" through this diagnostic, independent of whether
//     the bytes delivered are correct. CHECKSUM_MISMATCH_EXPLAINED_BY_
//     OFFSET=YES. File I/O itself untouched, per Part Q's own
//     instruction.
//
//   - G18 (this iteration): grLfbLock has the FIRST real linear-framebuffer
//     mapping in this bring-up -- a "shadow buffer" strategy (Part M):
//     allocates ONE permanent guest-mapped pixel buffer (via the engine's
//     own ignition_guest_alloc_pixels(), same pool ddraw.cpp already uses
//     for DirectDraw surfaces), sized to the LOGICAL Glide resolution
//     (640x400, not the Vita's physical 960x544) and the requested pixel
//     format, reused across repeated lock/unlock cycles rather than
//     re-allocated per call. Confirmed via disassembly (0x4549f0-0x454a2e,
//     the "lock" wrapper, and its sibling "unlock" wrapper at
//     0x454a40-0x454a53) that GrLfbInfo_t is exactly the well-established
//     public 20-byte layout (size@0, lfbPtr@4, strideInBytes@8,
//     writeMode@12, origin@16) and that Ignition reads back ONLY lfbPtr
//     and strideInBytes, saving them into persistent globals with a
//     save/restore pattern matched by the unlock wrapper. Real hardware:
//     arg0=1=GR_LFB_WRITE_ONLY, arg1=1=GR_BUFFER_BACKBUFFER,
//     arg2=0=GR_LFBWRITEMODE_565, arg3=0=GR_ORIGIN_UPPER_LEFT,
//     arg4=0=FXFALSE -- only this exact WRITE_ONLY/565 case is
//     implemented; GR_LFB_READ_ONLY and any other pixel format explicitly
//     halt rather than guess (Part H/P: no real-hardware evidence yet of
//     what content a READ_ONLY lock would need to return). Adds a
//     before/after LFB checksum (compute_lfb_checksum(), Part Y) so the
//     generic NEXT_UNIMPLEMENTED_GLIDE_CALL trap can report LFB_DIRTY=
//     YES/NO -- the only way this project can detect guest writes into
//     the buffer without a per-instruction trace (G16 confirmed none
//     exists on the real ARM dynarec target). grLfbUnlock deliberately
//     remains a clean trap (Part W) -- not implemented this iteration.
//     The other 19 imports remain clean traps -- whichever fires first
//     after grLfbLock is the next NEXT_UNIMPLEMENTED_GLIDE_CALL, still
//     not implemented.
//
//   - G19 (this iteration): grLfbUnlock has the FIRST real LFB-to-vitaGL
//     upload in this bring-up, plus the first real present-time compositing
//     step. Validates the unlock matches an active, MATCHING lock (type +
//     buffer) before doing anything, computes a final LFB checksum, and
//     samples cheaply (min/max/nonzero pixel count, first 16 pixels of row
//     0, four named sample coordinates) rather than dumping the whole 500KB
//     buffer. Confirmed RGB565 uploads DIRECTLY via
//     GL_RGB/GL_UNSIGNED_SHORT_5_6_5 (present in this vitaGL revision's
//     header) -- no CPU-side conversion to RGBA8888. The GL texture object
//     is created exactly once (GlideContext.lfb_texture/
//     lfb_texture_initialized) and updated via glTexSubImage2D on every
//     subsequent unlock, never recreated. grLfbUnlock itself never calls
//     vglSwapBuffers -- it only sets GlideContext.lfb_dirty=true;
//     grBufferSwap_impl (unchanged present semantics otherwise) checks
//     lfb_dirty right before its own vglSwapBuffers call and, if set,
//     calls the new lfb_blit_to_backbuffer() helper to draw the LFB
//     texture as a screen-aligned textured quad using vitaGL's
//     fixed-function immediate-mode path (glBegin/glVertex2f/glTexCoord2f
//     -- confirmed present, no GLSL shader written), reusing G12's
//     glide_logical_rect_to_physical_scissor for the exact same
//     logical(640x400)->physical mapping grClipWindow already uses. The
//     origin (GR_ORIGIN_UPPER_LEFT) is honored purely through texture-
//     coordinate assignment (bottom-of-quad gets V=1, top-of-quad gets
//     V=0) -- no CPU-side row flip anywhere. GL state the blit touches
//     (blend/depth/cull/texture binding/matrix stacks) is explicitly
//     saved via glIsEnabled/glGetIntegerv and restored afterward;
//     GL_SCISSOR_TEST is deliberately left as grClipWindow set it. Only
//     the exact real-hardware-observed case is supported
//     (WRITE_ONLY/BACKBUFFER/565/UPPER_LEFT) -- a mismatched unlock
//     (wrong type/buffer, or no active lock) halts rather than faking
//     success. The other 18 imports remain clean traps -- whichever fires
//     first after grLfbUnlock is the next NEXT_UNIMPLEMENTED_GLIDE_CALL,
//     still not implemented.
//
//   - G20 (this iteration): grSstIdle also has a real implementation --
//     confirmed to be the SAME tiny "present" leaf function G14/G15
//     already fully disassembled (0x458a30-0x458a41): `call grSstIdle` /
//     `push 1` / `call grBufferSwap(1)` / `ret`, no branch between them,
//     so grBufferSwap is provably Ignition's very next Glide call after
//     grSstIdle here, not assumed. Mapped to glFinish() (sceGxmFinish, a
//     real blocking GPU-idle wait), NOT glFlush() -- verified by reading
//     the exact linked vitaGL revision's own source (the vitaGL source checkout,
//     commit c5d2ad6, same commit G4C already matched by build
//     timestamp): glFlush() there only calls scene_reset() (submits
//     without waiting), which does not satisfy Glide's documented
//     "stall until the hardware is idle" semantic the way glFinish's
//     sceGxmFinish() does. No vglSwapBuffers call added (Part F) and
//     lfb_dirty is left untouched (Part G) -- grBufferSwap remains the
//     only place that blits+presents. Since grBufferSwap is itself
//     already implemented, it will execute normally (very likely
//     triggering the G19 LFB blit, since lfb_dirty was reported YES
//     right before this trap fired) without stopping the trap -- the
//     other 17 imports remain clean traps, so whichever call comes
//     AFTER grBufferSwap and is genuinely unimplemented is the next
//     NEXT_UNIMPLEMENTED_GLIDE_CALL, still not implemented.
//
//   - G20B (this iteration, DIAGNOSTIC ONLY on top of G20 -- grSstIdle's
//     own semantics/sync method UNCHANGED, still glFinish by default, no
//     other Glide API touched): G20's real-hardware run showed the log
//     ending abruptly right after "glGetError_after_idle=0x0000" -- no
//     "[ERROR] CPU fault" line (unlike G16's mciSendStringA bug, which
//     WAS caught and logged by ignition_boot.cpp's top-level catch),
//     nothing from grBufferSwap, nothing at all after that line. Since
//     log.cpp flushes every completed IGN_LOG line immediately (no
//     stdio buffering to lose, an already-established fact), this proves
//     glFinish() and glGetError() both genuinely completed successfully
//     -- the failure is strictly AFTER that point. Re-disassembled the
//     call site fresh this iteration (0x458a20-0x458a71): confirmed
//     call_site=0x00458a30 (the 5-byte `E8` CALL instruction) and
//     return_address=0x00458a35 (== the "return_target"/caller_guest_eip
//     field already logged -- verified this is genuinely the correct
//     post-CALL address, not mislabeled) are DIFFERENT addresses 5 bytes
//     apart, now logged as clearly distinct fields. Between grSstIdle's
//     return and grBufferSwap's own CALL there is exactly ONE guest
//     instruction (`push 0x1` at 0x458a35-0x458a36) -- not 16-32
//     instructions' worth of guest logic, so G20B's added breadcrumb (a
//     "handler about to return" line, the LAST thing this project's own
//     code does before Bridge's untouched trap_epilogue resumes the
//     guest) is enough to bisect "inside our own handler" from
//     "everything after it" (Bridge's trap_epilogue, the guest's single
//     push instruction, or grBufferSwap's own trap dispatch) without
//     needing per-instruction guest tracing. That per-instruction trace
//     (Part C's "16-32 instructions") and a rolling call/ret history
//     (Part G) are confirmed NOT achievable on the real ARM dynarec
//     backend this project ships with (same limitation G16 already
//     established: Cpu::set_code_hook is a no-op default, only
//     cpu_unicorn.cpp -- the desktop oracle -- overrides it) --
//     reported honestly rather than faked. No existing crash/exception
//     handler was found in this project's own source (Part M's own
//     instruction was conditional on one already existing), so none was
//     added. An A/B sync-mode switch (kG20BIdleSyncMode: 0=none,
//     1=glFlush, 2=glFinish) is prepared in grSstIdle_impl for a FUTURE
//     iteration to flip without touching this logic again, but the
//     default stays 2 (glFinish, unchanged from G20) this iteration --
//     Part H explicitly forbids switching away from it on suspicion
//     alone before the breadcrumb evidence narrows things down. Also
//     documented: the user's own hardware report of a black screen with
//     a visible FPS overlay right before the crash is logged as
//     VITAGL_PRESENT_PATH_VISIBLY_ACTIVE=YES (the vitaGL/display backend
//     is demonstrably alive) -- explicitly NOT treated as
//     FIRST_VISIBLE_GAME_PIXELS or FIRST_FRAME evidence.
//
//   - G20C (this iteration, DIAGNOSTIC ONLY -- grSstIdle's default
//     behavior UNCHANGED from G20/G20B, no other Glide API touched):
//     G20B's breadcrumbs proved the crash is strictly AFTER "grSstIdle
//     handler about to return" -- glFinish/glGetError/this project's own
//     handler code all completed successfully. Re-disassembled
//     0x458a20-0x458aaf fresh this iteration: the function containing
//     grSstIdle's call site is a tiny, STANDALONE leaf function
//     (0x458a30-0x458a41, bracketed by NOP padding on both sides, no
//     internal branch): `call grSstIdle(0x458a30)` / `push 1(0x458a35)`
//     / `call grBufferSwap(0x458a37)` / `mov eax,1` / `ret(0x458a41)` --
//     confirmed via G15's own driver-table finding that this function is
//     ONLY ever reached indirectly (called through the 14-slot
//     function-pointer table at 0x4f3330-0x4f3367's Present slot
//     0x4f3350, dispatched from ~0x4543d5), never via a static CALL.
//     Opcode audit of this specific function (Part D): only
//     CALL/PUSH/MOV/RET -- no FPU/MMX/REP/string ops/DIV/segment/
//     indirect-branch instructions anywhere in it. Compared directly
//     against guTexMemReset@0 (G17, real-hardware-proven working): both
//     are argc=0 stdcall traps going through the IDENTICAL Bridge
//     trap_epilogue path (esp_add=4+4*0=4 either way) and both traps
//     write EAX=0 into the guest register file the same way (neither
//     _impl sets any register explicitly -- Bridge's trap_epilogue does
//     it uniformly from the C++ return value) -- so trap mechanics
//     alone don't explain a difference. The one CONCRETE structural
//     difference found: guTexMemReset's own return (0x44a339) leads into
//     ordinary straight-line guest code (an FPU gamma-table loop) with
//     no other Glide trap nearby, while grSstIdle's return (0x458a35)
//     leads, ONE guest instruction later, straight into ANOTHER trap
//     entry (grBufferSwap) -- a back-to-back trap-exit/trap-entry
//     pattern guTexMemReset's own return never exercises. Not asserted
//     as the cause, only flagged as the clearest evidence-backed
//     candidate this iteration's disassembly turned up.
//
//     Built two diagnostic-only A/B variants (CMakeLists.txt options,
//     both default OFF, neither changes the shipping/default build):
//     G20C_IDLE_NOOP (Experiment A) makes grSstIdle_impl skip the GL
//     sync call entirely (kG20BIdleSyncMode forced to 0) while leaving
//     every log line and the trap itself untouched, to isolate
//     "glFinish's own side effect" from everything else.
//     G20C_BYPASS_IDLE_CALLSITE (Experiment B) NOPs out ONLY the single
//     guest CALL instruction at 0x00458a30 (verified byte-exact before
//     patching, via Cpu::write() -- which per cpu.h's own documented
//     contract handles dynarec code-cache invalidation automatically,
//     unlike hostptr() writes) right after Bridge::commit()/link(), so
//     the guest falls straight through to 0x00458a35 without the
//     grSstIdle trap ever firing -- isolates "trap/bridge return
//     interaction specific to grSstIdle" from "guest code starting at
//     0x00458a35." Neither touches grLfbLock/grLfbUnlock/the LFB
//     texture/grBufferSwap's blit path/vitaGL init.
//
//   - G21 (this iteration, DIAGNOSTIC ONLY -- no Glide semantics/logic
//     changed anywhere, only unconditional logging added): G20C's matrix
//     (both Experiment A -- no GL sync -- and Experiment B -- grSstIdle's
//     call site NOPped out -- crashed at the same point) pointed at
//     "guest code starting at 0x00458a35." Re-disassembled
//     0x458a20-0x458aaf fresh: that address is `push 0x1` (2 bytes),
//     followed immediately (0x458a37) by `call grBufferSwap` -- the
//     ENTIRE guest-code span in question is those two completely
//     ordinary, universally-implemented instructions (PUSH imm8, CALL
//     rel32), no FPU/MMX/REP/DIV/indirect-branch/segment-override
//     opcodes anywhere near it, so there is no "exotic guest opcode" for
//     the dynarec to mishandle in this specific span -- confirmed, not
//     assumed.
//
//     Re-examining G20C-B's own log line-by-line surfaced a real
//     observability gap this iteration closes: grBufferSwap_impl's own
//     entry log is gated behind `verbose` (g_completedSwapCount <
//     kDetailedLogCycles=4), which is almost certainly already false
//     this deep into a real run -- so its ABSENCE from G20C-B's log does
//     NOT prove grBufferSwap_impl was never entered, contrary to how it
//     initially read. More importantly: in EVERY prior run (G19/G20/
//     G20B), the crash happened at/around grSstIdle, which Ignition's
//     own code (0x458a30's tiny leaf function) always calls BEFORE
//     grBufferSwap -- meaning grBufferSwap, and specifically its
//     lfb_dirty branch calling lfb_blit_to_backbuffer() (G19's own
//     vitaGL immediate-mode blit, added but never yet proven to run to
//     completion on real hardware), was NEVER ACTUALLY REACHED in any
//     prior run. G20C-B (bypassing grSstIdle) is the FIRST run where
//     that specific, still-untested code path could even be attempted --
//     and grBufferSwap's own already-unconditional post-blit log lines
//     ("LFB blit"/"glGetError_after_lfb_blit") also never appeared. That
//     combination means this iteration's evidence does NOT let us rule
//     out "untested native blit code" as the crash site, alongside the
//     user's own "guest code post-0x458a35" hypothesis -- both remain
//     open. Added fine-grained, UNCONDITIONAL (never gated by `verbose`)
//     [G21] checkpoints at every GL call boundary inside
//     lfb_blit_to_backbuffer() (26 checkpoints: state queries, the
//     physical-rect resolution, each glDisable/glEnable/glBindTexture/
//     glTexEnvi/glColor4f/matrix call, each of the 4 vertices, the
//     matrix/state restore, and the final glGetError) plus an
//     unconditional grBufferSwap-entry breadcrumb and an unconditional
//     breadcrumb immediately before/after the lfb_blit_to_backbuffer()
//     call itself -- without changing a single GL call, argument, or
//     ordering anywhere in that function (Part O/R: LFB/vitaGL logic
//     completely frozen, only observability added). Delivered via the
//     same G20C_BYPASS_IDLE_CALLSITE build (Part Q's own preference, to
//     skip straight past the grSstIdle noise into this code for real).
//
//   - G22 (this iteration, DIAGNOSTIC + ROOT CAUSE IDENTIFIED, nothing
//     FIXED on purpose -- G22's own success definition doesn't require
//     it): G21's real-hardware run reached checkpoint 16 (glBegin
//     (GL_TRIANGLE_FAN)) and crashed with no further log line, narrowing
//     the culprit to the very first glTexCoord2f or glVertex2f call.
//     Added per-CALL [G22] checkpoints (before/after each of the 8 calls
//     in the quad -- 4x glTexCoord2f/glVertex2f pairs -- plus glEnd,
//     each with glGetError() logged when it DOES return) to pin down
//     which exact call fails, on top of G21's coarser per-vertex-pair
//     checkpoints (kept, unchanged).
//
//     While instrumenting, read the EXACT linked vitaGL revision's
//     source (the vitaGL source checkout, commit c5d2ad6 -- the same commit G4C
//     already matched by build timestamp) for glBegin/glVertex3f/
//     glTexCoord2f/glEnd and found the CONFIRMED root cause:
//     vglInitExtended's first parameter is `legacy_pool_size`
//     (source/vgl.c's vglInitWithCustomSizes: `legacy_pool_size =
//     pool_size;`; source/gxm.c: `if (legacy_pool_size) { legacy_pool =
//     gpu_alloc_mapped_temp(legacy_pool_size); legacy_pool_ptr =
//     legacy_pool; ... }`). grSstWinOpen_impl's own vglInitExtended call
//     (frozen since G4E) has ALWAYS passed 0 for this parameter -- so
//     `legacy_pool`/`legacy_pool_ptr` are NEVER allocated and
//     `legacy_pool_ptr` stays NULL for the entire process lifetime.
//     source/ffp.c's `glVertex3f` (which `glVertex2f` calls, which
//     `lfb_blit_to_backbuffer()` calls) unconditionally executes
//     `legacy_pool_ptr[0] = x;` with no null/bounds check -- a NULL
//     pointer write, i.e. exactly the kind of native (not guest x86)
//     crash that produces no CPU-fault log line and no further
//     IGN_LOG output at all, matching every symptom observed since G19.
//     This bug has been latent since G4E: nothing in this project ever
//     called glBegin/glVertex*/glTexCoord* before G19's blit was added,
//     so it was never triggered until now. NOT fixed this iteration,
//     per G22's own explicit instruction ("no hace falta arreglarla
//     todavía") -- flagged in place at the vglInitExtended call site
//     with the exact recommended fix (change the first argument from 0
//     to a nonzero byte count) for a future iteration to apply and
//     verify on real hardware.
//
//   - G23 (this iteration): G22's real-hardware run confirmed the crash
//     is exactly inside the first glVertex2f() call (glBegin/glTexCoord2f
//     both returned cleanly, glGetError=0; glVertex2f never returned).
//     Root cause (from G22's exact-source read): glVertex3f
//     unconditionally writes `legacy_pool_ptr[0] = x` and
//     legacy_pool_ptr is permanently NULL because grSstWinOpen_impl's
//     vglInitExtended call has always passed legacy_pool_size=0.
//     Rather than touch that frozen vitaGL-init call, lfb_blit_to_
//     backbuffer()'s immediate-mode quad (glBegin/glTexCoord2f/
//     glVertex2f/glEnd) is replaced ENTIRELY with the vertex-array path
//     (glEnableClientState(GL_VERTEX_ARRAY/GL_TEXTURE_COORD_ARRAY),
//     glVertexPointer/glTexCoordPointer, glDrawArrays(GL_TRIANGLE_STRIP,
//     0, 4), glDisableClientState) -- confirmed via the exact linked
//     vitaGL source (source/draw.c's glDrawArrays ->
//     _glDrawArrays_FixedFunctionIMPL -> sceGxmDraw with client-side
//     pointers; source/ffp.c's glVertexPointer/glTexCoordPointer/
//     glEnableClientState/glDisableClientState) that NONE of these
//     functions touch legacy_pool_ptr at all -- confirmed by reading
//     their bodies, not assumed. GL_TRIANGLE_STRIP confirmed natively
//     supported (shared.h's gl_primitive_to_gxm maps it directly to
//     SCE_GXM_PRIMITIVE_TRIANGLE_STRIP). The 4-vertex quad's
//     vertex/texcoord order is a direct topological translation of the
//     exact same corner-to-texcoord mapping the FAN version already
//     used (same GR_ORIGIN_UPPER_LEFT UV-only orientation logic, no new
//     guess), just reordered for strip winding (v0=BL, v1=BR, v2=TL,
//     v3=TR). Per-call [G23] checkpoints added around every array/draw
//     call (mirroring G22's granularity), plus explicit "LFB blit
//     complete" and "vglSwapBuffers begin/end" breadcrumbs, plus a
//     FIRST_VISIBLE_GAME_PIXELS_CANDIDATE=YES marker (logged only when a
//     real LFB blit actually happened this swap AND both the blit's own
//     and the present's glGetError are clean) -- still explicitly NOT
//     FIRST_FRAME until a real, coherent image is visually confirmed.
//     Nothing else changed: texture upload/RGB565/grLfbLock/grLfbUnlock/
//     grSstIdle/matrices/GL-state save-restore/scissor are all
//     byte-for-byte unchanged from G19-G22.
//
// Scope: only installed for the experimental 3dfx/Glide build
// (IGNITION_RENDERER_3DFX). The stable Ign_d3d.exe/DirectDraw branch never
// links or calls this.
#pragma once

#include <cstdint>

namespace d2rt { class Bridge; }

namespace ign {

void glide2x_g0_install(d2rt::Bridge& br);
// iteration-G150: installs the guest hot-loop replacements the control file asked for (none by default: it returns at once). Called by boot_run after Bridge::link(), before the first guest instruction.
void glide2x_g150_install_hooks(d2rt::Bridge& br);
void glide2x_g151_session_final();   // iteration-G151: writes the one-line session summary once, when the guest run has ended
void glide2x_g152_session_final();   // iteration-G152: writes the Championship detector's totals once, when the guest run has ended
void glide2x_g155_session_final();   // iteration-G155 (replaces G154's): writes the chroma-key totals once, when the guest run has ended
void glide2x_g156_install_hooks(d2rt::Bridge& br);   // iteration-G156: the display-list cull (two seams in the dispatcher of Ign_3dfx.exe; none when the code is not the one it was written from)
void glide2x_g156_session_final();   // iteration-G156: the cull totals of the session, written once when the guest run has ended
void glide2x_g157_install_hooks(d2rt::Bridge& br);   // iteration-G157: the split-screen confirmation of the multiplayer menu is swallowed (one seam in the menu input proc; none when the code is not the one it was written from)
void glide2x_g159_install_hooks(d2rt::Bridge& br);   // iteration-G159: the player name option opens the Vita's system keyboard (shares the G157 seam; none when the code is not the one it was written from)
void glide2x_g158_install_hooks(d2rt::Bridge& br);   // iteration-G158: the network-game confirmation of the multiplayer menu is swallowed too (shares the G157 seam; none when the code is not the one it was written from)

// iteration-G74, Parte 21-22: read-only snapshots of this file's own
// (otherwise TU-local, anonymous-namespace-scoped) present/triangle
// counters, for win32/kernel32_ign.cpp's [G74-LOAD-HEARTBEAT]/
// [G74-FIRST-RACE-SWAP] -- same cross-TU accessor-function pattern this
// project already uses for diagnostics/log.cpp's log_get_perf_stats().
// Never written from outside this file.
uint32_t ign_get_completed_swap_count();
uint32_t ign_get_total_triangles();

#if defined(IGNITION_RENDERER_3DFX) && defined(G4B_VITAGL_SELFTEST)
// iteration-G4B/G4C: standalone vitaGL init/clear/swap diagnostic, run
// from main() BEFORE (and instead of) the guest -- see glide2x_ign.cpp
// for the full rationale. Returns true iff the context is independently
// verified valid (glGetString/glGetError), NOT the raw vglInitExtended
// return code (see G4C's correction of that misreading).
bool glide2x_g4b_vitagl_selftest();
#endif

} // namespace ign
