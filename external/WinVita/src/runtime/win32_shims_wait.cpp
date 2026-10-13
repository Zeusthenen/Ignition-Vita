// src/runtime/win32_shims_wait.cpp — see win32_shims_wait.h.
//
// Every trace, counter, or timer is a notification emitted at the same
// point, in the same order it would have logged. No observer armed means no
// trace — the same as these switches being off by default.
#include "win32_shims_wait.h"
#include "guest_sync.h"
#include "guest_thread_ctx.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
using namespace d2rt;

static WxWaitObserverFn g_obs = nullptr;
void wx86_wait_set_observer(WxWaitObserverFn cb){ g_obs = cb; }
static inline void note(const WxWaitEvent& e){ if(g_obs) g_obs(e); }

// The shared "never signaled" event: a manual event that's never signaled,
// so a wait on it can only end via its timeout.
static WxEvent* never_event(){
    static WxEvent* nv=nullptr;
    if(!nv){ nv=new WxEvent(); nv->manual=true; nv->signaled=false; }
    return nv; }

void wx86_wait_sleep(uint32_t ms){
    ThreadScheduler* s = wx86_sched();
    if(!s) return;
    if(!ms){ s->yield(); return; }
    s->wait(never_event(), ms);
}

void win32_shims_wait_install(Bridge& br){
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };

    K("WaitForSingleObject",2,[](Cpu&c)->uint32_t{
        ThreadScheduler* sch=wx86_sched(); if(!sch) return 0u;
        const uint32_t h=c.arg(0), to=c.arg(1);   // two arg()s, nine more (see EnterCriticalSection)
        Waitable* itw=wx86_handle_find(h);
        // UNKNOWN handle => reported "signaled" immediately, WITHOUT waiting.
        // If an async loader waits on such a handle it never blocks, which
        // is a race — reported so the consumer can find out.
        if(!itw){ note({WX86_WAIT_SINGLE_UNKNOWN,&c,nullptr,h,to,0,0,0,0}); return 0u; }
        note({WX86_WAIT_SINGLE_ENTER,&c,itw,h,to,0,0,0,0});
        uint32_t r=sch->wait(itw,to);
        note({WX86_WAIT_SINGLE_EXIT,&c,itw,h,to,r,0,0,0});
        return r; });

    K("WaitForMultipleObjects",4,[](Cpu&c)->uint32_t{
        ThreadScheduler* sch=wx86_sched(); if(!sch) return 0u;
        uint32_t n=c.arg(0),pa=c.arg(1),all=c.arg(2),to=c.arg(3);
        // ONE WxMultiWait per guest thread, REUSED rather than allocated per
        // call and deleted after the wait. On the coop scheduler, wait() on
        // the blocked path stores the object in cur_->wait_obj and returns a
        // placeholder immediately (the actual yield happens after this shim
        // returns, EAX gets patched on wake) — so deleting after the wait
        // would free the object while every pending pick_ready() pass still
        // references it, a host use-after-free.
        //
        // Reuse is safe on BOTH backends: a thread has at most one
        // outstanding wait, and it can only re-enter this shim after that
        // wait completed (coop clears wait_obj on wake before the thread runs
        // again; native completes the wait inside wait() itself). Leaked by
        // design — one object per guest thread, reclaimed by the OS at exit.
        static std::map<uint32_t,WxMultiWait*> permw;
        uint32_t curtid = sch->current() ? sch->current()->id : 0;
        WxMultiWait*& mwslot = permw[curtid];
        if(!mwslot) mwslot = new WxMultiWait();
        WxMultiWait* mw=mwslot; mw->objs.clear(); mw->last=0; mw->all=all!=0;
        for(uint32_t i=0;i<n;i++){ Waitable* itw=wx86_handle_find(c.read_u32(pa+4*i)); mw->objs.push_back(itw?itw:nullptr); }
        // Unknown handles must NOT look permanently signaled (that made a
        // Storm I/O worker think its shutdown event had fired). Treat unknown
        // as never-signaled.
        struct KNever:Waitable{bool try_acquire(GuestThread*)override{return false;}bool ready(GuestThread*)override{return false;}const char*kind()const override{return "?"; }};
        static KNever never; for(auto& o:mw->objs) if(!o) o=&never;
        note({WX86_WAIT_MULTI_ENTER,&c,mw,0,to,0,n,pa,all});
        uint32_t r=sch->wait(mw,to);
        note({WX86_WAIT_MULTI_EXIT,&c,mw,0,to,r,n,pa,all});
        // NO delete here: on the coop blocked path the wait is still pending
        // when this shim returns (see the reuse comment above).
        return r; });

    K("SleepEx",2,[](Cpu&c){ wx86_wait_sleep(c.arg(0)); return 0u; });
    K("SwitchToThread",0,[](Cpu&){ if(ThreadScheduler* s=wx86_sched()) s->yield(); return 1u; });

    // ---- I/O completion port ---------------------------------------------------
    // A task system parks its worker thread on GetQueuedCompletionStatus and
    // feeds it work via PostQueuedCompletionStatus. Nothing guest-specific:
    // the WxIocp object already lives in the engine (guest_sync.h), and its
    // three shims join it here.
    K("CreateIoCompletionPort",4,[](Cpu&c)->uint32_t{ uint32_t ex=c.arg(1);
        if(ex) return ex;                          // associate file w/ existing port
        WxIocp* p=new WxIocp(); p->cpu=&c; return wx86_handle_add(p); });
    K("GetQueuedCompletionStatus",5,[](Cpu&c)->uint32_t{ Waitable* itw=wx86_handle_find(c.arg(0));
        if(!itw||!wx86_is_kind(itw,"iocp")) return 0u;
        WxIocp* p=static_cast<WxIocp*>(itw);
        ThreadScheduler* sch=wx86_sched();
        GuestThread* cur=sch?sch->current():nullptr;
        if(cur) p->outp[cur]={c.arg(1),c.arg(2),c.arg(3)};
        if(p->try_acquire(cur)) return 1u;         // immediate completion
        if(!sch) return 0u;
        // Pre-write the timeout outcome (FALSE + null packet); a signal wake
        // overwrites via try_acquire. GQCS returns BOOL: a timed-out wake must
        // NOT come back EAX=0x102 (TRUE) with a garbage packet — under the
        // real clock that fired constantly and workers executed phantom jobs
        // (wild-pointer crash during a level load).
        if(c.arg(1)) c.write_u32(c.arg(1),0);
        if(c.arg(2)) c.write_u32(c.arg(2),0);
        if(c.arg(3)) c.write_u32(c.arg(3),0);
        return sch->wait_timeout_result(p,c.arg(4),0); });  // blocks; EAX=1 on wake, 0 on timeout
    K("PostQueuedCompletionStatus",4,[](Cpu&c)->uint32_t{ Waitable* itw=wx86_handle_find(c.arg(0));
        if(!itw||!wx86_is_kind(itw,"iocp")) return 0u;
        WxIocp* p=static_cast<WxIocp*>(itw);
        p->q.push_back({c.arg(1),c.arg(2),c.arg(3)});
        if(ThreadScheduler* s=wx86_sched()) s->notify(p);
        return 1u; });
}
