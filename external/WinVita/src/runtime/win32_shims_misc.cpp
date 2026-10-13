// src/runtime/win32_shims_misc.cpp — see win32_shims_misc.h. DirectDraw/
// Bink/Smacker/ijl11 stubs: all constant "unavailable"/"not implemented"
// returns with zero state and zero dependency on any consumer-hosted
// helper. The surrounding catch-all group (remaining KERNEL32/USER32/GDI32/
// ADVAPI32/IMM32 entries, glide3x, VERSION/PSAPI module introspection)
// stays on the consumer side.
#include "win32_shims_misc.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_misc_install(Bridge& br){
    auto REG=[&](const char* dll,const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string(dll)+"!"+name; s.fn=std::move(fn);
        br.register_shim(dll,name,s); };
    auto REGORD=[&](const char* dll,uint32_t ord,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string(dll)+"!#"+std::to_string(ord); s.fn=std::move(fn);
        br.register_shim_ordinal(dll,ord,s); };

    // DirectDraw unavailable (DDERR_GENERIC) — a guest falls back to a
    // different renderer, same contract as a machine with no DirectDraw
    // driver installed.
    REG("DDRAW.dll","DirectDrawCreate",3,[](Cpu&){ return 0x80004005u; });
    REG("DDRAW.dll","DirectDrawEnumerateA",2,[](Cpu&){ return 0u; });             // DD_OK, no device enumerated

    // Bink/Smacker: NULL "player unavailable" — a caller that handles a
    // missing codec gracefully skips the video cleanly.
    REG("binkw32.dll","_BinkOpen@8",2,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkClose@4",1,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkDoFrame@4",1,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkNextFrame@4",1,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkWait@4",1,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkCopyToBuffer@28",7,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkDDSurfaceType@4",1,[](Cpu&){ return 0u; });
    REG("binkw32.dll","_BinkSetSoundSystem@8",2,[](Cpu&){ return 1u; });
    REG("binkw32.dll","_BinkOpenDirectSound@4",1,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackOpen@12",3,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackClose@4",1,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackDoFrame@4",1,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackNextFrame@4",1,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackWait@4",1,[](Cpu&){ return 0u; });
    REG("smackw32.dll","_SmackToBuffer@28",7,[](Cpu&){ return 0u; });

    // ijl11 (Intel JPEG, by ordinal): clean error path.
    REGORD("ijl11.dll",2,1,[](Cpu&){ return 0xFFFFFFFEu; });                      // ijlFree   -> clean error
    REGORD("ijl11.dll",3,1,[](Cpu&){ return 0xFFFFFFFEu; });                      // ijlRead
    REGORD("ijl11.dll",5,1,[](Cpu&){ return 0xFFFFFFFEu; });                      // ijlInit? (failure -> error path)

    // IMM32: no IME present, consistent responses.
    REG("IMM32.dll","ImmGetContext",1,[](Cpu&){ return 0u; });
    REG("IMM32.dll","ImmReleaseContext",2,[](Cpu&){ return 1u; });
    REG("IMM32.dll","ImmIsIME",1,[](Cpu&){ return 0u; });
    REG("IMM32.dll","ImmGetOpenStatus",1,[](Cpu&){ return 0u; });
    REG("IMM32.dll","ImmSetOpenStatus",2,[](Cpu&){ return 1u; });
    REG("IMM32.dll","ImmGetConversionStatus",3,[](Cpu&c){ if(c.arg(1)) c.write_u32(c.arg(1),0); if(c.arg(2)) c.write_u32(c.arg(2),0); return 1u; });
    REG("IMM32.dll","ImmSetConversionStatus",3,[](Cpu&){ return 1u; });
    REG("IMM32.dll","ImmGetCandidateListA",4,[](Cpu&){ return 0u; });
    REG("IMM32.dll","ImmGetCandidateListCountA",2,[](Cpu&c){ if(c.arg(1)) c.write_u32(c.arg(1),0); return 0u; });
    REG("IMM32.dll","ImmGetCompositionStringA",4,[](Cpu&){ return 0u; });
    REG("IMM32.dll","ImmSimulateHotKey",2,[](Cpu&){ return 1u; });
}
