/* src/dynarec86/dyn86_intrin.c — registry of native intrinsics.
 * Contract, proofs and safety rules: dyn86_intrin.h.
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#include <stdio.h>
#include "dyn86_intrin.h"

int dyn86_intrin_on = 0;
int dyn86_intrin_n  = 0;
dyn86_intrin_t dyn86_intrin_tbl[DYN86_INTRIN_MAX];

int dyn86_intrin_add(uintptr_t va, dyn86_intrin_fn fn, uint16_t retn,
                     uint16_t inmask, uint16_t regmask, const char* name)
{
    if (!va || !fn) return 0;
    if (dyn86_intrin_n >= DYN86_INTRIN_MAX) return 0;
    for (int i = 0; i < dyn86_intrin_n; ++i)
        if (dyn86_intrin_tbl[i].va == va) return 0;   /* already registered */
    dyn86_intrin_t* e = &dyn86_intrin_tbl[dyn86_intrin_n++];
    e->va = va; e->fn = fn; e->retn = retn;
    e->inmask = inmask; e->regmask = regmask;
    e->name = name ? name : "?";
    e->calls = e->served = 0;
    return 1;
}

/* Linear scan: the table holds at most 16 entries and is consulted only ONCE
 * per block start (not per instruction). A hash table here would be noise --
 * a full run translates on the order of a few thousand blocks. */
const dyn86_intrin_t* dyn86_intrin_find(uintptr_t va)
{
    if (!dyn86_intrin_on || !va) return 0;
    for (int i = 0; i < dyn86_intrin_n; ++i)
        if (dyn86_intrin_tbl[i].va == va) return &dyn86_intrin_tbl[i];
    return 0;
}

int dyn86_intrin_report(char* out, unsigned cap)
{
    if (!out || cap < 32) return 0;
    if (!dyn86_intrin_on || !dyn86_intrin_n)
        return snprintf(out, cap, "intrin: eteint (n=%d)", dyn86_intrin_n);
    int n = snprintf(out, cap, "intrin: on n=%d", dyn86_intrin_n);
    for (int i = 0; i < dyn86_intrin_n && n > 0 && (unsigned)n < cap; ++i) {
        const dyn86_intrin_t* e = &dyn86_intrin_tbl[i];
        n += snprintf(out + n, cap - n, " | %s appels=%llu servis=%llu replis=%llu",
                      e->name,
                      (unsigned long long)e->calls,
                      (unsigned long long)e->served,
                      (unsigned long long)(e->calls - e->served));
    }
    return n;
}
