// src/runtime/win32_shims_wintrust.cpp — see win32_shims_wintrust.h.
#include "win32_shims_wintrust.h"
#include "runtime/authenticode.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_thread_ctx.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>

using namespace d2rt;
namespace ac = wx86::ac;

namespace {

// ---- Codes ----
const uint32_t ERROR_FILE_NOT_FOUND        = 2;
const uint32_t ERROR_INVALID_HANDLE        = 6;
const uint32_t ERROR_INVALID_PARAMETER     = 87;
const uint32_t ERROR_CALL_NOT_IMPLEMENTED  = 120;
const uint32_t ERROR_MORE_DATA             = 234;
const uint32_t E_INVALIDARG                = 0x80070057u;
const uint32_t HR_NOT_IMPLEMENTED          = 0x80070078u;  // HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED)
const uint32_t CRYPT_E_INVALID_MSG_TYPE    = 0x80091004u;
const uint32_t CRYPT_E_INVALID_INDEX       = 0x80091008u;
const uint32_t CRYPT_E_NOT_FOUND           = 0x80092004u;

// ---- Constants from wintrust.h / wincrypt.h ----
const uint8_t GUID_GENERIC_VERIFY_V2[16] = { 0x6b,0xc5,0xaa,0x00, 0x44,0xcd, 0xd0,0x11,
                                             0x8c,0xc2,0x00,0xc0,0x4f,0xc2,0x95,0xee };
// Values copied from wintrust.h (mingw-w64), not typed from memory.
const uint32_t WTD_REVOKE_NONE = 0, WTD_CHOICE_FILE = 1;
const uint32_t WTD_STATEACTION_VERIFY = 1, WTD_STATEACTION_CLOSE = 2;
const uint32_t WTD_REVOCATION_CHECK_NONE = 0x10, WTD_REVOCATION_CHECK_ANY = 0x20 | 0x40 | 0x80;
const uint32_t WTD_HASH_ONLY_FLAG = 0x200, WTD_LIFETIME_SIGNING_FLAG = 0x800;
// WTD_SAFER_FLAG (0x100): "reserved" in the documentation, no documented
// effect on the verdict; read but ignored.
const uint32_t X509_PKCS7 = 0x00010001u;
const uint32_t CERT_QUERY_OBJECT_FILE = 1;
const uint32_t CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED = 0x400;
const uint32_t CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED = 10;
const uint32_t CERT_QUERY_FORMAT_BINARY = 1, CERT_QUERY_FORMAT_FLAG_BINARY = 2;
const uint32_t CMSG_SIGNED = 2;
const uint32_t CMSG_TYPE_PARAM = 1, CMSG_INNER_CONTENT_TYPE_PARAM = 4, CMSG_SIGNER_COUNT_PARAM = 5,
               CMSG_SIGNER_INFO_PARAM = 6, CMSG_SIGNER_CERT_INFO_PARAM = 7,
               CMSG_CERT_COUNT_PARAM = 11, CMSG_CERT_PARAM = 12;
const uint32_t CERT_FIND_ANY = 0, CERT_FIND_SHA1_HASH = 0x10000, CERT_FIND_SUBJECT_CERT = 0xB0000;
const uint32_t CERT_NAME_ATTR_TYPE = 3, CERT_NAME_SIMPLE_DISPLAY_TYPE = 4, CERT_NAME_FRIENDLY_DISPLAY_TYPE = 5;
const uint32_t CERT_NAME_ISSUER_FLAG = 1;

// ---- State ----
std::mutex g_mu;
Wx86WintrustFileSource g_src;
int64_t (*g_clock)() = nullptr;
Wx86WintrustObserverFn g_obs = nullptr;

std::map<uint32_t, std::shared_ptr<ac::SignedData>> g_msgs, g_stores;
std::map<uint32_t, int> g_states;                     // open hWVTStateData handles
uint32_t g_nextHandle = 0;
std::map<std::string, uint32_t> g_ctxByDer;            // CERT_CONTEXT per certificate
std::map<uint32_t, std::string> g_derByCtx;

uint32_t new_handle() {                                // opaque handle, never 0
    g_nextHandle++;
    return 0x7E5C0000u + 8u * g_nextHandle;
}

void observe(const char* fmt, const char* a, uint32_t v, const char* b) {
    if (!g_obs) return;
    char line[512];
    std::snprintf(line, sizeof line, fmt, a, (unsigned)v, b);
    g_obs(line);
}

std::string gread_wide_utf8(Cpu& c, uint32_t p) {
    std::string s; if (!p) return s;
    for (uint32_t off = 0; off < 0x10000; off += 2) {
        uint16_t ch = 0; if (!c.read(p + off, &ch, 2) || !ch) break;
        uint32_t cp = ch;
        if (ch >= 0xD800 && ch < 0xDC00) {
            uint16_t lo = 0; c.read(p + off + 2, &lo, 2);
            if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); off += 2; }
        }
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3f)); s += (char)(0x80 | (cp & 0x3f)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3f)); s += (char)(0x80 | ((cp >> 6) & 0x3f)); s += (char)(0x80 | (cp & 0x3f)); }
    }
    return s;
}
std::string gread_cstr(Cpu& c, uint32_t p) {
    std::string s; if (!p) return s;
    for (uint32_t i = 0; i < 0x1000; i++) { char ch = 0; if (!c.read(p + i, &ch, 1) || !ch) break; s += ch; }
    return s;
}

