// SPDX-License-Identifier: GPL-3.0-or-later
//
// RFG-065 -- the ground material of a NATIVELY loaded Enfusion world, read from the
// palette entry's own `.emat`.
//
// WHAT WAS WRONG. `LandLoadEnfusion.cpp` binds ONE texture per palette entry -- the
// `.emat`'s `BCRMap` and nothing else -- and hands it to the renderer as a legacy
// `Landscape::_texture` layer with `offsetUV = true`. The legacy branch in
// terrain.wgsl maps one period of that image onto each land cell, and a native
// Everon land cell is 12.5 m (12,800 m over a 1024-cell land grid; RFG-009 took it
// there from 256 cells / 50 m, and the palette comment beside it still says 50 --
// it is stale, and this file uses the measured 12.5). The corpus says the surfaces
// were authored for 2..8 m:
//
//     ScaleUV over the 51 shipped Terrains/Common/Surfaces/*.emat
//       2 -> 4 files   3 -> 4   3.9 -> 2   3.99 -> 3   4 -> 20   4.01 -> 1
//       6 -> 1   8 -> 1   (36 declare it; 15 do not)
//
// So the ground has been drawn at 12.5 / ScaleUV times the authored period: 6.25x
// too coarse at ScaleUV 2, 3.1x at the median 4, 1.6x at 8. Every surface that names
// a scale is stretched, and it also puts the texture's period exactly on the land
// grid -- one cell IS one texture -- which is a lattice the authored tiling does not
// have. That is the "flatter and more uniform" the ground reads as.
//
// WHAT THIS BINDS, in the order of measured value:
//
//   ScaleUV        36 of 51.  World-space UV at 1/ScaleUV repeats per metre. Also
//                  removes the per-cell jitter warp from the ground, because a
//                  world-space UV is continuous across a cell border by construction.
//   BCRMiddleMap   42 of 51 declare one; MiddleBCRBlend is non-zero on 25 of the 37
//                  that declare a blend (0 on 12). Crossfaded in over
//                  [DetailMaxDistance - DetailBlendDistance, DetailMaxDistance].
//                  At the authored 4 m tiling this is not decoration: it is what
//                  stops a 4 m period becoming a lattice in the middle distance.
//   MiddleColor    18 of 51 after inheritance; 6 of Everon's 22 palette entries, 5
//                  of which bind a middle map (ForestDeciduous_02 authors
//                  MiddleBCRBlend 0). RFG-073. A LINEAR multiplier on the middle
//                  texel -- the rule RFG-071 measured for `Color_N` against
//                  Enfusion's own bakes -- and on this corpus the pairing is the
//                  proof: all 6 Everon carriers use `Dirt_01_Middle_BCR`, a
//                  brightness tile measured at 216/216/216 (sd 12.6, the three
//                  channels identical), while the middle maps of the other 16
//                  entries are already coloured (Grass_01_Middle 62/71/45, SeaBed
//                  51/45/37) and name no MiddleColor. Multiplied in linear, 216 x
//                  Dirt_01's (0.175, 0.137, 0.095) is 97/86/72 beside Dirt_01_BCR's
//                  own measured 88/80/69; Dirt_02 predicts 97/82/62 against 95/82/67;
//                  ForestDeciduous_01 101/80/68 against 97/80/64; Dirt_03 (190/185/
//                  181 x 0.262/0.23/0.192) 103/94/84 against 102/96/89. Multiplied
//                  in gamma the same tile is 38/30/21. Left out, as it was, the
//                  coniferous floor faded from brown (103/86/65) to the untinted
//                  216 grey past DetailMaxDistance: the white forest floor at 50 m.
//                  Applied in the shader, not baked into the tile, because the tile
//                  is SHARED: that one image serves 17 materials under 12 distinct
//                  MiddleColors (Everon: 6 under 4), and a bake would need a copy
//                  per tint where the cache holds one.
//
// WHAT THIS DELIBERATELY DOES NOT BIND, with the measurement that decided it:
//
//   Color          21 of 51 declare one on the DETAIL level (Everon: 11 of 22),
//                  0.15..1.0, mostly a grey 0.58..0.91. Whether it multiplies the
//                  detail map alone, the whole material, or nothing cannot be read
//                  off a bake the way MiddleColor can, but one continuity CAN be
//                  measured: the middle map (MiddleColor applied) against the
//                  detail it takes over from at DetailMaxDistance, over the 19
//                  materials with a Color and a non-zero middle blend. Mean |log|
//                  luminance ratio: Color unused 0.221, applied to both 0.219,
//                  applied to the detail alone 0.367 (Everon's 7: 0.136 / 0.136 /
//                  0.209). Concrete_01 is the worked case: 119 untinted against its
//                  middle's 118; tinted by its Color 0.592 it would be 93 handing
//                  over to 118. "Detail alone" is rejected; the other two readings
//                  are indistinguishable on this data, so it stays unbound.
//   Diffuse        14 of 51 declare it and every one is `1 1 1 1`. Nothing to bind.
//
//   NHOMap normals are now bound: September 10's native texture tests established
//   RG normal packing. Height and occlusion remain unused, not albedo or coverage.
//   SatMapBlend    37 declare it and 19 of those declare ZERO. The other 18 need
//                  Everon's 2,500 `worlds/Eden/Eden/.Data/Eden_*_supertexture.edds`
//                  paged in per tile -- the same machinery the Arma satellite path
//                  has and the native loader has not built. That is a project, not a
//                  binding, and it is worth LESS than the two above, not more.
//   ParallaxScale  reads the detail map's height, which lives in the NHO. Blocked on
//                  the same measurement.
//
// The textures are built HERE rather than reused from `Landscape::_texture` for one
// reason: `CreateDynamic` uploads a single mip level. That is harmless while an image
// is stretched over a 12.5 m cell (it is always magnified), and it is fatal at 4 m and
// below -- a
// minified texture with no mip chain is aliasing noise. These are created with
// WGR_TEXTURE_GEN_MIPS.

