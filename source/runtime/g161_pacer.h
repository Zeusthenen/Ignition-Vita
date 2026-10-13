// runtime/g161_pacer.h -- iteration-G161 (v1.0 RC1): the 30 FPS presentation limiter. The algorithm is the pure code of win32/g161_pacing.h; this file gives it the Vita's monotonic clock and sleep.
#pragma once
namespace ign {
void g161_pacer_configure(const char* controlText);   // reads `fps-limit=` from the text of the existing control file (called by the control-file reader); default 30
void g161_pace_before_present();                      // once per frame, right before vglSwapBuffers: waits for what is left of the frame period (one sleep, no spinning); the first call also writes [RELEASE-FPS-LIMITER]
void g161_pacer_summary();                            // once, when the guest run has ended: [RELEASE-FPS-SUMMARY]; idempotent
}
