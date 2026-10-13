#include "runtime/g161_pacer.h"
#include "win32/g161_pacing.h"
#include "diagnostics/log.h"
#include "runtime/host_clock.h"      // wx86_now_us() = sceKernelGetProcessTimeWide() on the Vita: monotonic microseconds
#include <psp2/kernel/threadmgr.h>   // sceKernelDelayThread

namespace ign {
namespace {
g161::Pacer g_pacer;                 // constant-initialised (default: 30 FPS)
bool g_announced = false, g_summarised = false;
}

void g161_pacer_configure(const char* controlText) {
    g_pacer.cfg = g161::parse_fps_limit(controlText);
}

void g161_pace_before_present() {
    if (!g_announced) {
        g_announced = true;
        const g161::Config& c = g_pacer.cfg;
        IGN_LOG("[RELEASE-FPS-LIMITER]", "configured=%u target_us=%u mode=%s timing_source=sceKernelGetProcessTimeWide(us,monotonic) vblank=%s build=v1.0_RC1 source=%s%s%s sleep=%s",
                (unsigned)c.fps, (unsigned)c.targetUs, c.enabled() ? "sleep_before_present(absolute_grid,no_catch_up)" : "unlimited(diagnostic,no_wait)",
                "display_queue_callback_of_vitaGL(vsync_interval=1);no_wait_added_by_the_pacer", c.source == g161::kFromFile ? "control_file" : (c.source == g161::kRefused ? "default(refused_token)" : "default"),
                c.source == g161::kRefused ? " refused=" : "", c.source == g161::kRefused ? c.refused : "", c.enabled() ? "sceKernelDelayThread(remaining_us)" : "none");
    }
    const uint64_t now = wx86_now_us();
    const uint64_t sleepUs = g_pacer.plan(now);
    uint64_t rel = now;
    if (sleepUs) {
        sceKernelDelayThread((SceUInt32)(sleepUs > 2u * g_pacer.cfg.targetUs ? 2u * g_pacer.cfg.targetUs : sleepUs));
        rel = wx86_now_us();
    }
    g_pacer.released(rel, rel - now);
}

void g161_pacer_summary() {
    if (g_summarised) return;
    g_summarised = true;
    char buf[300];
    g161::format_summary(buf, sizeof buf, g_pacer);
    IGN_LOG("[RELEASE-FPS-SUMMARY]", "%s", buf);
}
}   // namespace ign
