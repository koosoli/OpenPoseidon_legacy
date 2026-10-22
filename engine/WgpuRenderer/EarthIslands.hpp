#pragma once
// Whole world: OFP islands placed into the open ocean (scripts/Prepare-EarthIsland.py writes them into the
// local Earth cache from the player's own installation). Their heights are blended into the streamed
// elevation and their ground image is laid over the streamed imagery; nothing else changes.
#include <filesystem>
#include <string>
#include <vector>

namespace Poseidon::EarthIslands
{
struct Island
{
    std::string name;
    double latitude = 0, longitude = 0; // centre of the island's grid
    double cellSize = 50;
    int cells = 0;
    std::vector<float> heights; // cells x cells vertex heights, row 0 = south (metres above sea level)
    std::filesystem::path image; // JPEG, row 0 = north
    double south = 0, west = 0, north = 0, east = 0; // the image's geographic bounds
};

// <POSEIDON_EARTH_CACHE>/islands/*/island.json, read once. Invalid entries are skipped with a log line.
const std::vector<Island>& All();

// `dem` with any island's heights blended in at this place. The island's own grid replaces the source
// elevation inside it; its outer cells fade into the real sea floor, so there is no cliff at its edge.
float Height(double latitude, double longitude, float dem);
} // namespace Poseidon::EarthIslands
