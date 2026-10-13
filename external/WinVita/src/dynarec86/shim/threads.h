/* D2Vita shim replacing Box86's src/include/threads.h */
#ifndef __THREADS_H_
#define __THREADS_H_
typedef struct x86emu_s x86emu_t;
x86emu_t* thread_get_emu(void);
void thread_set_emu(x86emu_t* emu);
// mutex unlock helper used by custommem's unlockCustommemMutex
int checkUnlockMutex(void* m);
#endif
