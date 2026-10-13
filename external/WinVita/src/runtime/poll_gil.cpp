// src/runtime/poll_gil.cpp — see poll_gil.h.
//
// A non-null g_native_sched implies the native scheduler is active (nothing
// else ever constructs it), so checking it directly is sufficient — no
// separate "is the native scheduler active" flag is needed.
#include "runtime/poll_gil.h"
#include "runtime/gil.h"
#include "runtime/sched_native.h"
#include <cerrno>

namespace d2rt {

int wx86_poll_gilfree_n(pollfd* pf, nfds_t nfds, int total_ms) {
    int left = total_ms;
    while (left > 0) {
        int chunk = left > 250 ? 250 : left;
        int r;
        { gil::Release rel; r = ::poll(pf, nfds, chunk); }
        // EINTR is not an error: returning -1 here would spin the guest
        // even though time remains. Consume the chunk (never wait longer
        // than requested) and loop.
        if (r < 0 && errno == EINTR) { left -= chunk; continue; }
        if (r != 0) return r;   // >0 ready, <0 real error (errno intact)
        if (g_native_sched && g_native_sched->shutdown_requested())
            return 0;   // teardown: don't outlive the bounded join (post-main UAF)
        left -= chunk;
    }
    return 0;
}

int wx86_poll_gilfree(pollfd* pf, int total_ms) {
    return wx86_poll_gilfree_n(pf, 1, total_ms);
}

bool wx86_poll_shutdown_requested() {
    return g_native_sched && g_native_sched->shutdown_requested();
}

} // namespace d2rt
