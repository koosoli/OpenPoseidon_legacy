// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// GPL-3.0-or-later with this project's Section 7 terms; see LICENSE.
#include "CombatAssistSettings.hpp"
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace Poseidon
{
namespace
{
struct Settings
{
    bool callouts = true;
    bool bearing = true;
    bool grenade = true;
};
Settings settings;
bool loaded = false;

FILE* Open(const char* mode)
{
    const auto& dir = Foundation::GamePaths::Instance().UserDir();
    if (dir.empty()) return nullptr;
    std::filesystem::path path(dir);
    std::error_code ec;
    if (mode[0] == 'w')
    {
        std::filesystem::create_directories(path, ec);
        if (ec) return nullptr;
    }
    return fopen((path / "combat_assists.cfg").string().c_str(), mode);
}

void Load()
{
    if (loaded) return;
    loaded = true;
    FILE* file = Open("r");
    if (!file) return;
    char line[128], key[64];
    int value;
    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, "%63[^=]=%d", key, &value) != 2 || (value != 0 && value != 1)) continue;
        if (strcmp(key, "bearingCallouts") == 0) settings.callouts = value != 0;
        else if (strcmp(key, "bearingReadout") == 0) settings.bearing = value != 0;
        else if (strcmp(key, "grenadeRange") == 0) settings.grenade = value != 0;
    }
    fclose(file);
}

void Save()
{
    if (FILE* file = Open("w"))
    {
        fprintf(file, "bearingCallouts=%d\nbearingReadout=%d\ngrenadeRange=%d\n",
                int(settings.callouts), int(settings.bearing), int(settings.grenade));
        fclose(file);
    }
}
}
bool BearingCalloutsEnabled() { Load(); return settings.callouts; }
void SetBearingCalloutsEnabled(bool enabled) { Load(); settings.callouts = enabled; Save(); }
bool BearingReadoutEnabled() { Load(); return settings.bearing; }
void SetBearingReadoutEnabled(bool enabled) { Load(); settings.bearing = enabled; Save(); }
bool GrenadeRangeEnabled() { Load(); return settings.grenade; }
void SetGrenadeRangeEnabled(bool enabled) { Load(); settings.grenade = enabled; Save(); }
void ResetCombatAssistSettings() { loaded = true; settings = Settings{}; Save(); }
}
