// SPDX-License-Identifier: GPL-3.0-or-later
//
// RFG-005 — loading an Enfusion (Arma Reforger) world NATIVELY, straight out of the
// installed game's `.pak` archives, with no conversion step and no file written.
//
// Everything this needs was already in the engine library: `PakArchive` opens the
// container, `ReadTerrainDescriptor` and `ReadTerrainTile` decode the terrain. What
// was missing was a caller — every one of those readers was reachable only from the
// asset CLI, so the engine could describe a Reforger world in perfect detail and not
// draw a pixel of it. This file is the caller.
//
// WHAT IT DOES NOT DO, stated up front so the gap is not mistaken for a bug:
//   * no objects. Placements live in the `.ent` EBIN world and name PREFABS, which
//     resolve through a `.et` inheritance chain that today lives in the tool
//     (`XobCommand::ResolvePrefabModel`). Terrain first.
//   * no surface materials. The per-cell assignment is the `.ttile` TMAT chunk and
//     its palette the `.terr` MATS list; the decoder for the former is in the tool
//     as well. Until it moves, the ground draws in the renderer's fallback, which is
//     white — the milestone the earlier records named as the right first one.
//
// Resampling: Everon is 6401 x 6401 samples at 2 m. The engine's terrain grid is a
// power of two, so the Enfusion grid is point-sampled into it. That is a reduction
// and it is deliberate — 2048 x 2048 over 12.8 km is 6.25 m a cell, the same figure
// the converting exporter produces, so the two routes are comparable.

#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // RFG-097/099: the nativeStreaming lever
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PrefabResolve.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ResourceDatabase.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobConvert.hpp>
#include <Poseidon/World/Model/ModelCache.hpp> // RFG-097 external loader
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainMaterials.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Core/ProgressSystem.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp> // RFG-070 `enft|` layer tint
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <iterator>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
using Poseidon::Asset::Formats::Enfusion::EnfusionMount;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainDescriptor;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainTile;
using Poseidon::Asset::Formats::Enfusion::TerrainDescriptor;
using Poseidon::Asset::Formats::Enfusion::TerrainTile;
using Poseidon::Asset::Formats::Enfusion::FourCC;
using Poseidon::Asset::Formats::Enfusion::IffChunk;
using Poseidon::Asset::Formats::Enfusion::IffFile;
using Poseidon::Asset::Formats::Enfusion::ReadIff;
namespace Tmat = Poseidon::Asset::Formats::Enfusion::Tmat;

std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Which world to load out of the archives. `POSEIDON_REFORGER_WORLD` overrides it;
/// the default is Everon, whose internal name is Eden.
std::string selectedNativeWorld;
const char* WantedWorld() { return selectedNativeWorld.c_str(); }

// RFG-008: one palette entry's `.emat` resolved to an uploadable ground texture.
//
// The chain, and why each link is what it is:
//   * `.emat` is TEXT and ParseEmat takes bytes, so the archive's bytes go straight
//     in. The engine's own `.emat` loaders go through QIFStreamB, which is exactly
//     the layer that cannot see inside a `.pak`.
//   * 18% of Reforger materials inherit, so the parent chain is resolved first, with
//     the loader reading parents out of the mount as well.
//   * The colour key order BCRMap -> AlbedoMap -> BCR_1 is measured, not guessed.
//     The Arma-side history is why it matters: picking the colour layer by FILENAME
//     broke on three consecutive generations, twice drawing a whole island white.
//   * A small mip, not the top one. An Everon ground BCR is 2048^2 BC7 and this is a
//     layer that gets minified anyway, so mip N costs nothing and keeps 22 materials
//     off the memory budget.
//! RFG-066: WHICH of the four ways an Enfusion material can name an albedo answered.
//!
//! Not cosmetic. "The buildings are pale and grey" is a claim about a proportion, and
//! before this there was no denominator anywhere in the load -- only a per-material
//! warning capped at 25 lines, which can say "these failed" and can never say "and
//! these 5,000 did not". The three earlier sessions that guessed at this each guessed
//! at a bucket that turns out to be empty.
enum class ColourKind
{
    None,      //!< nothing bindable; the section draws in the renderer's fallback
    OwnMap,    //!< `BCRMap` / `AlbedoMap` / `GBufferAlbedo` -- the object's own albedo
    LayerTile, //!< `BCR_1..4` -- a SHARED library tile, and see the census note below
    Flat       //!< no map at all, only a constant `Color` (RFG-039's `enfc|`)
};

//! The measured key order, in one place because two callers need it: the terrain's
//! surface palette and the model converter's texture slot. Picking the colour layer
//! by FILENAME instead broke on three consecutive Arma generations and twice drew a
//! whole island white.
std::string ColourTextureOf(const Poseidon::Asset::Material::EmatMaterial& material, ColourKind* kind = nullptr)
{
    const auto answer = [kind](const std::string& path, ColourKind k)
    {
        if (kind != nullptr)
            *kind = path.empty() ? ColourKind::None : k;
        return path;
    };

    std::string path = material.TextureOf("BCRMap");
    if (path.empty())
        path = material.TextureOf("AlbedoMap");
    if (!path.empty())
        return answer(path, ColourKind::OwnMap);
    // RFG-039: a `MatPBRMulti` numbers its layers, and layer 1 is not always the one
    // carrying an albedo -- `BarrelMetal_01` declares `Color_1` and `NMO_1` with no
    // `BCR_1`, and its actual maps are `BCR_2`..`BCR_4`. Taking only `BCR_1` left the
    // barrel white while four albedos sat in the same file.
    //
    // RFG-066 checked whether this order still leaves anything behind, over all 5,294
    // `.emat` under `Assets/Structures/**` with inheritance resolved, and it does not:
    // 2,176 answer a plain `BCRMap`, 2,889 a `BCR_N`, 121 only a flat `Color`, and the
    // 108 that answer nothing are 30 `MatLightPortal` + 1 `MatLightSource` (not
    // surfaces), 27 `MatPBRDecal` (already hidden below) and 50 genuinely empty base
    // materials. NOTHING falls through to a named layer map -- `MudBCRMap` (1,875
    // files) and `DirtBCRMap` (1,552) never occur without a `BCR_N` beside them, and
    // corpus-wide exactly 2 of 10,313 materials would ever reach such a fallback. So
    // the "take the dominant layer instead of nothing" repair this was expected to
    // need would move zero materials, and is deliberately not written.
    for (int layer = 1; layer <= 4 && path.empty(); ++layer)
        path = material.TextureOf("BCR_" + std::to_string(layer));
    // Skipping a layer whose `Enabled_N` is 0, the way the converting exporter does
    // (XobCommand.cpp), was measured too: layer 1 has no `Enabled_1` by construction
    // and is present in every one of them, so "lowest present" and "lowest ENABLED"
    // name the same tile in 0 of 9,745 `Assets/**` materials. The divergence from the
    // exporter is real and it is inert.
    if (!path.empty())
        return answer(path, ColourKind::LayerTile);
    return answer(material.TextureOf("GBufferAlbedo"), ColourKind::OwnMap);
}

//! RFG-066: what the load resolved, by kind, over the DISTINCT materials it touched.
//!
//! `tintDiscarded` is the one that names the reported defect. A `BCR_N` is not the
//! object's albedo: it is a one-metre shared library tile out of `Assets/_SharedData`
//! -- 2,548 of the 2,889 structure materials that use one, served by just 436 distinct
//! tiles -- and what makes a given wall its own colour is the per-layer `Color_N`
//! beside it. 2,608 of those 2,889 (90.3%) author one, median 0.385 0.356 0.322, and
//! this path used to bind the raw tile and discard it: unrelated walls came out the
//! same pale grey because they were literally the same square metre of texture.
//!
//! RFG-070 closed that. The tint now travels to the texture source as an `enft|r,g,b|`
//! wrapper around the name and the tile is rescaled to have it as its mean, which is
//! MAT-054's rule from the converting exporter (XobCommand.cpp) rather than a second
//! one. So `tintApplied` is what the repair does and `tintDiscarded` is what is LEFT --
//! and it must be read as "still dropped", not "dropped by design". It stays non-zero
//! only for materials that declare a `Color_N` this cannot resolve to a usable mix, and
//! for every material when the dev panel's switch is off, which is the A/B.
struct ColourCensus
{
    size_t ownMap = 0;
    size_t layerTile = 0;
    size_t tintAuthored = 0;  //!< subset of layerTile: the material authored a `Color_N` at all
    size_t tintApplied = 0;   //!< subset of tintAuthored: RFG-070 rescaled the tile to it
    size_t tintDiscarded = 0; //!< subset of tintAuthored: it was authored and still dropped
    size_t tintMaskWeighted = 0; //!< subset of tintApplied: a `MaskMap` decided the layer mix
    size_t flat = 0;
    size_t none = 0;
    std::string firstTintDiscarded;
    std::string firstNone;

    size_t Total() const { return ownMap + layerTile + flat + none; }
};

ColourCensus GColourCensus;

//! RFG-039: `enfc|R,G,B` for a material whose colour is a CONSTANT, not a map.
//!
//! Reforger's `MatPBRBasic` frequently carries no albedo at all -- 71 sections in one
//! village -- only a `Color` property. A section with no texture draws white, so a
//! material saying "dark grey" produced a bright white crash barrier. The colour is
//! there; nothing was reading it.
std::string ColourConstantOf(const Poseidon::Asset::Material::EmatMaterial& material)
{
    const auto* property = material.Find("Color");
    if (property == nullptr || property->values.size() < 3)
        return {};
    for (int i = 0; i < 3; ++i)
        if (!property->values[static_cast<size_t>(i)].isNumber)
            return {};
    char text[96];
    std::snprintf(text, sizeof(text), "enfc|%.4f,%.4f,%.4f", property->values[0].number, property->values[1].number,
                  property->values[2].number);
    return text;
}

//! Reads one `.emat` out of the mount, parents resolved.
bool LoadEmatFromMount(const EnfusionMount& mount, const std::string& ematPath,
                       Poseidon::Asset::Material::EmatMaterial& out)
{
    namespace Mat = Poseidon::Asset::Material;
    std::vector<uint8_t> bytes;
    if (!mount.Read(ematPath, bytes))
        return false;
    out = Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!out.valid())
        return false;
    Mat::ResolveEmatInheritance(out,
                                [&mount](const std::string& parentPath, std::string& text)
                                {
                                    std::vector<uint8_t> parentBytes;
                                    if (!mount.Read(parentPath, parentBytes))
                                        return false;
                                    text.assign(reinterpret_cast<const char*>(parentBytes.data()),
                                                parentBytes.size());
                                    return true;
                                });
    return true;
}

//! One `.edds` out of the archives, decoded to RGBA at or below `maxEdge`.
bool DecodeEddsRgba(const EnfusionMount& mount, const std::string& path, int maxEdge, int& width, int& height,
                    std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> bytes;
    if (!mount.Read(path, bytes))
        return false;
    if (!Poseidon::IsEddsBuffer(bytes.data(), bytes.size()))
        return false;
    const Poseidon::EddsImage image = Poseidon::ReadEddsBuffer(bytes.data(), bytes.size());
    if (!image.valid() || image.mipmaps.empty())
        return false;
    size_t level = 0;
    for (size_t i = 0; i < image.mipmaps.size(); ++i)
    {
        level = i;
        if (image.mipmaps[i].width <= maxEdge && image.mipmaps[i].height <= maxEdge)
            break;
    }
    const auto& mip = image.mipmaps[level];
    if (mip.width <= 0 || mip.height <= 0 || mip.data.empty())
        return false;
    rgba.assign(static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height) * 4, 0);
    if (!Poseidon::ConvertPixels(mip.data.data(), rgba.data(), mip.width, mip.height, image.format,
                                 Poseidon::PixelFormat::RGBA8888))
        return false;
    if (image.format == Poseidon::PixelFormat::ARGB8888)
        for (size_t i = 0; i + 2 < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
    width = mip.width;
    height = mip.height;
    return true;
}

//! RFG-070: the colour a shared layer tile is supposed to be worn in.
//!
//! This is MAT-054's rule (XobCommand.cpp:1030-1200), read out of the converting exporter
//! rather than re-derived, because the two paths drawing the same building in two colours
//! is a worse outcome than either colour being slightly wrong.
//!
//! What the exporter established, and what is copied verbatim:
//!
//!   * the TILE is the base layer -- the lowest present index -- because the mask weights
//!     OVERLAY layers (mud, rust, weathering) over a base, and picking the heaviest-weighted
//!     layer's tile swaps a church's ceramic roof for painted metal plates.
//!   * the COLOUR is the `Color_N` of every present, enabled layer, weighted by the
//!     `MaskMap`'s channel means -- R, G, B weight layers 2, 3, 4 and layer 1 takes the
//!     remainder, measured as a partition of unity on WaterTower_02's global mask.
//!   * first-wins is NOT good enough and this is the model that proved it: WaterTower_01's
//!     `Color_2` is a near-black blue (0.031 0.133 0.191) and its concrete and steel are
//!     0.381 and 0.402 grey, so taking layer 2 alone paints the whole tower near-black.
//!
//! One deliberate divergence, and it is a cost divergence, not a rule divergence: the mask
//! is decoded at a SMALL mip (64 px) instead of full size. Only its per-channel mean is
//! wanted and a mip is a box average, so the number is the same one; what changes is that
//! a world load does not decode a few thousand 2k masks to get it.
struct LayerTint
{
    bool authored = false;     //!< a `Color_N` sits beside a `BCR_N` in this material
    bool resolved = false;     //!< subset of authored: `rgb` below is usable
    bool maskWeighted = false; //!< the mix came from a `MaskMap` and not from equal weights
    float rgb[3] = {0.0f, 0.0f, 0.0f};
};

LayerTint ResolveLayerTint(const EnfusionMount& mount, const Poseidon::Asset::Material::EmatMaterial& material)
{
    LayerTint out;

    // The mask's channel means. Layer 1 is the base and takes whatever the three overlay
    // channels leave; a mask that will not decode is not an error, it just means the
    // layers weigh the same -- still a guess, but an unbiased one, where file order is not.
    float weight[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // 1-based
    const std::string maskPath = material.TextureOf("MaskMap");
    if (!maskPath.empty())
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        if (DecodeEddsRgba(mount, maskPath, 64, width, height, rgba) && !rgba.empty())
        {
            const size_t texels = rgba.size() / 4;
            double covered = 0.0;
            for (int c = 0; c < 3; ++c)
            {
                double sum = 0.0;
                for (size_t t = 0; t < texels; ++t)
                    sum += rgba[t * 4 + static_cast<size_t>(c)];
                const double mean = sum / (255.0 * static_cast<double>(texels));
                weight[c + 2] = static_cast<float>(mean);
                covered += mean;
            }
            weight[1] = static_cast<float>(covered < 1.0 ? 1.0 - covered : 0.0);
            out.maskWeighted = true;
        }
    }

    float total = 0.0f;
    float blended[3] = {0.0f, 0.0f, 0.0f};
    for (int layer = 1; layer <= 4; ++layer)
    {
        const std::string index = std::to_string(layer);
        if (material.TextureOf("BCR_" + index).empty())
            continue;
        float enabled = 1.0f;
        material.FloatOf("Enabled_" + index, enabled);
        if (enabled == 0.0f)
            continue;
        float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (!material.Vec4Of("Color_" + index, colour))
            continue;
        const float w = out.maskWeighted ? weight[layer] : 1.0f;
        for (int c = 0; c < 3; ++c)
            blended[c] += colour[c] * w;
        total += w;
    }
    // `authored` and `resolved` are two different questions and the census needs both.
    // A material can declare a `Color_N` that this cannot use -- every weight landing on
    // a layer that has no colour, or a mask saying the base layer is the whole surface
    // while the base declares no `Color_1`. Those are the ones that must still be counted
    // as a dropped tint, because they are exactly the case where the wall keeps the
    // shared tile's colour and the log would otherwise claim the gap was closed.
    for (int layer = 1; layer <= 4 && !out.authored; ++layer)
    {
        float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (!material.TextureOf("BCR_" + std::to_string(layer)).empty() &&
            material.Vec4Of("Color_" + std::to_string(layer), colour))
            out.authored = true;
    }
    if (total > 1e-4f)
    {
        for (int c = 0; c < 3; ++c)
            out.rgb[c] = blended[c] / total;
        out.resolved = true;
    }
    else
    {
        out.maskWeighted = false;
    }
    return out;
}

