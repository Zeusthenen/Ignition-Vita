// src/runtime/win32_shims_advapi32.h — generic ADVAPI32 security/token/ACL/
// Service-Control-Manager shims: fake-success stubs with no security model,
// correct for any guest that merely needs these calls to not fail. No
// literal, path, or behavior specific to any one guest.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_advapi32_install(d2rt::Bridge& br);
