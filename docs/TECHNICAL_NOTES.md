# Technical notes

A summary of how Ignition Vita works, for developers. File names are relative to the repository root. The engine library (`external/WinVita`) is a separate project by Franck Roustan, vendored here; see `external/WinVita/README.md` (in French) and the notices in `THIRD_PARTY_NOTICES.md`.

## 1. Architecture in one picture

```
                     ┌────────────────────────────────────────────────────────┐
                     │  Ign_3dfx.exe  (the original PE32 game, x86 machine code)│  "guest"
                     └──────────────▲─────────────────────────────────────────┘
        imports (kernel32, user32,  │  jumps / traps
        winmm, dsound, dinput,      │
        ddraw, glide2x ...)         │
   ┌────────────────────────────────┴─────────────────────────────────────────┐
   │ WinVita (external/WinVita): PE loader, Box86-derived x86→ARMv7 dynarec,   │
   │ Cpu / Bridge API, generic Win32 shims, DirectSound emulation, Vita audio  │
   ├───────────────────────────────────────────────────────────────────────────┤
   │ Ignition Vita (source/): the Ignition-specific part                        │
   │   runtime/   boot, memory layout, paths, clock profile, frame pacer       │
   │   win32/     kernel32 file I/O, winmm timers/MCI, user32 window messages, │
   │              ddraw, dinput (Vita pad → keyboard), glide2x → vitaGL        │
   │   music/     virtual CD audio (MCI) + Ogg decoding                         │
   │   diagnostics/ logger                                                      │
   └───────────────────────────────────────────────────────────────────────────┘
                                   │
                    vitaGL (OpenGL on sceGxm), sceAudio, sceCtrl, sceIme, sceDisplay ...
```

## 2. The guest and the dynarec