//! RFG-015: what the converter asks about a model's materials, answered from the
//! archives.
//!
//! The asset CLI answers these by BAKING -- it writes a `.paa` beside the converted
//! `.p3d` and names that. The native path writes nothing, so it names the `.edds`
//! inside the archive and lets the texture layer's mount fallback open it. Same
//! question, no file.
class MountMaterialSink : public Poseidon::Asset::Formats::Enfusion::XobMaterialSink
{
  public:
    explicit MountMaterialSink(const EnfusionMount& mount) : _mount(mount) {}

    //! RFG-039: hide a decal that brought no albedo -- see Resolve().
    bool HideFaces(const std::string& materialSource) const override
    {
        const Resolved& resolved = Resolve(materialSource);
        // A water-erase sheet is hidden whether or not it resolved a colour: unlike a
        // decal, it has no legitimate visible form at all.
        return resolved.waterErase || (resolved.decal && resolved.colour.empty());
    }

    std::string TexturePath(const std::string& materialSource) const override
    {
        const Resolved& r = Resolve(materialSource);
        if (r.colour.empty())
            return r.colour;
        // RFG-023: name the coverage WITH the colour so the texture layer can make
        // them one image. A face that names only the colour can never be a cutout,
        // and Enfusion keeps the coverage in its own file.
        std::string name = r.opacity.empty() ? r.colour : "enfa|" + r.opacity + "|" + r.colour;
        // RFG-070: and wrap the layer colour around whichever of the two that was. The
        // tint is OUTERMOST because the texture source strips it first and then reads the
        // rest exactly as it did before -- which is the only reason a tinted cutout is
        // still a cutout everywhere downstream.
        if (r.tinted && Poseidon::Enfusion::LayerTintEnabled())
            name = Poseidon::Enfusion::MakeLayerTintName(r.tint, name);
        return name;
    }

    //! RFG-019: the opacity map that belongs to a colour texture, or empty.
    //!
    //! Enfusion keeps a leaf's coverage in its OWN file -- `..._A.edds` beside the
    //! `_BCR` -- and the adapter's own census says so in as many words: "the alpha
    //! is in BCR alpha" is false, it lives in `OpacityMap`, 1,715 files, bound as
    //! coverage and never as a blend. Looked up by the colour path because that is
    //! what a built shape's texture is named after.
    std::string OpacityForColour(const std::string& colourPath) const
    {
        const auto hit = _byColour.find(colourPath);
        return hit == _byColour.end() ? std::string() : hit->second;
    }

  private:
    struct Resolved
    {
        std::string colour;
        std::string opacity;
        bool decal = false;
        bool waterErase = false;
        //! RFG-070: the layer colour this material wears its shared tile in, when it has
        //! one. Resolved once per `.emat` here rather than at every TexturePath call --
        //! it costs a `MaskMap` decode, and the cache below is what keeps that at one per
        //! material instead of one per section.
        bool tinted = false;
        float tint[3] = {0.0f, 0.0f, 0.0f};
    };

    const Resolved& Resolve(const std::string& materialSource) const
    {
        const auto cached = _cache.find(materialSource);
        if (cached != _cache.end())
            return cached->second;
        Resolved out;
        ColourKind kind = ColourKind::None;
        Poseidon::Asset::Material::EmatMaterial material;
        if (LoadEmatFromMount(_mount, materialSource, material))
        {
            out.colour = ColourTextureOf(material, &kind);
            out.opacity = material.TextureOf("OpacityMap");
            out.decal = material.className == "MatPBRDecal";
            // RFG-061: the OTHER invisible family, missing here and present in the
            // converting exporter since MAT-050.
            //
            // A `*WaterErase*` material is Enfusion's water-suppression surface: a big
            // flat sheet whose only job is to stop water drawing under a pier or inside
            // a hull. It carries no albedo because it is never meant to be seen. Drawn
            // opaque in fallback grey it becomes exactly what shows up on Everon's
            // shoreline in both maps -- an enormous stretched sail of sand-coloured
            // nothing, hanging across the view.
            //
            // Matched on the material PATH as well as the class name, because the
            // exporter found both spellings in the corpus and matching only one leaves
            // half of them drawing.
            {
                const std::string lowerPath = LowerCopy(materialSource);
                const std::string lowerClass = LowerCopy(material.className);
                out.waterErase = lowerPath.find("watererase") != std::string::npos ||
                                 lowerClass.find("watererase") != std::string::npos;
            }
            if (out.colour.empty())
            {
                // A decal is an overlay on the surface underneath it, and nothing here
                // can composite one. Drawn as an ordinary face it is a white rectangle
                // stuck to a wall, so a decal with no albedo of its own is hidden --
                // absent is closer to right than white.
                if (!out.decal)
                    out.colour = ColourConstantOf(material);
                out.opacity.clear();
                if (!out.colour.empty())
                    kind = ColourKind::Flat;
            }

            // RFG-066: the census, over DISTINCT materials -- this branch runs once per
            // `.emat`, on the cache miss, which is the denominator that means anything.
            // Counting per SECTION would weight a material by how often the world uses
            // it and answer a different question than "how many materials resolve".
            switch (kind)
            {
                case ColourKind::OwnMap: ++GColourCensus.ownMap; break;
                case ColourKind::Flat: ++GColourCensus.flat; break;
                case ColourKind::LayerTile:
                {
                    ++GColourCensus.layerTile;
                    // RFG-070: the tile is SHARED -- 436 distinct one-metre tiles serve all
                    // 2,889 structure materials that use one -- so what makes a given wall
                    // its own colour is the `Color_N` beside it. Resolved here, applied
                    // through the name in TexturePath, and counted three ways because
                    // "authored" and "applied" are different claims and the difference is
                    // the size of the remaining gap.
                    const LayerTint tint = ResolveLayerTint(_mount, material);
                    if (tint.authored)
                        ++GColourCensus.tintAuthored;
                    if (tint.resolved && Poseidon::Enfusion::LayerTintEnabled())
                    {
                        out.tinted = true;
                        for (int c = 0; c < 3; ++c)
                            out.tint[c] = tint.rgb[c];
                        ++GColourCensus.tintApplied;
                        if (tint.maskWeighted)
                            ++GColourCensus.tintMaskWeighted;
                    }
                    else if (tint.authored)
                    {
                        ++GColourCensus.tintDiscarded;
                        if (GColourCensus.firstTintDiscarded.empty())
                            GColourCensus.firstTintDiscarded = materialSource;
                    }
                    break;
                }
                case ColourKind::None:
                    ++GColourCensus.none;
                    if (GColourCensus.firstNone.empty())
                        GColourCensus.firstNone = materialSource;
                    break;
            }
        }
        else
        {
            ++GColourCensus.none;
            if (GColourCensus.firstNone.empty())
                GColourCensus.firstNone = materialSource;
        }
        if (!out.colour.empty() && !out.opacity.empty())
            _byColour.emplace(LowerCopy(out.colour), out.opacity);
        // RFG-038: name the materials that yield no colour texture at all. 71 sections
        // in one village bind nothing, and a section with no texture draws white --
        // which is what "many buildings are white and untextured" is made of.
        if (out.colour.empty())
        {
            static int reported = 0;
            if (reported < 25)
            {
                ++reported;
                std::string keys;
                for (const auto& property : material.properties)
                    keys += (keys.empty() ? "" : ",") + property.name;
                LOG_WARN(World, "Enfusion native load: NO COLOUR for '{}' (class '{}', texture keys: {})",
                         materialSource, material.className, keys.empty() ? "none" : keys);
            }
        }
        return _cache.emplace(materialSource, std::move(out)).first->second;
    }

  public:

    //! RFG-034: the `.emat` path, and it must NOT be empty.
    //!
    //! This returned nothing on the argument that naming a file which cannot be
    //! opened is worse than naming none. That argument expired when the mount
    //! fallback landed in `ReadEmatTextExact`: the file opens now.
    //!
    //! What the empty string cost, all from one cause. With no surface material the
    //! section's `alpha_ref` stays 0, and `veg_cutout` in both model shaders is
    //! `is_veg && alpha_ref > 0`. So every leaf card lost, at once: the two-sided
    //! normal flip (half of every card unlit -- "the trees look flat"), the crown
    //! normals, the leaf sub-surface term, the depth write, and its place in the
    //! retained GPU set. The blended route it fell back to is also the one that
    //! makes a card cast a full rectangular shadow or none.
    //!
    //! `EmatUsesLeafCards` keys on `MatPBRTreeCrown`/`Grass`/`OpacityMap`/`AlphaTest`,
    //! which is exactly what these materials declare.
    std::string MaterialField(const std::string& materialSource) const override
    {
        return _mount.Has(materialSource) ? materialSource : std::string();
    }

  private:
    const EnfusionMount& _mount;
    mutable std::unordered_map<std::string, Resolved> _cache;
    mutable std::unordered_map<std::string, std::string> _byColour;
};

Ref<Texture> ResolveSurfaceTexture(const EnfusionMount& mount, const std::string& ematPath, std::string& why)
{
    namespace Mat = Poseidon::Asset::Material;

    std::vector<uint8_t> bytes;
    if (!mount.Read(ematPath, bytes))
    {
        why = "no such entry";
        return {};
    }
    Mat::EmatMaterial material =
        Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!material.valid())
    {
        why = material.error;
        return {};
    }
    Mat::ResolveEmatInheritance(material,
                                [&mount](const std::string& parentPath, std::string& text)
                                {
                                    std::vector<uint8_t> parentBytes;
                                    if (!mount.Read(parentPath, parentBytes))
                                        return false;
                                    text.assign(reinterpret_cast<const char*>(parentBytes.data()), parentBytes.size());
                                    return true;
                                });

    std::string texturePath = ColourTextureOf(material);
    if (texturePath.empty())
    {
        // A colour-only material. The engine already has a mechanism for exactly this
        // and it is a name, not a new code path.
        float colour[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        if (material.Vec4Of("Color", colour))
        {
            char name[96];
            std::snprintf(name, sizeof(name), "#(argb,8,8,3)color(%.3f,%.3f,%.3f,1,CO)", colour[0], colour[1],
                          colour[2]);
            why = "flat colour";
            return Poseidon::GlobLoadTexture(RStringB(name));
        }
        why = "no BCRMap/AlbedoMap/BCR_1 and no Color";
        return {};
    }

    std::vector<uint8_t> imageBytes;
    if (!mount.Read(texturePath, imageBytes))
    {
        why = "texture '" + texturePath + "' not in the archives";
        return {};
    }
    if (!Poseidon::IsEddsBuffer(imageBytes.data(), imageBytes.size()))
    {
        why = "'" + texturePath + "' is not an .edds";
        return {};
    }
    const Poseidon::EddsImage edds = Poseidon::ReadEddsBuffer(imageBytes.data(), imageBytes.size());
    if (!edds.valid() || edds.mipmaps.empty())
    {
        why = edds.error.empty() ? std::string("no mip levels") : edds.error;
        return {};
    }
    // The largest level at or below 256 px, so a ground layer costs a quarter of a
    // megabyte rather than sixteen.
    size_t level = 0;
    for (size_t i = 0; i < edds.mipmaps.size(); ++i)
    {
        level = i;
        if (edds.mipmaps[i].width <= 256 && edds.mipmaps[i].height <= 256)
            break;
    }
    const auto& mip = edds.mipmaps[level];
    if (mip.width <= 0 || mip.height <= 0 || mip.data.empty())
    {
        why = "empty mip";
        return {};
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height) * 4);
    if (!Poseidon::ConvertPixels(mip.data.data(), rgba.data(), mip.width, mip.height, edds.format,
                                 Poseidon::PixelFormat::RGBA8888))
    {
        why = "cannot convert the pixel format";
        return {};
    }
    if (edds.format == Poseidon::PixelFormat::ARGB8888)
    {
        // The reader hands ARGB8888 back as B,G,R,A and the swap is the caller's, the
        // same way Image::FromFile does it. Getting this wrong is a blue island.
        for (size_t i = 0; i + 2 < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
    }

    AbstractTextBank* bank = Poseidon::GEngine != nullptr ? Poseidon::GEngine->TextBank() : nullptr;
    if (bank == nullptr)
    {
        why = "no texture bank";
        return {};
    }
    Ref<Texture> texture =
        bank->CreateDynamic(mip.width, mip.height, rgba.data(), static_cast<uint32_t>(rgba.size()), false);
    if (!texture)
    {
        why = "CreateDynamic refused";
        return {};
    }
    return texture;
}

/// The assembled Enfusion heightfield, still in raw units.
struct EnfusionTerrain
{
    TerrainDescriptor descriptor;
    std::vector<uint16_t> heights;
    uint32_t tileEdge = 0;
    size_t tilesFound = 0;
    size_t tilesFailed = 0;
    /// RFG-006: which authored surface covers each square metre, from the tiles' TMAT
    /// chunks. One byte per metre over the whole world, so 12.8 km costs 164 MB while
    /// it is being read and is thrown away as soon as the land grid has been filled.
    std::vector<uint8_t> materials;
    uint32_t materialEdge = 0;
    size_t subCells = 0;
    size_t subCellsFailed = 0;
    std::string materialError;
    std::string error;
};

