// win32/g153_key.h -- iteration-G153, PURE (no GL, no Vita API, no guest access, no logging, no heap, no global state; tools/g153_test.cpp drives exactly this code).
//
// THE SMOKE (and every other chroma-keyed sprite): ROOT CAUSE, proven by the code and by the game's data
//  * The game draws its particles (smoke, dark smoke, explosion smoke, light, boom ...: GENERAL\*.TEX, 256x256 / 256x128 8-bit palette-index images whose index 0 covers 56-85 % of the texels) through nine helper functions
//    0x00451380 .. 0x00453000 that all do:  guTexSource(i) ; guTexCombineFunction(0, DECAL) ; guColorCombineFunction(DECAL_TEXTURE) ; grChromakeyMode(ENABLE) ; grChromakeyValue([0x0061E720]) ; guDrawTriangleWithClip ;
//    grChromakeyMode(DISABLE).  Four of them first set a constant alpha (0x7F000000 / 0x4F000000 / 0x3F000000: 50 % / 31 % / 25 %), alpha source CC_ALPHA and the blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA): translucent smoke.
//  * [0x0061E720] is entry 0 of the palette table the loader 0x00458BA4 builds (0x00RRGGBB per entry): palette[0] = (0,0,0) in every .COL / sys.col  =>  the key is BLACK, and a texel is transparent when its colour equals it.
//  * G151/G152 (and since G43/G90, deliberately, "state storage only") never implemented that: grChromakeyMode / grChromakeyValue only store the state; the texture is uploaded as GL_RGB 565 (no alpha) and drawn with GL_REPLACE and
//    no alpha test. So every texel the game keys out is drawn as an OPAQUE BLACK texel: the black rectangles of the photographs (smoke of the truck, of the train in the menu, other effects).
//  * The Vita receives the textures as RGB332 because the game takes its "no palette" path (the console run only ever downloads fmt 0 from the scratch buffer; [0x0061EB20] is set by the hardware query at 0x00458690): 0x0044FE70 translates the palette indices through the 256-byte table 0x0061E020 (= RGB332 of the
//    palette, truncated: (r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6)) into the scratch buffer 0x004E25C0 and downloads THAT. Several indices share one code (indices 1..3 of the palette, (13,9,9) (26,15,29) ..., collapse onto code 0 with
//    black), so the key can NOT be decided from the downloaded bytes: it would punch holes in dark, legitimately opaque texels. It is decided from the INDEX image (the function's argument, still intact in guest memory when the download
//    runs), exactly like the palettised hardware would: a texel is keyed when palette[index] == key (RGB; the alpha byte is ignored).
// THE FIX: for a textured triangle drawn while the key is enabled, the texture is replaced by a variant uploaded as GL_RGBA/5_5_5_1 (the format G115 already uses) whose alpha is 0 on the keyed texels and 1 elsewhere; the draw session
// enables the alpha test for those triangles (and, when the Glide blend uses the source alpha, GL_MODULATE with white vertex colours, which equals GL_REPLACE for the colour and multiplies the alpha: the translucency of the smoke is
// preserved). Everything that is not provable (no index image, a palette that does not match the translation table, a key that no palette entry equals) leaves the draw exactly as G152 drew it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ign { namespace g153 {

constexpr uint32_t kPaletteTable = 0x0061E720u;     // 256 x 0x00RRGGBB (entry 0 is the chroma key of every helper)
constexpr uint32_t kTranslateTable = 0x0061E020u;   // 256 bytes: palette index -> RGB332 (built by 0x0044FE30)
constexpr uint32_t kRemapScratch = 0x004E25C0u;     // where 0x0044FE70 writes the translated image it downloads
constexpr uint32_t kDownloadReturn = 0x0044FF03u;   // the instruction after `call guTexDownloadMipMap` inside 0x0044FE70
constexpr uint32_t kIndexArgOffset = 0x1Cu;         // [ESP at the shim entry] + 0x1C = the argument of 0x0044FE70 = the index image (ret, mmid, src, table, saved edi, saved esi, ret, arg)
constexpr uint32_t kImageBytes = 65536u;            // 256 x 256 x 8 bit
constexpr uint32_t kKeyMark = 0x15300000u;          // in Key::ckValue: the high 24 bits say "keyed draw"; bit 0 = modulate

// ---- colour helpers (identical to gu_rgb332_to_rgb565 in glide2x_ign.cpp: tools/g153_test.cpp compares the text) ----
inline uint16_t rgb332_to_565(uint8_t p) {
    const uint32_t r3 = (p >> 5) & 0x7, g3 = (p >> 2) & 0x7, b2 = p & 0x3;
    const uint32_t r5 = (r3 << 2) | (r3 >> 1), g6 = (g3 << 3) | g3, b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}
