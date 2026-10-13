// win32/g150_ctl.h -- iteration-G150, PURE parts (no GL, no Vita API, no guest access, no logging, no heap, no global state; proven by tools/g150_test.cpp).
//  1. the control tokens of ux0:data/ignition/g146_geo.txt that G150 adds (exact whitespace-delimited tokens; anything else is ignored here);
//  2. the RESUME LOOP of the one guest run: WinVita's block-entry budget (2^31-1 blocks per Cpu::run) ends the run with "finished" when it is exhausted, and Bridge::call_va returns ok -- the game would leave by itself after ~65 minutes
//     of racing (the G149 run executed 548,605,530 blocks in 1,002 s). The loop below re-enters the run at the EIP where the budget stopped it (exactly what the engine's own schedulers do) and ends only when the guest really returned.
//  3. the figures of the [G150-*] lines.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g150 {

// ---- 1. control tokens ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum : uint32_t { kGrTex = 1u << 0, kGrLogBatch = 1u << 1, kGrAll = kGrTex | kGrLogBatch };
struct Control {
    uint32_t groups = kGrAll;        // g150=off clears all; g150-<tex|logbatch>=off clears one
    int memguard = -1;               // -1 = the build's default (LIGHT off, FULL on), 0 = memguard=off, 1 = memguard=on
    uint32_t budgetBlocks = 0;       // budget=<blocks>: blocks per Cpu::run (0 = the engine's 2^31-1); a small value makes the resume loop run every few seconds (the Vita-side test of the exhaustion)
    uint32_t hooks = 0;              // guest hot-loop replacements (win32/g150_hud.h): bit set by the tokens hook-hud-draw=on, hook-hud-sprite=on (all default OFF)
    char refused[96] = "";
};
inline bool tok_eq(const char* p, size_t n, const char* lit) { return std::strlen(lit) == n && std::memcmp(p, lit, n) == 0; }
enum : uint32_t { kHookDrawSurface = 1u << 0, kHookSprite = 1u << 1 };
inline Control parse_control(const char* text) {
    Control c;
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        const size_t n = (size_t)(e - p);
        if (n) {
            if (tok_eq(p, n, "g150=off")) c.groups = 0u;
            else if (tok_eq(p, n, "g150-tex=off")) c.groups &= ~kGrTex;
            else if (tok_eq(p, n, "g150-logbatch=off")) c.groups &= ~kGrLogBatch;
            else if (tok_eq(p, n, "memguard=on")) c.memguard = 1;
            else if (tok_eq(p, n, "memguard=off")) c.memguard = 0;
            else if (tok_eq(p, n, "hook-hud-draw=on")) c.hooks |= kHookDrawSurface;
            else if (tok_eq(p, n, "hook-hud-sprite=on")) c.hooks |= kHookSprite;
            else if (n > 7 && std::memcmp(p, "budget=", 7) == 0) {
                uint64_t v = 0; bool ok = n > 7 && n <= 7 + 10;
                for (size_t i = 7; i < n && ok; ++i) { if (p[i] < '0' || p[i] > '9') ok = false; else v = v * 10u + (uint64_t)(p[i] - '0'); }
                if (ok && v >= 100000u && v <= 2147483647u) c.budgetBlocks = (uint32_t)v;
                else { size_t u = std::strlen(c.refused); if (u + n + 2 < sizeof c.refused) { std::snprintf(c.refused + u, sizeof c.refused - u, "%s%.*s", u ? " " : "", (int)n, p); } }
            }
        }
        p = e;
    }
    return c;
}
// the budget the engine enforces per Cpu::run when the token is present: Cpu::set_run_limit(n) turns n guest instructions into n/8 block entries, so the call asks for 8 x blocks
inline uint64_t run_limit_for_blocks(uint32_t blocks) { return (uint64_t)blocks * 8u; }

// ---- 2. the resume loop -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct ResumeStats { uint32_t resumes = 0; bool capped = false; };
// `cpu` needs: bool take_limit_hit() (true when the last run ended by the block budget -- or by a stop request, which `halted()` tells apart), bool run(uint32_t eip, const char** fault), uint32_t reg(int).
// A run that ends by the budget is resumable "exactly at the EIP of the block that was about to run, nothing of it executed" (cpu_box86.cpp), so the loop only re-enters there. Ends when: the guest returned (no budget flag), a fault
// (run() false), a halt was requested (g_halted: ExitProcess, request_halt) or `maxResumes` is spent (never an endless loop: 2^31 blocks per resume means 1,000,000 resumes are 2e15 blocks).
template <class Cpu_, class Halted, class OnResume>
inline bool run_with_resume(Cpu_& cpu, bool firstRunOk, int eipReg, Halted halted, OnResume onResume, ResumeStats& st, const char** fault, uint32_t maxResumes = 1000000u) {
    bool ok = firstRunOk;
    while (ok && !halted() && cpu.take_limit_hit()) {
        if (st.resumes >= maxResumes) { st.capped = true; break; }
        ++st.resumes; onResume(st.resumes);
        ok = cpu.run(cpu.reg(eipReg), fault);
    }
    return ok;
}

// ---- 3. lines ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
inline int format_budget(char* out, size_t cap, uint32_t perRunBlocks, uint32_t resumes, bool capped) {
    if (!out || !cap) return 0;
    const int n = std::snprintf(out, cap, "budget{blocks_per_run=%u%s resumes=%u%s}", perRunBlocks ? perRunBlocks : 2147483647u, perRunBlocks ? " (control file)" : " (engine default 2^31-1)", resumes, capped ? " RESUME-CAP-REACHED" : "");
    if (n < 0) { out[0] = 0; return 0; }
    if ((size_t)n >= cap) { out[cap - 1] = 0; return (int)(cap - 1); }
    return n;
}

} }   // namespace ign::g150