EnfusionTerrain AssembleTerrain(const std::string& addonsDir)
{
    EnfusionTerrain out;

    // RFG-007: the archives are opened once into the process-wide mount and kept, so
    // the surface and model steps that come after this do not reopen sixteen files.
    EnfusionMount& mount = EnfusionMount::Instance();
    if (!mount.IsOpen() || mount.Root() != addonsDir)
    {
        if (!mount.Open(addonsDir))
        {
            out.error = "no readable .pak archives under '" + addonsDir + "'";
            return out;
        }
    }

    // A world's data is not guaranteed to sit in one archive; the mount indexes all
    // of them together, so this is one lookup over the whole install.
    const std::string needle = LowerCopy(WantedWorld());
    std::string descriptorPath;
    std::map<uint32_t, std::string> tiles;
    for (const std::string& path : mount.FindAll(needle))
    {
        if (path.size() > 5 && path.compare(path.size() - 5, 5, ".terr") == 0)
        {
            descriptorPath = path;
        }
        else if (path.size() > 6 && path.compare(path.size() - 6, 6, ".ttile") == 0)
        {
            // `<name>_<index>.ttile`; the index is row-major over the tile grid.
            const size_t underscore = path.rfind('_');
            if (underscore == std::string::npos)
                continue;
            const std::string digits = path.substr(underscore + 1, path.size() - underscore - 7);
            if (digits.empty() ||
                !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                continue;
            tiles[static_cast<uint32_t>(std::stoul(digits))] = path;
        }
    }

    if (descriptorPath.empty())
    {
        out.error = "no .terr descriptor matching '" + std::string(WantedWorld()) + "'";
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!mount.Read(descriptorPath, bytes))
    {
        out.error = "cannot read '" + descriptorPath + "'";
        return out;
    }
    out.descriptor = ReadTerrainDescriptor(bytes.data(), bytes.size());
    if (!out.descriptor.valid())
    {
        out.error = out.descriptor.error;
        return out;
    }
    out.tilesFound = tiles.size();
    if (tiles.empty())
    {
        out.error = "no .ttile files matching '" + std::string(WantedWorld()) + "'";
        return out;
    }

    const uint32_t gridW = out.descriptor.gridWidth;
    const uint32_t gridH = out.descriptor.gridHeight;
    out.heights.assign(static_cast<size_t>(gridW) * gridH, 0);

    // The tile edge comes from the tiles themselves: the descriptor does not carry it
    // and it is not constant across the corpus.
    uint32_t tilesX = 0;
    for (const auto& [index, tilePath] : tiles)
    {
        std::vector<uint8_t> tileBytes;
        if (!mount.Read(tilePath, tileBytes))
        {
            out.tilesFailed++;
            continue;
        }
        const TerrainTile tile = ReadTerrainTile(tileBytes.data(), tileBytes.size());
        if (!tile.valid() || tile.samples == 0)
        {
            out.tilesFailed++;
            continue;
        }
        if (out.tileEdge == 0)
        {
            out.tileEdge = tile.samples - 1;
            tilesX = (gridW - 1) / out.tileEdge;
            if (out.descriptor.subPerTileEdge > 0)
            {
                out.materialEdge = tilesX * out.descriptor.subPerTileEdge * Tmat::kMaskEdge;
                // 0xFF means "nothing said", which is distinguishable from material 0.
                out.materials.assign(static_cast<size_t>(out.materialEdge) * out.materialEdge, 0xFF);
            }
        }
        if (tilesX == 0)
            continue;
        const uint32_t originX = (index % tilesX) * out.tileEdge;
        const uint32_t originZ = (index / tilesX) * out.tileEdge;
        for (uint32_t z = 0; z < tile.samples; ++z)
        {
            const uint32_t worldZ = originZ + z;
            if (worldZ >= gridH)
                break;
            for (uint32_t x = 0; x < tile.samples; ++x)
            {
                const uint32_t worldX = originX + x;
                if (worldX >= gridW)
                    break;
                out.heights[static_cast<size_t>(worldZ) * gridW + worldX] = tile.At(x, z);
            }
        }

        // The surfaces ride in the same tile bytes that are already decompressed and
        // in hand; re-reading 2,500 tiles for a chunk we hold would be pure cost.
        if (out.materials.empty())
            continue;
        const IffFile tileIff = ReadIff(tileBytes.data(), tileBytes.size());
        const IffChunk* tmat = tileIff.valid() ? tileIff.Find(FourCC("TMAT")) : nullptr;
        if (tmat == nullptr)
            continue;
        std::vector<Tmat::SubCell> cells;
        std::string tmatError;
        if (!Tmat::ReadSubCells(tileBytes.data() + tmat->offset, tmat->size, cells, tmatError))
        {
            if (out.materialError.empty())
                out.materialError = tmatError;
            continue;
        }
        std::array<uint8_t, Tmat::kMaskEdge * Tmat::kMaskEdge> mask{};
        for (const Tmat::SubCell& cell : cells)
        {
            ++out.subCells;
            const size_t baseZ = static_cast<size_t>(cell.subY) * Tmat::kMaskEdge;
            const size_t baseX = static_cast<size_t>(cell.subX) * Tmat::kMaskEdge;
            if (baseZ + Tmat::kMaskEdge > out.materialEdge || baseX + Tmat::kMaskEdge > out.materialEdge)
            {
                ++out.subCellsFailed;
                continue;
            }
            if (cell.blendSize == 0)
            {
                // A single-material sub-cell carries no payload at all.
                mask.fill(0);
            }
            else if (!Tmat::DecodeBlend(cell.blend, cell.blendSize, cell.palette.size(), mask.data(), tmatError))
            {
                ++out.subCellsFailed;
                if (out.materialError.empty())
                    out.materialError = tmatError;
                mask.fill(0);
            }
            // The palette is the trap the decoder's own comment names: it is sorted
            // ascending by material index, so entry 0 is the lowest index present and
            // NOT the base layer.
            for (int mz = 0; mz < Tmat::kMaskEdge; ++mz)
                for (int mx = 0; mx < Tmat::kMaskEdge; ++mx)
                {
                    const uint8_t entry = mask[static_cast<size_t>(mz) * Tmat::kMaskEdge + mx];
                    if (entry < cell.palette.size())
                        out.materials[(baseZ + mz) * out.materialEdge + baseX + mx] =
                            static_cast<uint8_t>(cell.palette[entry]);
                }
        }
    }
    return out;
}

/// RFG-011: what the world places, resolved to meshes.
///
/// A placement names a prefab by GUID. Turning that into a drawable mesh is three
/// lookups, and each one has a trap the corpus taught somebody the hard way:
///
///   1. GUID -> `.et` path, from `resourceDatabase.rdb`. Those files sit LOOSE
///      beside the archives, not inside them. The tool also has a text-scan tier
///      that walks 57,000 pak entries to recover the same map; it is slower AND
///      incomplete -- it misses 562 Everon prefabs carrying 13,449 placements,
///      including the church that made a town churchless. Read the `.rdb`.
///   2. `.et` -> `.xob`, following inheritance (PrefabResolve.hpp).
///   3. Y is absolute or terrain-relative depending on a flag, and reading every Y
///      as absolute buries Everon's houses a median 29 m underground.
struct PlacementSummary
{
    size_t placements = 0;
    size_t withGuid = 0;
    size_t guidResolved = 0;
    size_t meshResolved = 0;
    size_t distinctMeshes = 0;
    size_t terrainRelative = 0;
    // RFG-064, the attachment side. Counted separately from the mesh totals above
    // because "the houses are missing" and "the houses have no doors" are different
    // failures and the placement histogram cannot tell them apart.
    size_t slotPrefabs = 0;    //!< distinct prefabs whose .et chain declared slots
    size_t slotBones = 0;      //!< socket bones that matched a slot rule
    size_t slotUnresolved = 0; //!< of those, bones whose part prefab gave no .xob
    size_t slotBuildings = 0;  //!< PLACEMENTS that carry at least one attachment
    std::string slotFailure;
    std::string firstFailure;
    std::string error;
};

/// Every `resourceDatabase.rdb` beside the archives, merged.
bool LoadResourceDatabases(const std::string& addonsDir, std::unordered_map<std::string, std::string>& byGuid,
                           std::string& error)
{
    namespace Enf = Poseidon::Asset::Formats::Enfusion;
    std::error_code ec;
    size_t files = 0;
    for (std::filesystem::recursive_directory_iterator it(addonsDir, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        if (LowerCopy(it->path().filename().string()) != "resourcedatabase.rdb")
            continue;
        std::ifstream file(it->path(), std::ios::binary);
        if (!file)
            continue;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const Enf::ResourceDatabase db = Enf::ReadResourceDatabase(bytes.data(), bytes.size());
        // Refused WHOLE rather than partially: a walk that stopped early yields
        // plausible paths for the wrong GUIDs, which is worse than none.
        if (!db.closes())
        {
            error = it->path().string() + ": " + (db.error.empty() ? "does not close" : db.error);
            return false;
        }
        for (const auto& [guid, path] : db.byGuid)
            byGuid.emplace(guid, path);
        ++files;
    }
    if (files == 0)
    {
        error = "no resourceDatabase.rdb beside the archives";
        return false;
    }
    return true;
}

//! RFG-064: one part hung off a named bone of its building's mesh.
//!
//! Everon's houses had holes where their doors, gates and windows belong because
//! `ResolvePrefabModel` returns exactly one `.xob` per prefab and Enfusion keeps the
//! openings as SEPARATE prefabs bound to socket bones of the shell. The bone gives
//! the transform and the prefab gives the mesh; the pair is what this record is.
//!
//! Already FLATTENED into the shell's model space: a door set is a frame with its
//! own `socket_door_left` carrying the leaf, so the tree is two or three deep and
//! composing it once per prefab beats composing it once per placement.
struct MeshAttachment
{
    Vector3 offset;    //!< in the shell's model space
    Matrix3 basis;     //!< part model space -> shell model space
    uint32_t mesh = 0; //!< index into the ATTACHMENT mesh list
};

//! Everything one building prefab hangs on itself. Keyed per prefab, not per mesh:
//! two prefabs can share a shell `.xob` and dress it differently.
struct AttachSet
{
    std::vector<MeshAttachment> parts;
};

constexpr uint32_t kNoAttachSet = 0xffffffffu;

//! A socket bone's rotation, in the convention the rest of this loader already uses.
//!
//! The `.xob` stores a quaternion and the engine's `MRotationY` is the TRANSPOSE of
//! the textbook quaternion matrix, so one of the two readings is 180 degrees wrong on
//! every wall and the difference is not visible on a symmetric door frame. Measured
//! rather than picked: `FarmHouse_E_1L01_Base.et` places three curtain props by
//! explicit `angles` next to the sockets of the windows they hang in -- the curtain
//! in the +X wall carries `angles 0 -90 0` and the sockets in that same wall carry
//! `(0, -0.707, 0, 0.707)`, while the curtains in the -Z wall carry `angles 0 0 0`
//! against an identity socket quaternion. So a socket quaternion of yaw theta is the
//! same rotation the loader already feeds `AddObject` as theta degrees, and since
//! `AddObject` builds it with `Matrix4(MRotationY, ...)` the matching quaternion
//! matrix is the textbook one CONJUGATED.
inline Matrix3 BoneBasis(const float q[4])
{
    const float norm = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(norm > 1e-6f))
        return Matrix3(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const float s = 2.0f / norm;
    // Conjugated: the vector part is negated on the way in, which is exactly the
    // transpose of the textbook matrix and costs nothing to write out directly.
    // RFG-068: no longer conjugated. This negation existed only to cancel the mirrored
    // yaw that AddObject was applying; with the sign fixed at the placement it would
    // mirror the sockets instead. The curtain-prop evidence in the comment above --
    // `angles 0 -90 0` beside a socket quaternion of (0, -0.707, 0, 0.707) -- matches
    // WITHOUT the conjugation, which is what says the two changes belong together.
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return Matrix3(1.0f - s * (y * y + z * z), s * (x * y - z * w), s * (x * z + y * w), s * (x * y + z * w),
                   1.0f - s * (x * x + z * z), s * (y * z - x * w), s * (x * z - y * w), s * (y * z + x * w),
                   1.0f - s * (x * x + y * y));
}


//! One placement, resolved: where it goes and which `.xob` draws it.
struct ResolvedPlacement
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yawDeg = 0.0f;
    //! RFG-028: the placement's own scale, already folded through the parent chain
    //! by the EBIN reader. Dropping it silently was drawing every scaled instance at
    //! its prefab's full size -- Everon's scales run 0.7 to 1.0, so a stand of trees
    //! came out up to 43% too large with nothing in the manifest to show for it.
    float scale = 1.0f;
    //! RFG-058: pitch and roll, in degrees. Kept because 65% of Everon uses them.
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;
    bool terrainRelative = false;
    uint32_t mesh = 0; //!< index into the mesh list
    uint32_t attachSet = kNoAttachSet; //!< RFG-064: index into the attachment sets, or none
};

//! RFG-027: one road, ready to become a strip of geometry.
struct ResolvedRoad
{
    float origin[3] = {0.0f, 0.0f, 0.0f}; //!< the entity's own `coords`
    std::vector<float> points;            //!< x,y,z triples, local to `origin`
    float yawDeg = 0.0f;                  //!< the entity's own yaw, parent yaw folded in
    float width = 0.0f;                   //!< metres, 0 when the road left it default
    std::string texture;                  //!< colour `.edds`, empty when unresolved
    std::string material;                 //!< the `.emat` the texture came from
};

namespace Enf = Poseidon::Asset::Formats::Enfusion;

/// RFG-064: the caches one attachment walk needs, kept for the whole world load.
///
/// All three are keyed on a path rather than a placement because the work is per
/// ASSET: Everon places one farmhouse prefab thousands of times, and reading its
/// shell `.xob` -- a multi-megabyte zlib decompress -- once per placement would cost
/// more than the rest of the world load put together.
struct AttachWorkspace
{
    const EnfusionMount* mount = nullptr;
    std::unordered_map<std::string, std::string> partToMesh;              //!< `.et` (lower) -> `.xob`
    std::unordered_map<std::string, std::vector<Enf::XobBone>> meshBones; //!< `.xob` -> its bones
    std::unordered_map<std::string, uint32_t> index;                      //!< `.xob` -> attachment mesh slot
    std::vector<size_t> counts;                                           //!< placements per attachment mesh
};

const std::vector<Enf::XobBone>& BonesOf(AttachWorkspace& ws, const std::string& xobPath, PlacementSummary& summary)
{
    const auto hit = ws.meshBones.find(xobPath);
    if (hit != ws.meshBones.end())
        return hit->second;
    std::vector<Enf::XobBone> bones;
    std::vector<uint8_t> blob;
    if (ws.mount->Read(xobPath, blob))
    {
        const Enf::XobHeader header = Enf::ReadXobHeader(blob.data(), blob.size());
        if (header.valid())
            bones = header.bones;
        else if (summary.slotFailure.empty())
            summary.slotFailure = xobPath + ": " + header.error;
    }
    return ws.meshBones.emplace(xobPath, std::move(bones)).first->second;
}

/// Every part a prefab hangs on its shell, flattened into the shell's model space.
///
/// Recursive because the tree is: `FarmHouse_E_1L01_Base.et` binds
/// `DoorSet_Village_E_01_L_EXT_ST_M1.et` to `socket_door_ext_left_01`, and that door
/// SET is itself a frame mesh with a `socket_door_LEFT` bone carrying the leaf. Stop
/// at the frame and every doorway has a frame and no door in it -- which looks like
/// the hole this whole change is here to close.
void CollectAttachments(AttachWorkspace& ws, PlacementSummary& summary, const std::string& etPath,
                        const std::string& xobPath, const Vector3& offset, const Matrix3& basis, int depth,
                        std::vector<MeshAttachment>& out)
{
    // Three levels is building -> set -> leaf, which is the deepest the corpus goes;
    // the limit is here to bound authored data, not to express an expectation.
    if (depth > 2)
        return;
    const Enf::PrefabSlots slots = Enf::ResolvePrefabSlots(*ws.mount, etPath);
    if (slots.empty())
        return;
    if (depth == 0)
        ++summary.slotPrefabs;

    // Copied, not referenced: the recursive call below inserts into `meshBones`.
    const std::vector<Enf::XobBone> bones = BonesOf(ws, xobPath, summary);
    for (const Enf::XobBone& bone : bones)
    {
        const std::string partEt = Enf::SlotPrefabForBone(slots, bone.name);
        if (partEt.empty())
            continue;
        ++summary.slotBones;
        const std::string key = LowerCopy(partEt);
        auto cached = ws.partToMesh.find(key);
        if (cached == ws.partToMesh.end())
        {
            const Enf::PrefabResolution resolved = Enf::ResolvePrefabModel(*ws.mount, partEt);
            if (resolved.xobPath.empty() && summary.slotFailure.empty())
                summary.slotFailure = partEt + ": " + resolved.reason;
            cached = ws.partToMesh.emplace(key, resolved.xobPath).first;
        }
        if (cached->second.empty())
        {
            ++summary.slotUnresolved;
            continue;
        }

        MeshAttachment part;
        part.basis = basis * BoneBasis(bone.rotation);
        part.offset = offset + basis * Vector3(bone.translation.x, bone.translation.y, bone.translation.z);
        {
            // RFG-100 diagnosis: the church's attachments, bone by bone.
            static int attachDiag = 0;
            if (LowerCopy(xobPath).find("church_01") != std::string::npos && attachDiag++ < 24)
                LOG_INFO(World, "RFG-100 attach: parent '{}' bone '{}' parent={} t=[{:.2f} {:.2f} {:.2f}] q=[{:.3f} {:.3f} {:.3f} {:.3f}] -> part '{}' offset=[{:.2f} {:.2f} {:.2f}] depth={}",
                         xobPath, bone.name, bone.parent, bone.translation.x, bone.translation.y, bone.translation.z,
                         bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3], cached->second,
                         part.offset.X(), part.offset.Y(), part.offset.Z(), depth);
        }
        auto slot = ws.index.find(cached->second);
        if (slot == ws.index.end())
        {
            slot = ws.index.emplace(cached->second, static_cast<uint32_t>(ws.counts.size())).first;
            ws.counts.push_back(0);
        }
        part.mesh = slot->second;
        out.push_back(part);

        CollectAttachments(ws, summary, partEt, cached->second, part.offset, part.basis, depth + 1, out);
    }
}