bool load_file(const std::string& path, std::vector<uint8_t>& bytes) {
    Wx86WintrustFileSource src;
    { std::lock_guard<std::mutex> lk(g_mu); src = g_src; }
    return src && src(path, bytes);
}

// ---- Builder for x86 structures with internal pointers ----
// Everything is laid out in ONE buffer whose guest base address is known in
// advance: the layout doesn't depend on the base, so a first pass with
// base 0 gives the exact size (CryptoAPI's "pvData = NULL" protocol).
struct Layout {
    std::vector<uint8_t> b; uint32_t base;
    explicit Layout(uint32_t bs) : base(bs) {}
    size_t align() { while (b.size() % 4) b.push_back(0); return b.size(); }
    size_t reserve(size_t n) { size_t o = align(); b.resize(o + n, 0); return o; }
    void u32(size_t off, uint32_t v) { std::memcpy(&b[off], &v, 4); }
    uint32_t at(size_t off) const { return base + (uint32_t)off; }
    size_t bytes(const uint8_t* p, size_t n) { size_t o = align(); b.insert(b.end(), p, p + n); return o; }
    size_t str(const std::string& s) { size_t o = align(); b.insert(b.end(), s.begin(), s.end()); b.push_back(0); return o; }
    // CRYPT_DATA_BLOB {cbData, pbData}
    void blob(size_t off, const uint8_t* p, size_t n) {
        if (!n) { u32(off, 0); u32(off + 4, 0); return; }
        size_t d = bytes(p, n); u32(off, (uint32_t)n); u32(off + 4, at(d));
    }
    // CRYPT_ALGORITHM_IDENTIFIER {pszObjId, Parameters{cb,pb}}
    void alg(size_t off, const std::string& oid, const uint8_t* params, size_t n) {
        size_t s = str(oid); u32(off, at(s)); blob(off + 4, params, n);
    }
};

std::vector<uint8_t> reversed(const uint8_t* p, size_t n) {     // INTEGER DER -> CRYPT_INTEGER_BLOB (little-endian)
    std::vector<uint8_t> r(p, p + n); std::reverse(r.begin(), r.end()); return r;
}
uint64_t filetime(int64_t t) { return (uint64_t)(t + 11644473600LL) * 10000000ULL; }

void put_attrs(Layout& L, size_t off, const ac::SignedData& sd, const std::vector<ac::Attribute>& attrs) {
    L.u32(off, (uint32_t)attrs.size());
    if (attrs.empty()) { L.u32(off + 4, 0); return; }
    size_t arr = L.reserve(12 * attrs.size());
    L.u32(off + 4, L.at(arr));
    for (size_t i = 0; i < attrs.size(); i++) {
        const ac::Attribute& a = attrs[i];
        size_t e = arr + 12 * i;
        size_t s = L.str(a.oid); L.u32(e, L.at(s));
        L.u32(e + 4, (uint32_t)a.values.size());
        if (a.values.empty()) { L.u32(e + 8, 0); continue; }
        size_t vals = L.reserve(8 * a.values.size());
        L.u32(e + 8, L.at(vals));
        for (size_t j = 0; j < a.values.size(); j++) L.blob(vals + 8 * j, sd.p(a.values[j]), a.values[j].len);
    }
}

