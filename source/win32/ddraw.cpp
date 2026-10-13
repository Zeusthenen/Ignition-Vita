#include "win32/ddraw.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/ignition_guest_alloc.h"
#include "runtime/ignition_boot.h"
#include "diagnostics/log.h"
#include "diagnostics/phase_state.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <functional>

using namespace d2rt;

namespace ign {

namespace {

// ---- COM identity -----------------------------------------------------
// Standard Microsoft GUID binary encoding: Data1/Data2/Data3 little-endian,
// Data4 stored as its 8 hex-pair bytes verbatim. Not guessed -- this is the
// universal COM/Win32 GUID memory layout.
constexpr uint8_t kIID_IUnknown[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00, 0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };
// IID_IDirectDraw = 6C14DB80-A733-11CE-A521-0020AF0BE560
constexpr uint8_t kIID_IDirectDraw[16] = {
    0x80,0xDB,0x14,0x6C, 0x33,0xA7, 0xCE,0x11, 0xA5,0x21, 0x00,0x20,0xAF,0x0B,0xE5,0x60 };

// DDSCAPS bits actually needed this iteration -- verified against Wine's
// ddraw.h (a faithful reimplementation of Microsoft's own DirectDraw
// headers), not recalled from memory:
//   DDSCAPS_COMPLEX        0x00000008
//   DDSCAPS_BACKBUFFER     0x00000004
//   DDSCAPS_FLIP           0x00000010
//   DDSCAPS_PRIMARYSURFACE 0x00000200
// PRIMARYSURFACE|FLIP|COMPLEX = 0x218, exactly the real hardware value
// Ignition's own CreateSurface call passed.
constexpr uint32_t kDDSCAPS_BACKBUFFER = 0x00000004u;
constexpr uint32_t kDDSCAPS_COMPLEX = 0x00000008u;
constexpr uint32_t kDDSCAPS_FLIP = 0x00000010u;
constexpr uint32_t kDDSCAPS_PRIMARYSURFACE = 0x00000200u;

// DDERR_NOTFOUND = MAKE_DDHRESULT(255) = MAKE_HRESULT(1, 0x876, 255).
// Verified against Wine's ddraw.h -- an earlier guess from memory (2150)
// would have been wrong; this is the real, fetched value.
constexpr uint32_t kDDERR_NOTFOUND = 0x887600FFu;

// DDERR_SURFACELOST = MAKE_DDHRESULT(450) = 0x887601C2. Confirmed TWICE:
// against Wine's ddraw.h, and independently against Ign_d3d.exe's own
// compiled code -- its IsLost caller does `CMP EAX, 0x887601C2` on the
// return value, the exact same constant.
constexpr uint32_t kDDERR_SURFACELOST = 0x887601C2u;

// DDPCAPS_* -- verified against Wine's ddraw.h. dwFlags=0x44 on Ignition's
// real CreatePalette call (confirmed by disassembly: `6A 44` immediately
// before the vtable call) is exactly DDPCAPS_ALLOW256|DDPCAPS_8BIT.
constexpr uint32_t kDDPCAPS_8BIT = 0x00000004u;
constexpr uint32_t kDDPCAPS_ALLOW256 = 0x00000040u;

struct DDrawObj {
    uint32_t vtable = 0;
    uint32_t refcount = 1;
    uint32_t hwnd = 0, coopFlags = 0;
    uint32_t dispW = 0, dispH = 0, dispBpp = 0;
};
struct DDrawPalette {
    uint32_t vtable = 0;
    uint32_t refcount = 1;
    uint32_t flags = 0;
    uint8_t entries[256 * 4] = {0};   // raw peRed,peGreen,peBlue,peFlags x256 (wingdi.h PALETTEENTRY, 4 bytes each)
    uint32_t setEntriesCalls = 0;
};
struct DDrawSurf {
    uint32_t vtable = 0;
    uint32_t refcount = 1;
    uint32_t width = 0, height = 0, bpp = 0, pitch = 0;
    uint32_t caps = 0;
    uint32_t pixels = 0;        // guest address of the real pixel buffer
    uint32_t pixelsSize = 0;
    uint32_t attachedPalette = 0;
    uint32_t attached = 0;      // guest address of the linked surface (primary <-> backbuffer), 0 if none
    bool lost = false;          // never set true artificially -- see IsLost's own comment
    uint32_t isLostCalls = 0;
};

std::map<uint32_t, DDrawObj>& g_objects() { static std::map<uint32_t, DDrawObj> m; return m; }
std::map<uint32_t, DDrawSurf>& g_surfaces() { static std::map<uint32_t, DDrawSurf> m; return m; }
std::map<uint32_t, DDrawPalette>& g_palettes() { static std::map<uint32_t, DDrawPalette> m; return m; }

// Cheap FNV-1a over the raw entry bytes -- enough to notice a change
// (SetEntries, a future Lock/Unlock write) without printing 256 lines.
uint32_t fnv1a(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 16777619u; }
    return h;
}

// IDirectDrawPalette vtable order -- verified against Wine's ddraw.h
// (QueryInterface, AddRef, Release, GetCaps, GetEntries, Initialize,
// SetEntries), not assumed.
const std::vector<const char*> kPaletteMethods = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries",
};

