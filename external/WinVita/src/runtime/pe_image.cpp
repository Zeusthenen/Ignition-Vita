// src/runtime/pe_image.cpp — see pe_image.h.
#include "runtime/pe_image.h"

#include <cstring>

namespace d2rt {

// -------- Packed PE layouts (little-endian). Local copies so this TU is
//          self-contained and buildable for Vita without the CLI tool. -------
#pragma pack(push, 1)
struct DosHeaderRT { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; };
struct CoffHeaderRT {
    uint16_t machine; uint16_t num_sections; uint32_t timestamp;
    uint32_t sym_table; uint32_t num_syms; uint16_t opt_size; uint16_t characteristics;
};
struct DataDirRT { uint32_t rva; uint32_t size; };
struct OptHeader32RT {
    uint16_t magic; uint8_t major_link, minor_link;
    uint32_t code_size, init_data_size, uninit_data_size, entry_point;
    uint32_t base_of_code, base_of_data, image_base, section_align, file_align;
    uint16_t major_os, minor_os, major_img, minor_img, major_sub, minor_sub;
    uint32_t win32_version, image_size, headers_size, checksum;
    uint16_t subsystem, dll_characteristics;
    uint32_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t loader_flags, num_rva; DataDirRT dirs[16];
};
struct SectionHeaderRT {
    char name[8]; uint32_t virt_size, virt_addr, raw_size, raw_ptr;
    uint32_t reloc_ptr, linenum_ptr; uint16_t num_relocs, num_linenums;
    uint32_t characteristics;
};
struct ImportDescriptorRT {
    uint32_t lookup_rva, timestamp, forwarder, name_rva, iat_rva;
};
struct ExportDirRT {
    uint32_t characteristics, timestamp; uint16_t major, minor;
    uint32_t name_rva, ordinal_base, num_functions, num_names;
    uint32_t functions_rva, names_rva, ordinals_rva;
};
#pragma pack(pop)

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_BASERELOC = 5 };

// -------- small helpers ------------------------------------------------------
const uint8_t* PeImage::at_rva(uint32_t rva, uint32_t need) const {
    if (rva >= image_size_) return nullptr;
    if (need && (uint64_t)rva + need > image_size_) return nullptr;
    return image_.data() + rva;
}
const char* PeImage::cstr_at_rva(uint32_t rva) const {
    if (rva >= image_size_) return nullptr;
    const char* p = reinterpret_cast<const char*>(image_.data() + rva);
    // ensure NUL within image
    for (uint32_t i = rva; i < image_size_; ++i)
        if (image_[i] == 0) return p;
    return nullptr;
}
bool PeImage::read_u32(uint32_t va, uint32_t& out) const {
    if (va < load_base_) return false;
    uint32_t rva = va - load_base_;
    const uint8_t* p = at_rva(rva, 4);
    if (!p) return false;
    std::memcpy(&out, p, 4);
    return true;
}
bool PeImage::write_u32(uint32_t va, uint32_t val) {
    if (va < load_base_) return false;
    uint32_t rva = va - load_base_;
    if ((uint64_t)rva + 4 > image_size_) return false;
    std::memcpy(image_.data() + rva, &val, 4);
    return true;
}
void PeImage::patch_iat(uint32_t iat_va, uint32_t value) { write_u32(iat_va, value); }

