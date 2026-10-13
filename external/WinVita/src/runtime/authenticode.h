// src/runtime/authenticode.h — real Authenticode verification of a PE file.
//
// WHY THIS BELONGS IN THE ENGINE. A Win32 program asking "is this file
// signed, and by whom?" (WinVerifyTrust, CryptQueryObject...) expects the
// SYSTEM to compute a real answer. There is no system here: the engine
// stands in for it. A constant answer ("yes, signed by X") would be
// FABRICATED; this file replaces it with the same computation Windows
// performs, which FAILS the same way Windows does when the file or its
// signature has been tampered with.
//
// WHAT IS VERIFIED (in this order, see verify_pe):
//   1. the PE security directory (WIN_CERTIFICATE, revision 2.0, type
//      PKCS_SIGNED_DATA);
//   2. DER decoding of the PKCS#7 SignedData, SpcIndirectDataContent content
//      (1.3.6.1.4.1.311.2.1.4), one signer;
//   3. the signer's authenticated attributes: contentType, then
//      messageDigest == H(signed content);
//   4. the RSA PKCS#1 v1.5 signature over the authenticated attributes,
//      using the signer certificate's key (found by issuer + serial
//      number);
//   5. the PE's Authenticode digest (SHA-1 or SHA-256, per
//      SpcIndirectDataContent) — checksum, security-directory entry and
//      certificate table EXCLUDED — compared against the signed digest;
//   6. the timestamp, if present: PKCS#9 countersignature
//      (1.2.840.113549.1.9.6) or Microsoft RFC 3161 token
//      (1.3.6.1.4.1.311.3.3.1); its digest must cover the signer's
//      encrypted signature, and its own RSA signature must verify;
//   7. both the signer's chain and the timestamp authority's chain, every
//      certificate signature verified, up to a TRUSTED root (embedded or
//      added by the consumer), each certificate checked valid AT THE
//      EVALUATION DATE: the timestamp date if there is one, otherwise the
//      current date (Windows Authenticode policy, outside "lifetime
//      signing");
//   8. usage: Code Signing (1.3.6.1.5.5.7.3.3) for the signer leaf, Time
//      Stamping (1.3.6.1.5.5.7.3.8) for the timestamp leaf, when the EKU
//      extension is present; basicConstraints CA for intermediates.
//
// WHAT IS NOT DONE, AND WHAT IS RETURNED INSTEAD:
//   * Revocation (CRL/OCSP): no network access here. If the caller doesn't
//     request revocation checking (WTD_REVOKE_NONE or
//     WTD_REVOCATION_CHECK_NONE), there is nothing to do. If it does, real
//     offline Windows gets an "unknown/offline" revocation status that the
//     default Authenticode policy (State\PolicyFlags = 0x23C00:
//     WTPF_OFFLINEOK_IND|_COM and WTPF_OFFLINEOKNBU_IND|_COM) ACCEPTS: so
//     the same success is returned here, and the report says so
//     (VerifyReport::revocation_unchecked). strict_revocation instead
//     returns CERT_E_REVOCATION_FAILURE (0x800B010E), matching Windows with
//     those policy bits cleared.
//   * "Disallowed" store, automatic root-update CTL, name constraints, root
//     revocation list: not implemented.
//   * Nested signatures (1.3.6.1.4.1.311.2.4.1): ignored, same as
//     WinVerifyTrust without WSS_VERIFY_SEALING / without walking secondary
//     signatures (only the primary signature decides).
//
// NO DEPENDENCIES: SHA-1, SHA-256, DER, X.509 and RSA modular exponentiation
// are all implemented here (target is PS Vita / newlib). No %zu: the
// bundled snprintf doesn't support it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wx86 { namespace ac {

