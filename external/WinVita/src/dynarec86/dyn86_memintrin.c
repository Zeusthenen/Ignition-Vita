/* src/dynarec86/dyn86_memintrin.c — see dyn86_memintrin.h for the contract.
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "debug.h"                 /* dyn86_membase / DYN86_G2H */
#include "regs.h"
#include "emu/x86emu_private.h"
#include "dyn86_memintrin.h"

int       dyn86_memintrin = 0;
uintptr_t dyn86_mi_cpy_va = 0, dyn86_mi_set_va = 0;
uint32_t  dyn86_mi_span   = 0;
uint32_t  dyn86_mi_maxn   = 0x01000000u;   /* 16 MiB */

unsigned long long dyn86_mi_cpy_served=0, dyn86_mi_cpy_fb=0, dyn86_mi_cpy_bytes=0;
unsigned long long dyn86_mi_set_served=0, dyn86_mi_set_fb=0, dyn86_mi_set_bytes=0;
unsigned long long dyn86_mi_cpy_calls=0,  dyn86_mi_set_calls=0;
unsigned long long dyn86_mi_rej[4] = {0,0,0,0};
unsigned long long dyn86_mi_cpy_hist[DYN86_MI_HBITS];
unsigned long long dyn86_mi_set_hist[DYN86_MI_HBITS];
unsigned long long dyn86_mi_cpy_align[4];
unsigned long long dyn86_mi_cpy_overlap = 0;
unsigned long long dyn86_mi_cpy_small   = 0;
unsigned long long dyn86_mi_cpy_maxseen = 0;
uint32_t dyn86_mi_sse2_va = 0;
unsigned long long dyn86_mi_cpy_sse[2] = {0,0}, dyn86_mi_set_sse[2] = {0,0};

void dyn86_mi_arm(int mode, uintptr_t cpy_va, uintptr_t set_va)
{
    dyn86_mi_cpy_va = cpy_va;
    dyn86_mi_set_va = set_va;
    dyn86_memintrin = mode;
}
void dyn86_mi_set_span(uint32_t span) { dyn86_mi_span = span; }
void dyn86_mi_set_sse2va(uint32_t va) { dyn86_mi_sse2_va = va; }
static inline int mi_sse2_on(void)
{ return dyn86_mi_sse2_va && *(const uint32_t*)DYN86_G2H(dyn86_mi_sse2_va); }

/* ---- cross-check oracle D2_MEMVERIFY (contract: dyn86_memintrin.h) ----------- */
int      dyn86_mi_verify = 0;
uint32_t dyn86_mi_verify_trap = 0;
unsigned long long dyn86_mi_ver_n = 0, dyn86_mi_ver_bad = 0, dyn86_mi_ver_skip = 0;
#define MI_VERBUF (256u*1024u)
static uint8_t  mi_vbuf[MI_VERBUF];
static uint32_t mi_v_dst, mi_v_n, mi_v_ra;
static volatile int mi_v_armed = 0;

void dyn86_mi_arm_verify(uint32_t trap_va) { dyn86_mi_verify_trap = trap_va; dyn86_mi_verify = trap_va ? 1 : 0; }

/* Prepares the comparison: snapshots the expected data, redirects the return
 * address. Returns 1 if the redirect was set (the caller must then FALL
 * BACK, not serve the call). */
