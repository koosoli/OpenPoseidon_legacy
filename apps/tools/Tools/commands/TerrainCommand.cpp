#include "TerrainCommand.hpp"
#include "../SDLPreview.hpp"
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Asset/Probes/AssetPreview.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <algorithm>
#include <exception>
#include <set>
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
        auto* cmd = terrain->add_subcommand("render", "Render heightmap to PNG image");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_option("-o,--output", outputFile, "Output PNG file path")->required();
        cmd->callback(
            []()
            {
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
        auto* cmd = terrain->add_subcommand("census", "Report compatibility coverage for a later-generation world");
        cmd->add_option("input", inputFile, "WRP terrain file")->required()->check(CLI::ExistingFile);
        cmd->add_flag("--json", asJson, "Emit the census as JSON");
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
                std::exit(0);
            });
    }
}

} // namespace PoseidonTools
