#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <vector>

#include <algorithm>
#include <cstring>

namespace Poseidon
{
namespace
{

// ---------------------------------------------------------------- BC4 / BC5 --

void DecodeBc4Palette(uint8_t e0, uint8_t e1, uint8_t palette[8])
{
    palette[0] = e0;
    palette[1] = e1;
    if (e0 > e1)
    {
        for (int i = 1; i <= 6; ++i)
            palette[i + 1] = static_cast<uint8_t>(((7 - i) * e0 + i * e1 + 3) / 7);
    }
    else
    {
        for (int i = 1; i <= 4; ++i)
            palette[i + 1] = static_cast<uint8_t>(((5 - i) * e0 + i * e1 + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
}

// ------------------------------------------------------------------- BC7 ----
//
// The tables below are the BC7 format's own, not derived: the 2- and 3-subset
// partition sets, the fixed anchor indices per partition, and the interpolation
// weights. They are reproduced from the block-compression specification.

// clang-format off
const uint8_t kPartition2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}, {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
    {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1}, {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1}, {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0}, {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0}, {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},
    {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0}, {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0}, {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0}, {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1}, {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
    {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0}, {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0}, {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
    {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1}, {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0}, {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0}, {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0}, {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
    {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1}, {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0}, {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0}, {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0}, {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1}, {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
    {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1}, {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1}, {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0}, {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};

const uint8_t kPartition3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},
    {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1}, {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},
    {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2}, {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2}, {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},
    {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},
    {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0}, {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2}, {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},
    {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
    {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0}, {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1}, {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},
    {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},
    {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2}, {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2}, {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},
    {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},
    {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2}, {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};

// Subset 0's anchor is always index 0. These give subset 1's (and, for the
// 3-subset partitions, subset 2's) fixed anchor position.
const uint8_t kAnchor2[64] = {
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,
     6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15,
};
const uint8_t kAnchor3a[64] = {
     3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,
     3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
     8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,
     3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3,
};
const uint8_t kAnchor3b[64] = {
    15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,
    15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
    15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,
    15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8,
};

const uint8_t kWeight2[4]  = {0, 21, 43, 64};
const uint8_t kWeight3[8]  = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bc7Mode
{
    uint8_t subsets;
    uint8_t partitionBits;
    uint8_t rotationBits;
    uint8_t indexSelectionBits;
    uint8_t colourBits;
    uint8_t alphaBits;
    uint8_t endpointPBits; // one P-bit per endpoint
    uint8_t sharedPBits;   // one P-bit per subset, shared by both its endpoints
    uint8_t indexBits;
    uint8_t indexBits2;
};

const Bc7Mode kModes[8] = {
    //         ns pb rb isb cb ab epb spb ib ib2
    /* 0 */ {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    /* 1 */ {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    /* 2 */ {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    /* 3 */ {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    /* 4 */ {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    /* 5 */ {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    /* 6 */ {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    /* 7 */ {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};
// clang-format on

// A little-endian bit reader over the 16-byte block. BC7 packs every field LSB
// first across the whole 128 bits, so a running cursor is the only sane way to
// read it -- the field widths change per mode.
class BitReader
{
  public:
    explicit BitReader(const uint8_t* data) : data_(data) {}

    uint32_t Read(int count)
    {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i)
        {
            const uint32_t bit = (data_[pos_ >> 3] >> (pos_ & 7)) & 1u;
            value |= bit << i;
            ++pos_;
        }
        return value;
    }

    int Position() const { return pos_; }

  private:
    const uint8_t* data_;
    int pos_ = 0;
};

uint8_t Unquantise(uint32_t value, int bits)
{
    // Replicate the high bits down, the format's own expansion rule.
    uint32_t v = value << (8 - bits);
    return static_cast<uint8_t>(v | (v >> bits));
}

uint8_t Interpolate(uint8_t a, uint8_t b, uint8_t weight)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(64 - weight) * a + static_cast<uint32_t>(weight) * b + 32) >> 6);
}

} // namespace

void DecodeBc4Block(const uint8_t* block, uint8_t* out, size_t rowPitch, size_t stride, int usableW, int usableH)
{
    uint8_t palette[8];
    DecodeBc4Palette(block[0], block[1], palette);

    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<uint64_t>(block[2 + i]) << (i * 8);

    for (int y = 0; y < 4; ++y)
    {
        if (y >= usableH)
            break;
        for (int x = 0; x < 4; ++x)
        {
            if (x >= usableW)
                break;
            const int index = static_cast<int>((bits >> ((y * 4 + x) * 3)) & 7u);
            out[static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x) * stride] = palette[index];
        }
    }
}

void DecodeBc5Block(const uint8_t* block, uint8_t* out, size_t rowPitch, size_t stride, int usableW, int usableH)
{
    DecodeBc4Block(block, out, rowPitch, stride, usableW, usableH);
    DecodeBc4Block(block + 8, out + 1, rowPitch, stride, usableW, usableH);
}

bool DecodeBc7Block(const uint8_t* block, uint8_t* out, size_t rowPitch, int usableW, int usableH)
{
    // The mode is a unary prefix: the number of leading zero bits before the
    // first 1. A byte-0 of zero means no mode bit is set anywhere in the first
    // eight, which is the reserved encoding.
    int mode = 0;
    while (mode < 8 && ((block[0] >> mode) & 1u) == 0)
        ++mode;
    if (mode == 8)
        return false;

    const Bc7Mode& m = kModes[mode];
    BitReader reader(block);
    reader.Read(mode + 1); // consume the unary mode prefix

    const uint32_t partition = m.partitionBits ? reader.Read(m.partitionBits) : 0;
    const uint32_t rotation = m.rotationBits ? reader.Read(m.rotationBits) : 0;
    const uint32_t indexSelection = m.indexSelectionBits ? reader.Read(m.indexSelectionBits) : 0;

    const int endpointCount = m.subsets * 2;

    // Endpoints are stored channel-major: every R, then every G, then every B,
    // then every A. Reading them endpoint-major is the classic way to get a
    // decoder that looks right and produces colour noise.
    uint32_t raw[6][4] = {};
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < endpointCount; ++e)
            raw[e][c] = reader.Read(m.colourBits);
    if (m.alphaBits)
    {
        for (int e = 0; e < endpointCount; ++e)
            raw[e][3] = reader.Read(m.alphaBits);
    }

    int colourBits = m.colourBits;
    int alphaBits = m.alphaBits;
    if (m.endpointPBits)
    {
        for (int e = 0; e < endpointCount; ++e)
        {
            const uint32_t p = reader.Read(1);
            for (int c = 0; c < 4; ++c)
                raw[e][c] = (raw[e][c] << 1) | p;
        }
        ++colourBits;
        if (alphaBits)
            ++alphaBits;
    }
    else if (m.sharedPBits)
    {
        for (int s = 0; s < m.subsets; ++s)
        {
            const uint32_t p = reader.Read(1);
            for (int e = s * 2; e < s * 2 + 2; ++e)
                for (int c = 0; c < 4; ++c)
                    raw[e][c] = (raw[e][c] << 1) | p;
        }
        ++colourBits;
        if (alphaBits)
            ++alphaBits;
    }

    uint8_t endpoint[6][4];
    for (int e = 0; e < endpointCount; ++e)
    {
        endpoint[e][0] = Unquantise(raw[e][0], colourBits);
        endpoint[e][1] = Unquantise(raw[e][1], colourBits);
        endpoint[e][2] = Unquantise(raw[e][2], colourBits);
        endpoint[e][3] = m.alphaBits ? Unquantise(raw[e][3], alphaBits) : 255;
    }

    const uint8_t* partitionTable = nullptr;
    if (m.subsets == 2)
        partitionTable = kPartition2[partition];
    else if (m.subsets == 3)
        partitionTable = kPartition3[partition];

    // Anchor positions carry one bit fewer, because the index's high bit is
    // implied by the endpoint ordering. Every other texel reads its full width.
    int anchor[3] = {0, 0, 0};
    if (m.subsets == 2)
        anchor[1] = kAnchor2[partition];
    else if (m.subsets == 3)
    {
        anchor[1] = kAnchor3a[partition];
        anchor[2] = kAnchor3b[partition];
    }

    auto subsetOf = [&](int texel) -> int { return partitionTable ? partitionTable[texel] : 0; };
    // Each subset has exactly one anchor texel, and the partition tables
    // guarantee anchor[s] lies in subset s -- so this is the whole test.
    auto isAnchor = [&](int texel) -> bool { return texel == anchor[subsetOf(texel)]; };

    uint8_t index1[16];
    for (int t = 0; t < 16; ++t)
        index1[t] = static_cast<uint8_t>(reader.Read(isAnchor(t) ? m.indexBits - 1 : m.indexBits));

    uint8_t index2[16] = {};
    if (m.indexBits2)
    {
        // The second index set has exactly one anchor, texel 0, because modes
        // with two index sets are all single-subset.
        for (int t = 0; t < 16; ++t)
            index2[t] = static_cast<uint8_t>(reader.Read(t == 0 ? m.indexBits2 - 1 : m.indexBits2));
    }

    const uint8_t* weights1 = m.indexBits == 2 ? kWeight2 : (m.indexBits == 3 ? kWeight3 : kWeight4);
    const uint8_t* weights2 = m.indexBits2 == 2 ? kWeight2 : (m.indexBits2 == 3 ? kWeight3 : kWeight4);

    for (int t = 0; t < 16; ++t)
    {
        const int x = t & 3;
        const int y = t >> 2;
        if (x >= usableW || y >= usableH)
            continue;

        const int s = subsetOf(t);
        const uint8_t* e0 = endpoint[s * 2];
        const uint8_t* e1 = endpoint[s * 2 + 1];

        // With two index sets, one drives colour and the other alpha, and
        // `indexSelection` swaps which is which.
        const uint8_t cw = m.indexBits2 ? (indexSelection ? weights2[index2[t]] : weights1[index1[t]]) : weights1[index1[t]];
        const uint8_t aw = m.indexBits2 ? (indexSelection ? weights1[index1[t]] : weights2[index2[t]]) : weights1[index1[t]];

        uint8_t rgba[4];
        rgba[0] = Interpolate(e0[0], e1[0], cw);
        rgba[1] = Interpolate(e0[1], e1[1], cw);
        rgba[2] = Interpolate(e0[2], e1[2], cw);
        rgba[3] = Interpolate(e0[3], e1[3], aw);

        // Rotation moves alpha into one of the colour channels, so a mode-4/5
        // block can spend its better index precision on whichever channel
        // mattered. Undo it here.
        if (rotation == 1)
            std::swap(rgba[0], rgba[3]);
        else if (rotation == 2)
            std::swap(rgba[1], rgba[3]);
        else if (rotation == 3)
            std::swap(rgba[2], rgba[3]);

        std::memcpy(out + static_cast<size_t>(y) * rowPitch + static_cast<size_t>(x) * 4, rgba, 4);
    }
    return true;
}

void DecodeBc4Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    const size_t rowPitch = static_cast<size_t>(width) * 4;
    std::memset(dst, 0, rowPitch * static_cast<size_t>(height));
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            uint8_t* out = dst + static_cast<size_t>(by) * 4 * rowPitch + static_cast<size_t>(bx) * 16;
            DecodeBc4Block(src, out, rowPitch, 4, std::min(4, width - bx * 4), std::min(4, height - by * 4));
            src += 8;
        }
    }
    // BC4 is one channel. Publish it as R with an opaque alpha rather than
    // replicating to grey: a caller that wants luminance can do that, but a
    // caller reading a roughness or occlusion map wants the channel itself.
    for (size_t i = 0; i < rowPitch * static_cast<size_t>(height); i += 4)
        dst[i + 3] = 255;
}

void DecodeBc5Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    const size_t rowPitch = static_cast<size_t>(width) * 4;
    std::memset(dst, 0, rowPitch * static_cast<size_t>(height));
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            uint8_t* out = dst + static_cast<size_t>(by) * 4 * rowPitch + static_cast<size_t>(bx) * 16;
            DecodeBc5Block(src, out, rowPitch, 4, std::min(4, width - bx * 4), std::min(4, height - by * 4));
            src += 16;
        }
    }
    for (size_t i = 0; i < rowPitch * static_cast<size_t>(height); i += 4)
        dst[i + 3] = 255;
}

bool DecodeBc7ImageBatched(const uint8_t* src, uint8_t* dst, int width, int height, TaskPool& pool)
{
    constexpr int rowsPerBatch = 64; // Sixteen BC rows; no job per tiny 4x4 block.
    if (height <= rowsPerBatch || width <= 0 || pool.ThreadCount() <= 1)
        return DecodeBc7Image(src, dst, width, height);
    const uint32_t count = static_cast<uint32_t>((height + rowsPerBatch - 1) / rowsPerBatch);
    const size_t blockRowBytes = static_cast<size_t>((width + 3) / 4) * 16;
    std::vector<uint8_t> valid(count, 0);
    pool.ParallelFor(count, [&](uint32_t first, uint32_t last) {
        for (uint32_t batch = first; batch < last; ++batch)
        {
            const int row = static_cast<int>(batch) * rowsPerBatch;
            valid[batch] = DecodeBc7Image(src + (row / 4) * blockRowBytes,
                dst + static_cast<size_t>(row) * width * 4, width, std::min(rowsPerBatch, height - row));
        }
    });
    return std::all_of(valid.begin(), valid.end(), [](uint8_t value) { return value != 0; });
}

bool DecodeBc7Image(const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int bw = (width + 3) / 4;
    const int bh = (height + 3) / 4;
    const size_t rowPitch = static_cast<size_t>(width) * 4;
    bool ok = true;
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            uint8_t* out = dst + static_cast<size_t>(by) * 4 * rowPitch + static_cast<size_t>(bx) * 16;
            if (!DecodeBc7Block(src, out, rowPitch, std::min(4, width - bx * 4), std::min(4, height - by * 4)))
                ok = false;
            src += 16;
        }
    }
    return ok;
}

} // namespace Poseidon
