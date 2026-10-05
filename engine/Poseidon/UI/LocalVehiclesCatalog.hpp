#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon
{
// This catalog activates authored CWA bridge addons, never a foreign game's
// CfgVehicles. A readable ODOL model alone does not establish gameplay support.
struct LocalVehicle
{
    std::string label, modPath, className, addonPatch, modelPath, diagnostic;
    bool ready = false;
};

namespace LocalVehicleDetail
{
inline std::string Canonical(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::filesystem::path Child(const std::filesystem::path& root, const std::string& name)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (Canonical(it->path().filename().string()) == Canonical(name))
            return it->path();
    return {};
}

inline bool CString(std::istream& in, std::string& value)
{
    value.clear();
    char c;
    while (value.size() <= 4096 && in.get(c))
    {
        if (!c)
            return true;
        value += c;
    }
    return false;
}

inline bool U32(std::istream& in, uint32_t& value)
{
    unsigned char bytes[4];
    if (!in.read(reinterpret_cast<char*>(bytes), 4))
        return false;
    value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return true;
}

// Read only a bounded entry table and the selected model's eight-byte header.
// Compressed payloads and unknown revisions fail closed rather than guessing.
inline bool ModelRevision(const std::filesystem::path& archive, const std::string& entry, uint32_t expected,
                          const std::string& modelPath)
{
    std::ifstream in(archive, std::ios::binary);
    struct Entry
    {
        std::string name;
        uint32_t method, stored;
    };
    std::vector<Entry> entries;
    std::string prefix;
    bool terminated = false;
    for (unsigned count = 0; count < 100000 && in && in.tellg() < 16 * 1024 * 1024; ++count)
    {
        std::string name;
        uint32_t method, original, reserved, stamp, stored;
        if (!CString(in, name) || !U32(in, method) || !U32(in, original) || !U32(in, reserved) || !U32(in, stamp) ||
            !U32(in, stored))
            return false;
        if (name.empty() && method == 0x56657273)
        {
            bool propertiesEnd = false;
            for (unsigned p = 0; p < 256; ++p)
            {
                std::string key, value;
                if (!CString(in, key))
                    return false;
                if (key.empty())
                {
                    propertiesEnd = true;
                    break;
                }
                if (!CString(in, value))
                    return false;
                if (Canonical(key) == "prefix")
                    prefix = Canonical(value);
            }
            if (!propertiesEnd)
                return false;
        }
        else if (name.empty() && !method && !stored)
        {
            terminated = true;
            break;
        }
        else
            entries.push_back({Canonical(name), method, stored});
    }
    if (!terminated || prefix.empty() || prefix + "\\" + Canonical(entry) != Canonical(modelPath) + ".p3d")
        return false;
    const auto payload = in.tellg();
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    uint64_t offset = static_cast<uint64_t>(payload);
    for (const auto& item : entries)
    {
        if (offset > static_cast<uint64_t>(length) || item.stored > static_cast<uint64_t>(length) - offset)
            return false;
        if (item.name == Canonical(entry))
        {
            if (item.method || item.stored < 8)
                return false;
            in.seekg(static_cast<std::streamoff>(offset));
            char magic[4];
            uint32_t revision;
            return bool(in.read(magic, 4)) && std::string(magic, 4) == "ODOL" && U32(in, revision) &&
                   revision == expected;
        }
        offset += item.stored;
    }
    return false;
}
} // namespace LocalVehicleDetail

inline std::vector<LocalVehicle> ScanLocalVehicles(const std::filesystem::path& gameDirectory)
{
    using namespace LocalVehicleDetail;
    auto mod = Child(gameDirectory, "@cwr_t72_a1");
    if (mod.empty())
        mod = Child(Child(gameDirectory, "Mods"), "@cwr_t72_a1");
    if (mod.empty())
        return {};
    const auto addons = Child(mod, "addons");
    const std::array<const char*, 11> required = {
        "cwr_t72_a1.pbo",       "cwr_t72_a1_data.pbo",  "cwr_t72_a2_data.pbo",
        "cwr_t100_a3_data.pbo", "cwr_v3s_dz_data.pbo",  "cwr_heli_a1_data.pbo",
        "cwr_heli_a2_data.pbo", "cwr_heli_a3_data.pbo", "cwr_heli_a3_shared_data.pbo",
        "cwr_heli_ca_data.pbo", "cwr_heli_dz_data.pbo"};
    std::string missing;
    for (auto file : required)
    {
        std::error_code ec;
        const auto path = Child(addons, file);
        if (path.empty() || !std::filesystem::is_regular_file(path, ec))
        {
            if (!missing.empty())
                missing += ", ";
            missing += file;
        }
    }
    std::ifstream config(Child(addons, "cwr_t72_a1.pbo"), std::ios::binary);
    std::string configBytes(65536, '\0');
    config.read(configBytes.data(), static_cast<std::streamsize>(configBytes.size()));
    configBytes.resize(static_cast<size_t>(config.gcount()));
    struct Recipe
    {
        const char* label;
        const char* unit;
        const char* archive;
        const char* entry;
        const char* model;
        uint32_t revision;
    };
    const std::array<Recipe, 6> recipes = {
        {{"Arma 1: T-72", "CWR_T72_A1", "cwr_t72_a1_data.pbo", "tracked\\t72.p3d", "ca\\tracked\\t72", 40},
         {"Arma 2: T-72", "CWR_T72_A2", "cwr_t72_a2_data.pbo", "t72.p3d", "ca\\tracka2\\t72", 48},
         {"Arma 3: T-100 Varsuk", "CWR_T100_A3", "cwr_t100_a3_data.pbo", "armor_f_gamma\\mbt_02\\mbt_02_cannon_f.p3d",
          "a3\\armor_f_gamma\\mbt_02\\mbt_02_cannon_f", 73},
         {"Arma 1: UH-60", "CWR_UH60_A1", "cwr_heli_a1_data.pbo", "uh_60.p3d", "ca\\air\\uh_60", 40},
         {"Arma 2: Mi-35", "CWR_MI35_A2", "cwr_heli_a2_data.pbo", "mi35\\mi24_v.p3d", "ca\\air2\\mi35\\mi24_v", 48},
         {"Arma 3: MH-9", "CWR_MH9_A3", "cwr_heli_a3_data.pbo", "heli_light_01\\heli_light_01_f.p3d",
          "a3\\air_f\\heli_light_01\\heli_light_01_f", 73}}};
    std::vector<LocalVehicle> result;
    for (const auto& recipe : recipes)
    {
        LocalVehicle vehicle{recipe.label, mod.string(), recipe.unit, "CWR_T72_A1", recipe.model, {}, false};
        if (!missing.empty())
            vehicle.diagnostic = "Incomplete local bridge: " + missing;
        else if (configBytes.find(std::string(recipe.unit) + '\0') == std::string::npos ||
                 configBytes.find("CfgVehicles") == std::string::npos)
            vehicle.diagnostic = "Bridge config does not declare this class";
        else if (!ModelRevision(Child(addons, recipe.archive), recipe.entry, recipe.revision, recipe.model))
            vehicle.diagnostic = "Model is absent, compressed, incorrectly mounted or has an unverified revision";
        else
        {
            vehicle.ready = true;
            vehicle.diagnostic = "Local bridge; OFP physics and weapons. Later-generation animations may differ.";
        }
        result.push_back(std::move(vehicle));
    }
    return result;
}

// A bounded native CWA preview mission. Only known ready catalog rows qualify;
// arbitrary strings never become executable mission content.
inline std::string MakeLocalVehicleMission(const LocalVehicle& vehicle)
{
    if (!vehicle.ready || vehicle.addonPatch != "CWR_T72_A1")
        return {};
    const std::array<const char*, 6> units = {"CWR_T72_A1",  "CWR_T72_A2",  "CWR_T100_A3",
                                              "CWR_UH60_A1", "CWR_MI35_A2", "CWR_MH9_A3"};
    if (std::find(units.begin(), units.end(), vehicle.className) == units.end())
        return {};
    return "version=11;class Mission{randomSeed=42;addOns[]={\"CWR_T72_A1\"};addOnsAuto[]={\"CWR_T72_A1\"};"
           "class Intel{year=1985;month=6;day=21;hour=12;minute=0;};class Groups{items=1;class Item0{side=\"WEST\";"
           "class Vehicles{items=1;class "
           "Item0{position[]={6532,0,6466};azimut=0;id=0;side=\"WEST\";vehicle=\"SoldierWB\";"
           "player=\"PLAYER COMMANDER\";leader=1;skill=1;};};};};class Vehicles{items=1;class Item0{"
           "position[]={6532,0,6476};azimut=0;id=1;side=\"EMPTY\";vehicle=\"" +
           vehicle.className + "\";skill=0.6;};};};class Intro{};class OutroWin{};class OutroLoose{};";
}
} // namespace Poseidon
