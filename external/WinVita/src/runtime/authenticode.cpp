// src/runtime/authenticode.cpp — see authenticode.h.
#include "runtime/authenticode.h"
#include "runtime/authenticode_roots.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>

namespace wx86 { namespace ac {

// =============================================================================
// Digests
// =============================================================================
size_t hash_len(HashAlg a) { return a == HASH_SHA1 ? 20 : a == HASH_SHA256 ? 32 : 0; }

static inline uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void Sha1::init() {
    h[0] = 0x67452301u; h[1] = 0xEFCDAB89u; h[2] = 0x98BADCFEu; h[3] = 0x10325476u; h[4] = 0xC3D2E1F0u;
    n = 0; fill = 0;
}
static void sha1_block(uint32_t* h, const uint8_t* b) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);            k = 0x5A827999u; }
        else if (i < 40) { f = bb ^ c ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = bb ^ c ^ d;                      k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}
void Sha1::update(const uint8_t* p, size_t len) {
    n += len;
    while (len) {
        if (fill == 0 && len >= 64) { sha1_block(h, p); p += 64; len -= 64; continue; }
        size_t k = std::min(len, 64 - fill);
        std::memcpy(buf + fill, p, k); fill += k; p += k; len -= k;
        if (fill == 64) { sha1_block(h, buf); fill = 0; }
    }
}
void Sha1::final(uint8_t out[20]) {
    uint64_t bits = n * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (fill != 56) update(&z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
void Sha256::init() {
    static const uint32_t I[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    std::memcpy(h, I, sizeof I); n = 0; fill = 0;
}
static inline uint32_t ror(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
static void sha256_block(uint32_t* h, const uint8_t* b) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0], bb=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
void Sha256::update(const uint8_t* p, size_t len) {
    n += len;
    while (len) {
        if (fill == 0 && len >= 64) { sha256_block(h, p); p += 64; len -= 64; continue; }
        size_t k = std::min(len, 64 - fill);
        std::memcpy(buf + fill, p, k); fill += k; p += k; len -= k;
        if (fill == 64) { sha256_block(h, buf); fill = 0; }
    }
}
void Sha256::final(uint8_t out[32]) {
    uint64_t bits = n * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (fill != 56) update(&z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(l, 8);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(h[i] >> (24 - 8 * j));
}
void Hasher::init(HashAlg a) { alg = a; if (a == HASH_SHA1) s1.init(); else if (a == HASH_SHA256) s2.init(); }
void Hasher::update(const uint8_t* p, size_t len) {
    if (alg == HASH_SHA1) s1.update(p, len); else if (alg == HASH_SHA256) s2.update(p, len);
}
size_t Hasher::final(uint8_t* out) {
    if (alg == HASH_SHA1) { s1.final(out); return 20; }
    if (alg == HASH_SHA256) { s2.final(out); return 32; }
    return 0;
}
void sha1(const uint8_t* p, size_t n, uint8_t out[20]) { Sha1 s; s.init(); s.update(p, n); s.final(out); }
void sha256(const uint8_t* p, size_t n, uint8_t out[32]) { Sha256 s; s.init(); s.update(p, n); s.final(out); }

// =============================================================================
// RSA: Montgomery exponentiation over 32-bit words
// =============================================================================
namespace {
using Limbs = std::vector<uint32_t>;   // little-endian by word

Limbs from_be(const uint8_t* p, size_t n, size_t k) {
    Limbs r(k, 0);
    for (size_t i = 0; i < n; i++) {
        size_t bit = (n - 1 - i);                // byte index from the least-significant end
        if (bit / 4 < k) r[bit / 4] |= (uint32_t)p[i] << (8 * (bit % 4));
        else if (p[i]) return Limbs();           // doesn't fit
    }
    return r;
}
int cmp(const Limbs& a, const Limbs& b) {
    for (size_t i = a.size(); i-- > 0;) { if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; }
    return 0;
}
void sub_in(Limbs& a, const Limbs& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d; borrow = (d >> 63) & 1;
    }
}
// t = a*b*R^-1 mod n (CIOS)
void mont_mul(const Limbs& a, const Limbs& b, const Limbs& n, uint32_t n0inv, Limbs& out) {
    size_t k = n.size();
    std::vector<uint32_t> t(k + 2, 0);
    for (size_t i = 0; i < k; i++) {
        uint64_t C = 0;
        for (size_t j = 0; j < k; j++) {
            uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + C;
            t[j] = (uint32_t)s; C = s >> 32;
        }
        uint64_t s = (uint64_t)t[k] + C; t[k] = (uint32_t)s; t[k+1] = (uint32_t)(s >> 32);
        uint32_t m = t[0] * n0inv;
        s = (uint64_t)t[0] + (uint64_t)m * n[0]; C = s >> 32;
        for (size_t j = 1; j < k; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * n[j] + C;
            t[j-1] = (uint32_t)s; C = s >> 32;
        }
        s = (uint64_t)t[k] + C; t[k-1] = (uint32_t)s; C = s >> 32;
        t[k] = t[k+1] + (uint32_t)C;
    }
    out.assign(t.begin(), t.begin() + k);
    if (t[k] || cmp(out, n) >= 0) sub_in(out, n);
}
} // namespace

bool rsa_public(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                const uint8_t* sig, size_t siglen, std::vector<uint8_t>& out) {
    while (nlen && !*n) { n++; nlen--; }
    while (elen && !*e) { e++; elen--; }
    while (siglen > nlen && !*sig) { sig++; siglen--; }
    if (nlen < 16 || nlen > 1024 || elen == 0 || elen > 8 || !(n[nlen-1] & 1)) return false;
    size_t k = (nlen + 3) / 4;
    Limbs N = from_be(n, nlen, k), S = from_be(sig, siglen, k);
    if (N.empty() || S.empty() || cmp(S, N) >= 0) return false;
    uint32_t inv = 1;                                        // Newton's method: inv = n0^-1 mod 2^32
    for (int i = 0; i < 5; i++) inv *= 2 - N[0] * inv;
    uint32_t n0inv = (uint32_t)(0u - inv);
    // R mod n and R^2 mod n via repeated doubling.
    Limbs x(k, 0); x[0] = 1;
    Limbs R2;
    for (size_t i = 0; i < 64 * k; i++) {
        uint32_t carry = 0;
        for (size_t j = 0; j < k; j++) { uint32_t v = x[j]; x[j] = (v << 1) | carry; carry = v >> 31; }
        if (carry || cmp(x, N) >= 0) sub_in(x, N);
    }
    R2 = x;
    Limbs one(k, 0); one[0] = 1;
    Limbs base, acc, tmp;
    mont_mul(S, R2, N, n0inv, base);                         // S*R
    mont_mul(one, R2, N, n0inv, acc);                        // R (= 1 in Montgomery form)
    uint64_t E = 0; for (size_t i = 0; i < elen; i++) E = (E << 8) | e[i];
    int top = 63; while (top >= 0 && !((E >> top) & 1)) top--;
    for (int i = top; i >= 0; i--) {
        mont_mul(acc, acc, N, n0inv, tmp); acc.swap(tmp);
        if ((E >> i) & 1) { mont_mul(acc, base, N, n0inv, tmp); acc.swap(tmp); }
    }
    mont_mul(acc, one, N, n0inv, tmp);                       // convert out of Montgomery form
    out.assign(nlen, 0);
    for (size_t i = 0; i < nlen; i++) {
        size_t bit = nlen - 1 - i;
        out[i] = (uint8_t)(tmp[bit / 4] >> (8 * (bit % 4)));
    }
    return true;
}

bool rsa_pkcs1v15_verify(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                         const uint8_t* sig, size_t siglen,
                         HashAlg alg, const uint8_t* digest, size_t dlen) {
    static const uint8_t P1[]  = {0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x05,0x00,0x04,0x14};
    static const uint8_t P1n[] = {0x30,0x1f,0x30,0x07,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x04,0x14};
    static const uint8_t P2[]  = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    static const uint8_t P2n[] = {0x30,0x2f,0x30,0x0b,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x04,0x20};
    if (dlen != hash_len(alg) || dlen == 0) return false;
    std::vector<uint8_t> em;
    if (!rsa_public(n, nlen, e, elen, sig, siglen, em)) return false;
    // em = 00 01 FF..FF 00 DigestInfo. Three forms accepted, matching
    // CryptoAPI's RSA provider (CPVerifySignature retries without the OID):
    // DigestInfo with NULL parameters, without parameters, and a bare digest
    // with no DigestInfo at all -- the latter is how Symantec/VeriSign
    // timestamp countersignatures encode it: 00 01 FF..FF 00 || SHA-1.
    for (int variant = 0; variant < 3; variant++) {
        const uint8_t* pre = variant == 2 ? P1 : alg == HASH_SHA1 ? (variant ? P1n : P1) : (variant ? P2n : P2);
        size_t plen = variant == 2 ? 0 : alg == HASH_SHA1 ? (variant ? sizeof P1n : sizeof P1) : (variant ? sizeof P2n : sizeof P2);
        size_t tlen = plen + dlen;
        if (em.size() < tlen + 11) return false;
        bool ok = em[0] == 0 && em[1] == 1;
        size_t ps = em.size() - tlen - 3;
        for (size_t i = 0; ok && i < ps; i++) ok = em[2 + i] == 0xFF;
        ok = ok && em[2 + ps] == 0;
        ok = ok && std::memcmp(&em[3 + ps], pre, plen) == 0 && std::memcmp(&em[3 + ps + plen], digest, dlen) == 0;
        if (ok) return true;
    }
    return false;
}

// =============================================================================
// DER
// =============================================================================
bool der_read(const uint8_t* buf, size_t pos, size_t end, Tlv& t) {
    if (pos >= end || end - pos < 2) return false;
    uint8_t tag = buf[pos];
    if ((tag & 0x1f) == 0x1f) return false;                  // long tag numbers: absent from Authenticode
    size_t l = buf[pos + 1], h = 2;
    if (l & 0x80) {
        size_t k = l & 0x7f;
        if (k == 0 || k > 4 || end - pos < 2 + k) return false; // 0x80 = indefinite length (BER): rejected
        l = 0;
        for (size_t i = 0; i < k; i++) l = (l << 8) | buf[pos + 2 + i];
        h = 2 + k;
    }
    if (l > end - pos - h) return false;
    t.tag = tag; t.all.off = pos; t.all.len = h + l; t.val.off = pos + h; t.val.len = l;
    return true;
}

// Decimal without snprintf: the target's bundled formatter guarantees
// neither %zu nor %llu.
static void append_u64(std::string& s, uint64_t v) {
    char tmp[24]; int k = 0;
    do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) s += tmp[--k];
}

std::string oid_to_string(const uint8_t* p, size_t n) {
    std::string s; if (!n) return s;
    uint64_t v = 0; bool first = true;
    for (size_t i = 0; i < n; i++) {
        if (v > (UINT64_MAX >> 7)) return std::string();
        v = (v << 7) | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) {
            if (first) {
                unsigned a = v < 40 ? 0 : v < 80 ? 1 : 2;
                append_u64(s, a); s += '.'; append_u64(s, v - 40 * a);
                first = false;
            } else { s += '.'; append_u64(s, v); }
            v = 0;
        } else if (i + 1 == n) return std::string();
    }
    return s;
}

namespace {
// Cursor over a sequence of TLVs.
struct Cur {
    const uint8_t* b; size_t pos, end; bool bad = false;
    Cur(const uint8_t* buf, const Span& s) : b(buf), pos(s.off), end(s.off + s.len) {}
    bool more() const { return pos < end; }
    bool peek(uint8_t& tag) const { if (pos >= end) return false; tag = b[pos]; return true; }
    bool next(Tlv& t) {
        if (pos >= end) return false;
        if (!der_read(b, pos, end, t)) { bad = true; return false; }
        pos = t.all.off + t.all.len; return true;
    }
    bool expect(uint8_t tag, Tlv& t) { return next(t) && t.tag == tag; }
};

bool parse_alg(const uint8_t* b, const Tlv& seq, std::string& oid, Span& params) {
    if (seq.tag != 0x30) return false;
    Cur c(b, seq.val); Tlv o;
    if (!c.expect(0x06, o)) return false;
    oid = oid_to_string(b + o.val.off, o.val.len);
    params = Span();
    Tlv pr; if (c.next(pr)) params = pr.all;
    return !oid.empty() && !c.bad;
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
bool digits(const uint8_t* p, int n, int& out) {
    out = 0;
    for (int i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return false; out = out * 10 + (p[i] - '0'); }
    return true;
}
} // namespace

bool der_time(const uint8_t* tlv, size_t n, int64_t& out) {
    Tlv t; if (!der_read(tlv, 0, n, t)) return false;
    const uint8_t* p = tlv + t.val.off; size_t l = t.val.len;
    int Y, M, D, h, mi, s;
    if (t.tag == 0x17) {                                    // UTCTime YYMMDDHHMMSSZ
        if (l != 13 || p[12] != 'Z') return false;
        if (!digits(p, 2, Y)) return false;
        Y += Y < 50 ? 2000 : 1900; p += 2;
    } else if (t.tag == 0x18) {                             // GeneralizedTime YYYYMMDDHHMMSS[.f*]Z
        if (l < 15 || p[l-1] != 'Z') return false;
        if (!digits(p, 4, Y)) return false;
        p += 4;
        if (l > 15) {                                       // fractional seconds: ignored, but must be well-formed
            if (p[10] != '.' || l < 17) return false;
            for (size_t i = 15; i + 1 < l; i++) if (tlv[t.val.off + i] < '0' || tlv[t.val.off + i] > '9') return false;
        }
    } else return false;
    if (!digits(p, 2, M) || !digits(p + 2, 2, D) || !digits(p + 4, 2, h) || !digits(p + 6, 2, mi) || !digits(p + 8, 2, s)) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || s > 60) return false;
    out = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

// =============================================================================
// DirectoryString and Name strings
// =============================================================================
bool directory_string_to_u16(const uint8_t* b, size_t n, std::u16string& out) {
    Tlv t; if (!der_read(b, 0, n, t)) return false;
    const uint8_t* p = b + t.val.off; size_t l = t.val.len;
    out.clear();
    switch (t.tag) {
    case 0x13: case 0x16: case 0x14: case 0x1a: case 0x12:  // Printable, IA5, T61 (treated as Latin-1), Visible, Numeric
        for (size_t i = 0; i < l; i++) out.push_back((char16_t)p[i]);
        return true;
    case 0x1e:                                              // BMPString (UTF-16BE)
        if (l % 2) return false;
        for (size_t i = 0; i < l; i += 2) out.push_back((char16_t)((p[i] << 8) | p[i+1]));
        return true;
    case 0x0c: {                                            // UTF8String
        for (size_t i = 0; i < l;) {
            uint32_t cp; uint8_t c = p[i];
            size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (!k || i + k > l) return false;
            cp = k == 1 ? c : k == 2 ? (c & 0x1f) : k == 3 ? (c & 0x0f) : (c & 0x07);
            for (size_t j = 1; j < k; j++) { if ((p[i+j] & 0xc0) != 0x80) return false; cp = (cp << 6) | (p[i+j] & 0x3f); }
            if (cp >= 0x10000) { cp -= 0x10000; out.push_back((char16_t)(0xD800 + (cp >> 10))); out.push_back((char16_t)(0xDC00 + (cp & 0x3ff))); }
            else out.push_back((char16_t)cp);
            i += k;
        }
        return true; }
    case 0x1c:                                              // UniversalString (UCS-4BE)
        if (l % 4) return false;
        for (size_t i = 0; i < l; i += 4) {
            uint32_t cp = be32(p + i);
            if (cp >= 0x10000) { cp -= 0x10000; out.push_back((char16_t)(0xD800 + (cp >> 10))); out.push_back((char16_t)(0xDC00 + (cp & 0x3ff))); }
            else out.push_back((char16_t)cp);
        }
        return true;
    }
    return false;
}

bool name_find_attr(const uint8_t* name, size_t n, const std::string& oid, std::u16string& out) {
    Tlv nm; if (!der_read(name, 0, n, nm) || nm.tag != 0x30) return false;
    Cur rdns(name, nm.val); Tlv set;
    while (rdns.next(set)) {
        if (set.tag != 0x31) return false;
        Cur atvs(name, set.val); Tlv atv;
        while (atvs.next(atv)) {
            if (atv.tag != 0x30) return false;
            Cur c(name, atv.val); Tlv o, v;
            if (!c.expect(0x06, o) || !c.next(v)) return false;
            if (oid_to_string(name + o.val.off, o.val.len) == oid)
                return directory_string_to_u16(name + v.all.off, v.all.len, out);
        }
    }
    return false;
}

// =============================================================================
// X.509
// =============================================================================
bool Certificate::parse(const uint8_t* d, size_t n) {
    der.assign(d, d + n);
    const uint8_t* b = der.data();
    Tlv top; if (!der_read(b, 0, n, top) || top.tag != 0x30) return false;
    Cur c(b, top.val); Tlv t, sa, sg;
    if (!c.expect(0x30, t) || !c.next(sa) || !c.expect(0x03, sg)) return false;
    tbs = t.all;
    Span dummy;
    if (!parse_alg(b, sa, sigAlgOid, dummy)) return false;
    if (sg.val.len < 1 || b[sg.val.off] != 0) return false;
    sig.off = sg.val.off + 1; sig.len = sg.val.len - 1;

    Cur tc(b, t.val); Tlv x;
    uint8_t tag;
    version = 0;
    if (tc.peek(tag) && tag == 0xA0) {
        tc.next(x); Tlv vi;
        if (!der_read(b, x.val.off, x.val.off + x.val.len, vi) || vi.tag != 0x02 || vi.val.len != 1) return false;
        version = b[vi.val.off];
    }
    if (!tc.expect(0x02, x)) return false;
    serial = x.val;
    if (!tc.next(x) || !parse_alg(b, x, tbsSigAlgOid, tbsSigAlgParams)) return false;
    if (!tc.expect(0x30, x)) return false;
    issuer = x.all;
    if (!tc.expect(0x30, x)) return false;
    { Cur vc(b, x.val); Tlv a, z;
      if (!vc.next(a) || !vc.next(z)) return false;
      if (!der_time(b + a.all.off, a.all.len, notBefore) || !der_time(b + z.all.off, z.all.len, notAfter)) return false; }
    if (!tc.expect(0x30, x)) return false;
    subject = x.all;
    if (!tc.expect(0x30, x)) return false;
    spki = x.all;
    { Cur kc(b, x.val); Tlv alg, bits;
      if (!kc.next(alg) || !parse_alg(b, alg, spkiAlgOid, spkiAlgParams) || !kc.expect(0x03, bits) || bits.val.len < 1) return false;
      pubKeyUnusedBits = b[bits.val.off];
      pubKey.off = bits.val.off + 1; pubKey.len = bits.val.len - 1;
      if (spkiAlgOid == "1.2.840.113549.1.1.1") {
          Tlv rk, in, ie;
          if (!der_read(b, pubKey.off, pubKey.off + pubKey.len, rk) || rk.tag != 0x30) return false;
          Cur rc(b, rk.val);
          if (!rc.expect(0x02, in) || !rc.expect(0x02, ie)) return false;
          rsaN = in.val; while (rsaN.len && !b[rsaN.off]) { rsaN.off++; rsaN.len--; }
          rsaE = ie.val; while (rsaE.len && !b[rsaE.off]) { rsaE.off++; rsaE.len--; }
      } }
    while (tc.next(x)) {
        if (x.tag == 0xA3) {
            Tlv seq; if (!der_read(b, x.val.off, x.val.off + x.val.len, seq) || seq.tag != 0x30) return false;
            Cur ec(b, seq.val); Tlv ext;
            while (ec.next(ext)) {
                if (ext.tag != 0x30) return false;
                Cur xc(b, ext.val); Tlv o, f;
                if (!xc.expect(0x06, o)) return false;
                Extension e; e.oid = oid_to_string(b + o.val.off, o.val.len);
                if (!xc.next(f)) return false;
                if (f.tag == 0x01) { e.critical = f.val.len == 1 && b[f.val.off] != 0; if (!xc.next(f)) return false; }
                if (f.tag != 0x04) return false;
                e.value = f.val;
                if (e.oid == "2.5.29.19") {
                    hasBasicConstraints = true;
                    Tlv bc; if (der_read(b, e.value.off, e.value.off + e.value.len, bc) && bc.tag == 0x30) {
                        Cur cc(b, bc.val); Tlv ca;
                        if (cc.next(ca) && ca.tag == 0x01 && ca.val.len == 1) isCA = b[ca.val.off] != 0;
                    }
                } else if (e.oid == "2.5.29.37") {
                    hasEku = true;
                    Tlv sq; if (der_read(b, e.value.off, e.value.off + e.value.len, sq) && sq.tag == 0x30) {
                        Cur uc(b, sq.val); Tlv u;
                        while (uc.next(u)) if (u.tag == 0x06) eku.push_back(oid_to_string(b + u.val.off, u.val.len));
                    }
                }
                exts.push_back(e);
            }
            if (ec.bad) return false;
        }
    }
    return !tc.bad && !c.bad;
}

// =============================================================================
// PKCS#7
// =============================================================================
static bool parse_attrs(const uint8_t* b, const Span& s, std::vector<Attribute>& out) {
    Cur c(b, s); Tlv a;
    while (c.next(a)) {
        if (a.tag != 0x30) return false;
        Cur ac(b, a.val); Tlv o, set;
        if (!ac.expect(0x06, o) || !ac.expect(0x31, set)) return false;
        Attribute at; at.oid = oid_to_string(b + o.val.off, o.val.len);
        Cur vc(b, set.val); Tlv v;
        while (vc.next(v)) at.values.push_back(v.all);
        if (vc.bad) return false;
        out.push_back(at);
    }
    return !c.bad;
}

bool parse_signer_info(const uint8_t* b, const Tlv& t, SignerInfo& si) {
    if (t.tag != 0x30) return false;
    Cur c(b, t.val); Tlv x;
    if (!c.expect(0x02, x) || x.val.len != 1) return false;
    si.version = b[x.val.off];
    if (!c.expect(0x30, x)) return false;                   // issuerAndSerialNumber (v1)
    { Cur ic(b, x.val); Tlv nm, sn;
      if (!ic.expect(0x30, nm) || !ic.expect(0x02, sn)) return false;
      si.issuer = nm.all; si.serial = sn.val; }
    if (!c.next(x) || !parse_alg(b, x, si.digestAlgOid, si.digestAlgParams)) return false;
    uint8_t tag;
    if (c.peek(tag) && tag == 0xA0) {
        c.next(x); si.authAttrsTlv = x.all;
        if (!parse_attrs(b, x.val, si.authAttrs)) return false;
    }
    if (!c.next(x) || !parse_alg(b, x, si.encAlgOid, si.encAlgParams)) return false;
    if (!c.expect(0x04, x)) return false;
    si.encryptedDigest = x.val;
    if (c.peek(tag) && tag == 0xA1) {
        c.next(x);
        if (!parse_attrs(b, x.val, si.unauthAttrs)) return false;
    }
    return !c.bad;
}

bool SignedData::parse(const uint8_t* d, size_t n, uint32_t* err) {
    auto fail = [&](uint32_t e) { if (err) *err = e; return false; };
    buf.assign(d, d + n);
    const uint8_t* b = buf.data();
    Tlv ci; if (!der_read(b, 0, n, ci)) return fail(CRYPT_E_ASN1_EOD);
    if (ci.tag != 0x30) return fail(CRYPT_E_ASN1_BADTAG);
    Cur c(b, ci.val); Tlv o, ex;
    if (!c.expect(0x06, o)) return fail(CRYPT_E_ASN1_BADTAG);
    if (oid_to_string(b + o.val.off, o.val.len) != "1.2.840.113549.1.7.2") return fail(CRYPT_E_UNEXPECTED_MSG_TYPE);
    if (!c.expect(0xA0, ex)) return fail(CRYPT_E_ASN1_BADTAG);
    Tlv sd; if (!der_read(b, ex.val.off, ex.val.off + ex.val.len, sd) || sd.tag != 0x30) return fail(CRYPT_E_ASN1_BADTAG);
    Cur s(b, sd.val); Tlv x;
    if (!s.expect(0x02, x) || x.val.len != 1) return fail(CRYPT_E_ASN1_CORRUPT);
    version = b[x.val.off];
    if (!s.expect(0x31, x)) return fail(CRYPT_E_ASN1_BADTAG);            // digestAlgorithms
    if (!s.expect(0x30, x)) return fail(CRYPT_E_ASN1_BADTAG);            // contentInfo
    { Cur cc(b, x.val); Tlv ct, cx;
      if (!cc.expect(0x06, ct)) return fail(CRYPT_E_ASN1_BADTAG);
      contentType = oid_to_string(b + ct.val.off, ct.val.len);
      if (cc.next(cx)) {
          if (cx.tag != 0xA0) return fail(CRYPT_E_ASN1_BADTAG);
          Tlv inner; if (!der_read(b, cx.val.off, cx.val.off + cx.val.len, inner)) return fail(CRYPT_E_ASN1_CORRUPT);
          content = inner.all;
      } }
    uint8_t tag;
    if (s.peek(tag) && tag == 0xA0) {
        s.next(x); Cur cc(b, x.val); Tlv cert;
        while (cc.next(cert)) {
            if (cert.tag != 0x30) continue;                  // other CertificateChoices: ignored
            Certificate ce;
            if (!ce.parse(b + cert.all.off, cert.all.len)) return fail(CRYPT_E_ASN1_CORRUPT);
            certs.push_back(std::move(ce));
        }
        if (cc.bad) return fail(CRYPT_E_ASN1_CORRUPT);
    }
    if (s.peek(tag) && tag == 0xA1) s.next(x);               // crls: ignored
    if (!s.expect(0x31, x)) return fail(CRYPT_E_ASN1_BADTAG);
    { Cur sc(b, x.val); Tlv si;
      while (sc.next(si)) {
          SignerInfo inf;
          if (!parse_signer_info(b, si, inf)) return fail(CRYPT_E_ASN1_CORRUPT);
          signers.push_back(std::move(inf));
      }
      if (sc.bad) return fail(CRYPT_E_ASN1_CORRUPT); }
    if (s.bad || c.bad) return fail(CRYPT_E_ASN1_CORRUPT);
    return true;
}

// =============================================================================
// PE
// =============================================================================
namespace {
inline uint32_t le32(const std::vector<uint8_t>& f, size_t o) {
    return (uint32_t)f[o] | ((uint32_t)f[o+1] << 8) | ((uint32_t)f[o+2] << 16) | ((uint32_t)f[o+3] << 24);
}
inline uint16_t le16(const std::vector<uint8_t>& f, size_t o) { return (uint16_t)(f[o] | (f[o+1] << 8)); }

struct PeLayout {
    size_t opt = 0, checksum = 0, secEntry = 0, sectab = 0;
    uint32_t sizeOfHeaders = 0, secOff = 0, secSize = 0; uint16_t nsec = 0;
    bool hasSecEntry = false;
};
// Returns 0, or TRUST_E_SUBJECT_FORM_UNKNOWN if this isn't a readable PE.
uint32_t pe_layout(const std::vector<uint8_t>& f, PeLayout& L) {
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return TRUST_E_SUBJECT_FORM_UNKNOWN;
    size_t pe = le32(f, 0x3c);
    if (pe > f.size() || f.size() - pe < 24 + 2 || le32(f, pe) != 0x00004550u) return TRUST_E_SUBJECT_FORM_UNKNOWN;
    L.nsec = le16(f, pe + 6);
    uint16_t optsz = le16(f, pe + 20);
    L.opt = pe + 24;
    if (f.size() < L.opt + optsz || optsz < 2) return TRUST_E_SUBJECT_FORM_UNKNOWN;
    uint16_t magic = le16(f, L.opt);
    size_t nrvaOff, dd;
    if (magic == 0x10b) { nrvaOff = 92; dd = 96; } else if (magic == 0x20b) { nrvaOff = 108; dd = 112; }
    else return TRUST_E_SUBJECT_FORM_UNKNOWN;
    if (optsz < dd) return TRUST_E_SUBJECT_FORM_UNKNOWN;
    L.checksum = L.opt + 64;
    L.sizeOfHeaders = le32(f, L.opt + 60);
    uint32_t nrva = le32(f, L.opt + nrvaOff);
    L.secEntry = L.opt + dd + 4 * 8;
    L.hasSecEntry = nrva > 4 && L.secEntry + 8 <= L.opt + optsz;
    if (L.hasSecEntry) { L.secOff = le32(f, L.secEntry); L.secSize = le32(f, L.secEntry + 4); }
    L.sectab = L.opt + optsz;
    if (f.size() < L.sectab + 40u * L.nsec) return TRUST_E_SUBJECT_FORM_UNKNOWN;
    return 0;
}
} // namespace

bool pe_extract_pkcs7(const std::vector<uint8_t>& f, std::vector<uint8_t>& out, uint32_t* err) {
    PeLayout L; uint32_t e = pe_layout(f, L);
    if (e) { if (err) *err = e; return false; }
    if (!L.hasSecEntry || L.secOff == 0 || L.secSize == 0) { if (err) *err = TRUST_E_NOSIGNATURE; return false; }
    if ((uint64_t)L.secOff + L.secSize > f.size() || L.secSize < 8) { if (err) *err = CRYPT_E_ASN1_EOD; return false; }
    size_t pos = L.secOff, end = (size_t)L.secOff + L.secSize;
    while (end - pos >= 8) {
        uint32_t len = le32(f, pos); uint16_t rev = le16(f, pos + 4), type = le16(f, pos + 6);
        if (len < 8 || len > end - pos) { if (err) *err = CRYPT_E_ASN1_EOD; return false; }
        if (rev == 0x0200 && type == 0x0002) {
            out.assign(f.begin() + pos + 8, f.begin() + pos + len);
            // WIN_CERTIFICATE is 8-byte aligned: the encoding may be followed by zero padding.
            Tlv t; if (der_read(out.data(), 0, out.size(), t)) out.resize(t.all.len);
            return true;
        }
        pos += (len + 7) & ~7u;
    }
    if (err) *err = TRUST_E_NOSIGNATURE;
    return false;
}

bool pe_image_digest(const std::vector<uint8_t>& f, HashAlg alg, std::vector<uint8_t>& out, std::string* why) {
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    PeLayout L; if (pe_layout(f, L)) return no("not a PE");
    if (!hash_len(alg)) return no("hash algorithm");
    if (!L.hasSecEntry) return no("no security directory entry");
    if (L.sizeOfHeaders > f.size() || L.sizeOfHeaders < L.secEntry + 8) return no("SizeOfHeaders");
    Hasher h; h.init(alg);
    h.update(f.data(), L.checksum);
    h.update(f.data() + L.checksum + 4, L.secEntry - (L.checksum + 4));
    h.update(f.data() + L.secEntry + 8, L.sizeOfHeaders - (L.secEntry + 8));
    struct Sec { uint32_t ptr, size; };
    std::vector<Sec> secs;
    for (uint16_t i = 0; i < L.nsec; i++) {
        size_t s = L.sectab + 40u * i;
        Sec x{ le32(f, s + 20), le32(f, s + 16) };
        if (x.size) secs.push_back(x);
    }
    std::sort(secs.begin(), secs.end(), [](const Sec& a, const Sec& b) { return a.ptr < b.ptr; });
    size_t hashedEnd = L.sizeOfHeaders;
    for (const Sec& s : secs) {
        if ((uint64_t)s.ptr + s.size > f.size()) return no("section beyond end of file");
        h.update(f.data() + s.ptr, s.size);
        hashedEnd = std::max<size_t>(hashedEnd, (size_t)s.ptr + s.size);
    }
    // Data beyond the last section, certificate table excluded.
    size_t certOff = L.secSize ? L.secOff : f.size(), certEnd = L.secSize ? (size_t)L.secOff + L.secSize : f.size();
    if (certEnd > f.size()) return no("certificate table beyond end of file");
    if (hashedEnd < certOff) h.update(f.data() + hashedEnd, certOff - hashedEnd);
    if (certEnd < f.size() && certEnd > hashedEnd) h.update(f.data() + certEnd, f.size() - certEnd);
    out.resize(hash_len(alg));
    h.final(out.data());
    return true;
}

// =============================================================================
// Trust store
// =============================================================================
namespace {
std::mutex g_rootsMu;
std::vector<roots::RootProps>& root_props_locked();   // parallel to roots_locked(), same index
std::vector<Certificate>& roots_locked() {
    static std::vector<Certificate> r;
    static bool init = false;
    if (!init) {
        init = true;
        for (const auto& e : roots::kEmbedded) {
            Certificate c;
            if (c.parse(e.der, e.len)) { r.push_back(std::move(c)); root_props_locked().push_back(e.props); }
        }
    }
    return r;
}
std::vector<roots::RootProps>& root_props_locked() { static std::vector<roots::RootProps> p; return p; }
} // namespace

bool add_trusted_root(const uint8_t* der, size_t n) {
    Certificate c; if (!c.parse(der, n)) return false;
    std::lock_guard<std::mutex> lk(g_rootsMu);
    auto& r = roots_locked();
    for (auto& x : r) if (x.der == c.der) return true;
    r.push_back(std::move(c));
    root_props_locked().push_back(roots::RootProps{ nullptr, 0, 0 });   // added root: no restriction
    return true;
}
size_t trusted_root_count() { std::lock_guard<std::mutex> lk(g_rootsMu); return roots_locked().size(); }

// =============================================================================
// Verification
// =============================================================================
namespace {
const char* OID_SPC_INDIRECT = "1.3.6.1.4.1.311.2.1.4";
const char* OID_CONTENT_TYPE = "1.2.840.113549.1.9.3";
const char* OID_MESSAGE_DIGEST = "1.2.840.113549.1.9.4";
const char* OID_SIGNING_TIME = "1.2.840.113549.1.9.5";
const char* OID_COUNTERSIG = "1.2.840.113549.1.9.6";
const char* OID_MS_RFC3161 = "1.3.6.1.4.1.311.3.3.1";
const char* OID_TSTINFO = "1.2.840.113549.1.9.16.1.4";
const char* OID_KP_CODESIGN = "1.3.6.1.5.5.7.3.3";
const char* OID_KP_TIMESTAMP = "1.3.6.1.5.5.7.3.8";

enum : uint32_t {                                    // CERT_TRUST_* (wincrypt.h)
    T_NOT_TIME_VALID = 0x1, T_NOT_SIG_VALID = 0x8, T_NOT_VALID_FOR_USAGE = 0x10,
    T_UNTRUSTED_ROOT = 0x20, T_INVALID_BASIC_CONSTRAINTS = 0x400, T_PARTIAL_CHAIN = 0x10000,
};
const uint32_t TRUST_E_BASIC_CONSTRAINTS = 0x80096019u;

HashAlg digest_alg(const std::string& oid) {
    if (oid == "1.3.14.3.2.26") return HASH_SHA1;
    if (oid == "2.16.840.1.101.3.4.2.1") return HASH_SHA256;
    return HASH_NONE;
}
HashAlg cert_sig_hash(const std::string& oid) {
    if (oid == "1.2.840.113549.1.1.5" || oid == "1.3.14.3.2.29") return HASH_SHA1;
    if (oid == "1.2.840.113549.1.1.11") return HASH_SHA256;
    return HASH_NONE;
}
std::string cn_of(const Certificate& c) {
    std::u16string w; std::string s;
    if (!name_find_attr(c.p(c.subject), c.subject.len, "2.5.4.3", w) &&
        !name_find_attr(c.p(c.subject), c.subject.len, "2.5.4.10", w)) return "?";
    for (char16_t ch : w) s += ch < 128 ? (char)ch : '?';
    return s;
}
bool same_span(const Certificate& a, const Span& sa, const uint8_t* bp, size_t bl) {
    return sa.len == bl && std::memcmp(a.p(sa), bp, bl) == 0;
}
// Serial number: integer comparison (leading zeros ignored).
bool same_serial(const uint8_t* a, size_t al, const uint8_t* b, size_t bl) {
    while (al > 1 && !*a) { a++; al--; }
    while (bl > 1 && !*b) { b++; bl--; }
    return al == bl && std::memcmp(a, b, al) == 0;
}
bool cert_signed_by(const Certificate& child, const Certificate& issuer) {
    HashAlg a = cert_sig_hash(child.sigAlgOid);
    if (!a || issuer.rsaN.empty()) return false;
    uint8_t d[32]; Hasher h; h.init(a); h.update(child.p(child.tbs), child.tbs.len); h.final(d);
    return rsa_pkcs1v15_verify(issuer.p(issuer.rsaN), issuer.rsaN.len, issuer.p(issuer.rsaE), issuer.rsaE.len,
                               child.p(child.sig), child.sig.len, a, d, hash_len(a));
}
bool eku_ok(const Certificate& c, const char* usage) {
    if (!c.hasEku) return true;
    for (auto& u : c.eku) if (u == usage || u == "2.5.29.37.0") return true;
    return false;
}
// CertVerifyCertificateChainPolicy / softpub: status priority order.
uint32_t status_to_hr(uint32_t st) {
    if (st & T_NOT_SIG_VALID) return TRUST_E_CERT_SIGNATURE;
    if (st & T_UNTRUSTED_ROOT) return CERT_E_UNTRUSTEDROOT;
    if (st & T_NOT_TIME_VALID) return CERT_E_EXPIRED;
    if (st & T_NOT_VALID_FOR_USAGE) return CERT_E_WRONG_USAGE;
    if (st & T_INVALID_BASIC_CONSTRAINTS) return TRUST_E_BASIC_CONSTRAINTS;
    if (st & T_PARTIAL_CHAIN) return CERT_E_CHAINING;
    return 0;
}

// Builds and evaluates `leaf`'s chain at date T. `pool` = certificates from the message.
uint32_t chain_status(const Certificate& leaf, const std::vector<Certificate>& pool, int64_t T,
                      const char* usage, std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lk(g_rootsMu);
    const auto& anchors = roots_locked();
    uint32_t st = 0;
    const Certificate* cur = &leaf;
    auto is_anchor = [&](const Certificate& c) -> const Certificate* {
        for (auto& a : anchors) {
            if (a.der == c.der) return &a;
            if (same_span(a, a.subject, c.p(c.subject), c.subject.len) && same_span(a, a.spki, c.p(c.spki), c.spki.len)) return &a;
        }
        return nullptr;
    };
    for (int depth = 0; depth < 16; depth++) {
        names.push_back(cn_of(*cur));
        if (T < cur->notBefore || T > cur->notAfter) st |= T_NOT_TIME_VALID;
        if (!eku_ok(*cur, usage)) st |= T_NOT_VALID_FOR_USAGE;
        if (depth > 0 && cur->version >= 2 && !(cur->hasBasicConstraints && cur->isCA)) st |= T_INVALID_BASIC_CONSTRAINTS;
        if (const Certificate* anc = is_anchor(*cur)) {          // anchor reached (store root)
            // Root program properties (authroot.stl), see authenticode_roots.h.
            const roots::RootProps& pr = root_props_locked()[(size_t)(anc - &anchors[0])];
            if (pr.eku_only && std::strcmp(pr.eku_only, usage) != 0) st |= T_NOT_VALID_FOR_USAGE;
            if (pr.not_before && leaf.notBefore > pr.not_before) st |= T_UNTRUSTED_ROOT;
            if (pr.disabled_at && T >= pr.disabled_at) st |= T_UNTRUSTED_ROOT;
            return st;
        }
        bool selfSigned = cur->issuer.len == cur->subject.len &&
                          std::memcmp(cur->p(cur->issuer), cur->p(cur->subject), cur->issuer.len) == 0;
        if (selfSigned) {
            if (!cert_signed_by(*cur, *cur)) st |= T_NOT_SIG_VALID;
            return st | T_UNTRUSTED_ROOT;
        }
        // Issuer: trusted roots first, then the message.
        const Certificate* next = nullptr; bool sigOk = false;
        for (auto& a : anchors)
            if (same_span(a, a.subject, cur->p(cur->issuer), cur->issuer.len)) {
                if (cert_signed_by(*cur, a)) { next = &a; sigOk = true; break; }
            }
        if (!next)
            for (auto& c : pool)
                if (&c != cur && same_span(c, c.subject, cur->p(cur->issuer), cur->issuer.len)) {
                    if (cert_signed_by(*cur, c)) { next = &c; sigOk = true; break; }
                    if (!next) next = &c;
                }
        if (!next) return st | T_PARTIAL_CHAIN;
        if (!sigOk) st |= T_NOT_SIG_VALID;
        cur = next;
    }
    return st | T_PARTIAL_CHAIN;
}

const Attribute* find_attr(const std::vector<Attribute>& v, const char* oid) {
    for (auto& a : v) if (a.oid == oid) return &a;
    return nullptr;
}

// Verifies a SignerInfo: authenticated attributes (contentType optional,
// messageDigest == H(content)) then the RSA signature. Returns 0 or a code.
uint32_t verify_signer(const uint8_t* b, const SignerInfo& si, const Certificate& cert,
                       const uint8_t* content, size_t clen, const char* expectContentType) {
    HashAlg ha = digest_alg(si.digestAlgOid);
    if (!ha) return NTE_BAD_ALGID;
    if (cert.rsaN.empty()) return NTE_BAD_ALGID;
    uint8_t md[32]; Hasher h; h.init(ha); h.update(content, clen); h.final(md);
    uint8_t sd[32];
    if (!si.authAttrsTlv.empty()) {
        if (expectContentType) {
            const Attribute* ct = find_attr(si.authAttrs, OID_CONTENT_TYPE);
            if (!ct || ct->values.size() != 1) return CRYPT_E_HASH_VALUE;
            Tlv o; const Span& v = ct->values[0];
            if (!der_read(b, v.off, v.off + v.len, o) || o.tag != 0x06 || oid_to_string(b + o.val.off, o.val.len) != expectContentType)
                return CRYPT_E_HASH_VALUE;
        }
        const Attribute* mda = find_attr(si.authAttrs, OID_MESSAGE_DIGEST);
        if (!mda || mda->values.size() != 1) return CRYPT_E_HASH_VALUE;
        Tlv o; const Span& v = mda->values[0];
        if (!der_read(b, v.off, v.off + v.len, o) || o.tag != 0x04 || o.val.len != hash_len(ha) ||
            std::memcmp(b + o.val.off, md, hash_len(ha)) != 0)
            return CRYPT_E_HASH_VALUE;
        std::vector<uint8_t> set(b + si.authAttrsTlv.off, b + si.authAttrsTlv.off + si.authAttrsTlv.len);
        set[0] = 0x31;                                           // [0] IMPLICIT -> SET OF
        Hasher h2; h2.init(ha); h2.update(set.data(), set.size()); h2.final(sd);
    } else std::memcpy(sd, md, hash_len(ha));
    if (!rsa_pkcs1v15_verify(cert.p(cert.rsaN), cert.rsaN.len, cert.p(cert.rsaE), cert.rsaE.len,
                             b + si.encryptedDigest.off, si.encryptedDigest.len, ha, sd, hash_len(ha)))
        return NTE_BAD_SIGNATURE;
    return 0;
}

int find_cert(const std::vector<Certificate>& pool, const uint8_t* b, const SignerInfo& si) {
    for (size_t i = 0; i < pool.size(); i++) {
        const Certificate& c = pool[i];
        if (same_span(c, c.issuer, b + si.issuer.off, si.issuer.len) &&
            same_serial(c.p(c.serial), c.serial.len, b + si.serial.off, si.serial.len)) return (int)i;
    }
    return -1;
}
} // namespace

uint32_t verify_pe(const std::vector<uint8_t>& file, const VerifyOptions& opt, VerifyReport& rep) {
    rep = VerifyReport();
    auto done = [&](uint32_t hr, uint32_t le, const char* why) { rep.hr = hr; rep.last_error = le; rep.detail = why; return hr; };
    int64_t now = opt.now ? opt.now : (int64_t)std::time(nullptr);

    std::vector<uint8_t> p7; uint32_t e = 0;
    if (!pe_extract_pkcs7(file, p7, &e)) {
        if (e == TRUST_E_SUBJECT_FORM_UNKNOWN) return done(TRUST_E_SUBJECT_FORM_UNKNOWN, TRUST_E_SUBJECT_FORM_UNKNOWN, "not a PE image");
        if (e == TRUST_E_NOSIGNATURE) return done(TRUST_E_NOSIGNATURE, TRUST_E_NOSIGNATURE, "no Authenticode signature");
        return done(TRUST_E_NOSIGNATURE, e, "corrupt security directory");
    }
    SignedData sd;
    if (!sd.parse(p7.data(), p7.size(), &e)) return done(TRUST_E_NOSIGNATURE, e, "PKCS#7 decode failed");
    if (sd.contentType != OID_SPC_INDIRECT || sd.content.empty() || sd.signers.empty())
        return done(TRUST_E_NOSIGNATURE, CRYPT_E_UNEXPECTED_MSG_TYPE, "not an Authenticode SignedData");
    const uint8_t* b = sd.buf.data();

    // SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue, messageDigest DigestInfo }
    Tlv spc; Span imgDigest; HashAlg imgAlg = HASH_NONE;
    {
        if (!der_read(b, sd.content.off, sd.content.off + sd.content.len, spc) || spc.tag != 0x30)
            return done(TRUST_E_NOSIGNATURE, CRYPT_E_ASN1_BADTAG, "SpcIndirectDataContent");
        Cur c(b, spc.val); Tlv data, di;
        if (!c.expect(0x30, data) || !c.expect(0x30, di)) return done(TRUST_E_NOSIGNATURE, CRYPT_E_ASN1_BADTAG, "SpcIndirectDataContent");
        Cur dc(b, di.val); Tlv alg, dg; std::string aoid; Span params;
        if (!dc.next(alg) || !parse_alg(b, alg, aoid, params) || !dc.expect(0x04, dg))
            return done(TRUST_E_NOSIGNATURE, CRYPT_E_ASN1_BADTAG, "DigestInfo");
        imgAlg = digest_alg(aoid); imgDigest = dg.val;
        if (!imgAlg || dg.val.len != hash_len(imgAlg)) return done(NTE_BAD_ALGID, NTE_BAD_ALGID, "image digest algorithm");
    }
    rep.image_alg = imgAlg;

    // 1. Image digest (CryptSIPVerifyIndirectData): BEFORE the signature,
    //    just as softpub's "object" step precedes its "signature" step.
    std::vector<uint8_t> dig; std::string why;
    if (!pe_image_digest(file, imgAlg, dig, &why)) return done(TRUST_E_BAD_DIGEST, TRUST_E_BAD_DIGEST, "image digest could not be computed");
    if (std::memcmp(dig.data(), b + imgDigest.off, dig.size()) != 0)
        return done(TRUST_E_BAD_DIGEST, TRUST_E_BAD_DIGEST, "image digest mismatch (file modified)");
    if (opt.hash_only) return done(AC_OK, 0, "image digest matches (hash only)");

    // 2. Primary signer.
    const SignerInfo& si = sd.signers[0];
    int ci = find_cert(sd.certs, b, si);
    if (ci < 0) return done(TRUST_E_NO_SIGNER_CERT, TRUST_E_NO_SIGNER_CERT, "signer certificate not in message");
    rep.signer_cert = ci;
    // Signer failure -> TRUST_E_CERT_SIGNATURE: matches Wine's wintrust
    // implementation; the internal detail is kept in the report.
    uint32_t sv = verify_signer(b, si, sd.certs[ci], b + spc.val.off, spc.val.len, OID_SPC_INDIRECT);
    if (sv) return done(TRUST_E_CERT_SIGNATURE, TRUST_E_CERT_SIGNATURE,
                        sv == NTE_BAD_SIGNATURE ? "signer RSA signature invalid" :
                        sv == NTE_BAD_ALGID ? "signer algorithm not supported" : "signed attributes digest mismatch");

    // 3. Timestamp.
    int64_t T = now;
    uint32_t tsChain = 0;
    SignedData tsd;                                          // RFC 3161 token (if there is one)
    const Certificate* tsaLeaf = nullptr;
    const std::vector<Certificate>* tsaPool = nullptr;
    if (const Attribute* cs = find_attr(si.unauthAttrs, OID_COUNTERSIG)) {
        if (cs->values.empty()) return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "empty countersignature");
        Tlv t; const Span& v = cs->values[0];
        SignerInfo csi;
        if (!der_read(b, v.off, v.off + v.len, t) || !parse_signer_info(b, t, csi))
            return done(TRUST_E_TIME_STAMP, CRYPT_E_ASN1_CORRUPT, "countersignature decode");
        int tci = find_cert(sd.certs, b, csi);
        if (tci < 0) return done(TRUST_E_TIME_STAMP, TRUST_E_NO_SIGNER_CERT, "timestamp certificate not in message");
        // The countersignature covers the CONTENT of the signer's encryptedDigest.
        if (verify_signer(b, csi, sd.certs[tci], b + si.encryptedDigest.off, si.encryptedDigest.len, nullptr))
            return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "countersignature invalid");
        const Attribute* stA = find_attr(csi.authAttrs, OID_SIGNING_TIME);
        int64_t ts;
        if (!stA || stA->values.empty() || !der_time(b + stA->values[0].off, stA->values[0].len, ts))
            return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "countersignature without signingTime");
        rep.timestamped = true; rep.timestamp = ts; T = ts;
        tsaLeaf = &sd.certs[tci]; tsaPool = &sd.certs;
    } else if (const Attribute* rt = find_attr(si.unauthAttrs, OID_MS_RFC3161)) {
        if (rt->values.empty()) return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "empty RFC3161 attribute");
        uint32_t te = 0;
        const Span& v = rt->values[0];
        if (!tsd.parse(b + v.off, v.len, &te) || tsd.contentType != OID_TSTINFO || tsd.signers.empty() || tsd.content.empty())
            return done(TRUST_E_TIME_STAMP, te ? te : CRYPT_E_UNEXPECTED_MSG_TYPE, "RFC3161 token decode");
        const uint8_t* tb = tsd.buf.data();
        Tlv oct; if (!der_read(tb, tsd.content.off, tsd.content.off + tsd.content.len, oct) || oct.tag != 0x04)
            return done(TRUST_E_TIME_STAMP, CRYPT_E_ASN1_BADTAG, "TSTInfo wrapper");
        // TSTInfo ::= SEQUENCE { version, policy, messageImprint, serialNumber, genTime, ... }
        Tlv tst; if (!der_read(tb, oct.val.off, oct.val.off + oct.val.len, tst) || tst.tag != 0x30)
            return done(TRUST_E_TIME_STAMP, CRYPT_E_ASN1_BADTAG, "TSTInfo");
        Cur c(tb, tst.val); Tlv ver, pol, imp, sn, gt;
        if (!c.expect(0x02, ver) || !c.expect(0x06, pol) || !c.expect(0x30, imp) || !c.expect(0x02, sn) || !c.expect(0x18, gt))
            return done(TRUST_E_TIME_STAMP, CRYPT_E_ASN1_BADTAG, "TSTInfo fields");
        Cur ic(tb, imp.val); Tlv ialg, ihash; std::string ioid; Span ip;
        if (!ic.next(ialg) || !parse_alg(tb, ialg, ioid, ip) || !ic.expect(0x04, ihash))
            return done(TRUST_E_TIME_STAMP, CRYPT_E_ASN1_BADTAG, "messageImprint");
        HashAlg ia = digest_alg(ioid);
        uint8_t hh[32];
        if (!ia || ihash.val.len != hash_len(ia)) return done(TRUST_E_TIME_STAMP, NTE_BAD_ALGID, "messageImprint algorithm");
        { Hasher h; h.init(ia); h.update(b + si.encryptedDigest.off, si.encryptedDigest.len); h.final(hh); }
        if (std::memcmp(hh, tb + ihash.val.off, ihash.val.len) != 0) return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "messageImprint mismatch");
        const SignerInfo& tsi = tsd.signers[0];
        int tci = find_cert(tsd.certs, tb, tsi);
        if (tci < 0) return done(TRUST_E_TIME_STAMP, TRUST_E_NO_SIGNER_CERT, "TSA certificate not in token");
        if (verify_signer(tb, tsi, tsd.certs[tci], tb + oct.val.off, oct.val.len, OID_TSTINFO))
            return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "RFC3161 token signature invalid");
        int64_t ts; if (!der_time(tb + gt.all.off, gt.all.len, ts)) return done(TRUST_E_TIME_STAMP, TRUST_E_TIME_STAMP, "genTime");
        rep.timestamped = true; rep.timestamp = ts; T = ts;
        tsaLeaf = &tsd.certs[tci]; tsaPool = &tsd.certs;
    }
    // WTD_LIFETIME_SIGNING_FLAG: the timestamp no longer extends validity;
    // both chains are then evaluated at `now`.
    if (opt.lifetime_signing) T = now;
    if (tsaLeaf) tsChain = chain_status(*tsaLeaf, *tsaPool, T, OID_KP_TIMESTAMP, rep.ts_chain);
    rep.evaluated_at = T;

    // 4. Chains.
    uint32_t st = chain_status(sd.certs[ci], sd.certs, T, OID_KP_CODESIGN, rep.chain);
    uint32_t hr = status_to_hr(st);
    if (hr) return done(hr, hr, "signer certificate chain not trusted");
    hr = status_to_hr(tsChain);
    if (hr) return done(hr, hr, "timestamp certificate chain not trusted");

    // 5. Revocation (see header comment).
    if (opt.revocation_requested) {
        rep.revocation_unchecked = true;
        if (opt.strict_revocation) return done(CERT_E_REVOCATION_FAILURE, CERT_E_REVOCATION_FAILURE, "revocation status unknown (offline)");
    }
    return done(AC_OK, 0, "trusted");
}

}} // namespace wx86::ac
