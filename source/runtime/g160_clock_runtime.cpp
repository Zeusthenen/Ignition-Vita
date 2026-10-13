#include "runtime/g160_clock_runtime.h"
#include "win32/g160_clocks.h"
#include "diagnostics/log.h"
#include <psp2/power.h>

namespace ign {
namespace {
// The only place of the program that calls the four setters; each is called at most once per run by apply() and at most once by restore().
struct VitaPower {
    int get(int comp) {
        switch (comp) {
            case g160::kArm:  return scePowerGetArmClockFrequency();
            case g160::kBus:  return scePowerGetBusClockFrequency();
            case g160::kGpu:  return scePowerGetGpuClockFrequency();
            default:          return scePowerGetGpuXbarClockFrequency();
        }
    }
    int set(int comp, int mhz) {
        switch (comp) {
            case g160::kArm:  return scePowerSetArmClockFrequency(mhz);
            case g160::kBus:  return scePowerSetBusClockFrequency(mhz);
            case g160::kGpu:  return scePowerSetGpuClockFrequency(mhz);
            default:          return scePowerSetGpuXbarClockFrequency(mhz);
        }
    }
};
g160::Profile g_profile;   // constant-initialised
}   // namespace

void g160_apply_clocks() {
    VitaPower p; char buf[420];
    g160::apply(p, g_profile);
    g160::format_start(buf, sizeof buf, g_profile);
    IGN_LOG("[G160-CLOCKS]", "%s", buf);
}

void g160_restore_clocks() {
    VitaPower p; char buf[420];
    const g160::RestoreReport rep = g160::restore(p, g_profile);
    if (!rep.ran) return;   // nothing was applied, or this already ran
    g160::format_restore(buf, sizeof buf, rep);
    IGN_LOG("[G160-CLOCKS-RESTORE]", "%s", buf);
}
}   // namespace ign
