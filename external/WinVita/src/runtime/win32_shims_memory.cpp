// src/runtime/win32_shims_memory.cpp — see win32_shims_memory.h.
//
// Occupancy counters and log lines are observer notifications, emitted at
// the same points and in the same order a log needs them, so a consumer's
// own log interleaving stays intact. The engine itself never caps how often
// it notifies — e.g. every MEM_RELEASE-miss reads the guest caller's return
// address, a rare error path — any capping for a log belongs to the
// observer, not here.
#include "win32_shims_memory.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "guest_region.h"
#include "layout.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>
using namespace d2rt;

// ---- zero-filling a guest range ---------------------------------------------
void wx86_gzero(Cpu& c, uint32_t va, uint32_t n){
    if(!n) return;
    if(void* hp=c.hostptr(va,n)){ c.invalidate_code(va,n); std::memset(hp,0,n); return; }
    std::vector<uint8_t> z(n<65536?n:65536,0);
    for(uint32_t o=0;o<n;){ uint32_t k=(n-o)>(uint32_t)z.size()?(uint32_t)z.size():(n-o); c.write(va+o,z.data(),k); o+=k; } }
static inline void gzero(Cpu& c, uint32_t va, uint32_t n){ wx86_gzero(c,va,n); }

// ---- state owned by this file -----------------------------------------------
static Wx86MemoryPlan  g_plan;
static WxMemObserverFn g_obs = nullptr;
static uint32_t        g_physMB = 256u;

void wx86_mem_set_observer(WxMemObserverFn cb){ g_obs = cb; }
void wx86_mem_set_total_phys_mb(uint32_t mb){ if(mb) g_physMB = mb; }
static inline void note(int kind, Cpu* c, uint32_t addr, uint32_t size,
                        uint32_t type=0, uint32_t protect=0, uint32_t ra=0){
    if(g_obs){ WxMemEvent e{kind,c,addr,size,type,protect,ra}; g_obs(e); } }

