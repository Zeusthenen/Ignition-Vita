/* D2Vita shim replacing Box86's src/include/signals.h */
#ifndef __SIGNALS_H_
#define __SIGNALS_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
// PoC: no guest signal delivery — these set emu->quit and record an error
void emit_signal(x86emu_t* emu, int sig, void* addr, int code);
void emit_div0(x86emu_t* emu, void* addr, int code);
void emit_interruption(x86emu_t* emu, int num, void* addr);
#endif
