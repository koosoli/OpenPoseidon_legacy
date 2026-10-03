
#include <Poseidon/Graphics/Textures/JpgImport.hpp>
#include <Poseidon/IO/FileServer.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{

namespace
{
inline uint16_t ConvertRGBA8To1555(int r, int g, int b, int a)
{
    return static_cast<uint16_t>(((r << (10 - 3)) & (0x1f << 10)) | ((g << (5 - 3)) & (0x1f << 5)) | ((b >> 3) & 0x1f) |
                                 (a >= 128 ? 0x8000 : 0));
}

inline uint16_t ConvertRGBA8To4444(int r, int g, int b, int a)
{
    return static_cast<uint16_t>(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

inline uint16_t ConvertRGB8To565(int r, int g, int b)
{
    return static_cast<uint16_t>(((r << (11 - 3)) & (0x1f << 11)) | ((g << (5 - 2)) & (0x3f << 5)) | ((b >> 3) & 0x1f));
}

// Pow2 + min/max. Matches the old IJL path's constraint.
bool IsPow2(int x)
{
    return x > 0 && (x & (x - 1)) == 0;
}

int Log2i(int x)
{
    int n = 0;
    while (x > 1)
    {
        x >>= 1;
        ++n;
    }
    return n;
}

// Read a whole file. Prefer the engine's FileServer when available (so PBO /
// search-path resolution works); fall back to std::ifstream in unit-test
// contexts where the server isn't initialised.
bool SlurpFile(const char* name, std::vector<uint8_t>& out)
{
    if (GFileServer)
    {
        QIFStream in;
        GFileServer->Open(in, name);
        if (!in.fail() && in.rest() > 0)
        {
            int rest = in.rest();
            const auto* bytes = reinterpret_cast<const uint8_t*>(in.act());
            out.assign(bytes, bytes + rest);
            return true;
        }
    }
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    auto size = file.tellg();
    if (size <= 0)
        return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good();
}

// Box-average color of an RGBA8 buffer. Used as ITextureSource::GetAverageColor().
PackedColor ComputeAverage(const uint8_t* rgba, int w, int h)
{
    uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i)
    {
        sumR += rgba[i * 4 + 0];
        sumG += rgba[i * 4 + 1];
        sumB += rgba[i * 4 + 2];
        sumA += rgba[i * 4 + 3];
    }
    if (n == 0)
        return PackedBlack;
    uint8_t r = static_cast<uint8_t>(sumR / n);
    uint8_t g = static_cast<uint8_t>(sumG / n);
    uint8_t b = static_cast<uint8_t>(sumB / n);
    uint8_t a = static_cast<uint8_t>(sumA / n);
    return PackedColor(r, g, b, a);
}

} // namespace

TextureSourceJPEG::TextureSourceJPEG() = default;
TextureSourceJPEG::~TextureSourceJPEG() = default;

bool TextureSourceJPEG::InitFromMemory(const uint8_t* data, size_t size, const char* nameForErrors)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4 /*force RGBA*/);
    if (!pixels)
    {
        LOG_DEBUG(Graphics, "stbi_load_from_memory failed for {}: {}", nameForErrors ? nameForErrors : "?",
                  stbi_failure_reason());
        return false;
    }

    if (!IsPow2(w) || !IsPow2(h))
    {
        Poseidon::Foundation::WarningMessage("Image %s: dimensions %dx%d not 2^n", nameForErrors ? nameForErrors : "?",
                                             w, h);
        stbi_image_free(pixels);
        return false;
    }

    _w = w;
    _h = h;
    _rgba0.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(pixels);

    int maxDim = std::max(_w, _h);
    _mipmaps = Log2i(maxDim) + 1;
    // Keep a small floor — mip dimensions below 4 pixels carry no detail and the
    // old IJL path also skipped them.
    while (_mipmaps > 1 && ((_w >> (_mipmaps - 1)) < 4 || (_h >> (_mipmaps - 1)) < 4))
        --_mipmaps;

    _avgColor = ComputeAverage(_rgba0.data(), _w, _h);

    // Classify the alpha that was decoded. stb forces four channels, so a JPEG (or an
    // RGB PNG) arrives with A=255 everywhere and stays opaque.
    bool partial = false, zero = false;
    const size_t n = static_cast<size_t>(_w) * static_cast<size_t>(_h);
    for (size_t i = 0; i < n && !partial; ++i)
    {
        const uint8_t a = _rgba0[i * 4 + 3];
        if (a == 0)
            zero = true;
        else if (a != 255)
            partial = true;
    }
    _isAlpha = partial;
    _isTransparent = !partial && zero;
    return true;
}

