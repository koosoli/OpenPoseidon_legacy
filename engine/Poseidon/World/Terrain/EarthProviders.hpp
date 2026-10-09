// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Whole-world tile providers: where Earth imagery and elevation come from.
//
// Read from <POSEIDON_EARTH_CACHE>/providers.cfg (written by the main menu's Whole World screen), then
// overridden by environment variables of the same meaning:
//
//   imagery = none | esri | mapbox | maptiler | custom     POSEIDON_EARTH_IMAGERY_PROVIDER
//   imagery_url = https://.../{z}/{x}/{y}.jpg?key={key}     POSEIDON_EARTH_IMAGERY_URL   (custom only)
//   imagery_key = ...                                       POSEIDON_EARTH_IMAGERY_KEY
//   imagery_max_zoom = 19                                   POSEIDON_EARTH_IMAGERY_MAX_ZOOM
//   elevation = terrarium | mapbox | custom                 POSEIDON_EARTH_ELEVATION_PROVIDER
//   elevation_url = https://.../{z}/{x}/{y}.png             POSEIDON_EARTH_ELEVATION_URL (custom only)
//   elevation_format = terrarium | terrain-rgb              POSEIDON_EARTH_ELEVATION_FORMAT (custom only)
//   elevation_key = ...                                     POSEIDON_EARTH_ELEVATION_KEY
//
// URL templates are web-mercator XYZ tiles: {z} {x} {y}, {-y} for TMS row order, {q} for a Bing-style
// quadkey, {s} for an a/b/c subdomain, {key} for the API key. Imagery none keeps the built-in sources
// (NASA overview + prepared regional caches); elevation defaults to the free AWS Terrain Tiles.
// Keys never appear in logs (Redact).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace Poseidon::EarthProviders
{
struct Preset
{
    const char* id;
    const char* label;
    const char* url; // nullptr: built-in (imagery) / taken from the config (custom)
    int maxZoom;
    bool needsKey;
    bool terrainRgb; // elevation only: Mapbox Terrain-RGB encoding instead of Terrarium
    const char* attribution;
};

inline const std::array<Preset, 5>& ImageryPresets()
{
    static const std::array<Preset, 5> presets = {{
        {"none", "Built-in (NASA globe + prepared caches)", nullptr, 0, false, false, "NASA Blue Marble"},
        {"esri", "Esri World Imagery",
         "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", 19,
         false, false, "Esri, Maxar, Earthstar Geographics, and the GIS User Community"},
        {"mapbox", "Mapbox Satellite (API key)",
         "https://api.mapbox.com/v4/mapbox.satellite/{z}/{x}/{y}.jpg90?access_token={key}", 19, true, false,
         "Mapbox, Maxar"},
        {"maptiler", "MapTiler Satellite (API key)",
         "https://api.maptiler.com/tiles/satellite-v2/{z}/{x}/{y}.jpg?key={key}", 20, true, false,
         "MapTiler, OpenStreetMap contributors"},
        {"custom", "Custom URL (providers.cfg)", nullptr, 19, false, false, "custom provider"},
    }};
    return presets;
}

inline const std::array<Preset, 3>& ElevationPresets()
{
    static const std::array<Preset, 3> presets = {{
        {"terrarium", "AWS Terrain Tiles (free)",
         "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png", 15, false, false,
         "Mapzen Terrain Tiles (AWS Open Data)"},
        {"mapbox", "Mapbox Terrain-RGB (API key)",
         "https://api.mapbox.com/v4/mapbox.terrain-rgb/{z}/{x}/{y}.pngraw?access_token={key}", 15, true, true,
         "Mapbox"},
        {"custom", "Custom URL (providers.cfg)", nullptr, 15, false, false, "custom provider"},
    }};
    return presets;
}

template <size_t N> inline const Preset* Find(const std::array<Preset, N>& presets, const std::string& id)
{
    for (const auto& p : presets)
        if (id == p.id)
            return &p;
    return nullptr;
}

struct Source
{
    std::string id;  // preset id
    std::string url; // template; empty = none
    std::string key;
    std::string attribution;
    int maxZoom = 19;
    bool terrainRgb = false;
    bool Enabled() const { return !url.empty(); }
};

struct Config
{
    std::string imageryId = "none", imageryUrl, imageryKey;
    int imageryMaxZoom = 0;
    std::string elevationId = "terrarium", elevationUrl, elevationFormat, elevationKey;
    Source Imagery() const;
    Source Elevation() const;
};

inline std::string Trim(std::string s)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
    return s;
}

inline std::filesystem::path ConfigPath(const std::filesystem::path& cache)
{
    return cache / "providers.cfg";
}

