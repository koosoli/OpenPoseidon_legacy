#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp> // RFG-070 `enft|` layer tint
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace Poseidon
{

namespace
{

//! SINKHOLE W0, from Malprave's ddsTextures: a DXT1 block in its 3-colour mode
//! (color0 <= color1) that uses index 3 has transparent texels. Such a texture is a
//! punch-through cut-out (leaves, fences), so report it as transparent rather than as
//! the opaque texture DXT1 otherwise is.
bool Dxt1HasPunchThrough(const std::vector<uint8_t>& blocks)
{
    for (size_t b = 0; b + 8 <= blocks.size(); b += 8)
    {
        const uint16_t c0 = static_cast<uint16_t>(blocks[b] | (blocks[b + 1] << 8));
        const uint16_t c1 = static_cast<uint16_t>(blocks[b + 2] | (blocks[b + 3] << 8));
        if (c0 > c1)
            continue;
        const uint32_t idx = static_cast<uint32_t>(blocks[b + 4]) | (static_cast<uint32_t>(blocks[b + 5]) << 8) |
                             (static_cast<uint32_t>(blocks[b + 6]) << 16) | (static_cast<uint32_t>(blocks[b + 7]) << 24);
        for (int k = 0; k < 16; ++k)
            if (((idx >> (k * 2)) & 3) == 3)
                return true;
    }
    return false;
}

int DecodedMaxEdge()
{
    const char* value = std::getenv("POSEIDON_DECODED_MAX_EDGE");
    const int parsed = value && *value ? std::atoi(value) : 0;
    return parsed >= 64 && parsed <= 8192 ? parsed : 1024;
}

// Diagnostic only: separate CPU preparation from later GPU upload. No scheduling changes.
struct DdsPreparationTrace
{
    using Clock = std::chrono::steady_clock;
    static bool Enabled()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("POSEIDON_DDS_PREP_TRACE");
            return value && value[0] == '1';
        }();
        return enabled;
    }
    bool enabled = Enabled();
    const char* name;
    Clock::time_point last = enabled ? Clock::now() : Clock::time_point{};
    double read = 0, decode = 0, coverageRead = 0, coverageDecode = 0, merge = 0, tint = 0;
    size_t inputBytes = 0, outputBytes = 0;
    bool success = false;
    bool reusedDecode = false;
    explicit DdsPreparationTrace(const char* path) : name(path) {}
    void Mark(double& phase)
    {
        if (!enabled) return;
        const auto now = Clock::now();
        phase += std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
    }
    ~DdsPreparationTrace()
    {
        if (!enabled) return;
        const double finish = std::chrono::duration<double, std::milli>(Clock::now() - last).count();
        const double total = read + decode + coverageRead + coverageDecode + merge + tint + finish;
        if (total >= 2.0)
            LOG_INFO(Graphics,
                "DDS prepare: total={:.3f} read={:.3f} decode={:.3f} coverageRead={:.3f} coverageDecode={:.3f} merge={:.3f} tint={:.3f} finish={:.3f} inputBytes={} outputBytes={} success={} reusedDecode={} mainThread={} name='{}'",
                total, read, decode, coverageRead, coverageDecode, merge, tint, finish,
                inputBytes, outputBytes, success, reusedDecode, Foundation::IsMainThread(), name);
    }
};

// RFG-023: a composite name, `enfa|<coverage path>|<colour path>`.
//
// Enfusion keeps a leaf's coverage in its OWN file, so a face that names only the
// colour can never be a cutout. Swapping a merged texture onto the built shape does
// not work -- Shape::RegisterTexture leaves the section pointing at the entry it
// had, measured -- so the two files are named TOGETHER and merged here, where the
// engine's ordinary texture path then uploads, alpha-classifies and caches the
// result like any other texture.
//
// The marker is a PREFIX so the name still ends in `.edds` and the extension-keyed
// factory selection keeps working unchanged.
constexpr const char* kEnfusionAlphaPrefix = "enfa|";

bool SplitEnfusionAlphaName(const char* name, std::string& colour, std::string& coverage)
{
    if (name == nullptr || std::strncmp(name, kEnfusionAlphaPrefix, 5) != 0)
        return false;
    const char* rest = name + 5;
    const char* bar = std::strchr(rest, '|');
    if (bar == nullptr)
        return false;
    coverage.assign(rest, static_cast<size_t>(bar - rest));
    colour.assign(bar + 1);
    return !coverage.empty() && !colour.empty();
}

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
        // RFG-047. Reached only when the compressed pass-through is on and the levels
        // were left as blocks; the decode branch rewrites _format to ARGB8888 first, so
        // a decoded texture never lands here.
        case PixelFormat::BC4:
            return PacBC4;
        case PixelFormat::BC5:
            return PacBC5;
        case PixelFormat::BC7:
            return PacBC7;
        default:
            return PacARGB8888;
    }
}

std::string LowerCopy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

