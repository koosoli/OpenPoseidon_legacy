#include <Poseidon/Graphics/Textures/EddsReader.hpp>

#include <Poseidon/Foundation/Algorithms/Lz4Block.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Poseidon
{

namespace
{

constexpr size_t kDdsHeaderSize = 128; // 4-byte magic + 124-byte DDS_HEADER
constexpr size_t kDxt10HeaderSize = 20;
constexpr size_t kSubBlockRaw = 65536; // .edds splits a mip into 64 KiB raw sub-blocks

// Little-endian FourCC, so the literal reads in file order at every use site.
constexpr uint32_t FourCC(const char (&s)[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

constexpr uint32_t kDdsMagic = FourCC("DDS ");
constexpr uint32_t kEnfMarker = FourCC("ENF1");
constexpr uint32_t kTagCopy = FourCC("COPY");
constexpr uint32_t kTagLz4 = FourCC("LZ4 ");

// Field offsets into the DDS header, from the magic. Named rather than derived from
// a packed struct because DDSConverter already owns that struct privately and this
// reader only needs seven of its fields.
constexpr size_t kOffFlags = 0x08;
constexpr size_t kOffHeight = 0x0C;
constexpr size_t kOffWidth = 0x10;
constexpr size_t kOffMipCount = 0x1C;
constexpr size_t kOffReserved1 = 0x24; // dwReserved1[1], where Enfusion writes 'ENF1'
constexpr size_t kOffPfFlags = 0x50;
constexpr size_t kOffPfFourCC = 0x54;
constexpr size_t kOffPfBitCount = 0x58;

constexpr uint32_t kDdpfAlphaPixels = 0x01;
constexpr uint32_t kDdpfAlpha = 0x02;
constexpr uint32_t kDdpfFourCC = 0x04;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfLuminance = 0x20000;

uint32_t ReadU32(const uint8_t* p, size_t offset)
{
    uint32_t v = 0;
    std::memcpy(&v, p + offset, sizeof(v));
    return v;
}

// Bytes a level of these dimensions occupies in the given format. This is the value
// every 'COPY' chunk size and every 'LZ4 ' decoded length is checked against, so a
// wrong field width anywhere earlier in the header shows up here rather than as a
// corrupt texture.
size_t NaturalSize(PixelFormat format, int w, int h)
{
    const size_t blocks = static_cast<size_t>(std::max(1, (w + 3) / 4)) * static_cast<size_t>(std::max(1, (h + 3) / 4));
    switch (format)
    {
        case PixelFormat::DXT1:
        case PixelFormat::BC4:
            return blocks * 8;
        case PixelFormat::DXT3:
        case PixelFormat::DXT5:
        case PixelFormat::BC5:
        case PixelFormat::BC7:
            return blocks * 16;
        case PixelFormat::RGBA8888:
        case PixelFormat::ARGB8888:
            return static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        case PixelFormat::R8:
            return static_cast<size_t>(w) * static_cast<size_t>(h);
        default:
            return 0;
    }
}

const char* DxgiName(uint32_t dxgi)
{
    switch (dxgi)
    {
        case 70:
        case 71:
        case 72:
            return "BC1";
        case 73:
        case 74:
        case 75:
            return "BC2";
        case 76:
        case 77:
        case 78:
            return "BC3";
        case 79:
        case 80:
        case 81:
            return "BC4";
        case 82:
        case 83:
        case 84:
            return "BC5";
        case 94:
        case 95:
        case 96:
            return "BC6H";
        case 97:
        case 98:
        case 99:
            return "BC7";
        default:
            return "unknown";
    }
}

EddsImage Unreadable(const char* fmt, ...)
{
    EddsImage image;
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    image.error = buffer;
    return image;
}

// Expands one 'LZ4 ' mip chunk. The sub-blocks are linked, so they all decode into
// one buffer and each is told how much output already exists.
bool ExpandLz4Mip(const uint8_t* chunk, size_t chunkSize, size_t expected, std::vector<uint8_t>& out,
                  std::string& error)
{
    if (chunkSize < 4)
    {
        error = "LZ4 mip chunk shorter than its length prefix";
        return false;
    }
    const uint32_t rawTotal = ReadU32(chunk, 0);
    if (rawTotal != expected)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "LZ4 mip declares %u raw bytes, dimensions require %zu", rawTotal,
                      expected);
        error = buffer;
        return false;
    }

    out.assign(rawTotal, 0);
    size_t p = 4;
    size_t produced = 0;
    bool sawFinal = false;
    while (produced < rawTotal)
    {
        if (chunkSize - p < 4)
        {
            error = "LZ4 mip ran out of sub-block headers";
            return false;
        }
        const uint32_t word = ReadU32(chunk, p);
        p += 4;
        const bool final = (word & 0x80000000u) != 0;
        const size_t compSize = word & 0x7FFFFFFFu;
        if (compSize > chunkSize - p)
        {
            error = "LZ4 sub-block overruns its mip chunk";
            return false;
        }
        const size_t want = std::min(kSubBlockRaw, static_cast<size_t>(rawTotal) - produced);
        const size_t got = Foundation::Lz4Block::Decompress(chunk + p, compSize, out.data(), out.size(), produced);
        if (got != want)
        {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "LZ4 sub-block produced %zu bytes, expected %zu", got, want);
            error = buffer;
            return false;
        }
        p += compSize;
        produced += got;
        if (final)
        {
            sawFinal = true;
            break;
        }
    }
    if (produced != rawTotal)
    {
        error = "LZ4 mip decoded short";
        return false;
    }
    if (!sawFinal)
    {
        error = "LZ4 mip had no final sub-block";
        return false;
    }
    if (p != chunkSize)
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "LZ4 mip consumed %zu of %zu chunk bytes", p, chunkSize);
        error = buffer;
        return false;
    }
    return true;
}

} // namespace

