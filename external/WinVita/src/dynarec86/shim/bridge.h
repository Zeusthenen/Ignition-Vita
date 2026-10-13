/* D2Vita shim replacing Box86's src/include/bridge.h */
#ifndef __BRIDGE_H_
#define __BRIDGE_H_
#include <stdint.h>
typedef struct bridge_s bridge_t;
typedef struct x86emu_s x86emu_t;
typedef void (*wrapper_t)(x86emu_t* emu, uintptr_t fnc);
static inline void* GetNativeFnc(uintptr_t fnc) { (void)fnc; return 0; }
static inline void* GetNativeFncOrFnc(uintptr_t fnc) { return (void*)fnc; }
/* Translation-time redirect table ("guest pristine" mode): when the dynarec
 * translates a call/jmp whose GUEST target is a registered "from", it emits
 * the branch to "to" instead, WITHOUT touching guest memory. Used to run a
 * native shim in place of a genuine, unpatched guest function.
 *
 * hasAlternate() is checked on every dynablock lookup -- it is the FIRST line
 * of internalDBGetBlock (third_party/box86-dynarec/dynarec/dynablock.c:258),
 * not just at translation time. The scan itself is cheap (linear, small N,
 * an already-hot pointer).
 *
 * Set via dyn86_set_alternate (src/dynarec86/alt_table.c, which owns the full
 * contract and its self-test). The backing arrays grow by doubling with no
 * fixed cap; the only failure mode (allocation refused) is counted in
 * dyn86_alt_refus and logged. */
extern uintptr_t *dyn86_alt_from;
extern uintptr_t *dyn86_alt_to;
extern int dyn86_alt_n;       /* alternates set; published after the entry write */
extern int dyn86_alt_refus;   /* alternates lost (allocation refused) */
static inline int hasAlternate(void* addr) {
    for(int i=0;i<dyn86_alt_n;i++) if(dyn86_alt_from[i]==(uintptr_t)addr) return 1;
    return 0;
}
static inline void* getAlternate(void* addr) {
    for(int i=0;i<dyn86_alt_n;i++) if(dyn86_alt_from[i]==(uintptr_t)addr) return (void*)dyn86_alt_to[i];
    return addr;
}
static inline uintptr_t CheckBridged(bridge_t* bridge, void* fnc) { (void)bridge; (void)fnc; return 0; }
#endif
