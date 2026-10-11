// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
namespace
{
namespace fs = std::filesystem;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ForwardSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

// Campaign/mission banks mounted from the Arma 1 install, as their declared prefix
// ("campaigns\ca", "ca\missions"). Used to find --arma1-mission without guessing.
std::vector<std::string>& MissionBankPrefixes()
{
    static std::vector<std::string> prefixes;
    return prefixes;
}

// File names (lower case, no extension) of every archive mounted from the Arma 1 folders. An Arma 1
// add-on's CfgPatches class is named after its archive (sara.pbo -> "sara"), so a mission's
// addOns[] entry with one of these names is provided by the mount even outside the CA* family.
std::set<std::string>& MountedArchiveNames()
{
    static std::set<std::string> names;
    return names;
}

bool HasPbos(const fs::path& folder)
{
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".pbo")
            return true;
    return false;
}

// --arma1 names one Arma 1 folder, or several separated by ';'. A retail install keeps its
// archives in AddOns\ and Campaigns\; Bohemia's licensed data packages (ALDP_A1_PBOs_*) split the
// same archives over several folders with the add-on PBOs at each folder's top level. Both work.
const std::vector<fs::path>& Roots()
{
    static std::vector<fs::path> roots;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        const std::string& configured = Foundation::AppConfig::Instance().GetArma1Root();
        size_t start = 0;
        while (start <= configured.size() && !configured.empty())
        {
            const size_t end = std::min(configured.find(';', start), configured.size());
            const std::string item = configured.substr(start, end - start);
            start = end + 1;
            if (item.empty())
                continue;
            std::error_code ec;
            const fs::path candidate = fs::absolute(fs::path(item), ec);
            if (!ec && (fs::is_directory(candidate / "AddOns", ec) || fs::is_directory(candidate / "Campaigns", ec) ||
                        fs::is_directory(candidate / "Missions", ec) || HasPbos(candidate)))
                roots.push_back(candidate);
            else
                LOG_ERROR(Mission, "--arma1 '{}' holds no Arma 1 archives (AddOns, Campaigns, Missions or PBOs)",
                          item);
        }
    }
    return roots;
}

void MountFolder(const fs::path& folder, bool missionBanks)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        return;
    std::vector<fs::path> pbos;
    // Mission folders keep loose groups one level down (Missions\tutorial\t01_...sara.pbo).
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it.depth() >= (missionBanks ? 1 : 0))
            it.disable_recursion_pending();
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".pbo")
            pbos.push_back(it->path());
    }
    std::sort(pbos.begin(), pbos.end());
    for (const fs::path& pbo : pbos)
    {
        const std::string dir = pbo.parent_path().string() + "\\";
        const int before = GFileBanks.Size();
        // emptyPrefix: the archive's declared prefix (ca\sara, campaigns\ca, ...) wins.
        // No config callback: Arma 1 configs are never parsed into the CWA config.
        GFileBanks.Load(dir.c_str(), "", pbo.stem().string().c_str(), true);
        if (GFileBanks.Size() > before)
        {
            MountedArchiveNames().insert(Lower(pbo.stem().string()));
            std::string prefix = (const char*)GFileBanks[GFileBanks.Size() - 1].GetPrefix();
            while (!prefix.empty() && (prefix.back() == '\\' || prefix.back() == '/'))
                prefix.pop_back();
            // A single-mission archive (Missions\m02minerconvoy.sara.pbo) may declare a bare prefix
            // ("M02MinerConvoy"). The terrain is the mission folder's extension, so mount it under
            // its file name as Arma 1 did; otherwise the mission loads with no world.
            const std::string stem = pbo.stem().string();
            if (missionBanks && prefix.find_first_of("\\/.") == std::string::npos &&
                fs::path(stem).has_extension())
            {
                GFileBanks.Set(GFileBanks.Size() - 1).SetPrefix(RString((stem + "\\").c_str()));
                prefix = stem;
            }
            if (missionBanks || Lower(prefix).find("missions") != std::string::npos)
                MissionBankPrefixes().push_back(prefix);
        }
        else
        {
            LOG_WARN(Mission, "Arma 1: could not mount {}", pbo.string());
        }
    }
}

