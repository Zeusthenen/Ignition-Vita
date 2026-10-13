// src/runtime/guest_sync.cpp — see guest_sync.h.
#include "guest_sync.h"
#include "runtime/cpu.h"
#include <cstring>
#include <map>
using namespace d2rt;

bool wx86_is_kind(Waitable* w, const char* k){
    return w && std::strcmp(w->kind(), k)==0;
}

// ---- Handle table ------------------------------------------------------------
static std::map<uint32_t,Waitable*> g_handles;
static uint32_t g_nextObj = 0x82000000;

uint32_t wx86_handle_add(Waitable* w){
    uint32_t h = g_nextObj++;
    g_handles[h] = w;
    return h;
}
Waitable* wx86_handle_find(uint32_t h){
    auto it = g_handles.find(h);
    return it==g_handles.end() ? nullptr : it->second;
}
void wx86_handle_erase(uint32_t h){ g_handles.erase(h); }
unsigned wx86_handle_count(){ return (unsigned)g_handles.size(); }
uint32_t wx86_handle_next_id(){ return g_nextObj++; }

// ---- Critical sections --------------------------------------------------------
static std::map<uint32_t,WxCrit*> g_crits;
static uint32_t g_csLastVa = 0;
static WxCrit*  g_csLastK  = nullptr;

WxCrit* wx86_crit_for(uint32_t cs){
    if(cs==g_csLastVa && g_csLastK) return g_csLastK;
    auto it = g_crits.find(cs);
    WxCrit* k;
    if(it!=g_crits.end()) k = it->second;
    else { k = new WxCrit(); k->va = cs; g_crits[cs] = k; }
    g_csLastVa = cs; g_csLastK = k;
    return k;
}
WxCrit* wx86_crit_lookup(uint32_t cs){
    if(cs==g_csLastVa && g_csLastK) return g_csLastK;
    auto it = g_crits.find(cs);
    if(it==g_crits.end()) return nullptr;
    g_csLastVa = cs; g_csLastK = it->second;
    return it->second;
}

bool wx86_crit_leave(uint32_t cs, ThreadScheduler* sched){
    WxCrit* k = wx86_crit_lookup(cs);
    if(!k) return false;
    if(k->count>0 && --k->count==0){
        k->owner = 0;
        if(sched) sched->notify(k);
        return true;
    }
    return false;
}

void wx86_crit_each_held(void (*fn)(uint32_t,uint32_t,int,void*), void* ud){
    if(!fn) return;
    for(auto& p : g_crits) if(p.second->owner) fn(p.first, p.second->owner, p.second->count, ud);
}

void wx86_crit_forget(uint32_t cs){
    g_crits.erase(cs);
    if(g_csLastVa==cs){ g_csLastVa = 0; g_csLastK = nullptr; }   // invalidate MRU cache
}

// ---- Observer ------------------------------------------------------------
static WxSyncObserverFn g_obs = nullptr;
void wx86_sync_set_observer(WxSyncObserverFn cb){ g_obs = cb; }
void wx86_sync_notify(const WxSyncEvent& e){ if(g_obs) g_obs(e); }
