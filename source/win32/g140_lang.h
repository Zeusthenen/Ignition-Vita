// iteration-G140: language persistence for the whole session -- pure decisions, no guest / GL / file access (host-testable).
//
// Proven by disassembly of Ign_3dfx.exe (details: G140_LANGUAGE_PERSISTENCE_REPORT.txt, G140_LANGUAGE_DISASSEMBLY_EXTRACTS.txt):
//  * The language lives in two guest variables. [0x00491A43] = k of the selected flag (the last dword of the 0x597-byte option block of ign_3dfx.btz: file offset 0x593 = 0x004914B0 + 0x593).
//    [0x0049051C] = k the menus actually use (text tables 0x0046EC44 + k*204, ~35 readers). Written only at 0x00404A9B/0x00404A8F (frontend init) and 0x0040690D (Enter in the selector).
//  * Every return to the front end re-runs the init 0x004049D0 (state [0x0061D3A4]==1, caller 0x0041136C): it reads k back from ign_3dfx.btz (4 bytes at offset 0x593 -> [0x0049051C]) and
//    the menu builder 0x00409F40 (called from 0x00406100) restarts the menu at node 0x00472070 (the language selector), resets the node tables to English, zeroes the option block
//    (0x0040E720) and re-reads the whole block from the btz ([0x00491A43] = k from the file). When the option block was already saved ([0x0046E81C]==0) the selector confirms itself
//    (0x00405808 sets the Enter key) with that k.
//  * Therefore the language after a race is whatever ign_3dfx.btz holds -- and, in G128..G139, the selector hook forced [0x00491A43]=0 at EVERY entry of the node.
// Fix: remember the confirmed language for the session; give it back where the original code loads it (the btz read) and never force English after a language was confirmed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ign::g140 {

constexpr uint32_t kLangCount = 6;             // k = 0 English, 1 Deutsch, 2 Italiano, 3 Espanol, 4 Svenska, 5 Francais (tables 0x0046EC44 + k*204, trees 0x004907F4 + k*100)
constexpr uint32_t kBtzLangOffset = 0x593;     // file offset of [0x00491A43] inside ign_3dfx.btz (block base 0x004914B0, size 0x597)
constexpr uint32_t kBtzLangSize = 4;
constexpr uint32_t kBtzBlockSize = 0x597;
constexpr uint32_t kNodeLang = 0x00472070u;    // the selector node (the same pointer in all six trees)
constexpr uint32_t kAddrSelected = 0x00491A43u;
constexpr uint32_t kAddrRefresh = 0x00490608u;
constexpr uint32_t kAddrMenuLang = 0x0049051Cu;
constexpr uint32_t kAddrTreeTable = 0x004907F4u;   // six trees of 25 node pointers (100 bytes each)
constexpr uint32_t kAddrTreeActive = 0x0049068Cu;  // the active tree (25 dwords) copied by Enter
constexpr uint32_t kAddrDataBaseTable = 0x00490A4Cu;
constexpr uint32_t kAddrDataBase = 0x0049C0E8u;
constexpr uint32_t kTreeMainNodeOffset = 0x2C;     // [0x004906B8] - 0x0049068C

inline bool valid_index(uint32_t k) { return k < kLangCount; }

// Small session state: the language the user confirmed in this process. A new process starts invalid (the selector starts in English, G128).
struct Session {
    bool valid = false;
    uint32_t index = 0;
    uint32_t enters = 0;        // entries of the selector node seen in this process
    uint32_t confirms = 0;      // confirmations captured
    uint32_t resets = 0;        // resets detected (a confirmation that is not the session language without the user touching the selector, or a stale btz value)
    uint32_t restores = 0;      // times the session language was given back
};

inline Session g_session{};

enum class EnterAction : uint8_t { None, ForceEnglish, SelectSession };

// First entry of the process and no language confirmed yet: the selector starts in English (kept from G128). After a confirmation: never force zero; the selection shown is the session language.
inline EnterAction on_enter(Session& s, uint32_t rawSelectedK) {
    ++s.enters;
    if (s.valid) { return rawSelectedK != s.index ? EnterAction::SelectSession : EnterAction::None; }
    if (s.enters == 1 && rawSelectedK != 0) { return EnterAction::ForceEnglish; }
    return EnterAction::None;
}

enum class ExitResult : uint8_t { Captured, Unchanged, ResetDetected, InvalidIgnored };

// The selector node was left. confirmedK = [0x0049051C] (the index the game really uses), userChanged = the selection was moved by the user during this visit.
inline ExitResult on_exit(Session& s, uint32_t confirmedK, bool userChanged) {
    if (!valid_index(confirmedK)) { return ExitResult::InvalidIgnored; }
    if (!s.valid) { s.valid = true; s.index = confirmedK; ++s.confirms; return ExitResult::Captured; }
    if (userChanged) {
        if (confirmedK == s.index) { return ExitResult::Unchanged; }
        s.index = confirmedK; ++s.confirms; return ExitResult::Captured;
    }
    if (confirmedK == s.index) { return ExitResult::Unchanged; }
    ++s.resets;   // a confirmation the user did not make: the session language is NOT overwritten
    return ExitResult::ResetDetected;
}

// ---- ign_3dfx.btz read patch ----
inline bool overlaps_btz_lang(int64_t fileOffset, uint32_t got) {
    return fileOffset >= 0 && got != 0 && fileOffset < (int64_t)kBtzLangOffset + kBtzLangSize && fileOffset + (int64_t)got > (int64_t)kBtzLangOffset;
}

// Case-insensitive "ends with ign_3dfx.btz" (a path separator or nothing before it).
inline bool is_btz_path(const char* path) {
    if (!path) { return false; }
    const char* name = "ign_3dfx.btz";
    const size_t n = strlen(name), len = strlen(path);
    if (len < n) { return false; }
    const char* tail = path + (len - n);
    for (size_t i = 0; i < n; ++i) {
        char a = tail[i], b = name[i];
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (a != b) { return false; }
    }
    return len == n || path[len - n - 1] == '/' || path[len - n - 1] == '\\';
}

// Replaces, inside a block that was just read from the btz at fileOffset, the bytes that cover the language field with the session language. Returns the number of bytes changed;
// *oldK receives the language read from the file when all four bytes were in the block (otherwise 0xFFFFFFFF).
inline size_t btz_patch(const Session& s, int64_t fileOffset, uint8_t* data, uint32_t got, uint32_t* oldK) {
    if (oldK) { *oldK = 0xFFFFFFFFu; }
    if (!s.valid || !valid_index(s.index) || !data || !overlaps_btz_lang(fileOffset, got)) { return 0; }
    const int64_t lo = fileOffset > (int64_t)kBtzLangOffset ? fileOffset : (int64_t)kBtzLangOffset;
    const int64_t hiBlock = fileOffset + (int64_t)got, hiField = (int64_t)kBtzLangOffset + kBtzLangSize;
    const int64_t hi = hiBlock < hiField ? hiBlock : hiField;
    if (lo == (int64_t)kBtzLangOffset && hi == hiField) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < kBtzLangSize; ++i) { v |= (uint32_t)data[(size_t)(lo - fileOffset) + i] << (8 * i); }
        if (oldK) { *oldK = v; }
    }
    size_t changed = 0;
    for (int64_t p = lo; p < hi; ++p) {
        const uint8_t want = (uint8_t)((s.index >> (8 * (uint32_t)(p - (int64_t)kBtzLangOffset))) & 0xFFu);
        uint8_t& b = data[(size_t)(p - fileOffset)];
        if (b != want) { b = want; ++changed; }
    }
    return changed;
}

}  // namespace ign::g140