//! RFG-039: `enfc|R,G,B` -- a flat colour, with no file behind it.
//!
//! Reforger's `MatPBRBasic` often carries no albedo map at all, only a `Color`
//! constant: `HouseRuin_01_BrickPile_MLOD`, `CrashBarrier_MLOD`,
//! `CrateWooden_Plain_MLOD` and 70-odd others in one village. A section with no
//! texture draws WHITE, so a material that says "this is dark grey" produced a
//! bright white crash barrier -- which is what "many buildings are white and
//! untextured" is actually made of. It is not a missing texture; it is a texture
//! that was never meant to exist.
bool SplitEnfusionColourName(const char* name, float rgb[3])
{
    if (name == nullptr || std::strncmp(name, "enfc|", 5) != 0)
        return false;
    return std::sscanf(name + 5, "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]) == 3;
}

bool IsBlockFormat(PixelFormat format)
{
    return format == PixelFormat::DXT1 || format == PixelFormat::DXT3 || format == PixelFormat::DXT5 ||
           format == PixelFormat::BC4 || format == PixelFormat::BC5 || format == PixelFormat::BC7;
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

// RFG-047. `std::atomic` rather than a plain bool: the object-stream workers construct
// texture sources off the main thread, and the switch is flipped from the dev panel.
std::atomic<bool> GDdsCompressedPassthrough{false};

} // namespace

//! RFG-070: rescale a SHARED library tile to the colour the material authored for it.
//!
//! This is MAT-054's rule (XobCommand.cpp:1665-1685), moved to the native path, and it is
//! deliberately the same arithmetic rather than a second one:
//!
//!   * per channel, gain = target / mean, so a grey tile becomes olive and keeps its grain;
//!   * a channel whose mean is ~0 is left alone rather than multiplied by infinity;
//!   * `Color_N` is LINEAR and the tile's texels are sRGB-ENCODED, so the target is encoded
//!     before the two are compared. Matching them raw is about a stop and a half too dark
//!     -- the exporter measured WaterTower_01 rendering as a flat black sphere that way.
//!
//! RFG-071 SUPERSEDES the second of those, and WaterTower_01 is exactly why: it is the one
//! model in a 35-material paired census where the encode-the-target rule lands on the
//! authored bake (107/116/120 against a predicted 118/123/127), and it is the model the
//! rule was calibrated on. Over the other 34 the same rule runs 1.82x bright. `Color_N`
//! multiplies -- see the block inside the function -- and this rescale is now the A/B
//! branch, kept because it is what shipped and what the converting exporter still bakes.
//!
//! Two things differ from the exporter and both are forced by where this runs:
//!
//!   * the mean comes from the TOP level and the gain is applied to every level. The
//!     exporter has one image and regenerates its mips; here the mip chain is already in
//!     the file, and a per-level mean would give each mip its own gain -- the same wall
//!     changing colour as you walk away from it.
//!   * alpha is not touched. Coverage arrives through `enfa|` (already merged by the time
//!     this runs) or not at all, and the fourth channel of a `_BCR` is roughness.
static void ApplyEnfusionLayerTintWithMode(std::vector<DDSMipLevel>& levels, const float rgb[3], bool linearMultiply)
{
    if (levels.empty())
        return;
    const DDSMipLevel& top = levels.front();
    const size_t texels = top.data.size() / 4;
    if (texels == 0)
        return;
    const auto linearToSrgb = [](double v)
    { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; };
    const auto srgbToLinear = [](double v)
    { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    // ARGB8888 in this engine means B,G,R,A in memory, so channel 0 is BLUE. Applying the
    // material's r,g,b in index order tints a beige wall blue -- the trap the `enfc|` flat
    // colour above already documents, and the one this has to get right per channel and
    // not just on average.
    const int channelOfRgb[3] = {2, 1, 0}; // r -> byte 2, g -> byte 1, b -> byte 0

    // RFG-071: `Color_N` is a MULTIPLIER, not the surface's mean.
    //
    // The rule below (kept, as the A/B) reads it as a linear mean and rescales the tile to
    // it. Measured against the only ground truth the corpus has -- `*_MLOD_BCR.edds`, the
    // albedo Enfusion's OWN toolchain bakes from the very same layered material -- over 35
    // materials where the pairing is unambiguous (one bake, one MatPBRMulti with a MaskMap
    // and a Color_N in the folder), predicting the bake's per-channel mean from the
    // mask-weighted Color_N mix:
    //
    //   rescale to linearToSrgb(C)   median 1.82x the authored mean, mean |log| 0.644
    //   rescale to C                 median 0.83x                  , mean |log| 0.588
    //   multiply in linear (this)    median 1.29x                  , mean |log| 0.403
    //   multiply in gamma            median 0.66x                  , mean |log| 0.656
    //
    // 1.82x in an 8-bit sRGB mean is ~3.6x the radiance, which is the reported defect: every
    // MatPBRMulti surface -- walls, benches, fences, signs -- washed out while the same
    // frame's cobblestone and tree trunks, which carry their own `BCRMap` and are never
    // tinted, looked right. It is also FLATTER: encoding the target compresses the channel
    // ratios, so the median max/min channel ratio came out 1.040 against the bake's 1.132
    // (this rule: 1.088). A near-white paint tile at Color_1 = (0.287, 0.192, 0.07) is the
    // worked example -- rescaled it reaches (0.573, 0.482, 0.297), a grey wall.
    //
    // Per-texel through a 256-entry LUT, so the tile keeps its grain in LINEAR terms rather
    // than having its gamma-space grain scaled; that is what a tint physically is.
    if (linearMultiply)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double k = std::clamp<double>(rgb[c], 0.0, 1.0);
            uint8_t lut[256];
            for (int v = 0; v < 256; ++v)
                lut[v] = static_cast<uint8_t>(
                    std::clamp(linearToSrgb(srgbToLinear(v / 255.0) * k) * 255.0 + 0.5, 0.0, 255.0));
            const size_t byte = static_cast<size_t>(channelOfRgb[c]);
            for (DDSMipLevel& level : levels)
                for (size_t at = byte; at < level.data.size(); at += 4)
                    level.data[at] = lut[level.data[at]];
        }
        return;
    }

    for (int c = 0; c < 3; ++c)
    {
        const size_t byte = static_cast<size_t>(channelOfRgb[c]);
        double sum = 0.0;
        for (size_t t = 0; t < texels; ++t)
            sum += top.data[t * 4 + byte];
        const double mean = sum / (255.0 * static_cast<double>(texels));
        if (!(mean > 1e-3))
            continue;
        const double target = linearToSrgb(std::clamp<double>(rgb[c], 0.0, 1.0));
        const double gain = target / mean;
        for (DDSMipLevel& level : levels)
            for (size_t at = byte; at < level.data.size(); at += 4)
                level.data[at] = static_cast<uint8_t>(std::clamp(level.data[at] * gain + 0.5, 0.0, 255.0));
    }
}

void ApplyEnfusionLayerTint(std::vector<DDSMipLevel>& levels, const float rgb[3])
{
    ApplyEnfusionLayerTintWithMode(levels, rgb, Enfusion::LayerTintLinearMultiply());
}

DdsPreparationOptions CaptureDdsPreparationOptions()
{
    return {DdsCompressedPassthrough(), Enfusion::LayerTintLinearMultiply(), DecodedMaxEdge()};
}

void SetDdsCompressedPassthrough(bool on)
{
    const bool was = GDdsCompressedPassthrough.exchange(on, std::memory_order_relaxed);
    if (was != on)
        LOG_INFO(Graphics,
                 "DDS compressed pass-through {} -- BC4/BC5/BC7 are {} (already-uploaded textures keep the "
                 "form they were loaded in; reload the world for a clean A/B)",
                 on ? "ON" : "OFF", on ? "handed to the GPU as blocks" : "decoded to 32-bit as before");
}

