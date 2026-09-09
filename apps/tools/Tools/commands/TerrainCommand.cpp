#include "TerrainCommand.hpp"
#include "../SDLPreview.hpp"
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/Asset/Formats/World/Oprw20.hpp>
#include <Poseidon/Asset/Formats/World/Oprw24.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol49.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Asset/Probes/AssetPreview.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <memory>
#include <fstream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <vector>
#include <CLI/App.hpp>
#include <CLI/Option.hpp>
#include <CLI/Validators.hpp>
#include <cstdio>
#include <functional>
#include <string>
#include <Poseidon/Foundation/Math/Math3DP.hpp>

using namespace Poseidon;

#ifdef GetObject
#undef GetObject
#endif

namespace PoseidonTools
{

namespace
{

std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value)
    {
        if (c == '\\' || c == '"')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
            out += buffer;
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

// -- revision-25 map rendering -----------------------------------------------
//
// The legacy preview normalises height across the file's own min and max, which
// is right for OFP worlds and wrong for these: an Arma 3 world has a real sea
// level at y = 0 and most of its terrain cells are seabed. Normalised that way,
// Stratis renders as a smear with the coastline nowhere near the water line.
//
// So elevation is shaded against absolute zero, and the surface is hillshaded --
// without a light the interior of an island is a flat colour field and nothing
// about the terrain can be judged from it.
Poseidon::PreviewImage RenderOprw25Map(const Poseidon::Asset::Formats::World::Oprw25World& world, bool withObjects)
{
    const int w = world.header.terrainRangeX;
    const int h = world.header.terrainRangeY;
    const float cell = world.header.TerrainCellSize();

    Poseidon::PreviewImage image;
    image.width = w;
    image.height = h;
    image.data.assign(static_cast<size_t>(w) * h * 4, 0);

    float highest = 1.0f;
    for (const float height : world.elevation)
        if (height > highest)
            highest = height;

    auto heightAt = [&](int x, int z)
    {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        z = z < 0 ? 0 : (z >= h ? h - 1 : z);
        return world.elevation[static_cast<size_t>(z) * w + x];
    };

    for (int z = 0; z < h; ++z)
    {
        for (int x = 0; x < w; ++x)
        {
            const float height = heightAt(x, z);

            float r, g, b;
            if (height < 0.0f)
            {
                // Depth ramp. Stratis reaches -157 m, so a linear ramp puts the
                // whole shelf in one shade; the square root keeps the shallows
                // -- which is where the coastline actually reads -- separable.
                const float depth = std::sqrt(std::min(1.0f, -height / 120.0f));
                r = 12.0f + (1.0f - depth) * 40.0f;
                g = 40.0f + (1.0f - depth) * 90.0f;
                b = 70.0f + (1.0f - depth) * 110.0f;
            }
            else
            {
                const float t = std::min(1.0f, height / highest);
                if (t < 0.45f)
                {
                    const float s = t / 0.45f;
                    r = 74.0f + s * 84.0f;
                    g = 118.0f + s * 32.0f;
                    b = 52.0f + s * 18.0f;
                }
                else
                {
                    const float s = (t - 0.45f) / 0.55f;
                    r = 158.0f + s * 62.0f;
                    g = 150.0f + s * 70.0f;
                    b = 70.0f + s * 130.0f;
                }
            }

            // Water is decided by height alone, deliberately. The geography grid
            // carries water-depth bits and they were tried here first; on both
            // local worlds they change no pixel, because every cell they flag is
            // already below zero (Stratis: 72.1% of land cells flagged, against
            // 71.3% of terrain cells below sea level -- two grids agreeing).
            // A world with genuine inland water would need them, but neither of
            // these has any, so the branch would be untested code that looks
            // supported. Altis's flat eastern basin is not a counterexample: it
            // sits at 2.6-10.5 m, carries no placements, and is flagged as land
            // because it is land -- a dry salt flat.

            // Hillshade from the height gradient, light from the north-west.
            const float dx = (heightAt(x + 1, z) - heightAt(x - 1, z)) / (2.0f * cell);
            const float dz = (heightAt(x, z + 1) - heightAt(x, z - 1)) / (2.0f * cell);
            float shade = (0.55f * -dx + 0.55f * dz + 1.0f) / std::sqrt(dx * dx + dz * dz + 1.0f);
            shade = 0.55f + 0.45f * std::max(0.0f, std::min(1.6f, shade));
            if (height < 0.0f)
                shade = 1.0f; // water is flat; shading the seabed only adds noise

            // Row 0 is north: the world's +Z runs north, images run downward.
            const size_t pixel = (static_cast<size_t>(h - 1 - z) * w + x) * 4;
            image.data[pixel + 0] = static_cast<uint8_t>(std::min(255.0f, r * shade));
            image.data[pixel + 1] = static_cast<uint8_t>(std::min(255.0f, g * shade));
            image.data[pixel + 2] = static_cast<uint8_t>(std::min(255.0f, b * shade));
            image.data[pixel + 3] = 255;
        }
    }

    if (withObjects)
    {
        // Additive, so density is the signal: one tree is barely visible, a
        // treeline or a town saturates. Plotting them opaquely would just paint
        // 163,953 identical dots over the terrain that gives them context.
        for (const auto& object : world.objects)
        {
            const int x = static_cast<int>(object.transform.rows[3].x / cell);
            const int z = static_cast<int>(object.transform.rows[3].z / cell);
            if (x < 0 || x >= w || z < 0 || z >= h)
                continue;
            const size_t pixel = (static_cast<size_t>(h - 1 - z) * w + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int lift = channel == 2 ? 40 : 70;
                image.data[pixel + channel] = static_cast<uint8_t>(std::min(255, image.data[pixel + channel] + lift));
            }
        }
    }

    return image;
}

// -- addon index -------------------------------------------------------------
//
// Maps a world's virtual asset paths onto the archives of a local install. The
// world names its models as `a3\rocks_f\water\x.p3d`; the archive stores that
// entry as `water\x.p3d` under a bank whose prefix is `a3\rocks_f`. Ignoring the
// prefix makes every Arma 3 path miss -- measured as 0 of Stratis's 350 models
// resolving, versus 350 of 350 once the prefix is applied.
struct AddonEntry
{
    std::string pboPath;
    std::string entryName; // as stored in the bank, prefix removed
};

std::string CanonicalAssetPath(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}

using AddonIndex = std::map<std::string, AddonEntry>;

AddonIndex BuildAddonIndex(const std::string& root, int& archivesRead)
{
    AddonIndex index;
    archivesRead = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        std::string extension = it->path().extension().string();
        for (char& c : extension)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".pbo")
            continue;

        const std::string pboPath = it->path().string();
        const auto info = Poseidon::InspectPbo(pboPath);
        if (!info.valid)
            continue;
        ++archivesRead;

        std::string prefix = CanonicalAssetPath(info.prefix);
        if (!prefix.empty() && prefix.back() != '/')
            prefix.push_back('/');
        for (const auto& entry : info.entries)
        {
            // First archive wins, which is the load order the engine would apply
            // in the absence of an explicit mod list. Both worlds' assets are
            // unambiguous, but recording the rule keeps a later duplicate honest.
            index.emplace(prefix + CanonicalAssetPath(entry.name), AddonEntry{pboPath, entry.name});
        }
    }
    return index;
}

// What happened when a referenced model was actually opened and parsed. Ordered
// worst-to-best so the report reads as a ladder.
enum class ModelOutcome
{
    Missing,             // no archive in the install provides this path
    Unreadable,          // the archive has it but the entry would not read
    NotP3D,              // the bytes are not a model container at all
    MlodSource,          // unbinarized source geometry, not a runtime asset
    UnsupportedRevision, // recognised ODOL, revision this build does not read
    ParseFailed,         // supported revision, but the file did not parse
    Static,              // parsed as static geometry
};

