#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/DXTCompressor.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/Graphics/Shared/BMPWriter.hpp>
#include <Poseidon/Graphics/Shared/TGAWriter.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <ctype.h>
#include <utility>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#include <stb_image.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace Poseidon
{

// --- Data size computation ---

size_t ComputeDataSize(int width, int height, PixelFormat fmt)
{
    const auto& info = PixelFormatRegistry::Get(fmt);
    if (info.isCompressed)
    {
        int bw = (width + 3) / 4;
        int bh = (height + 3) / 4;
        // BC1 and BC4 are the 8-byte block formats; BC2/BC3/BC5/BC7 are 16.
        int blockBytes = (fmt == PixelFormat::DXT1 || fmt == PixelFormat::BC4) ? 8 : 16;
        return static_cast<size_t>(bw) * bh * blockBytes;
    }
    return static_cast<size_t>(width) * height * info.bitsPerPixel / 8;
}

// --- RGBA8888 → format converters ---

static void rgbaToARGB4444(const uint8_t* src, uint8_t* dst, int w, int h)
{
    auto* out = reinterpret_cast<uint16_t*>(dst);
    for (int i = 0; i < w * h; ++i)
    {
        uint8_t r = src[i * 4 + 0] >> 4;
        uint8_t g = src[i * 4 + 1] >> 4;
        uint8_t b = src[i * 4 + 2] >> 4;
        uint8_t a = src[i * 4 + 3] >> 4;
        out[i] = (a << 12) | (r << 8) | (g << 4) | b;
    }
}

static void rgbaToARGB1555(const uint8_t* src, uint8_t* dst, int w, int h)
{
    auto* out = reinterpret_cast<uint16_t*>(dst);
    for (int i = 0; i < w * h; ++i)
    {
        uint8_t r = src[i * 4 + 0] >> 3;
        uint8_t g = src[i * 4 + 1] >> 3;
        uint8_t b = src[i * 4 + 2] >> 3;
        uint8_t a = src[i * 4 + 3] >= 128 ? 1 : 0;
        out[i] = (a << 15) | (r << 10) | (g << 5) | b;
    }
}

static void rgbaToRGB565(const uint8_t* src, uint8_t* dst, int w, int h)
{
    auto* out = reinterpret_cast<uint16_t*>(dst);
    for (int i = 0; i < w * h; ++i)
    {
        uint8_t r = src[i * 4 + 0] >> 3;
        uint8_t g = src[i * 4 + 1] >> 2;
        uint8_t b = src[i * 4 + 2] >> 3;
        out[i] = (r << 11) | (g << 5) | b;
    }
}

static void rgbaToAI88(const uint8_t* src, uint8_t* dst, int w, int h)
{
    auto* out = reinterpret_cast<uint16_t*>(dst);
    for (int i = 0; i < w * h; ++i)
    {
        // ITU-R BT.601 luma
        uint8_t intensity =
            static_cast<uint8_t>((src[i * 4 + 0] * 77 + src[i * 4 + 1] * 150 + src[i * 4 + 2] * 29) >> 8);
        uint8_t alpha = src[i * 4 + 3];
        out[i] = (alpha << 8) | intensity;
    }
}

static void rgbaToARGB8888(const uint8_t* src, uint8_t* dst, int w, int h)
{
    auto* out = reinterpret_cast<uint32_t*>(dst);
    for (int i = 0; i < w * h; ++i)
    {
        uint32_t r = src[i * 4 + 0];
        uint32_t g = src[i * 4 + 1];
        uint32_t b = src[i * 4 + 2];
        uint32_t a = src[i * 4 + 3];
        out[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

// --- Format → RGBA8888 converters ---

static void argb4444ToRGBA(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const auto* in = reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < w * h; ++i)
    {
        uint16_t p = in[i];
        uint8_t a = (p >> 12) & 0xF;
        uint8_t r = (p >> 8) & 0xF;
        uint8_t g = (p >> 4) & 0xF;
        uint8_t b = p & 0xF;
        dst[i * 4 + 0] = (r << 4) | r;
        dst[i * 4 + 1] = (g << 4) | g;
        dst[i * 4 + 2] = (b << 4) | b;
        dst[i * 4 + 3] = (a << 4) | a;
    }
}

static void argb1555ToRGBA(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const auto* in = reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < w * h; ++i)
    {
        uint16_t p = in[i];
        uint8_t r = (p >> 10) & 0x1F;
        uint8_t g = (p >> 5) & 0x1F;
        uint8_t b = p & 0x1F;
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 3) | (g >> 2);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = (p & 0x8000) ? 255 : 0;
    }
}

static void rgb565ToRGBA(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const auto* in = reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < w * h; ++i)
    {
        uint16_t p = in[i];
        uint8_t r = (p >> 11) & 0x1F;
        uint8_t g = (p >> 5) & 0x3F;
        uint8_t b = p & 0x1F;
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 2) | (g >> 4);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = 255;
    }
}