#include "TerrainWgpu.hpp"

#include "EngineWgpu.hpp"
#include "TextureBankWgpu.hpp"
#include "TextureWgpu.hpp"

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
namespace
{
using Poseidon::Asset::Formats::Enfusion::EnfusionMount;
namespace Mat = Poseidon::Asset::Material;

//! The detail map is what the player stands on; the middle map is a 20..150 m tile
//! seen from a distance and does not need the same budget.
constexpr int kDetailMaxEdge = 512;
constexpr int kMiddleMaxEdge = 256;

//! One `.edds` from the archives, decoded to RGBA at or below `maxEdge` and uploaded
//! with a generated mip chain. Deduplicated by path by the caller.
Ref<Texture> LoadMippedGroundTexture(const EnfusionMount& mount, const std::string& path, int maxEdge)
{
    std::vector<uint8_t> bytes;
    if (!mount.Read(path, bytes) || !IsEddsBuffer(bytes.data(), bytes.size()))
        return {};
    const EddsImage image = ReadEddsBuffer(bytes.data(), bytes.size());
    if (!image.valid() || image.mipmaps.empty())
        return {};
    auto* bank = dynamic_cast<TextureBankWgpu*>(GEngine != nullptr ? GEngine->TextBank() : nullptr);
    if (bank == nullptr)
        return {};
    // RFG-110: the authored maps are 2048^2 BC7 (detail BCR + NHO, 12 mips) and 1024^2
    // (middle), and they went in decoded to RGBA and shrunk to 512 / 256 -- 16x fewer texels
    // than Bohemia authored, which is the blur on the ground at your feet. Their own blocks go
    // up untouched instead: no decode, no re-encode, a quarter of the RGBA bytes per texel.
    // BC7 is created Unorm like the RGBA path (an sRGB alias exists for colour consumers that
    // ask), so the shader reads exactly the same values. `nativeMaxEdge` caps the top level
    // (WGR_ENFUSION_GROUND_MAX_EDGE); WGR_ENFUSION_GROUND_NATIVE=0 restores the RGBA path.
    static const bool native = []
    {
        const char* env = std::getenv("WGR_ENFUSION_GROUND_NATIVE");
        return env == nullptr || env[0] != '0';
    }();
    static const int nativeCap = []
    {
        const char* env = std::getenv("WGR_ENFUSION_GROUND_MAX_EDGE");
        const int v = env ? std::atoi(env) : 0;
        return v >= 64 ? v : 4096;
    }();
    if (native && image.format == PixelFormat::BC7)
    {
        const int cap = std::min(nativeCap, maxEdge * 4);
        size_t first = 0;
        while (first + 1 < image.mipmaps.size() &&
               (image.mipmaps[first].width > cap || image.mipmaps[first].height > cap))
            ++first;
        std::vector<uint8_t> chain;
        uint32_t levels = 0;
        bool ok = true;
        for (size_t i = first; i < image.mipmaps.size(); ++i)
        {
            const auto& m = image.mipmaps[i];
            const size_t blocks = static_cast<size_t>(std::max(1, (m.width + 3) / 4)) *
                                  static_cast<size_t>(std::max(1, (m.height + 3) / 4));
            if (m.data.size() < blocks * 16)
            {
                ok = levels > 0; // a short tail level ends the chain; a short top level fails it
                break;
            }
            chain.insert(chain.end(), m.data.begin(), m.data.begin() + static_cast<ptrdiff_t>(blocks * 16));
            ++levels;
        }
        const auto& top = image.mipmaps[first];
        if (ok && levels > 0 && chain.size() <= 0xffffffffull)
        {
            Ref<Texture> texture = bank->CreateCompressed(top.width, top.height, WGR_TEXTURE_BC7, levels, chain.data(),
                                                          static_cast<uint32_t>(chain.size()),
                                                          static_cast<int>(AlphaStats::Opaque));
            if (texture)
                return texture;
        }
    }
    size_t level = 0;
    for (size_t i = 0; i < image.mipmaps.size(); ++i)
    {
        level = i;
        if (image.mipmaps[i].width <= maxEdge && image.mipmaps[i].height <= maxEdge)
            break;
    }
    const auto& mip = image.mipmaps[level];
    if (mip.width <= 0 || mip.height <= 0 || mip.data.empty())
        return {};
    std::vector<uint8_t> rgba(static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height) * 4);
    if (!ConvertPixels(mip.data.data(), rgba.data(), mip.width, mip.height, image.format, PixelFormat::RGBA8888))
        return {};
    if (image.format == PixelFormat::ARGB8888)
    {
        // The reader hands ARGB8888 back as B,G,R,A; the swap is the caller's, the same
        // way Image::FromFile does it. Getting this wrong is a blue island.
        for (size_t i = 0; i + 2 < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
    }
    // `_BCR` alpha is ROUGHNESS, not coverage (measured, see the Reforger texture-chain
    // notes). The ground shader samples `.rgb` only, but the alpha class decides whether
    // a texture is treated as a cutout elsewhere, so it is pinned opaque rather than left
    // to a pixel scan that would read the roughness map as holes.
    Ref<Texture> texture = bank->CreateDynamic(mip.width, mip.height, rgba.data(),
                                               static_cast<uint32_t>(rgba.size()), /*mipmap=*/true,
                                               static_cast<int>(AlphaStats::Opaque));
    return texture;
}

//! A/B for the MiddleColor tint: `POSEIDON_ENFUSION_MIDDLE_COLOR=0` leaves every middle
//! map untinted (the pre-RFG-073 picture). Load-time, like the layer-tint switch in
//! EnfusionTextureName.hpp, because the value rides the material table that is built
//! once per world; the dev panel's "Enfusion ground material" switch covers the whole
//! branch, tint included, live.
bool MiddleColorEnabled()
{
    const char* env = std::getenv("POSEIDON_ENFUSION_MIDDLE_COLOR");
    return env == nullptr || env[0] != '0';
}

} // namespace

