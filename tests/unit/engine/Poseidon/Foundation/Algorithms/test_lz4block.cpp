// test_lz4block.cpp - DZ-002: LZ4 block decompression for Enfusion .edds mips.
//
// Blocks here are hand-built from the format so the expected output is known by
// construction rather than by round-tripping through the same code under test. The
// corpus check that matters lives outside CI, where the corpus is: all 1,493 LZ4
// mips across the 454 .edds files in a retail DayZ install decompress to exactly the
// byte count their dimensions and block format require.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Algorithms/Lz4Block.hpp>
#include <cstdint>
#include <string>
#include <vector>

using Poseidon::Foundation::Lz4Block;

namespace
{
// A token's high nibble is the literal count, its low nibble the match length minus
// the format's 4-byte minimum.
constexpr uint8_t Token(uint8_t literals, uint8_t matchExtra)
{
    return static_cast<uint8_t>((literals << 4) | matchExtra);
}

std::string Run(const std::vector<uint8_t>& block, size_t capacity = 64)
{
    std::vector<uint8_t> out(capacity);
    const size_t n = Lz4Block::Decompress(block.data(), block.size(), out.data(), out.size());
    return std::string(reinterpret_cast<const char*>(out.data()), n);
}
} // namespace

TEST_CASE("LZ4: a literal-only block", "[foundation][lz4][dz-002]")
{
    REQUIRE(Run({Token(4, 0), 'A', 'B', 'C', 'D'}) == "ABCD");
}

TEST_CASE("LZ4: a match copies earlier output", "[foundation][lz4][dz-002]")
{
    // 4 literals, then distance 4, match length 4 + 4 = 8.
    REQUIRE(Run({Token(4, 4), 'A', 'B', 'C', 'D', 0x04, 0x00}) == "ABCDABCDABCD");
}

TEST_CASE("LZ4: a match may overlap its own output", "[foundation][lz4][dz-002]")
{
    // Distance 1 is a run fill: the copy has to be byte at a time, because the
    // bytes it reads are the bytes it just wrote.
    REQUIRE(Run({Token(1, 1), 'A', 0x01, 0x00}) == "AAAAAA");
}

TEST_CASE("LZ4: literal and match lengths extend past their nibble", "[foundation][lz4][dz-002]")
{
    // Literal nibble 15 continues into following bytes until one is not 0xFF.
    std::vector<uint8_t> block = {Token(15, 0), 5}; // 15 + 5 = 20 literals
    for (int i = 0; i < 20; ++i)
        block.push_back(static_cast<uint8_t>('a' + i));
    const std::string out = Run(block);
    REQUIRE(out.size() == 20);
    REQUIRE(out == "abcdefghijklmnopqrst");
}

TEST_CASE("LZ4: sub-blocks are linked through dstOffset", "[foundation][lz4][dz-002]")
{
    // This is the property .edds actually depends on, and the one that is invisible
    // until a texture exceeds 64 KiB: a block may match against output an *earlier*
    // block produced. Decoded independently the second block below is malformed --
    // its back-reference points before its own start.
    std::vector<uint8_t> out(64);

    const std::vector<uint8_t> first = {Token(4, 0), 'A', 'B', 'C', 'D'};
    const size_t n1 = Lz4Block::Decompress(first.data(), first.size(), out.data(), out.size(), 0);
    REQUIRE(n1 == 4);

    // No literals; distance 4, length 4 -- entirely a reference into `first`.
    const std::vector<uint8_t> second = {Token(0, 0), 0x04, 0x00};
    const size_t n2 = Lz4Block::Decompress(second.data(), second.size(), out.data(), out.size(), n1);
    REQUIRE(n2 == 4);

    REQUIRE(std::string(reinterpret_cast<const char*>(out.data()), n1 + n2) == "ABCDABCD");

    // The same block at offset 0 has nothing to reach back to, and is rejected.
    REQUIRE(Lz4Block::Decompress(second.data(), second.size(), out.data(), out.size(), 0) == 0);
}

TEST_CASE("LZ4: a back-reference before the buffer is rejected", "[foundation][lz4][dz-002]")
{
    // Distance 8 with only 4 bytes of output would read memory this block never
    // produced -- the classic way an LZ decompressor reads out of bounds.
    REQUIRE(Run({Token(4, 0), 'A', 'B', 'C', 'D', 0x08, 0x00}) == "");
}

TEST_CASE("LZ4: a zero distance is rejected", "[foundation][lz4][dz-002]")
{
    REQUIRE(Run({Token(4, 0), 'A', 'B', 'C', 'D', 0x00, 0x00}) == "");
}

TEST_CASE("LZ4: output is never written past capacity", "[foundation][lz4][dz-002]")
{
    // 20 literals declared, 8 bytes of room.
    std::vector<uint8_t> block = {Token(15, 0), 5};
    for (int i = 0; i < 20; ++i)
        block.push_back('x');
    REQUIRE(Run(block, 8) == "");

    // And a match that would run past the end is refused rather than truncated.
    REQUIRE(Run({Token(1, 15), 'A', 0x01, 0x00, 200}, 8) == "");
}

TEST_CASE("LZ4: truncated input is rejected rather than half-decoded", "[foundation][lz4][dz-002]")
{
    // Literal run claims 4 bytes, only 2 follow.
    REQUIRE(Run({Token(4, 0), 'A', 'B'}) == "");
    // A single distance byte after a literal run cannot form an offset.
    REQUIRE(Run({Token(2, 0), 'A', 'B', 0x01}) == "");
}
