/* D2Vita shim replacing Box86's src/include/x86emu.h (minimal surface) */
#ifndef __X86EMU_H_
#define __X86EMU_H_
#include <stdint.h>

typedef struct x86emu_s x86emu_t;

uint64_t ReadTSC(x86emu_t* emu);

double FromLD(void* ld);        // long double (80bits pointer) -> double
void LD2D(void* ld, void* d);   // long double (80bits) -> double (64bits)
void D2LD(void* d, void* ld);   // double (64bits) -> long double (80bits)

void applyFlushTo0(x86emu_t* emu);

static inline void printFunctionAddr(uintptr_t nextaddr, const char* text) { (void)nextaddr; (void)text; }
static inline const char* getAddrFunctionName(uintptr_t addr) { (void)addr; return "?"; }

#endif /* __X86EMU_H_ */
