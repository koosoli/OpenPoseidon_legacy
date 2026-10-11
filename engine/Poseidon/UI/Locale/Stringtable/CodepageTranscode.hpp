#pragma once
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <string>

namespace Poseidon
{

enum class Codepage
{
    Utf8,   // already UTF-8, no conversion needed
    CP1252, // Western European: English, French, German, Italian, Spanish
    CP1250, // Central European: Czech, Polish, Slovak, Hungarian
    CP1251, // Cyrillic: Russian, Ukrainian, Bulgarian
};

// Map a stringtable language column name (e.g. "Czech", "English") to its codepage.
// Unknown languages fall back to CP1252.
Codepage CodepageForLanguage(const char* languageName);

// Transcode a byte string from the given codepage to UTF-8.
// For Utf8 input, returns the string unchanged.
// For legacy codepages, each byte in 0x80..0xFF is mapped to its Unicode codepoint
// and encoded as UTF-8 (1-3 bytes). Bytes 0x00..0x7F pass through unchanged.
std::string TranscodeToUtf8(const std::string& input, Codepage cp);

// Select the codepage used by DecodeLegacyTextToUtf8. Valid UTF-8 returns Utf8;
// otherwise the preferred legacy codepage is used unless another supported
// legacy codepage has stronger evidence.
Codepage SelectLegacyTextCodepage(const std::string& input, Codepage preferredCp);

// Decode user-facing legacy text to UTF-8 without double-encoding modern content.
// Valid UTF-8 is kept as-is. Otherwise, the preferred legacy codepage wins unless
// another supported legacy codepage produces clearly less-mojibaked text. This
// keeps original mods readable when their stringtable/config literals carry
// CP1250 or CP1251 text under a different selected language.
std::string DecodeLegacyTextToUtf8(const std::string& input, Codepage preferredCp);

// Arma 1 ships stringtables and briefings as UTF-16 with a byte-order mark. When
// `data` starts with an FF FE / FE FF BOM, writes the UTF-8 text (without the BOM)
// to `out` and returns true; otherwise returns false and leaves `out` untouched.
bool DecodeUtf16BomToUtf8(const char* data, size_t size, std::string& out);
RString DecodeLegacyTextToRString(RString input, Codepage preferredCp);
RString DecodeLegacyTextToRString(RString input, const char* languageName);

} // namespace Poseidon