bool DdsCompressedPassthrough()
{
    return GDdsCompressedPassthrough.load(std::memory_order_relaxed);
}

bool IsEnfusionRgNormalName(const char* name)
{
    // NMO = normal RG, metalness B, occlusion A; NTC = normal RG,
    // transmittance B, cavity A; NHO = normal RG, height B, occlusion A.
    // None stores normal X in alpha.
    if (!name) return false;
    const std::string lower = LowerCopy(name);
    if (lower.find("_nmo") != std::string::npos) return true;
    static const bool legacyNtc = [] {
        const char* value = std::getenv("POSEIDON_LEGACY_NTC_NORMALS");
        return value && value[0] == '1';
    }();
    const size_t slash = lower.find_last_of("/\\");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = lower.find_last_of('.');
    const size_t end = dot == std::string::npos || dot < start ? lower.size() : dot;
    if (end >= start + 4 && lower.compare(end - 4, 4, "_nho") == 0) return true;
    return !legacyNtc && end >= start + 4 && lower.compare(end - 4, 4, "_ntc") == 0;
}

TextureSourceDDS::TextureSourceDDS() = default;
TextureSourceDDS::~TextureSourceDDS() = default;

bool TextureSourceDDS::InitFromMemory(const void* data, size_t size, const char* nameForErrors, bool forceDecode)
{
    return InitFromMemoryWithOptions(data, size, nameForErrors, forceDecode, CaptureDdsPreparationOptions());
}

