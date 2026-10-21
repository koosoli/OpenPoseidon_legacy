// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace Poseidon
{
struct LocalMap
{
    std::string game, name, worldPath, archiveRoot;
    std::vector<std::string> archiveRoots;
    int revision = 0;
    bool supported = false;
    bool enfusion = false;
};
// A mission folder found inside an installed game's archives (a `mission.sqm` entry).
struct LocalMission
{
    std::string game;        // "Arma 1", ...
    std::string name;        // the mission folder, e.g. "m09convoyattack.sara"
    std::string collection;  // campaign or archive it ships in, e.g. "ca"
    std::string world;       // the folder's extension: the terrain it is set on ("sara")
    std::string virtualPath; // archive prefix + folder, without the trailing mission.sqm
    std::string installRoot; // the game's install folder (what --arma1 mounts)
    std::string reason;      // why it is not playable; empty when supported
    bool supported = false;
};
struct LocalMapsCatalog
{
    std::vector<LocalMap> maps;
    std::vector<LocalMission> missions;
    std::vector<std::string> diagnostics;
};
// Metadata only; does not mount foreign configuration or copy game assets.
LocalMapsCatalog ScanLocalMaps(const std::string& extraFolder = "");
LocalMapsCatalog ScanLocalMapsFolder(const std::string& folder);
// "Arma 1", "Arma 2", ... from the executable in `folder`; `fallback` when none matches.
std::string DetectLocalGame(const std::string& folder, const std::string& fallback);
std::vector<std::string> ParseSteamLibraryPaths(const std::string& vdf);
bool LocalMapRevisionSupported(int revision);
// Whether a mission of `game` set on `world` (the mission folder's extension) can be played: Arma 1
// missions run through the --arma1 compatibility profile on the terrains it registers. Otherwise
// `reason` says why not.
bool LocalMissionSupported(const std::string& game, const std::string& world, std::string& reason);
// The browser group a mission belongs to when it is part of a campaign ("Campaign: Harvest Red"),
// from its virtual path: Arma 1's campaigns\<name>\missions\..., Arma 2/3's
// <archive>\campaign[_n]\missions|cutscenes\.... Empty for a mission outside any campaign.
std::string LocalMissionCampaign(const std::string& virtualPath);
} // namespace Poseidon
