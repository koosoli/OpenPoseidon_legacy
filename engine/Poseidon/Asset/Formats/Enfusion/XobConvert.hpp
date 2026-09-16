// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-012 -- turning a `.xob` into an MLOD model the engine can build a shape from.
//
// Moved out of `apps/tools/Tools/commands/XobCommand.cpp` so the native world loader
// and the asset CLI share one converter rather than two that can drift. The tool
// keeps its own texture baking: the two places the converter consulted the baker are
// an interface now (`XobMaterialSink`), which the tool implements over TextureBaker
// and the engine leaves null.
//
// Nothing in the conversion changed in the move. The facts it encodes were each
// measured against the corpus and each one is load-bearing:
//   * LODs ordered by DESCENDING threshold -- taking the minimum yields a
//     six-triangle billboard that parses perfectly and is the wrong mesh;
//   * `autocenter=0` and the `map` property on EVERY LOD, because LODShape consults
//     the geometry LOD first;
//   * normal negation and winding swap as an XOR PAIR -- half of it is worse than
//     none of it;
//   * an unreferenced-vertex drop with a point remap, so nothing is written that no
//     triangle indexes.

#include <Poseidon/Asset/Formats/Enfusion/XobCollision.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobCollisionGeometry.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{
//! RFG-096: what the second UV set of every converted part looked like (see XobConvert).
struct XobUvCensus
{
    size_t      partsWithSecond = 0;
    size_t      partsOutOfRange = 0;
    size_t      partsDegenerate = 0;
    std::string firstOutOfRange;
    // RFG-102: the FIRST set (what the base colour samples). Buckets by the wider of the
    // u/v spans, and the first few wide parts with the range/extra they were unpacked with.
    size_t set0Parts = 0, set0SpanLe1 = 0, set0SpanLe8 = 0, set0SpanLe64 = 0, set0SpanGt64 = 0;
    std::vector<std::string> set0Wide;
    // Parts whose face UV was scaled by their material's GlobalMapsUVTransform
    // (first scaled part named, so a bad transform names itself in the log).
    size_t      partsFaceUvScaled = 0;
    std::string firstFaceUvScaled;
};
inline XobUvCensus& GXobUvCensus()
{
    static XobUvCensus census;
    return census;
}
} // namespace Poseidon::Asset::Formats::Enfusion

