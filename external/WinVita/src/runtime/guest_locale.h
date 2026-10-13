// src/runtime/guest_locale.h — locale the guest program believes it sees.
//
// The LCID -> codepage/language-name/decimal-separator mapping is fixed
// Windows knowledge, independent of any game, so it lives here. Which
// locale is active depends on the consumer, set via wx86_locale_set_lcid().
// Default is en-US (0x0409), never a value picked for a specific game.
//
// wx86_locale_detect_posix() is offered as a reasonable default for a POSIX
// host; a console that exposes its own system language can just set the
// LCID itself.
#pragma once
#include <cstdint>

// Active LCID. Defaults to 0x0409 (en-US) until the consumer sets one.
uint32_t wx86_locale_lcid();
void     wx86_locale_set_lcid(uint32_t lcid);

// POSIX host locale, derived from LC_ALL / LC_MESSAGES / LANG.
// Returns 0x0409 for "C"/"POSIX"/unset.
uint32_t wx86_locale_detect_posix();

// ANSI and OEM code pages for the active locale.
uint32_t wx86_cp_ansi();
uint32_t wx86_cp_oem();

// Text value of an LCTYPE (GetLocaleInfoA/W). Never null.
const char* wx86_locale_info(uint32_t lctype);

// Character classification like GetStringType (CT_CTYPE1).
uint16_t wx86_ctype1(uint32_t ch);
