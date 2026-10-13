/* D2Vita shim replacing Box86's src/include/x86tls.h */
#ifndef __X86TLS_H_
#define __X86TLS_H_
#include <stdint.h>
static inline void* GetSegmentBase(uint32_t desc) { (void)desc; return 0; }
#endif
