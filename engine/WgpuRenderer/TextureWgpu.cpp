#include "TextureWgpu.hpp"
#include "TextureBankWgpu.hpp"

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <cstring>
#include <vector>

namespace Poseidon
{

namespace
{

bool IsPaaName(const char* name)
{
    const char* ext = name ? strrchr(name, '.') : nullptr;
    return ext && strcmpi(ext, ".paa") == 0;
}

PacFormat BasicFormat(const char* name)
{
    return IsPaaName(name) ? PacARGB4444 : PacARGB1555;
}

PacFormat DstFormat(PacFormat srcFormat)
{
    switch (srcFormat)
    {
        case PacP8:
            return PacARGB1555;
        default:
            return srcFormat;
    }
}

int BcFormatFor(PacFormat fmt)
{
    switch (fmt)
    {
        case PacDXT1:
            return WGR_TEXTURE_BC1;
        case PacDXT2:
        case PacDXT3:
            return WGR_TEXTURE_BC2;
        case PacDXT4:
        case PacDXT5:
            return WGR_TEXTURE_BC3;
        default:
            return -1;
    }
}

// Decode the top (full-resolution) mip once and classify its alpha channel — mirrors
// TextureGL33::ScanTopMipAlphaClass. Reads the texture bytes through the VFS (works for
// PBO-packed textures) and decodes via the shared DecodePAABuffer, which handles every
// PAA/PAC pixel format. Top mip on purpose: a smaller mip blurs a cutout's crisp 0/255 holes
// into false partial-alpha, mis-routing a pole/fence to the blend pass.
AlphaStats ScanTopMipAlphaStats(const char* name)
{
    if (!name)
    {
        return {};
    }
    QIFStream in;
    GFileServer->Open(in, name);
    const int size = in.fail() ? 0 : in.rest();
    if (size <= 0)
    {
        return {};
    }
    AUTO_STATIC_ARRAY(char, fileData, 256 * 1024);
    fileData.Realloc(size);
    fileData.Resize(size);
    in.read(fileData.Data(), size);

    const size_t len = strlen(name);
    const bool isPaa = len >= 4 && (name[len - 1] == 'a' || name[len - 1] == 'A'); // .paa vs .pac
    const DecodedImage img = DecodePAABuffer(fileData.Data(), static_cast<size_t>(size), isPaa);
    if (!img.valid())
    {
        return {};
    }
    return ClassifyAlpha(img.rgba.data(), static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
}

} // namespace

TextureWgpu::TextureWgpu(TextureBankWgpu* bank) : _bank(bank) {}

TextureWgpu::~TextureWgpu()
{
    if (_gpuHandle && _bank)
    {
        if (WgrRenderer* r = _bank->Renderer())
        {
            wgr_texture_destroy(r, _gpuHandle);
        }
    }
}

int TextureWgpu::Init()
{
    // A procedural texture has no file behind it: `#(argb,8,8,3)color(...)` is
    // generated from its own name. Nothing in the file-source chain can open one,
    // so without this every such reference fails to upload -- which is why Arma 3's
    // landing and warning lights, whose whole appearance is an authored flat
    // colour, had nothing to draw.
    if (const auto generated = render::procedural::Parse(Name()); generated.generated)
    {
        InitDynamic(generated.width, generated.height, generated.rgba.data(),
                    static_cast<uint32_t>(generated.rgba.size()));
        return _gpuHandle ? 0 : -1;
    }

    PacFormat format = BasicFormat(Name());

    ITextureSourceFactory* factory = SelectTextureSourceFactory(Name());
    if (!factory || !factory->Check(Name()))
    {
        _nMipmaps = 0;
        return -1;
    }
    _src = factory->Create(Name(), _mipmaps, MAX_MIPMAPS);
    if (!_src)
    {
        return -1;
    }

    format = _src->GetFormat();
    if (format == PacARGB4444 || format == PacAI88 || format == PacARGB8888)
    {
        _src->ForceAlpha();
    }

    const PacFormat dFormat = DstFormat(format);

    const int nMipmaps = _src->GetMipmapCount();
    int i = 0;
    for (; i < nMipmaps; i++)
    {
        PacLevelMem& mip = _mipmaps[i];
        mip.SetDestFormat(dFormat, 8);
        if (mip._w < 2 || mip._h < 2)
        {
            break;
        }
    }
    _nMipmaps = i;

    _w = _mipmaps[0]._w;
    _h = _mipmaps[0]._h;
    return 0;
}

// Three-way alpha classification for the section-sort renderer (opaque/cutout occlude;
// blend defers to the back-to-front pass). Mirrors TextureGL33::GetAlphaClass: cheap header
// flags decide it outright except for a multi-bit-alpha format, which needs the top-mip decode
// to tell cutout from blend. Cached in _alphaClass. Dynamic textures keep the base Opaque.
AlphaStats::Kind TextureWgpu::GetAlphaClass()
{
    if (_alphaClass >= 0)
    {
        return static_cast<AlphaStats::Kind>(_alphaClass);
    }
    AlphaStats::Kind kind = AlphaStats::Opaque;
    if (_src)
    {
        const bool hasAlpha = _src->IsAlpha();
        const bool chroma = _src->IsTransparent();
        const bool oneBit = _src->GetFormat() == PacDXT1; // 1-bit alpha: punch-through only
        // Only multi-bit-alpha formats need the (cached) decode to tell cutout from blend.
        AlphaStats decoded;
        const AlphaStats* decodedPtr = nullptr;
        if (hasAlpha && !oneBit)
        {
            if (!_alphaStatsScanned)
            {
                _alphaStats = ScanTopMipAlphaStats(Name());
                _alphaStatsScanned = true;
            }
            decoded = _alphaStats;
            decodedPtr = &decoded;
        }
        kind = ClassifyTextureAlpha(hasAlpha, chroma, oneBit, decodedPtr);
    }
    _alphaClass = static_cast<signed char>(kind);
    return kind;
}

void TextureWgpu::EvictGpu()
{
    if (_dynamic || _gpuHandle == 0 || !_bank)
    {
        return;
    }
    if (WgrRenderer* r = _bank->Renderer())
    {
        wgr_texture_destroy(r, _gpuHandle);
    }
    _gpuHandle = 0;
    _uploadTried = false;
}

bool TextureWgpu::HasAlphaHoles()
{
    // The cutout decision is reached only for a decoded multi-bit-alpha texture in the
    // TreeAdv path. A 2% floor matches ClassifyAlpha's clear-coverage threshold and keeps
    // harmless compression noise from turning a solid bark map into a masked draw.
    if (!_alphaStatsScanned)
    {
        (void)GetAlphaClass();
    }
    return _alphaStatsScanned && _alphaStats.pctClear >= 2.0;
}

void TextureWgpu::InitDynamic(int w, int h, const void* rgba, uint32_t size)
{
    _dynamic = true;
    _uploadTried = true;
    _w = w;
    _h = h;
    _nMipmaps = 1;
    if (WgrRenderer* r = _bank ? _bank->Renderer() : nullptr)
    {
        _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(w), static_cast<uint32_t>(h), WGR_TEXTURE_RGBA8, 1, 0,
                                        static_cast<const uint8_t*>(rgba), size);
    }
    if (!_gpuHandle)
    {
        LOG_WARN(Graphics, "Wgpu: failed to upload dynamic texture {}x{} size={}", w, h, size);
    }
}

void TextureWgpu::UpdateDynamic(const void* rgba, uint32_t size)
{
    if (!_gpuHandle)
    {
        return;
    }

    if (WgrRenderer* r = _bank ? _bank->Renderer() : nullptr)
    {
        wgr_texture_update(r, _gpuHandle, static_cast<const uint8_t*>(rgba), size);
    }
}

// CPU-side pixel read, mirroring TextureGL33::GetPixel (without interpolation).
// Used by Scene::SetSkyTexture to derive the fog / background colour from the sky
// texture — a stub here left the fog and horizon black. Decodes the requested mip
// to its dest format and samples it.
Color TextureWgpu::GetPixel(int level, float u, float v) const
{
    if (!_src || _nMipmaps <= 0)
    {
        return HWhite;
    }
    if (level < 0 || level >= _nMipmaps)
    {
        level = 0;
    }

    PacLevelMem mip = _mipmaps[level];
    std::vector<char> mem(static_cast<size_t>(mip._pitch) * mip._h);
    if (mem.empty() || !_src->GetMipmapData(mem.data(), mip, level))
    {
        return HWhite;
    }
    return mip.GetPixel(mem.data(), u, v);
}

uint64_t TextureWgpu::EnsureUploaded()
{
    if (_bank)
    {
        _bank->NoteTextureUse(this);
    }
    if (_gpuHandle || _uploadTried)
    {
        return _gpuHandle;
    }
    _uploadTried = true;

    WgrRenderer* r = _bank ? _bank->Renderer() : nullptr;
    if (!r || _w <= 0 || _h <= 0)
    {
        return 0;
    }

    const PacFormat dst = _nMipmaps > 0 ? _mipmaps[0].DstFormat() : PacFormatN;
    const int bcFormat = BcFormatFor(dst);
    if (bcFormat >= 0 && _src)
    {
        // Full mip chain, tightly packed as wgr_texture_create expects: PAA mips
        // halve exactly, so level i is (_w>>i, _h>>i).
        std::vector<uint8_t> blocks;
        bool ok = true;
        for (int i = 0; i < _nMipmaps; i++)
        {
            const auto layout = render::mipmap::ComputeLayout(dst, _mipmaps[i]._w, _mipmaps[i]._h);
            const size_t off = blocks.size();
            blocks.resize(off + layout.dataSize);
            if (!_src->GetMipmapData(blocks.data() + off, _mipmaps[i], i))
            {
                ok = false;
                break;
            }
        }
        if (ok && !blocks.empty())
        {
            _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), bcFormat,
                                            static_cast<uint32_t>(_nMipmaps), 0, blocks.data(),
                                            static_cast<uint32_t>(blocks.size()));
        }
    }

    // Fallback (non-DXT formats, or a failed block upload): decode the whole file
    // to RGBA8 via the shared PAA decoder
    if (!_gpuHandle)
    {
        QIFStreamB stream;
        stream.AutoOpen(Name());
        const IFileBuffer* fb = stream.GetBuffer();
        if (fb && !fb->GetError() && fb->GetSize() > 0)
        {
            DecodedImage img = DecodePAABuffer(fb->GetData(), static_cast<size_t>(fb->GetSize()), IsPaaName(Name()));
            if (img.valid())
            {
                _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height),
                                                WGR_TEXTURE_RGBA8, 1, WGR_TEXTURE_GEN_MIPS, img.rgba.data(),
                                                static_cast<uint32_t>(img.rgba.size()));
                _w = img.width;
                _h = img.height;
            }
        }
    }

    if (!_gpuHandle)
    {
        LOG_WARN(Graphics, "Wgpu: failed to upload texture {}", Name());
    }
    return _gpuHandle;
}

} // namespace Poseidon