namespace Poseidon::Asset::Formats::Enfusion
{
namespace Fmt = Poseidon::Asset::Formats;

inline std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

//! What the asset CLI's texture baker contributes, seen from the converter.
//!
//! Three questions, because that is all ConvertXob ever asked the baker: may this
//! part draw at all, which `.paa` did you write for it, and which material file
//! should the face name instead of the source `.emat`. The engine's native loader
//! answers none of them -- it passes null, and the `.xob`'s own paths are written.
struct XobMaterialSink
{
    virtual ~XobMaterialSink() = default;
    virtual bool HideFaces(const std::string& materialSource) const = 0;
    virtual std::string TexturePath(const std::string& materialSource) const = 0;
    virtual std::string MaterialField(const std::string& materialSource) const = 0;
};

struct TextureOptions
{
    bool enabled = false;
    std::string prefix = "reforger\\everon\\textures";
    //! Longest edge written. Reforger authors 2048 and 4096 albedos; the whole point
    //! of the cap is that this writes one file per material and the corpus's top 50
    //! models name around sixty of them. 4096 also runs into the writer's truncated
    //! mip chain (measured on DXT3), so staying well under it is not only a budget.
    int maxSize = 1024;
    //! Bake `NMOMap` into a DXT5nm `.paa`. Needs `rvmats` to reach the renderer: a
    //! normal has no face-texture slot to arrive through, only a material stage.
    bool normals = false;
    //! Write a real `.rvmat` per material and name THAT in the MLOD material field.
    //! Superseded by `emats` below, which carries everything this does and the albedo
    //! too; kept for the A/B and for a reader that wants an RVMAT-only tree.
    bool rvmats = false;
    //! Deploy the material itself: the `.emat`, inheritance folded in and its texture
    //! keys rewritten to the `.paa` files this converter wrote, under the texture
    //! prefix at the source's own relative path (`Assets/X/Y.emat` ->
    //! `<prefix>\assets\x\y.emat`), and name THAT in the MLOD material field. The
    //! engine already reads an `.emat` as a material (LoadTranslatedMaterial); what it
    //! could never do was OPEN one, because every section named the path inside the
    //! `.pak`. Default ON with `--textures`: a written `.emat` the engine cannot parse
    //! costs exactly what the unopenable one cost, and one it can parse is the whole
    //! difference between `family=` and `family=MatPBRTreeCrown`. `--no-material-emat`
    //! is the A/B.
    bool emats = true;
    //! Deploy `MatPBRMulti`'s MASK and its `BCR_2..4` / `NMO_2..4` layer tiles too, so
    //! the runtime blends the layers itself instead of receiving one collapsed tile.
    //!
    //! The runtime blend has been complete for a while (`gpu_driven.wgsl`: one mask
    //! sample, RGB weighting three tiles at their own UV transforms, remainder to the
    //! base, renormalised on over-paint, same weights reused for the normals) and it
    //! never ran, because 0 of the 1,793 deployed materials named a `MaskMap` or a
    //! `BCR_N` -- this converter had already collapsed them. Against 4,044 and 3,905
    //! in Reforger's own 10,312. `MatPBRMulti` is 51% of the deployed corpus.
    //!
    //! `--no-material-layers` restores the collapse, which is the A/B and the fallback
    //! for a material whose mask or tiles do not decode.
    bool layers = true;
    //! Longest edge for a layer MASK, and the format is uncompressed on purpose.
    //!
    //! A mask's R, G and B are three INDEPENDENT weights. DXT1 encodes a 4x4 block as
    //! two endpoint colours and interpolates between them, so it forces the three
    //! channels onto one line in colour space -- which is exactly the correlation a mask
    //! does not have. Baked as DXT1 the church's mask came back as flat blocks of pure
    //! green, red and blue and the blend drew broad diagonal bands across the roof and
    //! walls. Uncompressed ARGB8888 keeps the channels apart.
    //!
    //! 512 because it is a low-frequency weight map, not detail: the detail comes from
    //! the tiles it selects between. Uncompressed at 1024 would cost ~5.6 MB per object.
    int maxMaskSize = 512;
};

struct ConvertOptions
{
    float resolution = 1.0f;  //!< runtime detail value at which the first coarser LOD becomes active
    bool secondUv = false;    //!< emit both #UVSet# blocks when a part declares two sets
    bool flipWinding = false; //!< see the winding note at the top of this file
    //! Fact 6: negate the written normal AND swap corners 0/1, together. Half of this
    //! is worse than none of it -- see the note at the top of the file.
    bool fixOrientation = false;
    //! Fact 7: write `autocenter=0` so the engine stops moving the model's origin to
    //! its bounding-box centre. See the note at the top of the file.
    bool fixOrigin = false;
    //! Fact 8: write a `map` named property from the source path -- `rock`, `tree`,
    //! `bush`, `house`, `wall`, else `hide` -- so the engine's one map-type table
    //! (ResolveMapTypeProperty, ShapeLOD.cpp) classifies a converted model the way it
    //! classifies an authored one, instead of falling back to a name heuristic. The
    //! table drives wind sway and canopy shading (Object.cpp GCurrentFoliageKind,
    //! EngineWgpu WGR_INSTANCE_CANOPY_*), so a rock and a tree MUST NOT share a class.
    //! Default ON: `--no-map-property` restores the property-less file, for the A/B.
    bool mapProperty = true;
    //! Fact 9: write a Geometry LOD built from the `.xob`'s COLL shapes, with
    //! `ComponentXX` selections and a `#Mass#` tagg. Default ON: `--no-geometry`
    //! restores the visual-only file, for the A/B.
    bool geometry = true;
    //! Fact 9, for inspection: use every COLL shape whatever its layer, so a fire
    //! geometry mesh and a foliage volume become walls too. Off by default.
    bool geometryAllLayers = false;
    //! RFG-022: derive each visual LOD's resolution from the `.xob`'s OWN threshold
    //! ratios instead of numbering the levels 1/2/3/4/5.
    //!
    //! The rank convention matches native Arma 3 ladders and is right for a model
    //! whose levels are spaced like Arma 3's. Reforger's are not: a spruce is
    //! authored at screen coverages 0.5 / 0.15 / 0.02 / 0.005 / 0.001, a range of
    //! 1:500, and its last level is FOUR TRIANGLES meant for the horizon. Squeezed
    //! into 1..5 that billboard arrives at a few tens of metres, which is what a
    //! forest of flat cards at close range actually is.
    //!
    //! Preserving the ratios keeps LOD 0 at `resolution` exactly, so near-field
    //! behaviour is unchanged, and pushes the impostor back out to where it was
    //! authored to appear.
    bool lodFromThreshold = false;
    TextureOptions textures;
};

//! Fact 9's mass table, keyed by fact 8's class. The numbers are the brief's -- a
//! house 5000, wall 2000, rock 1000, tree 500 -- with bush and the unclassified rest
//! chosen to sit well above Object::IsPassable's 10 kg. Mass changes nothing about
//! WHERE a static object collides, only whether the character response runs at all,
//! so these are not tuning values.
inline float GeometryMassForClass(const std::string& mapClass)
{
    if (mapClass == "house")
        return 5000.0f;
    if (mapClass == "wall")
        return 2000.0f;
    if (mapClass == "rock")
        return 1000.0f;
    if (mapClass == "tree")
        return 500.0f;
    if (mapClass == "bush")
        return 200.0f;
    return 1000.0f;
}

//! Fact 8's table: what a converted model's `map` named property says, from where the
//! `.xob` lives in the pak. Spelled in the words `ResolveMapTypeTable` accepts.
//!
//! Measured over the 1,132 models `ev_DALL.wrp` places (`%TEMP%\rf\manifestALL.txt`,
//! 2026-08-16): 30 under `Assets/Rocks/`, 66 `Assets/Vegetation/Tree/t_*`, 18
//! `Assets/Vegetation/Bush/b_*`, 11 other vegetation (plants `p_*`, vegetables, and the
//! `t_stump_debris_01` / `t_trunk_debris_01` tree parts), 30 `Structures/*/houses`, ~250
//! `Structures/Walls/*`, the rest props, signs, infrastructure and decals. The rules:
//!
//!   Assets/Rocks/**                                        rock
//!   Assets/Vegetation/Tree/**/t_*   (not stump/fallen/debris/trunk)   tree
//!   Assets/Vegetation/Bush/**       (and any b_* under Vegetation)    bush
//!   Assets/Structures/**/Houses/**  and Structures/Houses/**          house
//!   Assets/Structures/Walls/**                             wall
//!   everything else                                        hide
//!
//! `hide` is what an ABSENT property already resolves to (ShapeLOD.cpp: the unknown
//! case is MapHide), so nothing outside the five classes changes; it is written anyway
//! so a reader of the file sees a decision rather than an omission. A stump, a fallen
//! trunk or a piece of debris is `hide` and not `tree` for the reason the engine's own
//! fallback gives: it has no crown to bend, and swaying one reads as the ground moving.
//! Nothing here is `forest`, `small tree` or `building`: the first two would need a
//! measurement of canopy size this converter does not make, and `building` would put
//! a map symbol on every kerb and pole.
inline std::string MapPropertyForXob(const std::string& sourcePath)
{
    std::string path = LowerCopy(sourcePath);
    for (char& c : path)
        if (c == '\\')
            c = '/';
    const size_t slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto has = [&](const char* needle) { return path.find(needle) != std::string::npos; };
    const auto baseHas = [&](const char* needle) { return base.find(needle) != std::string::npos; };

    if (has("/rocks/"))
        return "rock";
    if (has("/vegetation/"))
    {
        const bool debris = baseHas("stump") || baseHas("fallen") || baseHas("debris") || baseHas("trunk");
        if (base.rfind("b_", 0) == 0 || has("/vegetation/bush/"))
            return debris ? "hide" : "bush";
        if (base.rfind("t_", 0) == 0 && has("/vegetation/tree/"))
            return debris ? "hide" : "tree";
        // RFG-042: `p_` under `/vegetation/plant/` -- nettles, weeds, ferns, the
        // knee-high layer. These fell through to `hide`, which costs them the canopy
        // flags, and the canopy flags are what the wind is gated on: measured in
        // gpu_driven.wgsl, sway applies only to INST_CANOPY_BUSH|TREE|FOREST. So a
        // field of nettles stood perfectly still next to a swaying hedge.
        //
        // Classed as `bush` and not `tree` deliberately: a tree's crown-Y lift assumes
        // a bounding centre at mid-trunk, and a 40 cm plant has no trunk.
        //
        // Rocks are unaffected and always were -- `/rocks/` returns `rock` above, and
        // MapRock reaches none of the canopy branches. That gate is the same one that
        // keeps a fence post from swaying.
        if (base.rfind("p_", 0) == 0 && has("/vegetation/plant/"))
            return debris ? "hide" : "bush";
        return "hide";
    }
    if (has("/structures/"))
    {
        if (has("/houses/"))
            return "house";
        if (has("/structures/walls/"))
            return "wall";
    }
    return "hide";
}

struct ConvertStats
{
    size_t lodIndex = 0;
    size_t lodCount = 0;
    size_t lodsWritten = 0;
    size_t lodsDropped = 0;
    float threshold = 0.0f;
    size_t parts = 0;
    size_t points = 0;
    size_t triangles = 0;
    size_t droppedUnreferenced = 0; //!< source vertices no triangle indexes
    size_t materials = 0;
    size_t partsTextured = 0;     //!< parts whose material resolved to a written `.paa`
    size_t partsWithMaterial = 0; //!< parts naming a DEPLOYED material file rather than the pak path
    size_t partsHidden = 0;       //!< parts dropped whole: unresolvable decal shells / water-erase surfaces
    size_t crossAlongNormal = 0;  //!< triangles whose edge cross agrees with the stored normals
    //! Fact 6's absolute check, over LOD0 only: does the WRITTEN normal point away from
    //! the LOD's point-cloud centroid? Meaningful per model only for a closed convex
    //! solid (a boulder); over a corpus the aggregate is still the number that moves
    //! when the sign is wrong, because no corpus is majority-concave.
    size_t normalOutward = 0;
    size_t normalInward = 0;
    float bboxMin[3] = {0.0f, 0.0f, 0.0f};
    float bboxMax[3] = {0.0f, 0.0f, 0.0f};
    std::string mapProperty; //!< fact 8: the `map` value written on every LOD, "" when none
    //! Fact 9: the geometry LOD. `geometryWritten` false with `collisionShapes > 0`
    //! is a model whose shapes are all fire/view/foliage volumes -- correct, and
    //! worth seeing on the model line as `geo 0 comps`.
    bool collisionPresent = false; //!< the `.xob` has a COLL chunk
    bool collisionCloses = true;   //!< its record walk closed
    std::string collisionError;    //!< why it did not
    size_t collisionShapes = 0;    //!< records in COLL
    size_t collisionBlocking = 0;  //!< on a character-blocking layer
    bool geometryWritten = false;
    GeometryLodStats geometry;
    float geometryMass = 0.0f;

