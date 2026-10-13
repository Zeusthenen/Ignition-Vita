#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#include <setjmp.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "tools/bridge_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynablock.h"
#include "dynablock_private.h"
#include "dynarec_private.h"
#include "elfloader.h"
#include "bridge.h"
#ifdef ARM
#include "dynarec_arm.h"

extern int dyn86_link_direct(void);   // D2Vita scheduler quantum mode
#include "arm_lock_helper.h"
#else
#error Unsupported architecture!
#endif
#include "custommem.h"
#include "khash.h"

KHASH_MAP_INIT_INT(dynablocks, dynablock_t*)

/* --- D2Vita D2_JITPROFILE counters (contract + inclusive/exclusive rules in
 * src/dynarec86/dyn86.h). Every write below sits behind `dyn86_jitprof`, which
 * is armed once in dyn86_init(); with the knob absent this file behaves
 * exactly as before. Nothing here touches code generation or linking. */
extern int dyn86_jitprof;
extern uint64_t dyn86_jp_clock_bias_ns;
#define DYN86_JP_SAMPLE 64u
uint64_t dyn86_jp_lookup_calls = 0;
uint64_t dyn86_jp_lookup_hits  = 0;
uint64_t dyn86_jp_lookup_miss  = 0;
uint64_t dyn86_jp_created      = 0;
uint64_t dyn86_jp_recompiles   = 0;
uint64_t dyn86_jp_invalid      = 0;
uint64_t dyn86_jp_x86_insns    = 0;
uint64_t dyn86_jp_arm_bytes    = 0;
uint64_t dyn86_jp_lookup_ns    = 0;
uint64_t dyn86_jp_lookup_smp   = 0;
/* Set while DBGetBlock is re-translating a block it just invalidated, so the
 * publish site can tell a RE-compile from a genuinely new block. One guest
 * thread at a time runs under the cooperative scheduler, so a plain int is
 * enough (and it is diagnostics-only). */
static int dyn86_jp_recomp = 0;

uint32_t X31_hash_code(void* addr, int len)
{
    if(!len) return 0;
    uint8_t* p = (uint8_t*)addr;
	int32_t h = *p;
	for (--len, ++p; len; --len, ++p) h = (h << 5) - h + (int32_t)*p;
	return (uint32_t)h;
}

dynablock_t* InvalidDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(db->gone)
            return NULL; // already in the process of deletion!
        if(dyn86_jitprof) ++dyn86_jp_invalid;
        dynarec_log(LOG_DEBUG, "InvalidDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        // remove jumptable
        setJumpTableDefault(db->x86_addr);
        db->done = 0;
        db->gone = 1;
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
    return db;
}

void FreeInvalidDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(!db->gone)
            return; // already in the process of deletion!
        dynarec_log(LOG_DEBUG, "FreeInvalidDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        FreeDynarecMap((uintptr_t)db->actual_block);
        customFree(db);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
}

void FreeDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(db->gone)
            return; // already in the process of deletion!
        if(dyn86_jitprof) ++dyn86_jp_invalid;
        dynarec_log(LOG_DEBUG, "FreeDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        // remove jumptable
        setJumpTableDefault(db->x86_addr);
        dynarec_log(LOG_DEBUG, " -- FreeDyrecMap(%p, %d)\n", db->actual_block, db->size);
        db->done = 0;
        db->gone = 1;
        if(db->previous)
            FreeInvalidDynablock(db->previous, 0);
        FreeDynarecMap((uintptr_t)db->actual_block);
        customFree(db);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
}



/* D2Vita (audit T12 §12.4 entree 2) — AUTO-INTERBLOCAGE CORRIGE.
 * need_lock dit si FreeInvalidDynablock doit prendre mutex_dyndump lui-meme.
 * Il le FAUT depuis cleanDBFromAddressRange (qui tient mutex_prot, pas
 * dyndump) et il ne le faut SURTOUT PAS depuis internalDBGetBlock, qui tient
 * deja dyndump : ce mutex est initialise avec des attributs par defaut
 * (src/runtime/cpu_box86.cpp:322, pthread_mutex_init(..., nullptr)) donc
 * PTHREAD_MUTEX_NORMAL, donc non recursif — le reprendre depuis le meme fil
 * est un blocage definitif, pas une erreur rendue.
 * Avant ce correctif la valeur etait cablee a 1 pour les deux chemins. */
void MarkDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        dynarec_log(LOG_DEBUG, "MarkDynablock %p %p-%p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1);
        if(!setJumpTableIfRef(db->x86_addr, db->jmpnext, db->block)) {
            dynablock_t* old = db;
            db = getDB((uintptr_t)old->x86_addr);
            if(!old->gone && db!=old) {
                printf_log(LOG_INFO, "Warning, couldn't mark block as dirty for %p, block=%p, current_block=%p\n", old->x86_addr, old, db);
                // the block is lost, need to invalidate it...
                old->gone = 1;
                old->done = 0;
                if(!db || db->previous)
                    FreeInvalidDynablock(old, need_lock);
                else
                    db->previous = old;
            }
        }
    }
}

static int IntervalIntersects(uintptr_t start1, uintptr_t end1, uintptr_t start2, uintptr_t end2)
{
    if(start1 > end2 || start2 > end1)
        return 0;
    return 1;
}

static int MarkedDynablock(dynablock_t* db)
{
    if(db) {
        if(getNeedTest((uintptr_t)db->x86_addr))
            return 1; // already done
    }
    return 0;
}

void MarkRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size, int need_lock)
{
    // Mark will try to find *any* blocks that intersect the range to mark
    if(!db)
        return;
    dynarec_log(LOG_DEBUG, "MarkRangeDynablock %p-%p .. startdb=%p, sizedb=%p\n", (void*)addr, (void*)addr+size-1, (void*)db->x86_addr, (void*)db->x86_size);
    if(IntervalIntersects((uintptr_t)db->x86_addr, (uintptr_t)db->x86_addr+db->x86_size-1, addr, addr+size+1))
        MarkDynablock(db, need_lock);
}

int FreeRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size)
{
    if(!db)
        return 1;

    int need_lock = my_context?1:0;
    if(IntervalIntersects((uintptr_t)db->x86_addr, (uintptr_t)db->x86_addr+db->x86_size-1, addr, addr+size+1)) {
        FreeDynablock(db, need_lock);
        return 0;
    }
    return 1;
}

dynablock_t *AddNewDynablock(uintptr_t addr)
{
    dynablock_t* block;
    #if 0
    // check if memory as the correct flags
    int prot = getProtection(addr);
    if(!(prot&(PROT_EXEC|PROT_DYNAREC|PROT_DYNAREC_R))) {
        dynarec_log(LOG_VERBOSE, "Block asked on a memory with no execution flags 0x%02X\n", prot);
        return NULL;
    }
    
    #endif
    // create and add new block
    dynarec_log(LOG_VERBOSE, "Ask for DynaRec Block creation @%p\n", (void*)addr);
    block = (dynablock_t*)customCalloc(1, sizeof(dynablock_t));
    return block;
}

//TODO: move this to dynrec_arm.c and track allocated structure to avoid memory leak
#if defined(__vita__)
#define JUMPBUFF jmp_buf         /* newlib: array type, same shape as ANDROID */
#elif defined(ANDROID)
#define JUMPBUFF sigjmp_buf
#else
#define JUMPBUFF struct __jmp_buf_tag
#endif
static __thread JUMPBUFF dynarec_jmpbuf;
#if defined(ANDROID) || defined(__vita__)
#define DYN_JMPBUF dynarec_jmpbuf
#else
#define DYN_JMPBUF &dynarec_jmpbuf
#endif

void cancelFillBlock()
{
    longjmp(DYN_JMPBUF, 1);
}