PlacementSummary ResolvePlacements(const EnfusionMount& mount, const std::string& addonsDir,
                                   std::vector<std::pair<std::string, size_t>>& meshes,
                                   std::vector<ResolvedPlacement>& resolved, std::vector<ResolvedRoad>& roads,
                                   std::vector<std::pair<std::string, size_t>>& attachMeshes,
                                   std::vector<AttachSet>& attachSets)
{
    namespace Enf = Poseidon::Asset::Formats::Enfusion;
    PlacementSummary out;

    std::unordered_map<std::string, std::string> byGuid;
    if (!LoadResourceDatabases(addonsDir, byGuid, out.error))
        return out;

    // The world's own `.ent`, beside the terrain in the same folder.
    std::string entPath;
    for (const std::string& path : mount.FindAll(std::string(WantedWorld()) + "/"))
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ent") == 0 &&
            path.find("empty") == std::string::npos)
        {
            entPath = path;
            break;
        }
    if (entPath.empty())
    {
        out.error = "no .ent world beside the terrain";
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!mount.Read(entPath, bytes))
    {
        out.error = "cannot read " + entPath;
        return out;
    }
    const Enf::EbinWorld world = Enf::ReadEbin(bytes.data(), bytes.size());
    if (!world.valid())
    {
        out.error = world.error;
        return out;
    }
    out.placements = world.placements.size();

    // One resolution per DISTINCT prefab, not per placement: Everon's 1.23 M
    // placements come from a few thousand prefabs, and the chain walk reads files.
    std::unordered_map<std::string, std::string> prefabToMesh;
    std::unordered_map<std::string, uint32_t> meshIndex;
    std::vector<size_t> meshCounts;
    // RFG-064: the attachment side keeps its OWN mesh list. Sharing the main one
    // would put doors and windows into the histogram the model cap ranks, and a
    // door is a rare mesh by construction -- Everon uses hundreds of distinct ones --
    // so the cap would drop exactly the parts this is here to restore.
    std::unordered_map<std::string, uint32_t> prefabToAttach;
    AttachWorkspace workspace;
    workspace.mount = &mount;
    resolved.reserve(world.placements.size());
    // Resolved once per material GUID: 2,370 roads share a couple of dozen surfaces
    // and each resolution reads an `.emat` out of an archive.
    std::unordered_map<std::string, std::pair<std::string, std::string>> roadTextures;

    for (const Enf::EbinPlacement& placement : world.placements)
    {
        if (!placement.points.empty())
        {
            ResolvedRoad road;
            road.origin[0] = placement.position[0];
            road.origin[1] = placement.position[1];
            road.origin[2] = placement.position[2];
            road.points = placement.points;
            road.yawDeg = placement.angles[1];
            road.width = placement.width;

            // Two routes to the same tarmac, and both are needed. 183 roads name a
            // `Material` GUID directly; 2,200 name a prefab whose `.et` names the
            // `.emat` in full. Keyed on whichever reference the road actually has,
            // so the cache never conflates a road that has neither with one that has
            // a material resolving to nothing.
            const std::string materialKey =
                placement.materialGuid.empty() ? ("et:" + placement.prefabGuid) : placement.materialGuid;
            auto cachedTexture = roadTextures.find(materialKey);
            if (cachedTexture == roadTextures.end())
            {
                std::string ematPath;
                const auto materialHit = byGuid.find(placement.materialGuid);
                if (materialHit != byGuid.end())
                {
                    ematPath = materialHit->second;
                }
                else
                {
                    const auto prefabHit = byGuid.find(placement.prefabGuid);
                    if (prefabHit != byGuid.end())
                        ematPath = Enf::ResolvePrefabMaterial(mount, prefabHit->second);
                }
                std::string texture;
                if (!ematPath.empty())
                {
                    Poseidon::Asset::Material::EmatMaterial material;
                    if (LoadEmatFromMount(mount, ematPath, material))
                        texture = ColourTextureOf(material);
                }
                cachedTexture = roadTextures.emplace(materialKey, std::make_pair(texture, ematPath)).first;
            }
            road.texture = cachedTexture->second.first;
            road.material = cachedTexture->second.second;
            roads.push_back(std::move(road));
            continue;
        }
        if (placement.prefabGuid.empty())
            continue;
        ++out.withGuid;
        if (!placement.hasFlags && std::fabs(placement.position[1]) < 2.0f)
            ++out.terrainRelative;

        auto cached = prefabToMesh.find(placement.prefabGuid);
        if (cached == prefabToMesh.end())
        {
            std::string mesh;
            const auto guidHit = byGuid.find(placement.prefabGuid);
            if (guidHit != byGuid.end())
            {
                const Enf::PrefabResolution resolved = Enf::ResolvePrefabModel(mount, guidHit->second);
                if (!resolved.xobPath.empty())
                    mesh = resolved.xobPath;
                else if (out.firstFailure.empty())
                    out.firstFailure = resolved.reason;
            }
            else if (out.firstFailure.empty())
            {
                out.firstFailure = "GUID " + placement.prefabGuid + " not in any resourceDatabase.rdb";
            }
            cached = prefabToMesh.emplace(placement.prefabGuid, mesh).first;
        }
        if (cached->second.empty())
            continue;
        if (byGuid.count(placement.prefabGuid) != 0)
            ++out.guidResolved;
        ++out.meshResolved;

        auto slot = meshIndex.find(cached->second);
        if (slot == meshIndex.end())
        {
            slot = meshIndex.emplace(cached->second, static_cast<uint32_t>(meshCounts.size())).first;
            meshCounts.push_back(0);
        }
        ++meshCounts[slot->second];

        ResolvedPlacement item;
        item.position[0] = placement.position[0];
        item.position[1] = placement.position[1];
        item.position[2] = placement.position[2];
        // RFG-058: the WHOLE triple. This kept index 1 and discarded pitch and roll,
        // described as a documented reduction rather than an omission. Measured over
        // Everon, that reduction throws away the orientation of 804,254 of 1,233,078
        // placements -- 65.2%, with tilts up to 230 degrees.
        //
        // It is what makes walkways, jetties and stepped paths sit wrong: a run of
        // modules following a slope carries its slope in the two angles being dropped,
        // so every piece is laid flat and the run stops being flush. The converted map
        // has the same defect from the same reduction, which is what says the fault is
        // here and not in either map's placement code.
        // RFG-068: NEGATE all three. The engine's rotation matrices are the transpose
        // of the textbook ones, and Enfusion's `angles` are the textbook ones.
        //
        // `Matrix3P::SetRotationY` (Math3DP.cpp:393) builds [[c,0,-s],[0,1,0],[s,0,c]],
        // which is Ry(-theta) in the usual convention, and `Landscape::AddObject`
        // (Landscape.cpp:2563) uses exactly that. So every natively placed object stood
        // at MINUS its authored yaw -- mirrored, not merely offset.
        //
        // Measured on Everon: neighbouring pairs of the same modular prefab, rotated
        // back into the model frame, land exactly on a local axis --
        //
        //   ConcreteKerb_01_2m_v2   2,406 of 2,406 pairs (100%) with the textbook
        //                           rotation, 296 with the engine's
        //   Pier_01                 123 of 126 (97.6%) against 0 of 126
        //   39 modular families     8,180 of 14,277 against 993
        //
        // Two places in this repo already do the yaw ARITHMETIC by hand -- the child
        // composition in EbinWorld.cpp and the road pre-rotation below -- and both write
        // `x' = c*x + s*z`, the textbook form, and both produce correct results. Only the
        // object path went through `MRotationY`, and only it was mirrored. The comment at
        // the road rotation claims it is "the same basis AddObject builds"; it is the
        // transpose, and that wrong sentence is why this took three sessions to find.
        //
        // Pitch and roll are transposed for the same reason, which is why RFG-058 applied
        // them and nothing looked better: with the old signs a tilted module was measured
        // WORSE than applying no tilt at all (median axis error 5.65 deg against 3.37).
        // Negated, it is 2.04 deg.
        item.yawDeg = -placement.angles[1];
        item.pitchDeg = -placement.angles[0];
        item.rollDeg = -placement.angles[2];
        {
            // RFG-100 diagnosis: every church_01* placement as resolved (doors stand in the nave).
            static int churchDiag = 0;
            if (LowerCopy(cached->second).find("church_01") != std::string::npos && churchDiag++ < 12)
                LOG_INFO(World, "RFG-100 placement: '{}' class='{}' pos=[{:.2f} {:.2f} {:.2f}] angles=[{:.1f} {:.1f} {:.1f}] scale={:.2f} flags={} points={}",
                         cached->second, placement.className, placement.position[0], placement.position[1], placement.position[2],
                         placement.angles[0], placement.angles[1], placement.angles[2], placement.scale, placement.hasFlags ? 1 : 0,
                         placement.points.size());
        }
        item.scale = (std::isfinite(placement.scale) && placement.scale > 0.0f) ? placement.scale : 1.0f;
        // The Y rule, measured over Everon's 1.23 M placements: with a Flags block
        // 96.3% sit within half a metre of the terrain, without one the median Y is
        // exactly 0. Reading every Y as absolute buries the houses a median 29 m
        // under, because SCR_DestructibleBuildingEntity is 93% flagless.
        item.terrainRelative = !placement.hasFlags && std::fabs(placement.position[1]) < 2.0f;
        item.mesh = slot->second;
        // RFG-064: the parts this prefab hangs off its shell's socket bones. Once
        // per DISTINCT prefab GUID, like the mesh above.
        auto attachCached = prefabToAttach.find(placement.prefabGuid);
        if (attachCached == prefabToAttach.end())
        {
            uint32_t setIndex = kNoAttachSet;
            const auto guidHit = byGuid.find(placement.prefabGuid);
            if (guidHit != byGuid.end())
            {
                AttachSet set;
                const Matrix3 identity(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
                CollectAttachments(workspace, out, guidHit->second, cached->second, Vector3(0.0f, 0.0f, 0.0f), identity, 0, set.parts);
                if (!set.parts.empty())
                {
                    setIndex = static_cast<uint32_t>(attachSets.size());
                    attachSets.push_back(std::move(set));
                }
            }
            attachCached = prefabToAttach.emplace(placement.prefabGuid, setIndex).first;
        }
        item.attachSet = attachCached->second;
        if (item.attachSet != kNoAttachSet)
        {
            ++out.slotBuildings;
            for (const MeshAttachment& part : attachSets[item.attachSet].parts)
                ++workspace.counts[part.mesh];
        }

        resolved.push_back(item);
    }
    attachMeshes.resize(workspace.counts.size());
    for (const auto& [path, index] : workspace.index)
        attachMeshes[index] = {path, workspace.counts[index]};
    out.distinctMeshes = meshCounts.size();
    meshes.resize(meshCounts.size());
    for (const auto& [path, index] : meshIndex)
        meshes[index] = {path, meshCounts[index]};
    // The placements index INTO this list, so it cannot be sorted here; the callers
    // that want an order build their own. (An earlier version sorted it and left the
    // indices pointing at the wrong meshes, which drew the right number of the wrong
    // objects -- a picture that looks plausible and is not.)
    return out;
}