// Reads every palette entry's `.emat` once per world and fills `_enfusionGround`.
// Returns false -- leaving the table empty and every consumer inert -- on any world
// that is not a natively loaded Reforger one.
bool TerrainWgpu::BuildEnfusionGroundMaterials(const Landscape& land)
{
    const int layerCount = land.GetNTextures();
    // The SAME test TerrainWgpu.cpp uses for the clutter path, and deliberately not
    // `_hasEnfusionSurfaces`: that flag is set in UploadGeography, which runs AFTER
    // UploadGroundTextures, so on the first upload of a world it is still false.
    // A palette whose surface count does not match its texture count is not a native
    // world; it is a converted one whose entries happen to carry a name.
    if (layerCount <= 0 || land.GetEnfusionSurfaceCount() != layerCount)
    {
        _enfusionGround.clear();
        _enfusionGroundTextures.clear();
        _enfusionGroundWorld.clear();
        ApplyEnfusionAmbientOcclusion(false);
        return false;
    }
    // Keyed on the palette itself rather than on the map name, for the same reason:
    // the name is not yet assigned when this first runs. Size plus entry 0 separates
    // every world in the corpus and costs one string compare per re-upload.
    std::string key = std::to_string(layerCount) + "|" + land.GetEnfusionSurface(0);
    if (static_cast<int>(_enfusionGround.size()) == layerCount && _enfusionGroundWorld == key)
        return true;

    _enfusionGround.assign(static_cast<size_t>(layerCount), EnfusionGroundMaterial{});
    _enfusionGroundTextures.clear();
    _enfusionGroundWorld = std::move(key);

    const EnfusionMount& mount = EnfusionMount::Instance();
    std::unordered_map<std::string, Ref<Texture>> cache;
    const auto load = [&](const std::string& path, int maxEdge) -> Texture*
    {
        if (path.empty())
            return nullptr;
        const auto found = cache.find(path);
        if (found != cache.end())
            return found->second;
        Ref<Texture> texture = LoadMippedGroundTexture(mount, path, maxEdge);
        cache.emplace(path, texture);
        if (texture)
            _enfusionGroundTextures.push_back(texture);
        return texture;
    };

    // Counters with a denominator. Every one of these is "of `layerCount` palette
    // entries", so a number that does not move after a change says the change did
    // not reach the data, which no per-surface spot check can say.
    int emats = 0, bcr = 0, nho = 0, middle = 0, scaled = 0, satmap = 0, bound = 0, middleBound = 0;
    int middleColour = 0, middleColourApplied = 0, normalBound = 0;
    const bool tintOn = MiddleColorEnabled();
    const char* normalEnv = std::getenv("WGR_ENFUSION_GROUND_NORMALS");
    const bool normalsOn = normalEnv == nullptr || normalEnv[0] != '0';
    const char* puddleFixtureEnv = std::getenv("WGR_TERRAIN_PUDDLE_FIXTURE");
    const bool puddleFixtureLog = puddleFixtureEnv != nullptr && puddleFixtureEnv[0] == '1';
    for (int i = 0; i < layerCount; ++i)
    {
        const std::string ematPath = land.GetEnfusionSurface(i);
        if (ematPath.empty())
            continue;
        std::vector<uint8_t> bytes;
        if (!mount.Read(ematPath, bytes))
            continue;
        Mat::EmatMaterial material =
            Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!material.valid())
            continue;
        // The `_aut` (autumn) variants are 176..589-byte overlays on a parent; without
        // this they resolve no BCRMap at all and would count as a failure.
        Mat::ResolveEmatInheritance(material,
                                    [&mount](const std::string& parentPath, std::string& text)
                                    {
                                        std::vector<uint8_t> parentBytes;
                                        if (!mount.Read(parentPath, parentBytes))
                                            return false;
                                        text.assign(reinterpret_cast<const char*>(parentBytes.data()),
                                                    parentBytes.size());
                                        return true;
                                    });
        ++emats;

        EnfusionGroundMaterial& out = _enfusionGround[static_cast<size_t>(i)];
        std::string colour = material.TextureOf("BCRMap");
        if (colour.empty())
            colour = material.TextureOf("AlbedoMap");
        if (!colour.empty())
            ++bcr;
        const std::string normalPath = material.TextureOf("NHOMap");
        if (!normalPath.empty())
            ++nho;
        const std::string middlePath = material.TextureOf("BCRMiddleMap");
        if (!middlePath.empty())
            ++middle;

        float scaleUv = 0.0f;
        if (material.FloatOf("ScaleUV", scaleUv) && scaleUv > 0.0f)
            ++scaled;
        float satBlend = 0.0f;
        if (material.FloatOf("SatMapBlend", satBlend) && satBlend > 0.0f)
            ++satmap;
        _enfusionGround[static_cast<size_t>(i)].satBlend = std::clamp(satBlend, 0.0f, 1.0f);

        Texture* detail = load(colour, kDetailMaxEdge);
        const bool semanticMud = TerrainPuddles::EligibleNativeSurface(ematPath);
        if (puddleFixtureLog)
            LOG_INFO(Graphics, "Wgpu terrain puddle native eligibility: material={} semantic={} BCR={} eligible={}",
                     i, ematPath, colour, semanticMud && detail != nullptr);
        if (detail == nullptr)
            continue;
        out.detail = detail;
        out.puddleEligible = semanticMud;
        // No ScaleUV means the material never named one, and the only defensible
        // reading of that is the corpus median rather than the 12.5 m cell: 36 of the 51
        // shipped surfaces declare a scale and 20 of those declare exactly 4.
        out.detailScale = 1.0f / (scaleUv > 0.0f ? scaleUv : 4.0f);
        if (normalsOn && !normalPath.empty())
        {
            out.normal = load(normalPath, kDetailMaxEdge);
            if (out.normal != nullptr)
                ++normalBound;
        }
        // RFG-110: the NHO's blue is HEIGHT (Bohemia's texture table), which ParallaxScale
        // displaces by. WGR_ENFUSION_PARALLAX=0 leaves it off.
        {
            static const bool parallaxOn = []
            {
                const char* env = std::getenv("WGR_ENFUSION_PARALLAX");
                return env == nullptr || env[0] != '0';
            }();
            float parallaxScale = 0.0f;
            if (parallaxOn && out.normal != nullptr && material.FloatOf("ParallaxScale", parallaxScale))
                out.parallax = std::clamp(parallaxScale, 0.0f, 0.2f);
        }
        ++bound;

        float middleScale = 0.0f;
        material.FloatOf("MiddleScaleUV", middleScale);
        float middleBlend = 0.0f;
        material.FloatOf("MiddleBCRBlend", middleBlend);
        // MiddleBCRBlend runs to 2 in the corpus, so it is a strength and not a lerp
        // weight; clamped to 1 here because the only thing this shader can do with it
        // is crossfade. Recorded as an interpretation, not a measurement.
        out.middleBlend = std::clamp(middleBlend, 0.0f, 1.0f);
        // Counted whether or not the middle map binds, so the number is a property of
        // the palette and not of what happened to load.
        float middleColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const bool hasMiddleColor = material.Vec4Of("MiddleColor", middleColor);
        if (hasMiddleColor)
            ++middleColour;
        if (out.middleBlend > 0.0f)
        {
            if (Texture* mid = load(middlePath, kMiddleMaxEdge); mid != nullptr)
            {
                out.middle = mid;
                out.middleScale = 1.0f / (middleScale > 0.0f ? middleScale : 50.0f);
                ++middleBound;
                if (hasMiddleColor && tintOn)
                {
                    // Multiplier, so clamped to the unit range rather than normalised:
                    // a value above 1 would be a brightening no bake in the corpus
                    // shows, and the corpus maximum is 0.644 (SulfurStream).
                    for (int c = 0; c < 3; ++c)
                        out.middleColor[c] = std::clamp(middleColor[c], 0.0f, 1.0f);
                    ++middleColourApplied;
                }
            }
            else
            {
                out.middleBlend = 0.0f;
            }
        }

        float detailMax = 0.0f;
        material.FloatOf("DetailMaxDistance", detailMax);
        float detailFade = 0.0f;
        material.FloatOf("DetailBlendDistance", detailFade);
        // 150 is the corpus mode (14 of the 30 that declare a distance); a surface that
        // names none keeps its detail to that range rather than to zero.
        out.detailMax = detailMax > 0.0f ? detailMax : 150.0f;
        out.detailFade = std::clamp(detailFade > 0.0f ? detailFade : out.detailMax, 0.0f, out.detailMax);
    }

    _enfusionSatmap = BuildEnfusionSatmap(land);
    ApplyEnfusionAmbientOcclusion(true);
    LOG_INFO(Graphics,
             "Wgpu Enfusion ground: {} of {} palette entries read an .emat; {} BCR, {} NHO, {} middle BCR, "
             "{} ScaleUV, {} SatMapBlend>0; "
             "bound {} detail + {} middle over {} distinct images; {} MiddleColor declared, {} MiddleColor applied{}",
             emats, layerCount, bcr, nho, middle, scaled, satmap, bound, middleBound, _enfusionGroundTextures.size(),
             middleColour, middleColourApplied, tintOn ? "" : " (POSEIDON_ENFUSION_MIDDLE_COLOR=0)");
    LOG_INFO(Graphics, "Wgpu Enfusion ground normals: {} of {} NHO sources bound (RG, native BC7 up to 2048 px unless WGR_ENFUSION_GROUND_NATIVE=0; enabled={})",
             normalBound, nho, normalsOn);
    return bound > 0;
}