// CMSG_SIGNER_INFO (0x44 bytes + data)
std::vector<uint8_t> build_signer_info(const ac::SignedData& sd, const ac::SignerInfo& si, uint32_t base) {
    Layout L(base);
    size_t h = L.reserve(0x44);
    L.u32(h + 0x00, si.version);
    L.blob(h + 0x04, sd.p(si.issuer), si.issuer.len);
    auto ser = reversed(sd.p(si.serial), si.serial.len);
    L.blob(h + 0x0c, ser.data(), ser.size());
    L.alg(h + 0x14, si.digestAlgOid, sd.p(si.digestAlgParams), si.digestAlgParams.len);
    L.alg(h + 0x20, si.encAlgOid, sd.p(si.encAlgParams), si.encAlgParams.len);
    L.blob(h + 0x2c, sd.p(si.encryptedDigest), si.encryptedDigest.len);
    put_attrs(L, h + 0x34, sd, si.authAttrs);
    put_attrs(L, h + 0x3c, sd, si.unauthAttrs);
    L.align();
    return L.b;
}

// CERT_INFO (0x70) at offset `h` of L.
void fill_cert_info(Layout& L, size_t h, const ac::Certificate& c) {
    L.u32(h + 0x00, c.version);
    auto ser = reversed(c.p(c.serial), c.serial.len);
    L.blob(h + 0x04, ser.data(), ser.size());
    L.alg(h + 0x0c, c.tbsSigAlgOid, c.p(c.tbsSigAlgParams), c.tbsSigAlgParams.len);
    L.blob(h + 0x18, c.p(c.issuer), c.issuer.len);
    uint64_t nb = filetime(c.notBefore), na = filetime(c.notAfter);
    L.u32(h + 0x20, (uint32_t)nb); L.u32(h + 0x24, (uint32_t)(nb >> 32));
    L.u32(h + 0x28, (uint32_t)na); L.u32(h + 0x2c, (uint32_t)(na >> 32));
    L.blob(h + 0x30, c.p(c.subject), c.subject.len);
    L.alg(h + 0x38, c.spkiAlgOid, c.p(c.spkiAlgParams), c.spkiAlgParams.len);
    L.blob(h + 0x44, c.p(c.pubKey), c.pubKey.len);          // PublicKey {cbData, pbData, cUnusedBits}
    L.u32(h + 0x4c, c.pubKeyUnusedBits);
    // IssuerUniqueId (0x50) / SubjectUniqueId (0x5c): absent -> zeros
    L.u32(h + 0x68, (uint32_t)c.exts.size());
    if (c.exts.empty()) { L.u32(h + 0x6c, 0); return; }
    size_t arr = L.reserve(16 * c.exts.size());
    L.u32(h + 0x6c, L.at(arr));
    for (size_t i = 0; i < c.exts.size(); i++) {
        size_t e = arr + 16 * i;
        size_t s = L.str(c.exts[i].oid); L.u32(e, L.at(s));
        L.u32(e + 4, c.exts[i].critical ? 1 : 0);
        L.blob(e + 8, c.p(c.exts[i].value), c.exts[i].value.len);
    }
}

// CERT_CONTEXT {dwCertEncodingType, pbCertEncoded, cbCertEncoded, pCertInfo, hCertStore}
std::vector<uint8_t> build_cert_context(const ac::Certificate& c, uint32_t base, uint32_t hStore) {
    Layout L(base);
    size_t h = L.reserve(20);
    size_t der = L.bytes(c.der.data(), c.der.size());
    size_t ci = L.reserve(0x70);
    fill_cert_info(L, ci, c);
    L.u32(h + 0, 1);                                         // X509_ASN_ENCODING
    L.u32(h + 4, L.at(der));
    L.u32(h + 8, (uint32_t)c.der.size());
    L.u32(h + 12, L.at(ci));
    L.u32(h + 16, hStore);
    L.align();
    return L.b;
}

// Returns the certificate's guest CERT_CONTEXT (created once), 0 if the scratch area is exhausted.
uint32_t context_for(Cpu& cpu, const ac::Certificate& c, uint32_t hStore) {
    std::string key(c.der.begin(), c.der.end());
    uint32_t p = 0;
    { std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_ctxByDer.find(key); if (it != g_ctxByDer.end()) p = it->second; }
    if (!p) {
        size_t n = build_cert_context(c, 0, hStore).size();
        p = wx86_scratch_alloc((uint32_t)n);
        if (!p) return 0;
        auto bytes = build_cert_context(c, p, hStore);
        cpu.write(p, bytes.data(), (uint32_t)bytes.size());
        std::lock_guard<std::mutex> lk(g_mu);
        g_ctxByDer[key] = p; g_derByCtx[p] = key;
    } else {
        cpu.write_u32(p + 16, hStore);                       // hCertStore of the store returning it
    }
    return p;
}

