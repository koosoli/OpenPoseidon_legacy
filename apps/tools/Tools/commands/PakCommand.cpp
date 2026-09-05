#include "PakCommand.hpp"

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainMaterials.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ClutterSet.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
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
            std::cout << std::left << std::setw(22) << std::filesystem::path(path).filename().string() << std::setw(12)
                      << entry.realSize << (entry.compressed() ? "zlib  " : "store ") << entry.path << "\n";
            shown++;
        }
    }
    std::cout << "\n" << shown << " entry(ies).\n";
    return 0;
}

//! Reads every `.ent` under the archives and reports EBIN closure plus placements.
//!
//! The check that matters is byte accounting over the WHOLE set: a record-stream
//! walk that is one field width wrong still consumes most small files and only
//! diverges on the large ones, so a spot check passes where the census does not.
int Worlds(const std::string& root, const std::string& filter, int topClasses, bool roads)
{
    size_t files = 0, closed = 0, placements = 0;
    std::map<std::string, size_t> classCounts;
    uint64_t declaredBytes = 0, consumedBytes = 0;
    const std::string needle = LowerCopy(filter);

    for (const std::string& path : FindPaks(root))
    {
        PakArchive archive;
        if (!archive.Open(path))
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".ent") != 0)
                continue;
            if (!needle.empty() && lower.find(needle) == std::string::npos)
                continue;
            std::vector<uint8_t> bytes;
            if (!archive.Read(entry, bytes))
            {
                std::cout << "  READ FAILED " << entry.path << ": " << archive.Error() << "\n";
                continue;
            }
            files++;
            declaredBytes += bytes.size();
            const auto world = ReadEbin(bytes.data(), bytes.size());
            if (world.valid())
            {
                closed++;
                consumedBytes += world.consumed;
                placements += world.placements.size();
                if (topClasses > 0)
                    for (const auto& item : world.placements)
                        ++classCounts[item.className];
                if (roads)
                {
                    // Where the towns are, measured rather than guessed: the densest
                    // 200 m cells of building placements. Framing a capture by eye
                    // costs a two-minute run per attempt and most attempts miss.
                    std::map<std::pair<int, int>, size_t> townCells;
                    for (const auto& item : world.placements)
                        if (item.className == "SCR_DestructibleBuildingEntity")
                            ++townCells[{int(item.position[0] / 200.0f), int(item.position[2] / 200.0f)}];
                    std::vector<std::pair<size_t, std::pair<int, int>>> towns;
                    for (const auto& [cell, count] : townCells)
                        towns.emplace_back(count, cell);
                    std::sort(towns.begin(), towns.end(), std::greater<>());
                    for (size_t i = 0; i < towns.size() && i < 8; ++i)
                        std::cout << "    town " << towns[i].first << " buildings at "
                                  << towns[i].second.first * 200 + 100 << " "
                                  << towns[i].second.second * 200 + 100 << std::endl;
                }
                if (roads)
                {
                    size_t withPoints = 0, without = 0, totalPoints = 0, withMaterial = 0, withWidth = 0, withPrefab = 0;
                    std::vector<std::pair<double, std::string>> longest;
                    double totalLength = 0.0;
                    size_t shown = 0;
                    for (const auto& item : world.placements)
                    {
                        if (!IsRoadSplineClass(item.className))
                            continue;
                        if (item.points.empty())
                        {
                            ++without;
                            continue;
                        }
                        ++withPoints;
                        const size_t count = item.points.size() / 3;
                        totalPoints += count;
                        for (size_t i = 1; i < count; ++i)
                        {
                            const float dx = item.points[i * 3 + 0] - item.points[(i - 1) * 3 + 0];
                            const float dy = item.points[i * 3 + 1] - item.points[(i - 1) * 3 + 1];
                            const float dz = item.points[i * 3 + 2] - item.points[(i - 1) * 3 + 2];
                            totalLength += std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
                        }
                        if (!item.materialGuid.empty())
                            ++withMaterial;
                        if (!item.prefabGuid.empty())
                            ++withPrefab;
                        if (item.width > 0.0f)
                            ++withWidth;
                        {
                            double len = 0.0;
                            for (size_t i = 1; i < count; ++i)
                            {
                                const float dx = item.points[i * 3 + 0] - item.points[(i - 1) * 3 + 0];
                                const float dz = item.points[i * 3 + 2] - item.points[(i - 1) * 3 + 2];
                                len += std::sqrt(double(dx) * dx + double(dz) * dz);
                            }
                            const size_t mid = count / 2;
                            longest.emplace_back(len, std::to_string(int(item.position[0] + item.points[mid * 3 + 0])) +
                                                          " " +
                                                          std::to_string(int(item.position[2] + item.points[mid * 3 + 2])) +
                                                          " w" + std::to_string(int(item.width)) + " n" +
                                                          std::to_string(count));
                        }
                        if (shown < 3)
                        {
                            ++shown;
                            std::cout << "    road " << item.className << " prefab '" << item.prefabGuid
                                      << "' material '" << item.materialGuid << "' width " << item.width << " at [" << int(item.position[0]) << " "
                                      << int(item.position[1]) << " " << int(item.position[2]) << "] " << count
                                      << " points, first local [" << item.points[0] << " " << item.points[1] << " "
                                      << item.points[2] << "]\n";
                        }
                    }
                    // Where the houses are. A 200 m cell histogram over the building
                    // classes, because "fly somewhere and look" costs a five-minute run
                    // per guess and the file already knows the answer.
                    {
                        std::map<std::pair<int, int>, size_t> cells;
                        for (const auto& q : world.placements)
                        {
                            if (q.className.find("Building") == std::string::npos &&
                                q.className.find("StaticModel") == std::string::npos)
                                continue;
                            cells[{int(q.position[0] / 200.0f), int(q.position[2] / 200.0f)}]++;
                        }
                        std::vector<std::pair<size_t, std::pair<int, int>>> best;
                        (void)0;
                        for (const auto& [cell, count] : cells)
                            best.emplace_back(count, cell);
                        std::sort(best.begin(), best.end(), std::greater<>());
                        for (size_t i = 0; i < best.size() && i < 6; ++i)
                            std::cout << "    densest " << best[i].first << " buildings at ["
                                      << best[i].second.first * 200 + 100 << " "
                                      << best[i].second.second * 200 + 100 << "]" << std::endl;
                        // Individual buildings in the densest cell, so a camera can be
                        // put in front of one instead of hunting for it at five minutes
                        // a guess.
                        if (!best.empty())
                        {
                            size_t shownHere = 0;
                            for (const auto& q : world.placements)
                            {
                                if (shownHere >= 6)
                                    break;
                                if (q.className.find("Building") == std::string::npos)
                                    continue;
                                if (int(q.position[0] / 200.0f) != best[0].second.first ||
                                    int(q.position[2] / 200.0f) != best[0].second.second)
                                    continue;
                                std::cout << "      building at [" << int(q.position[0]) << " "
                                          << int(q.position[1]) << " " << int(q.position[2])
                                          << "] yaw " << int(q.angles[1]) << std::endl;
                                ++shownHere;
                            }
                        }
                    }
                    // RFG-053: does the recorded yaw agree with the direction a run of
                    // modular pieces actually runs in?
                    //
                    // Walkways, jetties, fences and retaining walls are built from
                    // repeated modules laid end to end, so a run of the SAME prefab whose
                    // neighbours sit one module apart is a ruler: the bearing from one to
                    // the next is the direction the run goes, and the piece has to face
                    // along it. Histogramming (bearing - yaw) over thousands of such
                    // pairs says whether the convention is right, off by a constant, or
                    // mirrored -- which a screenshot of a crooked fence cannot.
                    {
                        std::map<std::string, std::vector<const EbinPlacement*>> byPrefab;
                        for (const auto& q : world.placements)
                            if (!q.prefabGuid.empty() && q.hasPosition)
                                byPrefab[q.prefabGuid].push_back(&q);

                        std::map<int, size_t> offsets; // (bearing - yaw) in 15-degree bins
                        size_t pairs = 0;
                        for (const auto& [guid, list] : byPrefab)
                        {
                            if (list.size() < 8)
                                continue;
                            for (size_t a = 0; a < list.size(); ++a)
                            {
                                // Nearest neighbour of the same prefab, within one module.
                                double bestSq = 1e30;
                                const EbinPlacement* mate = nullptr;
                                for (size_t b = 0; b < list.size(); ++b)
                                {
                                    if (b == a)
                                        continue;
                                    const double dx = list[b]->position[0] - list[a]->position[0];
                                    const double dz = list[b]->position[2] - list[a]->position[2];
                                    const double sq = dx * dx + dz * dz;
                                    if (sq < bestSq)
                                    {
                                        bestSq = sq;
                                        mate = list[b];
                                    }
                                }
                                if (mate == nullptr || bestSq < 0.25 || bestSq > 144.0)
                                    continue; // touching duplicates, or not a run
                                // SAME HEADING, or it is not a run. Without this the
                                // "nearest copy of the same prefab" is any scattered
                                // instance -- a hedge, a bollard, a rock -- and the
                                // histogram comes out flat at 8-9% per bin, which is
                                // exactly what a uniform distribution looks like and
                                // says nothing about the convention. A flat result from
                                // a loose test is not evidence of no effect.
                                if (std::fabs(mate->angles[1] - list[a]->angles[1]) > 1.0f)
                                    continue;
                                const double dx = mate->position[0] - list[a]->position[0];
                                const double dz = mate->position[2] - list[a]->position[2];
                                double bearing = std::atan2(dx, dz) * 57.29577951;
                                double delta = bearing - list[a]->angles[1];
                                while (delta < 0.0)
                                    delta += 360.0;
                                while (delta >= 360.0)
                                    delta -= 360.0;
                                // A module may run either way along its own axis, so fold
                                // 180 degrees together -- otherwise every run splits its
                                // votes between two bins and neither looks decisive.
                                if (delta >= 180.0)
                                    delta -= 180.0;
                                offsets[int(delta / 15.0) * 15]++;
                                ++pairs;
                            }
                        }
                        std::vector<std::pair<size_t, int>> ranked;
                        for (const auto& [bin, count] : offsets)
                            ranked.emplace_back(count, bin);
                        std::sort(ranked.begin(), ranked.end(), std::greater<>());
                        // How many placements actually use pitch or roll? The reader
                        // keeps a Euler triple and every consumer downstream reads index
                        // 1 and throws the other two away. If a walkway module is tilted
                        // to follow a slope, that tilt is in the two being discarded --
                        // and the converted map shows the same defect, which is what
                        // says the fault is in the shared reduction and not in either
                        // map's own placement code.
                        {
                            size_t tilted = 0, total = 0;
                            std::string worstName;
                            float worst = 0.0f;
                            for (const auto& q : world.placements)
                            {
                                if (!q.hasPosition)
                                    continue;
                                ++total;
                                const float tilt = std::max(std::fabs(q.angles[0]), std::fabs(q.angles[2]));
                                if (tilt > 0.5f)
                                {
                                    ++tilted;
                                    if (tilt > worst)
                                    {
                                        worst = tilt;
                                        worstName = q.className;
                                    }
                                }
                            }
                            std::cout << "    tilt: " << tilted << " of " << total << " placements carry pitch or roll ("
                                      << (total ? 100.0 * double(tilted) / double(total) : 0.0)
                                      << "%), worst " << worst << " deg on " << worstName << std::endl;
                        }
                        std::cout << "    module runs: " << pairs << " neighbour pairs; (bearing - yaw) mod 180:"
                                  << std::endl;
                        for (size_t i = 0; i < ranked.size() && i < 5; ++i)
                            std::cout << "      " << ranked[i].second << "-" << ranked[i].second + 15 << " deg: "
                                      << ranked[i].first << " ("
                                      << (pairs ? 100.0 * double(ranked[i].first) / double(pairs) : 0.0) << "%)"
                                      << std::endl;
                    }
                    std::sort(longest.begin(), longest.end(), std::greater<>());
                    for (size_t i = 0; i < longest.size() && i < 6; ++i)
                        std::cout << "    longest " << int(longest[i].first) << " m at " << longest[i].second << std::endl;
                    std::cout << "    roads: " << withMaterial << " with a material GUID, " << withWidth
                              << " with a width, " << withPrefab << " with a prefab GUID\n";
                    std::cout << "    roads: " << withPoints << " with a centreline, " << without << " without, "
                              << totalPoints << " points, " << std::fixed << std::setprecision(1)
                              << totalLength / 1000.0 << " km\n"
                              << std::defaultfloat;
                }
                if (world.placements.size() > 10000)
                    std::cout << "  " << std::left << std::setw(46) << entry.path << std::setw(12)
                              << world.placements.size() << " placements, " << world.entityBlocks << " entity blocks\n";
            }
            else
            {
                std::cout << "  NOT CLOSED  " << entry.path << ": " << world.error << "\n";
            }
        }
    }
    std::cout << "\n" << closed << " / " << files << " .ent files close their byte accounting.\n";
    std::cout << consumedBytes << " / " << declaredBytes << " bytes accounted for.\n";
    std::cout << placements << " placements with a transform.\n";
    if (topClasses > 0 && !classCounts.empty())
    {
        // A class histogram, because "the roads are missing" is a claim about a class
        // name and nothing else in this tool can say whether the name is even present.
        std::vector<std::pair<size_t, std::string>> ranked;
        ranked.reserve(classCounts.size());
        for (const auto& [name, count] : classCounts)
            ranked.emplace_back(count, name);
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        std::cout << "\n" << ranked.size() << " distinct classes; top " << topClasses << ":\n";
        for (int i = 0; i < topClasses && i < static_cast<int>(ranked.size()); ++i)
            std::cout << "  " << std::setw(10) << ranked[i].first << "  " << ranked[i].second << "\n";
    }
    return closed == files ? 0 : 1;
}


