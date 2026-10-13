// src/runtime/win32_shims_version.h — generic VERSION.dll shims
// (GetFileVersionInfoSizeW/W, VerQueryValueW/A): real VS_FIXEDFILEINFO
// extraction over whatever bytes the consumer's registered version-resource
// source returns. See Bridge::set_version_resource_source.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_version_install(d2rt::Bridge& br);