inline Config Load(const std::filesystem::path& cache)
{
    Config c;
    if (std::ifstream file(ConfigPath(cache)); file)
    {
        std::string line;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string name = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            if (name == "imagery") c.imageryId = value;
            else if (name == "imagery_url") c.imageryUrl = value;
            else if (name == "imagery_key") c.imageryKey = value;
            else if (name == "imagery_max_zoom") c.imageryMaxZoom = std::atoi(value.c_str());
            else if (name == "elevation") c.elevationId = value;
            else if (name == "elevation_url") c.elevationUrl = value;
            else if (name == "elevation_format") c.elevationFormat = value;
            else if (name == "elevation_key") c.elevationKey = value;
        }
    }
    const auto env = [](const char* name, std::string& target)
    {
        if (const char* v = std::getenv(name); v && *v)
            target = v;
    };
    env("POSEIDON_EARTH_IMAGERY_PROVIDER", c.imageryId);
    env("POSEIDON_EARTH_IMAGERY_URL", c.imageryUrl);
    env("POSEIDON_EARTH_IMAGERY_KEY", c.imageryKey);
    env("POSEIDON_EARTH_ELEVATION_PROVIDER", c.elevationId);
    env("POSEIDON_EARTH_ELEVATION_URL", c.elevationUrl);
    env("POSEIDON_EARTH_ELEVATION_FORMAT", c.elevationFormat);
    env("POSEIDON_EARTH_ELEVATION_KEY", c.elevationKey);
    if (const char* v = std::getenv("POSEIDON_EARTH_IMAGERY_MAX_ZOOM"); v && *v)
        c.imageryMaxZoom = std::atoi(v);
    // A custom URL given on its own implies the custom provider.
    if (std::getenv("POSEIDON_EARTH_IMAGERY_URL") && !std::getenv("POSEIDON_EARTH_IMAGERY_PROVIDER"))
        c.imageryId = "custom";
    if (std::getenv("POSEIDON_EARTH_ELEVATION_URL") && !std::getenv("POSEIDON_EARTH_ELEVATION_PROVIDER"))
        c.elevationId = "custom";
    return c;
}

inline bool Save(const std::filesystem::path& cache, const Config& c)
{
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);
    std::ofstream file(ConfigPath(cache), std::ios::trunc);
    if (!file)
        return false;
    file << "# OpenPoseidon whole-world providers (written by the main menu; see EarthProviders.hpp)\n"
         << "imagery = " << c.imageryId << "\n"
         << "imagery_url = " << c.imageryUrl << "\n"
         << "imagery_key = " << c.imageryKey << "\n";
    if (c.imageryMaxZoom > 0)
        file << "imagery_max_zoom = " << c.imageryMaxZoom << "\n";
    file << "elevation = " << c.elevationId << "\n"
         << "elevation_url = " << c.elevationUrl << "\n"
         << "elevation_format = " << c.elevationFormat << "\n"
         << "elevation_key = " << c.elevationKey << "\n";
    return bool(file);
}

inline Source Config::Imagery() const
{
    Source s;
    const Preset* p = Find(ImageryPresets(), imageryId);
    if (!p || std::string(p->id) == "none")
        return s;
    s.id = p->id;
    s.url = p->url ? p->url : imageryUrl;
    s.key = imageryKey;
    s.attribution = p->attribution;
    s.maxZoom = std::clamp(imageryMaxZoom > 0 ? imageryMaxZoom : p->maxZoom, 1, 22);
    if (p->needsKey && s.key.empty())
        s.url.clear(); // a keyed service without a key would only collect 401s
    return s;
}

inline Source Config::Elevation() const
{
    const Preset* p = Find(ElevationPresets(), elevationId);
    if (!p)
        p = &ElevationPresets()[0];
    Source s;
    s.id = p->id;
    s.url = p->url ? p->url : elevationUrl;
    s.key = elevationKey;
    s.attribution = p->attribution;
    s.maxZoom = p->maxZoom;
    s.terrainRgb = p->url ? p->terrainRgb : elevationFormat == "terrain-rgb";
    if (s.url.empty() || (p->needsKey && s.key.empty()))
    {
        const Preset& free = ElevationPresets()[0]; // never leave the globe without ground
        s = {free.id, free.url, "", free.attribution, free.maxZoom, false};
    }
    return s;
}

inline std::string Quadkey(int z, int x, int y)
{
    std::string q;
    for (int i = z; i > 0; --i)
    {
        const int mask = 1 << (i - 1);
        q += char('0' + ((x & mask) ? 1 : 0) + ((y & mask) ? 2 : 0));
    }
    return q;
}

inline void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

inline std::string ExpandUrl(const Source& source, int z, int x, int y)
{
    std::string url = source.url;
    ReplaceAll(url, "{z}", std::to_string(z));
    ReplaceAll(url, "{x}", std::to_string(x));
    ReplaceAll(url, "{-y}", std::to_string((1 << z) - 1 - y));
    ReplaceAll(url, "{y}", std::to_string(y));
    ReplaceAll(url, "{q}", Quadkey(z, x, y));
    ReplaceAll(url, "{s}", std::string(1, char('a' + (x + y) % 3)));
    ReplaceAll(url, "{key}", source.key);
    return url;
}

// For logs: no query string (where keys live) and no key anywhere in the path.
inline std::string Redact(std::string url, const std::string& key = {})
{
    if (const auto q = url.find('?'); q != std::string::npos)
        url = url.substr(0, q) + "?...";
    if (!key.empty())
        ReplaceAll(url, key, "***");
    return url;
}

// Stable name for a provider's tile cache directory (never contains the key).
inline std::string CacheName(const Source& source)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : source.url)
        h = (h ^ c) * 1099511628211ull;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s-%08x", source.id.c_str(), unsigned(h ^ (h >> 32)));
    return buffer;
}
} // namespace Poseidon::EarthProviders