bool TextureSourceDDS::InitFromMemoryWithOptions(const void* data, size_t size, const char* nameForErrors,
                                               bool forceDecode, const DdsPreparationOptions& options)
{
    _tintAverageLevel = {};
    _preparedBc3 = {};
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

    // RFG-047: the level cap, shared by BOTH paths so the A/B moves ONE variable.
    //
    // RFG-044 introduced it as a decode-cost cap; it is really a texel-budget cap and it
    // belongs to the texture, not to the decoder. Sharing it means the compressed and the
    // decoded upload of the same file describe the SAME image at the SAME resolution, so
    // the difference between them is bytes and nothing else -- which is the only way the
    // saving can be quoted as a number rather than as a trade.
    const auto capLevels = [this, &options]
    {
        const int maxEdge = options.decodedMaxEdge;
        size_t firstLevel = 0;
        while (firstLevel + 1 < _levels.size() &&
               (_levels[firstLevel].width > maxEdge || _levels[firstLevel].height > maxEdge))
            ++firstLevel;
        if (firstLevel > 0)
        {
            _levels.erase(_levels.begin(), _levels.begin() + static_cast<ptrdiff_t>(firstLevel));
            _w = _levels[0].width;
            _h = _levels[0].height;
        }
    };

    // RFG-047: hand the blocks straight through when the backend speaks them.
    //
    // The decode below exists only because the Pac container has no name for these three
    // formats; the GPU has had one since D3D11. Passing them through saves 4x the bytes for
    // BC7 and 8x for BC5, AND the decode itself. Everything the decode path does on the way
    // (the alpha copies, the coverage merge) has to be answered another way, so this is
    // deliberately NARROW -- it declines whenever it cannot answer honestly, and the decode
    // path below is then unchanged:
    //
    //   * `enfa|` composites (RFG-023) merge a SECOND file into the alpha channel. There is
    //     no way to do that to a compressed block without re-encoding, so Init() forces the
    //     decode for them. That is where Everon's cutout foliage lives, which is also why
    //     the alpha classification below can be conservative.
    //   * a BC7 whose alpha is NOT flat is declined, unless the name says the fourth channel
    //     is a material property rather than coverage (`_BCR` -- Base Colour + Roughness,
    //     RFG-033). The alpha CLASS is decided from decoded pixels several layers away
    //     (TextureWgpu::GetAlphaClass) and a wrong verdict there draws a wall see-through;
    //     rather than teach that scan a fourth block codec, anything that might need it
    //     keeps the old path. Measured on Everon, the population that matters (`_BCR`
    //     albedos and `_NMO` normals) is covered.
    //   * BC4/BC5 have no alpha channel at all -- BC5 samples as (r, g, 0, 1) -- so there is
    //     nothing to classify and they always pass through.
    //
    // The normal-map channel swap (RFG-045) cannot be done here either; see
    // IsEnfusionRgNormalName and the section flag it feeds.
    const bool dxgiBlock =
        _format == PixelFormat::BC4 || _format == PixelFormat::BC5 || _format == PixelFormat::BC7;
    if (dxgiBlock && !forceDecode && options.compressedPassthrough)
    {
        capLevels();
        // RFG-094: alpha that is DATA, not coverage, by the name's packing suffix. `_bcr`
        // keeps roughness there (measured, RFG-049); `_nmo` occlusion, `_nho` occlusion,
        // `_ntc` cavity, `_mask` / `_cmask` a layer or clutter weight. None of them is a
        // cutout, so none needs the decode that "live alpha" used to buy: on native Everon
        // at play settings the decoded normal maps and masks were the last 100 RGBA8
        // uploads of the 1024 class, 500 MB, and every one of them a BC7/BC5 file.
        const std::string lowerWho = LowerCopy(who);
        const bool roughnessInAlpha = lowerWho.find("_bcr") != std::string::npos;
        const bool dataInAlpha = roughnessInAlpha || lowerWho.find("_nmo") != std::string::npos ||
                                 lowerWho.find("_nho") != std::string::npos ||
                                 lowerWho.find("_ntc") != std::string::npos ||
                                 lowerWho.find("_mask") != std::string::npos;
        // One decode, of the SMALLEST level -- typically 4x4 or 8x8. It answers two
        // questions the block data cannot: the average colour (the distant-LOD stand-in,
        // which AverageOfSmallest returns black for on a block format), and whether the
        // alpha channel is flat enough that no classification is needed.
        bool alphaIsFlat = true;
        PackedColor average = PackedBlack;
        {
            const DDSMipLevel& small = _levels.back();
            const size_t texels = static_cast<size_t>(small.width) * static_cast<size_t>(small.height);
            std::vector<uint8_t> rgba(texels * 4);
            if (texels > 0 && !small.data.empty() &&
                ConvertPixels(small.data.data(), rgba.data(), small.width, small.height, _format,
                              PixelFormat::RGBA8888))
            {
                uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                uint8_t aMin = 255, aMax = 0;
                for (size_t i = 0; i < texels; ++i)
                {
                    sumR += rgba[i * 4 + 0];
                    sumG += rgba[i * 4 + 1];
                    sumB += rgba[i * 4 + 2];
                    const uint8_t a = rgba[i * 4 + 3];
                    sumA += a;
                    aMin = std::min(aMin, a);
                    aMax = std::max(aMax, a);
                }
                average = PackedColor(static_cast<uint8_t>(sumR / texels), static_cast<uint8_t>(sumG / texels),
                                      static_cast<uint8_t>(sumB / texels), static_cast<uint8_t>(sumA / texels));
                // 250 rather than 255: BC7's alpha endpoint interpolation is lossy, so an
                // authored opaque texture can come back a couple of levels short.
                alphaIsFlat = aMin >= 250;
            }
            else
            {
                alphaIsFlat = false; // could not look: decline rather than guess
            }
        }
        const bool noAlphaChannel = _format == PixelFormat::BC4 || _format == PixelFormat::BC5;
        if (noAlphaChannel || alphaIsFlat || dataInAlpha)
        {
            // Never claim coverage on this path: BC7 carries a fourth channel whether or
            // not the author used it, and the one population that deliberately uses it
            // (`_BCR`) uses it for ROUGHNESS. A texture with real coverage was declined
            // above and is on the decode path, where the old rules still apply.
            _hasAlpha = false;
            _avgColor = average;
            _pacFormat = ToPacFormat(_format);
            _mipmaps = static_cast<int>(_levels.size());
            _isTransparent = false;
            {
                static std::atomic<unsigned> reportedPassthrough{0};
                if (reportedPassthrough.load(std::memory_order_relaxed) < 8 &&
                    reportedPassthrough.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    LOG_INFO(Graphics, "EDDS {}: {}x{} format {} uploaded COMPRESSED ({} levels, {} bytes)", who, _w,
                             _h, static_cast<int>(_format), _mipmaps, [&]
                             {
                                 size_t total = 0;
                                 for (const DDSMipLevel& l : _levels)
                                     total += l.data.size();
                                 return total;
                             }());
                }
            }
            return true;
        }
        static std::atomic<unsigned> reportedDecline{0};
        if (reportedDecline.load(std::memory_order_relaxed) < 8 &&
            reportedDecline.fetch_add(1, std::memory_order_relaxed) < 8)
        {
            LOG_INFO(Graphics, "EDDS {}: format {} has live alpha -- decoding rather than passing blocks through", who,
                     static_cast<int>(_format));
        }
    }

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
        // RFG-044: drop the top mips before decoding, not after.
        //
        // These three formats have no PacFormat, so RFG-016 decodes them to plain
        // 32-bit -- which is 8x the bytes of the BC5 it came from and 4x the BC7. That
        // was affordable while only albedos took the path. It stopped being affordable
        // the moment normal maps started loading too (RFG-041): 600 distinct Reforger
        // meshes now exhaust GPU memory, and the failure is not a message but a named
        // buffer going invalid followed by every frame failing `get_current_texture`.
        //
        // The mip chain is already in the file, so skipping levels above the cap is a
        // pure saving: no resampling, no quality question beyond the cap itself. 1024
        // is four times the texels of 2048 and is the size at which a Reforger wall
        // texture still reads correctly at arm's length.
        capLevels();

        bool decoded = true;
        for (DDSMipLevel& level : _levels)
        {
            if (level.width <= 0 || level.height <= 0 || level.data.empty())
                continue;
            std::vector<uint8_t> rgba(static_cast<size_t>(level.width) * static_cast<size_t>(level.height) * 4);
            static const int batchMode = [] {
                const char* value = std::getenv("POSEIDON_DDS_BC7_BATCH");
                // Three same-route pairs reduced loading spikes with byte-identical output.
                // Keep 0 as the serial control, 2 as the expensive in-game oracle.
                return value && value[0] == '2' ? 2 : value && value[0] == '0' ? 0 : 1;
            }();
            TaskPool* pool = GetGlobalTaskPool();
            const bool batched = batchMode != 0 && _format == PixelFormat::BC7 &&
                level.width >= 256 && level.height >= 256 && Foundation::IsMainThread() && pool;
            const bool ok = batched ? DecodeBc7ImageBatched(level.data.data(), rgba.data(), level.width, level.height, *pool) :
                ConvertPixels(level.data.data(), rgba.data(), level.width, level.height, _format, PixelFormat::RGBA8888);
            if (batched && batchMode == 2)
            {
                std::vector<uint8_t> reference(rgba.size());
                const bool serialOk = DecodeBc7Image(level.data.data(), reference.data(), level.width, level.height);
                if (serialOk != ok || reference != rgba)
                {
                    LOG_ERROR(Graphics, "DDS BC7 batch verification: MISMATCH '{}' {}x{}", who, level.width, level.height);
                    return false;
                }
                LOG_INFO(Graphics, "DDS BC7 batch verification: identical '{}' {}x{}", who, level.width, level.height);
            }
            if (!ok)
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
        // RFG-045: a two-channel normal lands in R,G -- the engine reads A,G.
        //
        // `decode_nohq` (gpu_driven.wgsl:635) takes `vec2(texel.a, texel.g)`, which is the
        // DXT5nm convention every `_nohq.paa` in the OFP/Arma corpus uses. BC5 is by
        // definition a two-channel format and puts the normal in R,G, so read that way the
        // X component comes from an alpha channel that carries nothing -- a constant 255,
        // i.e. x = +1, a normal pointing fully sideways on every texel. That is the
        // silvery-blue sheen Reforger leaves picked up the moment normal maps started
        // loading at all.
        //
        // Fixed on the data side rather than in the shader: copying R into A makes the
        // existing decode correct without giving the shader a second convention to branch
        // on.
        //
        // Gated on the NAME as well as on BC5, and the name is the one that matters here:
        // measured, every Reforger `_NMO` is BC7 (format 13), not BC5. BC7 keeps all four
        // channels; NMO alpha is occlusion and NTC alpha is cavity, not normal X.
        // BC5 is covered too because it is a
        // two-channel format by definition and can carry nothing else.
        // Native consumers now decode RG in both upload paths. Preserve authored
        // height/transmission and AO/cavity; only legacy BC5 still needs AG.
        if (_format == PixelFormat::BC5 && !IsEnfusionRgNormalName(who))
        {
            for (DDSMipLevel& level : _levels)
                for (size_t i = 0; i + 3 < level.data.size(); i += 4)
                    level.data[i + 3] = level.data[i + 2]; // ARGB8888 in memory is B,G,R,A
        }
        {
            static std::atomic<unsigned> reportedNormal{0};
            if (IsEnfusionRgNormalName(who) && reportedNormal.load(std::memory_order_relaxed) < 4 &&
                reportedNormal.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                LOG_INFO(Graphics, "EDDS normal map {}: source format {} decoded to 32-bit", who,
                         static_cast<int>(_format));
            }
        }

        _format = PixelFormat::ARGB8888;
        // Do NOT claim alpha here. BC7 carries a fourth channel whether or not the
        // author used it, and forcing the flag put every leaf card on the blended
        // path -- the first run of this drew Everon's trees as flat white shards.
        // The existing format rule below decides, exactly as it does for a .paa.

        // RFG-033: in Enfusion, `_BCR` stands for Base Colour + ROUGHNESS. The fourth
        // channel is a material property, not coverage -- and read as coverage it
        // makes a plastered wall as see-through as its roughness map is dark. That is
        // the whole of "many houses look transparent or as if walls were missing":
        // Everon's buildings are BC7 `_BCR` albedos to a texture.
        //
        // Keep the authored roughness for future native PBR shading. The texture
        // source still reports no coverage; TextureWgpu::Init must also avoid
        // ForceAlpha for this exact source name so its pixel classifier never
        // mistakes roughness for holes. An enfa| composite replaces this channel
        // with its separate coverage image after the decode.
        if (Enfusion::IsOriginalBcrRoughnessName(who))
        {
            _hasAlpha = false;
        }
    }

    // RFG-098: R8 -- a single-channel mask. Reforger's murals (the painted adverts on
    // village gables, `Murals_NAVA_A.edds`, 2048^2) and other masks arrive as one byte per
    // texel, and the PAA-era format table has no such thing, so the material lost its mask
    // with an error on every load. Expanded to grey ARGB8888: every reader downstream
    // takes that, and a mask is read from one channel anyway.
    if (_format == PixelFormat::R8)
    {
        for (DDSMipLevel& level : _levels)
        {
            const size_t texels = static_cast<size_t>(level.width) * static_cast<size_t>(level.height);
            if (level.data.size() < texels)
                continue;
            std::vector<uint8_t> rgba(texels * 4);
            for (size_t t = 0; t < texels; ++t)
            {
                const uint8_t v = level.data[t];
                rgba[t * 4 + 0] = v; // B
                rgba[t * 4 + 1] = v; // G
                rgba[t * 4 + 2] = v; // R
                rgba[t * 4 + 3] = 255;
            }
            level.data = std::move(rgba);
        }
        _format = PixelFormat::ARGB8888;
        _hasAlpha = false;
    }
    _pacFormat = ToPacFormat(_format);
    if (!IsBlockFormat(_format) && _format != PixelFormat::ARGB8888 && _format != PixelFormat::RGBA8888)
    {
        LOG_ERROR(Graphics, "DDS {}: pixel format {} has no PacFormat mapping", who, static_cast<int>(_format));
        return false;
    }
    _mipmaps = static_cast<int>(_levels.size());
    _avgColor = AverageOfSmallest(_levels, _format);
    _isTransparent = _format == PixelFormat::DXT1 && Dxt1HasPunchThrough(_levels[0].data);
    return true;
}

