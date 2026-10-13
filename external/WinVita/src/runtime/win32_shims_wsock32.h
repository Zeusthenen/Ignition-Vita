// src/runtime/win32_shims_wsock32.h — Winsock 1.1/2 ordinal shims
// (WSOCK32.dll / WS2_32.dll) and the generic socket-handle table backing
// them. connect/recv/send/select are fully generic: application policy
// reaches them through the observer and the connect route declared below.
// The address helpers (inet_addr/inet_ntoa/gethostbyname) write through the
// guest scratch allocator (guest_scratch.h).
#pragma once
#include <cstdint>
#include "runtime/net_guard.h"   // the exit lock, a leaf unit
namespace d2rt { class Bridge; class Cpu; }

// --- socket-handle table -------------------------------------------------
// A guest Winsock SOCKET is just an opaque uint32_t handle; this table maps
// it to a host fd plus the two bits every Winsock emulation layer needs to
// track per socket (guest-visible blocking mode, and the last connect()
// destination port — generic bookkeeping, not particular to any protocol).
struct WsockHandle { int fd; bool nonblock; uint16_t port; };

uint32_t wx86_sock_alloc(int fd);              // new guest handle for fd (nonblock=false, port=0)
bool     wx86_sock_get(uint32_t h, WsockHandle& out);
int      wx86_sock_fd(uint32_t h);             // -1 if h is not a live handle
uint16_t wx86_sock_port(uint32_t h);           // 0 if h is not a live handle
void     wx86_sock_set_port(uint32_t h, uint16_t port);
bool     wx86_sock_nonblock(uint32_t h);
void     wx86_sock_set_nonblock(uint32_t h, bool nb);
void     wx86_sock_erase(uint32_t h);          // no-op if h is not live (mirrors std::map::erase)

// --- networking-enabled gate ---------------------------------------------
// One flag, owned here, read by every network shim (this file's own and
// any shim the embedder still registers) so there is exactly one "is networking
// on" bit instead of two that could drift apart.
bool wx86_net_enabled();
void wx86_net_set_enabled(bool on);

// --- small generic helpers, also usable by embedder-side shims ----------
uint32_t wx86_wsa_from_errno(int e);   // POSIX errno -> Winsock error code

// Single canonical "last Winsock error" store (what WSAGetLastError, itself
// registered here, returns). Every socket shim sets it on failure through
// this one setter — one store, not two that could read stale after a call
// that only updates one of them. Embedder-registered socket shims must use
// the same setter.
uint32_t wx86_net_last_error();
void wx86_net_set_last_error(uint32_t code);

// Completes a non-blocking ::connect() synchronously: waits (GIL released,
// native scheduler only) up to timeout_ms for the socket to become
// writable, then reads SO_ERROR to tell a real success from a failed
// handshake. Returns 0 on success; -1 on failure or timeout, with a
// Winsock-style code written to *outWsaErr (10060 ETIMEDOUT-style on
// timeout). Callers still make the initial ::connect() call themselves —
// this only owns the generic "wait for it to finish" dance every
// non-blocking Winsock connect() needs.
int wx86_connect_wait(int fd, int timeout_ms, uint32_t* outWsaErr);

// Generic blocking-emulated recv/send core: real Winsock sockets default to
// BLOCKING mode even though this runtime keeps the host fd non-blocking (to
// protect the cooperative scheduler) — recv emulates the wait via a
// GIL-released poll when the guest hasn't asked for non-blocking mode.
// Returns the same shape as POSIX recv/send: >=0 bytes (0 on EOF for recv),
// or -1 with *outWsaErr set. `blocking` is the guest-visible mode (i.e. the
// caller's own !nonblock check on the WsockHandle).
// wait_ms: 0 = Windows policy (block indefinitely, or WSAETIMEDOUT if
// SO_RCVTIMEO was set via setsockopt); > 0 = explicit caller-imposed bound.
int wx86_recv_blocking(int fd, void* buf, uint32_t len, bool blocking, int wait_ms, uint32_t* outWsaErr);
int wx86_send_simple(int fd, const void* buf, uint32_t len, uint32_t* outWsaErr);

// --- generic connect-time route override ---------------------------------
// The moral equivalent of a hosts file entry or an outbound proxy: when set,
// every connect() to a NON-loopback destination is rewritten to this address,
// keeping the requested port. Off (0) unless set. This is a sandboxing /
// offline-testing facility of the socket layer itself — it knows nothing
// about which protocol or which game is running on top of it.
//
// NOTE for embedders: this is a BACKSTOP, not the normal way to point a
// client at a private server. The faithful way is the one a real PC uses —
// configure the client's own server list (its registry keys / .ini) so it
// asks for your host in the first place. See win32_shims_wsock32.cpp.
void     wx86_net_set_redirect(uint32_t ip);   // 0 = disabled
uint32_t wx86_net_redirect();

