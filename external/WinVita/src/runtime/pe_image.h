// src/runtime/pe_image.h — PE32 image loader for the x86 runtime.
//
// This is NOT the pe_analyze CLI (that only dumps metadata). This loader
// actually *maps* a Diablo II DLL into a flat, relocated image buffer that
// the x86 CPU backend (Unicorn on desktop, ARMv7 dynarec on Vita) executes
// directly, and it exposes the import/export tables the bridge needs to
// wire x86↔ARM calls.
//
// Design constraints:
//   * No hardcoded addresses — every target is resolved via the PE's own
//     export directory (name + ordinal), so a different game build works
//     without changes here.
//   * Position-independent: the load base is chosen per image and .reloc is
//     applied, so several DLLs coexist in one emulated address space without
//     collisions.
//   * Zero dependency on host Win32 — pure byte parsing, builds for Vita.
//
// The loader does not resolve imports itself; it hands the caller a list of
// unresolved import slots (IAT address + dll + ordinal/name). The bridge
// fills each slot with a trampoline address (native shim) or the export of
// another loaded module (real x86 → real x86 direct call).

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace d2rt {

struct ImportRef {
    std::string dll;        // e.g. "Fog.dll" (as written in the PE, case kept)
    std::string name;       // symbol name, empty if imported by ordinal
    uint32_t    ordinal;    // ordinal (valid when name empty), else 0
    uint32_t    iat_va;     // absolute VA of the IAT slot to patch (image_base-relative resolved)
    uint32_t    iat_rva;    // same, as RVA into this image
};

struct ExportRef {
    std::string name;       // empty for ordinal-only exports
    uint32_t    ordinal;    // real ordinal (already includes ordinal_base)
    uint32_t    rva;        // RVA of the target within this image
    uint32_t    va;         // absolute VA (load_base + rva)
    std::string forward;    // "OtherDll.Symbol" if this is a forwarder, else empty
};

class PeImage {
public:
    // Parse `bytes` (a full DLL/EXE file) and lay it out at `load_base`.
    // Applies base relocations. Returns false on malformed input; `err`
    // gets a human-readable reason.
    bool load(const std::vector<uint8_t>& bytes, uint32_t load_base, std::string& err);

    // The mapped, relocated image. Size == image_size (section_align'd).
    // Index 0 corresponds to VA == load_base.
    std::vector<uint8_t>&       image()       { return image_; }
    const std::vector<uint8_t>& image() const { return image_; }

    uint32_t load_base()  const { return load_base_; }
    uint32_t image_size() const { return image_size_; }
    uint32_t entry_va()   const { return load_base_ + entry_rva_; }
    uint32_t entry_rva()  const { return entry_rva_; }
    uint16_t subsystem()  const { return subsystem_; }
    const std::string& module_name() const { return module_name_; }

    const std::vector<ImportRef>& imports() const { return imports_; }
    const std::vector<ExportRef>& exports() const { return exports_; }

    // Look up an export by name or by ordinal. Returns VA, or 0 if absent.
    uint32_t export_va(const std::string& name) const;
    uint32_t export_va_ordinal(uint32_t ordinal) const;

    // Patch an IAT slot (used by the bridge). va must be a slot from imports().
    void patch_iat(uint32_t iat_va, uint32_t value);

    // Raw image read/write helpers (bounds-checked, VA-based).
    bool read_u32(uint32_t va, uint32_t& out) const;
    bool write_u32(uint32_t va, uint32_t val);
    const uint8_t* at_rva(uint32_t rva, uint32_t need = 0) const;

private:
    std::vector<uint8_t> image_;
    uint32_t load_base_   = 0;
    uint32_t orig_base_   = 0;
    uint32_t image_size_  = 0;
    uint32_t entry_rva_   = 0;
    uint16_t subsystem_   = 0;
    std::string module_name_;
    std::vector<ImportRef> imports_;
    std::vector<ExportRef> exports_;

    const char* cstr_at_rva(uint32_t rva) const;
    bool parse_imports(const uint8_t* file, size_t flen,
                       const struct SectionHeaderRT* secs, int nsec,
                       uint32_t imp_rva, uint32_t imp_size, std::string& err);
    bool parse_exports(uint32_t exp_rva, uint32_t exp_size, std::string& err);
    void apply_relocs(uint32_t reloc_rva, uint32_t reloc_size, int32_t delta);
};

} // namespace d2rt
