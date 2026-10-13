/* src/dynarec86/shim/vita/dyn86_vita_compat.h — newlib compat for the Box86
 * core on PS Vita. Pulled in by x86emu_private.h under __vita__.
 *
 * sigjmp_buf/sigsetjmp exist in VitaSDK newlib only for Cygwin/RTEMS builds;
 * Box86 uses its jmpbuf solely to unwind out of the SIGSEGV handler during
 * translation, and Vita homebrew has no signals at all — so plain
 * setjmp/longjmp carry the exact same (never-taken) role. */
#pragma once
#include <setjmp.h>

#ifndef sigsetjmp
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val)     longjmp(env, val)
#endif

/* __clear_cache is rewritten to this by the build (-D…): icache/dcache sync
 * via sceKernelSyncVMDomain — the Linux cacheflush syscall doesn't exist. */
void dyn86_vita_clear_cache(void* beg, void* end);
