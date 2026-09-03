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

#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
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
using Poseidon::Asset::Formats::Enfusion::PakArchive;
using Poseidon::Asset::Formats::Enfusion::PakEntry;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainDescriptor;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainTile;
using Poseidon::Asset::Formats::Enfusion::TerrainDescriptor;
using Poseidon::Asset::Formats::Enfusion::TerrainTile;

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
    std::string error;
};

EnfusionTerrain AssembleTerrain(const std::string& addonsDir)
{
    EnfusionTerrain out;

    std::vector<std::string> paks;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(addonsDir, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        if (LowerCopy(it->path().extension().string()) == ".pak")
            paks.push_back(it->path().string());
    }
    if (paks.empty())
    {
        out.error = "no .pak archives under '" + addonsDir + "'";
        return out;
    }

    // A world's data is not guaranteed to sit in one archive, so every archive is
    // searched and the tiles are collected by index across all of them.
    const std::string needle = LowerCopy(WantedWorld());
    std::vector<PakArchive> archives(paks.size());
    const PakEntry* descriptorEntry = nullptr;
    size_t descriptorArchive = 0;
    std::map<uint32_t, std::pair<size_t, const PakEntry*>> tiles;

    for (size_t i = 0; i < paks.size(); ++i)
    {
        if (!archives[i].Open(paks[i]))
            continue;
        for (const PakEntry& entry : archives[i].Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (lower.find(needle) == std::string::npos)
                continue;
            if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".terr") == 0)
            {
                descriptorEntry = &entry;
                descriptorArchive = i;
            }
            else if (lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".ttile") == 0)
            {
                // `<name>_<index>.ttile`; the index is row-major over the tile grid.
                const size_t underscore = lower.rfind('_');
                if (underscore == std::string::npos)
                    continue;
                const std::string digits = lower.substr(underscore + 1, lower.size() - underscore - 7);
                if (digits.empty() ||
                    !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                    continue;
                tiles[static_cast<uint32_t>(std::stoul(digits))] = {i, &entry};
            }
        }
    }

    if (descriptorEntry == nullptr)
    {
        out.error = "no .terr descriptor matching '" + std::string(WantedWorld()) + "'";
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!archives[descriptorArchive].Read(*descriptorEntry, bytes))
    {
        out.error = archives[descriptorArchive].Error();
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
    for (const auto& [index, where] : tiles)
    {
        std::vector<uint8_t> tileBytes;
        if (!archives[where.first].Read(*where.second, tileBytes))
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
    for (int z = 0; z < landRange; ++z)
    {
        for (int x = 0; x < landRange; ++x)
        {
            _geography(x, z) = GeographyInfo{};
            _tex(x, z) = 0;
        }
    }

    // No surface materials yet: the TMAT decoder is still tool-side. One empty entry
    // rather than none, so every cell's index 0 resolves to something.
    _texture.Resize(1);
    _a3TerrainMaterials.Resize(1);
    _a3TerrainMaterials[0] = A3TerrainMaterial{};

    _mountains.Resize(0);

    ProgressFrame();
    InitGeography();
    ProgressFinish();

    LOG_INFO(World, "Enfusion native load: done in {} ms -- land {}x{} at {:.2f} m, terrain {}x{} at {:.2f} m",
             Poseidon::Foundation::GlobalTickCount() - start, landRange, landRange, landGrid, terrainRange,
             terrainRange, terrainGrid);
    return true;
}
