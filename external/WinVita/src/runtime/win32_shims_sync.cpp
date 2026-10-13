// src/runtime/win32_shims_sync.cpp — see win32_shims_sync.h.
//
// Ownership, not just the type: the handle table, critical-section map, and
// id counter all live in the engine, so these bodies can too without leaving
// a duplicate copy of state on the consumer side (a state split like that is
// invisible at compile time and surfaces later as a deadlock).
//
// Instrumentation does not follow: sync tracing and wait logging, plus
// profiling counters, are observers reached through wx86_sync_notify(). The
// consumer wires its own observer back in. The engine reports; it never
// asks for permission.
#include "win32_shims_sync.h"
#include "guest_sync.h"
#include "guest_thread_ctx.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_sync_install(Bridge& br){
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };

    auto mkevent=[](Cpu&c){
        WxEvent* e=new WxEvent(); e->manual=c.arg(1)!=0; e->signaled=c.arg(2)!=0;
        uint32_t h=wx86_handle_add(e);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,e,h});
        return h; };
    K("CreateEventA",4,mkevent);
    K("CreateEventW",4,mkevent);

    K("SetEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=true;
            wx86_sync_notify({WX86_SYNC_EVENT_SET,&c,w,h});
            if(ThreadScheduler* s=wx86_sched()) s->notify(w); }
        return 1u; });

    K("ResetEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=false;
            wx86_sync_notify({WX86_SYNC_EVENT_RESET,&c,w,h}); }
        return 1u; });

    // PulseEvent: a DELIBERATE infidelity, kept as-is — wakes NO ONE, just
    // leaves the event unsignaled. Real Windows PulseEvent is itself
    // documented as unreliable; no caller depends on it.
    K("PulseEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=false;
            wx86_sync_notify({WX86_SYNC_EVENT_PULSE,&c,w,h});
            return 1u; }
        return 0u; });

    // Mutex modeled as an auto-reset event: acquiring consumes the signal,
    // releasing restores it. Faithful for a non-recursive mutex.
    auto mkmutex=[](Cpu&c){
        WxEvent* e=new WxEvent(); e->manual=false; e->signaled=(c.arg(1)==0);
        uint32_t h=wx86_handle_add(e); wx86_set_lasterr(c,0);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,e,h});
        return h; };
    K("CreateMutexA",3,mkmutex);
    K("CreateMutexW",3,mkmutex);
    K("ReleaseMutex",1,[](Cpu&c){
        Waitable* w=wx86_handle_find(c.arg(0));
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=true;
            if(ThreadScheduler* s=wx86_sched()) s->notify(w);
            return 1u; }
        return 0u; });

    auto mksema=[](Cpu&c){
        WxSemaphore* s=new WxSemaphore(); s->count=(int)c.arg(1); s->maxc=(int)c.arg(2);
        uint32_t h=wx86_handle_add(s); wx86_set_lasterr(c,0);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,s,h});
        return h; };
    K("CreateSemaphoreA",4,mksema);
    K("CreateSemaphoreW",4,mksema);

    // The cap is honored like real Windows: an overflow FAILS and leaves the
    // count intact rather than silently saturating.
    K("ReleaseSemaphore",3,[](Cpu&c){ Waitable* itw=wx86_handle_find(c.arg(0));
        if(!itw||!wx86_is_kind(itw,"sema")) return 0u;
        WxSemaphore* s=static_cast<WxSemaphore*>(itw);
        if((int64_t)s->count + (int)c.arg(1) > s->maxc){ wx86_set_lasterr(c,298); return 0u; }  // ERROR_TOO_MANY_POSTS
        if(c.arg(2)) c.write_u32(c.arg(2),(uint32_t)s->count);              // lpPreviousCount
        s->count+=(int)c.arg(1); if(ThreadScheduler* sc=wx86_sched()) sc->notify(s); return 1u; });

    K("GetExitCodeThread",2,[](Cpu&c){
        Waitable* w=wx86_handle_find(c.arg(0));
        uint32_t code=259;   // STILL_ACTIVE
        if(w && wx86_is_kind(w,"thread")){
            WxThread* k=static_cast<WxThread*>(w);
            if(k->gt && k->gt->state==GuestThread::State::Finished) code=k->gt->exit_code; }
        if(uint32_t p=c.arg(1)) c.write_u32(p,code);
        return 1u; });
}
