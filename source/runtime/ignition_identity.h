// runtime/ignition_identity.h — single source of truth for "which guest
// executable are we running" (iteration G1: fixes a real bug where
// GetCommandLineA/GetModuleFileNameA answered "Ign_d3d.exe" even when the
// 3dfx build had actually loaded Ign_3dfx.exe -- those strings were
// hardcoded independently in kernel32_ign.cpp instead of deriving from the
// same IGNITION_RENDERER_3DFX switch ignition_boot.cpp's kExePath already
// used). Every shim or boot-sequence line that needs to name the current
// guest executable should include this header instead of hardcoding its
// own literal.
#pragma once

namespace ign {

#ifdef IGNITION_RENDERER_3DFX
constexpr const char* kGuestExeName          = "Ign_3dfx.exe";
constexpr const char* kGuestExePathFwdSlash  = "ux0:data/ignition/Ign_3dfx.exe";     // fopen()-style, forward slashes
constexpr const char* kGuestExePathBackSlash = "ux0:data\\ignition\\Ign_3dfx.exe";   // guest-visible Win32 path, backslashes
constexpr const char* kRendererVariantName   = "3DFX";
#else
constexpr const char* kGuestExeName          = "Ign_d3d.exe";
constexpr const char* kGuestExePathFwdSlash  = "ux0:data/ignition/Ign_d3d.exe";
constexpr const char* kGuestExePathBackSlash = "ux0:data\\ignition\\Ign_d3d.exe";
constexpr const char* kRendererVariantName   = "DDRAW";
#endif

} // namespace ign
