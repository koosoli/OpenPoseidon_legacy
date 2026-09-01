// Equivalence tests for the allocation-free case-insensitive search that replaced the
// lowercase-two-copies form on the per-section submission path (PERF-015).
//
// The point of these is not that substring search works. It is that the NEW function
// answers identically to the OLD one on every input the renderer can hand it, because the
// old one is what every material/texture classification in the wgpu backend was tuned
// against and a silently different answer would re-route a section's blend mode.

#include <Poseidon/Graphics/Rendering/AsciiSearch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <string>

using Poseidon::render::AsciiLower;
using Poseidon::render::ContainsNoCaseAscii;

namespace
{
// The exact body that lived in EngineWgpu.cpp, kept here as the oracle.
bool ContainsNoCaseAllocating(const char* text, const std::string& part)
{
    if (!text || part.empty())
        return false;
    std::string haystack(text);
    std::string needle(part);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    return haystack.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("AsciiLower matches std::tolower on every byte the C locale folds", "[graphics][render]")
{
    // The whole equivalence argument rests on this: outside 'A'..'Z' the default locale
    // changes nothing, so an ASCII-only fold is not a narrowing of the old behaviour.
    for (int b = 0; b < 256; ++b)
    {
        const char c = static_cast<char>(b);
        const char viaLocale = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(AsciiLower(c) == viaLocale);
    }
}

TEST_CASE("ContainsNoCaseAscii answers as the allocating form did", "[graphics][render]")
{
    // Real classifier inputs: the needles are the literals the wgpu backend tests for, and
    // the haystacks are paths out of the A1/A2/A3/DayZ corpora in their authored casing.
    const char* haystacks[] = {
        "ca\\structures\\data\\lamps\\lamp_searchlight.rvmat",
        "CA\\Structures\\Data\\Lamps\\LampHalogen.rvmat",
        "a3\\plants_f\\tree\\data\\bark_pine_co.paa",
        "dz\\plants\\tree\\data\\t_picea_bark_CO.paa",
        "ca\\buildings\\data\\okna_ca.paa",
        "ca\\misc\\data\\runwaylights\\light_ca.paa",
        "dz\\structures\\residential\\data\\wall_trunk_co.paa",
        "ca\\plants\\clutter\\data\\trnka_0_ca.paa",
        "",
        "x",
    };
    const char* needles[] = {"\\lamps\\", "lamphalogen", "lamp_searchlight", "\\lights\\",
                             "\\runwaylights\\", "warninglight", "okn", "window",
                             "\\_bark\\", "bark_", "_bark", "\\trunk", "_trunk", "x", "XY"};

    for (const char* h : haystacks)
    {
        for (const char* n : needles)
        {
            INFO("haystack=" << h << " needle=" << n);
            CHECK(ContainsNoCaseAscii(h, n) == ContainsNoCaseAllocating(h, n));
        }
    }
}

TEST_CASE("ContainsNoCaseAscii handles the degenerate inputs the old form guarded", "[graphics][render]")
{
    CHECK_FALSE(ContainsNoCaseAscii(static_cast<const char*>(nullptr), "anything"));
    // An empty needle was never "found" before, and must not become found now: several
    // classifiers pass a configurable token that is empty when the feature is off.
    CHECK_FALSE(ContainsNoCaseAscii("some\\path.paa", ""));
    CHECK_FALSE(ContainsNoCaseAscii("", "a"));
    // Needle longer than the haystack.
    CHECK_FALSE(ContainsNoCaseAscii("ab", "abc"));
    // Overlapping near-misses before the real hit: the loop must not stop at a partial.
    CHECK(ContainsNoCaseAscii("aab_barbark_x", "bark"));
    CHECK(ContainsNoCaseAscii("BARK", "bark"));
    CHECK(ContainsNoCaseAscii("bark", "BARK"));
    // Match at the very end of the string.
    CHECK(ContainsNoCaseAscii("path\\_trunk", "_trunk"));
}
