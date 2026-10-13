// src/runtime/win32_shims_wsock32.cpp — see win32_shims_wsock32.h.
//
// Holds every Winsock ordinal that is a generic Winsock->POSIX passthrough
// with no protocol-specific literal: byte-order conversions, WSAStartup/
// WSACleanup, gethostname, the socket-handle table, the networking-enabled
// gate, accept/bind/closesocket/getpeername/getsockname/getsockopt/
// ioctlsocket (both DLLs)/listen/setsockopt/shutdown/socket, connect/recv/
// send (generic bodies: wx86_connect_wait/wx86_recv_blocking/
// wx86_send_simple), select/__WSAFDIsSet, the address helpers (inet_addr/
// inet_ntoa/gethostbyname, which write through the guest scratch allocator,
// guest_scratch.h) and the socket-layer half of the exit lock (net_guard.h)
// — see win32_shims_wsock32.h for the observer/redirect/exit-lock contract
// these all sit behind. sendto/recvfrom are real registered ordinals (not
// left to the default shim) partly so the exit lock can gate them too.
//
// inet_ntoa and gethostbyname were fixed, not just relocated, when they
// moved here: inet_ntoa was returning a hardcoded "127.0.0.1" regardless of
// its argument, and gethostbyname made five permanent allocations per call,
// exhausting guest memory in long sessions. Both are now cached by key.
//
// Trace-log note: ordinals registered here through REGORD bypass any per-call
// trace wrapper an embedder applies to its own registrations; the observer
// is the way to follow them.
#include "win32_shims_wsock32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/poll_gil.h"
#include "runtime/host_clock.h"
#include "runtime/net_nonblock.h"
#include "runtime/guest_scratch.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <map>
#include <vector>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // absent from the VitaSDK newlib; harmless without SIGPIPE
#endif
using namespace d2rt;

// --- socket-handle table --------------------------------------------------
namespace {
std::map<uint32_t, WsockHandle> g_wsockTable;
uint32_t g_wsockNext = 1;
bool g_netEnabled = false;
uint32_t g_wsaLastErr = 0;
}

uint32_t wx86_sock_alloc(int fd) {
    uint32_t h = g_wsockNext++;
    g_wsockTable[h] = WsockHandle{fd, false, 0};
    return h;
}
bool wx86_sock_get(uint32_t h, WsockHandle& out) {
    auto it = g_wsockTable.find(h);
    if (it == g_wsockTable.end()) return false;
    out = it->second; return true;
}
int wx86_sock_fd(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? -1 : it->second.fd;
}
uint16_t wx86_sock_port(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? 0 : it->second.port;
}
void wx86_sock_set_port(uint32_t h, uint16_t port) {
    auto it = g_wsockTable.find(h);
    if (it != g_wsockTable.end()) it->second.port = port;
}
bool wx86_sock_nonblock(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? false : it->second.nonblock;
}
void wx86_sock_set_nonblock(uint32_t h, bool nb) {
    auto it = g_wsockTable.find(h);
    if (it != g_wsockTable.end()) it->second.nonblock = nb;
}
void wx86_sock_erase(uint32_t h) { g_wsockTable.erase(h); }

bool wx86_net_enabled() { return g_netEnabled; }
void wx86_net_set_enabled(bool on) { g_netEnabled = on; }

static uint32_t        g_routeIp = 0;
static WsockObserverFn g_observer = nullptr;

void wx86_net_set_redirect(uint32_t ip) { g_routeIp = ip; }
uint32_t wx86_net_redirect() { return g_routeIp; }
void wx86_net_set_observer(WsockObserverFn cb) { g_observer = cb; }


// Reads a guest C string byte by byte — address shims receive a guest
// char*. Bounded (1 KiB): a stray pointer must not make the engine loop
// over all of guest memory looking for a zero that doesn't exist.
static std::string wx86_guest_cstr(Cpu& c, uint32_t p) {
    std::string s;
    if (!p) return s;
    for (uint32_t i = 0; i < 1024; i++) {
        uint8_t ch = 0;
        c.read(p + i, &ch, 1);
        if (!ch) break;
        s.push_back((char)ch);
    }
    return s;
}

// Single dispatch point. Zero cost when nobody observes.
static void wx86_net_notify(int kind, Cpu* c, uint32_t handle, int fd,
                            uint32_t ip, uint32_t routeIp, uint16_t port,
                            int result, uint32_t wsaErr,
                            const uint8_t* data, int len) {
    if (!g_observer) return;
    WsockEvent e{kind, c, handle, fd, ip, routeIp, port, result, wsaErr, data, len};
    g_observer(e);
}

// The exit lock, applied AFTER routing: it only sees what would REALLY go
// out on the wire. Returns true if the destination is allowed. A refusal
// touches NO host socket: no TCP handshake started, no byte sent. The
// engine logs nothing itself (it stays silent) — it reports via
// WX86_NET_REFUSED, and the embedder logs it however it wants.
static bool wx86_net_allow(Cpu* c, uint32_t handle, int fd,
                           uint32_t ip_be, uint16_t port, int kindRefus) {
    // The DECISION and its counters live in net_guard.cpp (a leaf unit);
    // this keeps only how to REPORT a refusal, which needs this layer's own
    // Winsock error code and observer.
    if (wx86_net_addr_allowed(ip_be)) return true;
    wx86_net_set_last_error(10013);   // WSAEACCES — the Winsock code for a forbidden send
    wx86_net_notify(WX86_NET_REFUSED, c, handle, fd, ip_be, ip_be, port,
                    kindRefus, 10013, nullptr, 0);
    return false;
}