// IDirectDraw vtable order -- CONFIRMED against Ign_d3d.exe's own
// disassembly (see ddraw.h). Offsets in comments are byte offsets (slot*4).
const std::vector<const char*> kDDrawMethods = {
    "QueryInterface",        // 0x00
    "AddRef",                // 0x04
    "Release",               // 0x08
    "Compact",               // 0x0C
    "CreateClipper",         // 0x10
    "CreatePalette",         // 0x14 -- confirmed: real CALL [ebx+0x14]
    "CreateSurface",         // 0x18 -- confirmed: real CALL [eax+0x18]
    "DuplicateSurface",      // 0x1C
    "EnumDisplayModes",      // 0x20
    "EnumSurfaces",          // 0x24
    "FlipToGDISurface",      // 0x28
    "GetCaps",               // 0x2C
    "GetDisplayMode",        // 0x30
    "GetFourCCCodes",        // 0x34
    "GetGDISurface",         // 0x38
    "GetMonitorFrequency",   // 0x3C
    "GetScanLine",           // 0x40
    "GetVerticalBlankStatus",// 0x44
    "Initialize",            // 0x48
    "RestoreDisplayMode",    // 0x4C
    "SetCooperativeLevel",   // 0x50 -- confirmed: real CALL [eax+0x50]
    "SetDisplayMode",        // 0x54 -- confirmed: real CALL [ebx+0x54]
    "WaitForVerticalBlank",  // 0x58
};

// IDirectDrawSurface vtable order -- standard DirectX 2/3-era layout.
// Confirmed by real disassembly so far: GetAttachedSurface (index 12,
// 0x30), SetPalette (index 31, 0x7C), IsLost (index 24, 0x60 -- `CALL
// [EBX+0x60]`, return value compared against DDERR_SURFACELOST), and
// Restore (index 27, 0x6C -- seen as the target of the "surface really is
// lost" branch, not executed on our own DD_OK path but its call site is
// real). Everything else in this list is still the standard reference
// order, not yet independently verified.
const std::vector<const char*> kSurfaceMethods = {
    "QueryInterface", "AddRef", "Release",
    "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast",
    "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
    "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper",
    "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette",
    "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition",
    "SetPalette",   // 0x7C -- confirmed: real CALL [eax+0x7C]
    "Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
};

std::function<uint32_t(Cpu&)> unimpl_trap(std::string fullName) {
    return [fullName](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        uint32_t esp = c.reg(R_ESP);
        uint32_t retAddr = c.read_u32(esp);
        char detail[256];
        snprintf(detail, sizeof detail, "%s (this=0x%08x, called from guest EIP=0x%08x ESP=0x%08x)",
                 fullName.c_str(), thisPtr, retAddr, esp);
        IGN_LOG("[DDRAW]", "UNIMPLEMENTED %s", detail);
        request_halt(detail);
        return 0u;
    };
}

// One entry: method name (must match a name in the interface's method
// list) -> {argc including `this`, implementation}.
using Override = std::pair<uint32_t, std::function<uint32_t(Cpu&)>>;

// Builds one vtable: `size` guest-callable trap slots, in EXACT order,
// each either the caller's override or a shared diagnostic trap. Returns
// the guest address of the vtable (an array of `size` guest code
// addresses), or 0 on allocation failure.
uint32_t make_vtable(Bridge& br, Cpu& cpu, const char* ifaceTag,
                     const std::vector<const char*>& methodNames,
                     const std::map<std::string, Override>& overrides) {
    uint32_t vtable = ignition_guest_alloc((uint32_t)methodNames.size() * 4, 4);
    if (!vtable) return 0;
    for (size_t i = 0; i < methodNames.size(); ++i) {
        std::string fullName = std::string(ifaceTag) + "::" + methodNames[i];
        Shim s;
        auto it = overrides.find(methodNames[i]);
        if (it != overrides.end()) {
            s.argc = it->second.first;
            s.stdcall_cleanup = true;
            s.fn = it->second.second;
        } else {
            // Unknown real argc for a method we haven't implemented -- don't
            // clean up any stack bytes, matching the generic default_shim's
            // own "unknown: don't corrupt stack" convention. Harmless either
            // way since request_halt() stops the CPU before anything reads
            // the (possibly wrong) stack state again.
            s.argc = 0;
            s.stdcall_cleanup = false;
            s.fn = unimpl_trap(fullName);
        }
        s.tag = fullName;
        br.register_shim("native.ddraw", fullName, s);
        uint32_t va = br.shim_trap("native.ddraw", fullName);
        cpu.write_u32(vtable + (uint32_t)i * 4, va);
    }
    IGN_LOG("[COM]", "%s vtable built: %u slot(s) at 0x%08x", ifaceTag, (unsigned)methodNames.size(), vtable);
    return vtable;
}

} // namespace