// ---- Windows codes (HRESULT) returned by verification ----
enum : uint32_t {
    AC_OK                        = 0x00000000u,
    TRUST_E_PROVIDER_UNKNOWN     = 0x800B0001u,
    TRUST_E_ACTION_UNKNOWN       = 0x800B0002u,
    TRUST_E_SUBJECT_FORM_UNKNOWN = 0x800B0003u,
    TRUST_E_NOSIGNATURE          = 0x800B0100u,
    CERT_E_EXPIRED               = 0x800B0101u,
    CERT_E_UNTRUSTEDROOT         = 0x800B0109u,
    CERT_E_CHAINING              = 0x800B010Au,
    CERT_E_REVOCATION_FAILURE    = 0x800B010Eu,
    CERT_E_WRONG_USAGE           = 0x800B0110u,
    TRUST_E_NO_SIGNER_CERT       = 0x80096002u,
    TRUST_E_CERT_SIGNATURE       = 0x80096004u,
    TRUST_E_TIME_STAMP           = 0x80096005u,
    TRUST_E_BAD_DIGEST           = 0x80096010u,
    NTE_BAD_SIGNATURE            = 0x80090006u,
    NTE_BAD_ALGID                = 0x80090008u,
    CRYPT_E_HASH_VALUE           = 0x80091007u,
    CRYPT_E_UNEXPECTED_MSG_TYPE  = 0x8009200Au,
    CRYPT_E_NO_MATCH             = 0x80092009u,
    CRYPT_E_ASN1_BADTAG          = 0x8009310Bu,
    CRYPT_E_ASN1_EOD             = 0x80093102u,
    CRYPT_E_ASN1_CORRUPT         = 0x80093103u,
};

// ---- Digests ----
enum HashAlg { HASH_NONE = 0, HASH_SHA1 = 1, HASH_SHA256 = 2 };
size_t hash_len(HashAlg a);                     // 20, 32, 0

struct Sha1   { uint32_t h[5]; uint64_t n; uint8_t buf[64]; size_t fill;
                void init(); void update(const uint8_t* p, size_t len); void final(uint8_t out[20]); };
struct Sha256 { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t fill;
                void init(); void update(const uint8_t* p, size_t len); void final(uint8_t out[32]); };
struct Hasher {
    HashAlg alg = HASH_NONE; Sha1 s1; Sha256 s2;
    void init(HashAlg a);
    void update(const uint8_t* p, size_t len);
    size_t final(uint8_t* out);                  // returns the number of bytes written
};
void sha1(const uint8_t* p, size_t n, uint8_t out[20]);
void sha256(const uint8_t* p, size_t n, uint8_t out[32]);

// ---- RSA ----
// Raw public-key operation: out = sig^e mod n, nlen bytes (big-endian).
// Returns false if sig >= n, n is even, or sizes are out of bounds (n <= 8192 bits).
bool rsa_public(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                const uint8_t* sig, size_t siglen, std::vector<uint8_t>& out);
// PKCS#1 v1.5 verification (EMSA-PKCS1-v1_5, full DigestInfo required).
bool rsa_pkcs1v15_verify(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                         const uint8_t* sig, size_t siglen,
                         HashAlg alg, const uint8_t* digest, size_t dlen);

// ---- DER ----
// Slice of a buffer: offset + length. Never a pointer, so a structure stays
// valid when its owning buffer is copied.
struct Span { size_t off = 0, len = 0; bool empty() const { return len == 0; } };

struct Tlv {
    uint8_t tag = 0;
    Span    all;    // header + value
    Span    val;    // value only
};
// Reads a DER TLV at buf[pos], bounded by end. Definite forms only.
bool der_read(const uint8_t* buf, size_t pos, size_t end, Tlv& t);
std::string oid_to_string(const uint8_t* p, size_t n);   // "1.2.840..."; "" if malformed

// ---- X.509 ----
struct Extension { std::string oid; bool critical = false; Span value; };  // value = OCTET STRING content

struct Certificate {
    std::vector<uint8_t> der;
    Span tbs;                    // full TLV of the TBSCertificate (what is signed)
    uint32_t version = 0;        // 0 = v1, 2 = v3 (INTEGER value)
    Span serial;                 // INTEGER content, big-endian, as encoded
    std::string tbsSigAlgOid;    Span tbsSigAlgParams;   // params = full TLV (or empty)
    Span issuer, subject;        // full TLV of the Name
    int64_t notBefore = 0, notAfter = 0;                 // Unix seconds UTC
    Span spki;                   // full TLV of SubjectPublicKeyInfo
    std::string spkiAlgOid;      Span spkiAlgParams;
    Span pubKey;                 // BIT STRING content, without the unused-bits octet
    uint8_t pubKeyUnusedBits = 0;
    Span rsaN, rsaE;             // INTEGER contents (leading zeros stripped)
    std::vector<Extension> exts;
    std::string sigAlgOid;       // outer AlgorithmIdentifier
    Span sig;                    // signature (BIT STRING without the unused-bits octet)
    bool hasBasicConstraints = false, isCA = false;
    bool hasEku = false;         std::vector<std::string> eku;

    const uint8_t* p(const Span& s) const { return der.data() + s.off; }
    bool parse(const uint8_t* d, size_t n);   // copies the bytes
};

