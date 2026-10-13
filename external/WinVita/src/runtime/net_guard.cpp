// src/runtime/net_guard.cpp — see net_guard.h.
#include "runtime/net_guard.h"

static bool               g_privOnly = false;
static unsigned long long g_refused = 0, g_allowed = 0;

void wx86_net_set_private_only(bool on) { g_privOnly = on; }
bool wx86_net_private_only() { return g_privOnly; }
unsigned long long wx86_net_refused() { return g_refused; }
unsigned long long wx86_net_allowed() { return g_allowed; }

// "Can this address reach the public internet?" — a socket-layer question,
// with no awareness of protocol or of what runs on top. ip is in NETWORK
// byte order, as stored in a sockaddr.
bool wx86_net_addr_is_private(uint32_t ip_be) {
    const uint8_t a = (uint8_t)(ip_be & 0xff), b = (uint8_t)((ip_be >> 8) & 0xff);
    if (a == 127) return true;                       // loopback
    if (ip_be == 0) return true;                     // unspecified
    if (ip_be == 0xFFFFFFFFu) return true;           // LAN broadcast
    if (a == 10) return true;                        // 10/8
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12
    if (a == 192 && b == 168) return true;           // 192.168/16
    if (a == 169 && b == 254) return true;           // 169.254/16 link-local
    if (a >= 224 && a <= 239) return true;           // multicast
    return false;
}

bool wx86_net_addr_allowed(uint32_t ip_be) {
    if (!g_privOnly || wx86_net_addr_is_private(ip_be)) { g_allowed++; return true; }
    g_refused++;
    return false;
}
