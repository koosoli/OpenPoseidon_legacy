#include "PakCommand.hpp"

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
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
int Worlds(const std::string& root, const std::string& filter)
{
    size_t files = 0, closed = 0, placements = 0;
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
    std::string error;

    bool valid() const { return error.empty(); }
    float HeightAt(uint32_t x, uint32_t z) const
    {
        return descriptor.HeightAt(heights[static_cast<size_t>(z) * descriptor.gridWidth + x]);
    }
};

AssembledTerrain AssembleTerrain(const std::string& root, const std::string& world)
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
    std::vector<std::string> materials; //!< virtual paths written into the material table
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

    out.i32(0); // models
    out.i32(0); // static entities

    // Per-cell offsets into the object and map-info tables. Both tables are empty,
    // so both trees are uniform zero; the reader reads and discards them anyway.
    const std::vector<uint32_t> zeroOffsets(landCells, 0);
    QuadTreeWriter(land, land, 4, reinterpret_cast<const uint8_t*>(zeroOffsets.data())).Write(out);
    out.i32(0); // object table bytes
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
    check(world.objects.empty() && world.models.empty(), "object table empty");

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
                int terrainRange, const std::vector<std::string>& materials)
{
    const AssembledTerrain terrain = AssembleTerrain(root, world);
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

    Oprw::ExportOptions options;
    options.landRange = landRange;
    options.terrainRange = terrainRange;
    options.materials = materials;
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
        static std::string filter;
        auto* cmd = pak->add_subcommand("worlds", "Read every .ent (EBIN) world and report closure and placements");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring");
        cmd->callback([]() { std::exit(Worlds(root, filter)); });
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
        auto* cmd = pak->add_subcommand("export", "Export a world's terrain as an OPRW 25 .wrp the runtime loads");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-w,--world", world, "Substring naming the world, e.g. worlds/Eden")->required();
        cmd->add_option("-o,--output", output, "Output .wrp path")->required();
        cmd->add_option("--land-range", landRange, "Land grid cells per edge, a power of two (default 256)");
        cmd->add_option("--terrain-range", terrainRange,
                        "Terrain grid cells per edge, a power of two multiple of --land-range (default 2048)");
        cmd->add_option("--material", materials,
                        "Virtual path of a terrain surface material, repeatable. Omit for an untextured world.");
        cmd->callback([]() { std::exit(ExportWorld(root, world, output, landRange, terrainRange, materials)); });
    }
}

} // namespace PoseidonTools
