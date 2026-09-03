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

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainMaterials.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Core/ProgressSystem.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
    const int landRange = 256;
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
    for (int i = 0; i < paletteSize; ++i)
        _a3TerrainMaterials[i] = A3TerrainMaterial{};

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
        LOG_INFO(World,
                 "Enfusion native load: surfaces {} of {} palette entries used over {} land cells "
                 "({} sub-cells, {} failed{}); commonest: {}",
                 ranked.size(), d.materials.size(), cells, terrain.subCells, terrain.subCellsFailed,
                 terrain.materialError.empty() ? "" : (", first error: " + terrain.materialError), top);
    }

    _mountains.Resize(0);

    ProgressFrame();
    InitGeography();
    ProgressFinish();

    LOG_INFO(World, "Enfusion native load: done in {} ms -- land {}x{} at {:.2f} m, terrain {}x{} at {:.2f} m",
             Poseidon::Foundation::GlobalTickCount() - start, landRange, landRange, landGrid, terrainRange,
             terrainRange, terrainGrid);
    return true;
}