bool IsEddsBuffer(const void* data, size_t size)
{
    if (!data || size < kDdsHeaderSize)
        return false;
    const auto* p = static_cast<const uint8_t*>(data);
    return ReadU32(p, 0) == kDdsMagic && ReadU32(p, kOffReserved1) == kEnfMarker;
}

EddsImage ReadEddsBuffer(const void* data, size_t size)
{
    if (!data || size < kDdsHeaderSize)
        return Unreadable("buffer shorter than a DDS header (%zu bytes)", size);

    const auto* base = static_cast<const uint8_t*>(data);
    if (ReadU32(base, 0) != kDdsMagic)
        return Unreadable("not a DDS container");
    if (ReadU32(base, kOffReserved1) != kEnfMarker)
        return Unreadable("DDS carries no 'ENF1' marker -- read it as a plain DDS instead");

    const int height = static_cast<int>(ReadU32(base, kOffHeight));
    const int width = static_cast<int>(ReadU32(base, kOffWidth));
    const uint32_t flags = ReadU32(base, kOffFlags);
    constexpr uint32_t kDdsdMipMapCount = 0x00020000;
    int mipCount = (flags & kDdsdMipMapCount) ? static_cast<int>(ReadU32(base, kOffMipCount)) : 1;
    mipCount = std::max(1, mipCount);

    if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
        return Unreadable("implausible dimensions %dx%d", width, height);
    if (mipCount > 32)
        return Unreadable("implausible mip count %d", mipCount);

    const uint32_t pfFlags = ReadU32(base, kOffPfFlags);
    const uint32_t fourCC = ReadU32(base, kOffPfFourCC);
    const uint32_t bitCount = ReadU32(base, kOffPfBitCount);

    size_t cursor = kDdsHeaderSize;
    PixelFormat format = PixelFormat::Unknown;
    bool hasAlpha = false;

    if (pfFlags & kDdpfFourCC)
    {
        if (fourCC == FourCC("DX10"))
        {
            if (size < kDdsHeaderSize + kDxt10HeaderSize)
                return Unreadable("DX10 header truncated");
            const uint32_t dxgi = ReadU32(base, kDdsHeaderSize);
            cursor += kDxt10HeaderSize;
            // Reforger is a BC7-era corpus where DayZ was a DXT-era one: over a
            // stratified 1,692-file sample of its 29,026 `.edds`, BC7 was 87.2% of
            // everything this reader could not decode, then BC4 7.8%, BC1 2.2%,
            // BC5 1.9% (ARF-001).
            //
            // Note BC1/BC2/BC3 appear here at all. Those are the SAME block layouts
            // the FourCC branch below already decodes -- an encoder simply chose the
            // DX10 header to say so, usually to name the sRGB variant. Failing them
            // was never a missing codec, only a missing mapping, and it silently cost
            // 35 of 1,692 files in the sample.
            //
            // The sRGB enumerators map onto the same decoder as their UNORM twins:
            // sRGB is a transfer function on the sampler, not a different block
            // encoding, and this reader hands back raw 8-bit texels either way.
            switch (dxgi)
            {
                case 70: case 71: case 72: // BC1 typeless / UNORM / UNORM_SRGB
                    format = PixelFormat::DXT1;
                    hasAlpha = true;
                    break;
                case 73: case 74: case 75: // BC2
                    format = PixelFormat::DXT3;
                    hasAlpha = true;
                    break;
                case 76: case 77: case 78: // BC3
                    format = PixelFormat::DXT5;
                    hasAlpha = true;
                    break;
                case 79: case 80: case 81: // BC4 typeless / UNORM / SNORM
                    format = PixelFormat::BC4;
                    break;
                case 82: case 83: case 84: // BC5
                    format = PixelFormat::BC5;
                    break;
                case 97: case 98: case 99: // BC7 typeless / UNORM / UNORM_SRGB
                    format = PixelFormat::BC7;
                    hasAlpha = true;
                    break;
                case 87: case 88: case 90: case 91: case 92: case 93:
                    // B8G8R8A8 / B8G8R8X8 and their sRGB and typeless spellings.
                    // Stored B,G,R,A in memory order, which is what this engine
                    // calls ARGB8888 -- the same layout the non-FourCC branch
                    // below already handles, just declared the DX10 way.
                    format = PixelFormat::ARGB8888;
                    hasAlpha = (dxgi == 87 || dxgi == 90 || dxgi == 91);
                    break;
                default:
                    // BC6H (94-96) and the uncompressed DXGI formats are still out.
                    // BC6H is HDR and does not occur in the sampled corpus; a decoder
                    // with no input to check it against is a liability, not coverage.
                    return Unreadable("DX10 pixel format %s (DXGI %u) is not supported by this build", DxgiName(dxgi),
                                      dxgi);
            }
        }
        else if (fourCC == FourCC("DXT1"))
            format = PixelFormat::DXT1;
        else if (fourCC == FourCC("DXT3"))
            format = PixelFormat::DXT3;
        else if (fourCC == FourCC("DXT5"))
        {
            format = PixelFormat::DXT5;
            hasAlpha = true;
        }
        else
        {
            char cc[5] = {static_cast<char>(fourCC & 0xFF), static_cast<char>((fourCC >> 8) & 0xFF),
                          static_cast<char>((fourCC >> 16) & 0xFF), static_cast<char>((fourCC >> 24) & 0xFF), 0};
            return Unreadable("unsupported FourCC '%s'", cc);
        }
    }
    else if ((pfFlags & (kDdpfRgb | kDdpfAlpha)) && bitCount == 32)
    {
        // Stored B,G,R,A in memory order, which is what the engine calls ARGB8888.
        format = PixelFormat::ARGB8888;
        hasAlpha = (pfFlags & kDdpfAlphaPixels) != 0;
    }
    else if ((pfFlags & (kDdpfRgb | kDdpfLuminance | kDdpfAlpha)) && bitCount == 8)
    {
        // A single 8-bit channel. Reforger's procedural-clutter masks
        // (`c_GrassAtlas_*_MASK.edds`) are declared this way -- DDPF_RGB with a
        // red mask of 0xFF and every other mask zero, which is a mask, not a
        // palette index, so it must not be routed to P8.
        format = PixelFormat::R8;
    }
    else
    {
        return Unreadable("unsupported pixel layout (pfFlags 0x%02x, %u bits per pixel)", pfFlags, bitCount);
    }

    // Chunk table: mipCount records of {tag, size}, smallest level first.
    if (size - cursor < static_cast<size_t>(mipCount) * 8)
        return Unreadable("chunk table truncated (%d mips)", mipCount);

    struct Chunk
    {
        uint32_t tag;
        uint32_t size;
    };
    std::vector<Chunk> table(static_cast<size_t>(mipCount));
    for (int i = 0; i < mipCount; ++i)
    {
        table[static_cast<size_t>(i)].tag = ReadU32(base, cursor);
        table[static_cast<size_t>(i)].size = ReadU32(base, cursor + 4);
        cursor += 8;
    }

    // The file's own byte accounting. It closes exactly on all 454 .edds measured,
    // so a mismatch here means a field was read at the wrong width, not that this
    // particular texture is unusual.
    size_t declared = 0;
    for (const Chunk& c : table)
    {
        if (c.size > size)
            return Unreadable("chunk size %u exceeds the file", c.size);
        declared += c.size;
    }
    if (cursor + declared != size)
        return Unreadable("byte accounting does not close: header+table+chunks = %zu, file is %zu", cursor + declared,
                          size);

    EddsImage image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.hasAlpha = hasAlpha;
    image.mipmaps.resize(static_cast<size_t>(mipCount));

    std::vector<uint8_t> expanded;
    for (int i = 0; i < mipCount; ++i)
    {
        // Table index i is level (mipCount - 1 - i): the table runs smallest first.
        const int level = mipCount - 1 - i;
        const int lw = std::max(1, width >> level);
        const int lh = std::max(1, height >> level);
        const size_t expected = NaturalSize(format, lw, lh);
        if (expected == 0)
            return Unreadable("no size rule for the selected pixel format");

        const Chunk& chunk = table[static_cast<size_t>(i)];
        const uint8_t* payload = base + cursor;
        cursor += chunk.size;

        DDSMipLevel& out = image.mipmaps[static_cast<size_t>(level)];
        out.width = lw;
        out.height = lh;

        if (chunk.tag == kTagCopy)
        {
            if (chunk.size != expected)
                return Unreadable("level %d stored raw as %u bytes, %dx%d requires %zu", level, chunk.size, lw, lh,
                                  expected);
            out.data.assign(payload, payload + chunk.size);
        }
        else if (chunk.tag == kTagLz4)
        {
            std::string error;
            if (!ExpandLz4Mip(payload, chunk.size, expected, expanded, error))
                return Unreadable("level %d (%dx%d): %s", level, lw, lh, error.c_str());
            out.data = expanded;
        }
        else
        {
            char cc[5] = {static_cast<char>(chunk.tag & 0xFF), static_cast<char>((chunk.tag >> 8) & 0xFF),
                          static_cast<char>((chunk.tag >> 16) & 0xFF), static_cast<char>((chunk.tag >> 24) & 0xFF), 0};
            return Unreadable("level %d has unknown chunk tag '%s'", level, cc);
        }
    }

    return image;
}

} // namespace Poseidon