// First `oid` attribute of a Name (full TLV) converted to UTF-16; false if absent.
bool name_find_attr(const uint8_t* name, size_t n, const std::string& oid, std::u16string& out);
// A DirectoryString attribute value (Printable/IA5/UTF8/BMP/T61...) -> UTF-16.
bool directory_string_to_u16(const uint8_t* tlv, size_t n, std::u16string& out);

// ---- PKCS#7 ----
struct Attribute { std::string oid; std::vector<Span> values; };   // values = full TLVs

struct SignerInfo {
    uint32_t version = 0;
    Span issuer;                 // full TLV of the Name
    Span serial;                 // INTEGER content, big-endian
    std::string digestAlgOid;    Span digestAlgParams;
    Span authAttrsTlv;           // full [0] IMPLICIT (empty if absent)
    std::vector<Attribute> authAttrs;
    std::string encAlgOid;       Span encAlgParams;
    Span encryptedDigest;        // OCTET STRING content
    std::vector<Attribute> unauthAttrs;
};

struct SignedData {
    std::vector<uint8_t> buf;    // full ContentInfo; all Spans point into it
    uint32_t version = 0;
    std::string contentType;     // eContentType
    Span content;                // full TLV of the inner content (under the [0] EXPLICIT)
    std::vector<Certificate> certs;
    std::vector<SignerInfo> signers;
    const uint8_t* p(const Span& s) const { return buf.data() + s.off; }
    // Decodes a ContentInfo{signedData}. On failure, *err = a CRYPT_E_* code.
    bool parse(const uint8_t* d, size_t n, uint32_t* err);
};
// Decodes a SignerInfo located in `buf` (also used for countersignatures).
bool parse_signer_info(const uint8_t* buf, const Tlv& t, SignerInfo& si);

// ---- PE ----
// Extracts the PKCS#7 from the first WIN_CERTIFICATE PKCS_SIGNED_DATA entry.
// *err: TRUST_E_SUBJECT_FORM_UNKNOWN (not a PE), TRUST_E_NOSIGNATURE (no
// signature), CRYPT_E_ASN1_* (corrupt directory).
bool pe_extract_pkcs7(const std::vector<uint8_t>& file, std::vector<uint8_t>& pkcs7, uint32_t* err);
// Authenticode digest (Microsoft PE/COFF, "Calculating the PE Image Hash").
bool pe_image_digest(const std::vector<uint8_t>& file, HashAlg alg, std::vector<uint8_t>& out, std::string* why);

// ---- Trust store ----
// Embedded roots (authenticode_roots.h) + added roots. Adding a root is
// equivalent to placing it in a Windows "Root" store: it's a DECISION for
// the integrator to document. Returns false if the DER is unreadable.
// Idempotent (adding the same DER twice yields one entry).
bool add_trusted_root(const uint8_t* der, size_t n);
size_t trusted_root_count();

// ---- Verification ----
struct VerifyOptions {
    int64_t now = 0;                   // Unix seconds; 0 = std::time(nullptr)
    bool revocation_requested = false; // fdwRevocationChecks != NONE and not WTD_REVOCATION_CHECK_NONE
    bool strict_revocation = false;    // see header comment: Windows without WTPF_OFFLINEOK_*
    bool hash_only = false;            // WTD_HASH_ONLY_FLAG: image digest only
    bool lifetime_signing = false;     // WTD_LIFETIME_SIGNING_FLAG: chains evaluated at `now` even if timestamped
};
struct VerifyReport {
    uint32_t hr = 0;                   // returned code (= verify_pe's return value)
    uint32_t last_error = 0;           // the GetLastError Windows would set
    std::string detail;                // human-readable phrase (for logging)
    HashAlg image_alg = HASH_NONE;
    bool timestamped = false;
    int64_t timestamp = 0;             // Unix seconds of the timestamp
    int64_t evaluated_at = 0;          // date at which the chains were evaluated
    int signer_cert = -1;              // index into SignedData::certs
    std::vector<std::string> chain;    // subjects (CN) from leaf to root
    std::vector<std::string> ts_chain;
    bool revocation_unchecked = false;
};
// Verifies `file`. Returns the WinVerifyTrust HRESULT (0 = trusted).
uint32_t verify_pe(const std::vector<uint8_t>& file, const VerifyOptions& opt, VerifyReport& rep);

// DER time (UTCTime 0x17 / GeneralizedTime 0x18) -> Unix seconds.
bool der_time(const uint8_t* tlv, size_t n, int64_t& out);

}} // namespace wx86::ac