// Arma 1 class -> CWA candidates, first existing one wins.
const std::map<std::string, std::vector<const char*>>& VehicleAliases()
{
    static const std::map<std::string, std::vector<const char*>> table = {
        // USMC
        {"soldierwar", {"SoldierWMG", "SoldierWB"}},
        {"soldierwat", {"SoldierWLAW", "SoldierWAT", "SoldierWB"}},
        {"squadleaderw", {"OfficerW", "SoldierWB"}},
        {"teamleaderw", {"SoldierWG", "SoldierWB"}},
        {"soldierwsaboteurpipe", {"SoldierWSaboteur", "SoldierWB"}},
        {"soldierwsaboteurrecon", {"SoldierWSaboteur", "SoldierWB"}},
        {"soldierwsaboteurmarksman", {"SoldierWSaboteur", "SoldierWSniper"}},
        {"biscamelpilot", {"SoldierWPilot"}},
        // SLA
        {"soldierear", {"SoldierEMG", "SoldierEB"}},
        {"squadleadere", {"OfficerE", "SoldierEB"}},
        {"teamleadere", {"SoldierEG", "SoldierEB"}},
        {"soldieresaboteurpipe", {"SoldierESaboteur", "SoldierEB"}},
        {"soldieresaboteurmarksman", {"SoldierESaboteur", "SoldierESniper"}},
        // RACS
        {"soldiergar", {"SoldierGMG", "SoldierGB"}},
        {"squadleaderg", {"OfficerG", "SoldierGB"}},
        {"teamleaderg", {"SoldierGG", "SoldierGB"}},
        // Air
        {"ah1w", {"Cobra"}},
        {"ah6", {"OH58", "UH60"}},
        {"mh6", {"OH58", "UH60"}},
        {"uh60mg", {"UH60MG", "UH60"}},
        {"mi17_mg", {"Mi17"}},
        {"ka50", {"Mi24", "Mi17"}},
        {"av8b", {"A10"}},
        {"av8b2", {"A10"}},
        {"su34", {"Su25"}},
        {"su34b", {"Su25"}},
        {"c130j", {"UH60"}},
        // Armour / cars
        {"m1abrams", {"M1Abrams"}},
        {"m113_racs", {"M113"}},
        {"m113ambul_racs", {"M113Ambul", "M113"}},
        {"stryker_icv_m2", {"M2A2", "M113"}},
        {"stryker_icv_mk19", {"M2A2", "M113"}},
        {"stryker_tow", {"M2A2", "M113"}},
        {"lav25", {"M2A2", "M113"}},
        {"hmmwv50", {"HMMWV", "JeepMG"}},
        {"hmmwvmk", {"HMMWV", "JeepMG"}},
        {"hmmwvtow", {"HMMWV", "JeepMG"}},
        {"truck5tmg", {"Truck5tOpen", "Truck5t"}},
        {"t72_racs", {"T72"}},
        {"bmp2ambul", {"BMPAmbul"}},
        {"brdm2", {"BRDM"}},
        {"brdm2_atgm", {"BRDM"}},
        {"uazmg", {"UAZ"}},
        {"uaz_ags30", {"UAZ"}},
        {"uralopen", {"Ural"}},
        {"landrover_closed", {"GJeep", "Jeep"}},
        {"landrovermg", {"JeepMG", "GJeep"}},
        {"car_hatchback", {"Skoda", "SkodaBlue"}},
        {"car_sedan", {"SkodaBlue", "Skoda"}},
        {"datsun1_civil_1_open", {"Skoda"}},
        {"hilux1_civil_1_open", {"Skoda"}},
        {"bus_city", {"Bus"}},
        {"v3s_civ", {"Truck5t", "Ural"}},
        {"rhib", {"BoatW"}},
        {"rhib2turret", {"BoatW"}},
        {"zodiac", {"BoatW"}},
        {"pbx", {"BoatW"}},
        // Statics
        {"m2hd_mini_tripod", {"M2StaticMG"}},
        {"dshkm", {"M2StaticMG"}},
        {"dshkm_mini_tripod", {"M2StaticMG"}},
        {"d30", {"M2StaticMG"}},
        {"m119", {"M2StaticMG"}},
        // Ammo crates
        {"usbasicweapons", {"WeaponBoxWest", "ReammoBox"}},
        {"usbasicammunition", {"WeaponBoxWest", "ReammoBox"}},
        {"rubasicweapons", {"WeaponBoxEast", "ReammoBox"}},
        {"rubasicammunition", {"WeaponBoxEast", "ReammoBox"}},
        {"localbasicweapons", {"WeaponBoxGuer", "ReammoBox"}},
        {"localbasicammunition", {"WeaponBoxGuer", "ReammoBox"}},
    };
    return table;
}

