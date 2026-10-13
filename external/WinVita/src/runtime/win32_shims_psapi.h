// src/runtime/win32_shims_psapi.h — generic process/module enumeration
// (EnumProcessModules/K32EnumProcessModules, GetModuleInformation/
// K32GetModuleInformation, PSAPI.DLL!GetModuleInformation): honest views
// over Bridge::loaded_modules(), no guest-specific state.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_psapi_install(d2rt::Bridge& br);