uint32_t wx86_net_last_error() { return g_wsaLastErr; }
void wx86_net_set_last_error(uint32_t code) { g_wsaLastErr = code; }

uint32_t wx86_wsa_from_errno(int e) {
    switch (e) {
        case EWOULDBLOCK: return 10035; case EINPROGRESS: return 10036;
        case EALREADY: return 10037; case EISCONN: return 10056; case ECONNREFUSED: return 10061;
        case ETIMEDOUT: return 10060; case EHOSTUNREACH: return 10065; case ENETUNREACH: return 10051;
        default: return 10054; } // WSAECONNRESET as a catch-all
}

int wx86_connect_wait(int fd, int timeout_ms, uint32_t* outWsaErr) {
    pollfd pf{fd, POLLOUT, 0};
    int pr = wx86_poll_gilfree(&pf, timeout_ms);
    if (pr > 0) {
        int se = 0; socklen_t sl = sizeof se;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl);
        if (se == 0) return 0;
        uint32_t e = wx86_wsa_from_errno(se);
        wx86_net_set_last_error(e);
        if (outWsaErr) *outWsaErr = e;
        return -1;
    }
    wx86_net_set_last_error(10060);   // WSAETIMEDOUT
    if (outWsaErr) *outWsaErr = 10060;
    return -1;
}

// SO_RCVTIMEO per host fd (ms; absent/0 = wait forever, as on Windows).
static std::map<int, uint32_t> g_rcvTimeoMs;

int wx86_recv_blocking(int fd, void* buf, uint32_t len, bool blocking, int wait_ms, uint32_t* outWsaErr) {
    ssize_t n = ::recv(fd, buf, len, 0);
    // A BLOCKING Windows socket's semantics: recv waits INDEFINITELY unless
    // SO_RCVTIMEO is set, in which case it fails with WSAETIMEDOUT (10060) —
    // never WSAEWOULDBLOCK, a code a blocking Windows socket never returns.
    // The wait happens in GIL-released slices; it stops early if networking
    // gets disabled (shutdown, lock) or the socket is closed elsewhere.
    // `wait_ms` > 0 imposes an explicit caller bound (0 = Windows policy).
    if (n < 0 && blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        uint32_t limit = wait_ms > 0 ? (uint32_t)wait_ms : 0;
        if (!limit) { auto it = g_rcvTimeoMs.find(fd); if (it != g_rcvTimeoMs.end()) limit = it->second; }
        uint32_t waited = 0;
        for (;;) {
            const int slice = limit ? (int)((limit - waited) < 1000u ? (limit - waited) : 1000u) : 1000;
            pollfd pf{fd, POLLIN, 0};
            wx86_poll_gilfree(&pf, slice);
            n = ::recv(fd, buf, len, 0);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) break;
            waited += (uint32_t)slice;
            if (!wx86_net_enabled()) break;
            if (limit && waited >= limit) {
                wx86_net_set_last_error(10060); if (outWsaErr) *outWsaErr = 10060;
                return -1;
            }
        }
    }
    if (n >= 0) return (int)n;
    { uint32_t e = wx86_wsa_from_errno(errno); wx86_net_set_last_error(e); if (outWsaErr) *outWsaErr = e; }
    return -1;
}

int wx86_send_simple(int fd, const void* buf, uint32_t len, uint32_t* outWsaErr) {
    ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    { uint32_t e = wx86_wsa_from_errno(errno); wx86_net_set_last_error(e); if (outWsaErr) *outWsaErr = e; }
    return -1;
}

// --- select / __WSAFDIsSet -------------------------------------------------
// Winsock fd_set: { u32 fd_count; SOCKET fd_array[fd_count]; }. FD_SETSIZE is
// the caller's choice (64 by default), so fd_count is honored up to a bound
// that only rejects garbage.
static constexpr uint32_t kFdSetMaxEntries = 1024;

// Winsock reports select errors at once. A caller that retries on error would
// then never block, and under a run-to-block kernel a thread that never
// blocks starves every other guest thread. Errors are returned only once the
// requested timeout has elapsed (capped): Windows values, later.
static constexpr uint64_t kSelectErrorDelayCapMs = 1000;

// exceptfds reports urgent (OOB) data here; a failed non-blocking connect
// never shows up because connect() completes synchronously. Libcs that alias
// POLLPRI to POLLIN cannot tell urgent data apart, so exceptfds stays empty
// there instead of reporting ordinary data.
static constexpr bool kPollPriDistinct = POLLPRI != POLLIN;

static void wx86_net_notify_select(Cpu* c, int result, uint32_t wsaErr,
                                   int timeoutMs, uint32_t waitedMs) {
    if (!g_observer) return;
    WsockEvent e{WX86_NET_SELECT, c, 0, -1, 0, 0, 0, result, wsaErr, nullptr, 0, timeoutMs, waitedMs};
    g_observer(e);
}

