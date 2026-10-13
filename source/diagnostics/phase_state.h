// diagnostics/phase_state.h — the global "what were we doing" marker.
//
// The full roadmap from the project brief, named up front so later phases
// slot into the same enum instead of needing a header change each time.
// Phase 1 (this iteration) only ever reaches up through Running/Stopped/
// Error — everything from RegisterClass onward is unreachable until a
// later iteration implements the window/message-loop/DirectDraw shims.
#pragma once

namespace ign {

enum class Phase {
    Boot,
    WinVitaInit,
    PeOpen,
    PeParse,
    PeMap,
    Relocation,
    Imports,
    CpuSetup,
    Entrypoint,
    CrtStartup,
    Running,
    WinMain,
    RegisterClass,
    CreateWindow,
    MessageLoop,
    DirectDrawCreate,
    SetCooperativeLevel,
    SetDisplayMode,
    CreateSurface,
    PaletteChanged,
    MenuBkgCloseReturn,
    SurfaceOps,
    FirstPresent,
    FirstFrame,
    Stopped,
    Error,
};

const char* phase_name(Phase p);

// Records the transition and logs it under "[PHASE]" — always call this
// instead of writing to a plain variable, so every phase change is visible
// in the log even if nothing else about that phase produced output.
void set_phase(Phase p, const char* detail = nullptr);

Phase current_phase();

} // namespace ign
