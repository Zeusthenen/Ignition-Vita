/* dyn86_emitprof.h — profile of the code EMITTED by the x86 -> ARMv7
 * translator.
 *
 * WHY. Most of a frame's time is spent executing translated guest code
 * rather than in block transitions, so the interesting cost lives inside the
 * emitted code itself. A single scalar ("expansion=5.343x") says how much
 * the translator produces, never WHAT FOR. This module answers that: per x86
 * instruction family, how many ARM bytes.
 *
 * TWO NUMBERS, NEVER CONFLATE THEM:
 *   - STATIC:   bytes emitted at TRANSLATION time (once per translated
 *               block). Points at BULKY code.
 *   - WEIGHTED: the same bytes multiplied by the block's EXECUTION count.
 *               Points at HOT code. This is the one that matters.
 * The execution counter is EXACT (not sampled): each block's prologue
 * increments its own private 64-bit counter.
 *
 * COST. Everything is gated behind -DD2_EMITPROF. Without that define, this
 * file compiles no data and no macro emits anything: the shipped binary pays
 * NOTHING, not even a knob read.
 *
 * BENCH. qemu-arm only. This is COUNTING, not timing: qemu does not model
 * caches, so no microsecond or fps figure comes out of this.
 */
#ifndef DYN86_EMITPROF_H
#define DYN86_EMITPROF_H

#ifdef D2_EMITPROF

#include <stdint.h>

/* x86 instruction families. The D2EP_ORPHAN index collects every byte
 * emitted OUTSIDE an instruction's own window (block prologue, epilogue,
 * end-of-block barrier flush): this is what makes the cross-check exact --
 *   sum(fam_b[0..D2EP_NFAM]) == block's arm_size == dyn86_emit_arm_bytes. */
enum {
    D2EP_ALU32 = 0,   /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP/TEST/INC/DEC/NEG/MUL... 32-bit */
    D2EP_ALU8,        /* same, in 8-bit, and in 16-bit (0x66 prefix)          */
    D2EP_MOVLEA,      /* MOV, LEA, MOVZX/MOVSX, XCHG, BSWAP, CWDE/CDQ        */
    D2EP_BRANCH,      /* CALL / RET / JMP / Jcc / LOOP / INT                 */
    D2EP_X87,         /* D8..DF + FWAIT                                      */
    D2EP_SSE,         /* 0F : MMX / SSE / SSE2                               */
    D2EP_STRING,      /* MOVS/CMPS/STOS/LODS/SCAS (+ REP)                    */
    D2EP_STACK,       /* PUSH/POP/PUSHA/POPA/PUSHF/POPF/ENTER/LEAVE          */
    D2EP_FLAGS,       /* SETcc, CMOVcc, CLC/STC/CLD/STD/CMC, SAHF/LAHF       */
    D2EP_SHIFT,       /* SHL/SHR/SAR/ROL/ROR/RCL/RCR, SHLD/SHRD, BT*, BSF/BSR*/
    D2EP_OTHER,       /* NOP, CPUID, RDTSC, I/O, segment ops...              */
    D2EP_NFAM,
    D2EP_ORPHAN = D2EP_NFAM,
    D2EP_NFAM1        /* = D2EP_NFAM+1, array size                          */
};

/* Sub-buckets. They OVERLAP each other and overlap the families: these are
 * annotations, not a partition. FPUPUSHPOP is included in HELPER, so is
 * UPDFLAGS. Never sum them. */
enum {
    D2EP_S_PROLOG = 0,  /* block prologue (preemption budget)                */
    D2EP_S_HELPER,      /* any call to a C function (call_c/_d/_dr/_ddr)     */
    D2EP_S_UPDFLAGS,    /* flags materialization (full READFLAGS)            */
    D2EP_S_FPUPURGE,    /* fpu_purgecache: flushing x87/MMX/SSE to emu       */
    D2EP_S_FPUPUSHPOP,  /* fpu_pushcache/popcache around a C call            */
    D2EP_S_BLOCKEND,    /* jump_to_next / jump_to_epilog                     */
    D2EP_S_PROBE,       /* the probe itself (8 insns/block) -- to subtract   */
    D2EP_S_DEFERFLAGS,  /* archiving op1/op2/res/df for deferred flags       */
    D2EP_S_CMPJCC,      /* "ALU that sets flags" + fused Jcc pairs           */
    D2EP_S_NSUB
};

/* x86 -> ARM offset mapping, one entry per translated instruction. This is
 * what makes disassembly of the emitted code readable: without it, a block
 * is just a ribbon of ARM words with no way to tell which x86 instruction
 * produced what. Shared bump-allocated arena; an overflow is reported, never
 * silent. */
typedef struct { uint32_t x86; uint32_t off; } d2ep_map_t;
#define D2EP_MAPCAP 1200000

typedef struct d2ep_block_s {
    uint64_t exec;                    /* incremented by the EMITTED PROLOGUE */
    uint32_t arm_start;               /* HOST address of the emitted code    */
    uint32_t map_i, map_n;
    uint32_t x86_addr;
    uint32_t arm_bytes;
    uint32_t x86_bytes;
    uint32_t ninsts;
    uint32_t valid;                   /* 1 = block actually delivered        */
    uint32_t fam_b[D2EP_NFAM1];
    uint32_t fam_n[D2EP_NFAM1];
    uint32_t fam_x[D2EP_NFAM1];       /* x86 bytes consumed                  */
    uint32_t mem_b[D2EP_NFAM1];       /* of which: MEMORY operand (mod != 3) */
    uint32_t mem_n[D2EP_NFAM1];
    uint32_t sub_b[D2EP_S_NSUB];
    uint32_t sub_n[D2EP_S_NSUB];
} d2ep_block_t;

#define D2EP_MAXBLOCKS 32768

extern d2ep_block_t d2ep_blocks[D2EP_MAXBLOCKS];
extern int          d2ep_nblocks;
extern int          d2ep_overflow;
extern d2ep_block_t* d2ep_cur;        /* block currently being translated (pass2) */
extern int          d2ep_last_pos;    /* arm_size at the last change         */
extern int          d2ep_cur_fam;
/* EXECUTION counters for C helpers (exact, incremented in C) */
extern d2ep_map_t   d2ep_map[D2EP_MAPCAP];
extern int          d2ep_map_n;
extern int          d2ep_map_overflow;
extern uint64_t     d2ep_run_updateflags;      /* calls to UpdateFlags       */
extern uint64_t     d2ep_run_updateflags_work; /* of which df != d_none      */

int  d2ep_alloc(uintptr_t x86_addr);            /* -1 if full                */
void d2ep_begin(int idx);
void d2ep_switch(int fam, int pos);             /* closes the current window */
void d2ep_openinst(const uint8_t* p, int pos, uintptr_t x86);
void d2ep_closeinst(int pos, int x86len);
void d2ep_sub(int k, int bytes, int n);
void d2ep_end(int idx, int pos, int x86_bytes, int ninsts);
int  d2ep_classify(const uint8_t* p, int* hasmem);
void d2ep_report(void (*out)(const char*));
void d2ep_setarm(int idx, uintptr_t arm_start);
void d2ep_dump(const char* path, int topn);
void d2ep_csv(const char* path);

#endif /* D2_EMITPROF */
#endif
