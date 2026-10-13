// src/runtime/win32_shims_advapi32.cpp — see win32_shims_advapi32.h. These
// bodies carry no D2/Blizzard-specific literal, no game-state dependency,
// and no dependency on any consumer-hosted helper — unlike the registry
// emulation (hardcoded install-path strings) and a handful of security/SCM
// calls that do use consumer-hosted helpers, which stay in the consumer's
// own shim file (win32_shims_advapi32_d2.cpp).
//
// No consumer-side tracing here: winx86 links standalone with no knowledge
// of any specific consumer, so debug-log coverage of these calls lives on
// the consumer side, not here.
#include "win32_shims_advapi32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

void win32_shims_advapi32_install(Bridge& br){
    auto A=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("ADVAPI32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("ADVAPI32.dll",name,s);
    };
    // Security/token API (D2Game realm-startup admin check, via dynamic
    // LoadLibrary("advapi32")+GetProcAddress): report "running as admin".
    A("FreeSid",1,[](Cpu&){ return 0u; });
    A("EqualSid",2,[](Cpu&){ return 1u; });
    A("OpenProcessToken",3,[](Cpu&c){ if(c.arg(2)) c.write_u32(c.arg(2),0x8A000001u); return 1u; });
    A("OpenThreadToken",4,[](Cpu&c){ if(c.arg(3)) c.write_u32(c.arg(3),0x8A000001u); return 1u; });
    A("GetTokenInformation",5,[](Cpu&c){ uint32_t buf=c.arg(2),len=c.arg(3),ret=c.arg(4);
        if(buf&&len){ std::vector<uint8_t> z(len,0); c.write(buf,z.data(),len); }
        if(ret) c.write_u32(ret,len?len:4); return 1u; });
    // ACL / security-descriptor family (all "succeed" — the realm creates a
    // secured object; we have no security model, any descriptor is fine).
    A("InitializeAcl",3,[](Cpu&c){ uint32_t p=c.arg(0),n=c.arg(1);
        if(p&&n){ std::vector<uint8_t> z(n,0); c.write(p,z.data(),n); } return 1u; });
    A("GetLengthSid",1,[](Cpu&){ return 12u; });
    A("AddAccessAllowedAce",4,[](Cpu&){ return 1u; });
    A("AddAccessDeniedAce",4,[](Cpu&){ return 1u; });
    A("AddAce",5,[](Cpu&){ return 1u; });
    A("InitializeSecurityDescriptor",2,[](Cpu&c){ uint32_t p=c.arg(0);
        if(p){ std::vector<uint8_t> z(20,0); z[0]=1; c.write(p,z.data(),20); } return 1u; });
    A("SetSecurityDescriptorDacl",4,[](Cpu&){ return 1u; });
    A("SetSecurityDescriptorOwner",3,[](Cpu&){ return 1u; });
    A("SetSecurityDescriptorGroup",3,[](Cpu&){ return 1u; });
    A("SetSecurityDescriptorSacl",4,[](Cpu&){ return 1u; });
    A("GetSecurityDescriptorLength",1,[](Cpu&){ return 20u; });
    A("IsValidSecurityDescriptor",1,[](Cpu&){ return 1u; });
    A("IsValidSid",1,[](Cpu&){ return 1u; });
    A("IsValidAcl",1,[](Cpu&){ return 1u; });
    A("GetSidLengthRequired",1,[](Cpu&){ return 12u; });
    A("InitializeSid",3,[](Cpu&){ return 1u; });
    A("CopySid",3,[](Cpu&){ return 1u; });
    A("LookupPrivilegeValueA",3,[](Cpu&c){ if(c.arg(2)){ c.write_u32(c.arg(2),0x14); c.write_u32(c.arg(2)+4,0);} return 1u; });
    A("AdjustTokenPrivileges",6,[](Cpu&){ return 1u; });
    A("DuplicateToken",3,[](Cpu&c){ if(c.arg(2)) c.write_u32(c.arg(2),0x8A000002u); return 1u; });
    A("DuplicateTokenEx",6,[](Cpu&c){ if(c.arg(5)) c.write_u32(c.arg(5),0x8A000002u); return 1u; });
    A("ImpersonateLoggedOnUser",1,[](Cpu&){ return 1u; });
    A("RevertToSelf",0,[](Cpu&){ return 1u; });
    A("LookupAccountNameA",7,[](Cpu&){ return 1u; });
    A("GetSecurityInfo",8,[](Cpu&){ return 0u; });
    A("SetSecurityInfo",7,[](Cpu&){ return 0u; });
    A("CloseServiceHandle",1,[](Cpu&){ return 1u; });
    A("QueryServiceStatus",2,[](Cpu&){ return 0u; });
    A("StartServiceA",3,[](Cpu&){ return 0u; });
    A("ControlService",3,[](Cpu&){ return 0u; });
    // KNOWN GAP: writes to `b` without checking b != 0, though
    // GetUserNameA(NULL,&n) is the normal Win32 idiom to query the required
    // buffer size first. Left as-is to avoid changing behavior here.
    A("GetUserNameA",2,[](Cpu&c){ uint32_t b=c.arg(0),pn=c.arg(1); const char* u="player";
        c.write(b,u,7); if(pn) c.write_u32(pn,7); return 1u; });
    // Admin-membership check some installers/anti-tamper routines perform —
    // report TRUE so it proceeds.
    A("CheckTokenMembership",3,[](Cpu&c){ if(c.arg(2)) c.write_u32(c.arg(2),1); return 1u; });
}
