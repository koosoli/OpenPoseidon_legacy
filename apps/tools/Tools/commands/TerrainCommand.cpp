#include "TerrainCommand.hpp"
#include "../SDLPreview.hpp"
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Asset/Probes/AssetPreview.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <algorithm>
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
        const auto model = Asset::Formats::P3D::ReadOdol73StaticModel(reader, size);
        (void)model;
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
        std::cout << "\n  }\n}\n";
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
        auto* cmd = terrain->add_subcommand("objects", "List placed objects with positions");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("--near", nearArg, "Only objects within R metres of map point (X,Z): --near X Z R")
            ->expected(3);
        cmd->callback(
            []()
            {
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
                        if (Asset::Formats::World::IsOprw25(reader))
                        {
                            Asset::Formats::World::Oprw25World world;
                            try
                            {
                                world = Asset::Formats::World::ReadOprw25(reader);
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

                Asset::Formats::BinaryReader reader(file);
                if (!Asset::Formats::World::IsOprw25(reader))
                {
                    std::cerr << "Error: " << inputFile << " is not an OPRW revision 25 world.\n"
                              << "The census covers the later-generation container only; use "
                                 "'terrain inspect' for RVW and OPRW 2/3.\n";
                    std::exit(1);
                }

                Asset::Formats::World::Oprw25World world;
                try
                {
                    world = Asset::Formats::World::ReadOprw25(reader);
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
}

} // namespace PoseidonTools
