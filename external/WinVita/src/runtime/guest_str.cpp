#include "guest_str.h"
#include "runtime/cpu.h"
#include <cstdlib>
#include <cstring>

using namespace d2rt;

std::string wx86_gread_mb(Cpu& c, uint32_t p, int len) {
    std::string s;
    if (len < 0) {
        // Fast path: the byte-by-byte loop below pays a virtual c.read() call
        // PER CHARACTER (this is how wsprintf reads its format string), while
        // strnlen on the host view walks the same bytes without those calls.
        //
        // CAUTION: hostptr() only validates ONE byte. The slow loop reads via
        // bounded c.read(), which returns zero outside the arena and thus
        // stops at the boundary; a raw strnlen could run off into host
        // memory. The scan below is bounded to the actually-mapped span, and
        // disabled under the memory guard so its unmapped-read detection
        // keeps working.
        static const bool mg = std::getenv("WX86_MEMGUARD") || std::getenv("D2_MEMGUARD");
        if (!mg) {
            uint32_t span = 0x10000; const char* hp = nullptr;
            while (span && !(hp = (const char*)c.hostptr(p, span))) span >>= 1;
            if (hp) {
                size_t n = strnlen(hp, span);
                if (n < span) { s.assign(hp, n); return s; }   // null found within the safe span
            }
        }
        for (;;) { uint8_t b = 0; c.read(p++, &b, 1); if (!b) break; s.push_back((char)b); }
    } else {
        s.resize(len);
        if (len) c.read(p, &s[0], len);
    }
    return s;
}

std::vector<uint16_t> wx86_gread_wc(Cpu& c, uint32_t p, int len) {
    std::vector<uint16_t> s;
    if (len < 0) {
        for (;;) { uint16_t w = 0; c.read(p, &w, 2); p += 2; if (!w) break; s.push_back(w); }
    } else {
        s.resize(len);
        for (int i = 0; i < len; i++) { uint16_t w = 0; c.read(p + i * 2, &w, 2); s[i] = w; }
    }
    return s;
}

void wx86_gwrite_wc(Cpu& c, uint32_t p, uint16_t w) { c.write(p, &w, 2); }