/// RFG-027: a road's centreline -> a ribbon of quads the engine can draw.
///
/// Everon ships no road geometry at all: Enfusion generates the strip at load time
/// from `RoadEntity`'s spline, so anything that wants roads has to generate them
/// too. This is the same generation, reduced to what a first pass needs -- a flat
/// ribbon of the authored width, mitred at each interior point so the seams do not
/// gap on a bend, with V running along the road so the tarmac tiles by length.
///
/// Deliberately NOT modelled: crossings, end caps, the separate line-marking pass,
/// and the terrain cut Enfusion applies underneath. Those are the difference between
/// "the road network is there" and "the road network is finished", and the first is
/// worth having on its own.
LODShapeWithShadow* BuildRoadShape(const std::vector<float>& points, float width, const std::string& texture,
                                   const std::string& material, const std::string& name)
{
    namespace Fmt = Poseidon::Asset::Formats;

    const size_t count = points.size() / 3;
    if (count < 2)
        return nullptr;

    // The authored default. Everon's roads that set `Width` cluster on 6-8 m; a road
    // that leaves it unset gets the commonest value rather than a zero-area ribbon.
    const float halfWidth = 0.5f * ((width > 0.1f) ? width : 6.0f);

    Fmt::MLOD::WriteLod lod;
    lod.resolution = 1.0f;
    lod.points.reserve(count * 2);
    lod.normals.reserve(count * 2);
    lod.faces.reserve(count * 2);

    std::vector<float> distance(count, 0.0f);
    for (size_t i = 1; i < count; ++i)
    {
        const float dx = points[i * 3 + 0] - points[(i - 1) * 3 + 0];
        const float dz = points[i * 3 + 2] - points[(i - 1) * 3 + 2];
        distance[i] = distance[i - 1] + std::sqrt(dx * dx + dz * dz);
    }

    for (size_t i = 0; i < count; ++i)
    {
        // The mitre direction: the average of the segments meeting at this point, so
        // an interior vertex is shared by both quads and the ribbon has no seam. A
        // real mitre would also widen the offset by 1/cos(half-angle); at Everon's
        // curvature that correction is under a centimetre and is left out.
        float dx = 0.0f, dz = 0.0f;
        if (i + 1 < count)
        {
            dx += points[(i + 1) * 3 + 0] - points[i * 3 + 0];
            dz += points[(i + 1) * 3 + 2] - points[i * 3 + 2];
        }
        if (i > 0)
        {
            dx += points[i * 3 + 0] - points[(i - 1) * 3 + 0];
            dz += points[i * 3 + 2] - points[(i - 1) * 3 + 2];
        }
        const float length = std::sqrt(dx * dx + dz * dz);
        if (length > 1e-4f)
        {
            dx /= length;
            dz /= length;
        }
        else
        {
            dx = 1.0f;
            dz = 0.0f;
        }
        // Perpendicular in the ground plane.
        const float nx = -dz * halfWidth;
        const float nz = dx * halfWidth;

        Fmt::MLOD::WritePoint left;
        left.position = {points[i * 3 + 0] + nx, points[i * 3 + 1], points[i * 3 + 2] + nz};
        Fmt::MLOD::WritePoint right;
        right.position = {points[i * 3 + 0] - nx, points[i * 3 + 1], points[i * 3 + 2] - nz};
        lod.points.push_back(left);
        lod.points.push_back(right);
        // ONE NORMAL PER POINT, indexed by the point index, exactly as the `.xob`
        // converter writes them. A single shared normal with every corner indexing 0
        // is a legal MLOD and loads without complaint -- and the section it produces
        // draws untextured, which is a much harder thing to see than a failure.
        //
        // And it points DOWN in the file, which is what an upward-facing surface
        // looks like here: `MeshBuild.cpp:44` negates every normal on the way to the
        // vertex buffer, and `shading.wgsl` then takes `max(dot(N, -sun), 0)`. A road
        // written with +Y gets no sun term at all AND samples the sky irradiance from
        // the darkest point of the dome -- which is the whole of "the roads are
        // black". The `.xob` path reaches the same convention from the other side,
        // via `fixOrientation`.
        lod.normals.push_back({0.0f, -1.0f, 0.0f});
        lod.normals.push_back({0.0f, -1.0f, 0.0f});
    }

    for (size_t i = 0; i + 1 < count; ++i)
    {
        // V tiles every 2 x width of length, which is what makes a 6 m road's texture
        // square rather than smeared.
        const float v0 = distance[i] / (halfWidth * 4.0f);
        const float v1 = distance[i + 1] / (halfWidth * 4.0f);
        const int32_t base = static_cast<int32_t>(i) * 2;
        const float u[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        const float v[4] = {v0, v0, v1, v1};
        const int32_t point[4] = {base, base + 1, base + 3, base + 2};
        // Two triangles rather than one quad, for the same reason as the normals:
        // this is the shape the converter emits and the one the whole path is
        // exercised against.
        //
        // Ring order, and it is the RIGHT order even though the cross product of the
        // corner edges comes out negative on Y. Reversing it -- which the arithmetic
        // alone argues for -- makes every road disappear behind backface culling
        // while its shadow stays, so the engine's front face here is the one ring
        // order gives. The arithmetic was measured against the picture and lost.
        const int triangle[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& corners : triangle)
        {
            Fmt::MLOD::WriteFace face;
            face.vertexCount = 3;
            // A road without a texture (dirt tracks, and any material that
            // resolves to no colour texture) would otherwise draw with the
            // white untextured fallback across metres of landscape. Give it a
            // flat dirt-grey constant through the existing `enfc|` mechanism
            // (RFG-039) instead: native road ribbons only, no legacy path
            // names such a texture.
            face.texture = texture.empty() ? "enfc|0.40,0.35,0.29" : texture;
            face.material = material;
            for (int c = 0; c < 3; ++c)
            {
                face.vertices[c].point = point[corners[c]];
                face.vertices[c].normal = point[corners[c]];
                face.vertices[c].u = u[corners[c]];
                face.vertices[c].v = v[corners[c]];
            }
            lod.faces.push_back(face);
        }
    }

    Fmt::MLOD::WriteModel model;
    model.lods.push_back(std::move(lod));

    const std::vector<char> buffer = Fmt::MLODWriter::writeToBuffer(model);
    if (buffer.empty())
        return nullptr;
    auto parsed = std::make_shared<Poseidon::Model::Model>(
        Fmt::MLODLoader::loadFromBuffer(buffer.data(), static_cast<int>(buffer.size()), name));
    if (parsed->lodLevels.empty())
        return nullptr;
    return Poseidon::Shapes.NewFromModel(name.c_str(), false, true, parsed, nullptr);
}

/// RFG-013: one `.xob` in the archives -> one shape the engine can draw.
///
/// The hop through MLOD in memory is deliberate. A direct XobModel -> Model builder
/// would have to reimplement the loader's vertex merge, section building, UV-set
/// handling and winding bookkeeping, and then keep them in step with the loader
/// forever; `writeToBuffer` / `loadFromBuffer` are pure memory, so the round trip
/// costs one serialise plus one parse per DISTINCT model and buys all of that.
/// Nothing is written to disk.
//! RFG-097: the model half of BuildShapeFromXob, on its own so the object stream's cold
//! path (ModelCache) can build a native model by NAME when a placement far from the
//! start comes into range. Everything about how a `.xob` becomes a Model lives here.
std::shared_ptr<Poseidon::Model::Model> BuildModelFromXob(const EnfusionMount& mount, const std::string& xobPath,
                                                          std::string& why)
{
    namespace Enf = Poseidon::Asset::Formats::Enfusion;
    namespace Fmt = Poseidon::Asset::Formats;

    std::vector<uint8_t> bytes;
    if (!mount.Read(xobPath, bytes))
    {
        why = "not in any archive";
        return nullptr;
    }
    Enf::ConvertOptions options;
    // Both corrections ON. They are opt-in flags in the tool because that is where
    // the A/B lives; for a world that has to look right there is no argument for
    // either default. Orientation is an XOR PAIR -- negate the normal AND swap the
    // winding -- and half of it is worse than none. Origin writes autocenter=0, and
    // without it Landscape::AddObject offsets every object by its own bounding
    // centre, which sinks a tree by half its height.
    options.fixOrientation = true;
    options.fixOrigin = true;
    // RFG-022: keep the authored LOD spacing. Reforger ladders span 1:500 in screen
    // coverage and their last level is a four-triangle impostor; numbering the levels
    // 1..5 brings that impostor in to a few tens of metres, which reads as a forest of
    // flat cards.
    options.lodFromThreshold = true;
    // RFG-072: BOTH UV sets. A `MatPBRMulti` paints its GLOBAL_MASK in the `.xob`'s second
    // stream (`UVSrcGlobalMaps "UV set 2"`), and without this block the mask sampled the
    // first -- the metre-scale tiling unwrap, u[-16..32] on Church_01's walls -- and
    // repeated ~47 times across the wall as dark patches. The exporter has had the flag
    // since AST-011C; the native path never set it, so no native model had a `uv1`.
    options.secondUv = true;
    // The native path has no texture baker, so the `.xob`'s own material paths are
    // written and the models draw untextured until the material side lands.
    Fmt::MLOD::WriteModel model;
    Enf::ConvertStats stats;
    const MountMaterialSink sink(mount);
    if (!Enf::ConvertXob(bytes, options, &sink, xobPath, model, stats, why))
        return nullptr;

    // RFG-040: does the material path survive the trip? A section with no surface
    // material cannot reach `_materialBindings`, and that is the table the renderer
    // resolves a NORMAL MAP out of -- so "no normals on Reforger models" and "sections
    // report mat '(none)'" are one question, asked at two ends of the same chain. Count
    // it at the point the model leaves the converter, before MLOD can be blamed.
    {
        static int reportedMat = 0;
        if (reportedMat < 3 && !model.lods.empty())
        {
            ++reportedMat;
            size_t withMaterial = 0, withTexture = 0;
            for (const auto& face : model.lods[0].faces)
            {
                withMaterial += face.material.empty() ? 0 : 1;
                withTexture += face.texture.empty() ? 0 : 1;
            }
            LOG_INFO(World, "Enfusion native load: converter gave '{}' {} faces, {} with a material, {} with a texture",
                     xobPath, model.lods[0].faces.size(), withMaterial, withTexture);
        }
    }

    const std::vector<char> buffer = Fmt::MLODWriter::writeToBuffer(model);
    if (buffer.empty())
    {
        why = "MLOD writer produced nothing";
        return nullptr;
    }
    auto parsed = std::make_shared<Poseidon::Model::Model>(
        Fmt::MLODLoader::loadFromBuffer(buffer.data(), static_cast<int>(buffer.size()), xobPath));
    if (parsed->lodLevels.empty())
    {
        why = "MLOD loader produced no levels";
        return nullptr;
    }
    return parsed;
}

LODShapeWithShadow* BuildShapeFromXob(const EnfusionMount& mount, const std::string& xobPath, std::string& why)
{
    std::shared_ptr<Poseidon::Model::Model> parsed = BuildModelFromXob(mount, xobPath, why);
    if (!parsed)
        return nullptr;
    // The name is only a cache key here -- no file is opened, because the parsed
    // model is handed in.
    Poseidon::ShapeBank::WorldModelScope worldModel(Poseidon::Shapes); // RFG-099
    LODShapeWithShadow* shape = Poseidon::Shapes.NewFromModel(xobPath.c_str(), false, true, parsed, nullptr);
    if (shape == nullptr)
    {
        why = "ShapeBank refused the model";
        return shape;
    }
    // RFG-023 replaced an earlier attempt here: building a merged texture and
    // swapping it onto the shape with Shape::RegisterTexture. That does NOT work --
    // measured, the section keeps pointing at the entry it had ("same pointer: yes")
    // -- so the merge moved into the texture source, named through `enfa|`, where
    // the engine's own path uploads and classifies it.

    // What the DRAW path will see per section: its material and its alpha class.
    // The engine routes alpha by material family, not by texture histogram (see
    // ShapeDraw's own note), so "no material" and "wrong class" are the two answers
    // and they are not the same bug.
    {
        static int reportedSec = 0;
        // RFG-073: the section report was pinned to "betula" while the leaf cutout was
        // being chased, and stayed pinned. Every building question since then -- which
        // material a church wall actually carries, whether a section merged into a decal
        // -- ran into "the church never appears in any section line" and was read as
        // evidence about the church. It was evidence about this filter.
        // POSEIDON_REFORGER_LOG_SECTIONS=<substring> chooses the model; unset keeps the
        // old default so the log does not grow by 1,248 models.
        static const std::string sectionFilter = []
        {
            const char* v = std::getenv("POSEIDON_REFORGER_LOG_SECTIONS");
            return LowerCopy((v != nullptr && *v != 0) ? v : "betula");
        }();
        if (reportedSec < 6 && LowerCopy(xobPath).find(sectionFilter) != std::string::npos && shape->NLevels() > 0)
        {
            ++reportedSec;
            const Shape* level = shape->Level(0);
            std::string report;
            for (int i = 0; level != nullptr && i < level->NSections() && i < 8; ++i)
            {
                const ShapeSection& sec = level->GetSection(i);
                const char* mat = sec.surfMat.IsNull() ? nullptr : sec.surfMat->GetName().Data();
                Texture* tex = sec.properties.GetTexture();
                report += (report.empty() ? "" : " | ") + std::to_string(i) + ": mat '" +
                          (mat != nullptr && *mat != 0 ? mat : "(none)") + "' tex '" +
                          (tex != nullptr && tex->Name() != nullptr ? tex->Name() : "(none)") + "'";
                // Deliberately NOT GetAlphaClass() here: asking before the texture has
                // a source used to pin it to Opaque forever (RFG-024), and a
                // diagnostic that changes what it measures is worse than none.
            }
            LOG_INFO(World, "Enfusion native load: sections of '{}' -- {}", xobPath, report);
        }
    }

    // The LOD ladder as the engine sees it. A tree that draws as a flat card at
    // forty metres is either a cutout that did not punch or the WRONG LEVEL, and
    // those look identical from outside.
    {
        static int reportedLod = 0;
        if (reportedLod < 4 && LowerCopy(xobPath).find("betula") != std::string::npos)
        {
            ++reportedLod;
            std::string ladder;
            for (int i = 0; i < shape->NLevels(); ++i)
            {
                const Shape* lvl = shape->Level(i);
                ladder += (ladder.empty() ? "" : ", ") + std::to_string(i) + ": res " +
                          std::to_string(shape->Resolution(i)) + " tris " +
                          std::to_string(lvl != nullptr ? lvl->NFaces() : -1);
            }
            LOG_INFO(World, "Enfusion native load: LOD ladder for '{}' -- {}", xobPath, ladder);
        }
    }

    // RFG-038: every distinct texture a built shape actually binds, counted once.
    // "Many buildings are white and untextured" cannot be checked against the
    // untextured-section warning -- that fires only when a section binds NOTHING, and
    // all 166 of those were roads. A wrong texture is bound, not absent, so the list
    // of what IS bound is the only thing that can name it.
    if (std::getenv("POSEIDON_REFORGER_TEXTURE_CENSUS") != nullptr && shape->NLevels() > 0)
    {
        static std::map<std::string, size_t> census;
        static size_t shapesSeen = 0;
        const Shape* level = shape->Level(0);
        for (int i = 0; level != nullptr && i < level->NSections(); ++i)
        {
            Texture* tex = level->GetSection(i).properties.GetTexture();
            ++census[tex != nullptr && tex->Name() != nullptr ? tex->Name() : "(none)"];
        }
        if (++shapesSeen % 200 == 0)
        {
            std::vector<std::pair<size_t, std::string>> ranked;
            for (const auto& [path, count] : census)
                ranked.emplace_back(count, path);
            std::sort(ranked.begin(), ranked.end(), std::greater<>());
            for (size_t i = 0; i < ranked.size() && i < 40; ++i)
                LOG_INFO(World, "Enfusion texture census: {}x '{}'", ranked[i].first, ranked[i].second);
        }
    }

    // What the model asks the renderer for, said once. "The trees are white" has two
    // very different causes -- the faces name nothing, or they name something that
    // will not open -- and they want opposite fixes.
    static int reported = 0;
    if (reported < 3 && shape->NLevels() > 0)
    {
        ++reported;
        const Shape* level = shape->Level(0);
        std::string first;
        if (level != nullptr && level->NTextures() > 0 && level->GetTexture(0) != nullptr)
            first = level->GetTexture(0)->Name();
        LOG_INFO(World, "Enfusion native load: '{}' level 0 has {} sections, {} textures, first '{}'", xobPath,
                 level != nullptr ? level->NSections() : -1, level != nullptr ? level->NTextures() : -1,
                 first.empty() ? "(none)" : first);
    }
    return shape;
}
} // namespace

namespace
{
//! RFG-097: the ModelCache's route to a native `.xob` by name (see ModelCache::SetExternalLoader).
std::shared_ptr<Poseidon::Model::Model> LoadXobModelForCache(const std::string& path, std::string& why)
{
    const EnfusionMount& mount = EnfusionMount::Instance();
    if (!mount.IsOpen())
    {
        why = "Enfusion archives are not mounted";
        return nullptr;
    }
    return BuildModelFromXob(mount, path, why);
}
} // namespace

