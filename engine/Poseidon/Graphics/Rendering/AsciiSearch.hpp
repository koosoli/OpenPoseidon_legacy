#pragma once

#include <cstddef>
#include <string_view>

// ---------------------------------------------------------------------------------------
// Case-insensitive substring search that allocates nothing.
//
// The renderer classifies a section from the TEXT of its material and texture paths --
// "is this a lamp fixture", "is this bark", "is this a window" -- and it does so on the
// per-section submission path, i.e. thousands of times per frame. The obvious spelling of
// that test builds a lowercased std::string copy of BOTH the path and the needle before
// searching:
//
//     std::string haystack(text); std::transform(...tolower...);
//     std::string needle(part);   std::transform(...tolower...);
//     return haystack.find(needle) != npos;
//
// which is two heap allocations and two full passes per QUESTION, and the classifiers ask
// between two and six questions each. Measured on Chernarus+ that spelling was the single
// largest term in `land:obj` (PERF-015).
//
// Equivalence with the allocating form: the paths compared are ASCII asset paths and the
// original lowered with std::tolower under the process's default "C" locale, which maps
// 'A'..'Z' and leaves every other byte -- including everything above 0x7F -- untouched. So
// an ASCII-only fold is not an approximation of the old behaviour, it is the same function.
// tests/unit/.../test_ascii_search.cpp asserts that against std::tolower over all 256 bytes.
namespace Poseidon
{
namespace render
{

constexpr char AsciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when `part` occurs in `text`, comparing case-insensitively. An empty needle is
// never found (matching the std::string::find-based form it replaces, which was guarded by
// an explicit `part.empty()` early-out).
constexpr bool ContainsNoCaseAscii(std::string_view text, std::string_view part)
{
    if (part.empty() || part.size() > text.size())
    {
        return false;
    }
    const std::size_t last = text.size() - part.size();
    const char        head = AsciiLower(part[0]);
    for (std::size_t i = 0; i <= last; ++i)
    {
        if (AsciiLower(text[i]) != head)
        {
            continue;
        }
        std::size_t j = 1;
        while (j < part.size() && AsciiLower(text[i + j]) == AsciiLower(part[j]))
        {
            ++j;
        }
        if (j == part.size())
        {
            return true;
        }
    }
    return false;
}

// Null-tolerant overload: the callers hold `const char*` out of RString / Texture::Name(),
// and a null name must read as "does not contain", never as a fault.
inline bool ContainsNoCaseAscii(const char* text, std::string_view part)
{
    return text != nullptr && ContainsNoCaseAscii(std::string_view(text), part);
}

} // namespace render
} // namespace Poseidon