static void ai88ToRGBA(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const auto* in = reinterpret_cast<const uint16_t*>(src);
    for (int i = 0; i < w * h; ++i)
    {
        uint8_t intensity = in[i] & 0xFF;
        uint8_t alpha = (in[i] >> 8) & 0xFF;
        dst[i * 4 + 0] = intensity;
        dst[i * 4 + 1] = intensity;
        dst[i * 4 + 2] = intensity;
        dst[i * 4 + 3] = alpha;
    }
}

static void argb8888ToRGBA(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const auto* in = reinterpret_cast<const uint32_t*>(src);
    for (int i = 0; i < w * h; ++i)
    {
        uint32_t p = in[i];
        dst[i * 4 + 0] = (p >> 16) & 0xFF; // R
        dst[i * 4 + 1] = (p >> 8) & 0xFF;  // G
        dst[i * 4 + 2] = p & 0xFF;         // B
        dst[i * 4 + 3] = (p >> 24) & 0xFF; // A
    }
}

// --- DXT decompression (standalone, for ConvertPixels) ---

static inline void expand565(uint16_t c, uint8_t out[4])
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
    out[3] = 255;
}

static void decodeDXT1Block(const uint8_t* block, uint8_t pixels[4][4][4], bool punchThrough)
{
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    uint8_t colors[4][4];
    expand565(c0, colors[0]);
    expand565(c1, colors[1]);
    if (c0 > c1 || !punchThrough)
    {
        for (int i = 0; i < 3; i++)
        {
            colors[2][i] = (2 * colors[0][i] + colors[1][i] + 1) / 3;
            colors[3][i] = (colors[0][i] + 2 * colors[1][i] + 1) / 3;
        }
        colors[2][3] = colors[3][3] = 255;
    }
    else
    {
        for (int i = 0; i < 3; i++)
            colors[2][i] = (colors[0][i] + colors[1][i]) / 2;
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = 0;
        colors[3][3] = 0;
    }
    uint32_t idx = block[4] | (block[5] << 8) | (block[6] << 16) | ((uint32_t)block[7] << 24);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            std::memcpy(pixels[y][x], colors[(idx >> ((y * 4 + x) * 2)) & 3], 4);
}

static void writeDXTBlock(uint8_t* rgba, int imgW, int imgH, int bx, int by, const uint8_t pixels[4][4][4])
{
    for (int py = 0; py < 4 && by * 4 + py < imgH; py++)
        for (int px = 0; px < 4 && bx * 4 + px < imgW; px++)
            std::memcpy(&rgba[((by * 4 + py) * imgW + bx * 4 + px) * 4], pixels[py][px], 4);
}

static void decompressDXT1(const uint8_t* data, uint8_t* rgba, int w, int h)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            uint8_t pixels[4][4][4];
            decodeDXT1Block(data, pixels, true);
            data += 8;
            writeDXTBlock(rgba, w, h, bx, by, pixels);
        }
}

static void decompressDXT3(const uint8_t* data, uint8_t* rgba, int w, int h)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            uint8_t pixels[4][4][4];
            decodeDXT1Block(data + 8, pixels, false);
            for (int i = 0; i < 16; i++)
            {
                int nibble = (i & 1) ? (data[i / 2] >> 4) : (data[i / 2] & 0xF);
                pixels[i / 4][i % 4][3] = (nibble << 4) | nibble;
            }
            data += 16;
            writeDXTBlock(rgba, w, h, bx, by, pixels);
        }
}

static void decompressDXT5(const uint8_t* data, uint8_t* rgba, int w, int h)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            uint8_t a0 = data[0], a1 = data[1];
            uint8_t alphas[8] = {a0, a1};
            if (a0 > a1)
                for (int i = 1; i <= 6; i++)
                    alphas[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
            else
            {
                for (int i = 1; i <= 4; i++)
                    alphas[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
                alphas[6] = 0;
                alphas[7] = 255;
            }
            uint64_t aBits = 0;
            for (int i = 0; i < 6; i++)
                aBits |= (uint64_t)data[2 + i] << (i * 8);

            uint8_t pixels[4][4][4];
            decodeDXT1Block(data + 8, pixels, false);
            for (int i = 0; i < 16; i++)
                pixels[i / 4][i % 4][3] = alphas[(aBits >> (i * 3)) & 7];

            data += 16;
            writeDXTBlock(rgba, w, h, bx, by, pixels);
        }
}

// --- ConvertPixels ---

