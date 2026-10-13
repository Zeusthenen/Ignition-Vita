/* D2Vita shim replacing Box86's src/dynarec/arm_printer.h */
#ifndef __ARM_PRINTER_H_
#define __ARM_PRINTER_H_
#include <stdint.h>
static inline const char* arm_print(uint32_t opcode) { (void)opcode; return "?"; }
#endif
