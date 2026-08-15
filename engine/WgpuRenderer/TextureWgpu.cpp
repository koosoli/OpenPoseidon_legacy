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

#include <atomic>
#include <chrono>
#include <cstdlib>
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

// ---------------------------------------------------------------------------------------
// Streaming attribution.
//
// The Everon fill-in costs ~1.1 ms of CPU per object created and is unattributed WITHIN
// itself: texture upload, the synchronous ODOL parse and object construction are all inside
// the same number, so "textures are the problem" has been a hypothesis, not a measurement.
// This splits the texture side of it into the three things it actually does — read the mip
// chain, hand the blocks to wgpu, and (only for a multi-bit-alpha format) decode the top mip
// a second time to classify its alpha — and reports them as totals, not as a per-frame region.
//
// Cumulative on purpose. The transient is ~81 s long on Everon; what is wanted is "of the
// whole fill-in, how much was texture work", which a per-frame timer cannot answer once the
// frames stop being interesting.
//
// WGR_TEXTURE_STREAM_STATS=1 turns it on. Off, this costs one relaxed atomic load per upload
// and no clock reads at all: an unconditional steady_clock::now() pair per upload would be
// measuring the measurement on a path this hot.
struct TextureStreamStats
{
    std::atomic<uint64_t> uploads{0};       // EnsureUploaded calls that reached a real load
    std::atomic<uint64_t> blockBytes{0};    // mip-chain bytes read as compressed blocks
    std::atomic<uint64_t> readUs{0};        // ...and microseconds spent reading them
    std::atomic<uint64_t> createUs{0};      // microseconds inside wgr_texture_create
    std::atomic<uint64_t> fallbacks{0};     // whole-file RGBA8 decodes (block path unavailable)
    std::atomic<uint64_t> fallbackUs{0};
    std::atomic<uint64_t> alphaScans{0};    // top-mip decodes done only to classify alpha
    std::atomic<uint64_t> alphaUs{0};
};

TextureStreamStats GTextureStreamStats;

bool TextureStreamStatsEnabled()
{
    // getenv walks the process environment block and costs ~2.1 us per call — measured on
    // this machine, and the reason WGR_DRAW_SECTION_ENV_CACHE exists. Read once.
    static const bool enabled = std::getenv("WGR_TEXTURE_STREAM_STATS") != nullptr;
    return enabled;
}

