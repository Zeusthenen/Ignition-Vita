/* D2Vita shim replacing Box86's generated src/wrapped/generated/wrapper.h */
#ifndef __WRAPPER_H_
#define __WRAPPER_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
typedef void (*wrapper_t)(x86emu_t* emu, uintptr_t fnc);
int isRetX87Wrapper(wrapper_t fun);
#endif
