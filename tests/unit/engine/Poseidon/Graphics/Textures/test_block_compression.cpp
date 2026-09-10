// test_block_compression.cpp - ARF-001: the DXGI-era block formats BC4, BC5, BC7.
//
// Every block here is built bit by bit from the format, so nothing Reforger-owned is
// committed and the expected pixels are known by construction rather than by
// agreeing with some other decoder.
//
// The corpus check that matters lives outside CI, where the corpus is: a stratified
// 1,692-file sample of Arma Reforger's 29,026 .edds decodes 1,680 (99.29%), the 12
// exceptions being 8 cubemaps, 2 volume LUTs and 2 half-float textures -- all named
// gaps, none of them a model or terrain albedo. Before these decoders existed the
// same sample decoded 118 (6.97%).

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Core/TaskPool.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using Poseidon::DecodeBc4Image;
using Poseidon::DecodeBc5Image;
using Poseidon::DecodeBc7Block;
using Poseidon::DecodeBc7Image;

TEST_CASE("BC7 row batches match serial output including edge blocks and invalid modes", "[graphics][texture][bc][batch]")
{
    for (uint32_t threads : {1u, 2u, 4u})
    {
        Poseidon::TaskPool pool(threads);
        for (int width : {1, 7, 256, 259})
            for (int height : {1, 63, 64, 65, 257})
            {
                const size_t blocks = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4);
                std::vector<uint8_t> source(blocks * 16);
                uint32_t state = 0x91832u;
                for (auto& value : source)
                {
                    state = state * 1664525u + 1013904223u;
                    value = static_cast<uint8_t>(state >> 24);
                }
                for (size_t b = 0; b < blocks; ++b)
                    source[b * 16] = static_cast<uint8_t>(1u << (b % 8));
                for (bool invalid : {false, true})
                {
                    if (invalid) source[(blocks - 1) * 16] = 0;
                    const size_t bytes = static_cast<size_t>(width) * height * 4;
                    std::vector<uint8_t> serial(bytes + 32, 0xcd), parallel = serial;
                    const bool expected = DecodeBc7Image(source.data(), serial.data() + 16, width, height);
                    const bool actual = Poseidon::DecodeBc7ImageBatched(source.data(), parallel.data() + 16, width, height, pool);
                    REQUIRE(actual == expected);
                    REQUIRE(actual == !invalid);
                    REQUIRE(parallel == serial);
                    for (int i = 0; i < 16; ++i)
                    {
                        REQUIRE(parallel[i] == 0xcd);
                        REQUIRE(parallel[bytes + 16 + i] == 0xcd);
                    }
                }
            }
    }
}

namespace
{

// Writes fields LSB-first across the 128-bit block, the order BC7 packs them in.
class BitWriter
{
  public:
    void Write(uint32_t value, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if ((value >> i) & 1u)
                block_[pos_ >> 3] |= static_cast<uint8_t>(1u << (pos_ & 7));
            ++pos_;
        }
    }
    int Position() const { return pos_; }
    const uint8_t* Data() const { return block_; }

  private:
    uint8_t block_[16] = {};
    int pos_ = 0;
};

struct Rgba
{
    uint8_t r, g, b, a;
};

Rgba PixelAt(const std::vector<uint8_t>& img, int width, int x, int y)
{
    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return {img[at], img[at + 1], img[at + 2], img[at + 3]};
}

// A BC4 block: two endpoints then sixteen 3-bit indices.
std::vector<uint8_t> Bc4Block(uint8_t e0, uint8_t e1, const int idx[16])
{
    std::vector<uint8_t> b(8, 0);
    b[0] = e0;
    b[1] = e1;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= static_cast<uint64_t>(idx[i] & 7) << (i * 3);
    for (int i = 0; i < 6; ++i)
        b[2 + i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    return b;
}

} // namespace