//! One world's descriptor plus its tiles assembled into a single heightfield.
//!
//! Shared by `pak terrain` (which shades it to a PNG) and `pak export` (which
//! resamples it into a .wrp), so both see exactly the same bytes and a tile-order
//! mistake cannot show up in one and not the other.
struct AssembledTerrain
{
    TerrainDescriptor descriptor;
    std::string descriptorPath;
    std::vector<uint16_t> heights; //!< gridWidth * gridHeight, raw units, row-major, X fastest
    uint32_t tileEdge = 0;
    uint32_t tilesX = 0;
    size_t tilesFound = 0;
    size_t tilesPlaced = 0;
    size_t tilesFailed = 0;
    //! MATS index per mask texel, `materialEdge` square, row 0 at Z 0 like the
    //! heightfield. 0xFF where no BMAT covered it. Only filled when asked for.
    std::vector<uint8_t> materials;
    uint32_t materialEdge = 0;
    size_t subCells = 0;
    size_t subCellsFailed = 0;
    std::string materialError; //!< first blend-decode failure, for the summary
    std::string error;

    bool valid() const { return error.empty(); }
    float HeightAt(uint32_t x, uint32_t z) const
    {
        return descriptor.HeightAt(heights[static_cast<size_t>(z) * descriptor.gridWidth + x]);
    }
};

AssembledTerrain AssembleTerrain(const std::string& root, const std::string& world, bool wantMaterials = false)
{
    AssembledTerrain out;
    const std::vector<std::string> paks = FindPaks(root);
    if (paks.empty())
    {
        out.error = "no .pak files under " + root;
        return out;
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
                if (digits.empty() ||
                    !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
                    continue;
                tiles[static_cast<uint32_t>(std::stoul(digits))] = {i, &entry};
            }
        }
    }

    if (!descriptorEntry)
    {
        out.error = "no .terr descriptor matching '" + world + "'";
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!archives[descriptorArchive].Read(*descriptorEntry, bytes))
    {
        out.error = archives[descriptorArchive].Error();
        return out;
    }
    out.descriptorPath = descriptorEntry->path;
    out.descriptor = ReadTerrainDescriptor(bytes.data(), bytes.size());
    if (!out.descriptor.valid())
    {
        out.error = out.descriptor.error;
        return out;
    }
    out.tilesFound = tiles.size();
    if (tiles.empty())
    {
        out.error = "no .ttile files matching '" + world + "'";
        return out;
    }

    const uint32_t gridW = out.descriptor.gridWidth;
    const uint32_t gridH = out.descriptor.gridHeight;
    out.heights.assign(static_cast<size_t>(gridW) * gridH, 0);

    // The tile edge comes from the tiles themselves; the descriptor does not carry
    // it and it is not constant across the corpus.
    for (const auto& [index, where] : tiles)
    {
        std::vector<uint8_t> tileBytes;
        if (!archives[where.first].Read(*where.second, tileBytes))
        {
            out.tilesFailed++;
            continue;
        }
        const TerrainTile tile = ReadTerrainTile(tileBytes.data(), tileBytes.size());
        if (!tile.valid())
        {
            out.tilesFailed++;
            continue;
        }
        if (out.tileEdge == 0)
        {
            out.tileEdge = tile.samples - 1;
            out.tilesX = (gridW - 1) / out.tileEdge;
            if (wantMaterials && out.descriptor.subPerTileEdge > 0)
            {
                out.materialEdge = out.tilesX * out.descriptor.subPerTileEdge * Tmat::kMaskEdge;
                out.materials.assign(static_cast<size_t>(out.materialEdge) * out.materialEdge, 0xFF);
            }
        }
        const uint32_t row = index / out.tilesX;
        const uint32_t col = index % out.tilesX;
        const uint32_t originX = col * out.tileEdge;
        const uint32_t originZ = row * out.tileEdge;
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
        out.tilesPlaced++;

        // The surface assignment rides along here rather than in a second pass:
        // the tile bytes are already decompressed and in hand, and re-reading
        // 2,500 tiles to get at a chunk we already hold is pure cost.
        if (!out.materials.empty())
        {
            const IffFile iff = ReadIff(tileBytes.data(), tileBytes.size());
            const IffChunk* tmat = iff.valid() ? iff.Find(FourCC("TMAT")) : nullptr;
            if (tmat == nullptr)
                continue;
            std::vector<Tmat::SubCell> cells;
            std::string error;
            if (!Tmat::ReadSubCells(tileBytes.data() + tmat->offset, tmat->size, cells, error))
            {
                if (out.materialError.empty())
                    out.materialError = error;
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
                else if (!Tmat::DecodeBlend(cell.blend, cell.blendSize, cell.palette.size(), mask.data(), error))
                {
                    ++out.subCellsFailed;
                    if (out.materialError.empty())
                        out.materialError = error;
                    mask.fill(0);
                }
                for (int mz = 0; mz < Tmat::kMaskEdge; ++mz)
                    for (int mx = 0; mx < Tmat::kMaskEdge; ++mx)
                    {
                        const uint8_t entry = mask[static_cast<size_t>(mz) * Tmat::kMaskEdge + mx];
                        out.materials[(baseZ + mz) * out.materialEdge + baseX + mx] =
                            static_cast<uint8_t>(cell.palette[entry]);
                    }
            }
        }
    }
    return out;
}

void PrintTerrainSummary(const AssembledTerrain& terrain)
{
    const TerrainDescriptor& descriptor = terrain.descriptor;
    std::cout << "Descriptor:  " << terrain.descriptorPath << "\n";
    std::cout << "Version:     " << descriptor.version << "\n";
    std::cout << "Grid:        " << descriptor.gridWidth << " x " << descriptor.gridHeight << " @ " << std::fixed
              << std::setprecision(3) << descriptor.cellSize << " m  = " << std::setprecision(0)
              << descriptor.WorldWidth() << " x " << descriptor.WorldHeight() << " m\n";
    std::cout << "Height:      raw * " << std::setprecision(8) << descriptor.heightScale << " + "
              << descriptor.heightOffset << "  (sea level at raw "
              << static_cast<long>(-descriptor.heightOffset / descriptor.heightScale + 0.5f) << ")\n";
    std::cout << "Materials:   " << descriptor.materials.size() << "\n";
    std::cout << "Tiles found: " << terrain.tilesFound << "\n";
    std::cout << "Tiles read:  " << terrain.tilesPlaced << " / " << terrain.tilesFound;
    if (terrain.tilesFailed)
        std::cout << "  (" << terrain.tilesFailed << " FAILED)";
    std::cout << "  tileEdge=" << terrain.tileEdge << " tilesX=" << terrain.tilesX << "\n";
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
    const AssembledTerrain terrain = AssembleTerrain(root, world);
    if (!terrain.valid())
    {
        std::cerr << "Error: " << terrain.error << "\n";
        return 1;
    }
    PrintTerrainSummary(terrain);

    const TerrainDescriptor& descriptor = terrain.descriptor;
    const std::vector<uint16_t>& heights = terrain.heights;
    const uint32_t gridW = descriptor.gridWidth;
    const uint32_t gridH = descriptor.gridHeight;

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
                    r = 0.80f;
                    g = 0.76f;
                    b = 0.58f;
                }
                else if (t < 0.35f)
                {
                    const float k = (t - 0.04f) / 0.31f;
                    r = 0.36f + 0.22f * k;
                    g = 0.52f - 0.06f * k;
                    b = 0.24f + 0.06f * k;
                }
                else
                {
                    const float k = (t - 0.35f) / 0.65f;
                    r = 0.58f + 0.30f * k;
                    g = 0.46f + 0.40f * k;
                    b = 0.30f + 0.46f * k;
                }
            }

            const size_t at = (static_cast<size_t>(y) * outW + x) * 4;
            rgba[at + 0] = static_cast<uint8_t>(std::clamp(r * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 1] = static_cast<uint8_t>(std::clamp(g * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 2] = static_cast<uint8_t>(std::clamp(b * shade, 0.0f, 1.0f) * 255.0f);
            rgba[at + 3] = 255;
        }
    }

    if (!Poseidon::PNGWriter::WriteRGBA(outputPath.c_str(), static_cast<int>(outW), static_cast<int>(outH),
                                        rgba.data()))
    {
        std::cerr << "Error: could not write " << outputPath << "\n";
        return 1;
    }
    std::cout << "Wrote " << outputPath << " (" << outW << " x " << outH << ", stride " << stride << ")\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Reforger world -> OPRW revision 25