// --- outbound exit lock: nothing reaches the public internet unless
// explicitly allowed -------------------------------------------------------
// A socket-layer sandbox, as protocol-agnostic as the route above: the only
// question asked is "can this address reach the public internet?". When
// armed, any non-private destination (outside 127/8, 0.0.0.0, 10/8,
// 172.16/12, 192.168/16, 169.254/16, multicast 224/4, and the
// 255.255.255.255 broadcast) is refused with WSAEACCES (10013) on connect
// and sendto, and any datagram from such an address is dropped by recvfrom.
// A refusal touches no host socket: no TCP handshake started, no bytes sent.
//
// Default OFF. The engine has no opinion on its consumer's network policy;
// the embedder arms it and decides when to lift it. The verdict is applied
// AFTER routing, i.e. against the destination actually dialed. The engine
// prints nothing itself — it reports a refusal via WX86_NET_REFUSED to the
// observer, which logs it however it wants.
//
// The declarations themselves live in runtime/net_guard.h, a leaf unit: the
// flag is queried from two layers (here, and name resolution in
// net_nonblock.cpp), and this file alone pulls in the whole bridge and CPU.
// Included here so existing consumers of this header keep the same symbols
// available.

// --- generic socket-layer observer ---------------------------------------
// ONE passive observation point over the whole socket layer. The engine
// reports what happened; it never asks the observer for a decision and never
// changes behaviour based on it. Protocol framing, per-application policy,
// logging and instrumentation all belong to the embedder, which registers a
// single observer and dispatches internally. Nothing here names a protocol,
// a port or a product.
enum {
    WX86_NET_CONNECT = 0,    // before the host connect(): ip/port = requested
                             // destination, route_ip = address actually used
                             // (== ip when no redirect applied)
    WX86_NET_CONNECT_DONE,   // after it: result 0 = connected immediately,
                             // 1 = completed after a synchronous wait, -1 = failed
    WX86_NET_SEND,           // after a send: data/len = payload, result = bytes
    WX86_NET_RECV,           // after a recv: data/len = payload, result = bytes
                             // (0 = orderly shutdown, -1 = error)
    WX86_NET_CLOSE,
    WX86_NET_RESOLVE,        // name resolution: data/len = requested name,
                             // result 0 = resolved (ip = address obtained),
                             // -1 = failed (wsa_err set). The engine is
                             // otherwise silent, so this is how the embedder
                             // learns a name failed to resolve.
    WX86_NET_REFUSED,        // the exit lock (wx86_net_set_private_only)
                             // REFUSED a destination: ip/port = the one
                             // targeted, result = the refused operation
                             // (WX86_NET_CONNECT / _SEND / _RECV), wsa_err =
                             // 10013. NO host socket was touched. The engine
                             // is otherwise silent, so this is the only way
                             // the embedder learns a packet was withheld.
    WX86_NET_SELECT,         // after a select: result = its return value
                             // (ready count, 0 = timed out, -1 = error with
                             // wsa_err), timeout_ms = requested wait,
                             // waited_ms = time spent in the call
};
struct WsockEvent {
    int         kind;
    d2rt::Cpu*  cpu;       // CPU at the call site, for embedder-side diagnostics
    uint32_t    handle;    // guest SOCKET
    int         fd;        // host fd
    uint32_t    ip;        // CONNECT: requested destination, network byte order
    uint32_t    route_ip;  // CONNECT: address actually connected to
    uint16_t    port;      // CONNECT: destination port; SEND/RECV: socket's port
    int         result;    // see the kind comments above
    uint32_t    wsa_err;   // Winsock error code when result < 0
    const uint8_t* data;   // SEND/RECV payload, else null
    int         len;       // SEND/RECV payload length, else 0
    int         timeout_ms;  // SELECT: requested wait; -1 = NULL timeval (infinite), -2 = invalid
    uint32_t    waited_ms;   // SELECT: wall time spent inside the call
};
typedef void (*WsockObserverFn)(const WsockEvent&);
// Exactly ONE observer, deliberately: silently accepting a second
// registration would let a duplicate hook shadow the first unnoticed.
// Setting it twice replaces the first.
void wx86_net_set_observer(WsockObserverFn cb);

void win32_shims_wsock32_install(d2rt::Bridge& br);
