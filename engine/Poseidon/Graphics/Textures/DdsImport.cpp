#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace Poseidon
{

namespace
{

bool SlurpFile(const char* name, std::vector<uint8_t>& out)
{
    // RFG-014: a natively loaded Enfusion world names textures that live INSIDE a
    // `.pak`, which no part of the file layer can open -- `QFBank` is a concrete PBO
    // class, not an interface. The mount is asked FIRST because when it is open the
    // world being drawn is the one it came from, and asking the file layer for a
    // Reforger path first only costs a miss. It is closed for every other world, so
    // this is inert on Everon, Chernarus and the rest.
    {
        const auto& mount = Poseidon::Asset::Formats::Enfusion::EnfusionMount::Instance();
        if (mount.IsOpen() && mount.Read(name, out))
            return true;
    }
    if (GFileServer)
    {
        QIFStream in;
        GFileServer->Open(in, name);
        if (!in.fail() && in.rest() > 0)
        {
            const int rest = in.rest();
            const auto* bytes = reinterpret_cast<const uint8_t*>(in.act());
            out.assign(bytes, bytes + rest);
            return true;
        }
    }
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto size = file.tellg();
    if (size <= 0)
        return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), size);
    return file.good();
}

PacFormat ToPacFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::DXT1:
            return PacDXT1;
        case PixelFormat::DXT3:
            return PacDXT3;
        case PixelFormat::DXT5:
            return PacDXT5;
        default:
            return PacARGB8888;
    }
}

bool IsBlockFormat(PixelFormat format)
{
    return format == PixelFormat::DXT1 || format == PixelFormat::DXT3 || format == PixelFormat::DXT5;
}

inline uint16_t To1555(int r, int g, int b, int a)
{
    return static_cast<uint16_t>(((a >= 128) ? 0x8000 : 0) | ((r << (10 - 3)) & (0x1f << 10)) |
                                 ((g << (5 - 3)) & (0x1f << 5)) | ((b >> 3) & 0x1f));
}

inline uint16_t To565(int r, int g, int b)
{
    return static_cast<uint16_t>(((r << (11 - 3)) & (0x1f << 11)) | ((g << (5 - 2)) & (0x3f << 5)) | ((b >> 3) & 0x1f));
}

// Average of the smallest stored level. Deliberately not the largest: this is only
// used as a distant-LOD stand-in, and mip N-1 of a 4k texture is already the box
// average the caller wants without touching 16 MB to compute it.
PackedColor AverageOfSmallest(const std::vector<DDSMipLevel>& levels, PixelFormat format)
{
    if (levels.empty() || IsBlockFormat(format))
        return PackedBlack;
    const DDSMipLevel& level = levels.back();
    const size_t pixels = static_cast<size_t>(level.width) * static_cast<size_t>(level.height);
    if (pixels == 0 || level.data.size() < pixels * 4)
        return PackedBlack;
    uint64_t sumB = 0, sumG = 0, sumR = 0, sumA = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        sumB += level.data[i * 4 + 0];
        sumG += level.data[i * 4 + 1];
        sumR += level.data[i * 4 + 2];
        sumA += level.data[i * 4 + 3];
    }
    return PackedColor(static_cast<uint8_t>(sumR / pixels), static_cast<uint8_t>(sumG / pixels),
                       static_cast<uint8_t>(sumB / pixels), static_cast<uint8_t>(sumA / pixels));
}

} // namespace

TextureSourceDDS::TextureSourceDDS() = default;
TextureSourceDDS::~TextureSourceDDS() = default;