// ---------------------------------------------------------------------------
//
// Nothing in this engine reads Enfusion's `.terr`/`.ttile` terrain at runtime, and
// teaching it to would mean a new world type threaded through the whole terrain
// core. This instead SYNTHESISES a container the runtime already loads, so the
// existing landscape, LOD, lighting and WGPU paths are reused unchanged.
//
// The container is OPRW revision 25 and the reader in
// `Poseidon/Asset/Formats/World/Oprw25.hpp` is the spec this writes against --
// field for field, in its field order. Revision 25 rather than the OFP-era OPRW 3
// because `--test-world` reaches ONLY `Landscape::LoadOprwModern`
// (LandSave.cpp:274-285), which accepts {18, 20, 24, 25, 29} and nothing else; an
// OPRW 3 file would be refused before a single height was read.
//
// Geometry: land 256 / terrain 2048 / landCellSize 50 m gives 6.25 m terrain cells
// over exactly 12800 m, which is Everon's exact extent (6401 samples at 2.0 m) and
// is the same shape as DayZ's Enoch, a world this build already boots. The land and
// terrain ranges cannot be freely chosen: `Landscape::Dim` requires both to be
// square powers of two and `LoadOprwModern` requires terrain to be a whole multiple
// of land.
//
// The elevation is a resample, and that is the one real loss: Everon is authored at
// 2.0 m and this carries 6.25 m, so features narrower than about 12 m do not
// survive. It is bilinear rather than nearest so a ridge line does not alias into a
// staircase. Everything else in the file is either exact (the extent, the height
// scale) or synthesised and marked as such below.
// ===========================================================================
//  Terrain surface materials
// ===========================================================================
//
// Everon's ground is authored as 22 Enfusion surface materials plus a per-square-
// metre assignment (TMAT). The runtime wants the Arma 3 shape instead: one
// `TerrainSNX` RVMAT per land-cell region, holding a satellite colour image, an
// indexed selector mask, and a stack of at most five tiling ground surfaces.
// This translates one into the other.
namespace Surfaces
{

//! Every `.pak` under a root, opened once, with a case-insensitive path index.
//! `PakArchive` resolves within a single archive only, and a Reforger world's
//! materials and textures are spread across several.
class PakSet
{
  public:
    bool Open(const std::string& root)
    {
        const std::vector<std::string> paks = FindPaks(root);
        if (paks.empty())
            return false;
        _archives.resize(paks.size());
        for (size_t i = 0; i < paks.size(); ++i)
        {
            if (!_archives[i].Open(paks[i]))
                continue;
            for (const PakEntry& entry : _archives[i].Entries())
                _index.emplace(LowerCopy(entry.path), std::make_pair(i, &entry));
        }
        return !_index.empty();
    }

    size_t Size() const { return _index.size(); }

    //! Read by virtual path, case-insensitively, with either separator.
    bool Read(const std::string& path, std::vector<uint8_t>& out) const
    {
        std::string key = LowerCopy(path);
        std::replace(key.begin(), key.end(), '\\', '/');
        const auto found = _index.find(key);
        if (found == _index.end())
            return false;
        return _archives[found->second.first].Read(*found->second.second, out);
    }

  private:
    std::vector<PakArchive> _archives;
    std::map<std::string, std::pair<size_t, const PakEntry*>> _index;
};

// ------------------------------------------------------------------ raP -----
//
// RVMATs are the MATERIAL flavour of `\0raP`, not the config flavour, and the
// difference is not cosmetic: `PoseidonTools config bin` writes version 4 with
// the source path embedded, and `ParseArmaRapMaterial` throws on it inside a
// catch that swallows the exception (LandSave.cpp), so the world loads with a
// material table full of nothing and the ground draws white with no diagnostic.
//
// The material flavour is version 0, then `08`, then a u32 offset to the enum
// table, with the root class body at 0x10. Verified against a shipped Arma 3
// terrain RVMAT (packages/a3-compat/world/vr/data/layers/p_000-000_l00.rvmat).
namespace Rap
{

struct Value
{
    enum Kind
    {
        String,
        Float,
        Int
    };
    Kind kind = Float;
    std::string text;
    float number = 0.0f;
    int integer = 0;

    static Value Str(std::string v) { return Value{String, std::move(v), 0.0f, 0}; }
    static Value F32(float v) { return Value{Float, {}, v, 0}; }
    static Value I32(int v) { return Value{Int, {}, 0.0f, v}; }
};

struct Class
{
    std::string name;
    std::vector<std::pair<std::string, std::vector<Value>>> arrays;
    std::vector<std::pair<std::string, Value>> values;
    std::vector<Class> classes;

    Class& Array(std::string name, std::vector<Value> v)
    {
        arrays.emplace_back(std::move(name), std::move(v));
        return *this;
    }
    Class& Set(std::string name, Value v)
    {
        values.emplace_back(std::move(name), std::move(v));
        return *this;
    }
    Class& Sub(Class c)
    {
        classes.push_back(std::move(c));
        return *this;
    }
};

inline void PutCompressed(std::vector<uint8_t>& out, size_t n)
{
    for (;;)
    {
        uint8_t byte = static_cast<uint8_t>(n & 0x7F);
        n >>= 7;
        out.push_back(static_cast<uint8_t>(byte | (n != 0 ? 0x80 : 0)));
        if (n == 0)
            return;
    }
}

inline void PutAsciiz(std::vector<uint8_t>& out, const std::string& text)
{
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

inline void PutRaw(std::vector<uint8_t>& out, const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

inline uint8_t TypeByte(const Value& v)
{
    return v.kind == Value::String ? 0 : (v.kind == Value::Float ? 1 : 2);
}

inline void PutPayload(std::vector<uint8_t>& out, const Value& v)
{
    if (v.kind == Value::String)
        PutAsciiz(out, v.text);
    else if (v.kind == Value::Float)
        PutRaw(out, &v.number, 4);
    else
        PutRaw(out, &v.integer, 4);
}

inline std::vector<uint8_t> Emit(const Class& root)
{
    std::vector<uint8_t> out;
    PutRaw(out, "\0raP", 4);
    const uint32_t zero = 0, eight = 8;
    PutRaw(out, &zero, 4);  // version
    PutRaw(out, &eight, 4); // always 8
    PutRaw(out, &zero, 4);  // enum table offset, patched at the end

    std::vector<std::pair<size_t, const Class*>> pending;
    const auto body = [&](const Class& c)
    {
        out.push_back(0); // inherited class name: none
        PutCompressed(out, c.arrays.size() + c.values.size() + c.classes.size());
        for (const auto& [name, entries] : c.arrays)
        {
            out.push_back(2);
            PutAsciiz(out, name);
            PutCompressed(out, entries.size());
            for (const Value& v : entries)
            {
                out.push_back(TypeByte(v));
                PutPayload(out, v);
            }
        }
        for (const auto& [name, v] : c.values)
        {
            out.push_back(1);
            out.push_back(TypeByte(v));
            PutAsciiz(out, name);
            PutPayload(out, v);
        }
        for (const Class& sub : c.classes)
        {
            out.push_back(0);
            PutAsciiz(out, sub.name);
            pending.emplace_back(out.size(), &sub);
            PutRaw(out, &zero, 4); // absolute offset, patched below
        }
    };

    body(root);
    for (size_t i = 0; i < pending.size(); ++i)
    {
        const auto [patchAt, klass] = pending[i];
        const uint32_t here = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + patchAt, &here, 4);
        body(*klass);
    }
    const uint32_t enumAt = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 12, &enumAt, 4);
    PutRaw(out, &zero, 4); // empty enum table: a count of zero, as Arma 3 writes
    return out;
}

//! A top-down UV generator: u = scale*worldX + uOffset, v = scale*worldZ + vOffset.
//!
//! `uvTransform` is a BASIS, not a pair of row vectors. LandSave reads
//! u = (aside[0], up[0], dir[0], pos[0]) and v = (aside[1], up[1], dir[1], pos[1])
//! -- the first component of each of the three axes, not all three of `aside`.
//! Writing the v scale into `up[2]`, which reads correctly row-wise, leaves v
//! identically zero: every fragment in the world then samples row 0 of the
//! satellite, and the ground comes out a single smeared stripe with no hint in
//! any log that the UVs are degenerate. The v scale belongs in `dir[1]`.
inline Class TexGen(const std::string& name, float scale, float uOffset, float vOffset)
{
    Class transform{"uvTransform"};
    transform.Array("aside", {Value::F32(scale), Value::F32(0.0f), Value::F32(0.0f)});
    transform.Array("up", {Value::F32(0.0f), Value::F32(0.0f), Value::F32(scale)});
    transform.Array("dir", {Value::F32(0.0f), Value::F32(scale), Value::F32(0.0f)});
    transform.Array("pos", {Value::F32(uOffset), Value::F32(vOffset), Value::F32(0.0f)});
    Class gen{name};
    gen.Set("uvSource", Value::Str("worldPos"));
    gen.Sub(std::move(transform));
    return gen;
}

inline Class Stage(int index, const std::string& texture, int texGen)
{
    Class stage{"Stage" + std::to_string(index)};
    stage.Set("texture", Value::Str(texture));
    stage.Set("texGen", Value::I32(texGen));
    return stage;
}

} // namespace Rap

//! One authored surface bound to a mask slot.
struct Slot
{
    int material = -1;      //!< index into the .terr MATS list
    std::string colourPaa;  //!< virtual path written into the RVMAT
    float tileMetres = 4.0f;
};

struct MaterialAssets
{
    std::vector<std::string> rvmats; //!< virtual paths, one per region, material-table order
    std::vector<uint16_t> cellIndex; //!< land cell -> region, or empty for a single region
    size_t texturesWritten = 0;
    std::string error;
};

//! Reforger encodes a surface's authored tiling in its texture filename:
//! `Soil_2x2_01_BCR`, `Asphalt_01_4x4_BCR`. Anything unlabelled tiles at 4 m.
inline float TileMetresOf(const std::string& path)
{
    std::string name = LowerCopy(std::filesystem::path(path).stem().string());
    size_t at = 0;
    while (at < name.size())
    {
        size_t next = name.find('_', at);
        if (next == std::string::npos)
            next = name.size();
        const std::string part = name.substr(at, next - at);
        const size_t x = part.find('x');
        if (x != std::string::npos && x > 0 && x + 1 < part.size())
        {
            const std::string a = part.substr(0, x), b = part.substr(x + 1);
            const auto digits = [](const std::string& s)
            { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; }); };
            if (digits(a) && digits(b) && a == b)
            {
                const int metres = std::atoi(a.c_str());
                if (metres > 0 && metres <= 16)
                    return static_cast<float>(metres);
            }
        }
        at = next + 1;
    }
    return 4.0f;
}

//! Decode a `.edds` out of the paks into straight RGBA8.
inline bool LoadEdds(const PakSet& paks, const std::string& path, Poseidon::Image& out)
{
    std::vector<uint8_t> bytes;
    if (!paks.Read(path, bytes) || bytes.empty())
        return false;
    Poseidon::EddsImage edds = Poseidon::ReadEddsBuffer(bytes.data(), bytes.size());
    if (!edds.valid() || edds.mipmaps.empty())
        return false;
    Poseidon::Image image(edds.width, edds.height, edds.format, std::move(edds.mipmaps[0].data));
    out = image.ToRGBA();
    // Every caller walks this as width*height*4 bytes, so a short decode has to
    // fail here rather than run off the end of the buffer.
    return out.valid() &&
           out.data().size() >= static_cast<size_t>(out.width()) * static_cast<size_t>(out.height()) * 4;
}