namespace {
struct SelectEntry {
    uint32_t handle;
    int      fd;
    uint8_t  want;    // bit k: listed in set k (0 read, 1 write, 2 except)
    uint8_t  ready;   // bit k: reportable in set k
};
}

// select(nfds, readfds, writefds, exceptfds, timeout). nfds is ignored, as on
// Windows. The wait releases the GIL and stops on readiness, timeout,
// teardown, or networking being switched off.
static uint32_t wx86_select(Cpu& c) {
    const uint32_t setp[3] = { c.arg(1), c.arg(2), c.arg(3) };
    const uint32_t tvp = c.arg(4);
    const uint64_t t0 = wx86_now_ms();

    int64_t wait_ms = -1;   // NULL timeval: no limit
    bool tv_valid = true;
    if (tvp) {
        const int32_t sec = (int32_t)c.read_u32(tvp), usec = (int32_t)c.read_u32(tvp + 4);
        if (sec < 0 || usec < 0) tv_valid = false;
        else wait_ms = (int64_t)sec * 1000 + ((int64_t)usec + 999) / 1000;   // round up
    }
    const int ev_timeout = !tv_valid ? -2 : wait_ms < 0 ? -1 : (int)std::min<int64_t>(wait_ms, INT32_MAX);
    const uint64_t err_at = t0 + (tv_valid && wait_ms >= 0
                                  ? std::min<uint64_t>((uint64_t)wait_ms, kSelectErrorDelayCapMs)
                                  : kSelectErrorDelayCapMs);
    auto elapsed = [t0]() { return wx86_now_ms() - t0; };
    auto fail = [&](uint32_t code) -> uint32_t {
        const uint64_t now = wx86_now_ms();
        if (now < err_at && !wx86_poll_shutdown_requested())
            wx86_poll_gilfree_n(nullptr, 0, (int)(err_at - now));
        wx86_net_set_last_error(code);
        wx86_net_notify_select(&c, -1, code, ev_timeout, (uint32_t)elapsed());
        return 0xFFFFFFFFu;
    };

    if (!wx86_net_enabled()) return fail(10050);   // WSAENETDOWN
    if (!tv_valid) return fail(10022);             // WSAEINVAL

    // Snapshot the sets before any wait: guest memory can change while the
    // GIL is released.
    std::vector<uint32_t> lists[3];
    std::vector<SelectEntry> ents;
    std::map<uint32_t, size_t> index;
    for (int k = 0; k < 3; k++) {
        if (!setp[k]) continue;
        const uint32_t n = c.read_u32(setp[k]);
        if (n > kFdSetMaxEntries) return fail(10014);   // WSAEFAULT
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t h = c.read_u32(setp[k] + 4 + i * 4);
            lists[k].push_back(h);
            auto it = index.find(h);
            if (it == index.end()) {
                const int fd = wx86_sock_fd(h);
                if (fd < 0) return fail(10038);         // WSAENOTSOCK
                it = index.emplace(h, ents.size()).first;
                ents.push_back({h, fd, 0, 0});
            }
            ents[it->second].want |= (uint8_t)(1u << k);
        }
    }
    if (ents.empty()) return fail(10022);   // WSAEINVAL: no socket in any set

    std::vector<pollfd> pf(ents.size());
    for (size_t i = 0; i < ents.size(); i++) {
        short ev = 0;
        if (ents[i].want & 1) ev |= POLLIN;
        if (ents[i].want & 2) ev |= POLLOUT;
        if ((ents[i].want & 4) && kPollPriDistinct) ev |= POLLPRI;
        pf[i].fd = ev ? ents[i].fd : -1;   // poll ignores negative fds
        pf[i].events = ev;
    }

    for (;;) {
        int64_t chunk = 1000;
        if (wait_ms >= 0) chunk = std::min<int64_t>(chunk, std::max<int64_t>(0, wait_ms - (int64_t)elapsed()));
        bool live = false;
        for (auto& p : pf) { p.revents = 0; live |= p.fd >= 0; }
        int r = 0;
        if (!live) {
            if (chunk > 0) wx86_poll_gilfree_n(nullptr, 0, (int)chunk);
        } else if (chunk > 0) {
            r = wx86_poll_gilfree_n(pf.data(), (nfds_t)pf.size(), (int)chunk);
        } else {
            r = ::poll(pf.data(), (nfds_t)pf.size(), 0);
            if (r < 0 && errno == EINTR) r = 0;
        }
        if (r < 0) return fail(10050);   // host poll failed: treat the network as down

        // Another guest thread may have closed a socket during the wait.
        for (const auto& e : ents)
            if (wx86_sock_fd(e.handle) != e.fd) return fail(10038);

        bool any = false;
        if (r > 0) {
            for (size_t i = 0; i < ents.size(); i++) {
                const short rv = pf[i].revents;
                uint8_t ready = 0;
                // Windows marks a closed or reset connection readable only.
                if ((ents[i].want & 1) && (rv & (POLLIN | POLLHUP | POLLERR))) ready |= 1;
                if ((ents[i].want & 2) && (rv & POLLOUT)) ready |= 2;
                if ((ents[i].want & 4) && kPollPriDistinct && (rv & POLLPRI)) ready |= 4;
                ents[i].ready = ready;
                any |= ready != 0;
                // Woke on a condition none of this socket's sets can report:
                // stop watching it for this call rather than wake on it again.
                if (!ready && rv) pf[i].fd = -1;
            }
        }
        if (any) break;
        if (wait_ms >= 0 && (int64_t)elapsed() >= wait_ms) break;
        if (wx86_poll_shutdown_requested()) break;
        if (!wx86_net_enabled()) return fail(10050);
    }

    uint32_t total = 0;
    for (int k = 0; k < 3; k++) {
        if (!setp[k]) continue;
        uint32_t w = 0;
        for (uint32_t h : lists[k])
            if ((ents[index.find(h)->second].ready >> k) & 1) c.write_u32(setp[k] + 4 + 4 * w++, h);
        c.write_u32(setp[k], w);
        total += w;
    }
    wx86_net_notify_select(&c, (int)total, 0, ev_timeout, (uint32_t)elapsed());
    return total;
}