// RFG-PBR: a native world takes the screen-space AO its own post-process chain authors. Everon's
// GenericWorldPP_Default names an HBAO effect whose quality tiers (Common/Postprocess/HBAO_*.emat)
// all set RadiusMeters 8.5 and Intensity 2 -- against our 2 m / 1 -- and vary only the sample count.
// The parameter set (SmallScaleAO, LargeScaleAO, a power) is HBAO+'s, whose power is an exponent on
// visibility: the same meaning as AoSettings::strength. At 2 m the AO cannot reach between trees,
// which is most of why a measured Reforger forest bottoms out at sRGB 18 and ours at 70.
// WGR_ENFUSION_AO=0 keeps the engine defaults. Restored when a non-native world loads.
void TerrainWgpu::ApplyEnfusionAmbientOcclusion(bool native)
{
    static const bool enabled = [] {
        const char* v = std::getenv("WGR_ENFUSION_AO");
        return !(v && v[0] == '0');
    }();
    if (!native || !enabled)
    {
        if (_enfusionAoApplied)
        {
            _engine.SetAoSettings(_enfusionAoSaved);
            _enfusionAoApplied = false;
            LOG_INFO(Graphics, "Wgpu Enfusion AO: engine defaults restored");
        }
        return;
    }
    float radius = 0.0f, intensity = 0.0f;
    std::vector<uint8_t> bytes;
    if (EnfusionMount::Instance().Read("Common/Postprocess/HBAO_High.emat", bytes))
    {
        const Mat::EmatMaterial hbao =
            Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        hbao.FloatOf("RadiusMeters", radius);
        hbao.FloatOf("Intensity", intensity);
    }
    if (!(radius > 0.0f) || !(intensity > 0.0f))
    {
        LOG_INFO(Graphics, "Wgpu Enfusion AO: HBAO_High.emat not readable, engine AO kept");
        return;
    }
    if (!_enfusionAoApplied)
        _enfusionAoSaved = _engine.GetAoSettings();
    Engine::AoSettings ao = _enfusionAoSaved;
    ao.radius = radius;
    // RFG-133: the radius is Reforger's, the exponent is not. Workbench's own HBAOEffect class
    // reports Intensity defaulting to 10 (range 0.1-100), so Everon's 2 is a LOW setting, not
    // the power-of-two it was read as here -- which squared GTAO's visibility and turned every
    // room seen through a window from outside black (q11: room/facade 0.06, Reforger 0.29).
    // Exponent 1 (GTAO's physical result) measures 0.21 there, and Morton's conifers and meadow
    // do not move (48 vs 49, 143 vs 143). `intensity` stays read so the log says what was there.
    ao.strength = 1.0f;
    // Calibration levers: WGR_ENFUSION_AO_RADIUS / WGR_ENFUSION_AO_STRENGTH.
    if (const char* v = std::getenv("WGR_ENFUSION_AO_RADIUS"); v && *v)
        ao.radius = std::max(0.1f, static_cast<float>(std::atof(v)));
    if (const char* v = std::getenv("WGR_ENFUSION_AO_STRENGTH"); v && *v)
        ao.strength = std::max(0.0f, static_cast<float>(std::atof(v)));
    _engine.SetAoSettings(ao);
    _enfusionAoApplied = true;
    LOG_INFO(Graphics, "Wgpu Enfusion AO: radius {} m, exponent {} (HBAO_High.emat radius {} m, Intensity {})",
             ao.radius, ao.strength, radius, intensity);
}

