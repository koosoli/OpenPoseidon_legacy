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
#include "lzo1x_reference.hpp"

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

namespace
{
void differential(const std::vector<uint8_t>& input, size_t prefix, size_t capacity, size_t misalign)
{
    std::vector<uint8_t> source(input.size() + 3, 0xC3);
    for (size_t i = 0; i < input.size(); ++i) source[i + 1] = input[i];
    const size_t guard = 16 + misalign;
    std::vector<uint8_t> actual(capacity + guard + 16, 0xA5), expected = actual;
    size_t gotConsumed = SIZE_MAX, expectedConsumed = SIZE_MAX;
    const size_t got = Lzo1x::Decompress(source.data() + 1, prefix,
                                        actual.data() + guard, capacity, &gotConsumed);
    const size_t want = Lzo1xTestReference::FrozenLzo1x::Decompress(source.data() + 1, prefix,
                                        expected.data() + guard, capacity, &expectedConsumed);
    REQUIRE(got == want);
    REQUIRE(gotConsumed == expectedConsumed);
    REQUIRE(actual == expected); // Includes partial failure writes and both canaries.
    for (size_t i = 0; i < guard; ++i) REQUIRE(actual[i] == 0xA5);
    for (size_t i = guard + capacity; i < actual.size(); ++i) REQUIRE(actual[i] == 0xA5);
}
std::vector<uint8_t> matchStream(unsigned distance, unsigned length, unsigned literalCount = 16)
{
    std::vector<uint8_t> stream{static_cast<uint8_t>(17 + literalCount)};
    for (unsigned i = 0; i < literalCount; ++i) stream.push_back(static_cast<uint8_t>('A' + i % 23));
    unsigned encodedLength = length - 2;
    if (encodedLength <= 31) stream.push_back(static_cast<uint8_t>(32 + encodedLength));
    else {
        stream.push_back(32);
        encodedLength -= 31;
        while (encodedLength > 255) { stream.push_back(0); encodedLength -= 255; }
        stream.push_back(static_cast<uint8_t>(encodedLength));
    }
    const unsigned encodedDistance = (distance - 1) << 2;
    stream.push_back(static_cast<uint8_t>(encodedDistance));
    stream.push_back(static_cast<uint8_t>(encodedDistance >> 8));
    stream.insert(stream.end(), kEnd.begin(), kEnd.end());
    return stream;
}
}

TEST_CASE("LZO fast matches preserve disjoint boundaries and forward overlapping recurrence", "[foundation][lzo-fastpath]")
{
    for (const unsigned distance : {1u, 2u, 3u, 4u, 8u, 16u}) {
        for (const unsigned length : {3u, 4u, 8u, 16u, 33u, 305u, 8192u}) {
            const auto stream = matchStream(distance, length);
            std::vector<uint8_t> output(16 + length, 0);
            size_t consumed = 0;
            REQUIRE(Lzo1x::Decompress(stream.data(), stream.size(), output.data(), output.size(), &consumed) == output.size());
            REQUIRE(consumed == stream.size());
            for (size_t i = 16; i < output.size(); ++i) REQUIRE(output[i] == output[i - distance]);
            differential(stream, stream.size(), output.size(), 1);
        }
    }
}

TEST_CASE("LZO fast matches preserve every truncated prefix and bounded partial failure output", "[foundation][lzo-fastpath]")
{
    for (const auto& stream : {matchStream(1, 305), matchStream(2, 33), matchStream(16, 16), matchStream(16, 15)}) {
        for (size_t prefix = 0; prefix <= stream.size(); ++prefix)
            for (size_t capacity : {size_t(0), size_t(1), size_t(15), size_t(16), size_t(17), size_t(31), size_t(32), size_t(49), size_t(320), size_t(321), size_t(322)})
                differential(stream, prefix, capacity, prefix & 1);
    }
}

TEST_CASE("LZO fast matches agree with frozen decoder on fixed seed malformed inputs", "[foundation][lzo-fastpath]")
{
    uint32_t seed = 0x63A17F29u;
    const auto next = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    for (unsigned sample = 0; sample < 192; ++sample) {
        std::vector<uint8_t> stream(1 + next() % 64);
        for (auto& byte : stream) byte = static_cast<uint8_t>(next());
        if (sample % 3 == 0) { stream[0] = 21; if (stream.size() < 5) stream.resize(5, 'Q'); }
        for (const size_t capacity : {size_t(0), size_t(4), size_t(17), size_t(96)})
            differential(stream, stream.size(), capacity, sample & 1);
    }
}

TEST_CASE("LZO fast matches leave literal source alias semantics unchanged", "[foundation][lzo-fastpath]")
{
    const auto stream = matchStream(2, 33);
    for (const size_t offset : {size_t(0), size_t(1), size_t(4), size_t(16)}) {
        std::vector<uint8_t> actual(128, 0xA5);
        for (size_t i = 0; i < stream.size(); ++i) actual[16 + i] = stream[i];
        auto expected = actual;
        size_t aConsumed = 0, bConsumed = 0;
        const auto a = Lzo1x::Decompress(actual.data() + 16, stream.size(), actual.data() + 16 + offset, 64, &aConsumed);
        const auto b = Lzo1xTestReference::FrozenLzo1x::Decompress(expected.data() + 16, stream.size(), expected.data() + 16 + offset, 64, &bConsumed);
        REQUIRE(a == b);
        REQUIRE(aConsumed == bConsumed);
        REQUIRE(actual == expected);
    }
}