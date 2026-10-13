/* src/dynarec86/shim/vita/dlfcn.h — stub for the single dlsym call in
 * custommem.c: `cur_brk = dlsym(RTLD_NEXT, "__curbrk")`, which is NULL-guarded
 * at every use (brk emulation approximation). No dynamic linking on Vita. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_NEXT    ((void*)-1)
#define RTLD_DEFAULT ((void*)0)

static inline void* dlsym(void* handle, const char* symbol) {
    (void)handle; (void)symbol; return 0;
}

#ifdef __cplusplus
}
#endif
