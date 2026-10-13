// src/runtime/guest_str.h — read/write strings in GUEST memory.
//
// Any Win32 API that takes or returns a string must read or write it at a
// guest x86 address, not a host one. These helpers are engine primitives,
// like the scratch allocator (guest_scratch.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { class Cpu; }

// Guest multi-byte string. len<0 = until null terminator, else len bytes.
std::string wx86_gread_mb(d2rt::Cpu& c, uint32_t p, int len);

// Guest wide (UTF-16) string. len<0 = until null terminator, else len units.
std::vector<uint16_t> wx86_gread_wc(d2rt::Cpu& c, uint32_t p, int len);

// Writes one UTF-16 unit at a guest address.
void wx86_gwrite_wc(d2rt::Cpu& c, uint32_t p, uint16_t w);
