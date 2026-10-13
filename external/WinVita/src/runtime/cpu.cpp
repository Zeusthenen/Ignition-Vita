// src/runtime/cpu.cpp — default Cpu context save/restore (backend-agnostic).
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"

namespace d2rt {

void Cpu::save_context(X86Context& c) {
    for (int i = 0; i < 8; ++i) c.gpr[i] = reg(i);   // R_EAX..R_EDI
    c.eip = reg(R_EIP);
    c.eflags = reg(R_EFLAGS);
    // fs_base is managed by the threading backend (TIB-swap in cooperative,
    // real per-thread FS base in native); not read back here.
}

void Cpu::load_context(const X86Context& c) {
    for (int i = 0; i < 8; ++i) set_reg(i, c.gpr[i]);
    set_reg(R_EIP, c.eip);
    set_reg(R_EFLAGS, c.eflags);
}

} // namespace d2rt
