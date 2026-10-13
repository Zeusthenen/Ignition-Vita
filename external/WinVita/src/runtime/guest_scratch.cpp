// src/runtime/guest_scratch.cpp — see guest_scratch.h.
//
// Allocation and bounds logic is an engine primitive, not game code: bounds
// are set by the embedder via wx86_scratch_init, and OOM reporting goes
// through the embedder's callback instead of a direct log call.
#include "runtime/guest_scratch.h"
#include "runtime/cpu.h"
#include <cstring>

namespace {
uint32_t g_base = 0;
uint32_t g_cur  = 0;
uint32_t g_limit = 0;          // 0 = disarmed
bool     g_warned = false;
Wx86ScratchOomFn g_oom = nullptr;
}

void wx86_scratch_init(uint32_t base, uint32_t size) {
    g_base = base;
    g_cur = base;
    g_limit = size ? base + size : 0;             // 0 = unbounded (see header)
    g_warned = false;
}

void wx86_scratch_set_limit(uint32_t limit) { g_limit = limit; }

void wx86_scratch_set_oom_handler(Wx86ScratchOomFn cb) { g_oom = cb; }

uint32_t wx86_scratch_alloc(uint32_t n) {
    const uint32_t sz = (n + 7) & ~7u;
    const uint32_t a = g_cur;
    // The overflow check (a+sz < a) matters as much as the bound check: an
    // absurd request would wrap the addition and slip under the limit from
    // below. No bound set (g_limit == 0) means allocate without checking.
    if (g_limit && (a + sz > g_limit || a + sz < a)) {
        if (!g_warned) {
            g_warned = true;
            if (g_oom) g_oom(a, sz, g_limit);
        }
        return 0;
    }
    g_cur += sz;
    return a;
}

uint32_t wx86_scratch_put_cstr(d2rt::Cpu& c, const char* s) {
    const uint32_t n = (uint32_t)std::strlen(s) + 1;
    const uint32_t a = wx86_scratch_alloc(n);
    if (!a) return 0;
    c.write(a, s, n);
    return a;
}

uint32_t wx86_scratch_used() { return g_cur - g_base; }
uint32_t wx86_scratch_size() { return g_limit ? g_limit - g_base : 0; }
uint32_t wx86_scratch_base() { return g_base; }