// -------- load ---------------------------------------------------------------
bool PeImage::load(const std::vector<uint8_t>& bytes, uint32_t load_base, std::string& err) {
    const uint8_t* f = bytes.data();
    size_t flen = bytes.size();
    if (flen < sizeof(DosHeaderRT)) { err = "file too small"; return false; }
    auto* dos = reinterpret_cast<const DosHeaderRT*>(f);
    if (dos->e_magic != 0x5A4D) { err = "no MZ"; return false; }
    uint32_t peoff = dos->e_lfanew;
    if (peoff + 4 + sizeof(CoffHeaderRT) > flen) { err = "bad e_lfanew"; return false; }
    if (std::memcmp(f + peoff, "PE\0\0", 4) != 0) { err = "no PE sig"; return false; }
    auto* coff = reinterpret_cast<const CoffHeaderRT*>(f + peoff + 4);
    if (coff->machine != 0x014c) { err = "not i386"; return false; }
    uint32_t optoff = peoff + 4 + sizeof(CoffHeaderRT);
    auto* opt = reinterpret_cast<const OptHeader32RT*>(f + optoff);
    if (opt->magic != 0x10b) { err = "not PE32"; return false; }

    orig_base_  = opt->image_base;
    load_base_  = load_base ? load_base : opt->image_base;
    image_size_ = opt->image_size;
    entry_rva_  = opt->entry_point;
    subsystem_  = opt->subsystem;

    if (image_size_ < opt->headers_size || image_size_ > (256u << 20)) {
        err = "implausible image_size"; return false;
    }
    image_.assign(image_size_, 0);

    // Copy headers.
    std::memcpy(image_.data(), f, std::min<uint32_t>(opt->headers_size, flen));

    // Map sections.
    auto* secs = reinterpret_cast<const SectionHeaderRT*>(f + optoff + coff->opt_size);
    int nsec = coff->num_sections;
    for (int i = 0; i < nsec; ++i) {
        const auto& s = secs[i];
        if (s.virt_addr >= image_size_) continue;
        uint32_t copy = s.raw_size;
        if ((uint64_t)s.raw_ptr + copy > flen) copy = (s.raw_ptr < flen) ? (flen - s.raw_ptr) : 0;
        uint32_t space = image_size_ - s.virt_addr;
        if (copy > space) copy = space;
        if (copy) std::memcpy(image_.data() + s.virt_addr, f + s.raw_ptr, copy);
    }

    // Base relocations (delta from ORIGINAL preferred base).
    int32_t delta = (int32_t)(load_base_ - orig_base_);
    if (delta != 0) {
        const auto& rd = opt->dirs[DIR_BASERELOC];
        if (rd.rva && rd.size) apply_relocs(rd.rva, rd.size, delta);
    }

    // Parse imports/exports.
    const auto& id = opt->dirs[DIR_IMPORT];
    if (id.rva && id.size) {
        if (!parse_imports(f, flen, secs, nsec, id.rva, id.size, err)) return false;
    }
    const auto& ed = opt->dirs[DIR_EXPORT];
    if (ed.rva && ed.size) {
        if (!parse_exports(ed.rva, ed.size, err)) return false;
    }
    return true;
}

void PeImage::apply_relocs(uint32_t reloc_rva, uint32_t reloc_size, int32_t delta) {
    uint32_t off = reloc_rva, end = reloc_rva + reloc_size;
    while (off + 8 <= end && off + 8 <= image_size_) {
        uint32_t page_rva, block_size;
        std::memcpy(&page_rva, image_.data() + off, 4);
        std::memcpy(&block_size, image_.data() + off + 4, 4);
        if (block_size < 8 || off + block_size > end) break;
        uint32_t n = (block_size - 8) / 2;
        const uint8_t* entries = image_.data() + off + 8;
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t e; std::memcpy(&e, entries + i * 2, 2);
            uint32_t type = e >> 12, roff = e & 0xFFF;
            if (type == 3 /*HIGHLOW*/) {
                uint32_t tgt = page_rva + roff;
                if ((uint64_t)tgt + 4 <= image_size_) {
                    uint32_t v; std::memcpy(&v, image_.data() + tgt, 4);
                    v += delta;
                    std::memcpy(image_.data() + tgt, &v, 4);
                }
            }
            // type 0 = ABSOLUTE (padding), ignore.
        }
        off += block_size;
    }
}

