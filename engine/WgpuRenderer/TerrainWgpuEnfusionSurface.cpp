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
    auto* bank = dynamic_cast<TextureBankWgpu*>(GEngine != nullptr ? GEngine->TextBank() : nullptr);
    if (bank == nullptr)
        return {};
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

    LOG_INFO(Graphics,
             "Wgpu Enfusion ground: {} of {} palette entries read an .emat; {} BCR, {} NHO, {} middle BCR, "
             "{} ScaleUV, {} SatMapBlend>0 (not bound: needs the supertexture pages); "
             "bound {} detail + {} middle over {} distinct images; {} MiddleColor declared, {} MiddleColor applied{}",
             emats, layerCount, bcr, nho, middle, scaled, satmap, bound, middleBound, _enfusionGroundTextures.size(),
             middleColour, middleColourApplied, tintOn ? "" : " (POSEIDON_ENFUSION_MIDDLE_COLOR=0)");
    LOG_INFO(Graphics, "Wgpu Enfusion ground normals: {} of {} NHO sources bound (RG, 512px max; enabled={})",
             normalBound, nho, normalsOn);
    return bound > 0;
}

} // namespace Poseidon
