#include "TextureWgpu.hpp"
#include "TextureBankWgpu.hpp"

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/ProceduralTexture.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
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

// The block formats that carry a multi-bit alpha channel, i.e. exactly the ones that can
// reach the scan above. DXT1 is excluded here for the same reason GetAlphaClass excludes
// it: one bit of alpha is a punch-through, never a blend, so it is decided from the format
// alone and never decoded.
bool IsBlockAlphaFormat(PacFormat fmt)
{
    switch (fmt)
    {
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
            return true;
        default:
            return false;
    }
}

// Alpha stats for a BC2/BC3 top mip, taken straight off the compressed blocks.
//
// The classification only ever looks at byte 3 of a texel, but the path that produced it
// decoded all four: DecodePAABuffer runs decompressDXT3/decompressDXT5, which expand both
// colour endpoints, interpolate the 2- or 4-entry colour palette and memcpy 16 RGBA texels
// per block into a full-resolution image -- and then ClassifyAlpha reads one byte in four
// and throws the rest away. Three quarters of that work exists to be discarded.
//
// Here only the alpha half of each block is decoded, into a buffer whose colour bytes are
// never written or read. That is not an approximation of the old answer, it IS the old
// answer: BC3's alpha half is a BC4 block, and DecodeBc4Block builds the same 8-entry
// palette by the same rounding (BlockCompression.cpp) that decompressDXT5 builds inline;
// BC2's is the same low-nibble-first `(n<<4)|n` expansion decompressDXT3 does. The same
// bytes then go through the same unmodified ClassifyAlpha, so the verdict cannot move.
//
// Still the TOP mip -- the buffer is full resolution and every texel is classified. Nothing
// here samples a smaller level; the crisp 0/255 holes a cutout needs are all present.
bool ClassifyAlphaFromBlocks(const uint8_t* blocks, size_t size, int w, int h, PacFormat fmt, AlphaStats& out)
{
    if (!blocks || !IsBlockAlphaFormat(fmt) || w <= 0 || h <= 0)
    {
        return false;
    }
    const int blockCols = (w + 3) / 4;
    const int blockRows = (h + 3) / 4;
    const size_t need = static_cast<size_t>(blockCols) * static_cast<size_t>(blockRows) * 16u;
    if (size < need)
    {
        return false;
    }

    // RGBA-shaped so ClassifyAlpha can be reused verbatim; only the alpha lane is filled.
    // The colour lanes stay zero and are read by nobody -- ClassifyAlpha indexes `i*4+3`.
    std::vector<uint8_t> alphaOnly(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    const size_t rowPitch = static_cast<size_t>(w) * 4u;
    const bool bc3 = fmt == PacDXT4 || fmt == PacDXT5;

    for (int by = 0; by < blockRows; by++)
    {
        for (int bx = 0; bx < blockCols; bx++)
        {
            const uint8_t* block = blocks + (static_cast<size_t>(by) * blockCols + bx) * 16u;
            uint8_t* dst =
                alphaOnly.data() + static_cast<size_t>(by) * 4u * rowPitch + static_cast<size_t>(bx) * 16u + 3u;
            // A partial block at the right/bottom edge writes only the texels that exist,
            // matching writeDXTBlock's clamp. PAA levels are powers of two so this is the
            // dormant case, but a clamp that only holds for the common case is not a clamp.
            const int usableW = std::min(4, w - bx * 4);
            const int usableH = std::min(4, h - by * 4);
            if (bc3)
            {
                // BC3 = BC4 alpha block, then the colour block we deliberately never touch.
                DecodeBc4Block(block, dst, rowPitch, 4, usableW, usableH);
            }
            else
            {
                // BC2: 16 explicit 4-bit alphas in the first 8 bytes, low nibble first.
                for (int i = 0; i < 16; i++)
                {
                    const int py = i / 4;
                    const int px = i % 4;
                    if (py >= usableH || px >= usableW)
                    {
                        continue;
                    }
                    const int nibble = (i & 1) ? (block[i / 2] >> 4) : (block[i / 2] & 0xF);
                    dst[static_cast<size_t>(py) * rowPitch + static_cast<size_t>(px) * 4u] =
                        static_cast<uint8_t>((nibble << 4) | nibble);
                }
            }
        }
    }

    out = ClassifyAlpha(alphaOnly.data(), static_cast<size_t>(w) * static_cast<size_t>(h));
    return true;
}

// The top mip's blocks, handed from the upload that just inflated them to the
// classification that would otherwise read them again.
//
// Both wgpu call sites (EngineWgpu's immediate path and ClassifyGpuSection) do
// `EnsureUploaded()` and then `GetAlphaClass()` on the very next line, so the blocks the
// upload has in hand are still the right ones one call later. One slot, thread-local:
// this is a handoff, not a cache. A miss -- classified on a different thread, classified
// long after some other texture uploaded, or never uploaded at all -- costs nothing and
// falls back to reading the one level from the file, which is correct, only slower.
//
// Storing it is a vector move, so a texture that is uploaded and never classified pays
// nothing. That matters: computing the stats eagerly at upload time would instead have
// added a scan for every alpha-format texture nobody ever asks about.
//
// Keyed by texture NAME, not by the TextureWgpu address. An address is not an identity
// here -- a texture released between the park and the claim frees it, and the next
// allocation can land on it -- and a stale hit would hand one file's holes to another. The
// name plus the level's dimensions and format names the bytes themselves, so a hit is a
// hit on the same file or it is not a hit.
struct TopMipHandoff
{
    std::string name;
    std::vector<uint8_t> blocks;
    size_t topSize = 0;
    int w = 0;
    int h = 0;
    PacFormat format = PacFormatN;

    bool Matches(const char* forName, int forW, int forH, PacFormat forFormat) const
    {
        return topSize > 0 && forName && !name.empty() && format == forFormat && w == forW && h == forH &&
               name == forName;
    }

    void Release()
    {
        name.clear();
        topSize = 0;
        std::vector<uint8_t>().swap(blocks);
    }
};

thread_local TopMipHandoff GTopMipHandoff;

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
    // Of those scans: how many read alpha off the compressed blocks instead of decoding a
    // full RGBA8 image, and how many of THOSE needed no file access at all because the
    // upload had just inflated the level. The two together say whether the cost that is
    // left is decode or I/O, which a single total cannot.
    std::atomic<uint64_t> alphaBlockScans{0};
    std::atomic<uint64_t> alphaHandoffs{0};
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
             "createMs={:.1f} fallbacks={} fallbackMs={:.1f} alphaScans={} alphaBlockScans={} "
             "alphaHandoffs={} alphaMs={:.1f} totalMs={:.1f}",
             uploads, TextureMipOpenCount(),
             static_cast<double>(GTextureStreamStats.blockBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
             readUs / 1000.0, createUs / 1000.0, GTextureStreamStats.fallbacks.load(std::memory_order_relaxed),
             fallbackUs / 1000.0, GTextureStreamStats.alphaScans.load(std::memory_order_relaxed),
             GTextureStreamStats.alphaBlockScans.load(std::memory_order_relaxed),
             GTextureStreamStats.alphaHandoffs.load(std::memory_order_relaxed), alphaUs / 1000.0,
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

bool TextureWgpu::ScanTopMipAlphaBlocks(AlphaScanPath& path)
{
    if (!_src || _nMipmaps <= 0)
    {
        return false;
    }
    const PacFormat fmt = _src->GetFormat();
    // The chain is read in the source's own format only when the destination matches it,
    // which is what DstFormat() does for every DXT format. If that ever stops being true
    // the bytes would not be blocks, so check rather than assume.
    if (!IsBlockAlphaFormat(fmt) || _mipmaps[0].DstFormat() != fmt)
    {
        return false;
    }
    const int w = _mipmaps[0]._w;
    const int h = _mipmaps[0]._h;
    if (w <= 0 || h <= 0)
    {
        return false;
    }

    TopMipHandoff& handoff = GTopMipHandoff;
    if (handoff.Matches(Name(), w, h, fmt) &&
        ClassifyAlphaFromBlocks(handoff.blocks.data(), handoff.topSize, w, h, fmt, _alphaStats))
    {
        handoff.Release();
        path = AlphaScanPath::Handoff;
        return true;
    }

    // No handoff: read the ONE level being classified. The whole-file scan this replaces
    // read every level and then decoded only this one.
    const int need = render::mipmap::ComputeLayout(fmt, w, h).dataSize;
    if (need <= 0)
    {
        return false;
    }
    std::vector<uint8_t> blocks(static_cast<size_t>(need));
    const MipmapRead read{blocks.data(), &_mipmaps[0], 0};
    if (!_src->GetMipmapChain(&read, 1))
    {
        return false;
    }
    if (!ClassifyAlphaFromBlocks(blocks.data(), blocks.size(), w, h, fmt, _alphaStats))
    {
        return false;
    }
    path = AlphaScanPath::BlockRead;
    return true;
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
                // The one part of the texture cost that buys no pixels, so it is timed
                // separately. It used to be a whole second pass over the file: read every
                // level, decode the FULL-resolution one to RGBA8, walk every pixel's alpha
                // and discard the three colour bytes in four that the decode produced.
                //
                // A block format now answers it off the compressed alpha alone, from the
                // level the upload already inflated where it can. Everything else still
                // takes the old path, unchanged: no format loses its answer, and the answer
                // it gets is the same one.
                const bool timed = TextureStreamStatsEnabled();
                const uint64_t began = timed ? NowUs() : 0;
                AlphaScanPath path = AlphaScanPath::FullDecode;
                if (!ScanTopMipAlphaBlocks(path))
                {
                    _alphaStats = ScanTopMipAlphaStats(Name());
                }
                _alphaStatsScanned = true;
                if (timed)
                {
                    GTextureStreamStats.alphaScans.fetch_add(1, std::memory_order_relaxed);
                    if (path != AlphaScanPath::FullDecode)
                    {
                        GTextureStreamStats.alphaBlockScans.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (path == AlphaScanPath::Handoff)
                    {
                        GTextureStreamStats.alphaHandoffs.fetch_add(1, std::memory_order_relaxed);
                    }
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

// WGR_TEXTURE_MIP_BIAS=N drops the N most detailed mip levels of every block-compressed
// object texture at upload. 0 (default) uploads the full chain, exactly as before.
//
// This is the one lever here that BUYS PERFORMANCE WITH VISUAL QUALITY, so it is off by
// default and says so. Each level costs 4x the one below it, so a bias of 1 removes ~75% of
// a texture's bytes and halves its resolution.
//
// It exists for DayZ Chernarus, where the alternative is not "slightly softer textures", it
// is a world that does not render at all. Measured there: 3,552 object textures for 3.84 GB
// against a 2 GB budget, and the texture LRU starved to 1-31 eviction candidates per round
// because RetainGpuModel pins every texture of every registered model. Memory climbs
// unbounded until the driver refuses an allocation, and the world dies at ~22k of 199,917
// objects. Reforger Everon does not need this at all -- 199,881 objects but only 218 MB of
// textures, because they are heavily reused.
//
// The RIGHT fix is for a pin to mean "recently drawn" rather than "registered", so the LRU
// can do its job. That is not this change, because evicting a texture a live bind group
// still references is very plausibly the `Texture with 'wgr_texture' label is invalid`
// failure already seen, and it cannot be made safe without auditing the retained bind
// lifetime. Until then this bounds the demand instead of improving the supply.
int TextureMipBias()
{
    static const int bias = []
    {
        const char* v = std::getenv("WGR_TEXTURE_MIP_BIAS");
        const int parsed = v ? std::atoi(v) : 0;
        // Clamped: a bias large enough to consume the whole chain would leave nothing to
        // upload, and the floor below keeps at least one level regardless.
        return parsed > 0 ? std::min(parsed, 4) : 0;
    }();
    return bias;
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
        // WGR_TEXTURE_MIP_BIAS drops the most detailed levels: the chain simply STARTS
        // lower. Always leaves at least one level, so a heavily-biased small texture
        // degrades to its coarsest level rather than to nothing.
        const int firstLevel = std::min(TextureMipBias(), _nMipmaps - 1);
        const int levelCount = _nMipmaps - firstLevel;
        std::vector<size_t> offsets(static_cast<size_t>(levelCount));
        size_t total = 0;
        for (int i = 0; i < levelCount; i++)
        {
            offsets[i] = total;
            const PacLevelMem& mip = _mipmaps[firstLevel + i];
            total += render::mipmap::ComputeLayout(dst, mip._w, mip._h).dataSize;
        }
        // Destinations are resolved only once the buffer is at its final size — a pointer
        // taken before the allocation would not survive it.
        std::vector<uint8_t> blocks(total);
        std::vector<MipmapRead> reads(static_cast<size_t>(levelCount));
        for (int i = 0; i < levelCount; i++)
        {
            reads[i] = MipmapRead{blocks.data() + offsets[i], &_mipmaps[firstLevel + i], firstLevel + i};
        }
        const uint64_t readBegan = timed ? NowUs() : 0;
        const bool ok = _src->GetMipmapChain(reads.data(), levelCount);
        if (timed)
        {
            GTextureStreamStats.readUs.fetch_add(NowUs() - readBegan, std::memory_order_relaxed);
            GTextureStreamStats.blockBytes.fetch_add(total, std::memory_order_relaxed);
        }
        if (ok && !blocks.empty())
        {
            const uint64_t createBegan = timed ? NowUs() : 0;
            // Dimensions follow the level actually uploaded, not the file's level 0 —
            // handing wgpu the full-resolution size with a biased chain describes a
            // texture whose level 0 is missing, which is a validation error, not a
            // smaller texture. _w/_h are updated below so samplers and any later reader
            // see the size that really exists on the GPU.
            _w = _mipmaps[firstLevel]._w;
            _h = _mipmaps[firstLevel]._h;
            _gpuHandle = wgr_texture_create(r, static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), bcFormat,
                                            static_cast<uint32_t>(levelCount), 0, blocks.data(),
                                            static_cast<uint32_t>(blocks.size()));
            if (timed)
            {
                GTextureStreamStats.createUs.fetch_add(NowUs() - createBegan, std::memory_order_relaxed);
            }
            // Both call sites classify this texture on the line after they upload it. Park
            // the level they will need so that call reads no file. The condition is exactly
            // GetAlphaClass's: a texture that will never reach a scan -- no alpha, a 1-bit
            // format, or a class already decided -- is not worth parking, and one that is
            // costs only a vector move, never a decode.
            const char* name = Name();
            if (_gpuHandle && _alphaClass < 0 && !_alphaStatsScanned && _src->IsAlpha() && IsBlockAlphaFormat(dst) &&
                name && *name)
            {
                TopMipHandoff& handoff = GTopMipHandoff;
                // offsets[1] is where level 1 begins, i.e. exactly the size of level 0.
                handoff.topSize = _nMipmaps > 1 ? offsets[1] : total;
                handoff.blocks = std::move(blocks);
                handoff.w = _mipmaps[0]._w;
                handoff.h = _mipmaps[0]._h;
                handoff.format = dst;
                handoff.name.assign(name);
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
