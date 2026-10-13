// src/runtime/poll_gil.h — cooperative-scheduler-aware ::poll wrapper.
//
// Any shim that blocks on a host fd (recv, connect-completion, select)
// should use this instead of a raw ::poll.
#pragma once
#include <poll.h>

namespace d2rt {

// Polls host fds, releasing the cooperative GIL for the wait (native
// scheduler only — gil::Release is a no-op under coop) so other guest
// threads keep running. Splits the wait into <=250ms chunks so a shutdown
// request is observed promptly instead of surviving past teardown.
// Returns: >0 = fds ready, 0 = timeout or teardown, <0 = real ::poll error
// (errno left set by the failing ::poll). Callers must distinguish all
// three — folding an error into "nothing ready" reintroduces the busy-spin
// this primitive exists to avoid.
int wx86_poll_gilfree_n(pollfd* pf, nfds_t nfds, int total_ms);

// Single-fd convenience form (connect-completion, blocking recv).
int wx86_poll_gilfree(pollfd* pf, int total_ms);

// True once the native scheduler is tearing down. A wait built from several
// wx86_poll_gilfree_n calls must stop when this turns true.
bool wx86_poll_shutdown_requested();

} // namespace d2rt