//! Write a `.paa` and prove it reads back at the size it was written.
//!
//! `WritePAA` accepts formats and sizes the PAA READER cannot read, and a texture that
//! fails to load is silent all the way to a white draw -- there is no error anywhere,
//! only a ground that looks wrong. The mask is the live case: a 4096 DXT3 comes back
//! with a truncated mip chain, which is the whole reason `--mask-size` is documented
//! as capped at 2048. Before this, raising it past the cap wrote an unreadable mask
//! and said nothing; now it names the file and the size that failed.
inline bool WriteVerifiedPAA(const std::string& path, int width, int height, std::vector<uint8_t> rgba,
                             Poseidon::PixelFormat format, std::string& error)
{
    if (!Poseidon::PAAEncoder::WritePAA(path, Poseidon::Image::FromRGBA(width, height, std::move(rgba)), format))
    {
        error = "WritePAA refused " + path;
        return false;
    }
    Poseidon::PAAInfo info;
    const Poseidon::DecodedImage back = Poseidon::DecodePAAFile(path);
    if (!ReadPAAInfo(path, info) || !back.valid() || back.width != width || back.height != height)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        error = path + " does not read back at " + std::to_string(width) + "x" + std::to_string(height) +
                " -- the writer accepted a size the reader cannot load";
        return false;
    }
    return true;
}

//! Everything the RVMAT set needs, resolved per MATS entry.
struct SourceMaterial
{
    std::string ematPath;
    std::string bcrPath;
    bool decoded = false;
    std::array<float, 3> mean{{0.5f, 0.5f, 0.5f}}; //!< sRGB mean of the BCR albedo
    float tileMetres = 4.0f;
};

//! Translate a world's authored surfaces into a TerrainSNX RVMAT plus its textures.
//!
//! One region, so one material and five slots. TerrainSNX allows five surfaces
//! and Everon authors 22, so the five with the most area are bound and the other
//! seventeen are folded into whichever of the five is closest in mean colour --
//! NOT into slot 0. Slot 0 is the mask's unpainted value, so sweeping the tail
//! there paints the world's single most common material (on Everon the seabed)
//! across every unlisted patch of land.
inline MaterialAssets GenerateTerrainMaterials(const AssembledTerrain& terrain, const std::string& root,
                                               const std::string& outDir, const std::string& virtualDir,
                                               int satelliteSize, int maskSize)
{
    MaterialAssets assets;
    const uint32_t edge = terrain.materialEdge;
    if (edge == 0 || terrain.materials.empty())
    {
        assets.error = "no TMAT surface assignment was decoded";
        return assets;
    }

    PakSet paks;
    if (!paks.Open(root))
    {
        assets.error = "no .pak files under " + root;
        return assets;
    }

    // ---- resolve every MATS entry to its BCR albedo -------------------------
    const size_t count = terrain.descriptor.materials.size();
    std::vector<SourceMaterial> sources(count);
    const auto loadText = [&](const std::string& path, std::string& text)
    {
        std::vector<uint8_t> bytes;
        if (!paks.Read(path, bytes))
            return false;
        text.assign(bytes.begin(), bytes.end());
        return true;
    };
    for (size_t i = 0; i < count; ++i)
    {
        SourceMaterial& source = sources[i];
        source.ematPath = terrain.descriptor.materials[i];
        std::string text;
        if (!loadText(source.ematPath, text))
            continue;
        Poseidon::Asset::Material::EmatMaterial emat = Poseidon::Asset::Material::ParseEmat(text);
        if (!emat.valid())
            continue;
        // 18.3% of Reforger's materials get their maps only from a parent.
        Poseidon::Asset::Material::ResolveEmatInheritance(emat, loadText);
        source.bcrPath = emat.TextureOf("BCRMap");
        if (!source.bcrPath.empty())
            source.tileMetres = TileMetresOf(source.bcrPath);
    }

    // Decode each distinct BCR once; several materials share one texture.
    std::map<std::string, std::array<float, 3>> means;
    for (SourceMaterial& source : sources)
    {
        if (source.bcrPath.empty())
            continue;
        const std::string key = LowerCopy(source.bcrPath);
        auto found = means.find(key);
        if (found == means.end())
        {
            Poseidon::Image image;
            if (!LoadEdds(paks, source.bcrPath, image))
                continue;
            const std::vector<uint8_t>& px = image.data();
            double sum[3] = {0, 0, 0};
            const size_t texels = static_cast<size_t>(image.width()) * image.height();
            for (size_t t = 0; t < texels; ++t)
                for (int c = 0; c < 3; ++c)
                    sum[c] += px[t * 4 + c];
            std::array<float, 3> mean{};
            for (int c = 0; c < 3; ++c)
                mean[c] = static_cast<float>(sum[c] / (255.0 * static_cast<double>(texels)));
            found = means.emplace(key, mean).first;
        }
        source.mean = found->second;
        source.decoded = true;
    }

    // ---- pick the five slots ------------------------------------------------
    std::vector<size_t> area(count, 0);
    for (uint8_t m : terrain.materials)
        if (m < count)
            ++area[m];
    std::vector<int> order;
    for (size_t i = 0; i < count; ++i)
        if (area[i] > 0 && sources[i].decoded)
            order.push_back(static_cast<int>(i));
    std::sort(order.begin(), order.end(), [&](int a, int b) { return area[a] > area[b]; });
    if (order.empty())
    {
        assets.error = "no MATS entry resolved a BCR albedo";
        return assets;
    }

    constexpr int kSlots = 5; // TerrainSNX, ShaderSchema.hpp FindTerrainFamily
    std::vector<Slot> slots;
    for (int i = 0; i < kSlots && i < static_cast<int>(order.size()); ++i)
        slots.push_back(Slot{order[static_cast<size_t>(i)], {}, sources[order[static_cast<size_t>(i)]].tileMetres});

    // Fold the tail onto the nearest slot by mean colour.
    std::vector<uint8_t> slotOf(256, 0);
    for (size_t i = 0; i < count; ++i)
    {
        if (!sources[i].decoded)
            continue;
        float best = 1e30f;
        for (size_t s = 0; s < slots.size(); ++s)
        {
            const auto& a = sources[i].mean;
            const auto& b = sources[static_cast<size_t>(slots[s].material)].mean;
            const float d = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                            (a[2] - b[2]) * (a[2] - b[2]);
            if (d < best)
            {
                best = d;
                slotOf[i] = static_cast<uint8_t>(s);
            }
        }
    }
    for (size_t s = 0; s < slots.size(); ++s)
        slotOf[static_cast<size_t>(slots[s].material)] = static_cast<uint8_t>(s);

    // ---- write the files ----------------------------------------------------
    std::string vdir = virtualDir;
    while (!vdir.empty() && (vdir.back() == '\\' || vdir.back() == '/'))
        vdir.pop_back();
    std::filesystem::path dir(outDir);
    {
        std::string rel = vdir;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        dir /= rel;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto virtualPath = [&](const std::string& name) { return vdir.empty() ? name : vdir + "\\" + name; };
    const auto diskPath = [&](const std::string& name) { return (dir / name).string(); };

    // Surfaces. Each is divided by twice its own mean so it enters the shader's
    // `surface * 2` at unity and MODULATES the satellite. Arma 3's own ground
    // surfaces already sit there (VR's is 0.4745); Reforger's PBR base colours
    // average 0.21-0.47, so binding them raw darkens the ground by up to 2.4x.
    for (size_t s = 0; s < slots.size(); ++s)
    {
        SourceMaterial& source = sources[static_cast<size_t>(slots[s].material)];
        Poseidon::Image image;
        if (!LoadEdds(paks, source.bcrPath, image))
        {
            assets.error = "cannot decode " + source.bcrPath;
            return assets;
        }
        std::vector<uint8_t> px = image.data();
        const size_t texels = static_cast<size_t>(image.width()) * image.height();
        for (size_t t = 0; t < texels; ++t)
            for (int c = 0; c < 3; ++c)
            {
                const float gain = 1.0f / std::max(1e-3f, 2.0f * source.mean[c]);
                px[t * 4 + c] = static_cast<uint8_t>(
                    std::clamp(static_cast<float>(px[t * 4 + c]) * gain + 0.5f, 0.0f, 255.0f));
            }
        const std::string name = "surface" + std::to_string(s) + "_co.paa";
        if (!Poseidon::PAAEncoder::WritePAA(diskPath(name),
                                            Poseidon::Image::FromRGBA(image.width(), image.height(), std::move(px)),
                                            Poseidon::PixelFormat::DXT1))
        {
            assets.error = "cannot write " + diskPath(name);
            return assets;
        }
        slots[s].colourPaa = virtualPath(name);
        ++assets.texturesWritten;
    }

    // Satellite: the mean colour of whatever material covers each square metre,
    // area-averaged down. No hillshade and no water tint -- the engine lights and
    // floods the terrain itself, and baking either in double-counts it.
    {
        const int n = satelliteSize;
        std::vector<uint8_t> rgba(static_cast<size_t>(n) * n * 4, 255);
        // One output row at a time. The input rows feeding a given output row are
        // contiguous, so the accumulator is a row rather than a frame: at 4096
        // square a whole-image accumulator is 400 MB of doubles plus a 67 MB hit
        // count, on top of the 164 MB raster, and it took the process out.
        std::vector<double> accum(static_cast<size_t>(n) * 3, 0.0);
        std::vector<uint32_t> hits(static_cast<size_t>(n), 0);
        for (int oz = 0; oz < n; ++oz)
        {
            std::fill(accum.begin(), accum.end(), 0.0);
            std::fill(hits.begin(), hits.end(), 0u);
            const uint64_t zStart = (static_cast<uint64_t>(oz) * edge + n - 1) / n;
            const uint64_t zEnd = (static_cast<uint64_t>(oz + 1) * edge + n - 1) / n;
            for (uint64_t z = zStart; z < zEnd && z < edge; ++z)
            {
                const uint8_t* row = terrain.materials.data() + static_cast<size_t>(z) * edge;
                for (uint32_t x = 0; x < edge; ++x)
                {
                    const uint8_t m = row[x];
                    if (m >= count || !sources[m].decoded)
                        continue;
                    const size_t ox = static_cast<size_t>(static_cast<uint64_t>(x) * n / edge);
                    for (int c = 0; c < 3; ++c)
                        accum[ox * 3 + c] += sources[m].mean[c];
                    ++hits[ox];
                }
            }
            for (int ox = 0; ox < n; ++ox)
            {
                const size_t at = (static_cast<size_t>(oz) * n + ox) * 4;
                for (int c = 0; c < 3; ++c)
                    rgba[at + c] =
                        hits[ox] == 0
                            ? 128
                            : static_cast<uint8_t>(std::clamp(
                                  accum[static_cast<size_t>(ox) * 3 + c] / hits[ox] * 255.0 + 0.5, 0.0, 255.0));
            }
        }
        if (!Poseidon::PAAEncoder::WritePAA(diskPath("satellite.paa"),
                                            Poseidon::Image::FromRGBA(n, n, std::move(rgba)),
                                            Poseidon::PixelFormat::DXT1))
        {
            assets.error = "cannot write the satellite";
            return assets;
        }
        ++assets.texturesWritten;
    }

    // Mask. RGB is one-hot and selects slots 1..3; no channel high is slot 0;
    // alpha 119 selects slot 4 (terrain.wgsl decode_mask).
    //
    // DXT3, and that is measured. Of the formats the PAA container advertises,
    // only DXT1/3/5 survive a WritePAA round-trip -- ARGB4444/1555 and AI88 all
    // come back "LZW Decode error" and ARGB8888 will not load at all, which
    // showed up only as `complete=0` in a terrain log line naming no file. DXT3
    // over DXT5 because its alpha is explicit 4-bit per texel with no
    // interpolation across the block, so the authored levels stay crisp. Keep
    // the mask at or below 2048: a 4096 DXT3 comes out with a truncated mip
    // chain and will not read back.
    {
        const int n = maskSize;
        std::vector<uint8_t> rgba(static_cast<size_t>(n) * n * 4, 0);
        for (int z = 0; z < n; ++z)
        {
            const size_t sz = static_cast<size_t>(static_cast<uint64_t>(z) * edge / n);
            for (int x = 0; x < n; ++x)
            {
                const size_t sx = static_cast<size_t>(static_cast<uint64_t>(x) * edge / n);
                const uint8_t m = terrain.materials[sz * edge + sx];
                const uint8_t slot = m < count ? slotOf[m] : 0;
                const size_t at = (static_cast<size_t>(z) * n + x) * 4;
                rgba[at + 3] = 255;
                if (slot >= 1 && slot <= 3)
                    rgba[at + (slot - 1)] = 255;
                else if (slot == 4)
                    rgba[at + 3] = 119;
            }
        }
        if (!Poseidon::PAAEncoder::WritePAA(diskPath("mask.paa"), Poseidon::Image::FromRGBA(n, n, std::move(rgba)),
                                            Poseidon::PixelFormat::DXT3))
        {
            assets.error = "cannot write the mask";
            return assets;
        }
        ++assets.texturesWritten;
    }

    // ---- the RVMAT ----------------------------------------------------------
    // TerrainSNX: satellite Stage0, mask Stage1, surface colours from Stage4 with
    // stride 2 and the normal one stage BELOW its colour, so the colours land on
    // the even stages 4, 6, 8, 10, 12.
    const float world = terrain.descriptor.WorldWidth();
    Rap::Class rvmatRoot{""};
    rvmatRoot.Array("ambient", {Rap::Value::F32(1.0f), Rap::Value::F32(1.0f), Rap::Value::F32(1.0f), Rap::Value::F32(1.0f)});
    rvmatRoot.Array("diffuse",
               {Rap::Value::F32(0.55f), Rap::Value::F32(0.55f), Rap::Value::F32(0.55f), Rap::Value::F32(1.0f)});
    rvmatRoot.Array("forcedDiffuse", {Rap::Value::F32(0.0f), Rap::Value::F32(0.0f), Rap::Value::F32(0.0f),
                                 Rap::Value::F32(0.0f)});
    rvmatRoot.Array("emmisive",
               {Rap::Value::F32(0.0f), Rap::Value::F32(0.0f), Rap::Value::F32(0.0f), Rap::Value::F32(0.0f)});
    rvmatRoot.Array("specular",
               {Rap::Value::F32(0.0f), Rap::Value::F32(0.0f), Rap::Value::F32(0.0f), Rap::Value::F32(0.0f)});
    rvmatRoot.Set("specularPower", Rap::Value::F32(0.0f));
    rvmatRoot.Set("PixelShaderID", Rap::Value::Str("TerrainSNX"));
    rvmatRoot.Set("VertexShaderID", Rap::Value::Str("Terrain"));
    rvmatRoot.Sub(Rap::Stage(0, virtualPath("satellite.paa"), 0));
    rvmatRoot.Sub(Rap::Stage(1, virtualPath("mask.paa"), 0));
    for (size_t s = 0; s < slots.size(); ++s)
        rvmatRoot.Sub(Rap::Stage(4 + 2 * static_cast<int>(s), slots[s].colourPaa, 1 + static_cast<int>(s)));
    rvmatRoot.Sub(Rap::TexGen("TexGen0", 1.0f / world, 0.0f, 0.0f));
    for (size_t s = 0; s < slots.size(); ++s)
        rvmatRoot.Sub(Rap::TexGen("TexGen" + std::to_string(1 + s), 1.0f / slots[s].tileMetres, 0.0f, 0.0f));

    const std::vector<uint8_t> rvmat = Rap::Emit(rvmatRoot);
    {
        std::ofstream file(diskPath("terrain.rvmat"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(rvmat.data()), static_cast<std::streamsize>(rvmat.size()));
        if (!file)
        {
            assets.error = "cannot write the RVMAT";
            return assets;
        }
    }
    assets.rvmats.push_back(virtualPath("terrain.rvmat"));

    std::cout << "\nTerrain materials: " << count << " authored, " << means.size() << " distinct albedos, "
              << assets.texturesWritten << " textures written\n";
    const double total = static_cast<double>(edge) * edge;
    for (size_t s = 0; s < slots.size(); ++s)
    {
        const SourceMaterial& source = sources[static_cast<size_t>(slots[s].material)];
        size_t folded = 0;
        for (size_t i = 0; i < count; ++i)
            if (sources[i].decoded && slotOf[i] == s && static_cast<int>(i) != slots[s].material)
                ++folded;
        std::cout << "  slot " << s << "  " << std::setw(34) << std::left
                  << std::filesystem::path(source.ematPath).stem().string() << std::right << std::fixed
                  << std::setprecision(2) << std::setw(7) << 100.0 * area[static_cast<size_t>(slots[s].material)] / total
                  << "%  tile " << std::setprecision(0) << slots[s].tileMetres << " m  mean " << std::setprecision(3)
                  << source.mean[0] << " " << source.mean[1] << " " << source.mean[2] << "  +" << folded << " folded\n";
    }
    return assets;
}

} // namespace Surfaces

//! Object placements read from a manifest, ready for the OPRW object table.
namespace Placements
{

struct Item
{
    int32_t modelIndex = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f; //!< y is ABSOLUTE world elevation once Resolve has run
    float yawDeg = 0.0f;
    float scale = 1.0f;
    //! The manifest said this Y is an offset above the terrain. Cleared by
    //! ResolveTerrainRelative, which is the only thing that may make it false.
    bool terrainRelative = false;
};

struct Set
{
    std::vector<std::string> models; //!< virtual .p3d paths, the OPRW model table
    std::vector<Item> items;
    size_t skipped = 0;         //!< lines rejected as malformed or out of the world
    size_t terrainRelative = 0; //!< lines whose seventh field asked for a terrain offset
    std::string error;
};

//! Tab-separated: `virtualPath, x, y, z, yawDeg, scale [, terrainRelative]`, one
//! placement per line.
//!
//! The seventh field is optional and defaults to 0. A manifest written before it
//! existed therefore reads as entirely absolute, which is what it meant.
//!
//! Placements outside the world are dropped rather than clamped. A model index
//! outside the model table aborts the whole world parse in the reader
//! (Oprw25.hpp), so the table is built from the paths actually accepted and every
//! index written is an index into it by construction.
inline Set Read(const std::string& path, float worldExtent)
{
    Set set;
    std::ifstream file(path);
    if (!file)
    {
        set.error = "cannot open " + path;
        return set;
    }
    std::map<std::string, int32_t> byPath;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::vector<std::string> field;
        size_t at = 0;
        while (at <= line.size())
        {
            const size_t tab = line.find('\t', at);
            field.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos)
                break;
            at = tab + 1;
        }
        if (field.size() < 6 || field[0].empty())
        {
            ++set.skipped;
            continue;
        }
        Item item;
        item.x = std::strtof(field[1].c_str(), nullptr);
        item.y = std::strtof(field[2].c_str(), nullptr);
        item.z = std::strtof(field[3].c_str(), nullptr);
        item.yawDeg = std::strtof(field[4].c_str(), nullptr);
        item.scale = std::strtof(field[5].c_str(), nullptr);
        item.terrainRelative = field.size() >= 7 && std::strtol(field[6].c_str(), nullptr, 10) != 0;
        if (!(item.scale > 0.0f) || !std::isfinite(item.x) || !std::isfinite(item.y) || !std::isfinite(item.z) ||
            item.x < 0.0f || item.x > worldExtent || item.z < 0.0f || item.z > worldExtent)
        {
            ++set.skipped;
            continue;
        }
        const auto found = byPath.find(field[0]);
        if (found == byPath.end())
        {
            item.modelIndex = static_cast<int32_t>(set.models.size());
            byPath.emplace(field[0], item.modelIndex);
            set.models.push_back(field[0]);
        }
        else
        {
            item.modelIndex = found->second;
        }
        if (item.terrainRelative)
            ++set.terrainRelative;
        set.items.push_back(item);
    }
    return set;
}

