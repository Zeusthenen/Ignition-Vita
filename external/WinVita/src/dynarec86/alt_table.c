/* src/dynarec86/alt_table.c — the alternates table, and nothing else.
 *
 * An "alternate" is the dynarec's primary extension mechanism: a guest ->
 * guest redirection that the translator applies to call/jmp targets, without
 * touching a single byte of guest memory. This is how a port runs native code
 * in place of a guest function.
 *
 * This table lives in its own file, decoupled from the rest of the dynarec,
 * so it has no dependency beyond libc: tools/alt_table_selftest.cpp can
 * inject an allocation failure and verify it is reported correctly.
 *
 * The table grows by doubling with no fixed cap. The only failure mode
 * (allocation refused) increments a counter readable by the embedder and
 * writes a line to the console log.
 *
 * Call contract: dyn86_set_alternate is called at install time, before any
 * translated code runs. hasAlternate()/getAlternate() read the table on the
 * hot path (first line of internalDBGetBlock) without a lock, so
 * `dyn86_alt_n` is published last, after the entry itself is written. Old
 * backing arrays are never freed, since a concurrent reader may still hold
 * the previous pointer; the resulting waste is bounded by the sum of past
 * capacities, a few kilobytes.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "bridge.h"   /* extern declarations for the table (single source of truth) */

/* Console log: strong link, no-op outside __vita__ (platform/vita_host.cpp).
 * Never a weak reference by a consumer's name -- see docs/migration.md,
 * pitfall 9. */
#ifdef __cplusplus
extern "C" void wx86_vita_progress_c(const char* msg);
#else
void wx86_vita_progress_c(const char* msg);
#endif

uintptr_t *dyn86_alt_from = NULL;
uintptr_t *dyn86_alt_to   = NULL;
int dyn86_alt_n     = 0;      /* alternates set */
int dyn86_alt_refus = 0;      /* alternates lost (allocation refused) */
static int g_alt_cap = 0;

void dyn86_set_alternate(uintptr_t from, uintptr_t to) {
    int i;
    for (i = 0; i < dyn86_alt_n; i++)
        if (dyn86_alt_from[i] == from) { dyn86_alt_to[i] = to; return; }
    if (dyn86_alt_n == g_alt_cap) {
        const int cap = g_alt_cap ? g_alt_cap * 2 : 32;
        uintptr_t* nf = (uintptr_t*)malloc(sizeof(uintptr_t) * (size_t)cap);
        uintptr_t* nt = (uintptr_t*)malloc(sizeof(uintptr_t) * (size_t)cap);
        if (!nf || !nt) {
            char m[96];
            free(nf); free(nt);
            ++dyn86_alt_refus;
            snprintf(m, sizeof m,
                     "dyn86: set_alternate PERDU (%d poses, allocation refusee)", dyn86_alt_n);
            wx86_vita_progress_c(m);
            return;
        }
        if (dyn86_alt_n) {
            memcpy(nf, dyn86_alt_from, sizeof(uintptr_t) * (size_t)dyn86_alt_n);
            memcpy(nt, dyn86_alt_to,   sizeof(uintptr_t) * (size_t)dyn86_alt_n);
        }
        dyn86_alt_from = nf; dyn86_alt_to = nt; g_alt_cap = cap;
    }
    dyn86_alt_from[dyn86_alt_n] = from;
    dyn86_alt_to[dyn86_alt_n]   = to;
    dyn86_alt_n++;            /* published last (see contract above) */
}

int dyn86_alt_count(void) { return dyn86_alt_n; }
int dyn86_alt_lost(void)  { return dyn86_alt_refus; }