const char* ModelOutcomeName(ModelOutcome outcome)
{
    switch (outcome)
    {
        case ModelOutcome::Missing:
            return "missing";
        case ModelOutcome::Unreadable:
            return "unreadable";
        case ModelOutcome::NotP3D:
            return "not_p3d";
        case ModelOutcome::MlodSource:
            return "mlod_source";
        case ModelOutcome::UnsupportedRevision:
            return "unsupported_revision";
        case ModelOutcome::ParseFailed:
            return "parse_failed";
        case ModelOutcome::Static:
            return "static";
    }
    return "unknown";
}

struct ModelResult
{
    ModelOutcome outcome = ModelOutcome::Missing;
    uint32_t revision = 0;
    std::string detail;
};

ModelResult ClassifyModel(const std::string& virtualPath, const AddonIndex& index)
{
    ModelResult result;
    const auto found = index.find(CanonicalAssetPath(virtualPath));
    if (found == index.end())
        return result;

    QFBank bank;
    std::string bankName = found->second.pboPath;
    if (bankName.size() > 4)
        bankName.resize(bankName.size() - 4); // QFBank wants the name without .pbo
    if (!bank.open(RString(bankName.c_str())))
    {
        result.outcome = ModelOutcome::Unreadable;
        result.detail = "cannot open archive";
        return result;
    }
    bank.Lock();
    Ref<IFileBuffer> buffer = bank.error() ? Ref<IFileBuffer>() : bank.Read(found->second.entryName.c_str());
    if (!buffer || buffer->GetSize() < 8)
    {
        bank.Unlock();
        result.outcome = ModelOutcome::Unreadable;
        result.detail = "entry did not read";
        return result;
    }

    const auto* bytes = static_cast<const char*>(buffer->GetData());
    const int size = static_cast<int>(buffer->GetSize());

    if (std::memcmp(bytes, "P3DM", 4) == 0 || std::memcmp(bytes, "MLOD", 4) == 0)
    {
        bank.Unlock();
        result.outcome = ModelOutcome::MlodSource;
        return result;
    }
    if (std::memcmp(bytes, "ODOL", 4) != 0)
    {
        bank.Unlock();
        result.outcome = ModelOutcome::NotP3D;
        return result;
    }

    QIStream stream(bytes, size);
    Asset::Formats::BinaryReader reader(stream);
    try
    {
        const auto revision = Asset::Formats::P3D::PeekOdolRevision(reader);
        result.revision = revision.version;
        const bool readable = revision.support == Asset::Formats::P3D::OdolSupport::Parsed ||
                              revision.support == Asset::Formats::P3D::OdolSupport::NarrowSubset;
        if (!readable)
        {
            bank.Unlock();
            result.outcome = ModelOutcome::UnsupportedRevision;
            return result;
        }
        // A revision this build reads is only a claim about the container. The
        // census wants the file actually parsed, because "revision 73 is
        // supported" and "this revision-73 file reads" are different facts --
        // 73 is a NarrowSubset, so most of the difference is the point.
        //
        // Dispatch by revision the way ODOLLoader does. Feeding an Arma 2 model to
        // the revision-73 reader reports a parse failure that is really a census
        // defect, and that misclassification is invisible in the histogram.
        switch (result.revision)
        {
            case 40:
                // AST-020: Armed Assault's only revision. Without this the census
                // reports every A1 model as a bare container read and the
                // histogram cannot tell "parses" from "recognised".
                (void)Asset::Formats::P3D::ReadOdol40StaticModel(reader, size);
                break;
            case 48:
            case 49:
            case 50:
            case 52:
                (void)Asset::Formats::P3D::ReadOdolA2StaticModel(reader, size, result.revision);
                break;
            case 73:
                (void)Asset::Formats::P3D::ReadOdol73StaticModel(reader, size);
                break;
            default:
                // OFP revision 7 has no static-subset reader; the container is all
                // this census can honestly claim for it.
                break;
        }
        result.outcome = ModelOutcome::Static;
    }
    catch (const std::exception& e)
    {
        result.outcome = ModelOutcome::ParseFailed;
        result.detail = e.what();
    }
    bank.Unlock();
    return result;
}

// Counts every dimension the world exposes today. Anything not yet implemented is
// absent from this report rather than reported as zero, so a later run that adds a
// stage cannot be mistaken for a regression in one that was already there.
struct WorldCensus
{
    int placements = 0;
    int distinctModelsReferenced = 0;
    int modelsInTable = 0;
    int modelsUnreferenced = 0;
    int staticEntities = 0;
    int distinctEntityClasses = 0;
    int roadLinks = 0;
    int distinctRoadModels = 0;
    int surfaceMaterials = 0;
    int surfaceMaterialsUsed = 0;
    int landCellsBelowSeaLevel = 0;
    float minElevation = 0.0f;
    float maxElevation = 0.0f;
    size_t mapInfoBytes = 0;
};

WorldCensus MeasureWorld(const Poseidon::Asset::Formats::World::Oprw25World& world)
{
    WorldCensus census;
    census.placements = static_cast<int>(world.objects.size());
    census.modelsInTable = static_cast<int>(world.models.size());

    std::set<int32_t> referenced;
    for (const auto& object : world.objects)
        referenced.insert(object.modelIndex);
    census.distinctModelsReferenced = static_cast<int>(referenced.size());
    census.modelsUnreferenced = census.modelsInTable - census.distinctModelsReferenced;

    census.staticEntities = static_cast<int>(world.staticEntities.size());
    std::set<std::string> entityClasses;
    for (const auto& entity : world.staticEntities)
        entityClasses.insert(entity.className);
    census.distinctEntityClasses = static_cast<int>(entityClasses.size());

    std::set<std::string> roadModels;
    for (const auto& cell : world.roadNet)
        for (const auto& link : cell)
        {
            ++census.roadLinks;
            if (!link.p3dPath.empty())
                roadModels.insert(link.p3dPath);
        }
    census.distinctRoadModels = static_cast<int>(roadModels.size());

    census.surfaceMaterials = static_cast<int>(world.materials.size());
    std::set<uint16_t> usedSurfaces(world.materialIndex.begin(), world.materialIndex.end());
    census.surfaceMaterialsUsed = static_cast<int>(usedSurfaces.size());

    if (!world.elevation.empty())
    {
        const auto bounds = std::minmax_element(world.elevation.begin(), world.elevation.end());
        census.minElevation = *bounds.first;
        census.maxElevation = *bounds.second;
        census.landCellsBelowSeaLevel = static_cast<int>(
            std::count_if(world.elevation.begin(), world.elevation.end(), [](float height) { return height < 0.0f; }));
    }
    census.mapInfoBytes = world.mapInfo.size();
    return census;
}

