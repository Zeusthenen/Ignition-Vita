// src/runtime/win32_shims_wintrust.h — WINTRUST.dll!WinVerifyTrust and the
// slice of CRYPT32.dll that reads an embedded Authenticode signature
// (CryptQueryObject, CryptMsgGetParam, CryptMsgClose,
// CertFindCertificateInStore, CertGetNameStringW, CertFreeCertificateContext,
// CertCloseStore).
//
// HONEST BY CONSTRUCTION. Every response is COMPUTED over the bytes of the
// file requested by runtime/authenticode.cpp: a real digest check, RSA
// signature verification, and chain building up to a trusted root. A
// modified file, altered signature, or unanchored chain makes the API FAIL,
// with Windows' own codes and GetLastError. The returned structures
// (CERT_CONTEXT, CERT_INFO, CMSG_SIGNER_INFO) are REAL x86 structures in
// guest memory, filled from the certificate's and SignerInfo's DER.
//
// WHAT THE CONSUMER PROVIDES: resolving a GUEST path (the one the program
// passes, e.g. "C:\Game\module.dll") to the real file's bytes — the same
// division of responsibility as Bridge::set_version_resource_source.
// Without a source, every request fails as file-not-found.
//
// ALLOCATION. Returned CERT_CONTEXTs live in the guest scratch area
// (guest_scratch.h), one per distinct certificate (keyed on its DER bytes),
// reused afterward: memory usage is bounded by the number of distinct
// certificates, not the number of calls. CertFreeCertificateContext
// therefore frees nothing (on Windows the pointer becomes invalid; here it
// stays readable — the only difference, and it changes no response).
// HCERTSTORE/HCRYPTMSG are OPAQUE handles (never dereferenced by a correct
// Win32 caller), valid until closed.
//
// NOT IMPLEMENTED (an explicit failure, never a false success):
//   * WinVerifyTrust: actions other than WINTRUST_ACTION_GENERIC_VERIFY_V2
//     -> TRUST_E_PROVIDER_UNKNOWN; dwUnionChoice other than WTD_CHOICE_FILE,
//     or a file designated only by hFile -> HRESULT_FROM_WIN32(
//     ERROR_CALL_NOT_IMPLEMENTED) = 0x80070078;
//   * CryptQueryObject: BLOB objects and content types other than
//     CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED -> FALSE,
//     ERROR_CALL_NOT_IMPLEMENTED;
//   * CryptMsgGetParam: parameters other than TYPE, INNER_CONTENT_TYPE,
//     SIGNER_COUNT, SIGNER_INFO, SIGNER_CERT_INFO, CERT_COUNT, CERT
//     -> FALSE, CRYPT_E_INVALID_MSG_TYPE;
//   * CertFindCertificateInStore: searches other than ANY, SHA1_HASH,
//     SUBJECT_CERT -> NULL, ERROR_CALL_NOT_IMPLEMENTED;
//   * CertGetNameStringW: types other than ATTR_TYPE, SIMPLE_DISPLAY,
//     FRIENDLY_DISPLAY -> empty string (1) and a report to the observer.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace d2rt { class Bridge; class Cpu; struct Shim; }

// Source of a file's bytes, given a GUEST path (UTF-8). Returns false if the
// file doesn't exist. A SINGLE registration: a second call replaces the
// first.
typedef std::function<bool(const std::string& guestPath, std::vector<uint8_t>& bytes)> Wx86WintrustFileSource;
void wx86_wintrust_set_file_source(Wx86WintrustFileSource src);

// "Now" clock (Unix seconds UTC) for judging a signature that is NOT
// timestamped. Default: std::time(nullptr).
void wx86_wintrust_set_clock(int64_t (*now)());

// Observer: one line per call that DECIDES something (path, code, reason).
// The engine is silent by construction; it's up to the consumer to turn
// this into a log. Not set = silence.
typedef void (*Wx86WintrustObserverFn)(const char* line);
void wx86_wintrust_set_observer(Wx86WintrustObserverFn cb);

// Registers the shims with an arbitrary receiver (install() passes
// Bridge::register_shim; a host-side test harness passes its own, without a
// bridge or a real CPU).
void wx86_wintrust_register(const std::function<void(const char* dll, const char* name, const d2rt::Shim&)>& reg);

void win32_shims_wintrust_install(d2rt::Bridge& br);