void win32_shims_memory_install(Bridge& br, const Wx86MemoryPlan& plan){
    g_plan = plan;
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };

    // HeapCreate hands out a handle DISTINCT from GetProcessHeap (0x00AA0000)
    // so two heap handles never collide (honest: a private heap != process
    // heap). Backing is shared (halloc) — HeapAlloc/Free ignore the handle.
    K("HeapCreate",3,[](Cpu&){ static uint32_t next=0x00AB0000u; uint32_t h=next; next+=0x10000u; return h; });
    K("GetProcessHeap",0,[](Cpu&){ return 0x00AA0000u; });
    K("HeapAlloc",3,[](Cpu&c){ uint32_t n=c.arg(2),a=g_plan.heap->alloc(n);
        if(a && (c.arg(1)&0x8u)) gzero(c,a,n);  // HEAP_ZERO_MEMORY
        return a; });
    K("HeapReAlloc",4,[](Cpu&c){ uint32_t old=c.arg(2),n=c.arg(3),a=g_plan.heap->alloc(n);
        if(old&&a){ uint32_t osz=g_plan.heap->size_of(old); uint32_t cp=osz&&osz<n?osz:n;
            void* dhp=c.hostptr(a,cp); void* shp=c.hostptr(old,cp);
            if(dhp&&shp){ c.invalidate_code(a,cp); std::memcpy(dhp,shp,cp); }   // barrier == c.write()
            else { std::vector<uint8_t> tmp(cp,0); c.read(old,tmp.data(),cp); c.write(a,tmp.data(),cp); }
            g_plan.heap->free(old);} return a; });
    K("HeapFree",3,[](Cpu&c){ g_plan.heap->free(c.arg(2)); return 1u; });
    K("HeapSize",3,[](Cpu&c){ uint32_t s=g_plan.heap->size_of(c.arg(2)); return s?s:0u; });

    // Local/GlobalAlloc: backed by the SAME heap. Left unshimmed, the stdcall
    // arg-cleanup mismatch shifts ESP and returns 0 — enough to crash a
    // guest thread.
    K("LocalAlloc",2,[](Cpu&c){ uint32_t fl=c.arg(0),n=c.arg(1),a=g_plan.heap->alloc(n);
        if(a && (fl&0x40u)) gzero(c,a,n); return a; });  // LMEM_ZEROINIT
    K("LocalFree",1,[](Cpu&c){ g_plan.heap->free(c.arg(0)); return 0u; });     // NULL on success
    K("GlobalAlloc",2,[](Cpu&c){ uint32_t fl=c.arg(0),n=c.arg(1),a=g_plan.heap->alloc(n);
        if(a && (fl&0x40u)) gzero(c,a,n); return a; });  // GMEM_ZEROINIT
    K("GlobalFree",1,[](Cpu&c){ g_plan.heap->free(c.arg(0)); return 0u; });

    // VirtualAlloc: MEM_COMMIT inside an already-reserved block must return
    // that address (blocks are fully backed at reserve time), not a fresh
    // one. Committed memory is ZEROED (Win32 guarantee — Storm relies on it).
    // Windows guarantees MEM_COMMIT hands back ZERO pages for any page that
    // was decommitted (or never committed); already-committed pages keep
    // their content. Returning stale bytes on recommit would corrupt code
    // that decommit/recommit-cycles buffers (sprite caches) and expects
    // zeros, so decommitted pages are tracked and zeroed exactly on recommit.
    static std::set<uint32_t> g_decommitted;   // page VAs decommitted, backing kept
    K("VirtualAlloc",4,[](Cpu&c){ uint32_t hint=c.arg(0),sz=c.arg(1)?c.arg(1):0x1000,ty=c.arg(2);
        if(hint){ uint32_t blk=g_plan.va->block_of(hint); if(blk){
            note(WX86_MEM_VA_COMMIT_IN,&c,hint,sz,ty,c.arg(3));   // commit inside an existing reservation
            uint32_t p0=hint&~0xFFFu, p1=(hint+sz+0xFFFu)&~0xFFFu;
            for(uint32_t p=p0;p<p1;p+=4096){ auto it=g_decommitted.find(p);
                if(it!=g_decommitted.end()){ gzero(c,p,4096); g_decommitted.erase(it); } }
            return hint; } }
        note(ty&0x2000u?WX86_MEM_VA_RESERVE:WX86_MEM_VA_COMMIT_FRESH,&c,hint,sz,ty,c.arg(3));
        if(sz>=0x100000){ uint32_t ra=c.read_u32(c.reg(R_ESP));
            note(WX86_MEM_VA_BIG,&c,hint,sz,ty,c.arg(3),ra); }
        // A huge request (>= 2 GiB) is a corrupted size upstream — record it
        // before it fails.
        if(sz>=0x80000000u){ uint32_t ra=c.read_u32(c.reg(R_ESP));
            note(WX86_MEM_VA_GARBAGE,&c,hint,sz,ty,c.arg(3),ra); }
        uint32_t a=g_plan.va->alloc(sz);
        if(!a){ uint32_t ra=c.read_u32(c.reg(R_ESP));           // record the faulting site
            note(WX86_MEM_VA_FAIL,&c,hint,sz,ty,c.arg(3),ra); }
        if(a) gzero(c,a,sz);
        return a; });
    K("VirtualFree",3,[](Cpu&c){ uint32_t a=c.arg(0),sz=c.arg(1),ft=c.arg(2);
        if(ft&0x8000u){ uint32_t blk=g_plan.va->block_of(a)?g_plan.va->block_of(a):a;  // MEM_RELEASE
            uint32_t bs=g_plan.va->size_of(blk);
            if(g_plan.va->free(blk)) note(WX86_MEM_VA_RELEASE,&c,blk,bs,ft);
            else { uint32_t ra=c.read_u32(c.reg(R_ESP));         // a LOST free = arena leak
                note(WX86_MEM_VA_RELEASE_MISS,&c,a,bs,ft,0,ra); }
            return 1u; }
        if(ft&0x4000u){                                                      // MEM_DECOMMIT: keep backing,
            uint32_t p0=a&~0xFFFu, p1=(a+(sz?sz:1)+0xFFFu)&~0xFFFu;          // but a later recommit must zero
            note(WX86_MEM_VA_DECOMMIT,&c,p0,p1-p0,ft);
            for(uint32_t p=p0;p<p1;p+=4096) g_decommitted.insert(p); }
        return 1u; });
    // VirtualProtect: RX->RW->modify->RX is the canonical well-behaved JIT
    // sequence (Storm patches in memory). Honestly re-validate any translated
    // code in the range (mark needtest -> re-hash on next execute) and write
    // lpflOldProtect. We do NOT apply a restrictive host protection (mapping
    // .text read-only would break dynarec/IAT patching) — the backing stays RWX.
    K("VirtualProtect",4,[](Cpu&c){ uint32_t a=c.arg(0),sz=c.arg(1);
        if(c.arg(3)) c.write_u32(c.arg(3), 0x40u);   // lpflOldProtect = PAGE_EXECUTE_READWRITE
        c.invalidate_code(a,sz);
        return 1u; });
    // VirtualQuery: honest region introspection (a Warden's #1 memory-scan
    // primitive: GetSystemInfo then VirtualQuery region-by-region). Reports the
    // real module / heap / VA-arena / stack regions as MEM_COMMIT and the gaps
    // as MEM_FREE. Fills MEMORY_BASIC_INFORMATION (28 bytes) and returns 28.
    // The FORMAT is Win32's; the PLAN it describes is the consumer's.
    K("VirtualQuery",3,[](Cpu&c){ uint32_t addr=c.arg(0),buf=c.arg(1),len=c.arg(2);
        if(!buf || len<28) return 0u;
        uint32_t base=0,size=0,state=0x10000u,type=0,protect=0x01u,allocbase=0;   // default MEM_FREE/NOACCESS
        if(!g_plan.vquery || !g_plan.vquery(addr,base,size,state,type,protect,allocbase)) return 0u;
        c.write_u32(buf+0x00, base);
        c.write_u32(buf+0x04, allocbase);
        c.write_u32(buf+0x08, state==0x1000u?0x40u:0u);   // AllocationProtect
        c.write_u32(buf+0x0C, size);                       // RegionSize
        c.write_u32(buf+0x10, state);                      // State
        c.write_u32(buf+0x14, protect);                    // Protect
        c.write_u32(buf+0x18, type);                       // Type
        return 28u; });
    // FlushInstructionCache: THE documented Win32 "I modified code, re-fetch it"
    // signal (Storm / a JIT self-patcher calls it after rewriting a buffer).
    // Shimming it removes an UNSHIMMED halt AND is an honest SMC-invalidation
    // hook that benefits any well-behaved dynamic code.
    K("FlushInstructionCache",3,[](Cpu&c){ uint32_t a=c.arg(1),sz=c.arg(2)?c.arg(2):1;
        c.invalidate_code(a,sz); return 1u; });

    K("GetSystemInfo",1,[](Cpu&c){ uint32_t p=c.arg(0);
        c.write_u32(p+0,0);            // wProcessorArchitecture=PROCESSOR_ARCHITECTURE_INTEL, wReserved
        c.write_u32(p+4,0x1000);       // dwPageSize = 4096
        c.write_u32(p+8, d2rt::layout_user_lo());       // lpMinimumApplicationAddress (0x00010000 by default)
        c.write_u32(p+12,d2rt::layout_user_hi()-1u);    // lpMaximumApplicationAddress (0x7FFEFFFF by default)
        c.write_u32(p+16,1);           // dwActiveProcessorMask
        c.write_u32(p+20,1);           // dwNumberOfProcessors
        c.write_u32(p+24,586);         // dwProcessorType
        c.write_u32(p+28,0x10000);     // dwAllocationGranularity = 64 KiB
        c.write_u32(p+32,6);           // wProcessorLevel/Revision
        return 0u; });
    // MEMORYSTATUS with REAL values: many games size their caches off
    // dwTotalPhys, and a made-up value makes them either bail out or
    // reserve far more than the target actually has. The reported amount
    // is the consumer's decision (wx86_mem_set_total_phys_mb).
    K("GlobalMemoryStatus",1,[](Cpu&c){ uint32_t p=c.arg(0);
        const uint32_t physMB = g_physMB;
        c.write_u32(p+0,32);           // dwLength
        c.write_u32(p+4,25);           // dwMemoryLoad %
        c.write_u32(p+8, physMB*1024u*1024u);         // dwTotalPhys
        c.write_u32(p+12,(physMB*1024u*1024u)/4u*3u); // dwAvailPhys = 75 % of total
        c.write_u32(p+16,512u*1024*1024);   // dwTotalPageFile
        c.write_u32(p+20,400u*1024*1024);   // dwAvailPageFile
        c.write_u32(p+24,0x0C000000);       // dwTotalVirtual = 192 MiB (Vita-realistic; pool ~ avail/2)
        c.write_u32(p+28,0x08000000);       // dwAvailVirtual = 128 MiB
        return 0u; });
}