uint64_t NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Emitted on a power-of-two upload count: dense while the world is filling in (which is the
// part being attributed) and near-silent once it has settled, without a frame hook or a
// timer of its own.
void ReportTextureStreamStats(uint64_t uploads)
{
    if ((uploads & (uploads - 1)) != 0)
    {
        return;
    }
    const uint64_t readUs = GTextureStreamStats.readUs.load(std::memory_order_relaxed);
    const uint64_t createUs = GTextureStreamStats.createUs.load(std::memory_order_relaxed);
    const uint64_t fallbackUs = GTextureStreamStats.fallbackUs.load(std::memory_order_relaxed);
    const uint64_t alphaUs = GTextureStreamStats.alphaUs.load(std::memory_order_relaxed);
    LOG_INFO(Graphics,
             "Wgpu texture stream stats: uploads={} mipOpens={} blockMB={:.1f} readMs={:.1f} "
             "createMs={:.1f} fallbacks={} fallbackMs={:.1f} alphaScans={} alphaMs={:.1f} totalMs={:.1f}",
             uploads, TextureMipOpenCount(),
             static_cast<double>(GTextureStreamStats.blockBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
             readUs / 1000.0, createUs / 1000.0, GTextureStreamStats.fallbacks.load(std::memory_order_relaxed),
             fallbackUs / 1000.0, GTextureStreamStats.alphaScans.load(std::memory_order_relaxed), alphaUs / 1000.0,
             (readUs + createUs + fallbackUs + alphaUs) / 1000.0);
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
                // A whole second pass over the file: read it, decode the FULL-resolution mip
                // to RGBA8, then walk every pixel's alpha. Counted separately from the upload
                // because it is the one part of the texture cost that buys no pixels.
                const bool timed = TextureStreamStatsEnabled();
                const uint64_t began = timed ? NowUs() : 0;
                _alphaStats = ScanTopMipAlphaStats(Name());
                _alphaStatsScanned = true;
                if (timed)
                {
                    GTextureStreamStats.alphaScans.fetch_add(1, std::memory_order_relaxed);
                    GTextureStreamStats.alphaUs.fetch_add(NowUs() - began, std::memory_order_relaxed);
                }
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

    const bool timed = TextureStreamStatsEnabled();
    const PacFormat dst = _nMipmaps > 0 ? _mipmaps[0].DstFormat() : PacFormatN;
    const int bcFormat = BcFormatFor(dst);
    if (bcFormat >= 0 && _src)
    {
        // Full mip chain, tightly packed as wgr_texture_create expects: PAA mips
        // halve exactly, so level i is (_w>>i, _h>>i).
        //
        // Sized in one pass, then read in ONE call. The old shape — resize the vector
        // inside the loop, ask the source for one level per iteration — paid twice over
        // during the streaming transient: the growing resize recopies the chain up to
        // _nMipmaps times, and TextureSourcePac answers each per-level request by
        // re-opening the texture through the VFS, so a texture cost _nMipmaps opens
        // (up to MAX_MIPMAPS = 7) instead of one. GetMipmapChain's default implementation
        // is the old loop, so nothing changes for a source that has no per-call cost.
        std::vector<size_t> offsets(static_cast<size_t>(_nMipmaps));
        size_t total = 0;
        for (int i = 0; i < _nMipmaps; i++)
        {
            offsets[i] = total;
            total += render::mipmap::ComputeLayout(dst, _mipmaps[i]._w, _mipmaps[i]._h).dataSize;
        }
        // Destinations are resolved only once the buffer is at its final size — a pointer
        // taken before the allocation would not survive it.
        std::vector<uint8_t> blocks(total);
        std::vector<MipmapRead> reads(static_cast<size_t>(_nMipmaps));
        for (int i = 0; i < _nMipmaps; i++)
        {
            reads[i] = MipmapRead{blocks.data() + offsets[i], &_mipmaps[i], i};
        }
        const uint64_t readBegan = timed ? NowUs() : 0;
        const bool ok = _src->GetMipmapChain(reads.data(), _nMipmaps);
        if (timed)
        {
            GTextureStreamStats.readUs.fetch_add(NowUs() - readBegan, std::memory_order_relaxed);
            GTextureStreamStats.blockBytes.fetch_add(total, std::memory_order_relaxed);
        }
        if (ok && !blocks.empty())
        {
            const uint64_t createBegan = timed ? NowUs() : 0;
            _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), bcFormat,
                                            static_cast<uint32_t>(_nMipmaps), 0, blocks.data(),
                                            static_cast<uint32_t>(blocks.size()));
            if (timed)
            {
                GTextureStreamStats.createUs.fetch_add(NowUs() - createBegan, std::memory_order_relaxed);
            }
        }
    }

    // Fallback (non-DXT formats, or a failed block upload): decode the whole file
    // to RGBA8 via the shared PAA decoder
    if (!_gpuHandle)
    {
        const uint64_t began = timed ? NowUs() : 0;
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
        if (timed)
        {
            GTextureStreamStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
            GTextureStreamStats.fallbackUs.fetch_add(NowUs() - began, std::memory_order_relaxed);
        }
    }

    if (!_gpuHandle)
    {
        LOG_WARN(Graphics, "Wgpu: failed to upload texture {}", Name());
    }
    if (timed)
    {
        ReportTextureStreamStats(GTextureStreamStats.uploads.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    return _gpuHandle;
}

} // namespace Poseidon