//! Turns every terrain-relative Y into an absolute elevation. Returns how many it
//! moved, and by how much on average -- a caller that prints zero moved on a
//! Reforger world is looking at a manifest without the seventh field.
//!
//! Bilinear rather than nearest. The grid is 2 m on Everon and its houses are wider
//! than that, so a nearest sample leaves a building visibly stepped into any slope
//! it stands on. This is the same surface the runtime reads back out of the `.wrp`,
//! so a resolved placement sits ON the ground rather than near it.
inline size_t ResolveTerrainRelative(Set& set, const AssembledTerrain& terrain, double& meanRise)
{
    const TerrainDescriptor& descriptor = terrain.descriptor;
    const float cellSize = descriptor.cellSize;
    if (!(cellSize > 0.0f) || descriptor.gridWidth < 2 || descriptor.gridHeight < 2)
        return 0;
    const uint32_t lastX = descriptor.gridWidth - 1;
    const uint32_t lastZ = descriptor.gridHeight - 1;

    size_t moved = 0;
    double total = 0.0;
    for (Item& item : set.items)
    {
        if (!item.terrainRelative)
            continue;
        const float gx = std::clamp(item.x / cellSize, 0.0f, static_cast<float>(lastX));
        const float gz = std::clamp(item.z / cellSize, 0.0f, static_cast<float>(lastZ));
        const auto x0 = static_cast<uint32_t>(gx);
        const auto z0 = static_cast<uint32_t>(gz);
        const uint32_t x1 = x0 < lastX ? x0 + 1 : x0;
        const uint32_t z1 = z0 < lastZ ? z0 + 1 : z0;
        const float fx = gx - static_cast<float>(x0);
        const float fz = gz - static_cast<float>(z0);
        const float h00 = terrain.HeightAt(x0, z0);
        const float h10 = terrain.HeightAt(x1, z0);
        const float h01 = terrain.HeightAt(x0, z1);
        const float h11 = terrain.HeightAt(x1, z1);
        const float height = (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz) + (h01 * (1.0f - fx) + h11 * fx) * fz;

        item.y += height;
        item.terrainRelative = false;
        total += height;
        ++moved;
    }
    meanRise = moved != 0 ? total / static_cast<double>(moved) : 0.0;
    return moved;
}

} // namespace Placements

