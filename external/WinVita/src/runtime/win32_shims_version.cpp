// src/runtime/win32_shims_version.cpp — see win32_shims_version.h. The
// VS_FIXEDFILEINFO scan/repack (build_verinfo) is a plain PE-resource
// parser with zero guest-specific content; the only guest-specific piece is
// turning a guest file NAME into real bytes, which stays entirely on the
// consumer's side via Bridge::set_version_resource_source (a single
// registration, no override, same pattern as Cpu::set_alternate /
// Bridge::register_shim). GetModuleFileNameEx*/GetModuleFileName* stay on
// the consumer side: they resolve a full MODULE PATH string, which is where
// a guest's install-path fiction actually lives — this file never sees or
// needs that.
#include "win32_shims_version.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using namespace d2rt;

namespace {

std::vector<uint8_t> build_verinfo(const std::vector<uint8_t>& f) {
    std::vector<uint8_t> out;
    size_t sig = std::string::npos;                     // locate VS_FIXEDFILEINFO (0xFEEF04BD, LE)
    for (size_t i = 0; i + 52 <= f.size(); i++)
        if (f[i]==0xBD && f[i+1]==0x04 && f[i+2]==0xEF && f[i+3]==0xFE) { sig = i; break; }
    if (sig == std::string::npos) return out;
    static const char* KEY = "VS_VERSION_INFO";         // [hdr 6][wkey 32][FIXEDFILEINFO 52] = 92 B
    out.resize(92, 0);
    uint16_t vlen = 92, vallen = 52, type = 0;
    std::memcpy(&out[0], &vlen, 2); std::memcpy(&out[2], &vallen, 2); std::memcpy(&out[4], &type, 2);
    for (int i = 0; KEY[i]; i++) out[6 + i * 2] = (uint8_t)KEY[i];   // wide key, FIXEDFILEINFO at +40
    std::memcpy(&out[40], &f[sig], 52);
    return out;
}

// Narrow, null-terminated read of a wide guest string (ASCII subset only —
// matches the file names these APIs are ever called with).
std::string gread_narrow_wide(Cpu& c, uint32_t p) {
    std::string s;
    for (uint32_t off = 0;; off += 2) {
        uint16_t ch = 0; c.read(p + off, &ch, 2);
        if (!ch) break;
        if (ch < 128) s += (char)ch;
    }
    return s;
}

} // namespace

void win32_shims_version_install(Bridge& br) {
    Shim s; s.stdcall_cleanup = true;

    s.argc = 2; s.tag = "VERSION.dll!GetFileVersionInfoSizeW";
    s.fn = [&br](Cpu& c) -> uint32_t {
        std::string n = gread_narrow_wide(c, c.arg(0));
        auto v = build_verinfo(br.version_resource_bytes(n));
        if (c.arg(1)) c.write_u32(c.arg(1), 0);
        return (uint32_t)v.size();
    };
    br.register_shim("VERSION.dll", "GetFileVersionInfoSizeW", s);

    s.argc = 4; s.tag = "VERSION.dll!GetFileVersionInfoW";
    s.fn = [&br](Cpu& c) -> uint32_t {
        std::string n = gread_narrow_wide(c, c.arg(0));
        uint32_t buf = c.arg(3), len = c.arg(2);
        auto v = build_verinfo(br.version_resource_bytes(n));
        if (v.empty() || !buf) return 0u;
        uint32_t w = (uint32_t)v.size(); if (w > len) w = len;
        c.write(buf, v.data(), w);
        return 1u;
    };
    br.register_shim("VERSION.dll", "GetFileVersionInfoW", s);

    s.argc = 4; s.tag = "VERSION.dll!VerQueryValueW";
    s.fn = [](Cpu& c) -> uint32_t {
        uint32_t block = c.arg(0), ppBuf = c.arg(2), puLen = c.arg(3);
        std::string sub = gread_narrow_wide(c, c.arg(1));
        if (sub == "\\" || sub.empty()) {
            if (ppBuf) c.write_u32(ppBuf, block + 40);
            if (puLen) c.write_u32(puLen, 52);
            return 1u;
        }
        return 0u;
    };
    br.register_shim("VERSION.dll", "VerQueryValueW", s);

    s.argc = 4; s.tag = "VERSION.dll!VerQueryValueA";
    s.fn = [](Cpu& c) -> uint32_t {
        uint32_t block = c.arg(0), ppBuf = c.arg(2), puLen = c.arg(3);
        for (uint32_t off = 0; off < 0x400; off += 4) {                 // "\\" -> VS_FIXEDFILEINFO
            if (c.read_u32(block + off) == 0xFEEF04BDu) {
                if (ppBuf) c.write_u32(ppBuf, block + off);
                if (puLen) c.write_u32(puLen, 52);
                return 1u;
            }
        }
        return 0u;
    };
    br.register_shim("VERSION.dll", "VerQueryValueA", s);
}
