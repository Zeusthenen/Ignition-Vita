/* D2Vita shim replacing Box86's src/include/x86trace.h */
#ifndef __X86TRACE_H_
#define __X86TRACE_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
typedef struct zydis_dec_s zydis_dec_t;
static inline const char* DecodeX86Trace(zydis_dec_t* dec, uintptr_t p) { (void)dec; (void)p; return "?"; }
static inline const char* getFunctionName(uintptr_t addr) { (void)addr; return "?"; }
#endif