namespace Oprw
{

using Poseidon::Foundation::Lzo1x;

//! Little-endian byte sink. The container is little-endian on the wire and both
//! supported targets are little-endian hosts, so the primitives are plain copies.
struct ByteSink
{
    std::vector<uint8_t> bytes;

    void raw(const void* data, size_t size)
    {
        const uint8_t* at = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), at, at + size);
    }
    void u8(uint8_t value) { bytes.push_back(value); }
    void i16(int16_t value) { raw(&value, sizeof(value)); }
    void i32(int32_t value) { raw(&value, sizeof(value)); }
    void f32(float value) { raw(&value, sizeof(value)); }
    void asciiz(const std::string& text)
    {
        raw(text.data(), text.size());
        u8(0);
    }
    size_t size() const { return bytes.size(); }
};

//! An LZO1X stream that is one literal run and nothing else.
//!
//! There is no LZO compressor in this repository -- `Lzo1x` is decode-only -- and
//! none is needed: a decoder cannot tell a stream that failed to find any match
//! from one that did not look. The shape is dictated by `Lzo1x::Decompress`:
//!
//!   0x00            first opcode, taken as "long literal run" (Lzo1x.hpp:119)
//!   0x00 * k        each zero adds 255 to the run length (the `extend` loop)
//!   b               1..255; zero would be eaten as another 255
//!   <size bytes>    the literal run, size == 18 + 255k + b
//!   0x11 0x00 0x00  the end-of-stream marker (Lzo1x.hpp:186-193)
//!
//! The marker is mandatory, not decorative: `ReadCompressed` requires a non-zero
//! `consumed`, which only the marker sets, and without it the next field in the
//! world would be read from the wrong offset.
void AppendLzoLiterals(ByteSink& out, const void* data, size_t size)
{
    // 19 is the shortest run this encoding can express. Every payload here is at
    // least the 1024-byte compression threshold, so this is a guard, not a case.
    if (size < 19)
    {
        out.raw(data, size);
        return;
    }
    const size_t remainder = size - 18;
    const size_t zeroes = (remainder - 1) / 255;
    const size_t tail = remainder - 255 * zeroes;
    out.u8(0x00);
    for (size_t i = 0; i < zeroes; ++i)
        out.u8(0x00);
    out.u8(static_cast<uint8_t>(tail));
    out.raw(data, size);
    out.u8(0x11);
    out.u8(0x00);
    out.u8(0x00);
}

//! A bulk array as the reader expects it: raw below the threshold, LZO above it.
//! There is no per-payload flag -- the size alone decides (Oprw25.hpp:335-344).
void AppendBulk(ByteSink& out, const void* data, size_t size)
{
    if (size < Poseidon::Asset::Formats::COMPRESSION_THRESHOLD)
    {
        out.raw(data, size);
        return;
    }
    AppendLzoLiterals(out, data, size);
}

//! Writes the 16-ary tree `Detail::QuadTree` reads.
//!
//! The addressing is reproduced from the reader rather than reinvented, because it
//! has two properties a fresh design would not guess: the addressed space is padded
//! up to a whole number of levels and can be WIDER than the grid (a 256x256 grid of
//! 16-bit elements is addressed as 512x256), and a leaf is always four bytes
//! covering 4/elementSize cells. Regions that hold one value collapse to a single
//! leaf at whatever depth they became uniform, which is what makes a mostly-flat
//! grid cost bytes instead of megabytes.
class QuadTreeWriter
{
  public:
    //! `values` is a dense row-major sizeX*sizeY array of `elementSize`-byte cells.
    QuadTreeWriter(int sizeX, int sizeY, int elementSize, const uint8_t* values)
        : sizeX_(sizeX), sizeY_(sizeY), elementSize_(elementSize), values_(values)
    {
        leafLogX_ = elementSize_ == 4 ? 0 : 1;
        leafLogY_ = elementSize_ == 1 ? 1 : 0;
        const int logX = BitsToHold(sizeX_);
        const int logY = BitsToHold(sizeY_);
        const int levelsX = (logX - leafLogX_ + kLogSize - 1) / kLogSize;
        const int levelsY = (logY - leafLogY_ + kLogSize - 1) / kLogSize;
        levels_ = levelsX > levelsY ? levelsX : levelsY;
        logTotalX_ = levels_ * kLogSize + leafLogX_;
        logTotalY_ = levels_ * kLogSize + leafLogY_;
    }

    void Write(ByteSink& out) const
    {
        const int spanX = 1 << logTotalX_;
        const int spanY = 1 << logTotalY_;
        if (Uniform(0, 0, spanX, spanY))
        {
            out.u8(0); // root is a leaf
            WriteLeaf(out, 0, 0);
            return;
        }
        out.u8(1); // root is a node
        WriteNode(out, 0, 0, 0);
    }

  private:
    static int BitsToHold(int n)
    {
        int bits = 0;
        for (int v = n - 1; v != 0; v >>= 1)
            ++bits;
        return bits;
    }

    //! Cells outside the grid read as zero; the reader never asks for them, but the
    //! padded region still has to be written, and zero keeps those leaves uniform.
    const uint8_t* At(int x, int y) const
    {
        static const uint8_t zero[4] = {0, 0, 0, 0};
        if (x < 0 || y < 0 || x >= sizeX_ || y >= sizeY_)
            return zero;
        return values_ + (static_cast<size_t>(y) * sizeX_ + x) * elementSize_;
    }

    bool Uniform(int x0, int y0, int w, int h) const
    {
        const uint8_t* first = At(x0, y0);
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x)
                if (std::memcmp(At(x, y), first, static_cast<size_t>(elementSize_)) != 0)
                    return false;
        return true;
    }

    void WriteLeaf(ByteSink& out, int x0, int y0) const
    {
        uint8_t leaf[4] = {0, 0, 0, 0};
        const int leafW = 1 << leafLogX_;
        const int leafH = 1 << leafLogY_;
        for (int iy = 0; iy < leafH; ++iy)
            for (int ix = 0; ix < leafW; ++ix)
                std::memcpy(leaf + ((iy << leafLogX_) + ix) * elementSize_, At(x0 + ix, y0 + iy),
                            static_cast<size_t>(elementSize_));
        out.raw(leaf, sizeof(leaf));
    }

    void WriteNode(ByteSink& out, int depth, int x0, int y0) const
    {
        const int childW = 1 << (logTotalX_ - kLogSize * (depth + 1));
        const int childH = 1 << (logTotalY_ - kLogSize * (depth + 1));
        const bool childrenAreLeaves = depth + 1 >= levels_;

        bool isNode[16] = {};
        uint16_t mask = 0;
        for (int i = 0; i < 16; ++i)
        {
            const int cx = x0 + (i & 3) * childW;
            const int cy = y0 + (i >> 2) * childH;
            isNode[i] = !childrenAreLeaves && !Uniform(cx, cy, childW, childH);
            if (isNode[i])
                mask |= static_cast<uint16_t>(1u << i);
        }
        out.i16(static_cast<int16_t>(mask));
        for (int i = 0; i < 16; ++i)
        {
            const int cx = x0 + (i & 3) * childW;
            const int cy = y0 + (i >> 2) * childH;
            if (isNode[i])
                WriteNode(out, depth + 1, cx, cy);
            else
                WriteLeaf(out, cx, cy);
        }
    }

    static constexpr int kLogSize = 2;

    int sizeX_, sizeY_, elementSize_;
    const uint8_t* values_;
    int leafLogX_ = 0, leafLogY_ = 0, levels_ = 0, logTotalX_ = 0, logTotalY_ = 0;
};

struct ExportOptions
{
    int landRange = 256;
    int terrainRange = 2048;
    float landCellSize = 50.0f;
    std::vector<std::string> materials;               //!< virtual paths written into the material table
    const Placements::Set* placements = nullptr;      //!< object table, or null for an empty world
};

//! The world, built in memory. Written in one go so a partial file can never be
//! left behind for the runtime to half-read.
struct BuiltWorld
{
    std::vector<uint8_t> bytes;
    std::vector<float> elevation; //!< kept so the verify pass can compare
    float minHeight = 0.0f, maxHeight = 0.0f;
    float peakX = 0.0f, peakZ = 0.0f; //!< world position of maxHeight, a usable camera anchor
    int materialCount = 0;
    size_t objectCount = 0;
    size_t modelCount = 0;
};