void ddraw_install(Bridge& br, Cpu& cpu) {
    // ---- IDirectDrawSurface: built once, shared by every surface
    // CreateSurface produces (real COM: one vtable per class, not per
    // instance). QueryInterface/AddRef/Release are genuinely implemented;
    // everything else (Lock, Blt, Flip, GetSurfaceDesc, ...) is a
    // diagnostic trap until the log tells us which one Ignition needs.
    std::map<std::string, Override> surfaceOverrides;
    surfaceOverrides["QueryInterface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), riid = c.arg(1), ppv = c.arg(2);
        uint8_t guid[16] = {0};
        if (riid) c.read(riid, guid, 16);
        bool known = std::memcmp(guid, kIID_IUnknown, 16) == 0;
        IGN_LOG("[DDRAW]", "IDirectDrawSurface::QueryInterface(this=0x%08x) -> %s",
                thisPtr, known ? "S_OK (same object)" : "E_NOINTERFACE");
        if (known) {
            auto it = g_surfaces().find(thisPtr);
            if (it != g_surfaces().end()) ++it->second.refcount;
            if (ppv) c.write_u32(ppv, thisPtr);
            return 0u;
        }
        if (ppv) c.write_u32(ppv, 0);
        return 0x80004002u;   // E_NOINTERFACE
    }};
    surfaceOverrides["AddRef"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_surfaces().find(thisPtr);
        uint32_t rc = it != g_surfaces().end() ? ++it->second.refcount : 1u;
        IGN_LOG("[DDRAW]", "IDirectDrawSurface::AddRef(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    surfaceOverrides["Release"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_surfaces().find(thisPtr);
        uint32_t rc = 0;
        if (it != g_surfaces().end() && it->second.refcount > 0) rc = --it->second.refcount;
        IGN_LOG("[DDRAW]", "IDirectDrawSurface::Release(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    // HRESULT GetAttachedSurface(LPDDSCAPS lpDDSCaps, LPDIRECTDRAWSURFACE* lplpDDAttachedSurface).
    // DDSCAPS is just {DWORD dwCaps} (verified, see kDDSCAPS_* comment above) --
    // read it, and only honor a request for DDSCAPS_BACKBUFFER: that's the
    // only attachment this project models (a primary <-> one backbuffer
    // flip chain, matching Ignition's own dwBackBufferCount=1 request).
    surfaceOverrides["GetAttachedSurface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), lpDDSCaps = c.arg(1), lplpAttached = c.arg(2);
        uint32_t requestedCaps = lpDDSCaps ? c.read_u32(lpDDSCaps) : 0u;
        IGN_LOG("[DDSURFACE]", "GetAttachedSurface(this=0x%08x, requested_caps=0x%08x)", thisPtr, requestedCaps);

        auto it = g_surfaces().find(thisPtr);
        bool wantsBackBuffer = (requestedCaps & kDDSCAPS_BACKBUFFER) != 0;
        if (it == g_surfaces().end() || !wantsBackBuffer || !it->second.attached) {
            IGN_LOG("[DDSURFACE]", "GetAttachedSurface -> DDERR_NOTFOUND (%s)",
                    !wantsBackBuffer ? "only DDSCAPS_BACKBUFFER is modeled" : "no attached surface tracked for this object");
            if (lplpAttached) c.write_u32(lplpAttached, 0);
            return kDDERR_NOTFOUND;
        }

        uint32_t attachedAddr = it->second.attached;
        auto ait = g_surfaces().find(attachedAddr);
        if (ait != g_surfaces().end()) ++ait->second.refcount;
        if (lplpAttached) c.write_u32(lplpAttached, attachedAddr);
        IGN_LOG("[DDSURFACE]", "GetAttachedSurface -> BACKBUFFER 0x%08x", attachedAddr);
        IGN_LOG("[DDSURFACE]", "GetAttachedSurface -> DD_OK");
        return 0u;
    }};
    // HRESULT SetPalette(LPDIRECTDRAWPALETTE lpDDPalette). Which surface
    // (PRIMARY/BACKBUFFER) is named explicitly in the log -- caps already
    // distinguishes them (kDDSCAPS_PRIMARYSURFACE vs kDDSCAPS_BACKBUFFER,
    // set at CreateSurface time). Reference counting matches real COM: the
    // newly-attached palette is AddRef'd, any previously-attached one is
    // Released.
    surfaceOverrides["SetPalette"] = { 2, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), paletteObj = c.arg(1);
        auto it = g_surfaces().find(thisPtr);
        const char* which = "UNKNOWN";
        if (it != g_surfaces().end()) {
            which = (it->second.caps & kDDSCAPS_PRIMARYSURFACE) ? "PRIMARY"
                  : (it->second.caps & kDDSCAPS_BACKBUFFER) ? "BACKBUFFER" : "UNKNOWN";
        }
        char tag[40];
        snprintf(tag, sizeof tag, "[DDSURFACE %s]", which);
        IGN_LOG(tag, "SetPalette(0x%08x)", paletteObj);

        if (it != g_surfaces().end()) {
            if (it->second.attachedPalette && it->second.attachedPalette != paletteObj) {
                auto oldIt = g_palettes().find(it->second.attachedPalette);
                if (oldIt != g_palettes().end() && oldIt->second.refcount > 0) --oldIt->second.refcount;
            }
            it->second.attachedPalette = paletteObj;
            auto palIt = g_palettes().find(paletteObj);
            if (palIt != g_palettes().end()) ++palIt->second.refcount;
        }
        IGN_LOG(tag, "SetPalette -> DD_OK");
        return 0u;
    }};
    // HRESULT IsLost(void) -- confirmed slot 0x60. Real disassembly shows
    // Ignition looping over a small list of tracked objects calling this
    // repeatedly (see section-22 concern in this iteration's brief), so
    // only the FIRST call per surface gets the full log; later ones are
    // just counted, with a periodic summary line so a long run doesn't go
    // completely silent. Always DD_OK: this project's surfaces are plain
    // guest-RAM buffers with no video-memory eviction to model, so there is
    // no real event that would ever make one genuinely lost -- returning
    // DDERR_SURFACELOST here would be inventing a failure, not reporting one.
    surfaceOverrides["IsLost"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_surfaces().find(thisPtr);
        const char* which = "UNKNOWN";
        bool valid = false;
        uint32_t calls = 0;
        if (it != g_surfaces().end()) {
            which = (it->second.caps & kDDSCAPS_PRIMARYSURFACE) ? "PRIMARY"
                  : (it->second.caps & kDDSCAPS_BACKBUFFER) ? "BACKBUFFER" : "UNKNOWN";
            valid = it->second.pixels != 0 && !it->second.lost;
            calls = ++it->second.isLostCalls;
        }
        char tag[40];
        snprintf(tag, sizeof tag, "[DDSURFACE %s]", which);
        if (calls <= 1) {
            IGN_LOG(tag, "IsLost");
            IGN_LOG(tag, "  object=0x%08x", thisPtr);
            IGN_LOG(tag, "  pixels=0x%08x", it != g_surfaces().end() ? it->second.pixels : 0);
            IGN_LOG(tag, "  valid=%s", valid ? "yes" : "no");
            IGN_LOG(tag, "IsLost -> DD_OK");
        } else if ((calls % 100) == 0) {
            IGN_LOG(tag, "IsLost calls = %u (still DD_OK)", calls);
        }
        return 0u;   // DD_OK
    }};
    // HRESULT Restore(void) -- confirmed slot 0x6C (the real target of
    // Ignition's "surface actually lost" branch, even though our own
    // IsLost never takes it). Since nothing here is backed by real video
    // memory, there's nothing to reallocate -- just clear the flag.
    surfaceOverrides["Restore"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_surfaces().find(thisPtr);
        const char* which = "UNKNOWN";
        if (it != g_surfaces().end()) {
            which = (it->second.caps & kDDSCAPS_PRIMARYSURFACE) ? "PRIMARY"
                  : (it->second.caps & kDDSCAPS_BACKBUFFER) ? "BACKBUFFER" : "UNKNOWN";
            it->second.lost = false;
        }
        char tag[40];
        snprintf(tag, sizeof tag, "[DDSURFACE %s]", which);
        IGN_LOG(tag, "Restore -> DD_OK");
        return 0u;
    }};
    uint32_t surfaceVtable = make_vtable(br, cpu, "IDirectDrawSurface", kSurfaceMethods, surfaceOverrides);

    // ---- IDirectDrawPalette: built once, shared by every palette
    // CreatePalette produces. QueryInterface/AddRef/Release genuinely
    // implemented; GetCaps/GetEntries/Initialize/SetEntries stay
    // diagnostic traps until the log shows Ignition actually calling one.
    std::map<std::string, Override> paletteOverrides;
    paletteOverrides["QueryInterface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), riid = c.arg(1), ppv = c.arg(2);
        uint8_t guid[16] = {0};
        if (riid) c.read(riid, guid, 16);
        bool known = std::memcmp(guid, kIID_IUnknown, 16) == 0;
        IGN_LOG("[PALETTE]", "QueryInterface(this=0x%08x) -> %s", thisPtr, known ? "S_OK (same object)" : "E_NOINTERFACE");
        if (known) {
            auto it = g_palettes().find(thisPtr);
            if (it != g_palettes().end()) ++it->second.refcount;
            if (ppv) c.write_u32(ppv, thisPtr);
            return 0u;
        }
        if (ppv) c.write_u32(ppv, 0);
        return 0x80004002u;
    }};
    paletteOverrides["AddRef"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_palettes().find(thisPtr);
        uint32_t rc = it != g_palettes().end() ? ++it->second.refcount : 1u;
        IGN_LOG("[PALETTE]", "AddRef(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    paletteOverrides["Release"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_palettes().find(thisPtr);
        uint32_t rc = 0;
        if (it != g_palettes().end() && it->second.refcount > 0) rc = --it->second.refcount;
        IGN_LOG("[PALETTE]", "Release(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    // HRESULT SetEntries(DWORD dwFlags, DWORD dwStartingEntry, DWORD dwCount,
    // LPPALETTEENTRY lpEntries) -- slot 0x18 (index 6), confirmed by real
    // disassembly of Ign_d3d.exe around the caller at VA 0x45c71b: `PUSH
    // 0x00512450 (lpEntries, a static global -- NOT stack/heap); PUSH 0x100
    // (dwCount=256); PUSH 0; PUSH 0 (dwStartingEntry, dwFlags, both 0); PUSH
    // EAX (this); MOV EAX,[EAX]; CALL [EAX+0x18]`. So the real call is
    // SetEntries(flags=0, start=0, count=256, lpEntries=<static 256-entry
    // PALETTEENTRY table>) -- always the full palette, not a partial fade
    // step; the caller isn't itself inside a loop (it's a small
    // flag-gated helper, gated by a one-shot init flag at [0x512448], that
    // just converts a raw 768-byte RGB triplet table into this 1024-byte
    // PALETTEENTRY array immediately beforehand). The HRESULT is turned into
    // a plain BOOL by the caller (CMP EAX,1; SBB EAX,EAX; NEG EAX --
    // "1 if SUCCEEDED(hr)"), so only DD_OK vs. non-zero matters to Ignition.
    paletteOverrides["SetEntries"] = { 5, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), dwFlags = c.arg(1), dwStart = c.arg(2), dwCount = c.arg(3), lpEntries = c.arg(4);
        auto it = g_palettes().find(thisPtr);
        if (it == g_palettes().end()) {
            IGN_LOG("[PALETTE]", "SetEntries(this=0x%08x) -> DDERR_INVALIDPARAMS (unknown palette object)", thisPtr);
            return 0x80070057u;   // DDERR_INVALIDPARAMS
        }
        uint32_t calls = ++it->second.setEntriesCalls;

        if (dwStart >= 256 || dwCount > 256 || dwStart + dwCount > 256) {
            IGN_LOG("[PALETTE]", "SetEntries call #%u this=0x%08x flags=0x%08x start=%u count=%u ptr=0x%08x -> DDERR_INVALIDPARAMS (range out of [0,256))",
                    calls, thisPtr, dwFlags, dwStart, dwCount, lpEntries);
            return 0x80070057u;   // DDERR_INVALIDPARAMS
        }
        if (!lpEntries || !c.mapped(lpEntries)) {
            IGN_LOG("[PALETTE]", "SetEntries call #%u this=0x%08x -> DDERR_INVALIDPARAMS (lpEntries=0x%08x not mapped)",
                    calls, thisPtr, lpEntries);
            return 0x80070057u;
        }

        uint8_t* rangeStart = it->second.entries + (size_t)dwStart * 4;
        uint32_t checksumBefore = fnv1a(rangeStart, (size_t)dwCount * 4);
        std::vector<uint8_t> incoming((size_t)dwCount * 4);
        c.read(lpEntries, incoming.data(), incoming.size());
        std::memcpy(rangeStart, incoming.data(), incoming.size());
        uint32_t checksumAfter = fnv1a(rangeStart, (size_t)dwCount * 4);

        bool onlyFirstOrEvery64 = (calls <= 1) || (calls % 64) == 0;
        if (onlyFirstOrEvery64) {
            IGN_LOG("[PALETTE]", "SetEntries call #%u this=0x%08x flags=0x%08x start=%u count=%u ptr=0x%08x",
                    calls, thisPtr, dwFlags, dwStart, dwCount, lpEntries);
            IGN_LOG("[PALETTE]", "  checksum_before=0x%08x checksum_after=0x%08x", checksumBefore, checksumAfter);
            if (dwCount > 0) {
                const uint8_t* f = rangeStart;
                const uint8_t* l = rangeStart + ((size_t)dwCount - 1) * 4;
                IGN_LOG("[PALETTE]", "  first[%u]: R=%u G=%u B=%u flags=0x%02x", dwStart, f[0], f[1], f[2], f[3]);
                IGN_LOG("[PALETTE]", "  last [%u]: R=%u G=%u B=%u flags=0x%02x", dwStart + dwCount - 1, l[0], l[1], l[2], l[3]);
            }
        }
        if (checksumBefore != checksumAfter) {
            set_phase(Phase::PaletteChanged, nullptr);
            IGN_LOG("[PALETTE]", "palette modified by Ignition: before=0x%08x after=0x%08x (call #%u)",
                    checksumBefore, checksumAfter, calls);
        }
        return 0u;   // DD_OK
    }};
    uint32_t paletteVtable = make_vtable(br, cpu, "IDirectDrawPalette", kPaletteMethods, paletteOverrides);

    // ---- IDirectDraw ----
    std::map<std::string, Override> ddrawOverrides;
    ddrawOverrides["QueryInterface"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), riid = c.arg(1), ppv = c.arg(2);
        uint8_t guid[16] = {0};
        if (riid) c.read(riid, guid, 16);
        bool isUnknown = std::memcmp(guid, kIID_IUnknown, 16) == 0;
        bool isDDraw = std::memcmp(guid, kIID_IDirectDraw, 16) == 0;
        IGN_LOG("[DDRAW]", "IDirectDraw::QueryInterface(this=0x%08x, riid=%02x%02x%02x%02x-%02x%02x-%02x%02x-...) -> %s",
                thisPtr, guid[0], guid[1], guid[2], guid[3], guid[4], guid[5], guid[6], guid[7],
                (isUnknown || isDDraw) ? "S_OK (same object)" : "E_NOINTERFACE (only plain IDirectDraw is implemented so far)");
        if (isUnknown || isDDraw) {
            auto it = g_objects().find(thisPtr);
            if (it != g_objects().end()) ++it->second.refcount;
            if (ppv) c.write_u32(ppv, thisPtr);
            return 0u;
        }
        if (ppv) c.write_u32(ppv, 0);
        return 0x80004002u;   // E_NOINTERFACE
    }};
    ddrawOverrides["AddRef"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_objects().find(thisPtr);
        uint32_t rc = it != g_objects().end() ? ++it->second.refcount : 1u;
        IGN_LOG("[DDRAW]", "IDirectDraw::AddRef(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    ddrawOverrides["Release"] = { 1, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0);
        auto it = g_objects().find(thisPtr);
        uint32_t rc = 0;
        if (it != g_objects().end() && it->second.refcount > 0) rc = --it->second.refcount;
        IGN_LOG("[DDRAW]", "IDirectDraw::Release(this=0x%08x) -> %u", thisPtr, rc);
        return rc;
    }};
    ddrawOverrides["SetCooperativeLevel"] = { 3, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), hwnd = c.arg(1), flags = c.arg(2);
        IGN_LOG("[DDRAW]", "IDirectDraw::SetCooperativeLevel(this=0x%08x, hwnd=0x%08x, flags=0x%08x)", thisPtr, hwnd, flags);
        auto it = g_objects().find(thisPtr);
        if (it != g_objects().end()) { it->second.hwnd = hwnd; it->second.coopFlags = flags; }
        set_phase(Phase::SetCooperativeLevel, nullptr);
        IGN_LOG("[DDRAW]", "IDirectDraw::SetCooperativeLevel -> DD_OK");
        return 0u;
    }};
    ddrawOverrides["SetDisplayMode"] = { 4, [](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), w = c.arg(1), h = c.arg(2), bpp = c.arg(3);
        static uint32_t s_modeChangeCount = 0;
        IGN_LOG("[VIDEO]", "mode change #%u = %ux%ux%u", ++s_modeChangeCount, w, h, bpp);
        IGN_LOG("[DDRAW]", "IDirectDraw::SetDisplayMode(this=0x%08x, %ux%ux%u)", thisPtr, w, h, bpp);
        auto it = g_objects().find(thisPtr);
        if (it != g_objects().end()) { it->second.dispW = w; it->second.dispH = h; it->second.dispBpp = bpp; }
        set_phase(Phase::SetDisplayMode, nullptr);
        IGN_LOG("[DDRAW]", "IDirectDraw::SetDisplayMode -> DD_OK");
        return 0u;
    }};
    // HRESULT CreatePalette(DWORD dwFlags, LPPALETTEENTRY lpColorTable,
    // LPDIRECTDRAWPALETTE* lplpDDPalette, IUnknown* pUnkOuter). Real
    // Ignition call (confirmed by disassembly): dwFlags=0x44
    // (DDPCAPS_ALLOW256|DDPCAPS_8BIT), lpColorTable pointing at a genuine
    // 256-entry PALETTEENTRY array the game had already filled in (its
    // span is exactly 256*4=0x400 bytes, confirmed by the loop that
    // initializes it and the address of the very next global). Whatever is
    // actually there at call time is copied as-is -- never reinterpreted
    // or converted (Ignition needs to keep seeing an authentic 8-bit
    // palettized DirectDraw, not an RGBA one).
    ddrawOverrides["CreatePalette"] = { 5, [paletteVtable](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), dwFlags = c.arg(1), lpColorTable = c.arg(2),
                 lplpDDPalette = c.arg(3), pUnkOuter = c.arg(4);
        (void)pUnkOuter;
        IGN_LOG("[DDRAW]", "IDirectDraw::CreatePalette");
        IGN_LOG("[DDRAW]", "  this       = 0x%08x", thisPtr);
        IGN_LOG("[DDRAW]", "  flags      = 0x%08x%s%s", dwFlags,
                (dwFlags & kDDPCAPS_8BIT) ? " DDPCAPS_8BIT" : "",
                (dwFlags & kDDPCAPS_ALLOW256) ? " DDPCAPS_ALLOW256" : "");
        IGN_LOG("[DDRAW]", "  colorTable = 0x%08x", lpColorTable);
        IGN_LOG("[DDRAW]", "  outPalette = 0x%08x", lplpDDPalette);
        IGN_LOG("[DDRAW]", "  pUnkOuter  = 0x%08x", pUnkOuter);

        DDrawPalette pal;
        pal.vtable = paletteVtable;
        pal.flags = dwFlags;
        if (lpColorTable && c.mapped(lpColorTable)) {
            c.read(lpColorTable, pal.entries, sizeof pal.entries);
        } else {
            IGN_LOG("[PALETTE]", "no initial color table (colorTable=0x%08x not mapped/null) -- starting blank",
                    lpColorTable);
        }

        uint32_t obj = ignition_guest_alloc(4, 4);
        if (!obj) {
            IGN_LOG("[ERROR]", "CreatePalette: guest alloc failed");
            return 0x8007000Eu;   // DDERR_OUTOFMEMORY
        }
        c.write_u32(obj, paletteVtable);
        g_palettes()[obj] = pal;

        IGN_LOG("[PALETTE]", "entries=256 checksum=0x%08x", fnv1a(pal.entries, sizeof pal.entries));
        IGN_LOG("[PALETTE]", "first entries:");
        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = &pal.entries[(size_t)i * 4];
            IGN_LOG("[PALETTE]", "  %d: R=%u G=%u B=%u flags=0x%02x", i, e[0], e[1], e[2], e[3]);
        }

        if (lplpDDPalette && c.mapped(lplpDDPalette)) c.write_u32(lplpDDPalette, obj);
        IGN_LOG("[COM]", "IDirectDrawPalette object = 0x%08x", obj);
        IGN_LOG("[COM]", "IDirectDrawPalette vtable  = 0x%08x", paletteVtable);
        IGN_LOG("[DDRAW]", "CreatePalette -> DD_OK");
        return 0u;
    }};
    ddrawOverrides["CreateSurface"] = { 4, [surfaceVtable](Cpu& c) -> uint32_t {
        uint32_t thisPtr = c.arg(0), descVa = c.arg(1), ppSurf = c.arg(2), pUnkOuter = c.arg(3);
        // DDSURFACEDESC (original, 0x6C bytes) -- field offsets confirmed
        // against Ign_d3d.exe's own stack layout for this exact call
        // (dwFlags=0x21 at struct+0x04, ddsCaps.dwCaps=0x218 at struct+0x68,
        // i.e. DDSD_CAPS|DDSD_BACKBUFFERCOUNT requesting a
        // DDSCAPS_PRIMARYSURFACE|DDSCAPS_FLIP|DDSCAPS_COMPLEX flip chain).
        uint32_t dwFlags = c.read_u32(descVa + 0x04);
        uint32_t dwHeight = c.read_u32(descVa + 0x08);
        uint32_t dwWidth = c.read_u32(descVa + 0x0C);
        uint32_t dwBackBufferCount = c.read_u32(descVa + 0x14);
        uint32_t ddsCaps = c.read_u32(descVa + 0x68);
        (void)pUnkOuter;

        IGN_LOG("[DDRAW]", "IDirectDraw::CreateSurface(this=0x%08x)", thisPtr);
        IGN_LOG("[DDRAW]", "  dwFlags=0x%08x dwHeight=%u dwWidth=%u dwBackBufferCount=%u ddsCaps.dwCaps=0x%08x",
                dwFlags, dwHeight, dwWidth, dwBackBufferCount, ddsCaps);

        // A primary surface is asked for with dwWidth=dwHeight=0 (confirmed:
        // real hardware log shows exactly this) -- real DirectDraw fills
        // that in from the current display mode, so we do the same, reading
        // it back from what SetDisplayMode already stored on this same
        // IDirectDraw object.
        uint32_t width = dwWidth, height = dwHeight, bpp = 8;
        auto ddIt = g_objects().find(thisPtr);
        if (ddIt != g_objects().end()) {
            if (!width) width = ddIt->second.dispW;
            if (!height) height = ddIt->second.dispH;
            if (ddIt->second.dispBpp) bpp = ddIt->second.dispBpp;
        }
        if (!width || !height) {
            IGN_LOG("[ERROR]", "CreateSurface: no width/height available (SetDisplayMode not called yet?) -- refusing");
            return 0x80070057u;   // DDERR_INVALIDPARAMS
        }
        uint32_t bytesPerPixel = (bpp + 7) / 8;
        uint32_t pitch = width * bytesPerPixel;
        uint32_t surfaceBytes = pitch * height;

        uint32_t primaryPixels = ignition_guest_alloc_pixels(surfaceBytes);
        if (!primaryPixels) {
            IGN_LOG("[ERROR]", "CreateSurface: pixel allocation failed for primary (%u bytes)", surfaceBytes);
            return 0x8007000Eu;   // DDERR_OUTOFMEMORY
        }
        uint32_t primaryObj = ignition_guest_alloc(4, 4);
        if (!primaryObj) {
            IGN_LOG("[ERROR]", "CreateSurface: guest alloc failed for primary object");
            return 0x8007000Eu;
        }
        c.write_u32(primaryObj, surfaceVtable);
        DDrawSurf primary;
        primary.vtable = surfaceVtable; primary.refcount = 1;
        primary.width = width; primary.height = height; primary.bpp = bpp; primary.pitch = pitch;
        primary.caps = ddsCaps; primary.pixels = primaryPixels; primary.pixelsSize = surfaceBytes;
        IGN_LOG("[SURFACE]", "primary pixels = 0x%08x size=%u pitch=%u (%ux%ux%u)",
                primaryPixels, surfaceBytes, pitch, width, height, bpp);

        // DDSCAPS_COMPLEX + a backbuffer count means DirectDraw implicitly
        // creates the attached backbuffer(s) here -- CreateSurface only
        // ever hands back the PRIMARY pointer; the app retrieves the
        // backbuffer later via GetAttachedSurface(DDSCAPS_BACKBUFFER),
        // which is exactly the call the real hardware log shows next.
        // Two DISTINCT objects with two DISTINCT pixel buffers on purpose:
        // collapsing them would hide the real flip-chain behavior
        // (Lock backbuffer / draw / Unlock / Flip primary) this project
        // still needs to exercise correctly later.
        if ((ddsCaps & kDDSCAPS_COMPLEX) && dwBackBufferCount > 0) {
            uint32_t backPixels = ignition_guest_alloc_pixels(surfaceBytes);
            uint32_t backObj = backPixels ? ignition_guest_alloc(4, 4) : 0;
            if (!backPixels || !backObj) {
                IGN_LOG("[ERROR]", "CreateSurface: backbuffer allocation failed -- primary created without it");
            } else {
                c.write_u32(backObj, surfaceVtable);
                DDrawSurf back;
                back.vtable = surfaceVtable; back.refcount = 1;
                back.width = width; back.height = height; back.bpp = bpp; back.pitch = pitch;
                back.caps = kDDSCAPS_BACKBUFFER | kDDSCAPS_FLIP;
                back.pixels = backPixels; back.pixelsSize = surfaceBytes;
                back.attached = primaryObj;
                primary.attached = backObj;
                g_surfaces()[backObj] = back;
                IGN_LOG("[SURFACE]", "backbuffer pixels = 0x%08x size=%u pitch=%u (%ux%ux%u), object=0x%08x",
                        backPixels, surfaceBytes, pitch, width, height, bpp, backObj);
            }
        }
        g_surfaces()[primaryObj] = primary;

        if (ppSurf) c.write_u32(ppSurf, primaryObj);
        set_phase(Phase::CreateSurface, "primary + backbuffer created with real pixel backing");
        IGN_LOG("[DDRAW]", "IDirectDraw::CreateSurface -> DD_OK (primary=0x%08x, vtable=0x%08x)", primaryObj, surfaceVtable);
        return 0u;   // DD_OK
    }};
    uint32_t ddrawVtable = make_vtable(br, cpu, "IDirectDraw", kDDrawMethods, ddrawOverrides);

    // ---- DirectDrawCreate itself: overrides win32_shims_misc.cpp's honest
    // DDERR_GENERIC and this project's own previous "stop here" placeholder.
    Shim create;
    create.argc = 3;
    create.stdcall_cleanup = true;
    create.tag = "DDRAW.dll!DirectDrawCreate";
    create.fn = [ddrawVtable](Cpu& c) -> uint32_t {
        uint32_t pGUID = c.arg(0), ppDD = c.arg(1), pUnkOuter = c.arg(2);
        IGN_LOG("[WIN32]", "DirectDrawCreate(pGUID=0x%08x, ppDD=0x%08x, pUnkOuter=0x%08x)", pGUID, ppDD, pUnkOuter);

        if (ppDD && !c.mapped(ppDD)) {
            IGN_LOG("[ERROR]", "DirectDrawCreate: ppDD=0x%08x is not mapped guest memory -- refusing to write", ppDD);
            return 0x80070057u;   // DDERR_INVALIDPARAMS
        }

        uint32_t obj = ignition_guest_alloc(4, 4);
        if (!obj) {
            IGN_LOG("[ERROR]", "DirectDrawCreate: guest alloc failed");
            return 0x8007000Eu;   // DDERR_OUTOFMEMORY
        }
        c.write_u32(obj, ddrawVtable);
        DDrawObj st; st.vtable = ddrawVtable; st.refcount = 1;
        g_objects()[obj] = st;

        if (ppDD) c.write_u32(ppDD, obj);
        set_phase(Phase::DirectDrawCreate, "real COM object created");
        IGN_LOG("[DDRAW]", "object=0x%08x vtable=0x%08x refcount=1, *ppDD written at guest 0x%08x",
                obj, ddrawVtable, ppDD);
        IGN_LOG("[DDRAW]", "DirectDrawCreate -> DD_OK");
        return 0u;   // DD_OK
    };
    br.register_shim("DDRAW.dll", "DirectDrawCreate", create);
}

} // namespace ign