/* 
    return NULL if block is not found / cannot be created. 
    Don't create if create==0
*/
// Translation-time attribution (freeze diagnosis): cumulative wall time spent
// translating x86 blocks to ARM, sampled by the Vita watchdog.
uint64_t dyn86_fill_ns = 0; uint32_t dyn86_fill_count = 0;
uint32_t dyn86_fill_fail = 0;   // fills that produced no cached block (e.g. JIT map OOM)
// D2_EIPTRAP diag: armed with a guest EIP (hex env); the first chain to that
// address dumps the SOURCE block (who jumped there) — catches wild jumps
// through corrupted function pointers at the moment they happen.
uintptr_t dyn86_eiptrap = 0;
uintptr_t dyn86_eiptrap2 = 0;   // D2Vita: second trapped address (D2_EIPTRAP2), shared ring tagged A/B
uint32_t dyn86_diag_regs = 0;   // arms the arm_next.S reg-sync STM (set at boot when an eiptrap/xfertrace knob is present)

#include <time.h>
static inline uint64_t dyn86_prof_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

static dynablock_t* internalDBGetBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr, int create, int need_lock)
{
    if(hasAlternate((void*)addr))
        return NULL;
    dynablock_t* block = getDB(addr);
    if(block || !create) {
        // HIT (definition): the jump table already points at a live block for
        // this guest address, so the lookup returns without translating.
        if(dyn86_jitprof && block) ++dyn86_jp_lookup_hits;
        return block;
    }

    if(need_lock) {
        if(box86_dynarec_wait) {
            mutex_lock(&my_context->mutex_dyndump);
        } else {
            if(mutex_trylock(&my_context->mutex_dyndump))   // FillBlock not available for now
                return NULL;
        }
        block = getDB(addr);    // just in case
        if(block) {
            if(dyn86_jitprof) ++dyn86_jp_lookup_hits;   // raced, still a hit
            mutex_unlock(&my_context->mutex_dyndump);
            return block;
        }
    }

    // MISS (definition): no live block for this address -> this lookup pays a
    // full translation (a NEW block, or a re-translation of one just
    // invalidated by the hash test in DBGetBlock).
    if(dyn86_jitprof) ++dyn86_jp_lookup_miss;
    block = AddNewDynablock(addr);

    // fill the block
    block->x86_addr = (void*)addr;
    if(sigsetjmp(DYN_JMPBUF, 1)) {
        printf_log(LOG_INFO, "FillBlock at %p triggered a segfault, cancelling\n", (void*)addr);
        FreeDynablock(block, 0);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
        return NULL;
    }
    uint64_t dyn86_fb_t0 = dyn86_prof_now_ns();
    void* ret = FillBlock(block, filladdr);
    dyn86_fill_ns += dyn86_prof_now_ns() - dyn86_fb_t0; dyn86_fill_count++;
    if(!ret) {
        dyn86_fill_fail++;
        dynarec_log(LOG_DEBUG, "Fillblock of block %p for %p returned an error\n", block, (void*)addr);
        customFree(block);
        block = NULL;
    }
    // check size
    if(block) {
        int blocksz = block->x86_size;
        if(blocksz>my_context->max_db_size)
            my_context->max_db_size = blocksz;
        // fill-in jumptable
        // D2Vita quantum mode: publish the jmpnext stub instead of the direct
        // block so every chain funnels through LinkNext (getDB still works:
        // the dynablock_t* sits right before jmpnext too).
        if(!addJumpTableIfDefault(block->x86_addr, (block->dirty||!dyn86_link_direct())?block->jmpnext:block->block)) {
            FreeDynablock(block, 0);
            block = getDB(addr);
            // need_lock=0 : on tient DEJA mutex_dyndump ici, dans les DEUX cas.
            // Si le parametre need_lock de cette fonction valait 1 on l'a pris
            // plus haut ; s'il valait 0 c'est que l'appelant le tenait (le
            // trylock de DBGetBlock rend 0 en cas de SUCCES et cette valeur est
            // passee telle quelle). Il n'existe aucun chemin qui atteigne cette
            // ligne sans le mutex. Passer 1 ici etait l'auto-interblocage.
            MarkDynablock(block, 0);   // just in case...
        } else {
            if(block->x86_size)
                block->done = 1;    // don't validate the block if the size is null, but keep the block
            // Published. A re-translation is counted as a recompile at the
            // hash-mismatch site, not here, so "created" stays "blocks that
            // did not exist before".
            if(dyn86_jitprof && !dyn86_jp_recomp) ++dyn86_jp_created;
        }
    }
    if(need_lock)
        mutex_unlock(&my_context->mutex_dyndump);

    dynarec_log(LOG_DEBUG, "%04d| --- DynaRec Block created @%p:%p (%p, 0x%x bytes)\n", GetTID(), (void*)addr, (void*)(addr+((block)?block->x86_size:1)-1), (block)?block->block:0, (block)?block->size:0);

    return block;
}