BuiltWorld BuildOprw25(const AssembledTerrain& terrain, const ExportOptions& options)
{
    BuiltWorld built;
    const TerrainDescriptor& descriptor = terrain.descriptor;
    const int land = options.landRange;
    const int terrainRange = options.terrainRange;
    const size_t landCells = static_cast<size_t>(land) * land;
    const size_t terrainCells = static_cast<size_t>(terrainRange) * terrainRange;

    // Resample the authored heightfield onto the terrain grid. The source is a
    // sample lattice (gridWidth samples spanning (gridWidth-1)*cellSize metres), so
    // the mapping is by world position, not by index ratio.
    const float terrainCell = options.landCellSize * static_cast<float>(land) / static_cast<float>(terrainRange);
    const uint32_t gridW = descriptor.gridWidth;
    const uint32_t gridH = descriptor.gridHeight;
    built.elevation.resize(terrainCells);
    built.minHeight = 1e30f;
    built.maxHeight = -1e30f;
    for (int z = 0; z < terrainRange; ++z)
    {
        const float sz = static_cast<float>(z) * terrainCell / descriptor.cellSize;
        const uint32_t z0 = std::min(static_cast<uint32_t>(sz), gridH - 1);
        const uint32_t z1 = std::min(z0 + 1, gridH - 1);
        const float fz = sz - static_cast<float>(z0);
        for (int x = 0; x < terrainRange; ++x)
        {
            const float sx = static_cast<float>(x) * terrainCell / descriptor.cellSize;
            const uint32_t x0 = std::min(static_cast<uint32_t>(sx), gridW - 1);
            const uint32_t x1 = std::min(x0 + 1, gridW - 1);
            const float fx = sx - static_cast<float>(x0);
            const float h00 = terrain.HeightAt(x0, z0);
            const float h10 = terrain.HeightAt(x1, z0);
            const float h01 = terrain.HeightAt(x0, z1);
            const float h11 = terrain.HeightAt(x1, z1);
            const float height =
                (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz) + (h01 * (1.0f - fx) + h11 * fx) * fz;
            built.elevation[static_cast<size_t>(z) * terrainRange + x] = height;
            built.minHeight = std::min(built.minHeight, height);
            built.maxHeight = std::max(built.maxHeight, height);
        }
    }

    // Per-land-cell grids. Geography carries the AI/pathing semantics (water depth,
    // gradient, object counts) and none of it is authored anywhere in the Enfusion
    // data this reads, so it is left at zero rather than guessed: a wrong bit here
    // is a wrong path, and none of it decides whether the world draws.
    const std::vector<int16_t> geography(landCells, 0);
    const std::vector<uint8_t> soundMap(static_cast<size_t>(land) * land, 0);
    built.materialCount = static_cast<int>(options.materials.empty() ? 1 : options.materials.size());
    const std::vector<uint16_t> materialIndex(landCells, 0);
    const std::vector<uint8_t> perTerrainByte(terrainCells, 0);
    const std::vector<uint8_t> perLandByte(landCells, 0);

    ByteSink out;
    out.raw("OPRW", 4);
    out.i32(25);
    out.i32(0); // appId: revision 25's Steam app/DLC id. Zero -- this world is not one.
    out.i32(land);
    out.i32(land);
    out.i32(terrainRange);
    out.i32(terrainRange);
    out.f32(options.landCellSize);

    QuadTreeWriter(land, land, 2, reinterpret_cast<const uint8_t*>(geography.data())).Write(out);
    // The sound map is square on the X range on both axes -- not a slip, it is what
    // the reader consumes (Oprw25.hpp:516-519).
    QuadTreeWriter(land, land, 1, soundMap.data()).Write(out);

    // One mountain, the highest point, so the map has a peak rather than an empty
    // list. Its position is where the resampled maximum landed.
    {
        size_t peak = 0;
        for (size_t i = 1; i < built.elevation.size(); ++i)
            if (built.elevation[i] > built.elevation[peak])
                peak = i;
        built.peakX = static_cast<float>(peak % terrainRange) * terrainCell;
        built.peakZ = static_cast<float>(peak / terrainRange) * terrainCell;
        out.i32(1);
        out.f32(built.peakX);
        out.f32(built.elevation[peak]);
        out.f32(built.peakZ);
    }

    QuadTreeWriter(land, land, 2, reinterpret_cast<const uint8_t*>(materialIndex.data())).Write(out);
    // No randomization array: revision 24 dropped it and the engine regenerates the
    // equivalent at load time.
    AppendBulk(out, perTerrainByte.data(), perTerrainByte.size());                        // grassApprox
    AppendBulk(out, perTerrainByte.data(), perTerrainByte.size());                        // primaryTextureIndex (>=24)
    AppendBulk(out, built.elevation.data(), built.elevation.size() * sizeof(float));      // elevation

    out.i32(built.materialCount);
    if (options.materials.empty())
    {
        // The empty name is what a real world uses for an unassigned cell. The
        // loader skips it, the cell keeps the renderer's fallback layer, and the
        // terrain draws untextured rather than not at all.
        out.asciiz("");
        out.u8(0);
    }
    else
    {
        for (const std::string& material : options.materials)
        {
            out.asciiz(material);
            out.u8(0); // the per-material "major" flag; zero throughout on real worlds
        }
    }

    const size_t objectCount = options.placements ? options.placements->items.size() : 0;
    if (options.placements)
    {
        out.i32(static_cast<int32_t>(options.placements->models.size()));
        for (const std::string& model : options.placements->models)
            out.asciiz(model);
    }
    else
    {
        out.i32(0); // models
    }
    out.i32(0); // static entities

    // Per-cell offsets into the object and map-info tables. The reader reads both
    // trees and discards them -- it indexes the object table by walking it whole --
    // so a uniform zero tree is correct rather than merely tolerated.
    const std::vector<uint32_t> zeroOffsets(landCells, 0);
    QuadTreeWriter(land, land, 4, reinterpret_cast<const uint8_t*>(zeroOffsets.data())).Write(out);
    // Declared here, but the records themselves are the LAST thing in the file --
    // after the road network, not after this field.
    out.i32(static_cast<int32_t>(objectCount * Poseidon::Asset::Formats::World::kOprw25ObjectRecordSize));
    QuadTreeWriter(land, land, 4, reinterpret_cast<const uint8_t*>(zeroOffsets.data())).Write(out);
    out.i32(0); // map info bytes

    AppendBulk(out, perLandByte.data(), perLandByte.size());       // persistent
    AppendBulk(out, perTerrainByte.data(), perTerrainByte.size()); // subdivision hints

    out.i32(0); // max object id

    // The road network is a per-land-cell link count, and the file declares the
    // whole block's length. An empty net is still landCells counts of zero, and the
    // declared length has to match what walking them consumes or the reader throws.
    out.i32(static_cast<int32_t>(landCells * sizeof(int32_t)));
    for (size_t i = 0; i < landCells; ++i)
        out.i32(0);

    // The object table is the last block in the file, and the map-info size the
    // header declared is checked against whatever is left after it -- so writing
    // it anywhere else fails the reader's trailing-size check rather than merely
    // misplacing the objects.
    if (options.placements)
    {
        const size_t models = options.placements->models.size();
        for (size_t i = 0; i < objectCount; ++i)
        {
            const Placements::Item& item = options.placements->items[i];
            // Range-checked here as well as at parse time: an index outside the
            // model table makes the reader throw and abandon the whole world.
            const int32_t modelIndex = (item.modelIndex >= 0 && static_cast<size_t>(item.modelIndex) < models)
                                           ? item.modelIndex
                                           : 0;
            out.i32(static_cast<int32_t>(i + 1)); // objectId, 1-based like a real world
            out.i32(modelIndex);
            const float yaw = item.yawDeg * 3.14159265358979323846f / 180.0f;
            const float c = std::cos(yaw) * item.scale;
            const float s = std::sin(yaw) * item.scale;
            out.f32(c);           // aside
            out.f32(0.0f);
            out.f32(-s);
            out.f32(0.0f);        // up
            out.f32(item.scale);
            out.f32(0.0f);
            out.f32(s);           // dir
            out.f32(0.0f);
            out.f32(c);
            out.f32(item.x);      // pos
            out.f32(item.y);
            out.f32(item.z);
            out.i32(0); // shapeParam
        }
        built.objectCount = objectCount;
        built.modelCount = models;
    }

    built.bytes = std::move(out.bytes);
    return built;
}

//! Re-reads the file that was just written, with the runtime's own reader.
//!
//! This is the whole reason the writer can be trusted. `ReadOprwModern` is strict
//! in a way that makes a successful parse meaningful: the road network must consume
//! its declared byte count exactly, the object table must divide by its record
//! size, and the bytes left after it must equal the declared map-info size. Any
//! quadtree written a level too shallow, or any LZO payload whose end marker is
//! missing, desynchronises the stream and lands on one of those checks.
bool VerifyWorld(const std::string& path, const BuiltWorld& built, const ExportOptions& options)
{
    namespace Fmt = Poseidon::Asset::Formats;
    QIFStream file;
    file.open(path.c_str());
    if (file.fail())
    {
        std::cerr << "Verify: cannot reopen " << path << "\n";
        return false;
    }
    Fmt::BinaryReader reader(file);
    const int32_t revision = Fmt::World::PeekOprwModernRevision(reader);
    if (revision != 25)
    {
        std::cerr << "Verify: written file is not an OPRW 25 the runtime accepts (peeked " << revision << ")\n";
        return false;
    }
    Fmt::World::Oprw25World world;
    try
    {
        world = Fmt::World::ReadOprwModern(reader, revision);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Verify: " << e.what() << "\n";
        return false;
    }

    bool ok = true;
    const auto check = [&ok](bool condition, const char* what)
    {
        std::cout << "  " << std::left << std::setw(34) << what << (condition ? "ok" : "MISMATCH") << "\n";
        if (!condition)
            ok = false;
    };
    check(world.header.landRangeX == options.landRange && world.header.landRangeY == options.landRange, "land range");
    check(world.header.terrainRangeX == options.terrainRange && world.header.terrainRangeY == options.terrainRange,
          "terrain range");
    check(std::fabs(world.header.landCellSize - options.landCellSize) < 1e-6f, "land cell size");
    check(world.elevation.size() == built.elevation.size(), "elevation element count");
    check(world.geography.size() == static_cast<size_t>(options.landRange) * options.landRange, "geography cells");
    check(world.materialIndex.size() == static_cast<size_t>(options.landRange) * options.landRange, "material index cells");
    check(static_cast<int>(world.materials.size()) == built.materialCount, "material table");
    // The reader itself throws on a model index outside the table and on a
    // trailing map-info size that does not match, so reaching here already proves
    // both. What is worth stating is that every record survived the round trip.
    check(world.objects.size() == built.objectCount, "object table round-trips");
    check(world.models.size() == built.modelCount, "model table round-trips");

    // The elevation is the payload that matters, and the one an LZO or quadtree
    // mistake would corrupt without changing any size. Compare every sample.
    size_t differing = 0;
    float worst = 0.0f;
    if (world.elevation.size() == built.elevation.size())
    {
        for (size_t i = 0; i < built.elevation.size(); ++i)
        {
            const float delta = std::fabs(world.elevation[i] - built.elevation[i]);
            if (delta > 0.0f)
            {
                ++differing;
                worst = std::max(worst, delta);
            }
        }
    }
    check(differing == 0, "elevation round-trips exactly");
    if (differing)
        std::cout << "    " << differing << " samples differ, worst " << worst << " m\n";

    // The material index is the other quadtree with real content, and the one that
    // would silently pick the wrong texture if the tree were written wrong.
    size_t badIndex = 0;
    for (uint16_t index : world.materialIndex)
        if (index >= world.materials.size())
            ++badIndex;
    check(badIndex == 0, "every material index in range");

    std::cout << "  terrain cell size                 " << std::fixed << std::setprecision(4)
              << world.header.TerrainCellSize() << " m\n";
    std::cout << "  world extent                      " << std::setprecision(1) << world.header.WorldExtent()
              << " m\n";
    return ok;
}

} // namespace Oprw