bool ConvertPixels(const void* src, void* dst, int width, int height, PixelFormat srcFmt, PixelFormat dstFmt)
{
    if (srcFmt == dstFmt)
    {
        std::memcpy(dst, src, ComputeDataSize(width, height, srcFmt));
        return true;
    }

    const auto* srcBytes = static_cast<const uint8_t*>(src);
    auto* dstBytes = static_cast<uint8_t*>(dst);

    // If source is RGBA8888, encode directly
    if (srcFmt == PixelFormat::RGBA8888)
    {
        switch (dstFmt)
        {
            case PixelFormat::ARGB4444:
                rgbaToARGB4444(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::ARGB1555:
                rgbaToARGB1555(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::RGB565:
                rgbaToRGB565(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::AI88:
                rgbaToAI88(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::ARGB8888:
                rgbaToARGB8888(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::DXT1:
            {
                auto compressed = DXTCompressor::CompressDXT1(srcBytes, width, height);
                std::memcpy(dstBytes, compressed.data(), compressed.size());
                return true;
            }
            case PixelFormat::DXT5:
            {
                auto compressed = DXTCompressor::CompressDXT5(srcBytes, width, height);
                std::memcpy(dstBytes, compressed.data(), compressed.size());
                return true;
            }
            default:
                return false;
        }
    }

    // If destination is RGBA8888, decode directly
    if (dstFmt == PixelFormat::RGBA8888)
    {
        switch (srcFmt)
        {
            case PixelFormat::ARGB4444:
                argb4444ToRGBA(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::ARGB1555:
                argb1555ToRGBA(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::RGB565:
                rgb565ToRGBA(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::AI88:
                ai88ToRGBA(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::ARGB8888:
                argb8888ToRGBA(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::DXT1:
                decompressDXT1(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::DXT3:
                decompressDXT3(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::DXT5:
                decompressDXT5(srcBytes, dstBytes, width, height);
                return true;
            // The DXGI-era formats, reachable only through a DDS/EDDS "DX10"
            // header. BC4 lands in R and BC5 in R,G -- neither invents the
            // channels it does not carry (see BlockCompression.hpp).
            case PixelFormat::BC4:
                DecodeBc4Image(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::BC5:
                DecodeBc5Image(srcBytes, dstBytes, width, height);
                return true;
            case PixelFormat::BC7:
                // A false return is the reserved mode-8 encoding, which no
                // encoder emits; the rest of the image is still decoded.
                return DecodeBc7Image(srcBytes, dstBytes, width, height);
            case PixelFormat::R8:
                // Published as R with an opaque alpha, matching BC4: a mask's
                // one channel is the information, and replicating it to grey
                // would make a caller unable to tell mask from luminance.
                for (int i = 0; i < width * height; ++i)
                {
                    dstBytes[i * 4 + 0] = srcBytes[i];
                    dstBytes[i * 4 + 1] = 0;
                    dstBytes[i * 4 + 2] = 0;
                    dstBytes[i * 4 + 3] = 255;
                }
                return true;
            default:
                return false;
        }
    }

    // General case: src → RGBA8888 → dst (two-step via intermediate)
    size_t rgbaSize = ComputeDataSize(width, height, PixelFormat::RGBA8888);
    std::vector<uint8_t> intermediate(rgbaSize);
    if (!ConvertPixels(src, intermediate.data(), width, height, srcFmt, PixelFormat::RGBA8888))
        return false;
    return ConvertPixels(intermediate.data(), dst, width, height, PixelFormat::RGBA8888, dstFmt);
}

// --- Image class ---

Image::Image(int width, int height, PixelFormat format, std::vector<uint8_t> data)
    : _data(std::move(data)), _width(width), _height(height), _format(format)
{
}

Image Image::FromRGBA(int width, int height, std::vector<uint8_t> rgba)
{
    if (rgba.size() != static_cast<size_t>(width) * height * 4)
        return {};
    return {width, height, PixelFormat::RGBA8888, std::move(rgba)};
}

Image Image::FromFile(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return {};

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // PAA/PAC: use existing decoder → always RGBA8888
    if (ext == ".paa" || ext == ".pac")
    {
        auto decoded = DecodePAAFile(path);
        if (!decoded.valid())
            return {};
        return {decoded.width, decoded.height, PixelFormat::RGBA8888, std::move(decoded.rgba)};
    }

    // PNG/BMP/TGA/JPG: use stb_image → always RGBA8888
    if (ext == ".png" || ext == ".bmp" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg")
    {
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4); // force RGBA
        if (!pixels)
            return {};
        std::vector<uint8_t> data(pixels, pixels + w * h * 4);
        stbi_image_free(pixels);
        return {w, h, PixelFormat::RGBA8888, std::move(data)};
    }

    // DDS / Enfusion .edds: read the largest mipmap. Which reader applies is decided by
    // the file's own 'ENF1' marker rather than by the extension, because DayZ names the
    // two interchangeably and an .edds handed to the plain DDS reader does not fail --
    // it takes the first bytes of the chunk table as pixels and returns garbage (DZ-002).
    if (ext == ".dds" || ext == ".edds")
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file)
        {
            const auto size = file.tellg();
            if (size > 0)
            {
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                file.seekg(0);
                file.read(reinterpret_cast<char*>(bytes.data()), size);
                if (IsEddsBuffer(bytes.data(), bytes.size()))
                {
                    EddsImage edds = ReadEddsBuffer(bytes.data(), bytes.size());
                    if (!edds.valid())
                        return {};
                    auto pixels = std::move(edds.mipmaps[0].data);
                    if (edds.format == PixelFormat::ARGB8888)
                    {
                        // Stored B,G,R,A in memory; Image's RGBA8888 means what it says,
                        // and everything downstream (PNG, BMP, TGA) writes it verbatim --
                        // so the swap happens here or the image comes out with red and
                        // blue exchanged.
                        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                            std::swap(pixels[i], pixels[i + 2]);
                        return {edds.width, edds.height, PixelFormat::RGBA8888, std::move(pixels)};
                    }
                    // Block formats are held compressed; ToRGBA()/ConvertTo() decode them.
                    return {edds.width, edds.height, edds.format, std::move(pixels)};
                }
            }
        }

        auto dds = DDSConverter::ReadDDS(path);
        if (!dds.valid())
            return {};
        if (dds.format == PixelFormat::RGBA8888)
            return {dds.width, dds.height, PixelFormat::RGBA8888, std::move(dds.mipmaps[0].data)};
        // DXT: store as-is (Image can hold DXT data)
        return {dds.width, dds.height, dds.format, std::move(dds.mipmaps[0].data)};
    }

    return {};
}

Image Image::ToRGBA() const
{
    if (!valid())
        return {};
    if (_format == PixelFormat::RGBA8888)
        return *this;

    size_t rgbaSize = ComputeDataSize(_width, _height, PixelFormat::RGBA8888);
    std::vector<uint8_t> rgba(rgbaSize);
    if (!ConvertPixels(_data.data(), rgba.data(), _width, _height, _format, PixelFormat::RGBA8888))
        return {};
    return {_width, _height, PixelFormat::RGBA8888, std::move(rgba)};
}

Image Image::ConvertTo(PixelFormat target) const
{
    if (!valid())
        return {};
    if (_format == target)
        return *this;

    size_t dstSize = ComputeDataSize(_width, _height, target);
    std::vector<uint8_t> dstData(dstSize);
    if (!ConvertPixels(_data.data(), dstData.data(), _width, _height, _format, target))
        return {};
    return {_width, _height, target, std::move(dstData)};
}

// --- Container I/O ---

static ImageContainer detectContainer(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return ImageContainer::Unknown;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    const auto* ci = ImageContainerRegistry::FindByExtension(ext.c_str());
    return ci ? ci->container : ImageContainer::Unknown;
}

bool Image::Save(const std::string& path) const
{
    return Save(path, detectContainer(path));
}

bool Image::Save(const std::string& path, ImageContainer ct) const
{
    if (!valid())
        return false;

    // All standard outputs need RGBA
    Image rgba = ToRGBA();
    if (!rgba.valid())
        return false;

    switch (ct)
    {
        case ImageContainer::PNG:
            return PNGWriter::WriteRGBA(path.c_str(), rgba._width, rgba._height, rgba._data.data());

        case ImageContainer::TGA:
            return TGAWriter::WriteRGBA(path.c_str(), rgba._width, rgba._height, rgba._data.data());

        case ImageContainer::BMP:
        {
            // BMP needs BGR24
            std::vector<uint8_t> rgb(rgba._width * rgba._height * 3);
            for (int i = 0; i < rgba._width * rgba._height; ++i)
            {
                rgb[i * 3 + 0] = rgba._data[i * 4 + 0];
                rgb[i * 3 + 1] = rgba._data[i * 4 + 1];
                rgb[i * 3 + 2] = rgba._data[i * 4 + 2];
            }
            return BMPWriter::WriteBMP(path.c_str(), rgba._width, rgba._height, rgb.data());
        }

        case ImageContainer::PAA:
        case ImageContainer::PAC:
            return PAAEncoder::WritePAA(path, *this);

        case ImageContainer::DDS:
        {
            DDSFile dds;
            dds.format = PixelFormat::RGBA8888;
            dds.width = rgba._width;
            dds.height = rgba._height;
            DDSMipLevel level;
            level.width = rgba._width;
            level.height = rgba._height;
            level.data = rgba._data;
            dds.mipmaps.push_back(std::move(level));
            return DDSConverter::WriteDDS(path, dds);
        }

        default:
            return false;
    }
}

} // namespace Poseidon