// __WSAFDIsSet(s, set): the function behind FD_ISSET.
static uint32_t wx86_fd_isset(Cpu& c) {
    const uint32_t s = c.arg(0), set = c.arg(1);
    if (!set) return 0u;
    const uint32_t n = std::min(c.read_u32(set), kFdSetMaxEntries);
    for (uint32_t i = 0; i < n; i++)
        if (c.read_u32(set + 4 + i * 4) == s) return 1u;
    return 0u;
}

void win32_shims_wsock32_install(Bridge& br) {
    // NOTE for whoever edits this next: register EACH (dll, ordinal) pair via
    // its OWN direct `br.register_shim_ordinal(...)` call inside this
    // function's body. Do not factor the "same ordinal on both DLL names"
    // pattern into a second-level helper that itself calls a helper —
    // tools/shim_seq.py's static parser only resolves ONE level of local
    // lambda helper (it looks for a direct br.register_shim(_ordinal) call
    // inside a helper's own body); a helper that calls another helper
    // resolves to zero targets and every key it registers vanishes
    // SILENTLY from the safety-net table — e.g. a `BOTH(ord,...)` wrapping
    // `REGORD(dll,ord,...)` twice. This is caught only by shim_seq.sh's
    // ENSEMBLE check running before every commit, not by visual inspection.
    auto REGORD = [&](const char* dll, uint32_t ord, uint32_t ac, std::function<uint32_t(Cpu&)> fn) {
        Shim s;
        s.argc = ac;
        s.stdcall_cleanup = true;
        s.tag = std::string(dll) + "!#" + std::to_string(ord);
        s.fn = std::move(fn);
        br.register_shim_ordinal(dll, ord, s);
    };

    // Byte order — pure bit manipulation, zero state.
    auto htons_fn = [](Cpu& c) { uint32_t v = c.arg(0) & 0xffff; return ((v >> 8) | (v << 8)) & 0xffff; };
    REGORD("WSOCK32.dll", 9, 1, htons_fn);
    REGORD("WS2_32.dll", 9, 1, htons_fn);
    auto ntohs_fn = htons_fn;  // same bit-swap, different ordinal (BSD historically shares the body)
    REGORD("WSOCK32.dll", 15, 1, ntohs_fn);
    REGORD("WS2_32.dll", 15, 1, ntohs_fn);
    auto htonl_fn = [](Cpu& c) { return __builtin_bswap32(c.arg(0)); };
    REGORD("WSOCK32.dll", 8, 1, htonl_fn);
    REGORD("WS2_32.dll", 8, 1, htonl_fn);
    auto ntohl_fn = htonl_fn;
    REGORD("WSOCK32.dll", 14, 1, ntohl_fn);
    REGORD("WS2_32.dll", 14, 1, ntohl_fn);

    // WSAStartup(wVersionRequested, WSADATA*) / WSACleanup(): fill/no-op on a
    // caller-supplied buffer. Wrong argc here once sent Fog's init executing
    // .rdata (a stdcall ESP-balance bug, not a networking bug) — keep argc
    // exact if this is ever touched again.
    auto wsastartup_fn = [](Cpu& c) {
        uint32_t p = c.arg(1);
        if (p) {
            std::vector<uint8_t> z(400, 0);
            c.write(p, z.data(), 400);
            uint16_t v = (uint16_t)(c.arg(0) & 0xffff);
            c.write(p, &v, 2);
            c.write(p + 2, &v, 2);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 115, 2, wsastartup_fn);
    REGORD("WS2_32.dll", 115, 2, wsastartup_fn);
    auto wsacleanup_fn = [](Cpu&) { return 0u; };
    REGORD("WSOCK32.dll", 116, 0, wsacleanup_fn);
    REGORD("WS2_32.dll", 116, 0, wsacleanup_fn);

    auto wsaasyncselect_fn = [](Cpu&) { return 0u; };  // WSAAsyncSelect(s,hWnd,wMsg,lEvent) — stub
    REGORD("WSOCK32.dll", 101, 4, wsaasyncselect_fn);
    REGORD("WS2_32.dll", 101, 4, wsaasyncselect_fn);
    auto wsaisblocking_fn = [](Cpu&) { return 0u; };  // WSAIsBlocking -> FALSE
    REGORD("WSOCK32.dll", 112, 0, wsaisblocking_fn);
    REGORD("WS2_32.dll", 112, 0, wsaisblocking_fn);

    // gethostname: "vita" is the winx86 execution PLATFORM (Vita), not a D2
    // literal — same category as vita_kb.h/vita_audio.cpp already living here.
    auto gethostname_fn = [](Cpu& c) {
        uint32_t b = c.arg(0);
        if (b) c.write(b, "vita", 5);
        return 0u;
    };
    REGORD("WSOCK32.dll", 57, 2, gethostname_fn);
    REGORD("WS2_32.dll", 57, 2, gethostname_fn);

    // --- real generic socket primitives ---------------------------------
    auto closesocket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd >= 0) { g_rcvTimeoMs.erase(fd); ::close(fd); wx86_sock_erase(c.arg(0)); }
        return 0u;
    };
    REGORD("WSOCK32.dll", 3, 1, closesocket_fn);
    REGORD("WS2_32.dll", 3, 1, closesocket_fn);

    auto getpeername_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        if (::getpeername(fd, (sockaddr*)sa, &sl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return 0u;
    };
    REGORD("WSOCK32.dll", 5, 3, getpeername_fn);
    REGORD("WS2_32.dll", 5, 3, getpeername_fn);

    auto getsockname_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        if (::getsockname(fd, (sockaddr*)sa, &sl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return 0u;
    };
    REGORD("WSOCK32.dll", 6, 3, getsockname_fn);
    REGORD("WS2_32.dll", 6, 3, getsockname_fn);

    // getsockopt(s,level,optname,optval*,optlen*): the generic passthrough.
    // D2 polls SO_ERROR after a non-blocking connect signals writable —
    // that USE is d2vita's connect flow, but the shim body itself is a
    // plain Winsock->POSIX option translation, nothing D2-specific in it.
    auto getsockopt_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        int level = (int)c.arg(1) == 0xffff ? SOL_SOCKET : (int)c.arg(1);
        int opt = (int)c.arg(2);
        if (c.arg(1) == 0xffff && opt == 0x1007) opt = SO_ERROR;
        else if (c.arg(1) == 0xffff && opt == 0x1008) opt = SO_TYPE;
        uint8_t buf[64] = {0}; socklen_t bl = c.arg(4) ? c.read_u32(c.arg(4)) : 4; if (bl > 64) bl = 64;
        if (::getsockopt(fd, level, opt, buf, &bl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1) == 0xffff && (int)c.arg(2) == 0x1007 && bl >= 4) {
            int he; std::memcpy(&he, buf, 4);
            uint32_t w = he ? wx86_wsa_from_errno(he) : 0u;
            std::memcpy(buf, &w, 4);
        }
        if (c.arg(3)) c.write(c.arg(3), buf, bl);
        if (c.arg(4)) c.write_u32(c.arg(4), bl);
        return 0u;
    };
    REGORD("WSOCK32.dll", 7, 5, getsockopt_fn);
    REGORD("WS2_32.dll", 7, 5, getsockopt_fn);

    auto setsockopt_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        int lvl = (int)c.arg(1), opt = (int)c.arg(2);
        uint32_t vp = c.arg(3), vl = c.arg(4);
        std::vector<uint8_t> v(vl ? vl : 4, 0);
        if (vp && vl) c.read(vp, v.data(), vl);
        if (lvl == 0xffff && opt == 0x0008) {   // SO_KEEPALIVE
            int on = v.size() >= 4 ? *(int*)v.data() : 1;
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        } else if (lvl == 0xffff && opt == 0x1006) {   // SO_RCVTIMEO (DWORD ms, 0 = infinite)
            g_rcvTimeoMs[fd] = v.size() >= 4 ? *(uint32_t*)v.data() : 0u;
        } else if (lvl == 6 && opt == 1) {      // TCP_NODELAY
            int on = v.size() >= 4 ? *(int*)v.data() : 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 21, 5, setsockopt_fn);
    REGORD("WS2_32.dll", 21, 5, setsockopt_fn);

    auto shutdown_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd >= 0) ::shutdown(fd, (int)c.arg(1));
        return 0u;
    };
    REGORD("WSOCK32.dll", 22, 2, shutdown_fn);
    REGORD("WS2_32.dll", 22, 2, shutdown_fn);

    // ioctlsocket(s,cmd,u_long*): only FIONBIO is meaningful here (guest
    // blocking-mode flag; the host fd stays non-blocking regardless, see
    // wx86_recv_blocking). WSOCK32 and WS2_32 disagree on the ordinal
    // (#12 vs #10) — same body, two explicit registrations.
    auto ioctlsocket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        if (c.arg(1) == 0x8004667eu) {   // FIONBIO
            uint32_t v = c.arg(2) ? c.read_u32(c.arg(2)) : 0;
            wx86_sock_set_nonblock(c.arg(0), v != 0);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 12, 3, ioctlsocket_fn);
    REGORD("WS2_32.dll", 10, 3, ioctlsocket_fn);

    // --- address helpers: inet_addr / inet_ntoa / gethostbyname ------------
    // These return data to the guest, so they need to write into GUEST
    // memory (guest_scratch.h).
    //
    // Unlike the rest of this file, the two DLLs' ordinals do NOT match
    // here:
    //        WSOCK32: 10=inet_addr 11=inet_ntoa 12=ioctlsocket
    //        WS2_32:  10=ioctlsocket 11=inet_addr 12=inet_ntoa
    // Hence four explicit registrations below, each on its correct ordinal,
    // instead of the usual same-ordinal-both-DLLs pattern.
    auto inet_addr_fn = [](Cpu& c) -> uint32_t {
        std::string s = wx86_guest_cstr(c, c.arg(0));
        in_addr a;
        if (!s.empty() && inet_aton(s.c_str(), &a)) return a.s_addr;
        return 0xFFFFFFFFu;   // INADDR_NONE
    };
    REGORD("WSOCK32.dll", 10, 1, inet_addr_fn);
    REGORD("WS2_32.dll", 11, 1, inet_addr_fn);

    // inet_ntoa(in_addr) -> char*: the string must survive past return, so it
    // lives in guest scratch. ONE cache entry per distinct address: without
    // it, every call would permanently leak ~16 bytes (the allocator never
    // frees — see guest_scratch.h's contract), and a client formatting an
    // address every frame would eventually exhaust the arena.
    auto inet_ntoa_fn = [](Cpu& c) -> uint32_t {
        static std::map<uint32_t, uint32_t> cache;   // network addr -> guest address
        const uint32_t a = c.arg(0);
        auto it = cache.find(a);
        if (it != cache.end()) return it->second;
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u",
                      a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, (a >> 24) & 0xff);
        const uint32_t p = wx86_scratch_put_cstr(c, b);
        if (p) cache[a] = p;
        return p;
    };
    REGORD("WSOCK32.dll", 11, 1, inet_ntoa_fn);
    REGORD("WS2_32.dll", 12, 1, inet_ntoa_fn);

    // gethostbyname(name) -> hostent*: same story, bigger. The returned
    // structure (hostent + address list + alias list + name copy) made FIVE
    // permanent allocations per call — a client that re-resolves its server
    // on every connection attempt would run straight to arena exhaustion.
    // Fixed with one cache entry per name; the structure is built ONCE. The
    // cache deliberately does not track DNS changes: these Winsock
    // structures have process lifetime anyway, which their contract allows.
    auto gethostbyname_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        const std::string host = wx86_guest_cstr(c, c.arg(0));
        static std::map<std::string, uint32_t> cache;
        auto it = cache.find(host);
        if (it != cache.end()) return it->second;
        const uint32_t addr = wx86_net_resolve(host.c_str());
        // The engine stays silent: a resolution failure goes to the
        // observer, not to a log it has no notion of. This is often the
        // first failure to show up on an embedded platform, and it must
        // not stay invisible.
        if (!addr) {
            wx86_net_set_last_error(11001);   // WSAHOST_NOT_FOUND
            wx86_net_notify(WX86_NET_RESOLVE, &c, 0, -1, 0, 0, 0, -1, 11001,
                            (const uint8_t*)host.c_str(), (int)host.size());
            return 0u;
        }
        // hostent { char* h_name; char** h_aliases; short h_addrtype;
        //           short h_length; char** h_addr_list; }
        const uint32_t namep = wx86_scratch_put_cstr(c, host.c_str());
        const uint32_t addrbuf = wx86_scratch_alloc(4);
        const uint32_t addrlist = wx86_scratch_alloc(8);
        const uint32_t aliases = wx86_scratch_alloc(4);
        const uint32_t he = wx86_scratch_alloc(16);
        if (!namep || !addrbuf || !addrlist || !aliases || !he) {
            wx86_net_set_last_error(11001);
            return 0u;   // scratch exhausted: fail cleanly, don't write at address 0
        }
        c.write_u32(addrbuf, addr);
        c.write_u32(addrlist, addrbuf);
        c.write_u32(addrlist + 4, 0);
        c.write_u32(aliases, 0);
        c.write_u32(he + 0, namep);
        c.write_u32(he + 4, aliases);
        uint16_t at = 2 /*AF_INET*/, ln = 4;
        c.write(he + 8, &at, 2);     // h_addrtype and h_length are SHORTs
        c.write(he + 10, &ln, 2);    // packed together, not two 32-bit words
        c.write_u32(he + 12, addrlist);
        cache[host] = he;
        wx86_net_notify(WX86_NET_RESOLVE, &c, 0, -1, addr, addr, 0, 0, 0,
                        (const uint8_t*)host.c_str(), (int)host.size());
        return he;
    };
    REGORD("WSOCK32.dll", 52, 1, gethostbyname_fn);
    REGORD("WS2_32.dll", 52, 1, gethostbyname_fn);

    // socket(af,type,proto): real socket, host fd kept non-blocking
    // (protects the cooperative scheduler — see wx86_recv_blocking for how
    // guest-visible blocking mode is still honored).
    auto socket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = ::socket(AF_INET, (int)c.arg(1) == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
        if (fd < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        wx86_net_set_nonblock(fd, true);
        return wx86_sock_alloc(fd);
    };
    REGORD("WSOCK32.dll", 23, 3, socket_fn);
    REGORD("WS2_32.dll", 23, 3, socket_fn);

    // connect(s, sockaddr*, len). Generic all the way through: read the
    // destination, apply the generic route override if one is set, report the
    // attempt to the observer, then do the host connect — completing a
    // non-blocking one synchronously, because a guest whose own timeout is
    // driven by an emulated clock can "time out" while the real TCP handshake
    // is still in flight. No port literal, no protocol knowledge: which ports
    // matter, and what to do when one of them connects, is the embedder's
    // business and reaches it through the observer.
    auto connect_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16]; c.read(c.arg(1), sa, 16);
        uint32_t dip; std::memcpy(&dip, sa + 4, 4);
        uint16_t dport = (uint16_t)((sa[2] << 8) | sa[3]);
        wx86_sock_set_port(c.arg(0), dport);
        uint32_t rip = dip;
        if (g_routeIp) {
            // Loopback, "any" and broadcast are left alone: rewriting those
            // would break a guest talking to itself.
            uint8_t hi = dip & 0xff;
            if (hi != 127 && hi != 0 && dip != 0xffffffffu) {
                rip = g_routeIp;
                std::memcpy(sa + 4, &rip, 4);
                c.write(c.arg(1), sa, 16);
            }
        }
        // Lock applied AFTER routing: the destination actually dialed is
        // what's judged, and a refusal has NO side effect (no SYN goes out).
        if (!wx86_net_allow(&c, c.arg(0), fd, rip, dport, WX86_NET_CONNECT))
            return 0xFFFFFFFFu;
        wx86_net_notify(WX86_NET_CONNECT, &c, c.arg(0), fd, dip, rip, dport, 0, 0, nullptr, 0);
        int r = ::connect(fd, (sockaddr*)sa, (socklen_t)c.arg(2));
        if (r == 0) {
            wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport, 0, 0, nullptr, 0);
            return 0u;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
            uint32_t we = 0;
            int rc = wx86_connect_wait(fd, 8000, &we);
            wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport,
                            rc == 0 ? 1 : -1, rc == 0 ? 0u : we, nullptr, 0);
            return rc == 0 ? 0u : 0xFFFFFFFFu;
        }
        uint32_t e = wx86_wsa_from_errno(errno);
        wx86_net_set_last_error(e);
        wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport, -1, e, nullptr, 0);
        return 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 4, 3, connect_fn);
    REGORD("WS2_32.dll", 4, 3, connect_fn);

    auto recv_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        std::vector<uint8_t> b(c.arg(2));
        uint32_t we = 0;
        int n = wx86_recv_blocking(h.fd, b.data(), (uint32_t)b.size(), !h.nonblock, 0, &we);
        if (n > 0) {
            c.write(c.arg(1), b.data(), (uint32_t)n);
            wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port, n, 0, b.data(), n);
            return (uint32_t)n;
        }
        wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port,
                        n, n < 0 ? we : 0u, nullptr, 0);
        return n == 0 ? 0u : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 16, 4, recv_fn);
    REGORD("WS2_32.dll", 16, 4, recv_fn);

    auto send_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        std::vector<uint8_t> b(c.arg(2));
        if (c.arg(2)) c.read(c.arg(1), b.data(), c.arg(2));
        uint32_t we = 0;
        int n = wx86_send_simple(fd, b.data(), (uint32_t)b.size(), &we);
        wx86_net_notify(WX86_NET_SEND, &c, c.arg(0), fd, 0, 0, wx86_sock_port(c.arg(0)),
                        n, n < 0 ? we : 0u, b.data(), (int)b.size());
        return n >= 0 ? (uint32_t)n : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 19, 4, send_fn);
    REGORD("WS2_32.dll", 19, 4, send_fn);

    REGORD("WSOCK32.dll", 18, 5, wx86_select);
    REGORD("WS2_32.dll", 18, 5, wx86_select);
    REGORD("WSOCK32.dll", 151, 2, wx86_fd_isset);
    REGORD("WS2_32.dll", 151, 2, wx86_fd_isset);

    // ---- sendto (#20) / recvfrom (#17): real bodies, not the default shim -
    // argc is what matters here: a wrong stdcall argc shifts the stack on
    // EVERY call.
    //   int sendto  (SOCKET, const char* buf, int len, int flags,
    //                const struct sockaddr* to, int tolen);        -> 6
    //   int recvfrom(SOCKET, char* buf, int len, int flags,
    //                struct sockaddr* from, int* fromlen);         -> 6
    auto sendto_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        const uint32_t len = c.arg(2), pto = c.arg(4), tolen = c.arg(5);
        uint8_t sa[16] = {0};
        if (pto) c.read(pto, sa, tolen > 16 ? 16 : (tolen ? tolen : 16));
        uint32_t dip; std::memcpy(&dip, sa + 4, 4);
        const uint16_t dport = (uint16_t)((sa[2] << 8) | sa[3]);
        // SAME generic route as connect: a local test rig must be able to
        // redirect a UDP probe without touching the guest.
        uint32_t rip = dip;
        if (g_routeIp) {
            const uint8_t hi = dip & 0xff;
            if (hi != 127 && hi != 0 && dip != 0xffffffffu) {
                rip = g_routeIp; std::memcpy(sa + 4, &rip, 4);
            }
        }
        if (!wx86_net_allow(&c, c.arg(0), fd, rip, dport, WX86_NET_SEND))
            return 0xFFFFFFFFu;               // nothing goes out
        std::vector<uint8_t> b(len);
        if (len) c.read(c.arg(1), b.data(), len);
        ssize_t n = ::sendto(fd, b.data(), b.size(), MSG_NOSIGNAL,
                             (sockaddr*)sa, pto ? (socklen_t)16 : (socklen_t)0);
        uint32_t we = 0;
        if (n < 0) { we = wx86_wsa_from_errno(errno); wx86_net_set_last_error(we); }
        wx86_net_notify(WX86_NET_SEND, &c, c.arg(0), fd, dip, rip, dport,
                        (int)n, we, b.data(), (int)b.size());
        return n >= 0 ? (uint32_t)n : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 20, 6, sendto_fn);
    REGORD("WS2_32.dll", 20, 6, sendto_fn);

    auto recvfrom_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        const uint32_t len = c.arg(2), pfrom = c.arg(4), pflen = c.arg(5);
        std::vector<uint8_t> b(len);
        sockaddr_in sa{}; socklen_t sl = sizeof sa;
        ssize_t n = ::recvfrom(h.fd, b.data(), b.size(), 0, (sockaddr*)&sa, &sl);
        if (n < 0 && !h.nonblock && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // BOUNDED wait (3s): a UDP probe that gets no response must
            // degrade gracefully, never freeze the runtime.
            pollfd pf{h.fd, POLLIN, 0};
            wx86_poll_gilfree(&pf, 3000);
            sl = sizeof sa;
            n = ::recvfrom(h.fd, b.data(), b.size(), 0, (sockaddr*)&sa, &sl);
        }
        if (n < 0) {
            const uint32_t we = wx86_wsa_from_errno(errno);
            wx86_net_set_last_error(we);
            wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port, -1, we, nullptr, 0);
            return 0xFFFFFFFFu;
        }
        // Defense in depth: under the lock, a datagram ARRIVING FROM a
        // non-private address is dropped too — otherwise the lock would
        // only cover half the conversation.
        const uint32_t sip = (uint32_t)sa.sin_addr.s_addr;
        if (!wx86_net_allow(&c, c.arg(0), h.fd, sip, (uint16_t)ntohs(sa.sin_port), WX86_NET_RECV)) {
            wx86_net_set_last_error(10035);   // WSAEWOULDBLOCK: "nothing for you"
            return 0xFFFFFFFFu;
        }
        if (n > 0) c.write(c.arg(1), b.data(), (uint32_t)n);
        if (pfrom) {
            uint8_t out[16] = {0}; out[0] = 2; out[1] = 0;
            const uint16_t sp = (uint16_t)ntohs(sa.sin_port);
            out[2] = (uint8_t)(sp >> 8); out[3] = (uint8_t)(sp & 0xff);
            std::memcpy(out + 4, &sa.sin_addr.s_addr, 4);
            const uint32_t cap = pflen ? c.read_u32(pflen) : 16u;
            c.write(pfrom, out, cap < 16 ? cap : 16);
            if (pflen) c.write_u32(pflen, 16);
        }
        wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, sip, sip,
                        (uint16_t)ntohs(sa.sin_port), (int)n, 0, b.data(), (int)n);
        return (uint32_t)n;
    };
    REGORD("WSOCK32.dll", 17, 6, recvfrom_fn);
    REGORD("WS2_32.dll", 17, 6, recvfrom_fn);

    // accept/bind/listen: real POSIX passthrough. No D2 client ever
    // exercises this path (D2 is a pure Winsock client): implemented
    // correctly, but validated only in isolation (a standalone
    // connect/accept round-trip against this build), never by real D2
    // traffic.
    auto bind_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; c.read(c.arg(1), sa, 16);
        if (::bind(fd, (sockaddr*)sa, (socklen_t)c.arg(2)) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        return 0u;
    };
    REGORD("WSOCK32.dll", 2, 3, bind_fn);
    REGORD("WS2_32.dll", 2, 3, bind_fn);

    auto listen_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        if (::listen(fd, (int)c.arg(1)) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        return 0u;
    };
    REGORD("WSOCK32.dll", 13, 2, listen_fn);
    REGORD("WS2_32.dll", 13, 2, listen_fn);

    auto accept_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        int nfd = ::accept(fd, (sockaddr*)sa, &sl);
        if (nfd < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        wx86_net_set_nonblock(nfd, true);
        uint32_t gh = wx86_sock_alloc(nfd);
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return gh;
    };
    REGORD("WSOCK32.dll", 1, 3, accept_fn);
    REGORD("WS2_32.dll", 1, 3, accept_fn);

    // WSAGetLastError(): 10093 (WSANOTINITIALISED) when networking is off,
    // else the shared last-error store every shim above (and d2vita's own
    // connect/recv/send) sets on failure. Trivially generic itself — moved
    // here alongside the store it reads, so there is one owner instead of
    // this ordinal reading a d2vita-local copy of winx86-owned state.
    auto wsagetlasterror_fn = [](Cpu&) -> uint32_t {
        return wx86_net_enabled() ? wx86_net_last_error() : 10093u;
    };
    REGORD("WSOCK32.dll", 111, 0, wsagetlasterror_fn);
    REGORD("WS2_32.dll", 111, 0, wsagetlasterror_fn);
}