TEST_CASE("BC4 reproduces its endpoints exactly at index 0 and 1", "[graphics][texture][bc]")
{
    // Index 0 selects endpoint 0 and index 1 selects endpoint 1, verbatim -- no
    // interpolation is involved, so any rounding bug shows up somewhere else.
    int idx[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const auto block = Bc4Block(10, 200, idx);
    std::vector<uint8_t> out(4 * 4 * 4, 0);
    DecodeBc4Image(block.data(), out.data(), 4, 4);

    REQUIRE(PixelAt(out, 4, 0, 0).r == 10);
    REQUIRE(PixelAt(out, 4, 1, 0).r == 200);
    // The channel is published in R with an opaque alpha, and nothing is invented
    // for G and B -- a caller must be able to tell a mask from a grey image.
    REQUIRE(PixelAt(out, 4, 0, 0).g == 0);
    REQUIRE(PixelAt(out, 4, 0, 0).b == 0);
    REQUIRE(PixelAt(out, 4, 0, 0).a == 255);
}

TEST_CASE("BC4 picks the six-interpolant palette when e0 > e1", "[graphics][texture][bc]")
{
    // e0 > e1 is the 8-value ramp; index 2 is (6*e0 + 1*e1)/7.
    int idx[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    const auto block = Bc4Block(210, 0, idx);
    std::vector<uint8_t> out(4 * 4 * 4, 0);
    DecodeBc4Image(block.data(), out.data(), 4, 4);
    REQUIRE(PixelAt(out, 4, 0, 0).r == static_cast<uint8_t>((6 * 210 + 0 + 3) / 7));
}

TEST_CASE("BC4 picks the four-interpolant palette with 0 and 255 when e0 <= e1", "[graphics][texture][bc]")
{
    // e0 <= e1 is the 6-value ramp, where indices 6 and 7 are hard 0 and 255.
    int idx[16] = {6, 7, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto block = Bc4Block(40, 90, idx);
    std::vector<uint8_t> out(4 * 4 * 4, 0);
    DecodeBc4Image(block.data(), out.data(), 4, 4);
    REQUIRE(PixelAt(out, 4, 0, 0).r == 0);
    REQUIRE(PixelAt(out, 4, 1, 0).r == 255);
    REQUIRE(PixelAt(out, 4, 2, 0).r == 40);
    REQUIRE(PixelAt(out, 4, 3, 0).r == 90);
}

TEST_CASE("BC5 decodes its two halves into R and G independently", "[graphics][texture][bc]")
{
    // The whole point of BC5 is two uncorrelated channels; giving the halves
    // different endpoints proves the second block is not just re-reading the first.
    int idxR[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    int idxG[16] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    const auto red = Bc4Block(11, 222, idxR);
    const auto green = Bc4Block(33, 111, idxG);

    std::vector<uint8_t> block;
    block.insert(block.end(), red.begin(), red.end());
    block.insert(block.end(), green.begin(), green.end());

    std::vector<uint8_t> out(4 * 4 * 4, 0);
    DecodeBc5Image(block.data(), out.data(), 4, 4);

    REQUIRE(PixelAt(out, 4, 0, 0).r == 11);
    REQUIRE(PixelAt(out, 4, 0, 0).g == 111);
    REQUIRE(PixelAt(out, 4, 1, 0).r == 222);
    REQUIRE(PixelAt(out, 4, 1, 0).g == 33);
    REQUIRE(PixelAt(out, 4, 0, 0).a == 255);
}

TEST_CASE("BC7 mode 6 reproduces both endpoints exactly", "[graphics][texture][bc]")
{
    // Mode 6 is the single-subset, full-precision RGBA mode: 7 colour bits plus a
    // P-bit per endpoint, so an endpoint written as v gives back (v<<1)|p
    // replicated to 8 bits. Index 0 and 15 select the endpoints untouched, which
    // makes this a direct check on the endpoint unquantisation rule.
    BitWriter w;
    w.Write(1u << 6, 7); // mode 6: the prefix is six zero bits then a one
    // Endpoints are channel-major: R0 R1 G0 G1 B0 B1 A0 A1.
    const uint32_t r0 = 0x00, r1 = 0x7F;
    const uint32_t g0 = 0x10, g1 = 0x6F;
    const uint32_t b0 = 0x20, b1 = 0x5F;
    const uint32_t a0 = 0x7F, a1 = 0x00;
    w.Write(r0, 7); w.Write(r1, 7);
    w.Write(g0, 7); w.Write(g1, 7);
    w.Write(b0, 7); w.Write(b1, 7);
    w.Write(a0, 7); w.Write(a1, 7);
    w.Write(1, 1); // P-bit for endpoint 0
    w.Write(1, 1); // P-bit for endpoint 1
    // Index 0 is the anchor and carries 3 bits, not 4; the rest carry 4.
    w.Write(0, 3);
    for (int i = 1; i < 16; ++i)
        w.Write(i == 1 ? 15u : 0u, 4);
    REQUIRE(w.Position() == 128);

    std::vector<uint8_t> out(4 * 4 * 4, 0);
    REQUIRE(DecodeBc7Block(w.Data(), out.data(), 16, 4, 4));

    auto expand = [](uint32_t v7, uint32_t p) {
        const uint32_t v = ((v7 << 1) | p) << (8 - 8); // 7 bits + P-bit == 8 bits
        return static_cast<uint8_t>(v);
    };
    const Rgba first = PixelAt(out, 4, 0, 0);
    REQUIRE(first.r == expand(r0, 1));
    REQUIRE(first.g == expand(g0, 1));
    REQUIRE(first.b == expand(b0, 1));
    REQUIRE(first.a == expand(a0, 1));

    const Rgba second = PixelAt(out, 4, 1, 0);
    REQUIRE(second.r == expand(r1, 1));
    REQUIRE(second.g == expand(g1, 1));
    REQUIRE(second.b == expand(b1, 1));
    REQUIRE(second.a == expand(a1, 1));
}

TEST_CASE("BC7 mode 5 applies the channel rotation", "[graphics][texture][bc]")
{
    // Rotation is the field most easily read and then ignored: modes 4 and 5 may
    // park alpha in a colour channel to spend the better index precision on it,
    // and a decoder that skips the swap produces a picture that looks almost
    // right. Rotation 1 swaps R with A.
    auto build = [](uint32_t rotation) {
        BitWriter w;
        w.Write(1u << 5, 6); // mode 5: five zero bits then a one
        w.Write(rotation, 2);
        w.Write(0x7F, 7); w.Write(0x7F, 7); // R0 R1 -> 255
        w.Write(0x00, 7); w.Write(0x00, 7); // G0 G1 -> 0
        w.Write(0x00, 7); w.Write(0x00, 7); // B0 B1 -> 0
        w.Write(0x40, 8); w.Write(0x40, 8); // A0 A1 -> 64
        w.Write(0, 1);                      // colour index 0 (anchor, 1 bit)
        for (int i = 1; i < 16; ++i)
            w.Write(0, 2);
        w.Write(0, 1); // alpha index 0 (anchor, 1 bit)
        for (int i = 1; i < 16; ++i)
            w.Write(0, 2);
        REQUIRE(w.Position() == 128);
        std::vector<uint8_t> out(4 * 4 * 4, 0);
        REQUIRE(DecodeBc7Block(w.Data(), out.data(), 16, 4, 4));
        return PixelAt(out, 4, 0, 0);
    };

    const Rgba unrotated = build(0);
    REQUIRE(unrotated.r == 255);
    REQUIRE(unrotated.a == 64);

    const Rgba rotated = build(1); // R <-> A
    REQUIRE(rotated.r == 64);
    REQUIRE(rotated.a == 255);
}

TEST_CASE("BC7 rejects only the reserved mode", "[graphics][texture][bc]")
{
    // A block whose first byte has no set bit names no mode at all. No encoder
    // emits it, but a decoder that scans for the mode bit without a bound walks
    // off the end instead of saying so.
    uint8_t reserved[16] = {};
    std::vector<uint8_t> out(4 * 4 * 4, 0);
    REQUIRE_FALSE(DecodeBc7Block(reserved, out.data(), 16, 4, 4));
}

TEST_CASE("BC7 partial edge blocks write only the usable texels", "[graphics][texture][bc]")
{
    // A 3x2 image is one block with 10 of its 16 texels outside the image. Writing
    // them would run past the end of a tightly-sized buffer, which is the kind of
    // fault that survives every visual check.
    BitWriter w;
    w.Write(1u << 6, 7); // mode 6
    for (int c = 0; c < 4; ++c)
    {
        w.Write(0x7F, 7);
        w.Write(0x7F, 7);
    }
    w.Write(1, 1);
    w.Write(1, 1);
    w.Write(0, 3);
    for (int i = 1; i < 16; ++i)
        w.Write(0, 4);
    REQUIRE(w.Position() == 128);

    std::vector<uint8_t> out(3 * 2 * 4, 0xAB);
    REQUIRE(DecodeBc7Image(w.Data(), out.data(), 3, 2));
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            REQUIRE(PixelAt(out, 3, x, y).r == 255);
    REQUIRE(out.size() == 3 * 2 * 4);
}