bool TextureSourceDDS::Init(const char* name, PacLevelMem* mips, int maxMips)
{
    return InitFromReader(name, mips, maxMips, SlurpFile);
}

bool TextureSourceDDS::InitFromReader(const char* name, PacLevelMem* mips, int maxMips, const Reader& reader,
                                    DdsPreparationOptions options)
{
    if (!name || maxMips < 0 || (!mips && maxMips > 0) || !reader ||
        options.decodedMaxEdge < 64 || options.decodedMaxEdge > 8192)
        return false;
    DdsPreparationTrace trace(name);
    _name = name;
    _tintAverageLevel = {};
    _preparedBc3 = {};

    // RFG-070: `enft|r,g,b|<inner>` -- strip the tint FIRST and let everything below see
    // the name it would have seen anyway. That is what makes the two composite forms
    // nest: a leaf card on a tinted shared tile is `enft|...|enfa|coverage|colour`, and
    // the coverage merge and the tint are then two independent steps in one Init.
    //
    // `_name` keeps the WHOLE name on purpose. It is the texture bank's cache key, so two
    // materials tinting the same tile differently must not collide -- and it is what the
    // renderer's `enfa|` tests read, which is why they go through SkipLayerTint.
    float layerTint[3] = {1.0f, 1.0f, 1.0f};
    const char* inner = name;
    const bool tinted = Enfusion::SplitLayerTintName(name, layerTint, &inner);
    if (tinted)
        name = inner;

    // RFG-039: a flat colour needs no file. Built at 4x4 rather than 1x1 because the
    // mip chain and the pitch arithmetic downstream both assume a real image.
    //
    // Not tinted, deliberately: an `enfc|` colour IS the material's own constant, so
    // rescaling it to a second constant would be applying the same number twice. The
    // sink never builds that pairing, and this returns before the tint below in case it
    // ever does.
    float solid[3] = {1.0f, 1.0f, 1.0f};
    if (SplitEnfusionColourName(name, solid))
    {
        const auto channel = [](float v)
        { return static_cast<uint8_t>(v <= 0.0f ? 0.0f : (v >= 1.0f ? 255.0f : v * 255.0f + 0.5f)); };
        // ARGB8888 in this engine means B,G,R,A in memory.
        const uint8_t texel[4] = {channel(solid[2]), channel(solid[1]), channel(solid[0]), 255};
        DDSMipLevel level;
        level.width = 4;
        level.height = 4;
        level.data.resize(4 * 4 * 4);
        for (size_t i = 0; i < level.data.size(); i += 4)
            std::memcpy(level.data.data() + i, texel, 4);
        _levels.clear();
        _levels.push_back(std::move(level));
        _format = PixelFormat::ARGB8888;
        _pacFormat = ToPacFormat(_format);
        _w = 4;
        _h = 4;
        _hasAlpha = false;
        _mipmaps = 1;
        _avgColor = AverageOfSmallest(_levels, _format);
        _isTransparent = false;
        if (mips != nullptr && maxMips > 0)
        {
            mips[0]._w = 4;
            mips[0]._h = 4;
            mips[0]._sFormat = _pacFormat;
        }
        trace.success = true;
        trace.outputBytes = _levels.front().data.size();
        return true;
    }

    // RFG-023: `enfa|<coverage>|<colour>` -- load the colour, then fold the coverage
    // file into its alpha. Enfusion keeps a leaf's opacity in a separate texture, so
    // this is the only point at which the two can become one image without the engine
    // learning a second kind of texture.
    std::string compositeColour;
    std::string compositeCoverage;
    const bool composite = SplitEnfusionAlphaName(name, compositeColour, compositeCoverage);

    std::vector<uint8_t> buffer;
    if (!reader(composite ? compositeColour.c_str() : name, buffer) || buffer.empty())
        return false;
    trace.inputBytes = buffer.size();
    trace.Mark(trace.read);

    // A tint needs PIXELS, the same way the coverage merge does -- there is no way to
    // rescale a BC7 block in place. So a tinted tile is uploaded 32-bit rather than
    // compressed, which is the RFG-047 cost, paid only by the materials that ask for it.
    static const bool reuseTintDecode = [] {
        const char* value = std::getenv("POSEIDON_DDS_TINT_REUSE");
        return value && std::strcmp(value, "1") == 0; // Experimental until byte/performance A/B acceptance.
    }();
    const bool reusable = reuseTintDecode && tinted && !composite && !IsEnfusionRgNormalName(name);
    const std::string reuseKey = reusable ? "dds-decode|" + std::to_string(options.decodedMaxEdge) + "|" +
                                           render::PreparedTextureStore::Key(name) : "";
    auto& prepared = render::PreparedTextureStore::Instance();
    trace.reusedDecode = reusable && prepared.CopyDdsDecode(reuseKey, buffer, *this);
    static const bool verifyReuse = [] {
        const char* value = std::getenv("POSEIDON_DDS_TINT_REUSE_VERIFY");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (trace.reusedDecode && verifyReuse)
    {
        TextureSourceDDS reference;
        bool same = reference.InitFromMemoryWithOptions(buffer.data(), buffer.size(), name, true, options) &&
                    reference._format == _format && reference._pacFormat == _pacFormat &&
                    reference._w == _w && reference._h == _h && reference._mipmaps == _mipmaps &&
                    reference._hasAlpha == _hasAlpha && reference._isTransparent == _isTransparent &&
                    static_cast<DWORD>(reference._avgColor) == static_cast<DWORD>(_avgColor) &&
                    reference._levels.size() == _levels.size();
        if (same)
            for (size_t i = 0; i < _levels.size(); ++i)
                same = same && reference._levels[i].width == _levels[i].width &&
                       reference._levels[i].height == _levels[i].height && reference._levels[i].data == _levels[i].data;
        LOG_INFO(Graphics, "DDS tint reuse verification: {} name='{}'", same ? "identical" : "MISMATCH", name);
        if (!same)
        {
            if (!InitFromMemoryWithOptions(buffer.data(), buffer.size(), name, true, options)) return false;
            trace.reusedDecode = false;
        }
    }
    if (!trace.reusedDecode)
    {
        if (!InitFromMemoryWithOptions(buffer.data(), buffer.size(), name, /*forceDecode=*/composite || tinted, options))
            return false;
        if (reusable) prepared.PutDdsDecode(reuseKey, buffer, *this);
    }
    trace.Mark(trace.decode);

    if (composite)
    {
        std::vector<uint8_t> coverageBytes;
        TextureSourceDDS coverage;
        const bool coverageRead = reader(compositeCoverage.c_str(), coverageBytes) && !coverageBytes.empty();
        trace.inputBytes += coverageBytes.size();
        trace.Mark(trace.coverageRead);
        const bool coverageDecoded = coverageRead &&
            coverage.InitFromMemoryWithOptions(coverageBytes.data(), coverageBytes.size(), compositeCoverage.c_str(),
                                               /*forceDecode=*/true, options) &&
            !coverage._levels.empty();
        trace.Mark(trace.coverageDecode);
        if (coverageDecoded)
        {
            // Both are 32-bit here: InitFromMemory decodes BC4/BC5/BC7 and leaves raw
            // formats alone, and a coverage map is one of those. Nearest-sample rather
            // than assume matching sizes -- the two files are authored at the same
            // resolution but their mip counts need not agree.
            for (size_t i = 0; i < _levels.size(); ++i)
            {
                DDSMipLevel& dstLevel = _levels[i];
                const DDSMipLevel& srcLevel =
                    coverage._levels[std::min(i, coverage._levels.size() - 1)];
                if (dstLevel.width <= 0 || dstLevel.height <= 0 || srcLevel.width <= 0 || srcLevel.height <= 0)
                    continue;
                if (dstLevel.data.size() < static_cast<size_t>(dstLevel.width) * dstLevel.height * 4 ||
                    srcLevel.data.size() < static_cast<size_t>(srcLevel.width) * srcLevel.height * 4)
                    continue;
                for (int y = 0; y < dstLevel.height; ++y)
                {
                    const int sy = y * srcLevel.height / dstLevel.height;
                    for (int x = 0; x < dstLevel.width; ++x)
                    {
                        const int sx = x * srcLevel.width / dstLevel.width;
                        // The BRIGHTEST channel, not channel 0.
                        //
                        // A coverage map is single-channel (BC4 / R8), and the colour
                        // path above swaps red and blue to reach ARGB8888's B,G,R,A
                        // order -- which moves that one channel out of index 0. Taking
                        // index 0 read the swapped-in zero: measured as
                        // "aMin 0 aMax 0 clear 100.0%", an image that is entirely
                        // transparent and therefore classified opaque, which is a leaf
                        // card drawn as a solid quad. max() does not care which slot
                        // the value ended up in.
                        const size_t src = (static_cast<size_t>(sy) * srcLevel.width + sx) * 4;
                        const uint8_t coverageValue =
                            std::max({srcLevel.data[src], srcLevel.data[src + 1], srcLevel.data[src + 2]});
                        dstLevel.data[(static_cast<size_t>(y) * dstLevel.width + x) * 4 + 3] = coverageValue;
                    }
                }
            }
            // RFG-059: say what the coverage map actually contains, per channel.
            //
            // max(r,g,b) was chosen because the colour path's red/blue swap moves a
            // single-channel value out of index 0, and max() does not care which slot it
            // lands in. That is sound IF the file is single-channel. If it is not -- if
            // an `_A` packs several masks across RGB -- then max() unions them and the
            // card comes out nearly opaque with a few holes, which is exactly what the
            // leaf cards look like. One is a fix and the other is a bug, and the two are
            // indistinguishable from the merged alpha histogram alone. So the channels
            // are reported separately, once per texture.
            {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 4 && !_levels.empty() && !coverage._levels.empty())
                {
                    const auto& lv = coverage._levels[0];
                    long long sum[3] = {0, 0, 0};
                    uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
                    const size_t texels = lv.data.size() / 4;
                    for (size_t i = 0; i < texels; ++i)
                        for (int c = 0; c < 3; ++c)
                        {
                            const uint8_t v = lv.data[i * 4 + c];
                            sum[c] += v;
                            lo[c] = std::min(lo[c], v);
                            hi[c] = std::max(hi[c], v);
                        }
                    if (texels > 0)
                        LOG_INFO(Graphics,
                                 "EDDS coverage '{}' {}x{}: ch0 {}..{} mean {} | ch1 {}..{} mean {} | ch2 {}..{} mean {}",
                                 compositeCoverage, lv.width, lv.height, lo[0], hi[0], int(sum[0] / (long long)texels),
                                 lo[1], hi[1], int(sum[1] / (long long)texels), lo[2], hi[2],
                                 int(sum[2] / (long long)texels));
                }
            }
            _hasAlpha = true;
        }
        else
        {
            LOG_WARN(Graphics, "EDDS {}: coverage '{}' did not load; the cutout will be solid", name,
                     compositeCoverage);
        }
    }
    trace.Mark(trace.merge);

    // RFG-070: and now the colour. AFTER the coverage merge, so the two orders cannot
    // interact -- the merge only writes alpha and the tint only reads and writes RGB, but
    // running the tint first would compute its mean over an alpha channel that is about to
    // change, and the next person to read this would have to prove that does not matter.
    if (tinted)
    {
        ApplyEnfusionLayerTintWithMode(_levels, layerTint, options.linearTintMultiply);
        // The distant-LOD stand-in is derived from the pixels, so it has to be re-derived
        // from the tinted ones. Left alone it hands the far tier the SHARED tile's colour,
        // which is the whole defect this exists to fix, just at a distance.
        _avgColor = AverageOfSmallest(_levels, _format);
        static std::atomic<int> saidTint{0};
        if (saidTint.fetch_add(1) < 4)
            LOG_INFO(Graphics, "EDDS layer tint {:.3f},{:.3f},{:.3f} applied to '{}' ({} levels, avg now {},{},{})",
                     layerTint[0], layerTint[1], layerTint[2], name, _levels.size(), _avgColor.R8(), _avgColor.G8(),
                     _avgColor.B8());
    }
    trace.Mark(trace.tint);

    trace.success = BindPreparedMips(mips, maxMips);
    if (trace.enabled)
        for (const auto& level : _levels)
            trace.outputBytes += level.data.size();
    return trace.success;
}

