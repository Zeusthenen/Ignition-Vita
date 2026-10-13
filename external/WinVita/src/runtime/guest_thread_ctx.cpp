// src/runtime/guest_thread_ctx.cpp — see guest_thread_ctx.h.
#include "guest_thread_ctx.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"
using namespace d2rt;

static ThreadScheduler* g_wxSched = nullptr;
static uint32_t         g_wxMainTib = 0;

void wx86_set_main_tib(uint32_t tib){ g_wxMainTib = tib; }
void wx86_set_scheduler(ThreadScheduler* s){ g_wxSched = s; }

ThreadScheduler* wx86_sched(){ return g_wxSched; }

uint32_t wx86_cur_tib(){
    return (g_wxSched && g_wxSched->current()) ? g_wxSched->current()->tib
                                              : g_wxMainTib;
}

// 0x34 = TEB.LastErrorValue, fixed offset in the 32-bit Win32 ABI.
void     wx86_set_lasterr(Cpu& c, uint32_t v){ c.write_u32(wx86_cur_tib()+0x34, v); }
uint32_t wx86_get_lasterr(Cpu& c)            { return c.read_u32(wx86_cur_tib()+0x34); }