// RFG-PBR: the world's supertexture -- one `<name>_<index>_supertexture.edds` per terrain tile
// (Everon: 2,500 pages, 256 x 256 BC7 over 256 m, so 1 m a texel), the low-frequency colour of
// the ground as seen from a distance. Measured on Everon tile 1275: sRGB mean 65/66/44, darker
// and more saturated than any surface's own detail map, with ALPHA 0 under the road ribbons
// (the road meshes draw there). Composited here at one mip level per page -- 64 x 64, 4 m a
// texel, 3200 x 3200 on Everon -- into a single image the terrain shader samples by world
// position. Alpha is kept: the shader reads it as "the satellite has no say here".
Ref<Texture> TerrainWgpu::BuildEnfusionSatmap(const Landscape& land)
{
    _enfusionSatmapExtent = 0.0f;
    const char* env = std::getenv("WGR_ENFUSION_SATMAP");
    if (env != nullptr && env[0] == '0')
        return {};
    const std::string& prefix = land.GetEnfusionTilePrefix();
    const int tiles = land.GetEnfusionTilesX();
    if (prefix.empty() || tiles <= 0)
        return {};
    constexpr int kPageEdge = 64;
    const int edge = tiles * kPageEdge;
    std::vector<uint8_t> rgba(static_cast<size_t>(edge) * edge * 4, 0);
    // RFG-105: the pages at their FULL 1 m, from their own BC7 blocks. The 4 m composite above
    // cannot carry the 5-10 m green/straw mottling Reforger's meadows show past DetailMaxDistance
    // (SatMapBlend 0.7 hands most of the colour to the satellite there); the pages can, and as
    // BC7 they go to the GPU without a decode: block rows are copied into one 12,800^2 BC7 chain
    // (mips 0..6, 256 down to 4 per page), ~218 MB. Any page that is not BC7 with those mips --
    // or a device that refuses the upload -- keeps the 4 m image. WGR_ENFUSION_SATMAP_FULL=0 off.
    constexpr int kFullPageEdge = 256, kFullMips = 7;
    const char* fullEnv = std::getenv("WGR_ENFUSION_SATMAP_FULL");
    const bool wantFull = !(fullEnv != nullptr && fullEnv[0] == '0');
    std::vector<std::vector<uint8_t>> fullLevels;
    std::atomic<bool> fullOk{wantFull};
    if (wantFull)
        for (int level = 0; level < kFullMips; ++level)
        {
            const size_t blocksPerRow = static_cast<size_t>(tiles) * ((kFullPageEdge >> level) / 4);
            fullLevels.emplace_back(blocksPerRow * blocksPerRow * 16, 0);
        }
    const EnfusionMount& mount = EnfusionMount::Instance();
    const auto start = std::chrono::steady_clock::now();
    int pages = 0, missing = 0, failed = 0;
    double sum[3] = {0.0, 0.0, 0.0};
    size_t opaque = 0;
    // Pages decode in parallel: each writes its own block of the composite, and the archive reader
    // is already safe across threads (the object stream's workers use it). 2,500 BC7 pages took
    // 2.7 s on one core.
    struct PageStats
    {
        int pages = 0, missing = 0, failed = 0;
        double sum[3] = {0.0, 0.0, 0.0};
        size_t opaque = 0;
    };
    const unsigned workers = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<PageStats> stats(workers);
    const auto decodeRange = [&](unsigned worker)
    {
        PageStats& st = stats[worker];
        std::vector<uint8_t> bytes, page;
        for (int index = static_cast<int>(worker); index < tiles * tiles; index += static_cast<int>(workers))
        {
            const std::string path = prefix + std::to_string(index) + "_supertexture.edds";
            if (!mount.Read(path, bytes) || !IsEddsBuffer(bytes.data(), bytes.size()))
            {
                ++st.missing;
                continue;
            }
            const EddsImage image = ReadEddsBuffer(bytes.data(), bytes.size());
            const DDSMipLevel* chosen = nullptr;
            for (const auto& mip : image.mipmaps)
                if (mip.width == kPageEdge && mip.height == kPageEdge && !mip.data.empty())
                {
                    chosen = &mip;
                    break;
                }
            if (!image.valid() || chosen == nullptr)
            {
                ++st.failed;
                continue;
            }
            if (fullOk.load(std::memory_order_relaxed))
            {
                bool copied = image.format == PixelFormat::BC7;
                for (int level = 0; copied && level < kFullMips; ++level)
                {
                    const int pageEdge = kFullPageEdge >> level;
                    const DDSMipLevel* mip = nullptr;
                    for (const auto& m : image.mipmaps)
                        if (m.width == pageEdge && m.height == pageEdge)
                            mip = &m;
                    const size_t pageBlocks = static_cast<size_t>(pageEdge / 4);
                    if (mip == nullptr || mip->data.size() < pageBlocks * pageBlocks * 16)
                    {
                        copied = false;
                        break;
                    }
                    const size_t bigBlocks = static_cast<size_t>(tiles) * pageBlocks;
                    const size_t ox = static_cast<size_t>(index % tiles) * pageBlocks;
                    const size_t oz = static_cast<size_t>(index / tiles) * pageBlocks;
                    for (size_t row = 0; row < pageBlocks; ++row)
                        std::memcpy(fullLevels[level].data() + ((oz + row) * bigBlocks + ox) * 16,
                                    mip->data.data() + row * pageBlocks * 16, pageBlocks * 16);
                }
                if (!copied)
                    fullOk.store(false, std::memory_order_relaxed);
            }
            page.assign(static_cast<size_t>(kPageEdge) * kPageEdge * 4, 0);
            if (!ConvertPixels(chosen->data.data(), page.data(), kPageEdge, kPageEdge, image.format,
                               PixelFormat::RGBA8888))
            {
                ++st.failed;
                continue;
            }
            if (image.format == PixelFormat::ARGB8888)
                for (size_t i = 0; i + 2 < page.size(); i += 4)
                    std::swap(page[i], page[i + 2]);
            // Row-major over the tile grid, rows running with world z -- the `.ttile` order.
            const int originX = (index % tiles) * kPageEdge;
            const int originZ = (index / tiles) * kPageEdge;
            for (int z = 0; z < kPageEdge; ++z)
            {
                std::copy_n(page.data() + static_cast<size_t>(z) * kPageEdge * 4, kPageEdge * 4,
                            rgba.data() + (static_cast<size_t>(originZ + z) * edge + originX) * 4);
                for (int x = 0; x < kPageEdge; ++x)
                {
                    const uint8_t* t = page.data() + (static_cast<size_t>(z) * kPageEdge + x) * 4;
                    if (t[3] > 127)
                    {
                        st.sum[0] += t[0];
                        st.sum[1] += t[1];
                        st.sum[2] += t[2];
                        ++st.opaque;
                    }
                }
            }
            ++st.pages;
        }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned w = 1; w < workers; ++w)
            pool.emplace_back(decodeRange, w);
        decodeRange(0);
        for (std::thread& t : pool)
            t.join();
    }
    for (const PageStats& st : stats)
    {
        pages += st.pages;
        missing += st.missing;
        failed += st.failed;
        opaque += st.opaque;
        for (int c = 0; c < 3; ++c)
            sum[c] += st.sum[c];
    }
    if (pages == 0)
    {
        LOG_INFO(Graphics, "Wgpu Enfusion satmap: no supertexture pages under '{}' ({} missing, {} failed)", prefix,
                 missing, failed);
        return {};
    }
    // Orientation proof, with a denominator: the satellite is transparent under the road ribbons,
    // and the loader marks road cells from the ribbons themselves. Rows running with world z
    // must put road cells on transparent texels far more often than the flipped reading does.
    {
        const int range = land.GetLandRange();
        size_t roads = 0, hitAsIs = 0, hitFlipped = 0;
        for (int z = 0; z < range; ++z)
            for (int x = 0; x < range; ++x)
            {
                if (!land.GetEnfusionRoadCell(z, x))
                    continue;
                ++roads;
                const int sx = std::min(edge - 1, static_cast<int>((x + 0.5) * edge / range));
                const int sz = std::min(edge - 1, static_cast<int>((z + 0.5) * edge / range));
                hitAsIs += rgba[(static_cast<size_t>(sz) * edge + sx) * 4 + 3] < 128 ? 1 : 0;
                hitFlipped += rgba[(static_cast<size_t>(edge - 1 - sz) * edge + sx) * 4 + 3] < 128 ? 1 : 0;
            }
        LOG_INFO(Graphics, "Wgpu Enfusion satmap orientation: of {} road cells, {:.1f}% sit on transparent texels "
                           "as composited, {:.1f}% with rows flipped",
                 roads, roads ? 100.0 * hitAsIs / roads : 0.0, roads ? 100.0 * hitFlipped / roads : 0.0);
    }
    auto* bank = dynamic_cast<TextureBankWgpu*>(GEngine != nullptr ? GEngine->TextBank() : nullptr);
    if (bank == nullptr)
        return {};
    // Alpha is the road mask here, not cutout coverage; pinned opaque for the same reason as
    // the BCR maps above (the alpha class decides cutout handling elsewhere).
    Ref<Texture> texture;
    int builtEdge = edge;
    if (fullOk.load() && pages == tiles * tiles)
    {
        std::vector<uint8_t> chain;
        size_t total = 0;
        for (const auto& level : fullLevels)
            total += level.size();
        chain.reserve(total);
        for (auto& level : fullLevels)
        {
            chain.insert(chain.end(), level.begin(), level.end());
            std::vector<uint8_t>().swap(level);
        }
        builtEdge = tiles * kFullPageEdge;
        if (chain.size() <= 0xffffffffull)
            texture = bank->CreateCompressed(builtEdge, builtEdge, WGR_TEXTURE_BC7, kFullMips, chain.data(),
                                             static_cast<uint32_t>(chain.size()), static_cast<int>(AlphaStats::Opaque));
        if (!texture)
            builtEdge = edge;
    }
    if (!texture)
        texture = bank->CreateDynamic(edge, edge, rgba.data(), static_cast<uint32_t>(rgba.size()),
                                      /*mipmap=*/true, static_cast<int>(AlphaStats::Opaque));
    if (texture)
        _enfusionSatmapExtent = land.GetLandGrid() * static_cast<float>(land.GetLandRange());
    const double n = opaque > 0 ? static_cast<double>(opaque) : 1.0;
    LOG_INFO(Graphics,
             "Wgpu Enfusion satmap: {} of {} supertexture pages ({} missing, {} failed) -> {}x{} at {:.2f} m, "
             "opaque mean sRGB {:.0f}/{:.0f}/{:.0f} over {:.1f}% of texels, {} ms",
             pages, tiles * tiles, missing, failed, builtEdge, builtEdge,
             _enfusionSatmapExtent / static_cast<float>(builtEdge),
             sum[0] / n, sum[1] / n, sum[2] / n, 100.0 * static_cast<double>(opaque) / (static_cast<double>(edge) * edge),
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    return texture;
}

} // namespace Poseidon
