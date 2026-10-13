// src/runtime/win32_shims_shell32.cpp — see win32_shims_shell32.h. These two
// bodies carry no guest-specific literal or behavior (SHAppBarMessage
// always reports "not handled"; ShellExecuteA always reports success),
// unlike SHGetFolderPathA in the same original group, which hardcodes a
// game install path and stays a consumer-side shim.
#include "win32_shims_shell32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_shell32_install(Bridge& br){
    auto SH=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("SHELL32.dll!")+name;
        s.fn=[fn](Cpu&c){ return fn(c); };
        br.register_shim("SHELL32.dll",name,s); };
    SH("SHAppBarMessage",2,[](Cpu&){ return 0u; });
    SH("ShellExecuteA",6,[](Cpu&){ return 33u; });                         // > 32 == success
}
