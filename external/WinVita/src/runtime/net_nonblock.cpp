// src/runtime/net_nonblock.cpp — see net_nonblock.h.
#include "runtime/net_nonblock.h"
#include "runtime/net_guard.h"   // wx86_net_private_only() — leaf unit, see its header
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#ifndef __vita__
#include <sys/ioctl.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2common/net.h>
#else
#include <netdb.h>
#include <cstring>
#endif

namespace d2rt {

static int g_nbMethod = 0;
static int g_resolveRc = 0;
static unsigned long long g_resolveRefused = 0;

int wx86_net_nonblock_method() { return g_nbMethod; }
unsigned long long wx86_net_resolves_refused() { return g_resolveRefused; }
int wx86_net_last_resolve_rc() { return g_resolveRc; }

bool wx86_net_set_nonblock(int fd, bool on) {
    // Reset before every attempt: otherwise a failing call would still
    // display the previous success's method.
    g_nbMethod = 0;
    // 1) POSIX fcntl, VERIFIED by read-back: on Vita the call can "succeed"
    //    without changing anything — a read-back costs one syscall and
    //    avoids a false diagnostic.
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        int want = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        if (fcntl(fd, F_SETFL, want) == 0) {
            int back = fcntl(fd, F_GETFL, 0);
            if (back >= 0 && ((back & O_NONBLOCK) != 0) == on) { g_nbMethod = 1; return true; }
        }
    }
#ifndef __vita__
    // 2) ioctl FIONBIO. Host only: the VitaSDK has neither <sys/ioctl.h> nor
    //    FIONBIO, and the console equivalent is already covered by method 1.
    { int v = on ? 1 : 0; if (ioctl(fd, FIONBIO, &v) == 0) { g_nbMethod = 2; return true; } }
#endif
    // NO third method via sceNetSetsockopt: it would be wrong AND
    // dangerous. Proven by disassembling the SDK's libc:
    //   - socket.o: socket() stores the id returned by sceNetSocket in
    //     __vita_fdmap and returns a libc descriptor INDEX, not the Sony
    //     id. Passing that raw fd to sceNetSetsockopt would therefore
    //     target a different object, possibly a valid third-party socket —
    //     in which case the read-back would confirm success on the WRONG
    //     socket, this function would return true for a socket that stayed
    //     blocking, and the next recv would freeze the single runner.
    //   - lib_a-fcntl.o: fcntl() translates the descriptor
    //     (__vita_fd_grab) then calls sceNetGetsockopt/sceNetSetsockopt on
    //     option 0x1100 (SCE_NET_SO_NBIO). Method 1 IS therefore already
    //     the Sony option, applied to the right id.
    // A method that cannot work but can lie is worse than no method.
    return false;
}

uint32_t wx86_net_resolve(const char* host) {
    if (!host || !*host) return 0;
    in_addr a;
    if (inet_aton(host, &a)) return a.s_addr;

    // The exit lock must gate name resolution too, not just
    // connect/sendto/recvfrom: a DNS query leaves the machine in the clear,
    // naming the target host, even when the connect that would follow gets
    // refused — the requested name is often more revealing than the
    // resolved address.
    //
    // When armed, only the dotted-quad literal handled above is answered;
    // any name is refused before any packet leaves. This isn't an arbitrary
    // restriction: the faithful way to point a client at a private server is
    // to give it that address in its own configuration (see
    // wx86_net_set_redirect in win32_shims_wsock32.cpp), not to have it
    // resolve a public name. An embedder that genuinely needs local-network
    // name resolution disarms the lock — that's its own policy, not the
    // engine's.
    if (::wx86_net_private_only()) {
        ++g_resolveRefused;
        g_resolveRc = 0;   // no resolver was created: no code to report
        return 0;
    }
#ifdef __vita__
    // gethostbyname() itself is not called on console: per
    // lib_a-gethostbyname.o, it wraps sceNetResolverCreate +
    // sceNetResolverStartNtoa with a delay of ZERO and zero retries, making
    // it the file's only unbounded call — under the cooperative scheduler, a
    // call that never returns freezes the whole game, with no diagnostic.
    // The resolver is therefore driven directly below, with an explicit
    // bound.
    { int rid = sceNetResolverCreate("winx86", nullptr, 0);
        g_resolveRc = rid;   // < 0 here = creation failed, not the delay
        if (rid >= 0) {
            // addr starts at 0: a StartNtoa that returns >= 0 without
            // writing anything must read as a failure, never as the
            // 0.0.0.0 address that would then reach connect().
            SceNetInAddr addr; addr.s_addr = 0;
            // Timeout unit is undocumented by the header/SDK reference: it
            // is MICROSECONDS, not seconds — hence 5 * 1000 * 1000 below
            // for a 5s timeout.
            int r = sceNetResolverStartNtoa(rid, host, &addr, 5 * 1000 * 1000, 1, 0);
            g_resolveRc = r;   // >= 0 but addr null = resolved without an answer
            sceNetResolverDestroy(rid);
            if (r >= 0 && addr.s_addr != 0) return addr.s_addr;
        } }
#else
    if (hostent* he = gethostbyname(host))
        if (he->h_addr_list && he->h_addr_list[0]) {
            uint32_t v; std::memcpy(&v, he->h_addr_list[0], 4); return v;
        }
#endif
    return 0;
}

} // namespace d2rt
