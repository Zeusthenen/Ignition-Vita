// src/runtime/net_guard.h — outbound exit lock: nothing reaches the public
// internet unless explicitly allowed.
//
// Deliberately a leaf unit: it depends on neither the bridge, the CPU, nor
// the shim table, so both layers that need to ask the question can link it,
// and so can a standalone test:
//   - the address layer (net_nonblock.cpp, wx86_net_resolve) — a name
//     resolution is already an outbound step;
//   - the socket layer (win32_shims_wsock32.cpp, connect/sendto/recvfrom) —
//     what actually goes out on the wire.
//
// No weak symbol with a default value: a unit that fails to link the real
// guard must fail to build, not silently inherit a disarmed lock. A lock
// that lies is worse than no lock.
#pragma once
#include <cstdint>

// Default OFF. The engine has no opinion on its consumer's network policy;
// the embedder arms it and decides when to lift it.
void wx86_net_set_private_only(bool on);
bool wx86_net_private_only();

unsigned long long wx86_net_refused();   // destinations refused since startup
unsigned long long wx86_net_allowed();   // destinations let through (scale check)

// Bare predicate, exposed so an embedder can ask the same question elsewhere
// without reimplementing the table. ip is in NETWORK byte order, as stored
// in a sockaddr.
bool wx86_net_addr_is_private(uint32_t ip_be);

// THE decision, counters included: "does this destination pass?". Returns
// true if it passes, and keeps both counters up to date. Logs nothing and
// touches no socket — the calling layer reports the refusal however it sees
// fit (Winsock error code, observer).
bool wx86_net_addr_allowed(uint32_t ip_be);