void PrintWorldCensus(const Poseidon::Asset::Formats::World::Oprw25World& world, const std::string& path, bool asJson)
{
    const auto& header = world.header;
    const WorldCensus c = MeasureWorld(world);

    if (asJson)
    {
        std::cout << "{\n"
                  << "  \"world\": \"" << JsonEscape(path) << "\",\n"
                  << "  \"world_parse\": \"ok\",\n"
                  << "  \"container\": \"OPRW\",\n"
                  << "  \"revision\": " << header.version << ",\n"
                  << "  \"app_id\": " << header.appId << ",\n"
                  << "  \"land_grid\": [" << header.landRangeX << ", " << header.landRangeY << "],\n"
                  << "  \"terrain_grid\": [" << header.terrainRangeX << ", " << header.terrainRangeY << "],\n"
                  << "  \"land_cell_size_m\": " << header.landCellSize << ",\n"
                  << "  \"terrain_cell_size_m\": " << header.TerrainCellSize() << ",\n"
                  << "  \"world_extent_m\": " << header.WorldExtent() << ",\n"
                  << "  \"elevation_min_m\": " << c.minElevation << ",\n"
                  << "  \"elevation_max_m\": " << c.maxElevation << ",\n"
                  << "  \"terrain_cells_below_sea_level\": " << c.landCellsBelowSeaLevel << ",\n"
                  << "  \"mountains\": " << world.mountains.size() << ",\n"
                  << "  \"surface_materials\": " << c.surfaceMaterials << ",\n"
                  << "  \"surface_materials_used\": " << c.surfaceMaterialsUsed << ",\n"
                  << "  \"placements\": " << c.placements << ",\n"
                  << "  \"models_in_table\": " << c.modelsInTable << ",\n"
                  << "  \"models_referenced\": " << c.distinctModelsReferenced << ",\n"
                  << "  \"models_unreferenced\": " << c.modelsUnreferenced << ",\n"
                  << "  \"static_entities\": " << c.staticEntities << ",\n"
                  << "  \"static_entity_classes\": " << c.distinctEntityClasses << ",\n"
                  << "  \"road_links\": " << c.roadLinks << ",\n"
                  << "  \"road_models\": " << c.distinctRoadModels << ",\n"
                  << "  \"map_info_bytes\": " << c.mapInfoBytes << "\n"
                  << "}\n";
        return;
    }

    std::cout << "World:            " << path << "\n";
    std::cout << "Container:        OPRW revision " << header.version << " (app id " << header.appId << ")\n";
    std::cout << "world_parse:      ok\n\n";

    std::cout << "-- terrain --\n";
    std::cout << "Land grid:        " << header.landRangeX << " x " << header.landRangeY << " @ " << std::fixed
              << std::setprecision(1) << header.landCellSize << " m\n";
    std::cout << "Terrain grid:     " << header.terrainRangeX << " x " << header.terrainRangeY << " @ "
              << header.TerrainCellSize() << " m\n";
    std::cout << "World extent:     " << header.WorldExtent() << " m square\n";
    std::cout << "Elevation:        " << c.minElevation << " .. " << c.maxElevation << " m\n";
    std::cout << "Below sea level:  " << c.landCellsBelowSeaLevel << " / " << world.elevation.size()
              << " terrain cells\n";
    std::cout << "Mountains:        " << world.mountains.size() << "\n\n";

    // The geography grid's own flags, reported because they are the only per-cell
    // semantics the world carries besides elevation and surface material, and
    // because a count of zero is itself a finding.
    {
        int water = 0, forest = 0, road = 0, objects = 0;
        for (const int16_t bits : world.geography)
        {
            const Poseidon::Asset::Formats::World::Oprw25Geography cell{bits};
            if (cell.MaxWaterDepth() > 0 || cell.MinWaterDepth() > 0)
                ++water;
            if (cell.Forest())
                ++forest;
            if (cell.Road())
                ++road;
            if (cell.SomeObjects())
                ++objects;
        }
        std::cout << "-- geography flags (per land cell) --\n";
        std::cout << "Cells:            " << world.geography.size() << "\n";
        std::cout << "Water depth set:  " << water << "\n";
        std::cout << "Forest:           " << forest << "\n";
        std::cout << "Road:             " << road << "\n";
        std::cout << "Some objects:     " << objects << "\n\n";
    }

    std::cout << "-- surface materials --\n";
    std::cout << "Declared:         " << c.surfaceMaterials << "\n";
    std::cout << "Distinct in use:  " << c.surfaceMaterialsUsed << "\n\n";

    std::cout << "-- placements --\n";
    std::cout << "Objects:          " << c.placements << "\n";
    std::cout << "Model table:      " << c.modelsInTable << "\n";
    std::cout << "Models referenced:" << c.distinctModelsReferenced << "\n";
    std::cout << "Models unused:    " << c.modelsUnreferenced << "\n";
    std::cout << "Static entities:  " << c.staticEntities << " (" << c.distinctEntityClasses << " classes)\n\n";

    std::cout << "-- roads --\n";
    std::cout << "Links:            " << c.roadLinks << "\n";
    std::cout << "Segment models:   " << c.distinctRoadModels << "\n\n";

    std::cout << "-- not yet measured --\n";
    std::cout << "Map info:         " << c.mapInfoBytes << " bytes retained, not decoded\n";
    std::cout << "Model resolution, material families, and renderable state are not part of this census yet.\n";
}

// Weighted by placement count as well as by distinct model, because those two
// denominators answer different questions. 349 of 350 models parsing sounds like
// near-complete coverage; if the one failure is the model placed 40,000 times, the
// world is still mostly missing.

