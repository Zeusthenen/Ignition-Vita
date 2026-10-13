#ifndef __DYNABLOCK_H_
#define __DYNABLOCK_H_

typedef struct x86emu_s x86emu_t;
typedef struct dynablock_s dynablock_t;
typedef struct kh_dynablocks_s  kh_dynablocks_t;

// D2Vita: bound shared by the concurrent-invalidation retry loops (DBGetBlock,
// DBAlternateBlock, and DynaRun's re-lookup) — rationale in DBGetBlock
// (dynarec/dynablock.c). Exhaustion always falls through to the honest abort.
#define DYN86_DBGET_RETRY 64

uint32_t X31_hash_code(void* addr, int len);
void FreeDynablock(dynablock_t* db, int need_lock);
void MarkDynablock(dynablock_t* db, int need_lock);
void MarkRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size, int need_lock);
int FreeRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size);
void FreeInvalidDynablock(dynablock_t* db, int need_lock);
dynablock_t* InvalidDynablock(dynablock_t* db, int need_lock);

dynablock_t* FindDynablockFromNativeAddress(void* addr);    // defined in box64context.h

// Handling of Dynarec block (i.e. an exectable chunk of x64 translated code)
dynablock_t* DBGetBlock(x86emu_t* emu, uintptr_t addr, int create);   // return NULL if block is not found / cannot be created. Don't create if create==0
dynablock_t* DBAlternateBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr);

// for use in signal handler
void cancelFillBlock();

#endif //__DYNABLOCK_H_