bool Landscape::LoadEnfusionNative(const char* addonsDir, const char* worldDirectory, bool localPreview)
{
    // RFG-097: from here on a `.xob` name is loadable through the ShapeBank like any
    // other model name, which is what lets the object stream bring native placements
    // into range on its own.
    const auto& cliWorld = Poseidon::Foundation::AppConfig::Instance().GetReforgerWorld();
    const char* envWorld = std::getenv("POSEIDON_REFORGER_WORLD");
    selectedNativeWorld = worldDirectory && *worldDirectory ? worldDirectory :
        !cliWorld.empty() ? cliWorld : envWorld && *envWorld ? envWorld : "worlds/eden";
    ModelCache::SetExternalLoader(&LoadXobModelForCache);
    if (addonsDir == nullptr || *addonsDir == 0)
        return false;

    const DWORD start = Poseidon::Foundation::GlobalTickCount();
    size_t streamedTerrainRelativeTotal = 0; // RFG-097, reported at the end of the load
    float  maxBoundingCentreTotal = 0.0f;
    // RFG-066: a second load must not report the first one's materials.
    GColourCensus = ColourCensus();
    const EnfusionTerrain terrain = AssembleTerrain(addonsDir);
    if (!terrain.error.empty())
    {
        LOG_WARN(World, "Enfusion native load: {}", terrain.error);
        return false;
    }

    const TerrainDescriptor& d = terrain.descriptor;
    const float extent = d.WorldWidth();
    LOG_INFO(World,
             "Enfusion native load: '{}' {}x{} samples at {:.2f} m ({:.0f} m), {} tiles ({} failed), "
             "height = raw * {} + {}, assembled in {} ms",
             WantedWorld(), d.gridWidth, d.gridHeight, d.cellSize, extent, terrain.tilesFound, terrain.tilesFailed,
             d.heightScale, d.heightOffset, Poseidon::Foundation::GlobalTickCount() - start);

    // The engine's grids are powers of two. 2048 terrain cells over 12.8 km is 6.25 m,
    // matching what the converting exporter emits, so a native frame and a converted
    // one are the same resolution and can be compared directly.
    const int terrainRange = 2048;
    // RFG-009: 1024 land cells, not 256. The surface index is ONE material per land
    // cell, so a 50 m cell paints fifty metres of ground with whichever material sat
    // under its centre -- visible as square patches from the air. 12.5 m is four times
    // finer in each axis and still an exact divisor of the terrain grid.
    // POSEIDON_REFORGER_LAND_CELLS overrides it for an A/B.
    static const int landRange = []
    {
        const char* v = std::getenv("POSEIDON_REFORGER_LAND_CELLS");
        const int wanted = (v != nullptr && *v != 0) ? std::atoi(v) : 1024;
        // Must divide the terrain grid exactly and be a power of two.
        for (int candidate : {2048, 1024, 512, 256, 128})
            if (wanted >= candidate)
                return candidate;
        return 256;
    }();
    const float terrainGrid = extent / static_cast<float>(terrainRange);
    const float landGrid = extent / static_cast<float>(landRange);

    ProgressReset();
    ProgressStart(RString("Loading world"));
    Init();
    // No tide, for the same reason every other foreign world switches it off: these
    // coasts are authored against a sea at exactly zero.
    _tidal = false;
    Dim(landRange, landRange, terrainRange, terrainRange, landGrid);
    FlushCache();

    // Point-sampled, not filtered. A bilinear resample would look smoother and would
    // also invent heights the source does not have; for a first native frame the
    // question is whether the SHAPE arrives, and point sampling answers it without a
    // second thing to be wrong about.
    const uint32_t gridW = d.gridWidth;
    const uint32_t gridH = d.gridHeight;
    for (int z = 0; z < terrainRange; ++z)
    {
        const uint32_t srcZ = std::min<uint32_t>(
            gridH - 1, static_cast<uint32_t>(static_cast<float>(z) * static_cast<float>(gridH - 1) /
                                             static_cast<float>(terrainRange)));
        for (int x = 0; x < terrainRange; ++x)
        {
            const uint32_t srcX = std::min<uint32_t>(
                gridW - 1, static_cast<uint32_t>(static_cast<float>(x) * static_cast<float>(gridW - 1) /
                                                 static_cast<float>(terrainRange)));
            const uint16_t raw = terrain.heights[static_cast<size_t>(srcZ) * gridW + srcX];
            SetData(x, z, static_cast<float>(raw) * d.heightScale + d.heightOffset);
        }
    }
    ProgressRefresh();

    // Geography is derived, not read: Enfusion carries none of OFP's per-cell AI
    // hints. InitGeography() below computes what it can from the heightfield, which
    // is gradient and water; the forest and road bits stay zero, and AI pathing on a
    // natively loaded world is therefore worse than on a converted one. Named rather
    // than hidden.
    // RFG-006: the surface index per land cell, point-sampled at the cell centre out
    // of the 1 m TMAT mask. A land cell is 50 m, so this picks one of 2,500 authored
    // square metres and calls it the cell -- a reduction the single-index terrain path
    // forces, and the same one the converting exporter makes.
    std::vector<int> histogram(d.materials.size() + 1, 0);
    for (int z = 0; z < landRange; ++z)
    {
        for (int x = 0; x < landRange; ++x)
        {
            _geography(x, z) = GeographyInfo{};
            int index = 0;
            if (terrain.materialEdge > 0)
            {
                const uint32_t mx = std::min<uint32_t>(
                    terrain.materialEdge - 1,
                    static_cast<uint32_t>((static_cast<float>(x) + 0.5f) *
                                          static_cast<float>(terrain.materialEdge) /
                                          static_cast<float>(landRange)));
                const uint32_t mz = std::min<uint32_t>(
                    terrain.materialEdge - 1,
                    static_cast<uint32_t>((static_cast<float>(z) + 0.5f) *
                                          static_cast<float>(terrain.materialEdge) /
                                          static_cast<float>(landRange)));
                const uint8_t sample = terrain.materials[static_cast<size_t>(mz) * terrain.materialEdge + mx];
                if (sample != 0xFF && sample < d.materials.size())
                    index = sample;
            }
            _tex(x, z) = static_cast<short>(index);
            if (static_cast<size_t>(index) < histogram.size())
                ++histogram[index];
        }
    }

    // The palette itself. The names resolve to nothing yet -- they are `.emat` paths
    // INSIDE the archives, and the engine's file layer cannot open a `.pak` -- so the
    // ground still draws in the fallback. What is right now is the INDEX per cell,
    // which is what the next step needs and what a wrong palette read would ruin
    // silently: entry 0 of a TMAT palette is the lowest index present, not the base
    // layer, and taking it renders Everon as 41% dirt.
    const int paletteSize = std::max<int>(1, static_cast<int>(d.materials.size()));
    _texture.Resize(paletteSize);
    _a3TerrainMaterials.Resize(paletteSize);
    // RFG-062: the surface NAME per palette entry, which is what the renderer's clutter
    // binding hangs on. The palette already carries it -- it just never left this file.
    _enfusionSurfaces.Resize(paletteSize);
    int bound = 0;
    std::string firstFailure;
    for (int i = 0; i < paletteSize; ++i)
    {
        // Zeroed for EVERY entry including the failures: surfaceCount == 0 is what
        // selects the legacy single-texture branch in the terrain shader, and a
        // half-filled record flips the whole world onto the paged authored path,
        // which needs a real satellite and mask to look like anything.
        _a3TerrainMaterials[i] = A3TerrainMaterial{};
        // offsetUV has NO initialiser in TextureInfo, unlike its sibling array, so
        // this is a must-fill rather than a default. True = tile the layer; false
        // clamps one image onto each 50 m cell.
        _texture[i].offsetUV = true;
        if (static_cast<size_t>(i) >= d.materials.size())
            continue;
        _enfusionSurfaces[i] = RStringB(d.materials[i].c_str());
        std::string why;
        _texture[i].texture = ResolveSurfaceTexture(EnfusionMount::Instance(), d.materials[i], why);
        if (_texture[i].texture)
            ++bound;
        else if (firstFailure.empty())
            firstFailure = d.materials[i] + ": " + why;
    }

    {
        // Say what came out, with a denominator. "Surfaces loaded" is not a claim
        // anybody can check; "the four commonest of 22 cover 78% of the island" is.
        std::vector<std::pair<int, int>> ranked;
        for (size_t i = 0; i < histogram.size(); ++i)
            if (histogram[i] > 0)
                ranked.emplace_back(histogram[i], static_cast<int>(i));
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        const int cells = landRange * landRange;
        std::string top;
        for (size_t i = 0; i < ranked.size() && i < 4; ++i)
        {
            const std::string& name =
                static_cast<size_t>(ranked[i].second) < d.materials.size() ? d.materials[ranked[i].second] : std::string();
            top += (top.empty() ? "" : ", ") + std::to_string(ranked[i].second) + "=" +
                   std::to_string(ranked[i].first * 100 / cells) + "% " + name;
        }
        LOG_INFO(World, "Enfusion native load: {} of {} palette entries bound a colour{}", bound, paletteSize,
                 firstFailure.empty() ? std::string() : ("; first failure: " + firstFailure));
        LOG_INFO(World,
                 "Enfusion native load: surfaces {} of {} palette entries used over {} land cells "
                 "({} sub-cells, {} failed{}); commonest: {}",
                 ranked.size(), d.materials.size(), cells, terrain.subCells, terrain.subCellsFailed,
                 terrain.materialError.empty() ? "" : (", first error: " + terrain.materialError), top);
    }

    _mountains.Resize(0);

    // RFG-011: what the world places. Off unless asked for while the mesh path is
    // still being built -- resolving 1.23 M placements costs seconds and draws
    // nothing yet, so it would be a tax on every load for a log line.
    if (const char* wantObjects = std::getenv("POSEIDON_REFORGER_OBJECTS");
        (wantObjects && *wantObjects) ? wantObjects[0] == '1' : localPreview)
    {
        const DWORD placeStart = Poseidon::Foundation::GlobalTickCount();
        std::vector<std::pair<std::string, size_t>> meshes;
        std::vector<ResolvedPlacement> placed;
        std::vector<ResolvedRoad> roads;
        std::vector<std::pair<std::string, size_t>> attachMeshes;
        std::vector<AttachSet> attachSets;
        const PlacementSummary summary = ResolvePlacements(EnfusionMount::Instance(), addonsDir, meshes, placed,
                                                           roads, attachMeshes, attachSets);
        if (!summary.error.empty())
        {
            LOG_WARN(World, "Enfusion native load: placements unavailable: {}", summary.error);
        }
        else
        {
            std::string top;
            for (size_t i = 0; i < meshes.size() && i < 5; ++i)
                top += (top.empty() ? "" : ", ") + std::to_string(meshes[i].second) + "x " + meshes[i].first;
            LOG_INFO(World,
                     "Enfusion native load: {} placements, {} with a prefab GUID, {} resolved to {} distinct "
                     "meshes ({} terrain-relative) in {} ms{}; commonest: {}",
                     summary.placements, summary.withGuid, summary.meshResolved, summary.distinctMeshes,
                     summary.terrainRelative, Poseidon::Foundation::GlobalTickCount() - placeStart,
                     summary.firstFailure.empty() ? "" : ("; first failure: " + summary.firstFailure), top);

            // RFG-013: build the meshes and put their placements in the world.
            //
            // The caps default to "everything" -- Everon's 1,248 distinct meshes and
            // its full 1.23 M placements -- because a cap that hides the load is a cap
            // that hides what needs fixing. At 120 models the top of the histogram is
            // entirely vegetation and rock, so a village came out as an empty field
            // and the missing houses looked like a placement bug rather than a cap.
            // Both remain overridable for a cheap run.
            const size_t modelCap = [localPreview]
            {
                const char* v = std::getenv("POSEIDON_REFORGER_MAX_MODELS");
                return static_cast<size_t>((v != nullptr && *v != 0) ? std::atoi(v) : (localPreview ? 800 : 100000));
            }();
            const size_t objectCap = [localPreview]
            {
                const char* v = std::getenv("POSEIDON_REFORGER_MAX_OBJECTS");
                return static_cast<size_t>((v != nullptr && *v != 0) ? std::atoi(v) : (localPreview ? 400000 : 2000000));
            }();

            // RFG-032: a RADIUS around the viewer decides what is built, not a global
            // ranking. Everything inside it is built regardless of how rare its mesh
            // is; outside it, the interleaved ranking below fills whatever budget is
            // left, so distant landmarks still appear.
            //
            // This is the right shape for the problem. A global "top N meshes" budget
            // is a statement about the whole island and says nothing about what is in
            // front of the player: at 120 models a village came out as roads and empty
            // lots, because a house type with 30 placements loses to a rock type with
            // 11,000 no matter how close the house is. Distance is the thing the
            // player can actually see.
            //
            // The focus point is the free-fly camera when there is one -- that is
            // where a capture looks -- and the map centre otherwise.
            float focusX = 0.5f * static_cast<float>(gridW) * d.cellSize;
            float focusZ = 0.5f * static_cast<float>(gridH) * d.cellSize;
            {
                const std::vector<float>& freefly =
                    Poseidon::Foundation::AppConfig::Instance().GetTestWorldFreeFly();
                if (freefly.size() >= 2)
                {
                    focusX = freefly[0];
                    focusZ = freefly[1];
                }
            }
            const float nearRadius = [localPreview]
            {
                const char* v = std::getenv("POSEIDON_REFORGER_NEAR_RADIUS");
                return (v != nullptr && *v != 0) ? static_cast<float>(std::atof(v)) : (localPreview ? 300.0f : 800.0f);
            }();

            const auto isStructure = [](const std::string& path)
            {
                const std::string lower = LowerCopy(path);
                return lower.find("/structures/") != std::string::npos ||
                       lower.find("/props/") != std::string::npos ||
                       lower.find("/military/") != std::string::npos;
            };

            std::vector<bool> meshIsStructure(meshes.size(), false);
            for (size_t i = 0; i < meshes.size(); ++i)
                meshIsStructure[i] = isStructure(meshes[i].first);

            std::vector<bool> meshIsNear(meshes.size(), false);
            size_t nearPlacements = 0;
            for (const ResolvedPlacement& item : placed)
            {
                const float dx = item.position[0] - focusX;
                const float dz = item.position[2] - focusZ;
                if (dx * dx + dz * dz > nearRadius * nearRadius)
                    continue;
                ++nearPlacements;
                if (item.mesh < meshIsNear.size())
                    meshIsNear[item.mesh] = true;
            }

            // RFG-031: rank by placement count, but INTERLEAVE structures with the
            // rest rather than letting one list decide the whole budget.
            //
            // Everon's histogram is 820,184 trees and 307,497 environmental props
            // against 7,507 destructible buildings, so a budget filled strictly by
            // count is vegetation and rock for its first several hundred entries and
            // the houses never get built at all. At a 120-model cap that produced a
            // village of roads, fences and empty lots -- which reads as a placement
            // bug and is not one. Every second slot now goes to a structure while any
            // structure is left unbuilt, so a town has houses at any budget that can
            // afford trees.
            std::vector<std::pair<size_t, uint32_t>> structures;
            std::vector<std::pair<size_t, uint32_t>> others;
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                auto& list = isStructure(meshes[i].first) ? structures : others;
                list.emplace_back(meshes[i].second, static_cast<uint32_t>(i));
            }
            std::sort(structures.begin(), structures.end(), std::greater<>());
            std::sort(others.begin(), others.end(), std::greater<>());

            std::vector<std::pair<size_t, uint32_t>> ranked;
            ranked.reserve(meshes.size());
            for (size_t a = 0, b = 0; a < structures.size() || b < others.size();)
            {
                if (a < structures.size())
                    ranked.push_back(structures[a++]);
                if (b < others.size())
                    ranked.push_back(others[b++]);
            }

            // Near first, in that order; then everything else.
            std::vector<std::pair<size_t, uint32_t>> byCount;
            byCount.reserve(ranked.size());
            size_t nearMeshes = 0;
            for (const auto& entry : ranked)
                if (entry.second < meshIsNear.size() && meshIsNear[entry.second])
                {
                    byCount.push_back(entry);
                    ++nearMeshes;
                }
            for (const auto& entry : ranked)
                if (!(entry.second < meshIsNear.size() && meshIsNear[entry.second]))
                    byCount.push_back(entry);

            // Furniture counted separately: "the houses are empty" is a claim about a
            // subtree of the asset tree, and the mesh histogram's top five will never
            // show it -- a village has thousands of trees and a handful of chairs.
            size_t furnitureMeshes = 0, furniturePlacements = 0;
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                const std::string lower = LowerCopy(meshes[i].first);
                if (lower.find("/furniture/") == std::string::npos &&
                    lower.find("/props/civilian/") == std::string::npos)
                    continue;
                ++furnitureMeshes;
                furniturePlacements += meshes[i].second;
            }
            LOG_INFO(World,
                     "Enfusion native load: {} placements and {} distinct meshes within {:.0f} m of [{:.0f} {:.0f}]; "
                     "furniture and civilian props: {} meshes, {} placements world-wide",
                     nearPlacements, nearMeshes, nearRadius, focusX, focusZ, furnitureMeshes, furniturePlacements);

            std::unordered_map<uint32_t, LODShapeWithShadow*> shapes;
            size_t built = 0;
            size_t buildFailed = 0;
            std::string firstBuildFailure;
            const DWORD buildStart = Poseidon::Foundation::GlobalTickCount();
            // RFG-097/099: under streaming NO shape is pre-built here. A shape built now
            // has no object yet when ShapeBank::OptimizeAll converts every level at the end
            // of the load, so each level became a private CPU/GPU copy -- 5944 buffers,
            // 863 MB, identical for every budget. The stream's cold path builds the shape
            // when the first placement needs it, after the renderer registered it.
            const bool streamNative = Poseidon::Dev::GResidencyLevers().nativeStreaming; // RFG-097/099, Streaming tab
            for (size_t i = 0; !streamNative && i < byCount.size() && built < modelCap; ++i)
            {
                const uint32_t index = byCount[i].second;
                std::string why;
                LODShapeWithShadow* shape = BuildShapeFromXob(EnfusionMount::Instance(), meshes[index].first, why);
                if (shape == nullptr)
                {
                    ++buildFailed;
                    if (firstBuildFailure.empty())
                        firstBuildFailure = meshes[index].first + ": " + why;
                    continue;
                }
                shapes.emplace(index, shape);
                ++built;
                // RFG-030: name the giants. A Reforger asset is a house at most, so
                // anything with a hundred-metre bounding sphere is a resolution bug,
                // not a big model -- and it is the one defect a screenshot cannot
                // attribute, because at that size the offender fills the frame and
                // stops looking like an object at all.
                if (shape->BoundingSphere() > 60.0f)
                    LOG_WARN(World, "Enfusion native load: OVERSIZED '{}' radius {:.0f} m, {} placements",
                             meshes[index].first, shape->BoundingSphere(), meshes[index].second);
            }
            // RFG-064: the eager path builds attachment meshes OUTSIDE the
            // model cap, so rare doors and windows do not lose to common trees.
            //
            // The cap ranks meshes by placement count, and a door or a window is a
            // rare mesh by construction -- Everon draws hundreds of distinct ones,
            // each on a handful of houses. Ranked against 820,184 trees they never
            // make the budget, so a cap of 350-450 models (the owner's usual run)
            // would build every house shell and not one of the openings, and the
            // repair would show as nothing at all. This eager path builds every
            // distinct part resolved for the world, including parts of shells
            // outside the cap. Optional streaming deferral retains every part's
            // model path and flattened placement below for ordinary cold admission.
            // Keep eager attachment shapes as the default until the candidate's
            // GPU slowdown is explained; startup/lifecycle tests alone do not
            // establish performance acceptance.
            const char* deferAttachments = std::getenv("WGR_NATIVE_DEFER_ATTACHMENT_SHAPES");
            const bool deferAttachmentShapes = streamNative && deferAttachments != nullptr &&
                                               deferAttachments[0] == '1' && deferAttachments[1] == '\0';
            if (streamNative)
                LOG_INFO(World, "Enfusion native load: attachment shape deferral {} (WGR_NATIVE_DEFER_ATTACHMENT_SHAPES=1)",
                         deferAttachmentShapes ? "enabled" : "disabled");
            std::unordered_map<uint32_t, LODShapeWithShadow*> attachShapes;
            size_t attachBuilt = 0, attachBuildFailed = 0;
            std::string firstAttachFailure;
            for (size_t i = 0; !deferAttachmentShapes && i < attachMeshes.size(); ++i)
            {
                std::string why;
                LODShapeWithShadow* shape =
                    BuildShapeFromXob(EnfusionMount::Instance(), attachMeshes[i].first, why);
                if (shape == nullptr)
                {
                    ++attachBuildFailed;
                    if (firstAttachFailure.empty())
                        firstAttachFailure = attachMeshes[i].first + ": " + why;
                    continue;
                }
                attachShapes.emplace(static_cast<uint32_t>(i), shape);
                ++attachBuilt;
            }
            const DWORD buildMs = Poseidon::Foundation::GlobalTickCount() - buildStart;

            // Near first here too: the object cap walks this list in order, so leaving
            // it in file order empties whatever region the file happened to write
            // last -- which is a different region every world and none of them the
            // one being looked at.
            std::stable_sort(placed.begin(), placed.end(),
                             [&](const ResolvedPlacement& a, const ResolvedPlacement& b)
                             {
                                 const float ax = a.position[0] - focusX, az = a.position[2] - focusZ;
                                 const float bx = b.position[0] - focusX, bz = b.position[2] - focusZ;
                                 return ax * ax + az * az < bx * bx + bz * bz;
                             });

            size_t added = 0;
            size_t attachPlaced = 0;
            size_t attachSkipped = 0;
            size_t buriedObjects = 0;
            size_t buriedStructures = 0;
            size_t rescuedObjects = 0;
            size_t floatingObjects = 0;
            std::string firstFloating;
            std::string firstBuried;
            std::string firstBuriedStructure;
            // RFG-097: STREAM the world instead of placing a radius of it.
            //
            // Until now every placement inside `nearRadius` of the start became a static
            // object at load and nothing outside it ever existed: fly 300 m and the terrain
            // is bare. The converted worlds never had that problem because the OPRW loader
            // hands its placements to the object stream (LandSave.cpp, `_modernObject*`),
            // which admits by distance from the camera, evicts what falls out of the window
            // and loads a model the first time a placement needs it. The native path now
            // does the same: every placement -- shell and attached door alike -- is a stream
            // placement carrying its `.xob` path, and the ShapeBank can load that path cold
            // through ModelCache's external loader (installed at the top of this function).
            // Budget, radius and window growth are the stream's own knobs
            // (WGR_OBJECT_STREAM_MAX_OBJECTS / _RADIUS_CELLS); POSEIDON_REFORGER_STREAM=0
            // restores the radius-at-load behaviour for an A/B.
            // ON by default since RFG-099 (the direct-path duplication is fixed; 24 ms/frame
            // streamed against 46 ms for the radius path at the church). The Streaming tab's
            // "Native Reforger worlds stream their objects" lever / POSEIDON_REFORGER_STREAM=0
            // restore the radius-at-load behaviour; streamNative is read above the shape build.
            size_t streamed = 0, streamedParts = 0;
            if (streamNative)
            {
                // Native model/placement tables can be rebuilt independently of near-stream
                // progress. Release an opt-in converted DayZ shape before either table changes.
                _modernPendingOwnerPrimaryTextures.reset();
                _modernObjectModels.clear();
                _modernObjectModels.reserve(meshes.size() + attachMeshes.size());
                for (const auto& mesh : meshes)
                    _modernObjectModels.emplace_back(mesh.first.c_str());
                for (const auto& mesh : attachMeshes)
                    _modernObjectModels.emplace_back(mesh.first.c_str());
                _modernRequiredObjects.clear();
                _modernLogicalOnlyObjects.clear();
                _modernObjectPlacements.clear();
                _modernObjectPlacements.reserve(placed.size() * 2);
                _modernObjectCells.assign(static_cast<size_t>(_landRange) * _landRange, {});
                _modernObjectCellActive.assign(_modernObjectCells.size(), 0);
                _modernObjectCellDesired.assign(_modernObjectCells.size(), 0);
                _modernObjectCellCursor.assign(_modernObjectCells.size(), 0);
                _modernObjectCandidates.clear();
                _modernObjectCandidatesValid = false;
                _modernObjectCandidateCenterX = -0x3fffffff;
                _modernObjectCandidateCenterZ = -0x3fffffff;
                _modernObjectCandidateRadius = 0;
                if (const char* radius = std::getenv("WGR_OBJECT_STREAM_RADIUS_CELLS"))
                    _modernObjectRadius = std::clamp(static_cast<int>(std::strtol(radius, nullptr, 10)), 8, _landRange);
                if (const char* budget = std::getenv("WGR_OBJECT_STREAM_MAX_OBJECTS"))
                    _modernObjectBudget =
                        static_cast<uint32_t>(std::clamp<long>(std::strtol(budget, nullptr, 10), 1000, 1000000));
                else
                    _modernObjectBudget = 20000;
                // The near shapes were built above (they warm the ShapeBank for the first
                // window); measure their bounding centres, because AddObject offsets an object
                // by its centre and ObjectCreate does not -- with the converter's autocenter=0
                // the two agree only if the centre is the origin.
                for (const auto& entry : shapes)
                    if (entry.second != nullptr)
                        maxBoundingCentreTotal = std::max(maxBoundingCentreTotal, entry.second->BoundingCenter().Size());
                _modernObjectStreaming = true;
                // RFG-103: arm the worker pre-parser for the native models. The OPRW path has
                // had it since the stream existed; the native branch never called it, so every
                // cold `.xob` parsed on the main thread (measured 226 ms parse + 220 ms adapt per
                // house: the hitches). The worker reads the pak (one ifstream per read) and runs
                // the pure converter; the adapt stays on the main thread (see
                // WGR_OBJECT_STREAM_ASYNC_ADAPT and the heap note).
                EnsureModernObjectPreparer();
                EnableGeographyAccumulation(); // RFG-101: streamed objects fill the geography
                _modernObjectResidencyPending = false;
                _modernObjectCenterX = -0x3fffffff;
                _modernObjectCenterZ = -0x3fffffff;
                _modernResidentObjectCount = 0;
            }
            const auto pushStreamPlacement = [&](uint32_t modelIndex, const Vector3& position, const Matrix3& orientation)
            {
                ModernObjectPlacement placement;
                placement.id = NewObjectID();
                placement.modelIndex = modelIndex;
                const Vector3 aside = orientation.DirectionAside();
                const Vector3 up = orientation.DirectionUp();
                const Vector3 dir = orientation.Direction();
                placement.rows = {aside.X(), aside.Y(), aside.Z(), up.X(), up.Y(), up.Z(),
                                  dir.X(),   dir.Y(),   dir.Z(),   position.X(), position.Y(), position.Z()};
                const int cx = std::clamp(static_cast<int>(std::floor(position.X() * _invLandGrid)), 0, _landRange - 1);
                const int cz = std::clamp(static_cast<int>(std::floor(position.Z() * _invLandGrid)), 0, _landRange - 1);
                const uint32_t index = static_cast<uint32_t>(_modernObjectPlacements.size());
                _modernObjectPlacements.push_back(std::move(placement));
                _modernObjectCells[static_cast<size_t>(cz) * _landRange + cx].push_back(index);
            };
            for (const ResolvedPlacement& item : placed)
            {
                if (streamNative)
                {
                    float y = item.position[1];
                    if (item.terrainRelative)
                    {
                        y += SurfaceY(item.position[0], item.position[2]);
                        ++streamedTerrainRelativeTotal;
                    }
                    const float groundHere = SurfaceY(item.position[0], item.position[2]);
                    // RFG-047, kept: a structure the coordinates put more than 10 m under the
                    // drawn surface stands on it instead.
                    if (!item.terrainRelative && y < groundHere - 10.0f && meshIsStructure[item.mesh])
                        y = groundHere + item.position[1];
                    // The same basis AddObject + the orientation block below build, so a
                    // streamed object stands exactly where the placed one did (RFG-068 signs).
                    Matrix3 orientation(MRotationY, item.yawDeg * (H_PI / 180));
                    if (std::fabs(item.pitchDeg) > 0.01f)
                        orientation = orientation * Matrix3(MRotationX, item.pitchDeg * 0.01745329252f);
                    if (std::fabs(item.rollDeg) > 0.01f)
                        orientation = orientation * Matrix3(MRotationZ, item.rollDeg * 0.01745329252f);
                    if (std::fabs(item.scale - 1.0f) > 1e-3f)
                        orientation = orientation * Matrix3(MScale, item.scale);
                    const Vector3 position(item.position[0], y, item.position[2]);
                    pushStreamPlacement(item.mesh, position, orientation);
                    ++streamed;
                    if (item.attachSet != kNoAttachSet)
                    {
                        for (const MeshAttachment& part : attachSets[item.attachSet].parts)
                        {
                            pushStreamPlacement(static_cast<uint32_t>(meshes.size() + part.mesh),
                                                position + orientation * part.offset, orientation * part.basis);
                            ++streamedParts;
                        }
                    }
                    continue;
                }
                if (added >= objectCap)
                    break;
                const auto shape = shapes.find(item.mesh);
                if (shape == shapes.end())
                    continue;
                // Enfusion's X/Z are the engine's X/Z; index 1 is up in both.
                float y = item.position[1];
                if (item.terrainRelative)
                {
                    // RFG-051: against the surface that is DRAWN, not the one the file
                    // was authored on.
                    //
                    // This used the raw 2 m Enfusion heightfield, on the reasoning that
                    // the offsets were authored against it and our 12.5 m resample is a
                    // different surface. True, and exactly backwards for rendering: an
                    // object has to stand on the ground the player can see. On flat land
                    // the two agree within centimetres and the choice does not show. At
                    // a cliff they differ by the height of the cliff -- and Everon's
                    // coast came out with a curtain of boulders hanging a hundred metres
                    // up in the air, tracing the clifftop exactly, because each one sat
                    // correctly on a fine-grid surface that is nowhere near the coarse
                    // one underneath it.
                    y += SurfaceY(item.position[0], item.position[2]);
                }
                // RFG-047: rescue a placement that would land absurdly deep.
                //
                // The Y rule reads a flagless `coords` Y as terrain-relative only when
                // it is within 2 m of zero (RFG-011's measurement: flagless placements
                // have median Y 0). A flagless part with Y = 9 -- a loudspeaker on a
                // pole, a chimney, a gutter -- falls outside that and is read as an
                // absolute 9 m above sea level, which on a 117 m hill puts it 108 m
                // underground. Measured: 265 structures buried, the deepest by exactly
                // that mechanism.
                //
                // Ten metres, not two: a rock or a pipe embedded a couple of metres
                // into the ground is authored that way and must stay. Nothing is
                // deliberately ten metres under Everon, so below that the absolute
                // reading is simply wrong and the offset reading is the one that puts
                // the part back on its building.
                const float groundHere = SurfaceY(item.position[0], item.position[2]);
                // Structures ONLY, and that restriction was learned the expensive way.
                //
                // Applied to everything, the rescue lifted Everon's coastal boulders into
                // the sky: a beach rock sits at an absolute Y of a couple of metres, and
                // where it stands at the foot of a cliff the 12.5 m terrain sample reads
                // the CLIFF TOP, a hundred metres up. The rock then looks "buried" and is
                // rescued to ground + 2 -- a hundred metres above where it belongs, and
                // clearly visible as rocks hanging in mid-air over the coast road.
                //
                // The mechanism this repairs only ever applied to structures: a building
                // part whose Y is an offset within its parent. Rocks and vegetation are
                // placed absolutely and must be left alone, however wrong the coarse
                // terrain says they look.
                if (!item.terrainRelative && y < groundHere - 10.0f && meshIsStructure[item.mesh])
                {
                    y = groundHere + item.position[1];
                    ++rescuedObjects;
                }
                Object* placedObject =
                    AddObject(Vector3(item.position[0], y, item.position[2]), item.yawDeg, shape->second);
                // `AddObject` has no scale parameter, and the engine keeps scale in the
                // orientation matrix rather than beside it -- `FrameBase::SetOrientation`
                // recovers `_scale` from the matrix it is handed. So the scale goes on
                // through the orientation, which is also why it must be applied AFTER
                // the object exists rather than folded into the call.
                if (placedObject != nullptr)
                {
                    // Yaw is already in the orientation AddObject built, so only the two
                    // it has no parameter for are applied here, and they are applied in
                    // the object's OWN frame -- right-multiplied -- so a tilt stays a
                    // tilt of the piece rather than becoming a swing about the world.
                    Matrix3 orientation = placedObject->Orientation();
                    if (std::fabs(item.pitchDeg) > 0.01f)
                        orientation = orientation * Matrix3(MRotationX, item.pitchDeg * 0.01745329252f);
                    if (std::fabs(item.rollDeg) > 0.01f)
                        orientation = orientation * Matrix3(MRotationZ, item.rollDeg * 0.01745329252f);
                    if (std::fabs(item.scale - 1.0f) > 1e-3f)
                        orientation = orientation * Matrix3(MScale, item.scale);
                    placedObject->SetOrientation(orientation);

                    // RFG-064: the doors, gates and windows this building hangs on
                    // itself. `orientation` is the FINISHED house frame -- yaw, tilt
                    // and scale -- so a part rides all three, which is what makes it
                    // stay in its opening on a house standing on a slope.
                    //
                    // Not counted against `objectCap`: that budget exists to bound
                    // how much of the island is built, and a door is not a separate
                    // decision from the house it is in. Halving a house is worse than
                    // exceeding the cap by the number of its openings.
                    if (item.attachSet != kNoAttachSet)
                    {
                        for (const MeshAttachment& part : attachSets[item.attachSet].parts)
                        {
                            const auto partShape = attachShapes.find(part.mesh);
                            if (partShape == attachShapes.end())
                            {
                                ++attachSkipped;
                                continue;
                            }
                            const Vector3 here = Vector3(item.position[0], y, item.position[2]) +
                                                 orientation * part.offset;
                            Object* attached = AddObject(here, item.yawDeg, partShape->second);
                            if (attached == nullptr)
                            {
                                ++attachSkipped;
                                continue;
                            }
                            attached->SetOrientation(orientation * part.basis);
                            ++attachPlaced;
                        }
                    }
                }
                // RFG-035: count what ends up under the ground. "The buildings are
                // missing" and "the buildings are buried" look identical from the
                // air and have different causes, and nothing so far could tell them
                // apart. Measured against the engine's own surface, which is what the
                // player stands on.
                // RFG-052: the mirror of the buried counter. Everon's coast shows a
                // curtain of boulders hanging in mid-air, and three plausible causes were
                // each ruled out by a capture that looked identical -- the rescue rule,
                // the fine-vs-coarse height source, and the land grid resolution. None of
                // them moved a pixel. So the objects say what they are instead of being
                // guessed at: a counter cannot be argued with the way a screenshot can.
                if (y > groundHere + 50.0f)
                {
                    ++floatingObjects;
                    if (firstFloating.empty())
                        firstFloating = meshes[item.mesh].first + " at [" +
                                        std::to_string(int(item.position[0])) + " " +
                                        std::to_string(int(item.position[2])) + "] " +
                                        std::to_string(int(y - groundHere)) + " m up, coordsY " +
                                        std::to_string(int(item.position[1])) +
                                        (item.terrainRelative ? " (terrain-relative)" : " (absolute)");
                }
                if (y < groundHere - 2.0f)
                {
                    ++buriedObjects;
                    // Split by category, because the two halves mean opposite things.
                    // A rock or a pipe sunk into the ground is authored that way; a
                    // HOUSE two metres under is a height-rule bug. A single total
                    // cannot tell them apart and so cannot be acted on.
                    {
                        const std::string lower = LowerCopy(meshes[item.mesh].first);
                        if (lower.find("/structures/") != std::string::npos ||
                            lower.find("/houses/") != std::string::npos)
                        {
                            ++buriedStructures;
                            if (firstBuriedStructure.empty())
                                firstBuriedStructure = meshes[item.mesh].first + " at [" +
                                                       std::to_string(int(item.position[0])) + " " +
                                                       std::to_string(int(item.position[2])) + "] " +
                                                       std::to_string(int(groundHere - y)) + " m under";
                        }
                    }
                    if (firstBuried.empty())
                        firstBuried = meshes[item.mesh].first + " at [" + std::to_string(int(item.position[0])) +
                                      " " + std::to_string(int(item.position[2])) + "] " +
                                      std::to_string(int(SurfaceY(item.position[0], item.position[2]) - y)) +
                                      " m under";
                }
                ++added;
            }
            // Where the towns are. A world this size has no obvious place to point a
            // camera, and "fly around until you find a house" is not a method.
            {
                int shown = 0;
                for (const ResolvedPlacement& item : placed)
                {
                    if (shown >= 4)
                        break;
                    if (static_cast<size_t>(item.mesh) >= meshes.size())
                        continue;
                    const std::string& path = meshes[item.mesh].first;
                    const std::string lower = LowerCopy(path);
                    // Houses, not harbour furniture: piers and signs are "structures"
                    // too and there are thousands of them along the coast.
                    if (lower.find("structures/residential") == std::string::npos &&
                        lower.find("/house") == std::string::npos && lower.find("village") == std::string::npos)
                        continue;
                    ++shown;
                    LOG_INFO(World, "Enfusion native load: building at [{:.0f} {:.0f} {:.0f}] {}",
                             item.position[0], item.position[1], item.position[2], path);
                }
                // The same locator for interior dressing: "are the houses furnished"
                // is answered by standing in front of one, and finding one by eye
                // costs a run per guess.
                size_t shownProps = 0;
                for (const ResolvedPlacement& item : placed)
                {
                    if (shownProps >= 6)
                        break;
                    if (static_cast<size_t>(item.mesh) >= meshes.size())
                        continue;
                    const std::string lower = LowerCopy(meshes[item.mesh].first);
                    if (lower.find("/furniture/") == std::string::npos)
                        continue;
                    ++shownProps;
                    LOG_INFO(World, "Enfusion native load: furniture at [{:.0f} {:.0f} {:.0f}] {}",
                             item.position[0], item.position[1], item.position[2], meshes[item.mesh].first);
                }
            }
            LOG_INFO(World,
                     "Enfusion native load: built {} of {} meshes ({} failed) in {} ms, placed {} objects "
                     "({} more than 2 m under the terrain, {} of them structures, {} rescued from below 10 m){}{}",
                     built, meshes.size(), buildFailed, buildMs, added, buriedObjects, buriedStructures,
                     rescuedObjects,
                     firstBuriedStructure.empty() ? (firstBuried.empty() ? "" : ("; first: " + firstBuried))
                                                  : ("; first structure: " + firstBuriedStructure),
                     firstBuildFailure.empty() ? "" : ("; first build failure: " + firstBuildFailure));

            // RFG-064: the one line that says whether the houses got their doors
            // back. Everything else about this feature is invisible from the air --
            // a house with no door and a house with a door look the same from 200 m
            // and identical in the placement histogram -- so the numbers say it
            // instead: how many buildings declared openings, how many bones matched a
            // part, how many parts were actually put in the world, and how many were
            // lost and to what.
            LOG_INFO(World,
                     "Enfusion native load: attachments: {} prefabs declared slots over {} socket bones "
                     "({} unresolved), {} distinct part meshes ({} built, {} failed); {} parts placed on {} "
                     "buildings, {} skipped{}{}",
                     summary.slotPrefabs, summary.slotBones, summary.slotUnresolved, attachMeshes.size(),
                     attachBuilt, attachBuildFailed, attachPlaced, summary.slotBuildings, attachSkipped,
                     summary.slotFailure.empty() ? "" : ("; first slot failure: " + summary.slotFailure),
                     firstAttachFailure.empty() ? "" : ("; first build failure: " + firstAttachFailure));

            // RFG-027: the road network. One shape per road rather than one shared
            // shape placed many times, because every road is a different length and
            // a different curve -- there is nothing to instance.
            if (std::getenv("POSEIDON_REFORGER_NO_ROADS") == nullptr)
            {
                const DWORD roadStart = Poseidon::Foundation::GlobalTickCount();
                size_t roadsBuilt = 0, roadsFailed = 0, roadTextured = 0;
                std::map<std::string, size_t> textureUse;
                double roadKm = 0.0;
                // The land cells under the tarmac, for the clutter bake. A road is a
                // draped ribbon, not a surface, so the 12.5 m cell beneath it keeps its
                // grass index and would grow clutter through the road; Enfusion's own
                // road generator punches the clutter out under the carriageway (the
                // `cs_clutter_prepare$HOLES` permutation in the corpus). One byte per
                // cell, marked below as each ribbon is built.
                _enfusionRoadCells.assign(static_cast<size_t>(_landRange) * static_cast<size_t>(_landRange), 0);
                size_t roadCells = 0;
                for (size_t r = 0; r < roads.size(); ++r)
                {
                    ResolvedRoad& road = roads[r];
                    if (road.points.size() < 6)
                        continue;

                    // Subdivide before draping. Everon's centrelines average one point
                    // every 16 m (26,271 points over 419.8 km) and run much longer than
                    // that on a straight, so a segment draped only at its ends cuts
                    // clean across the terrain between them: half the road buried, the
                    // other half standing on a black embankment with visible sides.
                    // That is exactly what the first draped capture showed, and it is
                    // a sampling artefact rather than a height-rule one -- the rule was
                    // right, the ribbon simply had nowhere to bend.
                    {
                        std::vector<float> dense;
                        dense.reserve(road.points.size() * 4);
                        const size_t source = road.points.size() / 3;
                        for (size_t i = 0; i + 1 < source; ++i)
                        {
                            const float ax = road.points[i * 3 + 0];
                            const float ay = road.points[i * 3 + 1];
                            const float az = road.points[i * 3 + 2];
                            const float bx = road.points[(i + 1) * 3 + 0];
                            const float by = road.points[(i + 1) * 3 + 1];
                            const float bz = road.points[(i + 1) * 3 + 2];
                            const float length = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
                            // 4 m: comfortably under the 12.5 m land cell, so every
                            // cell the road crosses gets at least two samples.
                            const int steps = std::max(1, static_cast<int>(length / 4.0f));
                            for (int k = 0; k < steps; ++k)
                            {
                                const float t = static_cast<float>(k) / static_cast<float>(steps);
                                dense.push_back(ax + (bx - ax) * t);
                                dense.push_back(ay + (by - ay) * t);
                                dense.push_back(az + (bz - az) * t);
                            }
                        }
                        dense.push_back(road.points[(source - 1) * 3 + 0]);
                        dense.push_back(road.points[(source - 1) * 3 + 1]);
                        dense.push_back(road.points[(source - 1) * 3 + 2]);
                        road.points = std::move(dense);
                    }

                    const size_t pointCount = road.points.size() / 3;
                    if (pointCount < 2)
                        continue;

                    // RFG-056: rotate into world space FIRST, then drape, then place with
                    // no rotation of its own.
                    //
                    // The height was being sampled at the UNROTATED point and the ribbon
                    // drawn at the rotated one, so every road with a yaw was fitted to the
                    // ground somewhere it does not run. On flat ground that is invisible;
                    // on anything else the tarmac hangs in the air or sinks, and it is
                    // visibly turned against the terrain it is supposed to follow.
                    //
                    // Rotating the points removes the whole class of error rather than
                    // patching the sample position: after this there is only one place the
                    // road can be, and the drape and the draw agree by construction.
                    if (std::fabs(road.yawDeg) > 0.01f)
                    {
                        const float yaw = road.yawDeg * 0.01745329252f;
                        const float c = std::cos(yaw);
                        const float sn = std::sin(yaw);
                        for (size_t i = 0; i < pointCount; ++i)
                        {
                            const float lx = road.points[i * 3 + 0];
                            const float lz = road.points[i * 3 + 2];
                            // The same basis Landscape::AddObject builds from a heading:
                            // aside = (c, 0, -s), dir = (s, 0, c).
                            road.points[i * 3 + 0] = c * lx + sn * lz;
                            road.points[i * 3 + 2] = -sn * lx + c * lz;
                        }
                        road.yawDeg = 0.0f;
                    }

                    // The height rule: DRAPE on the engine's own terrain.
                    //
                    // The spline's own Y is the authored profile, smooth and correct
                    // against the 2 m heightfield Enfusion cuts to match it. Our grid
                    // is resampled from that to 12.5 m, so the two disagree by metres
                    // on a slope. Taking the higher of the two was tried first and is
                    // visibly worse than draping: on a hillside it builds the road as
                    // a black causeway standing a couple of metres proud of the
                    // ground, with its own side walls, which reads as a wall rather
                    // than as a road. Draping costs the smooth profile -- the road
                    // now follows every 12.5 m step of the coarse terrain -- and that
                    // is the cheaper defect until the terrain carries the road cut.
                    for (size_t i = 0; i < pointCount; ++i)
                    {
                        const float worldX = road.origin[0] + road.points[i * 3 + 0];
                        const float worldZ = road.origin[2] + road.points[i * 3 + 2];
                        const float splineY = road.origin[1] + road.points[i * 3 + 1];
                        const float groundY = SurfaceY(worldX, worldZ);
                        (void)splineY;
                        road.points[i * 3 + 1] = groundY + 0.08f - road.origin[1];
                        if (i > 0)
                        {
                            const float dx = road.points[i * 3 + 0] - road.points[(i - 1) * 3 + 0];
                            const float dz = road.points[i * 3 + 2] - road.points[(i - 1) * 3 + 2];
                            roadKm += std::sqrt(double(dx) * dx + double(dz) * dz) / 1000.0;
                        }
                    }

                    const std::string name = "enfusion_road_" + std::to_string(r);
                    LODShapeWithShadow* shape =
                        BuildRoadShape(road.points, road.width, road.texture, road.material, name);
                    if (shape == nullptr)
                    {
                        ++roadsFailed;
                        continue;
                    }
                    if (!road.texture.empty())
                        ++roadTextured;
                    ++textureUse[road.texture];
                    // Mark every land cell within half the road width plus a 1 m verge of
                    // the centreline. The dense points are up to 4 m apart (up to ~8 m
                    // where a source segment rounded down to one step), so each segment
                    // is walked at 2 m and each sample tests the cells it can reach as a
                    // disc against the cell SQUARE -- a cell the ribbon merely clips at a
                    // corner is cleared too, which is what actually keeps grass off the
                    // tarmac at this grid. The same default width BuildRoadShape uses.
                    {
                        const float reach = 0.5f * ((road.width > 0.1f) ? road.width : 6.0f) + 1.0f;
                        const float reach2 = reach * reach;
                        for (size_t i = 0; i + 1 < pointCount; ++i)
                        {
                            const float ax = road.origin[0] + road.points[i * 3 + 0];
                            const float az = road.origin[2] + road.points[i * 3 + 2];
                            const float bx = road.origin[0] + road.points[(i + 1) * 3 + 0];
                            const float bz = road.origin[2] + road.points[(i + 1) * 3 + 2];
                            const float length = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
                            const int sub = std::max(1, static_cast<int>(std::ceil(length / 2.0f)));
                            for (int k = 0; k <= sub; ++k)
                            {
                                const float t = static_cast<float>(k) / static_cast<float>(sub);
                                const float px = ax + (bx - ax) * t;
                                const float pz = az + (bz - az) * t;
                                const int x0 = std::max(0, static_cast<int>(std::floor((px - reach) * _invLandGrid)));
                                const int x1 = std::min(_landRange - 1,
                                                        static_cast<int>(std::floor((px + reach) * _invLandGrid)));
                                const int z0 = std::max(0, static_cast<int>(std::floor((pz - reach) * _invLandGrid)));
                                const int z1 = std::min(_landRange - 1,
                                                        static_cast<int>(std::floor((pz + reach) * _invLandGrid)));
                                for (int cz = z0; cz <= z1; ++cz)
                                {
                                    const float zMin = static_cast<float>(cz) * _landGrid;
                                    const float dz = std::max({zMin - pz, 0.0f, pz - (zMin + _landGrid)});
                                    for (int cx = x0; cx <= x1; ++cx)
                                    {
                                        const float xMin = static_cast<float>(cx) * _landGrid;
                                        const float dx = std::max({xMin - px, 0.0f, px - (xMin + _landGrid)});
                                        if (dx * dx + dz * dz > reach2)
                                            continue;
                                        uint8_t& cell = _enfusionRoadCells[static_cast<size_t>(cz) *
                                                                               static_cast<size_t>(_landRange) +
                                                                           static_cast<size_t>(cx)];
                                        if (cell == 0)
                                        {
                                            cell = 1;
                                            ++roadCells;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // The vertices are already local to `origin`, and a road carries
                    // no rotation of its own -- the spline is authored in world axes.
                    AddObject(Vector3(road.origin[0], road.origin[1], road.origin[2]), road.yawDeg, shape);
                    ++roadsBuilt;
                }
                for (const auto& [path, count] : textureUse)
                    LOG_INFO(World, "Enfusion native load: road surface {}x '{}'", count,
                             path.empty() ? "(none)" : path);
                LOG_INFO(World, "Enfusion native load: {} objects more than 50 m above the terrain{}",
                     floatingObjects, firstFloating.empty() ? "" : ("; first: " + firstFloating));
            LOG_INFO(World,
                     "Enfusion native load: {} roads built ({} failed, {} textured), {:.1f} km, {} of {} land cells "
                     "under roads ({:.2f}%), {} ms",
                     roadsBuilt, roadsFailed, roadTextured, roadKm, roadCells, _enfusionRoadCells.size(),
                     _enfusionRoadCells.empty() ? 0.0 : 100.0 * double(roadCells) / double(_enfusionRoadCells.size()),
                     Poseidon::Foundation::GlobalTickCount() - roadStart);
            }
        }
    }

    ProgressFrame();
    if (_modernObjectStreaming)
    {
        LOG_INFO(World,
                 "Enfusion native load: STREAMING -- {} placements binned into {} land cells, {} models "
                 "({} terrain-relative), budget {} resident, window {} cells; largest bounding centre of a "
                 "pre-built shape {:.2f} m [POSEIDON_REFORGER_STREAM=0 or the Streaming tab restore the radius at load]",
                 _modernObjectPlacements.size(), _modernObjectCells.size(), _modernObjectModels.size(),
                 streamedTerrainRelativeTotal, _modernObjectBudget, _modernObjectRadius, maxBoundingCentreTotal);
    }
    {
        // RFG-096: the second UV set carries the MatPBRMulti mask; say what it looked like.
        const auto& uv = Poseidon::Asset::Formats::Enfusion::GXobUvCensus();
        LOG_INFO(World,
                 "Enfusion native load: UV set 2 census -- {} parts carry one, {} outside [0,1], {} degenerate (all "
                 "corners one texel), first outside: {}",
                 uv.partsWithSecond, uv.partsOutOfRange, uv.partsDegenerate,
                 uv.firstOutOfRange.empty() ? "-" : uv.firstOutOfRange.c_str());
        LOG_INFO(World, "Enfusion native load: UV set 1 census (RFG-102) -- {} parts: span<=1: {}, <=8: {}, <=64: {}, >64: {}",
                 uv.set0Parts, uv.set0SpanLe1, uv.set0SpanLe8, uv.set0SpanLe64, uv.set0SpanGt64);
        for (const std::string& w : uv.set0Wide)
            LOG_INFO(World, "RFG-102 wide UV set 1: {}", w);
        LOG_INFO(World, "Enfusion native load: face-UV transform census -- {} parts scaled by GlobalMapsUVTransform, first: {}",
                 uv.partsFaceUvScaled, uv.firstFaceUvScaled.empty() ? "-" : uv.firstFaceUvScaled.c_str());
    }
    InitGeography();
    ProgressFinish();

    // RFG-066: the denominator for "are the buildings' textures right". Without it the
    // only material evidence in the log was a 25-line cap of failures, which cannot
    // distinguish a repair that fixes 5% from one that fixes 90% -- and the three
    // repairs that were proposed for this before it was measured each addressed a
    // bucket that comes out at 0.
    if (GColourCensus.Total() != 0)
    {
        LOG_INFO(World,
                 "Enfusion native load: materials -- {} of {} resolved their own albedo map, {} a shared layer tile "
                 "(of those {} author a Color_N tint: {} applied [{} mask-weighted], {} still discarded, first: "
                 "'{}'), {} a flat colour, {} nothing (first: '{}'){}",
                 GColourCensus.ownMap, GColourCensus.Total(), GColourCensus.layerTile, GColourCensus.tintAuthored,
                 GColourCensus.tintApplied, GColourCensus.tintMaskWeighted, GColourCensus.tintDiscarded,
                 GColourCensus.firstTintDiscarded.empty() ? "none" : GColourCensus.firstTintDiscarded.c_str(),
                 GColourCensus.flat, GColourCensus.none,
                 GColourCensus.firstNone.empty() ? "none" : GColourCensus.firstNone.c_str(),
                 Poseidon::Enfusion::LayerTintEnabled() ? "" : " [RFG-070 layer tint OFF]");
    }

    LOG_INFO(World, "Enfusion native load: done in {} ms -- land {}x{} at {:.2f} m, terrain {}x{} at {:.2f} m",
             Poseidon::Foundation::GlobalTickCount() - start, landRange, landRange, landGrid, terrainRange,
             terrainRange, terrainGrid);
    return true;
}