// CryptoAPI size protocol: pvData NULL -> size; too small -> ERROR_MORE_DATA.
uint32_t give(Cpu& c, uint32_t pvData, uint32_t pcbData, const std::vector<uint8_t>& bytes) {
    uint32_t need = (uint32_t)bytes.size();
    if (!pcbData) { wx86_set_lasterr(c, ERROR_INVALID_PARAMETER); return 0; }
    if (!pvData) { c.write_u32(pcbData, need); return 1; }
    uint32_t cap = c.read_u32(pcbData);
    c.write_u32(pcbData, need);
    if (cap < need) { wx86_set_lasterr(c, ERROR_MORE_DATA); return 0; }
    c.write(pvData, bytes.data(), need);
    return 1;
}

// ============================================================================
uint32_t WinVerifyTrust(Cpu& c) {
    uint32_t pAction = c.arg(1), pData = c.arg(2);
    auto ret = [&](uint32_t hr, uint32_t le, const char* path, const char* why) {
        wx86_set_lasterr(c, le);
        observe("WinVerifyTrust(%s) -> 0x%08x (%s)", path, hr, why);
        return hr;
    };
    uint8_t guid[16] = {0};
    if (!pAction || !c.read(pAction, guid, 16) || std::memcmp(guid, GUID_GENERIC_VERIFY_V2, 16) != 0)
        return ret(ac::TRUST_E_PROVIDER_UNKNOWN, ac::TRUST_E_PROVIDER_UNKNOWN, "-", "action GUID not implemented");
    if (!pData) return ret(E_INVALIDARG, E_INVALIDARG, "-", "pWVTData NULL");
    uint32_t cb = c.read_u32(pData);
    if (cb < 0x28) return ret(E_INVALIDARG, E_INVALIDARG, "-", "WINTRUST_DATA too small");
    uint32_t revoke = c.read_u32(pData + 0x10), choice = c.read_u32(pData + 0x14);
    uint32_t pFile = c.read_u32(pData + 0x18), action = c.read_u32(pData + 0x1c);
    uint32_t provFlags = cb >= 0x2c ? c.read_u32(pData + 0x28) : 0;
    if (action == WTD_STATEACTION_CLOSE) {
        uint32_t h = c.read_u32(pData + 0x20);
        { std::lock_guard<std::mutex> lk(g_mu); g_states.erase(h); }
        c.write_u32(pData + 0x20, 0);
        return ret(0, 0, "-", "state closed");
    }
    if (choice != WTD_CHOICE_FILE) return ret(HR_NOT_IMPLEMENTED, HR_NOT_IMPLEMENTED, "-", "dwUnionChoice not implemented");
    uint32_t pPath = pFile ? c.read_u32(pFile + 4) : 0;
    if (!pPath) return ret(HR_NOT_IMPLEMENTED, HR_NOT_IMPLEMENTED, "-", "hFile-only subject not implemented");
    std::string path = gread_wide_utf8(c, pPath);
    std::vector<uint8_t> bytes;
    // File not found: raw ERROR_FILE_NOT_FOUND (not an HRESULT) -- matches
    // Wine's wintrust; unconfirmed against real Windows.
    if (!load_file(path, bytes)) return ret(ERROR_FILE_NOT_FOUND, ERROR_FILE_NOT_FOUND, path.c_str(), "file not found");

    ac::VerifyOptions opt;
    opt.now = g_clock ? g_clock() : 0;
    // See authenticode.h: offline, the default policy returns the same result
    // whether revocation is requested or not; the report still records it.
    opt.revocation_requested = (revoke != WTD_REVOKE_NONE || (provFlags & WTD_REVOCATION_CHECK_ANY)) &&
                               !(provFlags & WTD_REVOCATION_CHECK_NONE);
    opt.hash_only = (provFlags & WTD_HASH_ONLY_FLAG) != 0;
    opt.lifetime_signing = (provFlags & WTD_LIFETIME_SIGNING_FLAG) != 0;
    ac::VerifyReport rep;
    uint32_t hr = ac::verify_pe(bytes, opt, rep);
    if (action == WTD_STATEACTION_VERIFY) {
        std::lock_guard<std::mutex> lk(g_mu);
        uint32_t h = new_handle(); g_states[h] = 1;
        c.write_u32(pData + 0x20, h);
    }
    return ret(hr, rep.last_error, path.c_str(), rep.detail.c_str());
}