dynablock_t* DBGetBlock(x86emu_t* emu, uintptr_t addr, int create)
{
    if(isInHotPage(addr))
        return NULL;
    // D2_JITPROFILE: time 1 lookup in DYN86_JP_SAMPLE. A single lookup costs
    // far less than the clock's resolution, so this is a SAMPLED ESTIMATE, not
    // a measurement: the per-call FillBlock time is subtracted (translation is
    // reported separately) and so is the calibrated cost of one clock read.
    // Timing every call would cost more than the path being measured.
    uint64_t jp_t0 = 0, jp_fill0 = 0; int jp_sample = 0;
    if(dyn86_jitprof) {
        ++dyn86_jp_lookup_calls;
        if(!(dyn86_jp_lookup_calls & (uint64_t)(DYN86_JP_SAMPLE-1))) {
            jp_sample = 1; jp_fill0 = dyn86_fill_ns; jp_t0 = dyn86_prof_now_ns();
        }
    }
    // D2Vita concurrency retry (native scheduler, torture mt/smc): a lookup can
    // observe a block whose done was zeroed by a CONCURRENT invalidator — the
    // SMC write barrier / FlushInstructionCache path (unprotectDB(mark=1) ->
    // cleanDBFromAddressRange -> MarkDynablock) runs under mutex_prot only,
    // NEVER mutex_dyndump, and translated-code dispatch holds no GIL. When the
    // lost-mark fallback (MarkDynablock: old->done=0) or FreeDynablock lands
    // between internalDBGetBlock's return and the db->done gate below, the
    // revalidation is skipped and a not-done block reaches DynaRun. Upstream
    // box86 shrugs: Run(emu,1) interprets one step and re-looks-up — a de
    // facto retry. D2Vita has NO interpreter (Run() is an aborting stub), so
    // the faithful minimal equivalent is to RETRY the lookup here, bounded:
    // the transient window closes as soon as the invalidator's step completes
    // (next lookup either creates a fresh block under mutex_dyndump or finds
    // the newly published one). A PERMANENT failure (FillBlock unimplemented
    // opcode, fill segfault-cancel) stays NULL through every retry and still
    // reaches the honest controlled abort, just DYN86_DBGET_RETRY yields
    // later. create==0 callers keep the immediate NULL (means "not translated
    // yet", not a race). DYN86_DBGET_RETRY lives in dynablock.h (shared with
    // DBAlternateBlock and DynaRun's downstream re-lookup, dynarec.c).
    dynablock_t *db;
    int dyn86_retry = 0;
    for(;;) {
        db = internalDBGetBlock(emu, addr, addr, create, 1);
        if(db && db->done && db->block && getNeedTest(addr)) {
            if(db->always_test)
                sched_yield();  // just calm down...
            uint32_t hash = X31_hash_code((void*)DYN86_G2H(db->x86_addr), db->x86_size);
            int need_lock = mutex_trylock(&my_context->mutex_dyndump);
            if(hash!=db->hash) {
                if(dyn86_jitprof) { ++dyn86_jp_recompiles; dyn86_jp_recomp = 1; }
                db->done = 0;   // invalidating the block
                dynarec_log(LOG_DEBUG, "Invalidating block %p from %p:%p (hash:%X/%X, always_test:%d) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1, hash, db->hash, db->always_test, (void*)addr);
                // Free db, it's now invalid!
                dynablock_t* old = InvalidDynablock(db, need_lock);
                // start again... (will create a new block)
                db = internalDBGetBlock(emu, addr, addr, create, need_lock);
                if(db) {
                    if(db->previous)
                        FreeInvalidDynablock(db->previous, need_lock);
                    db->previous = old;
                } else
                    FreeInvalidDynablock(old, need_lock);
                dyn86_jp_recomp = 0;
            } else {
                dynarec_log(LOG_DEBUG, "Validating block %p from %p:%p (hash:%X, always_test:%d) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1, db->hash, db->always_test, (void*)addr);
                protectDB((uintptr_t)db->x86_addr, db->x86_size);
                // fill back jumptable (unless D2Vita quantum mode: stay funneled)
                if(isprotectedDB((uintptr_t)db->x86_addr, db->x86_size) && !db->always_test && dyn86_link_direct()) {
                    setJumpTableIfRef(db->x86_addr, db->block, db->jmpnext);
                }
            }
            if(!need_lock)
                mutex_unlock(&my_context->mutex_dyndump);
        }
        if(db && db->block && db->done)
            break;                                   // valid block: done
        if(!create || ++dyn86_retry > DYN86_DBGET_RETRY)
            break;                                   // permanent: honest abort path
        sched_yield();                               // let the invalidator finish its step
    }
    if(!db || !db->block || !db->done)
        emu->test.test = 0;
    if(jp_sample) {
        uint64_t dt = dyn86_prof_now_ns() - jp_t0;
        uint64_t fdt = dyn86_fill_ns - jp_fill0;         // translation: reported apart
        dt = (dt > fdt) ? dt - fdt : 0;
        dt = (dt > dyn86_jp_clock_bias_ns) ? dt - dyn86_jp_clock_bias_ns : 0;
        dyn86_jp_lookup_ns += dt; ++dyn86_jp_lookup_smp;
    }
    return db;
}

dynablock_t* DBAlternateBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr)
{
    dynarec_log(LOG_DEBUG, "Creating AlternateBlock at %p for %p\n", (void*)addr, (void*)filladdr);
    int create = 1;
    // D2Vita concurrency retry: same invalidation dance as DBGetBlock, same
    // window (see the comment there — flagged in 427d51f's own commit body).
    // create is always 1 here, so a permanent failure (NULL or never-done)
    // exhausts the bound and returns as before: the caller's honest path.
    dynablock_t *db;
    int dyn86_retry = 0;
    for(;;) {
        db = internalDBGetBlock(emu, addr, filladdr, create, 1);
        if(db && db->done && db->block && getNeedTest(filladdr)) {
            if(db->always_test)
                sched_yield();  // just calm down...
            int need_lock = mutex_trylock(&my_context->mutex_dyndump);
            uint32_t hash = X31_hash_code((void*)DYN86_G2H(db->x86_addr), db->x86_size);
            if(hash!=db->hash) {
                if(dyn86_jitprof) { ++dyn86_jp_recompiles; dyn86_jp_recomp = 1; }
                db->done = 0;   // invalidating the block
                dynarec_log(LOG_DEBUG, "Invalidating alt block %p from %p:%p (hash:%X/%X) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size, hash, db->hash, (void*)addr);
                // Free db, it's now invalid!
                dynablock_t* old = InvalidDynablock(db, need_lock);
                // start again... (will create a new block)
                db = internalDBGetBlock(emu, addr, filladdr, create, need_lock);
                if(db) {
                    if(db->previous)
                        FreeInvalidDynablock(db->previous, need_lock);
                    db->previous = old;
                } else
                    FreeInvalidDynablock(old, need_lock);
                dyn86_jp_recomp = 0;
            } else {
                protectDB((uintptr_t)db->x86_addr, db->x86_size);
                // fill back jumptable (unless D2Vita quantum mode: stay funneled)
                if(isprotectedDB((uintptr_t)db->x86_addr, db->x86_size) && !db->always_test && dyn86_link_direct()) {
                    setJumpTableIfRef(db->x86_addr, db->block, db->jmpnext);
                }
            }
            if(!need_lock)
                mutex_unlock(&my_context->mutex_dyndump);
        }
        if(db && db->block && db->done)
            break;                                   // valid block: done
        if(++dyn86_retry > DYN86_DBGET_RETRY)
            break;                                   // permanent: honest abort path
        sched_yield();                               // let the invalidator finish its step
    }
    if(!db || !db->block || !db->done)
        emu->test.test = 0;
    return db;
}
