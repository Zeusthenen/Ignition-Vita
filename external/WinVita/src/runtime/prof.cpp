// src/runtime/prof.cpp — see prof.h. Compiled into the runtime always; every
// definition is inside #ifdef PROF_COUNTERS so the default build carries none
// of it.
#include "runtime/prof.h"

#ifdef PROF_COUNTERS
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#else
#include <time.h>
#endif

namespace d2rt { namespace prof {

uint64_t now_ns() {
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide() * 1000ull;   // µs -> ns
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t run_ns = 0;
uint64_t trap_ns = 0;
uint64_t trap_calls = 0;
uint64_t wait_calls = 0, wait_immediate = 0;
uint64_t cs_enter = 0, cs_contended = 0;

static std::unordered_map<uint32_t, uint64_t> g_hist;   // eip>>6 -> hits
static uint64_t g_samples = 0;

void sample_eip(uint32_t eip) { ++g_hist[eip >> 6]; ++g_samples; }
uint64_t eip_samples() { return g_samples; }

void top_eips(int n, void (*cb)(uint32_t, uint64_t, void*), void* u) {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    v.reserve(g_hist.size());
    for (auto& p : g_hist) v.push_back({p.second, p.first});
    std::sort(v.rbegin(), v.rend());
    for (int i = 0; i < n && i < (int)v.size(); ++i)
        cb(v[i].second << 6, v[i].first, u);
}

// ---- per guest thread ------------------------------------------------------
struct TidProf { uint32_t entry = 0; uint64_t samples = 0, blocks = 0;
                 std::unordered_map<uint32_t, uint64_t> hist; };
static std::map<uint32_t, TidProf> g_tid;

void sample_eip_tid(uint32_t eip, uint32_t tid) { TidProf& t = g_tid[tid]; ++t.hist[eip]; ++t.samples; }
void add_blocks_tid(uint32_t tid, uint32_t blocks) { g_tid[tid].blocks += blocks; }
void note_thread(uint32_t tid, uint32_t entry) { TidProf& t = g_tid[tid]; if (!t.entry) t.entry = entry; }
void threads(void (*cb)(uint32_t, uint32_t, uint64_t, uint64_t, void*), void* u) {
    for (auto& p : g_tid) cb(p.first, p.second.entry, p.second.samples, p.second.blocks, u);
}
void top_eips_tid(uint32_t tid, int n, void (*cb)(uint32_t, uint64_t, void*), void* u) {
    auto it = g_tid.find(tid); if (it == g_tid.end()) return;
    std::vector<std::pair<uint64_t, uint32_t>> v; v.reserve(it->second.hist.size());
    for (auto& p : it->second.hist) v.push_back({p.second, p.first});
    std::sort(v.rbegin(), v.rend());
    for (int i = 0; (n < 0 || i < n) && i < (int)v.size(); ++i) cb(v[i].second, v[i].first, u);
}

}} // namespace d2rt::prof
#endif // PROF_COUNTERS
