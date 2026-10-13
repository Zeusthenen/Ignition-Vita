/* src/dynarec86/shim/vita/sys/mman.h — mmap façade for the Box86 dynarec on
 * PS Vita (VitaSDK newlib has no <sys/mman.h>).
 *
 * Only the surface custommem.c actually uses is provided:
 *   - mmap(NULL, len, RW,  ANON|PRIVATE)  -> USER_RW memblock (internal allocator)
 *   - mmap(NULL, len, RWX, ANON|PRIVATE)  -> VM-domain memblock (dynarec chunks)
 *   - munmap(exact base, len)             -> free the memblock
 *   - mprotect(...)                        -> no-op (W^X is the VM domain's job;
 *     we never rely on the SIGSEGV write-barrier — no signals on Vita, and the
 *     runtime avoids guest SMC by design)
 * Implementation: mman_vita.c (sceKernelAllocMemBlock/ForVM + OpenVMDomain).
 */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
// Linux-only hints unused on Vita (single-arena mode never takes the fixed
// paths); defined so the shared runtime compiles unchanged.
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t len, int prot);

/* icache/dcache sync after code emission — replaces GCC's __clear_cache
 * (which would issue a Linux cacheflush syscall that doesn't exist here).
 * The build redefines __clear_cache(b,e) to this via -D. */
void dyn86_vita_clear_cache(void* beg, void* end);
/* Per-thread VM domain: any thread that may WRITE the JIT arena must call
 * this before its first write (idempotent per thread; DACR is per-thread
 * context -- full comment in mman_vita.c). mmap() covers the allocating
 * thread; NativeScheduler calls this on entry to main and to each runner. */
void dyn86_vita_open_vm_thread(void);

#ifdef __cplusplus
}
#endif