bool PeImage::parse_imports(const uint8_t* /*file*/, size_t /*flen*/,
                            const SectionHeaderRT* /*secs*/, int /*nsec*/,
                            uint32_t imp_rva, uint32_t imp_size, std::string& err) {
    (void)imp_size;
    uint32_t off = imp_rva;
    for (;;) {
        const uint8_t* p = at_rva(off, sizeof(ImportDescriptorRT));
        if (!p) break;
        ImportDescriptorRT d; std::memcpy(&d, p, sizeof(d));
        if (d.name_rva == 0 && d.lookup_rva == 0 && d.iat_rva == 0) break;
        const char* dllname = cstr_at_rva(d.name_rva);
        std::string dll = dllname ? dllname : "?";
        // Prefer the lookup (INT) table for names; fall back to IAT.
        uint32_t lut = d.lookup_rva ? d.lookup_rva : d.iat_rva;
        uint32_t iat = d.iat_rva;
        for (uint32_t k = 0;; ++k) {
            const uint8_t* lp = at_rva(lut + k * 4, 4);
            const uint8_t* ip = at_rva(iat + k * 4, 4);
            if (!lp || !ip) break;
            uint32_t thunk; std::memcpy(&thunk, lp, 4);
            if (thunk == 0) break;
            ImportRef ref;
            ref.dll = dll;
            ref.iat_rva = iat + k * 4;
            ref.iat_va  = load_base_ + ref.iat_rva;
            if (thunk & 0x80000000u) {
                ref.ordinal = thunk & 0xFFFF;
            } else {
                // hint/name table: u16 hint then name
                const char* nm = cstr_at_rva((thunk & 0x7FFFFFFF) + 2);
                ref.name = nm ? nm : "";
                ref.ordinal = 0;
            }
            imports_.push_back(std::move(ref));
        }
        off += sizeof(ImportDescriptorRT);
    }
    (void)err;
    return true;
}

bool PeImage::parse_exports(uint32_t exp_rva, uint32_t exp_size, std::string& err) {
    const uint8_t* p = at_rva(exp_rva, sizeof(ExportDirRT));
    if (!p) { err = "bad export dir"; return false; }
    ExportDirRT ed; std::memcpy(&ed, p, sizeof(ed));
    const char* modname = cstr_at_rva(ed.name_rva);
    module_name_ = modname ? modname : "";

    uint32_t exp_end = exp_rva + exp_size;
    // Functions array (EAT): num_functions * u32 RVA.
    for (uint32_t i = 0; i < ed.num_functions; ++i) {
        const uint8_t* fp = at_rva(ed.functions_rva + i * 4, 4);
        if (!fp) break;
        uint32_t frva; std::memcpy(&frva, fp, 4);
        if (frva == 0) continue;
        ExportRef ex;
        ex.ordinal = ed.ordinal_base + i;
        ex.rva = frva;
        ex.va  = load_base_ + frva;
        // Forwarder if the RVA points inside the export directory.
        if (frva >= exp_rva && frva < exp_end) {
            const char* fw = cstr_at_rva(frva);
            ex.forward = fw ? fw : "";
            ex.va = 0;
        }
        exports_.push_back(std::move(ex));
    }
    // Names → attach to the right ordinal via the ordinals table.
    for (uint32_t i = 0; i < ed.num_names; ++i) {
        const uint8_t* np = at_rva(ed.names_rva + i * 4, 4);
        const uint8_t* op = at_rva(ed.ordinals_rva + i * 2, 2);
        if (!np || !op) break;
        uint32_t nrva; std::memcpy(&nrva, np, 4);
        uint16_t oidx;  std::memcpy(&oidx, op, 2);
        const char* nm = cstr_at_rva(nrva);
        if (!nm) continue;
        // oidx is index into the functions array (0-based).
        for (auto& ex : exports_)
            if (ex.ordinal == ed.ordinal_base + oidx) { ex.name = nm; break; }
    }
    (void)err;
    return true;
}

uint32_t PeImage::export_va(const std::string& name) const {
    for (const auto& e : exports_) if (e.name == name) return e.va;
    return 0;
}
uint32_t PeImage::export_va_ordinal(uint32_t ordinal) const {
    for (const auto& e : exports_) if (e.ordinal == ordinal) return e.va;
    return 0;
}

} // namespace d2rt
