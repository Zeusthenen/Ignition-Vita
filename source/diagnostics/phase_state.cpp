#include "diagnostics/phase_state.h"
#include "diagnostics/log.h"

namespace ign {

namespace {
Phase g_phase = Phase::Boot;
}

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Boot:               return "BOOT";
        case Phase::WinVitaInit:        return "WINVITA_INIT";
        case Phase::PeOpen:             return "PE_OPEN";
        case Phase::PeParse:            return "PE_PARSE";
        case Phase::PeMap:              return "PE_MAP";
        case Phase::Relocation:         return "RELOCATION";
        case Phase::Imports:            return "IMPORTS";
        case Phase::CpuSetup:           return "CPU_SETUP";
        case Phase::Entrypoint:         return "ENTRYPOINT";
        case Phase::CrtStartup:         return "CRT_STARTUP";
        case Phase::Running:            return "RUNNING";
        case Phase::WinMain:            return "WINMAIN";
        case Phase::RegisterClass:      return "REGISTER_CLASS";
        case Phase::CreateWindow:       return "CREATE_WINDOW";
        case Phase::MessageLoop:        return "MESSAGE_LOOP";
        case Phase::DirectDrawCreate:   return "DIRECTDRAW_CREATE";
        case Phase::SetCooperativeLevel:return "SET_COOPERATIVE_LEVEL";
        case Phase::SetDisplayMode:     return "SET_DISPLAY_MODE";
        case Phase::CreateSurface:      return "CREATE_SURFACE";
        case Phase::PaletteChanged:     return "PALETTE_CHANGED";
        case Phase::MenuBkgCloseReturn: return "MENUBKG_CLOSE_RETURN";
        case Phase::SurfaceOps:         return "SURFACE_OPS";
        case Phase::FirstPresent:       return "FIRST_PRESENT";
        case Phase::FirstFrame:         return "FIRST_FRAME";
        case Phase::Stopped:            return "STOPPED";
        case Phase::Error:              return "ERROR";
    }
    return "?";
}

void set_phase(Phase p, const char* detail) {
    g_phase = p;
    if (detail && *detail) IGN_LOG("[PHASE]", "-> %s (%s)", phase_name(p), detail);
    else                   IGN_LOG("[PHASE]", "-> %s", phase_name(p));
}

Phase current_phase() { return g_phase; }

} // namespace ign
