#pragma once

// Reskins: the same island in another climate.  Every ground texture the island uses
// is recoloured and written out as a new texture in an addon of its own, the island's
// texture table is pointed at them, and - if wanted - trees and bushes are thinned out.
// The addon folder is packed into a .pbo the game loads from its AddOns folder; with a
// world config in it the reskin shows up in the game as an island of its own.
//
// The transitions between textures stay right, because each transition texture is
// recoloured the same way as the plain ones it joins.

#include "GameData.hpp"
#include "Island.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace island
{
struct ReskinStyle
{
    const char* name;        // "Snow"
    const char* suffix;      // for the addon's name: "snow"
    const char* description;
    float removeTrees;       // suggested share of the trees to take away, 0..1
    float removeBushes;
};

const std::vector<ReskinStyle>& ReskinStyles();

// Recolours one texture's pixels (RGBA, 8 bits each) in place.  strength 0..1.
void ReskinPixels(int style, float strength, uint8_t* rgba, size_t count);

struct ReskinOptions
{
    int style = 0;
    float strength = 1.0f;
    std::string addon;        // name of the addon and of the new island: letters, digits, _ ; at most 12
    std::string outFolder;    // the addon folder and the .pbo are made in here
    float removeTrees = 0, removeBushes = 0;
    unsigned seed = 1;
    bool worldConfig = true;  // the reskin as an island of its own (config.cpp and the .wrp inside the addon)
    std::string title;        // its name in the game's lists
    std::string baseWorld;    // world class its settings come from: Eden, Abel, Cain, Noe, Intro ...
    bool pack = true;         // make <addon>.pbo
    bool install = false;     // copy the .pbo into the game folder's AddOns
};

struct ReskinReport
{
    int textures = 0, failed = 0, objectsRemoved = 0;
    std::string folder, pbo, wrp, installed;
};

// The island is changed (one undo step) and, with worldConfig, saved inside the addon.
bool ReskinIsland(Island& is, GameData& game, const ReskinOptions& opt, ReskinReport& report, std::string& error);

// Saves the island and packs its addon folder again (after more editing of a reskin).
// False when the island is not inside an addon folder made by ReskinIsland.
bool IsReskinAddon(const Island& is);
bool RepackAddon(Island& is, std::string& pbo, std::string& error);

// the world class a stock island file belongs to ("eden.wrp" -> "Eden"), "Intro" for others
std::string GuessBaseWorld(const std::string& islandFile);
} // namespace island
