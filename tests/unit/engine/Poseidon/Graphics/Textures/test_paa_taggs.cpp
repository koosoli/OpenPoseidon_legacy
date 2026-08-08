// test_paa_taggs.cpp - AST-013: PAA TAGG capture and compressed-mip recognition.
//
// The decoder skipped the whole TAGG block. SWIZ says how stored channels map onto
// meaning and FLAG carries the alpha policy, so discarding them means guessing
// both. Measured across 3,283 textures read from the Arma 3 archives:
// AVGC/MAXC/OFFS on every one, FLAG on 1,345, SWIZ on 1,146.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include "test_fixtures.hpp"
#include <string>

using Poseidon::PAAInfo;
using Poseidon::ReadPAAInfo;

TEST_CASE("PAA: TAGG values are captured, not skipped", "[graphics][paa][ast-013]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/taggs_full.paa"), info));

    REQUIRE(info.taggs.hasAvgColor);
    REQUIRE(info.taggs.avgColor == 0xFF8080FEu);
    REQUIRE(info.taggs.hasMaxColor);
    REQUIRE(info.taggs.hasOffsets);

    // FLAG is the alpha/transparency policy. Only values 1 and 2 occur in the
    // sampled corpus; it is carried verbatim rather than reinterpreted here.
    REQUIRE(info.taggs.hasFlags);
    REQUIRE(info.taggs.flags == 1u);

    // SWIZ is preserved byte for byte. It is deliberately NOT applied: the encoding
    // is ambiguous across the observed values and no local fixture settles it, so
    // applying a guess would silently corrupt every normal map it touched.
    REQUIRE(info.taggs.hasSwizzle);
    REQUIRE(info.taggs.swizzle[0] == 0x05);
    REQUIRE(info.taggs.swizzle[1] == 0x04);
    REQUIRE(info.taggs.swizzle[2] == 0x02);
    REQUIRE(info.taggs.swizzle[3] == 0x03);
    REQUIRE_FALSE(info.taggs.isIdentitySwizzle());

    // Reading the tags must leave the mip chain readable behind them.
    REQUIRE(info.width == 4);
    REQUIRE(info.height == 4);
    REQUIRE(info.mipmapCount == 1);
}

TEST_CASE("PAA: an unknown TAGG is recorded and stepped over", "[graphics][paa][ast-013]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/taggs_unknown.paa"), info));

    // Captured by name so a fixture needing it is visible, rather than dropped.
    REQUIRE(info.taggs.hasAvgColor);
    REQUIRE(info.taggs.unknown.size() == 1);
    REQUIRE(info.taggs.unknown[0] == "ZZZZ");

    // Stepped over by its declared size: an unknown tag of an odd length must not
    // desynchronise the mip chain that follows it.
    REQUIRE(info.width == 4);
    REQUIRE(info.height == 4);
    REQUIRE(info.mipmapCount == 1);
}

TEST_CASE("PAA: a file with no TAGGs is still valid", "[graphics][paa][ast-013]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/taggs_none.paa"), info));
    REQUIRE_FALSE(info.taggs.hasAvgColor);
    REQUIRE_FALSE(info.taggs.hasSwizzle);
    REQUIRE(info.taggs.unknown.empty());
    REQUIRE(info.taggs.isIdentitySwizzle()); // absent swizzle means no rearrangement
    REQUIRE(info.width == 4);
}

TEST_CASE("PAA: a compressed mip is recognised, not read as a huge width", "[graphics][paa][ast-013]")
{
    // Bit 15 of the stored width marks compressed data. Unmasked, a 2048-wide mip
    // reads as 34816 and gets reported as an "extreme texture size" -- blaming the
    // dimension for what is really an unsupported compression, and sending anyone
    // reading the log after a corrupt file that is perfectly valid.
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/taggs_compressed_mip.paa"), info));

    REQUIRE(info.hasCompressedMips);
    REQUIRE(info.width == 4);   // masked, not 0x8004
    REQUIRE(info.height == 4);
}
