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

#include <Poseidon/World/Terrain/Landscape.hpp>

#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PrefabResolve.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ResourceDatabase.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobConvert.hpp>
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
#include <Poseidon/Graphics/Textures/Image.hpp>
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
const char* WantedWorld()
{
    static const std::string wanted = []
    {
        const char* v = std::getenv("POSEIDON_REFORGER_WORLD");
        return std::string((v != nullptr && *v != 0) ? v : "worlds/eden");
    }();
    return wanted.c_str();
}

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

    std::string texturePath = material.TextureOf("BCRMap");
    if (texturePath.empty())
        texturePath = material.TextureOf("AlbedoMap");
    if (texturePath.empty())
        texturePath = material.TextureOf("BCR_1");
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

//! One placement, resolved: where it goes and which `.xob` draws it.
struct ResolvedPlacement
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float yawDeg = 0.0f;
    bool terrainRelative = false;
    uint32_t mesh = 0; //!< index into the mesh list
};

PlacementSummary ResolvePlacements(const EnfusionMount& mount, const std::string& addonsDir,
                                   std::vector<std::pair<std::string, size_t>>& meshes,
                                   std::vector<ResolvedPlacement>& resolved)
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
    resolved.reserve(world.placements.size());
    for (const Enf::EbinPlacement& placement : world.placements)
    {
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
        // Yaw only. The reader carries a Euler triple in degrees and everything
        // downstream of it uses index 1 and discards pitch and roll -- a documented
        // reduction of the reader, not an omission here.
        item.yawDeg = placement.angles[1];
        // The Y rule, measured over Everon's 1.23 M placements: with a Flags block
        // 96.3% sit within half a metre of the terrain, without one the median Y is
        // exactly 0. Reading every Y as absolute buries the houses a median 29 m
        // under, because SCR_DestructibleBuildingEntity is 93% flagless.
        item.terrainRelative = !placement.hasFlags && std::fabs(placement.position[1]) < 2.0f;
        item.mesh = slot->second;
        resolved.push_back(item);
    }
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

/// RFG-013: one `.xob` in the archives -> one shape the engine can draw.
///
/// The hop through MLOD in memory is deliberate. A direct XobModel -> Model builder
/// would have to reimplement the loader's vertex merge, section building, UV-set
/// handling and winding bookkeeping, and then keep them in step with the loader
/// forever; `writeToBuffer` / `loadFromBuffer` are pure memory, so the round trip
/// costs one serialise plus one parse per DISTINCT model and buys all of that.
/// Nothing is written to disk.
LODShapeWithShadow* BuildShapeFromXob(const EnfusionMount& mount, const std::string& xobPath, std::string& why)
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
    // The native path has no texture baker, so the `.xob`'s own material paths are
    // written and the models draw untextured until the material side lands.
    Fmt::MLOD::WriteModel model;
    Enf::ConvertStats stats;
    if (!Enf::ConvertXob(bytes, options, nullptr, xobPath, model, stats, why))
        return nullptr;

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
    // The name is only a cache key here -- no file is opened, because the parsed
    // model is handed in.
    LODShapeWithShadow* shape = Poseidon::Shapes.NewFromModel(xobPath.c_str(), false, true, parsed, nullptr);
    if (shape == nullptr)
        why = "ShapeBank refused the model";
    return shape;
}
} // namespace

bool Landscape::LoadEnfusionNative(const char* addonsDir)
{
    if (addonsDir == nullptr || *addonsDir == 0)
        return false;

    const DWORD start = Poseidon::Foundation::GlobalTickCount();
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
        wantObjects != nullptr && wantObjects[0] == '1')
    {
        const DWORD placeStart = Poseidon::Foundation::GlobalTickCount();
        std::vector<std::pair<std::string, size_t>> meshes;
        std::vector<ResolvedPlacement> placed;
        const PlacementSummary summary =
            ResolvePlacements(EnfusionMount::Instance(), addonsDir, meshes, placed);
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

            // RFG-013: build the commonest meshes and put their placements in the
            // world. Capped while this is new -- 1.23 M objects is the streaming
            // problem, not the loading one, and a cap that says so in the log is
            // better than a load that never finishes.
            const size_t modelCap = []
            {
                const char* v = std::getenv("POSEIDON_REFORGER_MAX_MODELS");
                return static_cast<size_t>((v != nullptr && *v != 0) ? std::atoi(v) : 60);
            }();
            const size_t objectCap = []
            {
                const char* v = std::getenv("POSEIDON_REFORGER_MAX_OBJECTS");
                return static_cast<size_t>((v != nullptr && *v != 0) ? std::atoi(v) : 40000);
            }();

            std::vector<std::pair<size_t, uint32_t>> byCount;
            byCount.reserve(meshes.size());
            for (size_t i = 0; i < meshes.size(); ++i)
                byCount.emplace_back(meshes[i].second, static_cast<uint32_t>(i));
            std::sort(byCount.begin(), byCount.end(), std::greater<>());

            std::unordered_map<uint32_t, LODShapeWithShadow*> shapes;
            size_t built = 0;
            size_t buildFailed = 0;
            std::string firstBuildFailure;
            const DWORD buildStart = Poseidon::Foundation::GlobalTickCount();
            for (size_t i = 0; i < byCount.size() && built < modelCap; ++i)
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
            }
            const DWORD buildMs = Poseidon::Foundation::GlobalTickCount() - buildStart;

            size_t added = 0;
            for (const ResolvedPlacement& item : placed)
            {
                if (added >= objectCap)
                    break;
                const auto shape = shapes.find(item.mesh);
                if (shape == shapes.end())
                    continue;
                // Enfusion's X/Z are the engine's X/Z; index 1 is up in both.
                float y = item.position[1];
                if (item.terrainRelative)
                {
                    // Against the RAW Enfusion heightfield, not SurfaceY: the engine's
                    // grid is resampled from 2 m to 6.25 m, so its surface is not the
                    // one these offsets were authored against, and on a slope the
                    // difference is what makes a tree float or sink.
                    const uint32_t sx = std::min<uint32_t>(
                        gridW - 1, static_cast<uint32_t>(item.position[0] / d.cellSize + 0.5f));
                    const uint32_t sz = std::min<uint32_t>(
                        gridH - 1, static_cast<uint32_t>(item.position[2] / d.cellSize + 0.5f));
                    const uint16_t raw = terrain.heights[static_cast<size_t>(sz) * gridW + sx];
                    y += static_cast<float>(raw) * d.heightScale + d.heightOffset;
                }
                AddObject(Vector3(item.position[0], y, item.position[2]), item.yawDeg, shape->second);
                ++added;
            }
            LOG_INFO(World,
                     "Enfusion native load: built {} of {} meshes ({} failed) in {} ms, placed {} objects "
                     "(caps {} models / {} objects){}",
                     built, meshes.size(), buildFailed, buildMs, added, modelCap, objectCap,
                     firstBuildFailure.empty() ? "" : ("; first build failure: " + firstBuildFailure));
        }
    }

    ProgressFrame();
    InitGeography();
    ProgressFinish();

    LOG_INFO(World, "Enfusion native load: done in {} ms -- land {}x{} at {:.2f} m, terrain {}x{} at {:.2f} m",
             Poseidon::Foundation::GlobalTickCount() - start, landRange, landRange, landGrid, terrainRange,
             terrainRange, terrainGrid);
    return true;
}
