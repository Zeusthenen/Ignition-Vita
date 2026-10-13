// src/runtime/win32_shims_psapi.cpp — see win32_shims_psapi.h. These four
// entry points only ever read Bridge's own module table (load_base/
// image_size), never a guest-specific hardcoded path or literal, so they
// generalize cleanly. GetModuleFileNameEx*/GetModuleFileName* and the
// VERSION.dll trio stay on the consumer side because they resolve a MODULE
// PATH STRING, which is where a guest's install-path fiction lives.
#include "win32_shims_psapi.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <functional>
#include <string>
using namespace d2rt;

namespace {

uint32_t enum_process_modules(Bridge& br, Cpu& c) {
    uint32_t arr = c.arg(1), cb = c.arg(2), needed = c.arg(3);
    auto mods = br.loaded_modules();
    uint32_t n = (uint32_t)mods.size(), i = 0;
    for (auto& m : mods) { if ((i + 1) * 4 <= cb) c.write_u32(arr + i * 4, m.first); i++; }
    if (needed) c.write_u32(needed, n * 4);
    return 1u;
}

uint32_t get_module_information(Bridge& br, Cpu& c) {
    uint32_t h = c.arg(1), out = c.arg(2);
    for (auto& m : br.loaded_modules()) {
        if (m.first != h) continue;
        if (!out) return 0u;
        c.write_u32(out + 0, h);        // lpBaseOfDll
        c.write_u32(out + 4, m.second); // SizeOfImage
        c.write_u32(out + 8, h);        // EntryPoint ~ base (honest placeholder, matches prior behaviour)
        return 1u;
    }
    return 0u;
}

} // namespace

void win32_shims_psapi_install(Bridge& br) {
    auto REG = [&](const char* dll, const char* name, uint32_t ac, std::function<uint32_t(Cpu&)> fn) {
        Shim s; s.argc = ac; s.stdcall_cleanup = true; s.tag = std::string(dll) + "!" + name; s.fn = std::move(fn);
        br.register_shim(dll, name, s);
    };

    REG("KERNEL32.dll", "EnumProcessModules", 4, [&br](Cpu& c) { return enum_process_modules(br, c); });
    REG("KERNEL32.dll", "K32EnumProcessModules", 4, [&br](Cpu& c) { return enum_process_modules(br, c); });
    REG("KERNEL32.dll", "GetModuleInformation", 4, [&br](Cpu& c) { return get_module_information(br, c); });
    REG("KERNEL32.dll", "K32GetModuleInformation", 4, [&br](Cpu& c) { return get_module_information(br, c); });
    REG("PSAPI.DLL", "GetModuleInformation", 4, [&br](Cpu& c) { return get_module_information(br, c); });
}