const std::map<std::string, std::vector<const char*>>& WeaponAliases()
{
    static const std::map<std::string, std::vector<const char*>> table = {
        {"m16a2", {"M16"}},          {"m16a2gl", {"M16GrenadeLauncher", "M16"}},
        {"m16a4", {"M16"}},          {"m16a4gl", {"M16GrenadeLauncher", "M16"}},
        {"m16a4_acg", {"M16"}},      {"m4", {"M16"}},
        {"m4gl", {"M16GrenadeLauncher", "M16"}},
        {"m4a1", {"M16"}},           {"m4aim", {"M16"}},
        {"m4a1sd", {"M16"}},         {"m4spr", {"M21", "M16"}},
        {"m24", {"M21"}},            {"m107", {"M21"}},
        {"m249", {"M60"}},           {"m240", {"M60"}},
        {"mp5a5", {"HK", "M16"}},    {"mp5sd", {"HK", "M16"}},
        {"m9", {"Beretta", "Revolver"}}, {"m9sd", {"Beretta", "Revolver"}},
        {"colt1911", {"Revolver"}},  {"makarov", {"Tokarev", "Revolver"}},
        {"m136", {"LAWLauncher"}},   {"smaw", {"CarlGustavLauncher", "LAWLauncher"}},
        {"javelin", {"CarlGustavLauncher", "LAWLauncher"}},
        {"stinger", {"Stinger"}},    {"strela", {"Strela"}},
        {"ak74", {"AK74"}},          {"ak74gl", {"AK74GrenadeLauncher", "AK74"}},
        {"aks74u", {"AK74SU", "AK74"}}, {"aks74un", {"AK74SU", "AK74"}},
        {"ak47", {"AK47"}},          {"aks74pso", {"AK74", "SVDDragunov"}},
        {"svd", {"SVDDragunov"}},    {"pk", {"PK"}},
        {"rpg7v", {"RPGLauncher"}},  {"rpg18", {"RPGLauncher"}},
        {"put", {"Put", "PipeBomb"}},
        {"throw", {"Throw"}},        {"binocular", {"Binocular"}},
        {"nvgoggles", {"NVGoggles"}},
    };
    return table;
}

const std::map<std::string, std::vector<const char*>>& MagazineAliases()
{
    static const std::map<std::string, std::vector<const char*>> table = {
        {"30rnd_556x45_stanag", {"M16"}},   {"30rnd_556x45_stanagsd", {"M16"}},
        {"200rnd_556x45_m249", {"M60"}},    {"100rnd_762x51_m240", {"M60"}},
        {"5rnd_762x51_m24", {"M21"}},       {"10rnd_127x99_m107", {"M21"}},
        {"30rnd_9x19_mp5", {"HK", "M16"}},  {"30rnd_9x19_mp5sd", {"HK", "M16"}},
        {"15rnd_9x19_m9", {"Beretta", "Revolver"}}, {"15rnd_9x19_m9sd", {"Beretta", "Revolver"}},
        {"7rnd_45acp_1911", {"Revolver"}},  {"8rnd_9x18_makarov", {"Tokarev", "Revolver"}},
        {"30rnd_545x39_ak", {"AK74"}},      {"30rnd_762x39_ak47", {"AK47"}},
        {"10rnd_762x54_svd", {"SVDDragunov"}}, {"100rnd_762x54_pk", {"PK"}},
        {"m136", {"LAWLauncher"}},          {"smaw_he", {"CarlGustavLauncher", "LAWLauncher"}},
        {"smaw_hedp", {"CarlGustavLauncher", "LAWLauncher"}},
        {"javelin", {"CarlGustavLauncher", "LAWLauncher"}},
        {"stinger", {"Stinger"}},           {"strela", {"Strela"}},
        {"pg7v", {"RPGLauncher"}},          {"pg7vr", {"RPGLauncher"}},
        {"handgrenade", {"HandGrenade"}},   {"handgrenade_west", {"HandGrenade"}},
        {"handgrenade_east", {"HandGrenade"}}, {"smokeshell", {"SmokeShell"}},
        {"smokeshellred", {"SmokeShellRed", "SmokeShell"}},
        {"smokeshellgreen", {"SmokeShellGreen", "SmokeShell"}},
        {"1rnd_he_m203", {"GrenadeLauncher"}}, {"1rnd_he_gp25", {"GrenadeLauncher"}},
        {"pipebomb", {"PipeBomb"}},         {"timebomb", {"TimeBomb"}},
        {"mine", {"Mine"}},                 {"mine_e", {"MineE", "Mine"}},
    };
    return table;
}