// GL_RGBA / GL_UNSIGNED_SHORT_5_5_5_1: R5 G5 B5 A1. The 5-bit green is the top five bits of the 6-bit one the base texture has (at most 1/32 of a level apart); red and blue are the same bits.
inline uint16_t to_5551(uint8_t code, bool opaque) {
    const uint16_t c = rgb332_to_565(code);
    return (uint16_t)((c & 0xF800u) | (((c >> 6) & 0x1Fu) << 6) | ((c & 0x1Fu) << 1) | (opaque ? 1u : 0u));
}
inline uint8_t code_of_rgb(uint32_t rgb) { return (uint8_t)(((rgb >> 16) & 0xE0u) | (((rgb >> 8) >> 3) & 0x1Cu) | ((rgb & 0xFFu) >> 6)); }   // 0x0044FE30: (r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6)

// the translation table of the game must be the truncation of its own palette; otherwise the palette snapshot is not the one the image was translated with
inline bool palette_matches_translation(const uint32_t pal[256], const uint8_t xlat[256]) {
    for (int i = 0; i < 256; ++i) if (code_of_rgb(pal[i]) != xlat[i]) return false;
    return true;
}
// the downloaded image must be the index image translated through that table (checked on the whole image: 64 K compares, once per texture)
inline bool image_matches_indices(const uint8_t* idx, const uint8_t* code, const uint8_t xlat[256], uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (code[i] != xlat[idx[i]]) return false;
    return true;
}
// which palette indices the key matches (RGB compare, alpha ignored); returns how many
inline uint32_t keyed_indices(const uint32_t pal[256], uint32_t keyValue, uint8_t mask[256]) {
    uint32_t n = 0; const uint32_t k = keyValue & 0x00FFFFFFu;
    for (int i = 0; i < 256; ++i) { mask[i] = ((pal[i] & 0x00FFFFFFu) == k) ? 1u : 0u; n += mask[i]; }
    return n;
}
// the variant image; returns the number of transparent texels
inline uint32_t build_variant(const uint8_t* idx, const uint8_t xlat[256], const uint8_t mask[256], uint16_t* out, uint32_t n) {
    uint32_t t = 0;
    for (uint32_t i = 0; i < n; ++i) { const uint8_t ix = idx[i]; const bool key = mask[ix] != 0; out[i] = to_5551(xlat[ix], !key); t += key ? 1u : 0u; }
    return t;
}

// the emergency switch: the exact token g153=off in ux0:data/ignition/g146_geo.txt (text with newlines / tabs already turned into spaces) puts the 3D chroma key back to what G152 drew
inline bool off_from_text(const char* text) {
    for (const char* p = text ? text : ""; *p;) {
        while (*p == ' ') ++p;
        const char* e = p; while (*e && *e != ' ') ++e;
        if ((size_t)(e - p) == 8 && memcmp(p, "g153=off", 8) == 0) return true;
        p = e;
    }
    return false;
}
// ---- what a draw needs (decided from the Glide state of the triangle) ----
// translucent: the Glide blend is active and uses the source alpha (SRC_ALPHA = 1, ONE_MINUS_SRC_ALPHA = 5 in either RGB slot); the identity blend (ONE, ZERO, ONE, ZERO) is "off"
inline bool blend_active(bool valid, uint32_t rgbSf, uint32_t rgbDf, uint32_t aSf, uint32_t aDf) { return valid && !(rgbSf == 4u && rgbDf == 0u && aSf == 4u && aDf == 0u); }
inline bool wants_modulate(bool valid, uint32_t rgbSf, uint32_t rgbDf, uint32_t aSf, uint32_t aDf) {
    return blend_active(valid, rgbSf, rgbDf, aSf, aDf) && (rgbSf == 1u || rgbSf == 5u || rgbDf == 1u || rgbDf == 5u);
}
inline uint32_t key_value(bool modulate) { return kKeyMark | (modulate ? 1u : 0u); }
inline bool is_keyed(uint32_t ckEnabled, uint32_t ckValue) { return ckEnabled != 0u && (ckValue & 0xFFFFFF00u) == kKeyMark; }
inline bool is_modulate(uint32_t ckEnabled, uint32_t ckValue) { return is_keyed(ckEnabled, ckValue) && (ckValue & 1u) != 0u; }