// Read one virtual asset path out of the addon index, as bytes.
//
// The bank is cached by archive path. Opening a QFBank re-reads and re-indexes the
// whole PBO, and a terrain census asks for thousands of small files out of one
// archive -- Sahrani's 5,126 materials all live in `sara.pbo`, so re-opening per
// material turned a two-second report into minutes.
bool ReadIndexedAsset(const AddonIndex& index, const std::string& virtualPath, std::vector<uint8_t>& out)
{
    static std::map<std::string, std::shared_ptr<QFBank>> banks;
    const auto found = index.find(CanonicalAssetPath(virtualPath));
    if (found == index.end())
        return false;
    std::string bankName = found->second.pboPath;
    if (bankName.size() > 4)
        bankName.resize(bankName.size() - 4);
    auto cached = banks.find(bankName);
    if (cached == banks.end())
    {
        auto bank = std::make_shared<QFBank>();
        if (!bank->open(RString(bankName.c_str())) || bank->error())
            bank.reset();
        cached = banks.emplace(bankName, std::move(bank)).first;
    }
    if (!cached->second)
        return false;
    QFBank& bank = *cached->second;
    bank.Lock();
    Ref<IFileBuffer> buffer = bank.Read(found->second.entryName.c_str());
    if (!buffer || buffer->GetSize() <= 0)
    {
        bank.Unlock();
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(buffer->GetData());
    out.assign(bytes, bytes + buffer->GetSize());
    bank.Unlock();
    return true;
}

// The terrain-material census.
//
// Separate from `census` because it answers a different question: not "can this
// world's models be read" but "does every authored ground surface reach a
// binding". A world whose terrain draws white has a number here, and it is zero.
// It runs the same TerrainFamily record the world loader runs, so a disagreement
// between this report and the frame is a renderer question, not a parsing one.
void PrintTerrainMaterialCensus(const Poseidon::Asset::Formats::World::Oprw25World& world, const AddonIndex& index,
                                int archives, const std::string& path, bool listStages)
{
    namespace Mat = Poseidon::Asset::Material;

    std::map<std::string, int> families;
    std::map<std::string, int> unknownFamilies;
    int declared = 0, opened = 0, parsed = 0;
    int withSatellite = 0, withMask = 0, withAnySurface = 0;
    int surfaceBindings = 0, normalBindings = 0, macroBindings = 0, tileNormals = 0;
    std::map<int, int> slotHistogram;
    std::map<int, int> surfaceCountHistogram;
    std::set<std::string> distinctSurfaces, distinctMacros, distinctMasks, distinctSatellites;
    int packedWithHole = 0;
    std::string stageSample;

    std::vector<uint8_t> bytes;
    for (const std::string& materialPath : world.materials)
    {
        if (materialPath.empty())
            continue;
        ++declared;
        if (!ReadIndexedAsset(index, materialPath, bytes))
            continue;
        ++opened;
        Mat::RvMaterialSource source;
        try
        {
            source = Mat::ParseArmaRapMaterial(bytes, materialPath);
        }
        catch (const std::exception&)
        {
            continue;
        }
        ++parsed;

        Mat::TerrainFamily family;
        if (!Mat::FindTerrainFamily(source.pixelShaderId, family))
        {
            ++unknownFamilies[source.pixelShaderId];
            continue;
        }
        ++families[source.pixelShaderId];

        const auto textureOf = [&](int stageIndex) -> std::string
        {
            for (const auto& stage : source.stages)
            {
                if (stage.index != stageIndex)
                    continue;
                if (stage.texture.raw.empty() || Mat::RvTextureRef::LooksProcedural(stage.texture.raw))
                    return std::string();
                return stage.texture.raw;
            }
            return std::string();
        };

        if (listStages && stageSample.empty())
        {
            stageSample = materialPath + "  [" + source.pixelShaderId + "]\n";
            for (const auto& stage : source.stages)
                stageSample += "    Stage" + std::to_string(stage.index) + "  texGen=" + std::to_string(stage.texGen) +
                               "  " + stage.texture.raw + "\n";
            for (const auto& texGen : source.texGens)
                stageSample += "    TexGen" + std::to_string(texGen.index) + "  uvSource=" + texGen.uvSource +
                               "  aside.x=" + std::to_string(texGen.uvTransform.aside[0]) + "\n";
        }

        const std::string satellite = textureOf(family.satelliteStage);
        if (!satellite.empty())
        {
            ++withSatellite;
            distinctSatellites.insert(CanonicalAssetPath(satellite));
        }
        const std::string mask = textureOf(family.maskStage);
        if (!mask.empty())
        {
            ++withMask;
            distinctMasks.insert(CanonicalAssetPath(mask));
        }
        if (family.tileNormalStage > 0 && !textureOf(family.tileNormalStage).empty())
            ++tileNormals;

        int bound = 0;
        const int groups = family.GroupCount();
        bool sawHole = false;
        for (int group = 0; group < groups; ++group)
        {
            const int slot = family.SlotOfGroup(group);
            if (slot != group)
                sawHole = true;
            if (slot < 0)
                continue;
            const int base = family.firstSurfaceStage + group * family.stageStride;
            const std::string colour = textureOf(base + family.colourOffset);
            if (colour.empty())
                continue;
            ++bound;
            ++surfaceBindings;
            ++slotHistogram[slot];
            distinctSurfaces.insert(CanonicalAssetPath(colour));
            if (!textureOf(base + family.normalOffset).empty())
                ++normalBindings;
            if (family.hasMacro)
            {
                const std::string macro = textureOf(base + family.macroOffset);
                if (!macro.empty())
                {
                    ++macroBindings;
                    distinctMacros.insert(CanonicalAssetPath(macro));
                }
            }
        }
        if (sawHole)
            ++packedWithHole;
        ++surfaceCountHistogram[bound];
        if (bound > 0)
            ++withAnySurface;
    }

    std::cout << "World:            " << path << "\n";
    std::cout << "Archives indexed: " << archives << " (" << index.size() << " entries)\n\n";
    std::cout << "-- terrain materials --\n";
    std::cout << "Declared:         " << declared << "\n";
    std::cout << "Opened:           " << opened << "\n";
    std::cout << "Parsed:           " << parsed << "\n";
    std::cout << "Families:         ";
    for (const auto& entry : families)
        std::cout << entry.first << "=" << entry.second << " ";
    std::cout << "\n";
    if (!unknownFamilies.empty())
    {
        std::cout << "Unrecognised:     ";
        for (const auto& entry : unknownFamilies)
            std::cout << entry.first << "=" << entry.second << " ";
        std::cout << "\n";
    }
    std::cout << "\n-- bindings --\n";
    std::cout << "Satellite:        " << withSatellite << " (" << distinctSatellites.size() << " distinct)\n";
    std::cout << "Selector mask:    " << withMask << " (" << distinctMasks.size() << " distinct)\n";
    std::cout << "Tile normal:      " << tileNormals << "\n";
    std::cout << "Surface colours:  " << surfaceBindings << " (" << distinctSurfaces.size() << " distinct)\n";
    std::cout << "Surface normals:  " << normalBindings << "\n";
    std::cout << "Surface macros:   " << macroBindings << " (" << distinctMacros.size() << " distinct)\n";
    std::cout << "Materials with at least one surface: " << withAnySurface << " of " << parsed << "\n";
    std::cout << "Packed groups whose slots are not 0..n-1: " << packedWithHole << "\n";
    std::cout << "\nSurfaces per material: ";
    for (const auto& entry : surfaceCountHistogram)
        std::cout << entry.first << "->" << entry.second << "  ";
    std::cout << "\nSlot usage:            ";
    for (const auto& entry : slotHistogram)
        std::cout << entry.first << "->" << entry.second << "  ";
    std::cout << "\n";
    if (!stageSample.empty())
        std::cout << "\n-- stage table of one material --\n" << stageSample;
}

void PrintModelCensus(const Poseidon::Asset::Formats::World::Oprw25World& world, const AddonIndex& index,
                      int archivesRead, bool asJson)
{
    std::vector<int> placementsPerModel(world.models.size(), 0);
    for (const auto& object : world.objects)
        ++placementsPerModel[static_cast<size_t>(object.modelIndex)];

    std::map<ModelOutcome, int> byModel;
    std::map<ModelOutcome, long long> byPlacement;
    std::map<uint32_t, int> revisions;
    std::vector<std::pair<std::string, ModelResult>> notStatic;

    for (size_t i = 0; i < world.models.size(); ++i)
    {
        const ModelResult result = ClassifyModel(world.models[i], index);
        ++byModel[result.outcome];
        byPlacement[result.outcome] += placementsPerModel[i];
        if (result.revision != 0)
            ++revisions[result.revision];
        if (result.outcome != ModelOutcome::Static)
            notStatic.emplace_back(world.models[i], result);
    }

    // Most-placed failures first: that is the order the work should be done in.
    std::sort(notStatic.begin(), notStatic.end(),
              [&](const auto& a, const auto& b)
              {
                  const auto count = [&](const std::string& name)
                  {
                      for (size_t i = 0; i < world.models.size(); ++i)
                          if (world.models[i] == name)
                              return placementsPerModel[i];
                      return 0;
                  };
                  return count(a.first) > count(b.first);
              });

    const auto modelsOf = [&](ModelOutcome o) { return byModel.count(o) ? byModel.at(o) : 0; };
    const auto placementsOf = [&](ModelOutcome o) { return byPlacement.count(o) ? byPlacement.at(o) : 0LL; };
    const ModelOutcome all[] = {ModelOutcome::Static,     ModelOutcome::ParseFailed, ModelOutcome::UnsupportedRevision,
                                ModelOutcome::MlodSource, ModelOutcome::NotP3D,      ModelOutcome::Unreadable,
                                ModelOutcome::Missing};

    if (asJson)
    {
        std::cout << "{\n  \"archives_indexed\": " << archivesRead << ",\n  \"index_entries\": " << index.size()
                  << ",\n  \"models\": {";
        bool first = true;
        for (const ModelOutcome outcome : all)
        {
            std::cout << (first ? "\n" : ",\n") << "    \"" << ModelOutcomeName(outcome)
                      << "\": {\"models\": " << modelsOf(outcome) << ", \"placements\": " << placementsOf(outcome)
                      << "}";
            first = false;
        }
        std::cout << "\n  },\n  \"odol_revisions\": {";
        first = true;
        for (const auto& [revision, count] : revisions)
        {
            std::cout << (first ? "\n" : ",\n") << "    \"" << revision << "\": " << count;
            first = false;
        }
        // Every failure, not the head of the list. The text report truncates so a
        // human can read it; the JSON exists precisely so the backlog can be worked
        // through, and a truncated backlog is not one.
        std::cout << "\n  },\n  \"failures\": [";
        first = true;
        for (const auto& [name, result] : notStatic)
        {
            int placements = 0;
            for (size_t i = 0; i < world.models.size(); ++i)
                if (world.models[i] == name)
                    placements = placementsPerModel[i];
            std::cout << (first ? "\n" : ",\n") << "    {\"model\": \"" << JsonEscape(name) << "\", \"outcome\": \""
                      << ModelOutcomeName(result.outcome) << "\", \"revision\": " << result.revision
                      << ", \"placements\": " << placements << ", \"detail\": \"" << JsonEscape(result.detail) << "\"}";
            first = false;
        }
        std::cout << (first ? "" : "\n") << "  ]\n}\n";
        return;
    }

    std::cout << "\n-- referenced models --\n";
    std::cout << "Archives indexed: " << archivesRead << " (" << index.size() << " entries)\n";
    std::cout << std::left << std::setw(22) << "outcome" << std::right << std::setw(8) << "models" << std::setw(12)
              << "placements" << "\n";
    std::cout << std::string(42, '-') << "\n";
    for (const ModelOutcome outcome : all)
    {
        if (modelsOf(outcome) == 0)
            continue;
        std::cout << std::left << std::setw(22) << ModelOutcomeName(outcome) << std::right << std::setw(8)
                  << modelsOf(outcome) << std::setw(12) << placementsOf(outcome) << "\n";
    }
    std::cout << std::left << std::setw(22) << "TOTAL" << std::right << std::setw(8) << world.models.size()
              << std::setw(12) << world.objects.size() << "\n";

    if (!revisions.empty())
    {
        std::cout << "\nODOL revisions seen: ";
        for (const auto& [revision, count] : revisions)
            std::cout << revision << " (" << count << " models) ";
        std::cout << "\n";
    }

    // The whole reason histogram, not just the head of the list: the point of the
    // census is to say which missing capability unlocks the most, and a truncated
    // listing cannot answer that.
    if (!notStatic.empty())
    {
        std::map<std::string, std::pair<int, long long>> reasons;
        for (const auto& [name, result] : notStatic)
        {
            const std::string reason = result.detail.empty() ? ModelOutcomeName(result.outcome) : result.detail;
            auto& tally = reasons[reason];
            ++tally.first;
            for (size_t i = 0; i < world.models.size(); ++i)
                if (world.models[i] == name)
                    tally.second += placementsPerModel[i];
        }
        std::cout << "\nBlocking reasons (all failures):\n";
        std::vector<std::pair<std::string, std::pair<int, long long>>> ordered(reasons.begin(), reasons.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        for (const auto& [reason, tally] : ordered)
            std::cout << "  " << std::setw(4) << tally.first << " models, " << std::setw(7) << tally.second
                      << " placements  " << reason << "\n";
    }

    if (!notStatic.empty())
    {
        std::cout << "\nNot yet static, most-placed first:\n";
        const size_t shown = notStatic.size() < 20 ? notStatic.size() : 20;
        for (size_t i = 0; i < shown; ++i)
        {
            std::cout << "  [" << ModelOutcomeName(notStatic[i].second.outcome) << "] " << notStatic[i].first;
            if (!notStatic[i].second.detail.empty())
                std::cout << "\n      " << notStatic[i].second.detail;
            std::cout << "\n";
        }
        if (notStatic.size() > shown)
            std::cout << "  ... and " << (notStatic.size() - shown) << " more\n";
    }
}

} // namespace

void TerrainCommand::Setup(CLI::App& app)
{
    auto* terrain = app.add_subcommand("terrain", "Inspect and analyze WRP terrain files");
    terrain->require_subcommand(1);
    {
        static std::string source, destination;
        auto* cmd = terrain->add_subcommand("export-rvw4", "Export legacy OPRW 2/3 to editable 4WVR (50 m cells)");
        cmd->add_option("input", source)->required()->check(CLI::ExistingFile);
        cmd->add_option("output", destination)->required();
        cmd->callback([]()
        {
            try
            {
                WrpReader world;
                if (!world.Load(source.c_str()) ||
                    (world.GetFormat() != WrpReader::OPRW_V2 && world.GetFormat() != WrpReader::OPRW_V3))
                    throw std::runtime_error("Expected legacy OPRW 2/3 world");
                const int cells = world.GetGridX()*world.GetGridZ();
                if (world.GetTextureCount() > 512 || world.GetTexIndices().Size() != cells*2)
                    throw std::runtime_error("Legacy texture table does not fit RVW4");
                std::ofstream out(destination, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("Cannot create output");
                auto write = [&](const auto& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
                auto fixed = [&](const char* value, size_t length)
                {
                    const std::string text(value);
                    if (text.size() >= length) throw std::runtime_error("Path exceeds RVW4 field");
                    out.write(text.data(), text.size());
                    const std::string padding(length-text.size(), '\0');
                    out.write(padding.data(), padding.size());
                };
                out.write("4WVR",4);
                const int32_t width=world.GetGridX(), height=world.GetGridZ();
                write(width); write(height);
                for (int i=0;i<cells;++i)
                {
                    const float h=world.GetHeightmapData()[i]/0.045f;
                    if (!std::isfinite(h) || h < -32768 || h > 32767)
                        throw std::runtime_error("Height outside RVW4 range");
                    const int16_t quantized=static_cast<int16_t>(std::lround(h));
                    write(quantized);
                }
                out.write(reinterpret_cast<const char*>(world.GetTexIndices().Data()),cells*2);
                for (int i=0;i<512;++i)
                    fixed(i<world.GetTextureCount() ? static_cast<const char*>(world.GetTextureName(i)) : "",32);
                for (int i=0;i<world.GetObjectCount();++i)
                {
                    const auto& object=world.GetObject(i);
                    if (!object.hasMatrix) throw std::runtime_error("Object lacks transform");
                    const auto matrix=ConvertToP(object.transform);
                    static_assert(sizeof(matrix)==48);
                    write(matrix);
                    const int32_t id=i;
                    write(id);
                    fixed(object.name,76);
                }
                out.close();
                if (!out) throw std::runtime_error("Output write failed");
                // Preserve authored terrain semantics and exact float heights for fusion.
                std::ofstream meta(destination+".meta",std::ios::binary | std::ios::trunc);
                meta.write("FMD1",4);
                meta.write(reinterpret_cast<const char*>(world.GetGeography().Data()),cells*4);
                meta.write(reinterpret_cast<const char*>(world.GetSoundMap().Data()),cells);
                meta.write(reinterpret_cast<const char*>(world.GetRandom().Data()),cells*4);
                meta.write(reinterpret_cast<const char*>(world.GetHeightmapData()),cells*4);
                const int32_t peaks=world.GetMountains().Size();
                meta.write(reinterpret_cast<const char*>(&peaks),4);
                for (int i=0;i<peaks;++i)
                {
                    const auto& peak=world.GetMountains()[i];
                    const float xyz[]={peak.X(),peak.Y(),peak.Z()};
                    meta.write(reinterpret_cast<const char*>(xyz),12);
                }
                meta.close();
                if (!meta) throw std::runtime_error("Metadata write failed");
                std::cout << "Exported " << destination << ": " << width << "x" << height
                          << ", " << world.GetObjectCount() << " placements\n";
            }
            catch (const std::exception& e) { std::cerr << e.what() << '\n'; std::exit(1); }
        });
    }
    {
        static std::string inputFile;
        auto* cmd = terrain->add_subcommand("inspect", "Show terrain file information");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->callback(
            []()
            {
                auto info = Poseidon::InspectTerrain(inputFile);
                if (!info.valid)
                {
                    std::cerr << "Error: Failed to load terrain: " << inputFile << "\n";
                    std::exit(1);
                }

                std::cout << "Format:      " << info.formatName << "\n";
                std::cout << "Grid:        " << info.gridX << " x " << info.gridZ << "\n";
                std::cout << "Terrain:     " << info.terrainX << " x " << info.terrainZ << "\n";
                std::cout << "Heightmap:   " << info.heightmapSize << " cells\n";
                std::cout << "Elevation:   " << std::fixed << std::setprecision(1) << info.minHeight << " - "
                          << info.maxHeight << " m\n";
                std::cout << "Textures:    " << info.textureCount << "\n";
                std::cout << "Objects:     " << info.objectCount << "\n";
                if (info.objectNameCount > 0)
                    std::cout << "Obj classes: " << info.objectNameCount << "\n";

                std::exit(0);
            });
    }
    {
        static std::string inputFile;
        auto* cmd = terrain->add_subcommand("textures", "List terrain texture assignments");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->callback(
            []()
            {
                WrpReader reader;
                if (!reader.Load(inputFile.c_str()))
                {
                    std::cerr << "Error: " << (reader.GetError() ? reader.GetError() : "Unknown error") << "\n";
                    std::exit(1);
                }

                if (reader.GetTextureCount() == 0)
                {
                    std::cout << "No textures.\n";
                    std::exit(0);
                }

                std::cout << std::left << std::setw(6) << "Index" << "Texture\n";
                std::cout << std::string(40, '-') << "\n";
                for (int i = 0; i < reader.GetTextureCount(); i++)
                {
                    std::cout << std::left << std::setw(6) << i << reader.GetTextureName(i).Data() << "\n";
                }

                std::exit(0);
            });
    }
    {
        static std::string inputFile;
        static std::vector<double> nearArg;
        static std::string modelFilter;
        static bool withBasis = false;
        auto* cmd = terrain->add_subcommand("objects", "List placed objects with positions");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("--near", nearArg, "Only objects within R metres of map point (X,Z): --near X Z R")
            ->expected(3);
        cmd->add_option("--model", modelFilter,
                        "Only objects whose model path contains this substring (case-insensitive)");
        // A placement's transform is the only per-instance orientation a world file
        // carries, so anything that wants to know which way a placed mesh points --
        // a river segment's downstream axis, for one -- has to read the basis, not
        // just the position. Revision-25-family worlds only.
        cmd->add_flag("--basis", withBasis, "Also print the aside/up/dir rows of the placement transform");
        cmd->callback(
            []()
            {
                // Later-generation worlds carry their anonymous props in the
                // OPRW-25 object table, not WrpReader's legacy OPRW 2/3 table.
                QIFStream file;
                file.open(inputFile.c_str());
                if (!file.fail())
                {
                    Asset::Formats::BinaryReader probe(file);
                    if (Asset::Formats::World::PeekOprwModernRevision(probe) != 0)
                    {
                        Asset::Formats::World::Oprw25World world;
                        try
                        {
                            world = Asset::Formats::World::ReadOprwModern(probe);
                        }
                        catch (const std::exception& e)
                        {
                            std::cerr << "Error: " << e.what() << "\n";
                            std::exit(1);
                        }

                        const bool filter = nearArg.size() == 3;
                        const double nx = filter ? nearArg[0] : 0.0;
                        const double nz = filter ? nearArg[1] : 0.0;
                        const double r2 = filter ? nearArg[2] * nearArg[2] : -1.0;
                        std::string needle = modelFilter;
                        std::transform(needle.begin(), needle.end(), needle.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        std::cout << std::left << std::setw(8) << "ID" << std::setw(64) << "Model" << std::setw(12)
                                  << "X" << std::setw(12) << "Y" << std::setw(12) << "Z";
                        if (withBasis)
                            std::cout << std::setw(24) << "aside" << std::setw(24) << "up" << std::setw(24) << "dir";
                        std::cout << "\n";
                        std::cout << std::string(withBasis ? 180 : 108, '-') << "\n";
                        int shown = 0;
                        for (const auto& obj : world.objects)
                        {
                            const auto& position = obj.transform.rows[3];
                            if (filter)
                            {
                                const double dx = position.x - nx;
                                const double dz = position.z - nz;
                                if (dx * dx + dz * dz > r2)
                                    continue;
                            }
                            const std::string& model = world.models[static_cast<size_t>(obj.modelIndex)];
                            if (!needle.empty())
                            {
                                std::string lowered = model;
                                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                if (lowered.find(needle) == std::string::npos)
                                    continue;
                            }
                            std::cout << std::left << std::setw(8) << obj.objectId << std::setw(64) << model
                                      << std::fixed << std::setprecision(1) << std::setw(12) << position.x
                                      << std::setw(12) << position.y << std::setw(12) << position.z;
                            if (withBasis)
                            {
                                auto row = [](const auto& v)
                                {
                                    char buffer[64];
                                    std::snprintf(buffer, sizeof(buffer), "%.4f,%.4f,%.4f", v.x, v.y, v.z);
                                    return std::string(buffer);
                                };
                                std::cout << std::setprecision(4) << std::setw(24) << row(obj.transform.rows[0])
                                          << std::setw(24) << row(obj.transform.rows[1]) << std::setw(24)
                                          << row(obj.transform.rows[2]);
                            }
                            std::cout << "\n";
                            shown++;
                        }
                        if (filter)
                            std::cout << "\n"
                                      << shown << " object(s) within " << nearArg[2] << " m of (" << nx << ", " << nz
                                      << ").\n";
                        else if (!needle.empty())
                            std::cout << "\n" << shown << " object(s) matching '" << modelFilter << "'.\n";
                        std::exit(0);
                    }
                }

                WrpReader reader;
                if (!reader.Load(inputFile.c_str()))
                {
                    std::cerr << "Error: " << (reader.GetError() ? reader.GetError() : "Unknown error") << "\n";
                    std::exit(1);
                }

                if (reader.GetObjectCount() == 0)
                {
                    std::cout << "No objects.\n";
                    std::exit(0);
                }

                // --near X Z R: horizontal (X,Z) distance filter — Y is height. r2 < 0 disables.
                const bool filter = nearArg.size() == 3;
                const double nx = filter ? nearArg[0] : 0.0;
                const double nz = filter ? nearArg[1] : 0.0;
                const double r2 = filter ? nearArg[2] * nearArg[2] : -1.0;

                std::cout << std::left << std::setw(8) << "ID" << std::setw(40) << "Class" << std::setw(12) << "X"
                          << std::setw(12) << "Y" << std::setw(12) << "Z"
                          << "\n";
                std::cout << std::string(84, '-') << "\n";

                int shown = 0;
                for (int i = 0; i < reader.GetObjectCount(); i++)
                {
                    const auto& obj = reader.GetObject(i);
                    if (filter)
                    {
                        const double dx = obj.position.X() - nx;
                        const double dz = obj.position.Z() - nz;
                        if (dx * dx + dz * dz > r2)
                            continue;
                    }
                    std::cout << std::left << std::setw(8) << obj.id << std::setw(40) << obj.name.Data() << std::fixed
                              << std::setprecision(1) << std::setw(12) << obj.position.X() << std::setw(12)
                              << obj.position.Y() << std::setw(12) << obj.position.Z() << "\n";
                    shown++;
                }

                if (filter)
                    std::cout << "\n"
                              << shown << " object(s) within " << nearArg[2] << " m of (" << nx << ", " << nz << ").\n";

                std::exit(0);
            });
    }
    {
        // A placement's Y and the heightfield under it are stored in two unrelated
        // parts of the file, so "does this quad sit on the ground or in a hollow?"
        // is not answerable by reading either alone -- and answering it for one
        // pond says nothing about the other 669. This probes both together, either
        // at a named point or across every placement whose model matches a
        // substring, and reports the *distribution* of (placement Y - terrain Y).
        static std::string inputFile;
        static std::vector<double> atArg;
        static std::string modelFilter;
        static int histogramBuckets = 12;
        auto* cmd = terrain->add_subcommand("probe", "Sample terrain height and geography under a point or a model");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("--at", atArg, "Probe one world point: --at X Z")->expected(2);
        cmd->add_option("--model", modelFilter,
                        "Probe under every placement whose model path contains this substring "
                        "(case-insensitive) and summarise the height deltas");
        cmd->add_option("--buckets", histogramBuckets, "Histogram bucket count for --model (default 12)");
        cmd->callback(
            []()
            {
                QIFStream file;
                file.open(inputFile.c_str());
                if (file.fail())
                {
                    std::cerr << "Error: cannot open " << inputFile << "\n";
                    std::exit(1);
                }
                Asset::Formats::BinaryReader probeReader(file);
                if (Asset::Formats::World::PeekOprwModernRevision(probeReader) == 0)
                {
                    std::cerr << "Error: terrain probe requires a revision-18/20/24/25 world; " << inputFile
                              << " is not one.\n";
                    std::exit(1);
                }

                Asset::Formats::World::Oprw25World world;
                try
                {
                    world = Asset::Formats::World::ReadOprwModern(probeReader);
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Error: " << e.what() << "\n";
                    std::exit(1);
                }

                const auto& header = world.header;
                const float terrainCell = header.TerrainCellSize();
                const float landCell = header.landCellSize;
                if (terrainCell <= 0.0f || landCell <= 0.0f)
                {
                    std::cerr << "Error: world declares a non-positive cell size.\n";
                    std::exit(1);
                }

                // Bilinear over the four surrounding terrain samples. Elevation is
                // stored per grid *vertex*, so cell (i,j) spans [i,i+1] and a world
                // point maps to a fractional index, not a cell index.
                auto clampIndex = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
                auto heightAt = [&](double wx, double wz)
                {
                    const double fx = wx / terrainCell;
                    const double fz = wz / terrainCell;
                    const int x0 = clampIndex(static_cast<int>(std::floor(fx)), header.terrainRangeX - 1);
                    const int z0 = clampIndex(static_cast<int>(std::floor(fz)), header.terrainRangeY - 1);
                    const int x1 = clampIndex(x0 + 1, header.terrainRangeX - 1);
                    const int z1 = clampIndex(z0 + 1, header.terrainRangeY - 1);
                    const double tx = fx - std::floor(fx);
                    const double tz = fz - std::floor(fz);
                    const double h00 = world.ElevationAt(x0, z0);
                    const double h10 = world.ElevationAt(x1, z0);
                    const double h01 = world.ElevationAt(x0, z1);
                    const double h11 = world.ElevationAt(x1, z1);
                    return (h00 * (1.0 - tx) + h10 * tx) * (1.0 - tz) + (h01 * (1.0 - tx) + h11 * tx) * tz;
                };
                auto geographyAt = [&](double wx, double wz)
                {
                    Asset::Formats::World::Oprw25Geography g;
                    if (world.geography.empty())
                        return g;
                    const int lx = clampIndex(static_cast<int>(std::floor(wx / landCell)), header.landRangeX - 1);
                    const int lz = clampIndex(static_cast<int>(std::floor(wz / landCell)), header.landRangeY - 1);
                    g.bits = world.geography[static_cast<size_t>(lz) * header.landRangeX + lx];
                    return g;
                };

                std::cout << "World:       " << inputFile << "\n";
                std::cout << "Terrain:     " << header.terrainRangeX << " x " << header.terrainRangeY << " @ "
                          << std::fixed << std::setprecision(2) << terrainCell << " m\n";
                std::cout << "Land:        " << header.landRangeX << " x " << header.landRangeY << " @ " << landCell
                          << " m\n";

                if (atArg.size() == 2)
                {
                    const double wx = atArg[0];
                    const double wz = atArg[1];
                    const int x0 = clampIndex(static_cast<int>(std::floor(wx / terrainCell)), header.terrainRangeX - 1);
                    const int z0 = clampIndex(static_cast<int>(std::floor(wz / terrainCell)), header.terrainRangeY - 1);
                    const auto g = geographyAt(wx, wz);
                    std::cout << "\nPoint (" << std::setprecision(1) << wx << ", " << wz << "):\n";
                    std::cout << "  height        " << std::setprecision(3) << heightAt(wx, wz) << " m\n";
                    std::cout << "  cell          [" << x0 << ", " << z0 << "] corners "
                              << world.ElevationAt(x0, z0) << " "
                              << world.ElevationAt(clampIndex(x0 + 1, header.terrainRangeX - 1), z0) << " "
                              << world.ElevationAt(x0, clampIndex(z0 + 1, header.terrainRangeY - 1)) << " "
                              << world.ElevationAt(clampIndex(x0 + 1, header.terrainRangeX - 1),
                                                   clampIndex(z0 + 1, header.terrainRangeY - 1))
                              << "\n";
                    std::cout << "  geography     waterDepth " << static_cast<int>(g.MinWaterDepth()) << ".."
                              << static_cast<int>(g.MaxWaterDepth()) << "  gradient "
                              << static_cast<int>(g.Gradient()) << "  forest " << (g.Forest() ? 1 : 0) << "  road "
                              << (g.Road() ? 1 : 0) << "  objects " << (g.SomeObjects() ? 1 : 0) << "\n";
                }

                if (!modelFilter.empty())
                {
                    std::string needle = modelFilter;
                    std::transform(needle.begin(), needle.end(), needle.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    std::vector<double> deltas;
                    std::array<int, 4> minDepthHistogram{};
                    std::array<int, 4> maxDepthHistogram{};
                    for (const auto& obj : world.objects)
                    {
                        std::string lowered = world.models[static_cast<size_t>(obj.modelIndex)];
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (lowered.find(needle) == std::string::npos)
                            continue;
                        const auto& position = obj.transform.rows[3];
                        deltas.push_back(position.y - heightAt(position.x, position.z));
                        const auto g = geographyAt(position.x, position.z);
                        minDepthHistogram[g.MinWaterDepth()]++;
                        maxDepthHistogram[g.MaxWaterDepth()]++;
                    }

                    std::cout << "\nPlacements matching '" << modelFilter << "': " << deltas.size() << "\n";
                    if (deltas.empty())
                        std::exit(0);

                    std::sort(deltas.begin(), deltas.end());
                    double sum = 0.0;
                    for (double d : deltas)
                        sum += d;
                    auto quantile = [&](double q)
                    { return deltas[static_cast<size_t>(q * static_cast<double>(deltas.size() - 1))]; };
                    std::cout << std::setprecision(3);
                    std::cout << "  placementY - terrainY   min " << deltas.front() << "  p05 " << quantile(0.05)
                              << "  median " << quantile(0.5) << "  p95 " << quantile(0.95) << "  max "
                              << deltas.back() << "  mean " << (sum / static_cast<double>(deltas.size())) << "\n";
                    // The sign question the whole defect turns on: a quad sunk into a
                    // hollow is negative, a quad lying on the ground is ~0.
                    int within10cm = 0;
                    for (double d : deltas)
                        if (d > -0.10 && d < 0.10)
                            within10cm++;
                    std::cout << "  |delta| < 0.10 m        " << within10cm << " / " << deltas.size() << " ("
                              << std::setprecision(1) << (100.0 * within10cm / static_cast<double>(deltas.size()))
                              << "%)\n";
                    std::cout << std::setprecision(3);

                    const int buckets = histogramBuckets < 2 ? 2 : histogramBuckets;
                    const double lo = deltas.front();
                    const double hi = deltas.back();
                    const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;
                    std::vector<int> counts(static_cast<size_t>(buckets), 0);
                    for (double d : deltas)
                    {
                        int b = static_cast<int>((d - lo) / span * buckets);
                        if (b >= buckets)
                            b = buckets - 1;
                        counts[static_cast<size_t>(b)]++;
                    }
                    std::cout << "  histogram:\n";
                    for (int b = 0; b < buckets; ++b)
                    {
                        std::cout << "    [" << std::setw(8) << (lo + span * b / buckets) << ", " << std::setw(8)
                                  << (lo + span * (b + 1) / buckets) << ")  " << std::setw(6)
                                  << counts[static_cast<size_t>(b)] << "\n";
                    }
                    std::cout << "  geography minWaterDepth under them: ";
                    for (int i = 0; i < 4; ++i)
                        std::cout << i << "=" << minDepthHistogram[static_cast<size_t>(i)] << " ";
                    std::cout << "\n  geography maxWaterDepth under them: ";
                    for (int i = 0; i < 4; ++i)
                        std::cout << i << "=" << maxDepthHistogram[static_cast<size_t>(i)] << " ";
                    std::cout << "\n";
                }

                std::exit(0);
            });
    }
    {
        static std::string inputFile;
        static std::string outputFile;
        static bool renderObjects = false;
        auto* cmd = terrain->add_subcommand("render", "Render heightmap to PNG image");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("-o,--output", outputFile, "Output PNG file path")->required();
        cmd->add_flag("--objects", renderObjects, "Overlay object placement density (revision 25 only)");
        cmd->callback(
            []()
            {
                // Revision 25 gets its own renderer; the legacy preview's
                // relative height normalisation misplaces its coastline.
                {
                    QIFStream probe;
                    probe.open(inputFile.c_str());
                    if (!probe.fail())
                    {
                        Asset::Formats::BinaryReader reader(probe);
                        if (Asset::Formats::World::PeekOprwModernRevision(reader) != 0)
                        {
                            Asset::Formats::World::Oprw25World world;
                            try
                            {
                                world = Asset::Formats::World::ReadOprwModern(reader);
                            }
                            catch (const std::exception& e)
                            {
                                std::cerr << "Error: " << e.what() << "\n";
                                std::exit(1);
                            }
                            const auto map = RenderOprw25Map(world, renderObjects);
                            if (!map.saveToFile(outputFile))
                            {
                                std::cerr << "Error: Failed to write output\n";
                                std::exit(1);
                            }
                            std::cout << "Rendered: " << outputFile << " (" << map.width << "x" << map.height << ")\n";
                            std::cout << "Elevation: " << std::fixed << std::setprecision(1)
                                      << *std::min_element(world.elevation.begin(), world.elevation.end()) << " - "
                                      << *std::max_element(world.elevation.begin(), world.elevation.end()) << " m over "
                                      << world.header.WorldExtent() << " m\n";
                            if (renderObjects)
                                std::cout << "Placements: " << world.objects.size() << "\n";
                            std::exit(0);
                        }
                    }
                }

                auto preview = Poseidon::PreviewTerrain(inputFile);
                if (!preview.valid())
                {
                    std::cerr << "Error: Failed to load terrain: " << inputFile << "\n";
                    std::exit(1);
                }

                if (!preview.saveToFile(outputFile))
                {
                    std::cerr << "Error: Failed to write output" << "\n";
                    std::exit(1);
                }

                auto info = Poseidon::InspectTerrain(inputFile);
                std::cout << "Rendered: " << outputFile << " (" << preview.width << "x" << preview.height << ")"
                          << "\n";
                if (info.valid)
                    std::cout << "Elevation: " << std::fixed << std::setprecision(1) << info.minHeight << " - "
                              << info.maxHeight << " m" << "\n";
                std::exit(0);
            });
    }
    {
        static std::string inputFile;
        static std::string screenshotPath;
        auto* cmd = terrain->add_subcommand("show", "Display heightmap in an SDL window");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("--screenshot", screenshotPath, "Save screenshot to file and exit");
        cmd->callback(
            []()
            {
                auto preview = Poseidon::PreviewTerrain(inputFile);
                if (!preview.valid())
                {
                    std::cerr << "Error: Failed to load terrain: " << inputFile << "\n";
                    std::exit(1);
                }

                int w = preview.width;
                int h = preview.height;

                if (!screenshotPath.empty())
                {
                    if (!preview.saveToFile(screenshotPath))
                    {
                        std::cerr << "Error: Failed to write screenshot" << "\n";
                        std::exit(1);
                    }
                    std::cout << "Screenshot: " << screenshotPath << " (" << w << "x" << h << ")" << "\n";
                    std::exit(0);
                }

                auto info = Poseidon::InspectTerrain(inputFile);
                char title[256];
                std::snprintf(title, sizeof(title), "Terrain - %s (%dx%d, %.1f-%.1fm)", inputFile.c_str(), w, h,
                              info.minHeight, info.maxHeight);
                DisplayWindowRGBA(title, w, h, preview.data.data());
                std::exit(0);
            });
    }
    {
        // The compatibility census. Its point is denominators: "the world loaded"
        // is not a measurement, whereas "163,953 placements over 350 distinct
        // models, of which N resolve" is one that can go up or down between runs.
        static std::string inputFile;
        static bool asJson = false;
        static std::string addonRoot;
        auto* cmd = terrain->add_subcommand("census", "Report compatibility coverage for a later-generation world");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_flag("--json", asJson, "Emit the census as JSON");
        cmd->add_option("--addons", addonRoot,
                        "Game install to resolve referenced models against (searched recursively for PBOs)")
            ->check(CLI::ExistingDirectory);
        cmd->callback(
            []()
            {
                QIFStream file;
                file.open(inputFile.c_str());
                if (file.fail())
                {
                    std::cerr << "Error: cannot open " << inputFile << "\n";
                    std::exit(1);
                }

                // The census covers every later-generation container the engine
                // itself loads, which is now Armed Assault's revisions 18 and 20 as
                // well as 24 and 25. A census that only accepted 25 could not
                // measure the generation whose coverage is least known.
                Asset::Formats::BinaryReader reader(file);
                if (Asset::Formats::World::PeekOprwModernRevision(reader) == 0)
                {
                    std::cerr << "Error: " << inputFile << " is not an OPRW revision 18, 20, 24 or 25 world.\n"
                              << "The census covers the later-generation container only; use "
                                 "'terrain inspect' for RVW and OPRW 2/3.\n";
                    std::exit(1);
                }

                Asset::Formats::World::Oprw25World world;
                try
                {
                    world = Asset::Formats::World::ReadOprwModern(reader);
                }
                catch (const std::exception& e)
                {
                    // A parse failure is the census result, not a tool crash: it is
                    // the finding the next piece of work starts from.
                    if (asJson)
                        std::cout << "{\"world_parse\":\"failed\",\"error\":\"" << e.what() << "\"}\n";
                    else
                        std::cerr << "world_parse: FAILED -- " << e.what() << "\n";
                    std::exit(1);
                }

                PrintWorldCensus(world, inputFile, asJson);
                if (!addonRoot.empty())
                {
                    int archives = 0;
                    const auto index = BuildAddonIndex(addonRoot, archives);
                    PrintModelCensus(world, index, archives, asJson);
                }
                std::exit(0);
            });
    }
    {
        // The terrain-material census. Separate from `census` because it answers a
        // different question: not "can the world's models be read" but "does every
        // authored ground surface reach a binding". A world whose terrain draws
        // white has a number here, and it is 0.
        static std::string inputFile;
        static std::string addonRoot;
        static bool listStages = false;
        auto* cmd = terrain->add_subcommand("materials", "Report terrain-material coverage for a world");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("--addons", addonRoot, "Game install to resolve the terrain RVMATs against")
            ->required()
            ->check(CLI::ExistingDirectory);
        cmd->add_flag("--stages", listStages, "Print the stage table of the first material of each family");
        cmd->callback(
            []()
            {
                QIFStream file;
                file.open(inputFile.c_str());
                if (file.fail())
                {
                    std::cerr << "Error: cannot open " << inputFile << "\n";
                    std::exit(1);
                }
                Asset::Formats::BinaryReader reader(file);
                if (Asset::Formats::World::PeekOprwModernRevision(reader) == 0)
                {
                    std::cerr << "Error: " << inputFile << " is not a later-generation OPRW world.\n";
                    std::exit(1);
                }
                Asset::Formats::World::Oprw25World world;
                try
                {
                    world = Asset::Formats::World::ReadOprwModern(reader);
                }
                catch (const std::exception& e)
                {
                    std::cerr << "world_parse: FAILED -- " << e.what() << "\n";
                    std::exit(1);
                }
                int archives = 0;
                const auto index = BuildAddonIndex(addonRoot, archives);
                PrintTerrainMaterialCensus(world, index, archives, inputFile, listStages);
                std::exit(0);
            });
    }
}

} // namespace PoseidonTools
