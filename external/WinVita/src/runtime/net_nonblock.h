// src/runtime/net_nonblock.h — generic socket-nonblocking and hostname
// resolution helpers: set O_NONBLOCK on an arbitrary fd and resolve an
// arbitrary hostname to an IPv4 address, with no protocol- or
// application-specific assumptions.
#pragma once
#include <cstdint>

namespace d2rt {

// Sets (or clears) non-blocking mode on fd, verified by read-back — on Vita
// an fcntl() can report success without changing anything, so a caller that
// trusted the return value alone could keep a socket it believes is
// non-blocking but isn't. See net_nonblock.cpp for why no third (sceNet)
// fallback exists: proven-by-disassembly that fcntl already reaches the
// right Sony socket option, a raw sceNetSetsockopt call would target the
// WRONG object (libc's fd-to-Sony-id remap is private to fcntl/ioctl).
bool wx86_net_set_nonblock(int fd, bool on);

// 0 unknown, 1 fcntl, 2 ioctl FIONBIO (host only) — which method the most
// recent wx86_net_set_nonblock() call used. Diagnostic only.
int wx86_net_nonblock_method();

// Resolves host to an IPv4 address in network byte order, or 0 on failure.
// Tries a literal dotted-quad first; falls back to the platform resolver
// (Sony's sceNetResolver on Vita, gethostbyname() elsewhere).
uint32_t wx86_net_resolve(const char* host);

// Raw platform resolver return code from the most recent wx86_net_resolve()
// call (Sony sceNetResolverCreate/StartNtoa result on Vita; unused, always 0,
// elsewhere). Diagnostic only — distinguishes "resolver object failed to
// create" from "created fine, no answer" when a name lookup fails.
int wx86_net_last_resolve_rc();

// Count of name resolutions refused by the exit lock
// (wx86_net_set_private_only) since startup. A dotted-quad literal never
// consults the guard, so it's never counted here. Needed as a positive
// signal: without it, "no request went out" and "this code path was never
// reached" look identical.
unsigned long long wx86_net_resolves_refused();

} // namespace d2rt
