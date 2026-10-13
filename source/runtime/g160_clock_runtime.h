// runtime/g160_clock_runtime.h -- iteration-G160: the default clock profile (ARM 444 / BUS 222 / GPU 222 / XBAR 166 as a MINIMUM, applied once at start-up, restored once at the normal end). The policy is the pure code of win32/g160_clocks.h;
// this file only gives it the Vita's userland power service (psp2/power.h, ScePower_stub) and the log.
#pragma once
namespace ign {
void g160_apply_clocks();     // once, from main(), after the log folder exists and before anything is initialised (vitaGL / GXM / guest); never fails, never exits
void g160_restore_clocks();   // once, from main(), after the guest has stopped and before the process exit; idempotent
}
