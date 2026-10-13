/* D2Vita shim replacing Box86's src/include/elfloader.h */
#ifndef __ELF_LOADER_H_
#define __ELF_LOADER_H_
#include <stdint.h>
typedef struct elfheader_s elfheader_t;
typedef struct box86context_s box86context_t;
/* dynarec_arm_pass.c derives `stopblock = 2 + (FindElfAddress(...) ? 0 : 1)`;
 * upstream returns non-null for the main binary, letting its blocks extend
 * under bigblock==2. Returning 0 here would force stopblock=3 everywhere,
 * giving smaller blocks and more transitions. Everything translated here is
 * the PE image plus our own stable stubs, so always report it as "in elf
 * memory", like upstream's main binary. The only other caller is a log line
 * (dynarec.c) whose ElfName() ignores the returned token. */
static inline elfheader_t* FindElfAddress(box86context_t *context, uintptr_t addr) { (void)context; (void)addr; return (elfheader_t*)1; }
static inline const char* ElfName(elfheader_t* head) { (void)head; return "?"; }
#endif