static int mi_verify_arm(uint32_t esp, uint32_t dst, uint32_t n, const void* expect, int fill)
{
    if(!dyn86_mi_verify || !dyn86_mi_verify_trap) return 0;
    /* Only one call in flight at a time: under the native scheduler two guest
     * threads can enter here concurrently, hence the atomic exchange instead
     * of a plain test. */
    { int z = 0; if(n > MI_VERBUF || !__atomic_compare_exchange_n(&mi_v_armed, &z, 1, 0,
                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) { ++dyn86_mi_ver_skip; return 0; } }
    if(fill) memset(mi_vbuf, *(const int*)expect, n);
    else     memcpy(mi_vbuf, expect, n);
    mi_v_dst = dst; mi_v_n = n;
    { uint32_t* sp = (uint32_t*)DYN86_G2H(esp);
      mi_v_ra = sp[0]; sp[0] = dyn86_mi_verify_trap; }
    return 1;
}

/* Called by the return-trap shim (rt_boot). Compares, then returns the return address. */
uint32_t dyn86_mi_verify_ret(void)
{
    if(!mi_v_armed) return 0;
    ++dyn86_mi_ver_n;
    const uint8_t* got = (const uint8_t*)DYN86_G2H(mi_v_dst);
    if(memcmp(got, mi_vbuf, mi_v_n)) {
        uint32_t i = 0; while(i < mi_v_n && got[i] == mi_vbuf[i]) ++i;
        ++dyn86_mi_ver_bad;
        if(dyn86_mi_ver_bad <= 12)
            printf("[memverify] appel #%llu : divergence a +0x%x sur %u octets "
                   "(dst=%08x) invite=%02x natif=%02x\n",
                   (unsigned long long)dyn86_mi_ver_n, i, mi_v_n, mi_v_dst,
                   got[i], mi_vbuf[i]);
    }
    { uint32_t ra = mi_v_ra;
      __atomic_store_n(&mi_v_armed, 0, __ATOMIC_RELEASE);
      return ra; }
}

/* log2 bucket: 0 => n==0, k => n in [2^(k-1), 2^k) */
static inline int mi_bucket(uint32_t n)
{
    int k = 0;
    while (n) { ++k; n >>= 1; }
    return (k < DYN86_MI_HBITS) ? k : (DYN86_MI_HBITS - 1);
}

/* Census (PROFILE mode only): kept out of mode 1's hot path, where every
 * instruction executes millions of times over a run. */
static void mi_census_copy(uint32_t dst, uint32_t src, uint32_t n)
{
    dyn86_mi_cpy_hist[mi_bucket(n)]++;
    dyn86_mi_cpy_align[(dst | src) & 3u]++;
    if (n < 16u) ++dyn86_mi_cpy_small;
    if (n > dyn86_mi_cpy_maxseen) dyn86_mi_cpy_maxseen = n;
    if (n && ((dst > src && dst - src < n) || (src > dst && src - dst < n)))
        ++dyn86_mi_cpy_overlap;
    /* memcpy's SSE2 branch: n >= 0x100, flag armed, and dst/src share the
     * same residue modulo 16 */
    if (n >= 0x100u && mi_sse2_on() && ((dst & 15u) == (src & 15u)))
        { ++dyn86_mi_cpy_sse[0]; dyn86_mi_cpy_sse[1] += n; }
}

int dyn86_mi_copy(void* emu, uint32_t esp)
{
    /* cdecl: [esp] = return address, [esp+4] = dst, [esp+8] = src, [esp+0xc] = n */
    const uint32_t* a = (const uint32_t*)DYN86_G2H(esp);
    uint32_t dst = a[1], src = a[2], n = a[3];

    ++dyn86_mi_cpy_calls;

    if (dyn86_memintrin != 1) {            /* mode 2: PROFILE, do not serve */
        mi_census_copy(dst, src, n);
        ++dyn86_mi_rej[2]; ++dyn86_mi_cpy_fb; return 0; }
    if (n > dyn86_mi_maxn)         { ++dyn86_mi_rej[1]; ++dyn86_mi_cpy_fb; return 0; }
    if (dyn86_mi_span && (((uint64_t)dst + n) > dyn86_mi_span ||
                          ((uint64_t)src + n) > dyn86_mi_span))
                                   { ++dyn86_mi_rej[0]; ++dyn86_mi_cpy_fb; return 0; }

    /* Cross-check oracle: let the GUEST do the copy and compare at its return. */
    if (dyn86_mi_verify && mi_verify_arm(esp, dst, n, (const void*)DYN86_G2H(src), 0)) {
        ++dyn86_mi_cpy_fb; return 0; }

    { void* d = (void*)DYN86_G2H(dst); const void* sp = (const void*)DYN86_G2H(src);
      /* The guest CRT's memcpy body is the shared memcpy/memmove body: it
       * handles overlap via a backward branch. Overlap is essentially never
       * observed in practice, hence memcpy() on the hot path and memmove()
       * as the fallback. This isn't a gratuitous micro-optimization: on ARM,
       * libc's memmove() is generic C while memcpy() is LDM/NEON assembly. */
      if (dst > src ? (dst - src) < n : (src - dst) < n) memmove(d, sp, n);
      else                                              memcpy (d, sp, n); }
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;      /* memcpy returns dst */
    ++dyn86_mi_cpy_served; dyn86_mi_cpy_bytes += n;
    return 1;
}

int dyn86_mi_set(void* emu, uint32_t esp)
{
    /* cdecl: [esp] = return address, [esp+4] = dst, [esp+8] = c, [esp+0xc] = n */
    const uint32_t* a = (const uint32_t*)DYN86_G2H(esp);
    uint32_t dst = a[1], c = a[2], n = a[3];

    ++dyn86_mi_set_calls;

    if (dyn86_memintrin != 1) {            /* mode 2: PROFILE, do not serve */
        dyn86_mi_set_hist[mi_bucket(n)]++;
        /* memset's SSE2 branch: value 0, n >= 0x100, flag armed ->
         * _VEC_memzero, already vectorized on the guest side */
        if ((c & 0xffu) == 0 && n >= 0x100u && mi_sse2_on())
            { ++dyn86_mi_set_sse[0]; dyn86_mi_set_sse[1] += n; }
        ++dyn86_mi_rej[2]; ++dyn86_mi_set_fb; return 0; }
    if (n > dyn86_mi_maxn)    { ++dyn86_mi_rej[1]; ++dyn86_mi_set_fb; return 0; }
    if (dyn86_mi_span && ((uint64_t)dst + n) > dyn86_mi_span)
                              { ++dyn86_mi_rej[0]; ++dyn86_mi_set_fb; return 0; }

    { int fillv = (int)(c & 0xffu);
      if (dyn86_mi_verify && mi_verify_arm(esp, dst, n, &fillv, 1)) {
          ++dyn86_mi_set_fb; return 0; }
      memset((void*)DYN86_G2H(dst), fillv, n); }
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;      /* memset returns dst */
    ++dyn86_mi_set_served; dyn86_mi_set_bytes += n;
    return 1;
}