bool TextureSourceJPEG::Init(const char* name, PacLevelMem* mips, int maxMips)
{
    _name = name;

    std::vector<uint8_t> buf;
    if (!SlurpFile(name, buf) || buf.empty())
        return false;

    if (!InitFromMemory(buf.data(), buf.size(), name))
        return false;

    int count = std::min(_mipmaps, maxMips);
    for (int i = 0; i < count; ++i)
    {
        mips[i]._w = static_cast<short>(_w >> i);
        mips[i]._h = static_cast<short>(_h >> i);
        mips[i]._sFormat = PacARGB8888;
    }
    _mipmaps = count;
    return true;
}

bool TextureSourceJPEG::GetMipmapData(void* mem, const PacLevelMem& mip, int level) const
{
    if (level < 0 || level >= _mipmaps)
    {
        LOG_ERROR(Graphics, "Image mip level {} out of range ({} mipmaps) for {}", level, _mipmaps,
                  static_cast<const char*>(_name));
        return false;
    }

    int lvlW = _w >> level;
    int lvlH = _h >> level;
    if (lvlW != mip._w || lvlH != mip._h)
    {
        LOG_ERROR(Graphics, "Image mip {} dim mismatch: expected {}x{}, got {}x{} for {}", level, int(mip._w),
                  int(mip._h), lvlW, lvlH, static_cast<const char*>(_name));
        return false;
    }

    // For level 0 we already have the RGBA data; for deeper levels we downsample
    // on demand using stb_image_resize2 (matches the PAAEncoder path elsewhere
    // in the engine). Intermediate storage is stack-allocated for typical sizes.
    const uint8_t* src = nullptr;
    std::vector<uint8_t> resized;
    if (level == 0)
    {
        src = _rgba0.data();
    }
    else
    {
        resized.resize(static_cast<size_t>(lvlW) * static_cast<size_t>(lvlH) * 4);
        stbir_resize_uint8_linear(_rgba0.data(), _w, _h, _w * 4, resized.data(), lvlW, lvlH, lvlW * 4, STBIR_RGBA);
        src = resized.data();
    }

    const int pixels = lvlW * lvlH;

    PacFormat format = mip._dFormat;
    switch (format)
    {
        case PacARGB1555:
        {
            auto* dst = static_cast<uint16_t*>(mem);
            for (int i = 0; i < pixels; ++i)
            {
                int r = src[i * 4 + 0];
                int g = src[i * 4 + 1];
                int b = src[i * 4 + 2];
                dst[i] = ConvertRGBA8To1555(r, g, b, src[i * 4 + 3]);
            }
            return true;
        }
        case PacARGB4444:
        {
            auto* dst = static_cast<uint16_t*>(mem);
            for (int i = 0; i < pixels; ++i)
                dst[i] = ConvertRGBA8To4444(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]);
            return true;
        }
        case PacRGB565:
        {
            auto* dst = static_cast<uint16_t*>(mem);
            for (int i = 0; i < pixels; ++i)
            {
                int r = src[i * 4 + 0];
                int g = src[i * 4 + 1];
                int b = src[i * 4 + 2];
                dst[i] = ConvertRGB8To565(r, g, b);
            }
            return true;
        }
        case PacARGB8888:
        {
            auto* dst = static_cast<uint8_t*>(mem);
            // DX / engine expects BGRA layout in memory for ARGB8888.
            for (int i = 0; i < pixels; ++i)
            {
                dst[i * 4 + 0] = src[i * 4 + 2]; // B
                dst[i * 4 + 1] = src[i * 4 + 1]; // G
                dst[i * 4 + 2] = src[i * 4 + 0]; // R
                dst[i * 4 + 3] = src[i * 4 + 3]; // A (255 for JPEG and RGB images)
            }
            return true;
        }
        default:
            LOG_ERROR(Graphics, "Image unsupported destination format {} for {}", static_cast<int>(format),
                      static_cast<const char*>(_name));
            return false;
    }
}

bool TextureSourceJPEGFactory::Check(const char* name)
{
    return QIFStreamB::FileExist(name);
}

void TextureSourceJPEGFactory::PreInit(const char* name)
{
    if (GFileServer)
        GFileServer->Request(name, 2);
}

ITextureSource* TextureSourceJPEGFactory::Create(const char* name, PacLevelMem* mips, int maxMips)
{
    auto* source = new TextureSourceJPEG;
    if (!source->Init(name, mips, maxMips))
    {
        delete source;
        return nullptr;
    }
    return source;
}

static TextureSourceJPEGFactory STextureSourceJPEGFactory;
TextureSourceJPEGFactory* GTextureSourceJPEGFactory = &STextureSourceJPEGFactory;
} // namespace Poseidon
