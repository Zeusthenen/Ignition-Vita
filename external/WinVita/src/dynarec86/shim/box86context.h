/* D2Vita shim replacing Box86's src/include/box86context.h
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 * Minimal context: only fields the dynarec core actually touches.
 * NOTE: emitted code bakes in offsetof() of THIS struct — internal
 * consistency is all that matters, not matching Box86's layout.
 */
#ifndef __BOX86CONTEXT_H_
#define __BOX86CONTEXT_H_
#include <stdint.h>
#include <pthread.h>

typedef struct elfheader_s elfheader_t;
typedef struct x86emu_s x86emu_t;
typedef struct zydis_dec_s zydis_dec_t;
typedef struct dynablock_s dynablock_t;
typedef struct mmaplist_s mmaplist_t;
typedef struct kh_dynablocks_s kh_dynablocks_t;

#define DYNAMAP_SHIFT 16
#define DYNAMAP_SIZE (1<<(32-DYNAMAP_SHIFT))
#define JMPTABL_SHIFT 16
#define JMPTABL_SIZE (1<<(32-JMPTABL_SHIFT))
#define JMPTABLE_MASK ((1<<JMPTABL_SHIFT)-1)

#define MAX_SIGNAL 64

typedef struct box86context_s {
    uint32_t            sel_serial;     // incremented each time selectors change
    uintptr_t           exit_bridge;    // exit bridge value (guest addr of CC 'S''C' 0000 stub)
    pthread_mutex_t     mutex_dyndump;
    pthread_mutex_t     mutex_lock;
    uintptr_t           max_db_size;    // biggest built dynablock (x86 bytes)
    int                 trace_dynarec;
    zydis_dec_t         *dec;           // trace disassembler (always NULL here)
    uint8_t             canary[4];
    uintptr_t           signals[MAX_SIGNAL+1];
    uintptr_t           restorer[MAX_SIGNAL+1];
    int                 onstack[MAX_SIGNAL+1];
    int                 is_sigaction[MAX_SIGNAL+1];
    x86emu_t            *emu_sig;
    int                 no_sigsegv;
    int                 no_sigill;
    void*               tlsdata;
    int32_t             tlssize;
} box86context_t;

#define mutex_lock(A)       pthread_mutex_lock(A)
#define mutex_trylock(A)    pthread_mutex_trylock(A)
#define mutex_unlock(A)     pthread_mutex_unlock(A)

extern box86context_t *my_context;

static inline int GetTID(void) { return 0; }

// mutex unlock/relock for signal handling: no-ops in the PoC
static inline int unlockMutex(void) { return 0; }
static inline void relockMutex(int locks) { (void)locks; }

#endif /* __BOX86CONTEXT_H_ */
