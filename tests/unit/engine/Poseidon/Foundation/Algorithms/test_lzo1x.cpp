// test_lzo1x.cpp - AST-013: LZO1X decompression for compressed PAA mips.
//
// Streams here are hand-built from the format so the expected output is known by
// construction rather than by round-tripping through the same code under test.
// The corpus check that matters lives outside CI, where the corpora are: all 98
// compressed mips in the Arma 3 sample set decompress to exactly the byte count
// their DXT dimensions require, and 124/124 sample textures then decode.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <cstdint>
#include <string>
#include <vector>

using Poseidon::Foundation::Lzo1x;

namespace
{
// 0x11 0x00 0x00 is the end-of-stream opcode: a match with a zero distance.
const std::vector<uint8_t> kEnd = {0x11, 0x00, 0x00};

std::string run(std::vector<uint8_t> stream, size_t capacity = 64)
{
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    std::vector<uint8_t> out(capacity);
    const size_t         n = Lzo1x::Decompress(stream.data(), stream.size(), out.data(), out.size());
    return std::string(reinterpret_cast<const char*>(out.data()), n);
}
} // namespace

TEST_CASE("LZO1X: a leading literal run", "[foundation][lzo][ast-013]")
{
    // A first byte above 17 is a literal run of (byte - 17).
    REQUIRE(run({0x15, 'A', 'B', 'C', 'D'}) == "ABCD");
}

TEST_CASE("LZO1X: a back-reference copies earlier output", "[foundation][lzo][ast-013]")
{
    // Opcode 0x6C: length (0x6C >> 5) - 1 + 2 = 4, distance 1 + ((0x6C >> 2) & 7) = 4.
    REQUIRE(run({0x15, 'A', 'B', 'C', 'D', 0x6C, 0x00}) == "ABCDABCD");
}

TEST_CASE("LZO1X: a match may overlap its own output", "[foundation][lzo][ast-013]")
{
    // Distance 1, length 4. This is how a run is encoded, so the copy has to be
    // byte-at-a-time -- a bulk copy would read the pre-match bytes four times over
    // instead of the byte it just wrote.
    REQUIRE(run({0x15, 'A', 'B', 'C', 'D', 0x60, 0x00}) == "ABCDDDDD");
}

TEST_CASE("LZO1X: a back-reference before the output start is refused", "[foundation][lzo][ast-013]")
{
    // Distance 2044 against 4 bytes of output. This is the classic LZ decompressor
    // failure and it reads out of bounds long before it writes anything wrong, so
    // it must be rejected rather than clamped.
    std::vector<uint8_t> stream = {0x15, 'A', 'B', 'C', 'D', 0x6C, 0xFF};
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    std::vector<uint8_t> out(64);
    REQUIRE(Lzo1x::Decompress(stream.data(), stream.size(), out.data(), out.size()) == 0);
}

TEST_CASE("LZO1X: output is never written past the destination", "[foundation][lzo][ast-013]")
{
    // The same valid stream against a destination too small for it. Refusing beats
    // a partial fill: the caller checks the produced length against the exact block
    // count, and a truncated buffer would decode as garbage in the tail.
    std::vector<uint8_t> stream = {0x15, 'A', 'B', 'C', 'D', 0x6C, 0x00};
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    std::vector<uint8_t> out(6); // needs 8
    REQUIRE(Lzo1x::Decompress(stream.data(), stream.size(), out.data(), out.size()) == 0);
}

TEST_CASE("LZO1X: truncated and empty input are refused", "[foundation][lzo][ast-013]")
{
    std::vector<uint8_t> out(64);
    REQUIRE(Lzo1x::Decompress(nullptr, 0, out.data(), out.size()) == 0);

    // A literal run promising more bytes than the stream holds.
    const std::vector<uint8_t> shortRun = {0x20, 'A', 'B'};
    REQUIRE(Lzo1x::Decompress(shortRun.data(), shortRun.size(), out.data(), out.size()) == 0);
}

TEST_CASE("LZO1X: the end-of-stream opcode terminates", "[foundation][lzo][ast-013]")
{
    // Bytes after the marker must not be decoded: a mip's payload is followed by
    // the next mip's header, and running on would consume it.
    std::vector<uint8_t> stream = {0x15, 'A', 'B', 'C', 'D'};
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    stream.insert(stream.end(), {0xFF, 0xFF, 0xFF, 0xFF});
    std::vector<uint8_t> out(64);
    const size_t         n = Lzo1x::Decompress(stream.data(), stream.size(), out.data(), out.size());
    REQUIRE(n == 4);
}

TEST_CASE("LZO1X: reports the boundary after an end marker", "[foundation][lzo][ast-013]")
{
    std::vector<uint8_t> stream = {0x15, 'A', 'B', 'C', 'D'};
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    stream.insert(stream.end(), {0xFF, 0xFF});
    std::vector<uint8_t> out(64);
    size_t consumed = 0;
    REQUIRE(Lzo1x::Decompress(stream.data(), stream.size(), out.data(), out.size(), &consumed) == 4);
    REQUIRE(consumed == 8);
}
