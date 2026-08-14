#include "PakCommand.hpp"

#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Formats::Enfusion;

namespace PoseidonTools
{
namespace
{

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

//! Every `.pak` under a directory, recursively. Reforger keeps them in
//! `addons/core` and `addons/data`, and a world's tiles are spread across several.
std::vector<std::string> FindPaks(const std::string& root)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec))
    {
        out.push_back(root);
        return out;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec)
            break;
        if (it->is_regular_file(ec) && LowerCopy(it->path().extension().string()) == ".pak")
            out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int Inspect(const std::string& root)
{
    const std::vector<std::string> paks = FindPaks(root);
    if (paks.empty())
    {
        std::cerr << "Error: no .pak files under " << root << "\n";
        return 1;
    }

    size_t totalFiles = 0;
    size_t totalDirs = 0;
    size_t closedCount = 0;
    std::map<std::string, size_t> byExtension;

    std::cout << std::left << std::setw(24) << "archive" << std::setw(14) << "bytes" << std::setw(10) << "files"
              << std::setw(8) << "dirs" << "closure\n";
    std::cout << std::string(78, '-') << "\n";

    for (const std::string& path : paks)
    {
        PakArchive archive;
        if (!archive.Open(path))
        {
            std::cout << std::left << std::setw(24) << std::filesystem::path(path).filename().string()
                      << "FAILED: " << archive.Error() << "\n";
            continue;
        }
        const PakClosure& closure = archive.Closure();
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        std::cout << std::left << std::setw(24) << std::filesystem::path(path).filename().string() << std::setw(14)
                  << size << std::setw(10) << archive.Entries().size() << std::setw(8) << archive.DirectoryCount()
                  << (closure.complete() ? "closes" : "INCOMPLETE");
        if (!closure.complete())
        {
            std::cout << " (form=" << closure.formSizeCloses << " chunks=" << closure.chunkWalkCloses
                      << " dir=" << closure.directoryCloses << " tile=" << closure.dataTiles
                      << " gap=" << closure.gapBytes << " overlap=" << closure.overlaps << ")";
        }
        if (closure.nonZeroReserved)
            std::cout << " reserved!=0 in " << closure.nonZeroReserved;
        std::cout << "\n";

        if (closure.complete())
            closedCount++;
        totalFiles += archive.Entries().size();
        totalDirs += archive.DirectoryCount();
        for (const PakEntry& entry : archive.Entries())
        {
            const size_t dot = entry.path.rfind('.');
            byExtension[dot == std::string::npos ? "<none>" : LowerCopy(entry.path.substr(dot))]++;
        }
    }

    std::cout << "\n" << closedCount << " / " << paks.size() << " archives close on all four criteria.\n";
    std::cout << totalFiles << " files, " << totalDirs << " directories.\n\n";

    std::vector<std::pair<std::string, size_t>> sorted(byExtension.begin(), byExtension.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "Most common extensions:\n";
    for (size_t i = 0; i < sorted.size() && i < 14; ++i)
        std::cout << "  " << std::left << std::setw(12) << sorted[i].first << sorted[i].second << "\n";
    return 0;
}

int ListEntries(const std::string& root, const std::string& filter)
{
    const std::string needle = LowerCopy(filter);
    size_t shown = 0;
    for (const std::string& path : FindPaks(root))
    {
        PakArchive archive;
        if (!archive.Open(path))
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            if (!needle.empty() && LowerCopy(entry.path).find(needle) == std::string::npos)
                continue;
            std::cout << std::left << std::setw(22) << std::filesystem::path(path).filename().string()
                      << std::setw(12) << entry.realSize << (entry.compressed() ? "zlib  " : "store ") << entry.path
                      << "\n";
            shown++;
        }
    }
    std::cout << "\n" << shown << " entry(ies).\n";
    return 0;
}