int ExportWorld(const std::string& root, const std::string& world, const std::string& outputPath, int landRange,
                int terrainRange, const std::vector<std::string>& materials, const std::string& assetDir,
                const std::string& assetVirtualDir, int satelliteSize, int maskSize,
                const std::string& placementsPath)
{
    const bool wantMaterials = !assetDir.empty();
    const AssembledTerrain terrain = AssembleTerrain(root, world, wantMaterials);
    if (!terrain.valid())
    {
        std::cerr << "Error: " << terrain.error << "\n";
        return 1;
    }
    PrintTerrainSummary(terrain);
    if (terrain.tilesFailed || terrain.tilesPlaced == 0)
    {
        std::cerr << "Error: refusing to export a world with unread tiles\n";
        return 1;
    }
    if (wantMaterials)
    {
        std::cout << "Surface cells: " << terrain.subCells << " sub-cells, " << terrain.subCellsFailed << " failed";
        if (!terrain.materialError.empty())
            std::cout << "  (first: " << terrain.materialError << ")";
        std::cout << "  raster " << terrain.materialEdge << "x" << terrain.materialEdge << "\n";
        if (terrain.subCellsFailed != 0)
        {
            std::cerr << "Error: refusing to author materials from a partly decoded surface assignment\n";
            return 1;
        }
    }

    Oprw::ExportOptions options;
    options.landRange = landRange;
    options.terrainRange = terrainRange;
    options.materials = materials;

    // Authoring the materials can only ADD to the material table, so it runs
    // before the world is built and its RVMAT path is appended to whatever
    // --material already named.
    if (wantMaterials)
    {
        const Surfaces::MaterialAssets assets = Surfaces::GenerateTerrainMaterials(
            terrain, root, assetDir, assetVirtualDir, satelliteSize, maskSize);
        if (!assets.error.empty())
        {
            std::cerr << "Error: " << assets.error << "\n";
            return 1;
        }
        for (const std::string& rvmat : assets.rvmats)
            options.materials.push_back(rvmat);
    }

    Placements::Set placements;
    if (!placementsPath.empty())
    {
        placements = Placements::Read(placementsPath, terrain.descriptor.WorldWidth());
        if (!placements.error.empty())
        {
            std::cerr << "Error: " << placements.error << "\n";
            return 1;
        }
        std::cout << "Placements:  " << placements.items.size() << " objects over " << placements.models.size()
                  << " models, " << placements.skipped << " skipped\n";
        // Before the object table is written, and only here: this is the one point
        // in the pipeline that holds both the placements and a heightfield.
        double meanRise = 0.0;
        const size_t resolved = Placements::ResolveTerrainRelative(placements, terrain, meanRise);
        std::cout << "             " << resolved << " of " << placements.items.size()
                  << " had a terrain-relative Y, raised by " << std::fixed << std::setprecision(2) << meanRise
                  << " m on average\n";
        if (placements.terrainRelative != 0 && resolved != placements.terrainRelative)
            std::cerr << "Warning: " << placements.terrainRelative - resolved
                      << " terrain-relative placements were left absolute -- the heightfield refused to sample\n";
        if (placements.items.empty())
        {
            std::cerr << "Error: the placement manifest yielded no usable objects\n";
            return 1;
        }
        options.placements = &placements;
    }
    // The land cell size is chosen so the container's extent equals the authored
    // extent exactly, rather than being a round number that nearly matches.
    options.landCellSize = terrain.descriptor.WorldWidth() / static_cast<float>(landRange);

    if (landRange <= 0 || (landRange & (landRange - 1)) != 0 || terrainRange <= 0 ||
        (terrainRange & (terrainRange - 1)) != 0 || terrainRange % landRange != 0)
    {
        std::cerr << "Error: land and terrain ranges must be powers of two with terrain a multiple of land\n";
        return 1;
    }

    std::cout << "\nExporting OPRW 25: land " << landRange << " / terrain " << terrainRange << " @ "
              << std::fixed << std::setprecision(4) << options.landCellSize << " m land cell = "
              << std::setprecision(4) << options.landCellSize * landRange / terrainRange << " m terrain cell, "
              << std::setprecision(1) << terrain.descriptor.WorldWidth() << " m extent\n";

    const Oprw::BuiltWorld built = Oprw::BuildOprw25(terrain, options);
    std::cout << "Elevation:   " << std::setprecision(2) << built.minHeight << " m .. " << built.maxHeight << " m\n";
    std::cout << "Peak at:     X " << std::setprecision(0) << built.peakX << "  Z " << built.peakZ << "\n";

    // A coarse land/water sketch, so a camera position can be chosen from measured
    // ground rather than from a guess that lands in the sea. Row 0 is Z = 0.
    {
        const int cells = 32;
        const int block = terrainRange / cells;
        std::cout << "Land sketch (row 0 = Z 0, col 0 = X 0; '#' >40 m, '+' >0 m, '.' sea):\n";
        for (int r = 0; r < cells; ++r)
        {
            std::cout << "  ";
            for (int c = 0; c < cells; ++c)
            {
                float sum = 0.0f;
                for (int z = r * block; z < (r + 1) * block; z += 8)
                    for (int x = c * block; x < (c + 1) * block; x += 8)
                        sum += built.elevation[static_cast<size_t>(z) * terrainRange + x];
                const float mean = sum / static_cast<float>((block / 8) * (block / 8));
                std::cout << (mean > 40.0f ? '#' : mean > 0.0f ? '+' : '.');
            }
            std::cout << "\n";
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path(), ec);
    {
        std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            std::cerr << "Error: cannot open " << outputPath << " for writing\n";
            return 1;
        }
        file.write(reinterpret_cast<const char*>(built.bytes.data()), static_cast<std::streamsize>(built.bytes.size()));
        if (!file)
        {
            std::cerr << "Error: failed writing " << outputPath << "\n";
            return 1;
        }
    }
    std::cout << "Wrote " << outputPath << " (" << built.bytes.size() << " bytes)\n";

    std::cout << "\nVerifying with the runtime's own reader:\n";
    if (!Oprw::VerifyWorld(outputPath, built, options))
    {
        std::cerr << "Error: the written world does not read back correctly\n";
        return 1;
    }
    std::cout << "OK\n";
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
        static std::string surfaceFilter;
        auto* cmd = pak->add_subcommand("clutter", "Report Reforger's own ground cover per terrain surface");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", surfaceFilter, "Only surfaces containing this substring");
        cmd->callback(
            []()
            {
                EnfusionMount mount;
                if (!mount.Open(root))
                {
                    std::cerr << "no .pak archives under " << root << std::endl;
                    std::exit(1);
                }
                const std::string needle = LowerCopy(surfaceFilter);
                size_t surfaces = 0, withBlades = 0, withPlants = 0, plantTotal = 0;
                std::set<std::string> distinctModels;
                for (const std::string& path : mount.FindAll(".emat"))
                {
                    const std::string lower = LowerCopy(path);
                    if (lower.find("terrains/") == std::string::npos ||
                        lower.find("/surfaces/") == std::string::npos)
                        continue;
                    if (!needle.empty() && lower.find(needle) == std::string::npos)
                        continue;
                    ++surfaces;
                    const SurfaceClutter clutter = ReadSurfaceClutter(mount, path);
                    if (!clutter.valid())
                    {
                        std::cout << "  UNRESOLVED " << path << ": " << clutter.error << std::endl;
                        continue;
                    }
                    if (clutter.blades.valid())
                        ++withBlades;
                    if (!clutter.plants.empty())
                        ++withPlants;
                    plantTotal += clutter.plants.size();
                    for (const auto& plant : clutter.plants)
                        distinctModels.insert(LowerCopy(plant.model));
                    if (withPlants <= 3 && !clutter.plants.empty())
                    {
                        std::cout << "  " << path << std::endl;
                        std::cout << "    blades " << clutter.blades.plants << " plants x "
                                  << clutter.blades.bladesPerPlant << " blades, height "
                                  << clutter.blades.height << " m, atlas " << clutter.blades.colour << std::endl;
                        std::cout << "    mask " << clutter.distributionMap << std::endl;
                        for (const auto& plant : clutter.plants)
                            std::cout << "    plant " << plant.model << " density " << plant.density << " scale "
                                      << plant.minScale << ".." << plant.maxScale << std::endl;
                    }
                }
                std::cout << std::endl << surfaces << " terrain surfaces; " << withBlades
                          << " carry a procedural blade layer, " << withPlants << " carry scattered plants ("
                          << plantTotal << " definitions, " << distinctModels.size() << " distinct models)."
                          << std::endl;
                std::exit(0);
            });
    }
    {
        static std::string root;
        static std::string wanted;
        static int maxBytes = 8192;
        static std::string outPath;
        auto* cmd = pak->add_subcommand("cat", "Print one archive entry as text");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-e,--entry", wanted, "Path inside the archive (case-insensitive substring)")->required();
        cmd->add_option("-n,--bytes", maxBytes, "Maximum bytes to print (default 8192)");
        // RFG-067: write to a FILE, in binary.
        //
        // Printing an archive entry to stdout is fine for text and destroys anything
        // else on Windows, where the C runtime turns every 0x0A into 0D 0A on the way
        // out. A 5.6 MB `.edds` extracted that way grew corrupt in a way that reads as
        // "this file has no ENF1 marker" -- a container error for a container that was
        // never touched.
        cmd->add_option("-o,--out", outPath, "Write the entry to this file instead (binary, exact bytes)");
        cmd->callback(
            []()
            {
                const std::string needle = LowerCopy(wanted);
                for (const std::string& path : FindPaks(root))
                {
                    PakArchive archive;
                    if (!archive.Open(path))
                        continue;
                    for (const PakEntry& entry : archive.Entries())
                    {
                        if (LowerCopy(entry.path).find(needle) == std::string::npos)
                            continue;
                        std::vector<uint8_t> bytes;
                        if (!archive.Read(entry, bytes))
                            continue;
                        if (!outPath.empty())
                        {
                            std::ofstream out(outPath, std::ios::binary);
                            out.write(reinterpret_cast<const char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()));
                            std::cout << entry.path << " -> " << outPath << " (" << bytes.size() << " bytes)"
                                      << std::endl;
                            std::exit(out ? 0 : 1);
                        }
                        std::cout << "=== " << entry.path << " (" << bytes.size() << " bytes) ===" << std::endl;
                        const size_t n = std::min(bytes.size(), static_cast<size_t>(maxBytes));
                        std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(n));
                        std::cout << std::endl;
                        std::exit(0);
                    }
                }
                std::cerr << "no entry matching '" << wanted << "'" << std::endl;
                std::exit(1);
            });
    }
    {
        static std::string root;
        static std::string filter;
        static int topClasses = 0;
        static bool roads = false;
        auto* cmd = pak->add_subcommand("worlds", "Read every .ent (EBIN) world and report closure and placements");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring");
        cmd->add_option("--classes", topClasses, "Also print the N commonest entity classes");
        cmd->add_flag("--roads", roads, "Report road spline centrelines");
        cmd->callback([]() { std::exit(Worlds(root, filter, topClasses, roads)); });
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
    {
        static std::string root;
        static std::string world;
        static std::string output;
        static int landRange = 256;
        static int terrainRange = 2048;
        static std::vector<std::string> materials;
        static std::string assetDir;
        static std::string assetVirtualDir;
        static int satelliteSize = 4096;
        static int maskSize = 2048;
        static std::string placements;
        auto* cmd = pak->add_subcommand("export", "Export a world's terrain as an OPRW 25 .wrp the runtime loads");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-w,--world", world, "Substring naming the world, e.g. worlds/Eden")->required();
        cmd->add_option("-o,--output", output, "Output .wrp path")->required();
        cmd->add_option("--land-range", landRange, "Land grid cells per edge, a power of two (default 256)");
        cmd->add_option("--terrain-range", terrainRange,
                        "Terrain grid cells per edge, a power of two multiple of --land-range (default 2048)");
        cmd->add_option("--material", materials,
                        "Virtual path of a terrain surface material, repeatable. Omit for an untextured world.");
        cmd->add_option("--terrain-assets", assetDir,
                        "Author the world's surfaces into this directory: satellite, selector mask, ground "
                        "surfaces and a TerrainSNX RVMAT, decoded from the .terr MATS list and the .ttile TMAT "
                        "assignment. The RVMAT is appended to the material table.");
        cmd->add_option("--terrain-vdir", assetVirtualDir,
                        "Virtual directory the authored assets are named by, e.g. reforger\\everon\\data. Files "
                        "are written under --terrain-assets at this path so the tree can be copied to the game "
                        "directory whole.");
        cmd->add_option("--satellite-size", satelliteSize, "Satellite edge in texels (default 4096)");
        cmd->add_option("--mask-size", maskSize,
                        "Selector mask edge in texels (default 2048; 4096 DXT3 does not round-trip)");
        cmd->add_option("--placements", placements,
                        "Object placement manifest: tab-separated virtualPath, x, y, z, yawDeg, scale "
                        "and an optional seventh field. y is absolute world elevation unless that field "
                        "is 1, in which case it is an offset above the terrain and is resolved here. A "
                        "six-field manifest reads as entirely absolute, which is what it meant.");
        cmd->callback(
            []() {
                std::exit(ExportWorld(root, world, output, landRange, terrainRange, materials, assetDir,
                                      assetVirtualDir, satelliteSize, maskSize, placements));
            });
    }
}

} // namespace PoseidonTools