bool TextureSourceDDS::InitFromMemory(const void* data, size_t size, const char* nameForErrors)
{
    const char* who = nameForErrors ? nameForErrors : "?";

    if (IsEddsBuffer(data, size))
    {
        EddsImage image = ReadEddsBuffer(data, size);
        if (!image.valid())
        {
            // Say which constraint broke. A bare "failed to load" here is what made
            // the ODOL work a hex-editor exercise (DZ-001), and this is the entry
            // point for every DayZ texture.
            LOG_ERROR(Graphics, "EDDS {}: {}", who, image.error.empty() ? "unreadable" : image.error);
            return false;
        }
        _format = image.format;
        _w = image.width;
        _h = image.height;
        _hasAlpha = image.hasAlpha;
        _levels = std::move(image.mipmaps);
    }
    else
    {
        DDSFile dds = DDSConverter::ReadDDSBuffer(data, size);
        if (!dds.valid())
        {
            LOG_ERROR(Graphics, "DDS {}: header or mip chain unreadable", who);
            return false;
        }
        _format = dds.format;
        _w = dds.width;
        _h = dds.height;
        _hasAlpha = dds.format == PixelFormat::DXT3 || dds.format == PixelFormat::DXT5;
        _levels = std::move(dds.mipmaps);
    }

    if (_levels.empty())
        return false;

    // RFG-016: BC4/BC5/BC7 have no PacFormat and never will -- the Pac container is a
    // 2001 format and these are not in it. But `ConvertPixels` decodes all three, so
    // decode here and hand the rest of the pipeline plain RGBA rather than refusing
    // the file. Reforger's ground and object albedos are BC7 to a texture, so without
    // this every native Enfusion model draws white while the file itself loaded fine.
    //
    // The cost is real and bounded: a decoded mip is 4 bytes a texel instead of 1, so
    // this trades VRAM for the file being usable at all. Only formats the block path
    // cannot carry take it; DXT1/3/5 stay compressed end to end.
    if (_format == PixelFormat::BC4 || _format == PixelFormat::BC5 || _format == PixelFormat::BC7)
    {
        bool decoded = true;
        for (DDSMipLevel& level : _levels)
        {
            if (level.width <= 0 || level.height <= 0 || level.data.empty())
                continue;
            std::vector<uint8_t> rgba(static_cast<size_t>(level.width) * static_cast<size_t>(level.height) * 4);
            if (!ConvertPixels(level.data.data(), rgba.data(), level.width, level.height, _format,
                               PixelFormat::RGBA8888))
            {
                decoded = false;
                break;
            }
            // ARGB8888 is what ToPacFormat maps every non-block format to, and in
            // this engine that name means B,G,R,A in memory -- Image::FromFile does
            // the same swap at its own call site. Decoding to RGBA and labelling it
            // ARGB uploads a blue island, or nothing at all.
            for (size_t i = 0; i + 2 < rgba.size(); i += 4)
                std::swap(rgba[i], rgba[i + 2]);
            level.data = std::move(rgba);
        }
        if (!decoded)
        {
            LOG_ERROR(Graphics, "DDS {}: pixel format {} could not be decoded to RGBA", who,
                      static_cast<int>(_format));
            return false;
        }
        _format = PixelFormat::ARGB8888;
        // Do NOT claim alpha here. BC7 carries a fourth channel whether or not the
        // author used it, and forcing the flag put every leaf card on the blended
        // path -- the first run of this drew Everon's trees as flat white shards.
        // The existing format rule below decides, exactly as it does for a .paa.
    }

    _pacFormat = ToPacFormat(_format);
    if (!IsBlockFormat(_format) && _format != PixelFormat::ARGB8888 && _format != PixelFormat::RGBA8888)
    {
        LOG_ERROR(Graphics, "DDS {}: pixel format {} has no PacFormat mapping", who, static_cast<int>(_format));
        return false;
    }
    _mipmaps = static_cast<int>(_levels.size());
    _avgColor = AverageOfSmallest(_levels, _format);
    _isTransparent = false;
    return true;
}

bool TextureSourceDDS::Init(const char* name, PacLevelMem* mips, int maxMips)
{
    _name = name;

    std::vector<uint8_t> buffer;
    if (!SlurpFile(name, buffer) || buffer.empty())
        return false;

    if (!InitFromMemory(buffer.data(), buffer.size(), name))
        return false;

    const int count = std::min(_mipmaps, maxMips);
    for (int i = 0; i < count; ++i)
    {
        mips[i]._w = static_cast<short>(_levels[static_cast<size_t>(i)].width);
        mips[i]._h = static_cast<short>(_levels[static_cast<size_t>(i)].height);
        mips[i]._sFormat = _pacFormat;
    }
    // Drop the levels past the cap so GetMipmapData can never be asked for one the
    // caller was not told about.
    if (count < _mipmaps)
        _levels.resize(static_cast<size_t>(count));
    _mipmaps = count;
    return _mipmaps > 0;
}