//! Assembles a world's tiles into one heightfield and writes a shaded PNG.
//!
//! This is the end-to-end check on the C++ readers: it uses nothing but
//! PakArchive and the TERR readers, so a PNG that shows a recognisable coastline
//! proves the archive, the descriptor and the tile decode all agree. A numeric
//! summary alone would not -- a transposed tile order still produces sane
//! statistics, and only the picture (or the edge test) shows it.
int RenderTerrain(const std::string& root, const std::string& world, const std::string& outputPath, int maxSize)
{
    const std::vector<std::string> paks = FindPaks(root);
    if (paks.empty())
    {
        std::cerr << "Error: no .pak files under " << root << "\n";
        return 1;
    }

    // Locate the descriptor and every tile belonging to this world, across all
    // archives -- a world's data is not guaranteed to sit in one of them.
    const std::string needle = LowerCopy(world);
    std::vector<PakArchive> archives(paks.size());
    const PakEntry* descriptorEntry = nullptr;
    size_t descriptorArchive = 0;
    std::map<uint32_t, std::pair<size_t, const PakEntry*>> tiles; // index -> (archive, entry)

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
                // `<name>_<index>.ttile`
                const size_t underscore = lower.rfind('_');
                if (underscore == std::string::npos)
                    continue;
                const std::string digits = lower.substr(underscore + 1, lower.size() - underscore - 7);
                if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                                   [](unsigned char c) { return std::isdigit(c) != 0; }))
                    continue;
                tiles[static_cast<uint32_t>(std::stoul(digits))] = {i, &entry};
            }
        }
    }

    if (!descriptorEntry)
    {
        std::cerr << "Error: no .terr descriptor matching '" << world << "'\n";
        return 1;
    }
    std::vector<uint8_t> bytes;
    if (!archives[descriptorArchive].Read(*descriptorEntry, bytes))
    {
        std::cerr << "Error: " << archives[descriptorArchive].Error() << "\n";
        return 1;
    }
    const TerrainDescriptor descriptor = ReadTerrainDescriptor(bytes.data(), bytes.size());
    if (!descriptor.valid())
    {
        std::cerr << "Error: " << descriptor.error << "\n";
        return 1;
    }

    std::cout << "Descriptor:  " << descriptorEntry->path << "\n";
    std::cout << "Version:     " << descriptor.version << "\n";
    std::cout << "Grid:        " << descriptor.gridWidth << " x " << descriptor.gridHeight << " @ " << std::fixed
              << std::setprecision(3) << descriptor.cellSize << " m  = " << std::setprecision(0)
              << descriptor.WorldWidth() << " x " << descriptor.WorldHeight() << " m\n";
    std::cout << "Height:      raw * " << std::setprecision(8) << descriptor.heightScale << " + "
              << descriptor.heightOffset << "  (sea level at raw "
              << static_cast<long>(-descriptor.heightOffset / descriptor.heightScale + 0.5f) << ")\n";
    std::cout << "Materials:   " << descriptor.materials.size() << "\n";
    std::cout << "Tiles found: " << tiles.size() << "\n";
    if (tiles.empty())
    {
        std::cerr << "Error: no .ttile files matching '" << world << "'\n";
        return 1;
    }

    const uint32_t gridW = descriptor.gridWidth;
    const uint32_t gridH = descriptor.gridHeight;
    std::vector<uint16_t> heights(static_cast<size_t>(gridW) * gridH, 0);

    // The tile edge comes from the tiles themselves; the descriptor does not carry
    // it and it is not constant across the corpus.
    uint32_t tileEdge = 0;
    uint32_t tilesX = 0;
    size_t placed = 0;
    size_t failed = 0;

    for (const auto& [index, where] : tiles)
    {
        std::vector<uint8_t> tileBytes;
        if (!archives[where.first].Read(*where.second, tileBytes))
        {
            failed++;
            continue;
        }
        const TerrainTile tile = ReadTerrainTile(tileBytes.data(), tileBytes.size());
        if (!tile.valid())
        {
            failed++;
            continue;
        }
        if (tileEdge == 0)
        {
            tileEdge = tile.samples - 1;
            tilesX = (gridW - 1) / tileEdge;
        }
        const uint32_t row = index / tilesX;
        const uint32_t col = index % tilesX;
        const uint32_t originX = col * tileEdge;
        const uint32_t originZ = row * tileEdge;
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
                heights[static_cast<size_t>(worldZ) * gridW + worldX] = tile.At(x, z);
            }
        }
        placed++;
    }

    std::cout << "Tiles read:  " << placed << " / " << tiles.size();
    if (failed)
        std::cout << "  (" << failed << " FAILED)";
    std::cout << "  tileEdge=" << tileEdge << " tilesX=" << tilesX << "\n";

    uint16_t rawMin = 0xFFFF;
    uint16_t rawMax = 0;
    for (uint16_t value : heights)
    {
        rawMin = std::min(rawMin, value);
        rawMax = std::max(rawMax, value);
    }
    std::cout << "Elevation:   " << std::setprecision(2) << descriptor.HeightAt(rawMin) << " m .. "
              << descriptor.HeightAt(rawMax) << " m\n";

    // Downsample by an integer stride so the output stays honest -- a resampled
    // heightfield would smear the coastline that is the whole point of looking.
    const uint32_t stride = std::max<uint32_t>(1, (std::max(gridW, gridH) + static_cast<uint32_t>(maxSize) - 1) /
                                                      static_cast<uint32_t>(maxSize));
    const uint32_t outW = (gridW + stride - 1) / stride;
    const uint32_t outH = (gridH + stride - 1) / stride;
    std::vector<uint8_t> rgba(static_cast<size_t>(outW) * outH * 4, 0);

    const float seaLevelRaw = -descriptor.heightOffset / descriptor.heightScale;
    // Lambertian hillshade from the north-west, the cartographic convention.
    const float sunX = -0.5f, sunY = 0.7071f, sunZ = 0.5f;
    const float spacing = descriptor.cellSize * static_cast<float>(stride);

    for (uint32_t y = 0; y < outH; ++y)
    {
        for (uint32_t x = 0; x < outW; ++x)
        {
            const uint32_t sx = std::min(x * stride, gridW - 1);
            const uint32_t sz = std::min(y * stride, gridH - 1);
            const float here = descriptor.HeightAt(heights[static_cast<size_t>(sz) * gridW + sx]);

            const uint32_t xPrev = sx > stride ? sx - stride : 0;
            const uint32_t xNext = std::min(sx + stride, gridW - 1);
            const uint32_t zPrev = sz > stride ? sz - stride : 0;
            const uint32_t zNext = std::min(sz + stride, gridH - 1);
            const float dx = descriptor.HeightAt(heights[static_cast<size_t>(sz) * gridW + xNext]) -
                             descriptor.HeightAt(heights[static_cast<size_t>(sz) * gridW + xPrev]);
            const float dz = descriptor.HeightAt(heights[static_cast<size_t>(zNext) * gridW + sx]) -
                             descriptor.HeightAt(heights[static_cast<size_t>(zPrev) * gridW + sx]);
            float nx = -dx / (2.0f * spacing);
            float nz = -dz / (2.0f * spacing);
            const float length = std::sqrt(nx * nx + nz * nz + 1.0f);
            nx /= length;
            nz /= length;
            const float ny = 1.0f / length;
            float shade = nx * sunX + ny * sunY + nz * sunZ;
            shade = std::clamp(0.35f + 0.75f * shade, 0.0f, 1.35f);

            float r, g, b;
            const bool underwater = heights[static_cast<size_t>(sz) * gridW + sx] < seaLevelRaw;
            if (underwater)
            {
                // Depth-shaded water, so the shelf around the island reads.
                const float depth = std::clamp(-here / 60.0f, 0.0f, 1.0f);
                r = 0.05f + 0.10f * (1.0f - depth);
                g = 0.16f + 0.26f * (1.0f - depth);
                b = 0.28f + 0.34f * (1.0f - depth);
                shade = 1.0f;
            }
            else
            {
                // A plain hypsometric ramp: beach, lowland, upland, rock.
                const float t = std::clamp(here / 380.0f, 0.0f, 1.0f);
                if (t < 0.04f)
                {
                    r = 0.80f; g = 0.76f; b = 0.58f;
                }
                else if (t < 0.35f)
                {
                    const float k = (t - 0.04f) / 0.31f;
                    r = 0.36f + 0.22f * k; g = 0.52f - 0.06f * k; b = 0.24f + 0.06f * k;
                }
                else
                {
                    const float k = (t - 0.35f) / 0.65f;
                    r = 0.58f + 0.30f * k; g = 0.46f + 0.40f * k; b = 0.30f + 0.46f * k;
                }
            }

            const size_t at = (static_cast<size_t>(y) * outW + x) * 4;
            rgba[at + 0] = static_cast<uint8_t>(std::clamp(r * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 1] = static_cast<uint8_t>(std::clamp(g * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 2] = static_cast<uint8_t>(std::clamp(b * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 3] = 255;
        }
    }

    if (!Poseidon::PNGWriter::WriteRGBA(outputPath.c_str(), static_cast<int>(outW), static_cast<int>(outH), rgba.data()))
    {
        std::cerr << "Error: could not write " << outputPath << "\n";
        return 1;
    }
    std::cout << "Wrote " << outputPath << " (" << outW << " x " << outH << ", stride " << stride << ")\n";
    return 0;
}

} // namespace

void PakCommand::Setup(CLI::App& app)
{
    auto* pak = app.add_subcommand("pak", "Enfusion .pak archives and their terrain (Arma Reforger)");
    pak->require_subcommand(1);
    {
        static std::string root;
        auto* cmd = pak->add_subcommand("inspect", "Report byte-accounting closure over every .pak under a path");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->callback([]() { std::exit(Inspect(root)); });
    }
    {
        static std::string root;
        static std::string filter;
        auto* cmd = pak->add_subcommand("list", "List archive entries");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring (case-insensitive)");
        cmd->callback([]() { std::exit(ListEntries(root, filter)); });
    }
    {
        static std::string root;
        static std::string world;
        static std::string output;
        static int maxSize = 2048;
        auto* cmd = pak->add_subcommand("terrain", "Assemble a world's .ttile heightfield and render it to PNG");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-w,--world", world, "Substring naming the world, e.g. worlds/Eden")->required();
        cmd->add_option("-o,--output", output, "Output PNG path")->required();
        cmd->add_option("--max-size", maxSize, "Longest output edge in pixels (default 2048)");
        cmd->callback([]() { std::exit(RenderTerrain(root, world, output, maxSize)); });
    }
}

} // namespace PoseidonTools
