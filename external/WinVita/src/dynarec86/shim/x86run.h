/* D2Vita shim replacing Box86's src/include/x86run.h (minimal surface) */
#ifndef __X86RUN_H_
#define __X86RUN_H_
#include <stdint.h>

typedef struct x86emu_s x86emu_t;

// No interpreter in the PoC: the stub sets emu->quit=1
int Run(x86emu_t *emu, int step);
int DynaRun(x86emu_t *emu);

void PltResolver(x86emu_t* emu);    // dummy symbol (address taken only)

#endif /* __X86RUN_H_ */