// ---- reasons a texture was left as G152 drew it, and the counters ----
enum Why : uint32_t { kWhyNone = 0, kWhyNoIndexImage, kWhyBadFormat, kWhyPaletteMismatch, kWhyImageMismatch, kWhyNoKeyedIndex, kWhyGlError, kWhyCount };
inline const char* why_name(uint32_t w) {
    static const char* const n[kWhyCount] = {"none", "no_index_image(download not from 0x0044FE70)", "not_256x256_RGB332", "palette_does_not_match_the_translation_table", "image_does_not_match_the_index_image", "no_palette_entry_equals_the_key", "gl_error_creating_the_variant"};
    return w < kWhyCount ? n[w] : "?";
}
// the order in which a download is accepted as "the game's remap of an index image" (the first failing step names the reason; anything but kWhyNone leaves the texture as G152 drew it)
inline uint32_t download_verdict(uint32_t format, uint32_t width, uint32_t height, uint32_t bytes, uint32_t callerEip, uint32_t srcPtr, bool readOk, bool paletteOk, bool imageOk) {
    if (format != 0u || width != 256u || height != 256u || bytes != kImageBytes) return kWhyBadFormat;
    if (callerEip != kDownloadReturn || srcPtr != kRemapScratch) return kWhyNoIndexImage;
    if (!readOk) return kWhyNoIndexImage;
    if (!paletteOk) return kWhyPaletteMismatch;
    if (!imageOk) return kWhyImageMismatch;
    return kWhyNone;
}
// at the first chroma-keyed draw: may a variant be built for this key?
inline uint32_t variant_verdict(bool uploaded, bool idxValid, uint32_t idxWhy, uint32_t keyedIdx) {
    if (!uploaded) return kWhyNoIndexImage;
    if (!idxValid) return idxWhy != kWhyNone ? idxWhy : kWhyNoIndexImage;
    if (keyedIdx == 0u) return kWhyNoKeyedIndex;
    return kWhyNone;
}
struct Stats {
    uint64_t keyedTris = 0, translucentTris = 0, plainKeyTris = 0;      // triangles drawn with a variant (translucent = modulate), chroma-key-enabled triangles drawn as before
    uint64_t variantsBuilt = 0, variantBytes = 0, variantsRebuilt = 0, downloads = 0, indexCaptured = 0, texelsKeyed = 0;
    uint64_t plainWhy[kWhyCount] = {};                                   // chroma-key-enabled triangles drawn as before, by reason
    uint32_t probeLines = 0;
};
inline size_t why_list(char* out, size_t cap, const uint64_t* w) {
    size_t p = 0; out[0] = 0; bool any = false;
    for (uint32_t i = 1; i < kWhyCount; ++i) if (w[i] && p + 90 < cap) { p += (size_t)snprintf(out + p, cap - p, "%s%s=%llu", any ? " " : "", why_name(i), (unsigned long long)w[i]); any = true; }
    if (!any) snprintf(out, cap, "none");
    return strlen(out);
}
// [G153-SMOKE-PROBE]: the first appearance of a chroma-keyed texture (at most kProbeLines per session)
constexpr uint32_t kProbeLines = 24;
inline int format_probe(char* out, size_t n, uint32_t mmid, uint32_t callerEip, uint32_t keyValue, uint32_t keyedIndices, uint32_t transparentTexels, uint32_t texels, bool modulate,
                        uint32_t rgbSf, uint32_t rgbDf, uint32_t aSf, uint32_t aDf, uint32_t colorFnc, uint32_t texFnc, uint32_t glBase, uint32_t glVariant, const uint32_t* firstIndices) {
    return snprintf(out, n, "texture mmid=%u 256x256 RGB332 (index image captured) | first chroma-keyed draw: caller_eip=0x%08x key=0x%08x (RGB %02x%02x%02x) | palette entries equal to the key=%u -> %u of %u texels transparent (%.1f%%) | "
                            "glide state: color_combine=0x%x tex_combine=0x%x blend=(%u,%u,%u,%u) -> %s | gl texture %u -> keyed variant %u (RGBA 5551, alpha test, %s) | first palette indices of the image: %u %u %u %u",
                    mmid, callerEip, keyValue, (keyValue >> 16) & 0xFF, (keyValue >> 8) & 0xFF, keyValue & 0xFF, keyedIndices, transparentTexels, texels, texels ? 100.0 * (double)transparentTexels / (double)texels : 0.0,
                    colorFnc, texFnc, rgbSf, rgbDf, aSf, aDf, modulate ? "translucent (source alpha used)" : "opaque cut-out", glBase, glVariant, modulate ? "GL_MODULATE with white vertex colour" : "GL_REPLACE",
                    firstIndices ? firstIndices[0] : 0u, firstIndices ? firstIndices[1] : 0u, firstIndices ? firstIndices[2] : 0u, firstIndices ? firstIndices[3] : 0u);
}
inline int format_summary(char* out, size_t n, const Stats& s, bool final) {
    char why[420]; why_list(why, sizeof why, s.plainWhy);
    return snprintf(out, n, "%s keyed_draws{triangles=%llu translucent=%llu opaque_cutout=%llu} variants{built=%llu rebuilt=%llu bytes=%llu texels_keyed=%llu} downloads{total=%llu index_image_captured=%llu} chroma_key_draws_left_as_before{%llu: %s} probe_lines=%u",
                    final ? "FINAL" : "RACE", (unsigned long long)s.keyedTris, (unsigned long long)s.translucentTris, (unsigned long long)(s.keyedTris - s.translucentTris), (unsigned long long)s.variantsBuilt, (unsigned long long)s.variantsRebuilt,
                    (unsigned long long)s.variantBytes, (unsigned long long)s.texelsKeyed, (unsigned long long)s.downloads, (unsigned long long)s.indexCaptured,
                    (unsigned long long)s.plainKeyTris, why, s.probeLines);
}

} }   // namespace ign::g153