size_t TextureSourceDDS::PreparedByteSize() const
{
    size_t bytes = sizeof(TextureSourceDDS) + _levels.capacity() * sizeof(DDSMipLevel) +
                   _tintAverageLevel.data.capacity();
    for (const auto& level : _levels) bytes += level.data.capacity();
    return bytes + _preparedBc3.RetainedBytes();
}

size_t TextureSourceDDS::CompositeBc3Reservation() const
{
    if (std::strncmp(static_cast<const char*>(_name), "enfa|", 5) != 0 ||
        _pacFormat != PacARGB8888 || _levels.empty() || _w > 2048 || _h > 2048 ||
        !_preparedBc3.blocks.empty()) return 0;
    Bc3EncodingFootprint footprint;
    if (!MeasureBc3EncodingFootprint(_w, _h, footprint) ||
        _levels.front().width != _w || _levels.front().height != _h ||
        _levels.front().data.size() < footprint.rgbaBytes) return 0;
    return PreparedByteSize() + footprint.scratchBytes;
}

bool TextureSourceDDS::PrepareCompositeBc3(size_t reservedBytes, const Bc3EncodingObserver* observer)
{
    const size_t required = CompositeBc3Reservation();
    if (!required || reservedBytes < required) return false;
    Bc3EncodingFootprint footprint;
    if (!MeasureBc3EncodingFootprint(_w, _h, footprint)) return false;
    // Same top source pixels, channel swap, encoder and generated mips as wgpu's
    // owner path. Lower original DDS mips and alpha/header semantics are untouched.
    std::vector<uint8_t> rgba(footprint.rgbaBytes);
    PacLevelMem mip;
    mip._w = static_cast<short>(_w); mip._h = static_cast<short>(_h);
    mip._dFormat = PacARGB8888; mip._pitch = _w * 4;
    if (!GetMipmapData(rgba.data(), mip, 0)) return false;
    for (size_t i = 0; i + 2 < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    Bc3MipChain prepared;
    if (!EncodeBc3ChainRGBA(rgba.data(), _w, _h, prepared.blocks, prepared.offsets, prepared.levels, observer)) return false;
    prepared.width = _w; prepared.height = _h;
    _preparedBc3 = std::move(prepared);
    return true;
}

bool TextureSourceDDS::TakeCompositeBc3(const char* sourceName, int width, int height, Bc3MipChain& out)
{
    if (!sourceName || std::strcmp(sourceName, static_cast<const char*>(_name)) != 0 ||
        width != _preparedBc3.width || height != _preparedBc3.height ||
        _preparedBc3.blocks.empty() || _preparedBc3.levels <= 0) return false;
    out = std::move(_preparedBc3);
    _preparedBc3 = {};
    return true;
}

class TextureSourceDDSLinearTint final : public ITextureSource
{
    const TextureSourceDDS& _base;
    float _rgb[3];
    int _count;
    PackedColor _average;
    bool _alpha = false;

    bool Decode(const DDSMipLevel& source, TextureSourceDDS& output) const
    {
        DDSMipLevel decoded;
        decoded.width = source.width;
        decoded.height = source.height;
        decoded.data.resize(static_cast<size_t>(source.width) * source.height * 4);
        if (!ConvertPixels(source.data.data(), decoded.data.data(), source.width, source.height,
                           PixelFormat::BC7, PixelFormat::RGBA8888))
            return false;
        const bool preserveRoughnessAlpha =
            Enfusion::IsOriginalBcrRoughnessName(static_cast<const char*>(_base._name));
        for (size_t i = 0; i < decoded.data.size(); i += 4)
        {
            std::swap(decoded.data[i], decoded.data[i + 2]);
            // The lazy view also accepts legacy *_BCR.dds. Preserve data alpha
            // only for original Enfusion EDDS; legacy DDS keeps its former
            // opaque-alpha behavior until its channel semantics are proven.
            if (!preserveRoughnessAlpha)
                decoded.data[i + 3] = 255;
        }
        output._levels.push_back(std::move(decoded));
        ApplyEnfusionLayerTintWithMode(output._levels, _rgb, true);
        output._format = PixelFormat::ARGB8888;
        output._pacFormat = PacARGB8888;
        output._mipmaps = 1;
        return true;
    }

  public:
    TextureSourceDDSLinearTint(const TextureSourceDDS& base, const float rgb[3], int count)
        : _base(base), _rgb{rgb[0], rgb[1], rgb[2]}, _count(count) {}
    bool Bind(PacLevelMem* mips)
    {
        TextureSourceDDS smallest;
        const auto& averageLevel = _base._tintAverageLevel.data.empty() ?
            _base._levels.back() : _base._tintAverageLevel;
        if (!Decode(averageLevel, smallest)) return false;
        _average = AverageOfSmallest(smallest._levels, PixelFormat::ARGB8888);
        for (int i = 0; i < _count; ++i)
        {
            mips[i]._w = static_cast<short>(_base._levels[i].width);
            mips[i]._h = static_cast<short>(_base._levels[i].height);
            mips[i]._sFormat = PacARGB8888;
        }
        return true;
    }
    bool Init(const char*, PacLevelMem*, int) override { return false; }
    int GetMipmapCount() const override { return _count; }
    PacFormat GetFormat() const override { return PacARGB8888; }
    PackedColor GetAverageColor() const override { return _average; }
    bool IsAlpha() const override { return _alpha; }
    bool IsTransparent() const override { return false; }
    void ForceAlpha() override { _alpha = true; }
    bool GetMipmapData(void* mem, const PacLevelMem& mip, int level) const override
    {
        if (!mem || level < 0 || level >= _count ||
            mip._w != _base._levels[level].width || mip._h != _base._levels[level].height)
            return false;
        TextureSourceDDS decoded;
        return Decode(_base._levels[level], decoded) && decoded.GetMipmapData(mem, mip, 0);
    }
};

std::unique_ptr<ITextureSource> TextureSourceDDS::CreateLinearTintView(const float rgb[3], PacLevelMem* mips,
                                                                     int maxMips) const
{
    const std::string name = LowerCopy(static_cast<const char*>(_name));
    if (!rgb || !mips || maxMips <= 0 || _mipmaps <= 0 ||
        _format != PixelFormat::BC7 || _hasAlpha || _levels.size() != static_cast<size_t>(_mipmaps) ||
        !(name.ends_with("_bcr.edds") || name.ends_with("_bcr.dds")))
        return nullptr;
    for (int c = 0; c < 3; ++c)
        if (!std::isfinite(rgb[c]) || rgb[c] < 0 || rgb[c] > 1) return nullptr;
    auto view = std::make_unique<TextureSourceDDSLinearTint>(*this, rgb, std::min(_mipmaps, maxMips));
    if (!view->Bind(mips)) return nullptr;
    return view;
}

bool TextureSourceDDS::BindPreparedMips(PacLevelMem* mips, int maxMips)
{
    if (!mips || maxMips <= 0 || _mipmaps <= 0 || _levels.size() != static_cast<size_t>(_mipmaps))
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
    {
        if (_format == PixelFormat::BC7 && _tintAverageLevel.data.empty())
            _tintAverageLevel = std::move(_levels.back());
        _levels.resize(static_cast<size_t>(count));
    }
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
    // RFG-070: the tint wrapper decides nothing about whether the file exists, so it comes
    // off before any of the questions below are asked. Without this a tinted name reaches
    // QIFStreamB::FileExist verbatim, misses, and the section falls back to white -- the
    // exact defect the tint was added to fix, arriving through the factory instead.
    name = Enfusion::SkipLayerTint(name);
    float solid[3];
    if (SplitEnfusionColourName(name, solid))
        return true;
    std::string colour, coverage;
    if (SplitEnfusionAlphaName(name, colour, coverage))
        name = colour.c_str();
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
    if (render::PreparedTextureStore::NativeDdsEnabled())
    {
        auto prepared = render::PreparedTextureStore::Instance().TakeDdsPrepared(
            render::PreparedTextureStore::Key(name), CaptureDdsPreparationOptions());
        if (prepared && prepared->BindPreparedMips(mips, maxMips))
        {
            // The store accepted its normalized key and preparation options.
            // Adopt the consuming texture's exact spelling for diagnostics and
            // the one-shot sidecar claim; no composition/header data changes.
            prepared->_name = name;
            static const bool verify = [] {
                const char* value = std::getenv("WGR_NATIVE_DDS_PREPARE_VERIFY");
                return value && std::strcmp(value, "1") == 0;
            }();
            if (verify)
            {
                auto reference = std::make_unique<TextureSourceDDS>();
                const bool valid = reference->Init(name, mips, maxMips);
                bool same = valid && reference->_format == prepared->_format &&
                    reference->_pacFormat == prepared->_pacFormat && reference->_mipmaps == prepared->_mipmaps &&
                    reference->_w == prepared->_w && reference->_h == prepared->_h &&
                    reference->_hasAlpha == prepared->_hasAlpha && reference->_isTransparent == prepared->_isTransparent &&
                    reference->_avgColor == prepared->_avgColor && reference->_levels.size() == prepared->_levels.size();
                if (same) for (size_t i = 0; i < reference->_levels.size(); ++i)
                    same = same && reference->_levels[i].width == prepared->_levels[i].width &&
                        reference->_levels[i].height == prepared->_levels[i].height &&
                        reference->_levels[i].data == prepared->_levels[i].data;
                LOG_INFO(Graphics, "DDS prepared verification: {} name='{}'", same ? "identical" : "MISMATCH", name);
                if (!same) return valid ? reference.release() : nullptr;
            }
            if (DdsPreparationTrace::Enabled())
                LOG_INFO(Graphics, "DDS prepared take: bytes={} name='{}'", prepared->PreparedByteSize(), name);
            return prepared.release();
        }
    }
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