RString ResolveFrom(const char* configClass, const RString& name,
                    const std::map<std::string, std::vector<const char*>>& table)
{
    if (!Active() || name.GetLength() == 0)
        return name;
    const ParamEntry* config = Pars.FindEntry(configClass);
    if (!config || config->FindEntry(name))
        return name;
    static std::set<std::string> reported;
    const std::string key = Lower(name.Data());
    const auto found = table.find(key);
    if (found != table.end())
    {
        for (const char* candidate : found->second)
        {
            if (config->FindEntry(candidate))
            {
                if (reported.insert(std::string(configClass) + key).second)
                    LOG_INFO(Mission, "Arma 1: {} '{}' is represented by CWA '{}'", configClass, name.Data(), candidate);
                return RString(candidate);
            }
        }
    }
    if (reported.insert(std::string(configClass) + key).second)
        LOG_WARN(Mission, "Arma 1: {} '{}' has no CWA counterpart", configClass, name.Data());
    return name;
}

fs::path KeysFile()
{
    return fs::path(Foundation::GamePaths::Instance().UserDir()) / "Arma1CampaignKeys.txt";
}

std::set<std::string>& Keys()
{
    static std::set<std::string> keys;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        std::ifstream in(KeysFile());
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty())
                keys.insert(Lower(line));
        }
    }
    return keys;
}
} // namespace

bool Active()
{
    return !Roots().empty();
}

void MountArchives()
{
    if (!Active())
        return;
    MissionBankPrefixes().clear();
    MountedArchiveNames().clear();
    for (const fs::path& root : Roots())
    {
        MountFolder(root, false); // a data package's add-on PBOs lie at its top level
        MountFolder(root / "AddOns", false);
        MountFolder(root / "DBE1" / "AddOns", false);
        MountFolder(root / "Campaigns", true);
        MountFolder(root / "DBE1" / "Campaigns", true);
        MountFolder(root / "Missions", true);
        LOG_INFO(Mission, "Arma 1: mounted read-only from {} ({} mission archives so far)", root.string(),
                 MissionBankPrefixes().size());
    }
}

void RegisterWorldClasses()
{
    if (!Active())
        return;
    ParamEntry* cfgWorlds = Pars.FindEntry("CfgWorlds");
    ParamEntry* cfgList = Pars.FindEntry("CfgWorldList");
    ParamEntry* eden = cfgWorlds ? cfgWorlds->FindEntry("Eden") : nullptr;
    if (!cfgWorlds || !eden || !eden->IsClass())
        return;
    struct World
    {
        const char* name;
        const char* wrp;
        const char* description;
    };
    static const World worlds[] = {
        {"Sara", "\\ca\\sara\\sara.wrp", "Arma 1 / Sahrani"},
        {"SaraLite", "\\ca\\saralite\\SaraLite.wrp", "Arma 1 / Southern Sahrani"},
        {"Sara_dbe1", "\\DBE1\\Sara_DBE1\\sara_dbe1.wrp", "Arma 1 / United Sahrani"},
        {"Porto", "\\ca\\Desert2\\porto.wrp", "Arma 1 / Porto"},
    };
    for (const World& world : worlds)
    {
        if (!QIFStreamB::FileExist(world.wrp + 1))
            continue;
        if (ParamEntry* existing = cfgWorlds->FindEntryNoInheritance(world.name))
        {
            const ParamEntry* owned = existing->FindEntry("opArma1World");
            if (!owned || static_cast<int>(*owned) != 1)
            {
                LOG_WARN(Mission, "Arma 1: CfgWorlds already defines '{}', left unchanged", world.name);
                continue;
            }
        }
        const ParamAccessMode worldAccess = cfgWorlds->GetAccessMode();
        cfgWorlds->SetAccessMode(PAReadAndWrite);
        ParamClass* cls = cfgWorlds->AddClass(world.name);
        cfgWorlds->SetAccessMode(worldAccess);
        if (!cls)
            continue;
        // Write owned entries before attaching Eden as the base (see RegisterWorldClass).
        cls->SetBase(nullptr);
        cls->Add("opArma1World", 1);
        cls->Add("worldName", RString(world.wrp));
        cls->Add("description", RString(world.description));
        if (ParamClass* names = cls->AddClass("Names"))
        {
            names->SetBase(nullptr);
            names->Clear();
        }
        cls->SetBase(eden->GetClassInterface());
        if (cfgList)
        {
            const ParamAccessMode listAccess = cfgList->GetAccessMode();
            cfgList->SetAccessMode(PAReadAndWrite);
            cfgList->AddClass(world.name);
            cfgList->SetAccessMode(listAccess);
        }
        LOG_INFO(Mission, "Arma 1: world class '{}' -> {}", world.name, world.wrp);
    }
}