    float Extent(int axis) const { return bboxMax[axis] - bboxMin[axis]; }
};

//! Selects the finest visual LOD: the MAXIMUM threshold. See fact 1 at the top --
//! taking the minimum is the mistake this exists to make impossible.
inline size_t FinestLodIndex(const XobHeader& header)
{
    size_t best = 0;
    for (size_t i = 1; i < header.lods.size(); ++i)
        if (header.lods[i].threshold > header.lods[best].threshold)
            best = i;
    return best;
}

//! XOB thresholds and this engine's resolutions do not have proven compatible
//! units. Use the native content convention instead: measured Arma 3 tree and rock
//! ladders are 1/2/3/4[/5], independent of their very different bounds. `base`
//! remains an explicit calibration multiplier rather than hiding a guessed unit
//! conversion in the exporter.
inline float VisualLodResolution(size_t rank, float base)
{
    return base * static_cast<float>(rank + 1);
}

//! Resolution from the authored threshold ratio: level 0 keeps `base`, and each
//! coarser level is `base * (finestThreshold / itsThreshold)`.
//!
//! Falls back to the rank convention when the thresholds cannot carry it -- a
//! non-positive or non-decreasing ladder would produce equal or descending
//! resolutions, and MLOD requires them strictly ascending.
inline float VisualLodResolutionFromThreshold(size_t rank, float base, float finestThreshold, float threshold)
{
    if (!(finestThreshold > 0.0f) || !(threshold > 0.0f) || threshold > finestThreshold)
        return VisualLodResolution(rank, base);
    const float scaled = base * (finestThreshold / threshold);
    // Guard the same contract from the other side: a ratio that rounds to the
    // previous level's value is no better than the rank convention.
    return scaled > base ? scaled : VisualLodResolution(rank, base);
}

//! Face-UV scale/offset from a part material's `GlobalMapsUVTransform`
//! (e.g. NetFence_Wire.emat TilingU/V 24: sampled at the face's raw UV the
//! wire cutout is magnified 24x and the panel reads as an empty frame).
//! Enfusion samples its global maps through this transform, so the converter
//! bakes it into the written face UV -- both render paths then inherit it,
//! with no shader or ABI change. The second (mask) set is the object's own
//! unwrap and stays unscaled. Cached by material path; a material that names
//! none (or cannot be read, e.g. the offline exporter with no mount open)
//! converts exactly as before.
struct FaceUvScale
{
    float su = 1.0f, sv = 1.0f, ou = 0.0f, ov = 0.0f;
    bool  active = false;
};
inline FaceUvScale FaceUvScaleForMaterial(const std::string& materialPath)
{
    static std::unordered_map<std::string, FaceUvScale> cache;
    static std::mutex                                   cacheMutex;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto found = cache.find(materialPath); found != cache.end())
            return found->second;
    }
    FaceUvScale result;
    try
    {
        if (materialPath.size() > 5 && ::Poseidon::Asset::Material::IsEmatPath(materialPath))
        {
            ::Poseidon::Asset::Material::EmatMaterial  emat;
            ::Poseidon::Asset::Material::RvUvTransform t;
            if (::Poseidon::Asset::Material::ReadEmatFile(materialPath, emat) &&
                ::Poseidon::Asset::Material::EmatUvTransform(emat, "GlobalMapsUVTransform", t) &&
                t.present)
            {
                result.su = t.aside[0];
                result.sv = t.up[1];
                result.ou = t.pos[0];
                result.ov = t.pos[1];
                result.active =
                    (result.su != 1.0f || result.sv != 1.0f || result.ou != 0.0f || result.ov != 0.0f);
            }
        }
    }
    catch (...)
    {
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache[materialPath] = result;
    return result;
}