uint32_t CryptQueryObject(Cpu& c) {
    uint32_t type = c.arg(0), pv = c.arg(1), contentFlags = c.arg(2), fmtFlags = c.arg(3);
    uint32_t pEnc = c.arg(5), pCt = c.arg(6), pFmt = c.arg(7), phStore = c.arg(8), phMsg = c.arg(9);
    auto fail = [&](uint32_t le, const char* path, const char* why) {
        wx86_set_lasterr(c, le);
        observe("CryptQueryObject(%s) -> FALSE le=0x%08x (%s)", path, le, why);
        return 0u;
    };
    if (type != CERT_QUERY_OBJECT_FILE) return fail(ERROR_CALL_NOT_IMPLEMENTED, "-", "object type not implemented");
    if (!(contentFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED) || !(fmtFlags & CERT_QUERY_FORMAT_FLAG_BINARY))
        return fail(ERROR_CALL_NOT_IMPLEMENTED, "-", "content/format not implemented");
    std::string path = gread_wide_utf8(c, pv);
    std::vector<uint8_t> bytes;
    // Any failure -> CRYPT_E_NO_MATCH, including file-not-found -- matches
    // Wine's crypt32; unconfirmed against real Windows.
    if (!load_file(path, bytes)) return fail(ac::CRYPT_E_NO_MATCH, path.c_str(), "file not found");
    std::vector<uint8_t> p7; uint32_t e = 0;
    if (!ac::pe_extract_pkcs7(bytes, p7, &e)) return fail(ac::CRYPT_E_NO_MATCH, path.c_str(), "no embedded PKCS#7");
    auto sd = std::make_shared<ac::SignedData>();
    if (!sd->parse(p7.data(), p7.size(), &e)) return fail(ac::CRYPT_E_NO_MATCH, path.c_str(), "PKCS#7 decode failed");
    uint32_t hs, hm;
    { std::lock_guard<std::mutex> lk(g_mu);
      hs = new_handle(); hm = new_handle(); g_stores[hs] = sd; g_msgs[hm] = sd; }
    if (pEnc) c.write_u32(pEnc, X509_PKCS7);
    if (pCt) c.write_u32(pCt, CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED);
    if (pFmt) c.write_u32(pFmt, CERT_QUERY_FORMAT_BINARY);
    if (phStore) c.write_u32(phStore, hs); else { std::lock_guard<std::mutex> lk(g_mu); g_stores.erase(hs); }
    if (phMsg) c.write_u32(phMsg, hm); else { std::lock_guard<std::mutex> lk(g_mu); g_msgs.erase(hm); }
    observe("CryptQueryObject(%s) -> TRUE (%u signers) %s", path.c_str(), (uint32_t)sd->signers.size(), "");
    return 1u;
}

std::shared_ptr<ac::SignedData> lookup(std::map<uint32_t, std::shared_ptr<ac::SignedData>>& m, uint32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = m.find(h); return it == m.end() ? nullptr : it->second;
}

