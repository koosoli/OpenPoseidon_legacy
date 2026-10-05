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
struct LocalMapsCatalog
{
    std::vector<LocalMap> maps;
    std::vector<std::string> diagnostics;
};
// Metadata only; does not mount foreign configuration or copy game assets.
LocalMapsCatalog ScanLocalMaps(const std::string& extraFolder = "");
LocalMapsCatalog ScanLocalMapsFolder(const std::string& folder);
std::vector<std::string> ParseSteamLibraryPaths(const std::string& vdf);
bool LocalMapRevisionSupported(int revision);
} // namespace Poseidon
