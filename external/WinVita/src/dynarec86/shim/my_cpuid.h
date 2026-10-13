/* D2Vita shim replacing Box86's src/include/my_cpuid.h */
#ifndef __MY_CPUID_H_
#define __MY_CPUID_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
void my_cpuid(x86emu_t* emu, uint32_t tmp32u);
/* CPUID leaf-1 feature bits as reported by my_cpuid, exposed so Win32 shims
   describing the processor (IsProcessorFeaturePresent) derive their answers
   from these instead of inventing new ones. */
void wx86_cpuid_features(uint32_t* edx, uint32_t* ecx);
#endif