uint32_t CryptMsgGetParam(Cpu& c) {
    uint32_t h = c.arg(0), param = c.arg(1), idx = c.arg(2), pvData = c.arg(3), pcbData = c.arg(4);
    auto sd = lookup(g_msgs, h);
    if (!sd) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
    std::vector<uint8_t> out;
    auto dword = [&](uint32_t v) { out.resize(4); std::memcpy(out.data(), &v, 4); };
    uint32_t base = pvData;
    switch (param) {
    case CMSG_TYPE_PARAM: dword(CMSG_SIGNED); break;
    case CMSG_INNER_CONTENT_TYPE_PARAM: out.assign(sd->contentType.begin(), sd->contentType.end()); out.push_back(0); break;
    case CMSG_SIGNER_COUNT_PARAM: dword((uint32_t)sd->signers.size()); break;
    case CMSG_CERT_COUNT_PARAM: dword((uint32_t)sd->certs.size()); break;
    case CMSG_CERT_PARAM:
        if (idx >= sd->certs.size()) { wx86_set_lasterr(c, CRYPT_E_INVALID_INDEX); return 0; }
        out = sd->certs[idx].der; break;
    case CMSG_SIGNER_INFO_PARAM:
        if (idx >= sd->signers.size()) { wx86_set_lasterr(c, CRYPT_E_INVALID_INDEX); return 0; }
        out = build_signer_info(*sd, sd->signers[idx], base); break;
    case CMSG_SIGNER_CERT_INFO_PARAM: {
        if (idx >= sd->signers.size()) { wx86_set_lasterr(c, CRYPT_E_INVALID_INDEX); return 0; }
        const ac::SignerInfo& si = sd->signers[idx];
        Layout L(base); size_t ci = L.reserve(0x70);
        auto ser = reversed(sd->p(si.serial), si.serial.len);
        L.blob(ci + 0x04, ser.data(), ser.size());
        L.blob(ci + 0x18, sd->p(si.issuer), si.issuer.len);
        L.align(); out = L.b; break; }
    default:
        wx86_set_lasterr(c, CRYPT_E_INVALID_MSG_TYPE);
        observe("CryptMsgGetParam(%s) param %u -> FALSE (%s)", "-", param, "not implemented");
        return 0;
    }
    return give(c, pvData, pcbData, out);
}

uint32_t CryptMsgClose(Cpu& c) {
    uint32_t h = c.arg(0);
    if (h) { std::lock_guard<std::mutex> lk(g_mu); g_msgs.erase(h); }
    return 1u;
}

uint32_t CertCloseStore(Cpu& c) {
    uint32_t h = c.arg(0);
    if (h) { std::lock_guard<std::mutex> lk(g_mu); g_stores.erase(h); }
    return 1u;
}

uint32_t CertFreeCertificateContext(Cpu&) { return 1u; }

bool blob_eq_int_le(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t la = a.size(), lb = b.size();
    while (la > 1 && !a[la - 1]) la--;                       // high-order zeros (little-endian)
    while (lb > 1 && !b[lb - 1]) lb--;
    return la == lb && std::memcmp(a.data(), b.data(), la) == 0;
}
std::vector<uint8_t> gread_blob(Cpu& c, uint32_t at) {
    uint32_t cb = c.read_u32(at), pb = c.read_u32(at + 4);
    std::vector<uint8_t> v;
    if (cb && pb && cb < (1u << 24)) { v.resize(cb); c.read(pb, v.data(), cb); }
    return v;
}

uint32_t CertFindCertificateInStore(Cpu& c) {
    uint32_t hs = c.arg(0), findType = c.arg(3), pPara = c.arg(4), pPrev = c.arg(5);
    auto sd = lookup(g_stores, hs);
    if (!sd) { wx86_set_lasterr(c, ERROR_INVALID_HANDLE); return 0; }
    size_t start = 0;
    if (pPrev) {
        std::string prev;
        { std::lock_guard<std::mutex> lk(g_mu); auto it = g_derByCtx.find(pPrev); if (it != g_derByCtx.end()) prev = it->second; }
        for (size_t i = 0; i < sd->certs.size(); i++)
            if (std::string(sd->certs[i].der.begin(), sd->certs[i].der.end()) == prev) { start = i + 1; break; }
    }
    std::vector<uint8_t> issuer, serial, hash;
    if (findType == CERT_FIND_SUBJECT_CERT) {
        if (!pPara) { wx86_set_lasterr(c, E_INVALIDARG); return 0; }
        serial = gread_blob(c, pPara + 0x04);
        issuer = gread_blob(c, pPara + 0x18);
    } else if (findType == CERT_FIND_SHA1_HASH) {
        if (!pPara) { wx86_set_lasterr(c, E_INVALIDARG); return 0; }
        hash = gread_blob(c, pPara);
    } else if (findType != CERT_FIND_ANY) {
        wx86_set_lasterr(c, ERROR_CALL_NOT_IMPLEMENTED);
        observe("CertFindCertificateInStore(%s) type 0x%x -> NULL (%s)", "-", findType, "not implemented");
        return 0;
    }
    for (size_t i = start; i < sd->certs.size(); i++) {
        const ac::Certificate& ce = sd->certs[i];
        bool ok = true;
        if (findType == CERT_FIND_SUBJECT_CERT) {
            ok = ce.issuer.len == issuer.size() && std::memcmp(ce.p(ce.issuer), issuer.data(), issuer.size()) == 0 &&
                 blob_eq_int_le(reversed(ce.p(ce.serial), ce.serial.len), serial);
        } else if (findType == CERT_FIND_SHA1_HASH) {
            uint8_t d[20]; ac::sha1(ce.der.data(), ce.der.size(), d);
            ok = hash.size() == 20 && std::memcmp(d, hash.data(), 20) == 0;
        }
        if (!ok) continue;
        uint32_t p = context_for(c, ce, hs);
        if (!p) { wx86_set_lasterr(c, 8 /* ERROR_NOT_ENOUGH_MEMORY */); return 0; }
        return p;
    }
    wx86_set_lasterr(c, CRYPT_E_NOT_FOUND);
    return 0;
}