- `Ign_3dfx.exe` is mapped by WinVita's PE loader (`pe_image`). Its imports are resolved to **shims**: native functions registered in WinVita's `Bridge`. An import with no shim becomes a trap that stops the game cleanly and logs `[STOP] ...` (this is how the exit path ends: the game's window teardown reaches an unimplemented Glide call and the run stops by design).
- The code runs through a dynamic recompiler derived from Box86's ARMv7 dynarec (`external/WinVita/third_party/box86-dynarec`, plus the integration layer `src/dynarec86`). The JIT uses the stock VitaSDK memory-block / VM-domain API, hence the *unsafe* homebrew requirement.
- The guest runs in a single `run()` resumed in budgeted slices: the engine's run budget is finite, so `boot_run` resumes the guest when the budget is exhausted instead of treating it as an exit.
- Native code can intercept guest code at known addresses with 5-byte jump "seams" (see below), always after checking a hash of the original bytes.

## 3. Win32 shims specific to the port (`source/win32`, `source/runtime`)

| File | Role |
|---|---|
| `runtime/ignition_boot.cpp` | start-up: memory arenas, WinVita init, shim registration, installation of the native hooks, the guest run loop, shutdown |
| `runtime/ignition_paths.cpp` | guest path → `ux0:data/ignition/...` (drive letters stripped, either slash accepted) |
| `win32/kernel32_ign.cpp` | module / command line / file I/O (`CreateFileA`, `ReadFile`, ...) over the translated paths |
| `win32/winmm_ign.cpp` | `timeGetTime`, timers, joystick stubs, `mciSendStringA` (forwarded to the virtual CD) |
| `win32/user32_window.cpp` | window class / message queue, `PostMessageA`, the `WM_CLOSE` exit chain |
| `win32/ddraw.cpp` | DirectDraw entry points (only what the 3dfx executable touches) |
| `win32/dinput.cpp` | DirectInput keyboard device fed from the Vita pad (see CONTROLS.md); filters the pad while the keyboard dialog is open |
| `win32/glide2x_ign.cpp`, `glide2x_ign.h` | the Glide 2.x API (the 34 functions the game imports) over vitaGL, plus the compositor, the hooks and the telemetry |

The game is **wall-clock driven**: it reads the real clock once per frame, converts the elapsed time into logic steps (clamped to 300 ms) and draws. That is why frame-rate limiting does not change game speed.

## 4. Glide → vitaGL

- 3D triangles arrive through `guDrawTriangleWithClip` and friends. Vertices are decoded from guest memory, state (blend, depth, clip, texture, combiner) is kept as a shadow of the GL state and applied lazily ("deferred state packet"), and consecutive triangles are submitted in batches inside one draw session to cut driver calls.
- Triangles wholly outside the viewport/scissor are rejected before decoding; a display-list **cull** hook skips whole groups of track polygons that are provably off screen.
- Textures are uploaded when the game downloads them (palettised 8-bit data expanded with the current palette). **Chroma-keyed** textures (the smoke) get keyed variants, built only when a texture really contains the key colour, cached per (palette, key) and selected per draw.
- The game draws its **HUD, sprites and 2D screens in software** straight into the Glide linear frame buffer (LFB). The port locks/unlocks that buffer as guest memory, and a **compositor** uploads only the changed regions as textures and draws them over the 3D scene at present time (an incremental, vectorised change scan; a persistent overlay for UI; separate paths for menu and race scenes). Getting this right (alpha, dirty regions, pause, car selection carousel) was most of the early work.

## 5. Native fast paths

Two hot pixel loops of the game (the palette expansion of a draw surface and the sprite blitter) are replaced by native ARM code that was verified against the original machine code; they are switched on by default and fall back to the guest code for any case they do not cover. The 3D geometry path has a fast guest-vertex reader and the trivial-rejection described above. All of them check hashes of the original code before installing.

## 6. Menu seams (disabled entries and the name keyboard)

All menu nodes of the game share one input procedure. A single 5-byte seam there (`source/win32/g157_split.h`, `g158_online.h`, `g159_name.h` describe the decisions) lets the port:

- swallow the *confirm* of the split-screen item and of the network-game item (the original code of the latter would end the game window);
- swallow the *confirm* of the name item and open the Vita keyboard (`sceImeDialog`), whose answer is converted with the game's own character table and handed to **the game's original commit routine**, so the name is stored exactly as if typed on a PC keyboard.

The decisions are pure functions over guest memory that identify the menu items by **structure** (node variables, item indices, callback tables), never by text, so they work in every language.

## 7. Audio

- **Effects**: WinVita emulates DirectSound (`src/runtime/ds_emul.*`); the game mixes on its own thread and the port's audio thread plays the result through `sceAudio`.
- **Music**: the original plays CD audio through MCI. `source/music` implements a **virtual CD**: a table of contents built from `Music/Track02..08.ogg`, MCI commands (`open`, `play from..to`, `status position`, ...), a music thread that decodes Ogg with `stb_vorbis` and feeds a second audio port.
- The audio threads are created with explicit core and priority settings so that they keep running while the guest thread is busy.

## 8. Frame pacing and the 30 FPS limiter

- One `grBufferSwap` per game frame. The limiter (`source/win32/g161_pacing.h`, `source/runtime/g161_pacer.cpp`) waits **once per frame, just before the present**, for the time left of 33,333 µs, with `sceKernelDelayThread` and the monotonic `sceKernelGetProcessTimeWide`. It keeps an absolute time grid: an early frame waits only the remainder, a late frame is presented at once and the grid restarts from the real time (no catch-up).
- It adds no vertical-blank wait of its own: vitaGL's display-queue callback already aligns each flip to the 60 Hz refresh.
- Configurable with `fps-limit=` (see section 10). One log line at start and one summary at exit; nothing per frame.

## 9. Clock management

`source/win32/g160_clocks.h` (policy) and `source/runtime/g160_clock_runtime.cpp` (the only caller of the `scePower*ClockFrequency` setters): once at start-up, per component, raise to ARM 444 / BUS 222 / GPU 222 / XBAR 166 only if the current value is lower; never lower a higher value; at exit restore only what the port changed and only if it is still at the value the port set. Idempotent, never fatal, no per-frame calls. There is no suspend/resume handling: the port does not receive those events.

## 10. Control-file tokens

`ux0:data/ignition/g146_geo.txt` is a plain text file of space-separated tokens read once at start-up.

| Token | Effect |
|---|---|
| `fps-limit=30` / `fps-limit=0` / `fps-limit=off` / `fps-limit=N` (10–60) | frame limiter (supported setting) |

The remaining tokens are **diagnostic switches, not supported settings**, kept for developers: `g159=off` (name entry back to the game's own keyboard entry), `g158=off` and `g157=off` (re-enable the network / split-screen entries — both end the game window and exit to the LiveArea), `g156=off` (no display-list cull), `g153=off`, `g155=classic` (older smoke drawing), `hook-hud-draw=off`, `hook-hud-sprite=off` (guest code instead of the native pixel loops), plus engine-level tokens read by `ignition_boot.cpp`/`g150_ctl.h`.

## 11. Logs and diagnostics

- `source/diagnostics/log.cpp`: files are opened in append mode and closed per write (crash-safe), previous run kept as `.previous`; an **allowlist** with per-tag caps decides which lines are ever written, so the release build writes few lines (batching and caps keep the card I/O small).
- `boot.txt` (checkpoints written even before the logger is up), `ignitionvita.log`, `ignition_critical.log` (errors and key lines), `ignition_session_id.txt`.
- The default build is `LIGHT`; `-DIGN_DIAGNOSTICS=FULL` compiles the heavy profilers back in (never for normal use).
- Useful lines: `[G160-CLOCKS]`, `[RELEASE-FPS-LIMITER]`, `[RELEASE-FPS-SUMMARY]`, `[G138-PERF-BASELINE]` (one line per 120 frames: FPS, percentiles, per-frame work), `[G151-SESSION]` (native hooks and error counters: `gl`, `ring`, `shadow_mismatches`, `scan_mismatches`, `budget_resumes` should all be 0), `[G117-EXIT]` (exit sequence).

## 12. Tests

During development every change was covered by host-side tests (pure logic compiled on a PC, plus a harness that executed the game's real menu code natively to compare behaviour). That test suite depends on private development snapshots and is **not** part of this repository.