RString ResolveVehicleClass(const RString& className)
{
    return ResolveFrom("CfgVehicles", className, VehicleAliases());
}

RString ResolveWeaponClass(const RString& className)
{
    return ResolveFrom("CfgWeapons", className, WeaponAliases());
}

RString ResolveMagazineClass(const RString& className)
{
    // OFP/CWA keeps magazines in CfgWeapons as well.
    return ResolveFrom("CfgWeapons", className, MagazineAliases());
}

void FilterMissionAddons(FindArrayRStringCI& addOns)
{
    if (!Active())
        return;
    const ParamEntry* patches = Pars.FindEntry("CfgPatches");
    for (int i = addOns.Size() - 1; i >= 0; i--)
    {
        const std::string name = Lower(addOns[i].Data());
        const bool known = patches && patches->FindEntry(addOns[i]);
        // Arma 1 content patches are CA*, the expansion DBE1 and Warfare.
        if (!known && (name.rfind("ca", 0) == 0 || name.rfind("dbe1", 0) == 0 || name == "warfare" ||
                       MountedArchiveNames().count(name) > 0))
        {
            LOG_INFO(Mission, "Arma 1: addon '{}' is provided by the mounted Arma 1 archives", addOns[i].Data());
            addOns.DeleteAt(i);
        }
    }
}

bool IsKeyActive(const char* key)
{
    return key && Keys().count(Lower(key)) > 0;
}

void ActivateKey(const char* key)
{
    if (!key || !*key)
        return;
    if (!Keys().insert(Lower(key)).second)
        return;
    std::error_code ec;
    fs::create_directories(KeysFile().parent_path(), ec);
    std::ofstream out(KeysFile(), std::ios::app);
    out << key << "\n";
    LOG_INFO(Mission, "Arma 1: campaign key '{}' activated", key);
}

std::string ResolveMissionFile(const std::string& spec, std::string& error)
{
    error.clear();
    if (!Active())
    {
        error = "--arma1 must name the Arma 1 install folder";
        return {};
    }
    std::string mission = spec;
    std::replace(mission.begin(), mission.end(), '/', '\\');
    while (!mission.empty() && mission.back() == '\\')
        mission.pop_back();
    if (mission.size() > 12 && Lower(mission.substr(mission.size() - 12)) == "\\mission.sqm")
        mission.resize(mission.size() - 12);

    std::vector<std::string> candidates;
    // A full virtual path ("campaigns\ca\missions\m09convoyattack.sara") is used as is -- and so is a
    // bare name, which is the whole prefix of a single-mission archive (Missions\m09convoyattack.sara.pbo).
    candidates.push_back(mission);
    for (const std::string& prefix : MissionBankPrefixes())
    {
        candidates.push_back(prefix + "\\missions\\" + mission);
        candidates.push_back(prefix + "\\" + mission);
    }
    for (const std::string& candidate : candidates)
    {
        const std::string file = candidate + "\\mission.sqm";
        if (QIFStreamB::FileExist(file.c_str()))
            return ForwardSlashes(file);
    }
    error = "mission '" + spec + "' was not found in the Arma 1 campaign/mission archives";
    return {};
}

std::string CampaignBaseForMission(const std::string& missionFile)
{
    const std::string path = ForwardSlashes(missionFile);
    const std::string lower = Lower(path);
    if (lower.rfind("campaigns/", 0) != 0)
        return {};
    const size_t missions = lower.find("/missions/");
    if (missions == std::string::npos)
        return {};
    return path.substr(0, missions + 1);
}
} // namespace Poseidon::Arma1Compat