uint32_t CertGetNameStringW(Cpu& c) {
    uint32_t pCtx = c.arg(0), type = c.arg(1), flags = c.arg(2), para = c.arg(3), psz = c.arg(4), cch = c.arg(5);
    std::u16string name;
    if (pCtx) {
        uint32_t pb = c.read_u32(pCtx + 4), cb = c.read_u32(pCtx + 8);
        std::vector<uint8_t> der;
        if (pb && cb && cb < (1u << 24)) { der.resize(cb); c.read(pb, der.data(), cb); }
        ac::Certificate ce;
        if (!der.empty() && ce.parse(der.data(), der.size())) {
            const ac::Span& nm = (flags & CERT_NAME_ISSUER_FLAG) ? ce.issuer : ce.subject;
            if (type == CERT_NAME_ATTR_TYPE) {
                std::string oid = gread_cstr(c, para);
                if (!oid.empty()) ac::name_find_attr(ce.p(nm), nm.len, oid, name);
            } else if (type == CERT_NAME_SIMPLE_DISPLAY_TYPE || type == CERT_NAME_FRIENDLY_DISPLAY_TYPE) {
                static const char* order[] = { "2.5.4.3", "2.5.4.11", "2.5.4.10", "1.2.840.113549.1.9.1" };
                for (const char* o : order) if (ac::name_find_attr(ce.p(nm), nm.len, o, name)) break;
            } else {
                observe("CertGetNameStringW(%s) type %u -> empty (%s)", "-", type, "not implemented");
            }
        }
    }
    uint32_t len = (uint32_t)name.size();
    if (!psz || !cch) return len + 1;
    uint32_t w = len < cch - 1 ? len : cch - 1;
    for (uint32_t i = 0; i < w; i++) { uint16_t ch = (uint16_t)name[i]; c.write(psz + 2 * i, &ch, 2); }
    uint16_t z = 0; c.write(psz + 2 * w, &z, 2);
    return w + 1;
}

} // namespace

void wx86_wintrust_set_file_source(Wx86WintrustFileSource src) { std::lock_guard<std::mutex> lk(g_mu); g_src = std::move(src); }
void wx86_wintrust_set_clock(int64_t (*now)()) { g_clock = now; }
void wx86_wintrust_set_observer(Wx86WintrustObserverFn cb) { g_obs = cb; }

void wx86_wintrust_register(const std::function<void(const char*, const char*, const Shim&)>& reg) {
    auto R = [&](const char* dll, const char* name, uint32_t argc, uint32_t (*fn)(Cpu&)) {
        Shim s; s.argc = argc; s.stdcall_cleanup = true; s.tag = std::string(dll) + "!" + name; s.fn = fn;
        reg(dll, name, s);
    };
    R("WINTRUST.dll", "WinVerifyTrust", 3, WinVerifyTrust);
    R("CRYPT32.dll", "CryptQueryObject", 11, CryptQueryObject);
    R("CRYPT32.dll", "CryptMsgGetParam", 5, CryptMsgGetParam);
    R("CRYPT32.dll", "CryptMsgClose", 1, CryptMsgClose);
    R("CRYPT32.dll", "CertFindCertificateInStore", 6, CertFindCertificateInStore);
    R("CRYPT32.dll", "CertGetNameStringW", 6, CertGetNameStringW);
    R("CRYPT32.dll", "CertFreeCertificateContext", 1, CertFreeCertificateContext);
    R("CRYPT32.dll", "CertCloseStore", 2, CertCloseStore);
}

void win32_shims_wintrust_install(Bridge& br) {
    wx86_wintrust_register([&br](const char* dll, const char* name, const Shim& s) { br.register_shim(dll, name, s); });
}
