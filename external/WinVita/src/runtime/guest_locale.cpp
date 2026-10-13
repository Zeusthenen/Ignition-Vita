// Engine defaults to en-US (0x0409) until the consumer calls
// wx86_locale_set_lcid(), rather than hardcoding a game's language.
#include "guest_locale.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t g_lcid = 0x0409u;   // en-US until the consumer sets one
}

uint32_t wx86_locale_lcid() { return g_lcid; }
void     wx86_locale_set_lcid(uint32_t lcid) { if (lcid) g_lcid = lcid; }

uint32_t wx86_locale_detect_posix() {
    const char* l = std::getenv("LC_ALL");
    if (!l || !*l) l = std::getenv("LC_MESSAGES");
    if (!l || !*l) l = std::getenv("LANG");
    if (!l || !*l || (!std::strncmp(l, "C", 1) && (l[1] == 0 || l[1] == '.'))) return 0x0409u;
    char lang[3] = {0}, ctry[3] = {0};
    int i = 0;
    for (; l[i] && l[i] != '_' && l[i] != '.' && l[i] != '@' && i < 2; i++)
        lang[i] = (char)std::tolower((unsigned char)l[i]);
    if (l[i] == '_')
        for (int j = 0; l[i + 1 + j] && l[i + 1 + j] != '.' && l[i + 1 + j] != '@' && j < 2; j++)
            ctry[j] = (char)std::toupper((unsigned char)l[i + 1 + j]);
    struct M { const char* ll; const char* cc; uint32_t lcid; };
    static const M m[] = {
        {"en","GB",0x0809},{"en",nullptr,0x0409},{"fr","CA",0x0C0C},{"fr",nullptr,0x040C},
        {"de",nullptr,0x0407},{"es","MX",0x080A},{"es",nullptr,0x0C0A},{"it",nullptr,0x0410},
        {"pt","BR",0x0416},{"pt",nullptr,0x0816},{"ja",nullptr,0x0411},{"ko",nullptr,0x0412},
        {"zh","TW",0x0404},{"zh",nullptr,0x0804},{"ru",nullptr,0x0419},{"pl",nullptr,0x0415},
        {"nl",nullptr,0x0413},{"sv",nullptr,0x041D} };
    for (auto& e : m)
        if (!std::strcmp(e.ll, lang) && (!e.cc || !std::strcmp(e.cc, ctry))) return e.lcid;
    return 0x0409u;
}

// Code page for the active locale (a fixed 1252/437 would be wrong for a
// Cyrillic, CJK, Turkish, or Greek system).
uint32_t wx86_cp_ansi() {
    switch (g_lcid) {
        case 0x0419: return 1251;  case 0x0415: return 1250;  case 0x0405: return 1250;  // ru / pl / cs
        case 0x0411: return 932;   case 0x0412: return 949;   case 0x0804: return 936;
        case 0x0404: return 950;   case 0x041F: return 1254;  case 0x0408: return 1253;
        default:     return 1252;                                                        // Western Europe
    }
}

uint32_t wx86_cp_oem() {
    switch (g_lcid) {
        case 0x0419: return 866;   case 0x0411: return 932;   case 0x0412: return 949;
        case 0x0804: return 936;   case 0x0404: return 950;   case 0x0409: return 437;    // US
        default:     return 850;                                                         // Western Europe (multilingual)
    }
}

const char* wx86_locale_info(uint32_t lctype) {
    const bool fr = (g_lcid & 0xff) == 0x0c;                 // 0x040C etc. = French
    switch (lctype & 0xffff) {
        case 0x1004: return "1252";                          // IDEFAULTANSICODEPAGE
        case 0x000B: return fr ? "850" : "437";              // IDEFAULTCODEPAGE (OEM)
        case 0x0001: return fr ? "040c" : "0409";            // ILANGUAGE (hex id as text)
        case 0x0009: return fr ? "040c" : "0409";            // IDEFAULTLANGUAGE
        case 0x0059: return fr ? "fr" : "en";                // SISO639LANGNAME
        case 0x005A: return fr ? "FR" : "US";                // SISO3166CTRYNAME
        case 0x0002: return fr ? "French (France)" : "English (United States)";   // SLANGUAGE
        case 0x0003: return fr ? "FRA" : "ENU";              // SABBREVLANGNAME
        case 0x1001: return fr ? "French" : "English";       // SENGLANGUAGE
        case 0x0005: return fr ? "33" : "1";                 // ICOUNTRY (numeric code)
        case 0x0006: return fr ? "France" : "United States"; // SCOUNTRY (localized)
        case 0x0007: return fr ? "FRA" : "USA";              // SABBREVCTRYNAME
        case 0x1002: return fr ? "France" : "United States"; // SENGCOUNTRY
        case 0x000E: return fr ? "," : ".";                  // SDECIMAL
        case 0x000F: return fr ? "\xA0" : ",";               // STHOUSAND (FR = non-breaking space)
        default:     return "1";
    }
}

uint16_t wx86_ctype1(uint32_t ch) {
    uint16_t t = 0;
    if (ch >= 'A' && ch <= 'Z') t |= 0x001 | 0x100;
    else if (ch >= 'a' && ch <= 'z') t |= 0x002 | 0x100;
    if (ch >= '0' && ch <= '9') t |= 0x004 | 0x080;
    if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) t |= 0x080;
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') t |= 0x008;
    if (ch == ' ' || ch == '\t') t |= 0x040;
    if (ch < 0x20 || ch == 0x7f) t |= 0x020;
    if (t == 0 && ch >= 0x21 && ch < 0x7f) t |= 0x010;       // PUNCT
    return t;
}