//! `sourcePath` is the `.xob`'s own path in the pak; it decides fact 8's `map` property
//! and nothing else. Empty disables the property (a caller with no path has no class).
inline bool ConvertXob(const std::vector<uint8_t>& bytes, const ConvertOptions& options,
                       const XobMaterialSink* baker,
                const std::string& sourcePath, Fmt::MLOD::WriteModel& out, ConvertStats& stats, std::string& error)
{
    const XobHeader header = ReadXobHeader(bytes.data(), bytes.size());
    if (!header.valid())
    {
        error = "HEAD: " + header.error;
        return false;
    }
    if (header.lods.empty())
    {
        error = "no LODs";
        return false;
    }

    if (!(options.resolution > 0.0f) || !std::isfinite(options.resolution))
    {
        error = "LOD resolution anchor must be finite and positive";
        return false;
    }

    stats.lodCount = header.lods.size();
    stats.lodIndex = FinestLodIndex(header);
    stats.threshold = header.lods[stats.lodIndex].threshold;
    stats.mapProperty = options.mapProperty && !sourcePath.empty() ? MapPropertyForXob(sourcePath) : std::string();
    std::unordered_set<std::string> materialPaths;
    float boxMin[3] = {1e30f, 1e30f, 1e30f};
    float boxMax[3] = {-1e30f, -1e30f, -1e30f};

    // The corpus is already finest-first, but order by threshold here so a valid
    // file with descriptors in another order still reaches MLOD's ascending
    // resolution contract. Equal thresholds are impossible in the measured corpus.
    std::vector<size_t> lodOrder(header.lods.size());
    for (size_t i = 0; i < lodOrder.size(); ++i)
        lodOrder[i] = i;
    std::stable_sort(lodOrder.begin(), lodOrder.end(),
                     [&](size_t a, size_t b) { return header.lods[a].threshold > header.lods[b].threshold; });

    out.lods.clear();
    for (size_t rank = 0; rank < lodOrder.size(); ++rank)
    {
        const size_t lodIndex = lodOrder[rank];
        XobLod lod;
        std::string lodError;
        if (!ReadXobLod(bytes.data(), bytes.size(), header, lodIndex, lod, lodError))
        {
            if (rank == 0)
            {
                error = "LOD " + std::to_string(lodIndex) + ": " + lodError;
                return false;
            }
            stats.lodsDropped++;
            continue;
        }

        Fmt::MLOD::WriteLod writeLod;
        // Fact 7. On every LOD, not just LOD0: `Model.cpp:67` reads lodLevels[0] but
        // `LODShape::PropertyValue` consults the GEOMETRY LOD first, and a converted
        // model has no geometry LOD today but may grow one.
        if (options.fixOrigin)
            writeLod.properties.push_back({"autocenter", "0"});
        // Fact 8, on every LOD for the same reason as autocenter: `PropertyValue` reads
        // the geometry LOD first and LOD 0 second, and which LOD is which is the
        // engine's decision, not this file's.
        if (!stats.mapProperty.empty())
            writeLod.properties.push_back({"map", stats.mapProperty});
        writeLod.resolution =
            options.lodFromThreshold
                ? VisualLodResolutionFromThreshold(rank, options.resolution,
                                                   header.lods[lodOrder[0]].threshold, header.lods[lodIndex].threshold)
                : VisualLodResolution(rank, options.resolution);
        // RFG-029: an authored ladder whose ratio runs past the visual range falls
        // back to the rank convention for THAT level rather than refusing the model.
        //
        // Refusing cost 418 of Everon's 1,248 distinct meshes -- disproportionately
        // buildings, whose impostor level sits a thousand times coarser than their
        // finest and so blows past 900 on the ratio alone. A model with one badly
        // spaced coarse level is worth having; a village with no houses in it is not.
        if (!(writeLod.resolution < 900.0f) || !std::isfinite(writeLod.resolution))
        {
            // RFG-072: the fallback must stay ABOVE the level before it.
            //
            // RFG-029 fell back to the rank convention, base * (rank + 1): rank 5 gets
            // 6, rank 6 gets 7. Measured on Church_01 (thresholds 0.5 / 0.15 / 0.02 /
            // 0.005 / 0.001 / 0.0002 / 0.0001) the ratio ladder is 1, 3.3, 25, 100, 500
            // and then, out of range, 6 and 7 -- so the 2,806-triangle far level sits at
            // resolution 6, BELOW the 41,756-triangle lod2 at 25, and the engine picks
            // it point-blank. Ten metres from the wall the church bound only
            // `Church_01_Ext_MLOD_Yellow_V2.emat`, its baked far material, and never
            // `ExtWalls`; that is why no wall-material fix could show.
            //
            // Nothing in MLODWriter or the loader enforces the ascending order the
            // comments assert, so a descending value is not refused, it is obeyed. The
            // out-of-range level is therefore placed past the last in-range one, at a
            // fixed step, and pinned under the 900 ceiling.
            const float previous = out.lods.empty() ? options.resolution : out.lods.back().resolution;
            writeLod.resolution = std::min(899.0f, std::max(previous * 1.6f, 600.0f));
        }
        if (!(writeLod.resolution < 900.0f) || !std::isfinite(writeLod.resolution))
        {
            error = "LOD " + std::to_string(lodIndex) + " maps outside the visual resolution range";
            return false;
        }
        // RFG-054: how coarse a level is allowed to be, judged by what is left of it.
        //
        // Measured on Everon: `t_betula_pendula_1s` maps its 6-TRIANGLE level to
        // resolution 100, and `t_betula_pendula_2sw` maps a 6-triangle level to 500.
        // Resolution 100 is mid-distance, not the horizon, so a birch wood turns into a
        // field of six-triangle stubs while it is still plainly a wood -- which reads
        // as a bright, flat, contrastless mass rather than as trees.
        //
        // The threshold ratio RFG-022 preserves is the AUTHORED spacing, and it is
        // right about the ORDER of the levels; what it cannot supply is the unit. Enfusion
        // thresholds and this engine's resolutions have never been shown to share one.
        // So the ratio still orders the ladder, and a floor keeps a level that has almost
        // no geometry left from being reached while the object still fills pixels.
        //
        // Deliberately a floor and not a drop: the level is real, the file wants it, and
        // at true distance it is exactly what should draw.
        const size_t triangleFloor = 32;
        bool anySecondSet = false;

        for (const XobPart& part : lod.parts)
        {
            if (part.uvSets.empty() || part.uvSets[0].size() != part.positions.size())
            {
                error = "LOD " + std::to_string(lodIndex) + " part has no usable UV set 0";
                return false;
            }

            // The material first, because it can say the whole part must not draw
            // (BakedMaterial::hideFaces) -- and then its points must not be written
            // either, or they would sit in the file referenced by nothing.
            std::string materialSource;
            if (part.materialIndex < header.materials.size())
                materialSource = header.materials[part.materialIndex].path;
            const XobMaterialSink* bakedPart = nullptr;
            if (baker && !materialSource.empty())
            {
                bakedPart = baker;
                if (bakedPart->HideFaces(materialSource))
                {
                    stats.partsHidden++;
                    continue;
                }
            }

            // Only referenced source vertices become points. Every point must be
            // referenced for the round-trip check below to be an identity.
            std::vector<int32_t> remap(part.positions.size(), -1);
            for (uint16_t index : part.indices)
                if (index < remap.size())
                    remap[index] = 0;

            for (size_t v = 0; v < part.positions.size(); ++v)
            {
                if (remap[v] != 0)
                {
                    stats.droppedUnreferenced++;
                    continue;
                }
                remap[v] = static_cast<int32_t>(writeLod.points.size());

                Fmt::MLOD::WritePoint point;
                point.position = {part.positions[v].x, part.positions[v].y, part.positions[v].z};
                point.flags = 0;
                writeLod.points.push_back(point);
                // Fact 6: `.xob` normals are outward, this engine's are inward. The
                // reader stays faithful to the file; the convention change belongs here,
                // in the converter, and is paired with the winding swap below.
                const float sign = options.fixOrientation ? -1.0f : 1.0f;
                writeLod.normals.push_back(
                    {sign * part.normals[v].x, sign * part.normals[v].y, sign * part.normals[v].z});

                for (int axis = 0; axis < 3; ++axis)
                {
                    const float value = axis == 0   ? part.positions[v].x
                                        : axis == 1 ? part.positions[v].y
                                                    : part.positions[v].z;
                    boxMin[axis] = std::min(boxMin[axis], value);
                    boxMax[axis] = std::max(boxMax[axis], value);
                }
            }

            std::string texturePath;
            std::string materialPath = materialSource;
            if (bakedPart != nullptr)
            {
                texturePath = bakedPart->TexturePath(materialSource);
                // Only when a file was actually written. Naming a material file that
                // does not exist is strictly worse than naming the source `.emat`:
                // both fail to open, but the second at least says which material it
                // meant.
                if (!bakedPart->MaterialField(materialSource).empty())
                {
                    materialPath = bakedPart->MaterialField(materialSource);
                    stats.partsWithMaterial++;
                }
            }
            if (!texturePath.empty())
                stats.partsTextured++;
            if (!materialPath.empty())
                materialPaths.insert(materialPath);

            // The material's face-UV transform (identity for the overwhelming
            // majority); resolved once per part, applied per face below.
            const FaceUvScale faceUvT = FaceUvScaleForMaterial(materialPath);
            if (faceUvT.active)
            {
                XobUvCensus& c = GXobUvCensus();
                ++c.partsFaceUvScaled;
                if (c.firstFaceUvScaled.empty())
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "%s (%s %.2f,%.2f %+.2f,%+.2f)", sourcePath.c_str(),
                                  materialPath.c_str(), faceUvT.su, faceUvT.sv, faceUvT.ou, faceUvT.ov);
                    c.firstFaceUvScaled = buf;
                }
            }

            const bool hasSecond = part.uvSets.size() > 1 && part.uvSets[1].size() == part.positions.size();
            anySecondSet = anySecondSet || hasSecond;
            if (!part.uvSets.empty() && part.uvSets[0].size() == part.positions.size())
            {
                float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                for (const XobVec2& uv : part.uvSets[0])
                {
                    u0 = std::min(u0, uv.u); u1 = std::max(u1, uv.u);
                    v0 = std::min(v0, uv.v); v1 = std::max(v1, uv.v);
                }
                const float span = std::max(u1 - u0, v1 - v0);
                XobUvCensus& c = GXobUvCensus();
                ++c.set0Parts;
                if (span <= 1.01f) ++c.set0SpanLe1;
                else if (span <= 8.0f) ++c.set0SpanLe8;
                else if (span <= 64.0f) ++c.set0SpanLe64;
                else ++c.set0SpanGt64;
                if (span > 8.0f && c.set0Wide.size() < 12)
                {
                    const XobUvSet d = part.uvSetDescs.empty() ? XobUvSet{} : part.uvSetDescs[0];
                    char buf[300];
                    std::snprintf(buf, sizeof(buf), "%s part %zu: u[%.2f..%.2f] v[%.2f..%.2f] desc u[%.2f..%.2f] v[%.2f..%.2f] extra=%.3f verts=%zu",
                                  sourcePath.c_str(), static_cast<size_t>(&part - &lod.parts[0]), u0, u1, v0, v1, d.uMin, d.uMax, d.vMin, d.vMax, d.extra,
                                  part.positions.size());
                    c.set0Wide.push_back(buf);
                }
            }
            // RFG-096: a census of the SECOND UV set, because the MatPBRMulti mask is sampled
            // through it and a wall wearing a triangular patchwork is what a wrong set looks
            // like. Counted per part: values outside [-0.01, 1.01] and sets whose span is
            // under 0.01 (every corner the same texel).
            if (hasSecond)
            {
                float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                for (const XobVec2& uv : part.uvSets[1])
                {
                    u0 = std::min(u0, uv.u); u1 = std::max(u1, uv.u);
                    v0 = std::min(v0, uv.v); v1 = std::max(v1, uv.v);
                }
                XobUvCensus& c = GXobUvCensus();
                ++c.partsWithSecond;
                if (u0 < -0.01f || v0 < -0.01f || u1 > 1.01f || v1 > 1.01f)
                    ++c.partsOutOfRange;
                if (u1 - u0 < 0.01f && v1 - v0 < 0.01f)
                    ++c.partsDegenerate;
                if (c.firstOutOfRange.empty() && (u0 < -0.01f || v0 < -0.01f || u1 > 1.01f || v1 > 1.01f))
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s u[%.2f..%.2f] v[%.2f..%.2f]", sourcePath.c_str(), u0, u1, v0, v1);
                    c.firstOutOfRange = buf;
                }
            }

            // RFG-102: with render-stream UVs the face runs over list B; the corner's
            // position/normal come through renderSource, its UV from the render vertex.
            const bool renderUv = !part.renderUvSets.empty() && part.renderIndices.size() == part.indices.size() &&
                                  part.renderUvSets[0].size() == part.renderSource.size();
            for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
            {
                uint16_t corner[3] = {part.indices[t], part.indices[t + 1], part.indices[t + 2]};
                uint16_t rcorner[3] = {part.renderIndices[t], part.renderIndices[t + 1], part.renderIndices[t + 2]};
                if (renderUv)
                    for (int c = 0; c < 3; ++c)
                        corner[c] = static_cast<uint16_t>(part.renderSource[rcorner[c]]);
                // Two independent reasons to swap, so XOR: asking for both is asking for
                // the original order back, and silently applying one twice would be the
                // kind of cancellation this file exists to make impossible.
                if (options.flipWinding != options.fixOrientation)
                {
                    std::swap(corner[0], corner[1]);
                    std::swap(rcorner[0], rcorner[1]);
                }

                {
                    const XobVec3& p0 = part.positions[corner[0]];
                    const XobVec3& p1 = part.positions[corner[1]];
                    const XobVec3& p2 = part.positions[corner[2]];
                    const float e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                    const float e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                    const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                            e1[0] * e2[1] - e1[1] * e2[0]};
                    float mean[3] = {0.0f, 0.0f, 0.0f};
                    for (int c = 0; c < 3; ++c)
                    {
                        mean[0] += part.normals[corner[c]].x;
                        mean[1] += part.normals[corner[c]].y;
                        mean[2] += part.normals[corner[c]].z;
                    }
                    if (cross[0] * mean[0] + cross[1] * mean[1] + cross[2] * mean[2] > 0.0f)
                        stats.crossAlongNormal++;
                }

                Fmt::MLOD::WriteFace face;
                face.vertexCount = 3;
                face.flags = 0;
                face.material = materialPath;
                face.texture = texturePath;
                for (int c = 0; c < 3; ++c)
                {
                    const int32_t point = remap[corner[c]];
                    face.vertices[c].point = point;
                    face.vertices[c].normal = point;
                    // Enfusion samples its global maps through the material's
                    // GlobalMapsUVTransform; u1/v1 carry the object's own mask
                    // unwrap and stay unscaled. Identity when the material names
                    // none, which is bit-exact (u*1+0 == u).
                    const float fu =
                        renderUv ? part.renderUvSets[0][rcorner[c]].u : part.uvSets[0][corner[c]].u;
                    const float fv =
                        renderUv ? part.renderUvSets[0][rcorner[c]].v : part.uvSets[0][corner[c]].v;
                    face.vertices[c].u = fu * faceUvT.su + faceUvT.ou;
                    face.vertices[c].v = fv * faceUvT.sv + faceUvT.ov;
                    if (hasSecond)
                    {
                        face.vertices[c].u1 = (renderUv && part.renderUvSets.size() > 1) ? part.renderUvSets[1][rcorner[c]].u : part.uvSets[1][corner[c]].u;
                        face.vertices[c].v1 = (renderUv && part.renderUvSets.size() > 1) ? part.renderUvSets[1][rcorner[c]].v : part.uvSets[1][corner[c]].v;
                    }
                }
                writeLod.faces.push_back(std::move(face));
            }
        }

        if (writeLod.points.empty() || writeLod.faces.empty())
        {
            error = "LOD " + std::to_string(lodIndex) + " has no geometry";
            return false;
        }

        // Fact 6's absolute orientation check. LOD0 only: it is the level that is drawn
        // near the camera, it is the only one whose geometry is dense enough for the
        // centroid to be a fair reference, and running it on every rung would say the
        // same thing five times. Faces whose centroid coincides with the model's are
        // skipped rather than counted as either sign.
        if (rank == 0 && writeLod.points.size() >= 4)
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            for (const Fmt::MLOD::WritePoint& point : writeLod.points)
            {
                cx += point.position.x;
                cy += point.position.y;
                cz += point.position.z;
            }
            const float invN = 1.0f / static_cast<float>(writeLod.points.size());
            cx *= invN;
            cy *= invN;
            cz *= invN;
            for (const Fmt::MLOD::WriteFace& face : writeLod.faces)
            {
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                for (int c = 0; c < face.vertexCount; ++c)
                {
                    // -1 is the sanctioned "no normal" and would index out of range.
                    if (face.vertices[c].normal < 0)
                        continue;
                    const auto& normal = writeLod.normals[static_cast<size_t>(face.vertices[c].normal)];
                    nx += normal.x;
                    ny += normal.y;
                    nz += normal.z;
                    const auto& position = writeLod.points[static_cast<size_t>(face.vertices[c].point)].position;
                    fx += position.x;
                    fy += position.y;
                    fz += position.z;
                }
                const float invC = 1.0f / static_cast<float>(face.vertexCount);
                fx = fx * invC - cx;
                fy = fy * invC - cy;
                fz = fz * invC - cz;
                const float dot = nx * fx + ny * fy + nz * fz;
                if (dot > 0.0f)
                    stats.normalOutward++;
                else if (dot < 0.0f)
                    stats.normalInward++;
            }
        }

        writeLod.secondUVChannel = options.secondUv && anySecondSet;
        stats.parts += lod.parts.size();
        stats.points += writeLod.points.size();
        stats.triangles += writeLod.faces.size();
        stats.lodsWritten++;
        if (writeLod.faces.size() < triangleFloor && rank > 0)
        {
            // Pushed out in proportion to how little is left: a 4-triangle card goes
            // furthest, a 30-triangle one barely moves.
            const float scarcity = static_cast<float>(triangleFloor) /
                                   static_cast<float>(writeLod.faces.empty() ? 1 : writeLod.faces.size());
            const float pushed = writeLod.resolution * scarcity;
            writeLod.resolution = pushed < 899.0f ? pushed : 899.0f;
            // The ladder must stay strictly ascending, so never fall below the level
            // before it -- MLOD rejects an equal or descending resolution outright.
            if (!out.lods.empty() && writeLod.resolution <= out.lods.back().resolution)
                writeLod.resolution = out.lods.back().resolution * 1.5f;
            if (!(writeLod.resolution < 900.0f))
                writeLod.resolution = 899.0f;
        }
        out.lods.push_back(std::move(writeLod));
    }

    stats.materials = materialPaths.size();
    for (int axis = 0; axis < 3; ++axis)
    {
        stats.bboxMin[axis] = boxMin[axis];
        stats.bboxMax[axis] = boxMax[axis];
    }

    // Fact 9: the Geometry LOD, from the COLL chunk. After the visual ladder, so
    // MLOD's ascending-resolution contract holds (1e13 is above any visual value),
    // and carrying the same properties as every visual LOD because
    // LODShape::PropertyValue consults the geometry LOD FIRST -- a geometry LOD
    // without `autocenter=0` would undo fact 7 on the model that has it.
    if (options.geometry && !out.lods.empty())
    {
        const XobCollision collision = ReadXobCollision(bytes.data(), bytes.size(), header);
        stats.collisionPresent = collision.present;
        stats.collisionCloses = collision.closes || !collision.present;
        stats.collisionError = collision.error;
        stats.collisionShapes = collision.shapes.size();
        for (const XobCollisionShape& shape : collision.shapes)
            if (XobLayerBlocksCharacters(shape.layer))
                ++stats.collisionBlocking;
        if (collision.present && !collision.shapes.empty())
        {
            GeometryLodOptions geometryOptions;
            geometryOptions.totalMass = GeometryMassForClass(stats.mapProperty);
            geometryOptions.allLayers = options.geometryAllLayers;
            // A boulder is solid, lumpy and sometimes modelled without an underside;
            // a building's open pieces are sheets around rooms and its concavities
            // are the rooms. Only the class knows which, so rocks take their mesh
            // pieces whole at a looser tolerance (a stone's bumps, up to half its
            // radius, become the intersection of its face half-spaces -- measured on
            // GraniteBeachCluster_01/02: 9 and 11 stones, 5-50% of radius), chunk
            // what is still concave, and everything else keeps the tight tolerance
            // that sends a house shell to slabs.
            const bool rock = stats.mapProperty == "rock";
            geometryOptions.solidMesh = rock;
            if (rock)
            {
                geometryOptions.nearConvexTolerance = 0.5f;
                geometryOptions.nearConvexMaxTolerance = 2.0f;
            }
            Fmt::MLOD::WriteLod geometryLod;
            if (BuildGeometryLod(collision, geometryOptions, geometryLod, stats.geometry))
            {
                geometryLod.properties = out.lods.front().properties;
                stats.geometryWritten = true;
                stats.geometryMass = geometryOptions.totalMass;
                out.lods.push_back(std::move(geometryLod));
            }
        }
    }

    return !out.lods.empty();
}

} // namespace Poseidon::Asset::Formats::Enfusion