bool TextureSourceDDS::GetMipmapData(void* mem, const PacLevelMem& mip, int level) const
{
    if (level < 0 || level >= _mipmaps)
    {
        LOG_ERROR(Graphics, "DDS mip level {} out of range ({} mipmaps) for {}", level, _mipmaps,
                  static_cast<const char*>(_name));
        return false;
    }

    const DDSMipLevel& src = _levels[static_cast<size_t>(level)];
    if (src.width != mip._w || src.height != mip._h)
    {
        LOG_ERROR(Graphics, "DDS mip {} dim mismatch: file has {}x{}, caller expects {}x{} for {}", level, src.width,
                  src.height, int(mip._w), int(mip._h), static_cast<const char*>(_name));
        return false;
    }

    const PacFormat dest = ENUM_CAST(PacFormat, mip._dFormat);

    if (IsBlockFormat(_format))
    {
        // Blocks transfer verbatim, and only to the same block format -- there is no
        // decompressor on this path by design (see the header).
        if (dest != _pacFormat)
        {
            LOG_ERROR(Graphics, "DDS {}: block format {} cannot be delivered as {}", static_cast<const char*>(_name),
                      static_cast<int>(_pacFormat), static_cast<int>(dest));
            return false;
        }
        std::memcpy(mem, src.data.data(), src.data.size());
        return true;
    }

    const int pixels = src.width * src.height;
    if (src.data.size() < static_cast<size_t>(pixels) * 4)
        return false;
    const uint8_t* bgra = src.data.data(); // B,G,R,A in memory order

    switch (dest)
    {
        case PacARGB8888:
        {
            // Destination rows are padded to _pitch, which for a non-power-of-two or
            // aligned target is wider than the source row.
            auto* dst = static_cast<uint8_t*>(mem);
            const int srcPitch = src.width * 4;
            const int dstPitch = mip._pitch > 0 ? mip._pitch : srcPitch;
            for (int y = 0; y < src.height; ++y)
                std::memcpy(dst + static_cast<size_t>(y) * dstPitch, bgra + static_cast<size_t>(y) * srcPitch,
                            static_cast<size_t>(srcPitch));
            return true;
        }
        case PacARGB1555:
        {
            auto* dst = static_cast<uint16_t*>(mem);
            for (int i = 0; i < pixels; ++i)
                dst[i] = To1555(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0], bgra[i * 4 + 3]);
            return true;
        }
        case PacRGB565:
        {
            auto* dst = static_cast<uint16_t*>(mem);
            for (int i = 0; i < pixels; ++i)
                dst[i] = To565(bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0]);
            return true;
        }
        default:
            LOG_ERROR(Graphics, "DDS unsupported destination format {} for {}", static_cast<int>(dest),
                      static_cast<const char*>(_name));
            return false;
    }
}

bool TextureSourceDDSFactory::Check(const char* name)
{
    const auto& mount = Poseidon::Asset::Formats::Enfusion::EnfusionMount::Instance();
    if (mount.IsOpen() && mount.Has(name))
        return true;
    return QIFStreamB::FileExist(name);
}

void TextureSourceDDSFactory::PreInit(const char* name)
{
    if (GFileServer)
        GFileServer->Request(name, 2);
}

ITextureSource* TextureSourceDDSFactory::Create(const char* name, PacLevelMem* mips, int maxMips)
{
    auto* source = new TextureSourceDDS;
    if (!source->Init(name, mips, maxMips))
    {
        delete source;
        return nullptr;
    }
    return source;
}

static TextureSourceDDSFactory STextureSourceDDSFactory;
TextureSourceDDSFactory* GTextureSourceDDSFactory = &STextureSourceDDSFactory;

} // namespace Poseidon
