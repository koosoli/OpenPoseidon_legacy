// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Evaluator/express.hpp>

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

std::vector<std::string>& AllBankPrefixesMutable()
{
    static std::vector<std::string> prefixes;
    return prefixes;
}

const fs::path& RootPath()
{
    static fs::path root;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        const std::string& configured = Foundation::AppConfig::Instance().GetArma1Root();
        if (!configured.empty())
        {
            std::error_code ec;
            fs::path candidate = fs::absolute(fs::path(configured), ec);
            if (!ec && fs::is_directory(candidate / "AddOns", ec))
                root = candidate;
            else
                LOG_ERROR(Mission, "--arma{} '{}' is not an Arma folder (no AddOns subfolder)",
                          Foundation::AppConfig::Instance().GetArmaGeneration(), configured);
        }
    }
    return root;
}

// --arma2oa: the Operation Arrowhead folder, or empty. Its Expansion/BAF/PMC/ACR
// folders are looked for there and in the --arma2 folder (a combined install).
const fs::path& OaRootPath()
{
    static fs::path root;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        const std::string& configured = Foundation::AppConfig::Instance().GetArma2OaRoot();
        if (!configured.empty())
        {
            std::error_code ec;
            fs::path candidate = fs::absolute(fs::path(configured), ec);
            if (!ec && fs::is_directory(candidate, ec))
                root = candidate;
            else
                LOG_ERROR(Mission, "--arma2oa '{}' is not a folder", configured);
        }
    }
    return root;
}

// `missionRoot`: single/multiplayer mission archives declare only "M09ConvoyAttack"
// as their prefix, while the mission loader needs the world suffix that is in the
// FILE name ("m09convoyattack.sara"). They are mounted as <missionRoot>\<file stem>\.
void MountFolder(const fs::path& folder, bool missionBanks, const char* remountPrefix = nullptr,
                 const char* missionRoot = nullptr)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        return;
    std::vector<fs::path> pbos;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".pbo")
            continue;
        // CWR_* are the bundled Cold War Rearmed conversions (they need the @CWR mod,
        // which is not mounted), not Arma 1 content.
        if (Lower(it->path().filename().string()).rfind("cwr_", 0) == 0)
            continue;
        pbos.push_back(it->path());
    }
    std::sort(pbos.begin(), pbos.end());
    const std::string dir = folder.string() + "\\";
    for (const fs::path& pbo : pbos)
    {
        const int before = GFileBanks.Size();
        // emptyPrefix: the archive's declared prefix (ca\sara, campaigns\ca, ...) wins.
        // No config callback: Arma 1 configs are never parsed into the CWA config.
        GFileBanks.Load(dir.c_str(), "", pbo.stem().string().c_str(), true);
        if (GFileBanks.Size() > before)
        {
            QFBank& bank = GFileBanks[GFileBanks.Size() - 1];
            // Arma 1's Dta archives declare prefixes ("bin", "core") that would shadow CWA's
            // own folders; they are re-mounted under a private name instead.
            if (remountPrefix)
                bank.SetPrefix(RString(remountPrefix) + RString(Lower(pbo.stem().string()).c_str()) + RString("\\"));
            else if (missionRoot)
                bank.SetPrefix(RString(missionRoot) + RString("\\") + RString(Lower(pbo.stem().string()).c_str()) +
                               RString("\\"));
            std::string prefix = (const char*)bank.GetPrefix();
            AllBankPrefixesMutable().push_back(prefix);
            while (!prefix.empty() && (prefix.back() == '\\' || prefix.back() == '/'))
                prefix.pop_back();
            if (missionRoot)
            {
                if (std::find(MissionBankPrefixes().begin(), MissionBankPrefixes().end(), missionRoot) ==
                    MissionBankPrefixes().end())
                    MissionBankPrefixes().push_back(missionRoot);
            }
            else if (!remountPrefix && (missionBanks || Lower(prefix).find("missions") != std::string::npos))
                MissionBankPrefixes().push_back(prefix);
        }
        else
        {
            LOG_WARN(Mission, "{}: could not mount {}", Label(), pbo.string());
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
        {"handgrenade_east", {"HandGrenade"}}, {"handgrenadetimed", {"HandGrenade"}}, {"smokeshell", {"SmokeShell"}},
        {"smokeshellred", {"SmokeShellRed", "SmokeShell"}},
        {"smokeshellgreen", {"SmokeShellGreen", "SmokeShell"}},
        {"1rnd_he_m203", {"GrenadeLauncher"}}, {"1rnd_he_gp25", {"GrenadeLauncher"}},
        {"pipebomb", {"PipeBomb"}},         {"timebomb", {"TimeBomb"}},
        {"mine", {"Mine"}},                 {"mine_e", {"MineE", "Mine"}},
    };
    return table;
}

RString ResolveFrom(const char* configClass, const RString& name,
                    const std::map<std::string, std::vector<const char*>>& table, bool warn = true)
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
                    LOG_INFO(Mission, "{}: {} '{}' is represented by CWA '{}'", Label(), configClass, name.Data(), candidate);
                return RString(candidate);
            }
        }
    }
    // Arma 2 names hundreds of weapons and magazines; the ones not in the table are
    // carried by the CWA weapon of the same kind and calibre (Arma2WeaponFallback).
    if (Generation() == 2 && strcmp(configClass, "CfgWeapons") == 0)
    {
        const RString fallback = Arma2WeaponFallback(name);
        if (fallback.GetLength() > 0 && config->FindEntry(fallback))
        {
            if (reported.insert(std::string(configClass) + key).second)
                LOG_INFO(Mission, "{}: {} '{}' is carried by CWA '{}' (by kind and calibre)", Label(), configClass,
                         name.Data(), fallback.Data());
            return fallback;
        }
    }
    if (warn && reported.insert(std::string(configClass) + key).second)
        LOG_WARN(Mission, "{}: {} '{}' has no CWA counterpart", Label(), configClass, name.Data());
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

const std::vector<std::string>& BankPrefixes()
{
    return AllBankPrefixesMutable();
}

std::string RootFolder()
{
    return RootPath().string();
}

bool Active()
{
    return !RootPath().empty();
}

int Generation()
{
    return Active() ? Foundation::AppConfig::Instance().GetArmaGeneration() : 0;
}

const char* Label()
{
    return Generation() == 2 ? "Arma 2" : "Arma 1";
}

std::vector<std::string> DtaFolders()
{
    std::vector<std::string> out;
    if (!Active())
        return out;
    out.push_back((RootPath() / "Dta").string());
    if (Generation() == 2)
    {
        std::vector<fs::path> roots{RootPath()};
        if (!OaRootPath().empty())
            roots.push_back(OaRootPath());
        for (const fs::path& r : roots)
        {
            std::error_code ec;
            if (fs::is_directory(r / "Expansion" / "Dta", ec))
                out.push_back((r / "Expansion" / "Dta").string());
        }
    }
    return out;
}

void MountArchives()
{
    if (!Active())
        return;
    MissionBankPrefixes().clear();
    AllBankPrefixesMutable().clear();
    const fs::path& root = RootPath();
    // Dta\bin.pbo / core.pbo are NOT mounted: their declared prefixes ("bin", "core")
    // would shadow CWA's own UI resources. Arma1Config reads bin.pbo privately.
    GameState::s_singleQuoteStrings = Generation() == 2;
    if (Generation() == 2)
    {
        // Arma 2 keeps its campaigns and scenarios inside AddOns\missions*.pbo. Operation
        // Arrowhead and its DLCs add folders of their own; later mounts are searched first.
        MountFolder(root / "AddOns", false);
        std::vector<fs::path> roots{root};
        if (!OaRootPath().empty() && OaRootPath() != root)
            roots.push_back(OaRootPath());
        for (const fs::path& r : roots)
        {
            if (r != root)
                MountFolder(r / "AddOns", false);
            for (const char* dlc : {"Expansion", "BAF", "PMC", "ACR"})
                MountFolder(r / dlc / "AddOns", false);
        }
        for (const fs::path& r : roots)
        {
            MountFolder(r / "Missions", true, nullptr, "arma2\\missions");
            MountFolder(r / "MPMissions", true, nullptr, "arma2\\mpmissions");
        }
    }
    else
    {
        MountFolder(root / "AddOns", false);
        MountFolder(root / "DBE1" / "AddOns", false);
        MountFolder(root / "Campaigns", true);
        MountFolder(root / "DBE1" / "Campaigns", true);
        MountFolder(root / "Missions", true, nullptr, "arma1\\missions");
        MountFolder(root / "MPMissions", true, nullptr, "arma1\\mpmissions");
    }
    LOG_INFO(Mission, "{}: mounted {} archives read-only from {}{}{} ({} mission archives)", Label(),
             AllBankPrefixesMutable().size(), root.string(), OaRootPath().empty() ? "" : " and ",
             OaRootPath().string(), MissionBankPrefixes().size());
}

void RegisterWorldClasses()
{
    if (!Active())
        return;
    LoadConfig();
    ParamEntry* cfgWorlds = Pars.FindEntry("CfgWorlds");
    ParamEntry* cfgList = Pars.FindEntry("CfgWorldList");
    ParamEntry* eden = cfgWorlds ? cfgWorlds->FindEntry("Eden") : nullptr;
    if (!cfgWorlds || !eden || !eden->IsClass())
        return;
    struct World
    {
        std::string name;
        std::string wrp;
        std::string description;
    };
    std::vector<World> worlds;
    if (Generation() == 2)
    {
        // Arma 2 / OA: every CfgWorlds class of the Arma 2 config whose world file is
        // mounted (Chernarus, Utes, Takistan, Zargabad, Desert_E, Shapur_BAF, ...). A name
        // CWA already uses ("Intro") gets the private prefix A2_ and is chosen only for
        // missions from an Arma 2 archive (WorldClassForMission).
        const ParamEntry* a2Worlds = ConfigRoot() ? ConfigRoot()->FindEntry("CfgWorlds") : nullptr;
        for (int i = 0; a2Worlds && i < a2Worlds->GetEntryCount(); i++)
        {
            const ParamEntry& w = a2Worlds->GetEntry(i);
            if (!w.IsClass())
                continue;
            const ParamEntry* wn = w.FindEntry("worldName");
            if (!wn || wn->IsClass() || wn->IsArray())
                continue;
            std::string wrp = (const char*)(RStringB)*wn;
            if (wrp.empty())
                continue;
            if (wrp[0] != '\\')
                wrp = "\\" + wrp;
            std::string name = (const char*)w.GetName();
            if (ParamEntry* existing = cfgWorlds->FindEntryNoInheritance(name.c_str()))
            {
                const ParamEntry* owned = existing->FindEntry("opArma1World");
                if (!owned || static_cast<int>(*owned) != 1)
                    name = "A2_" + name;
            }
            std::string description = std::string("Arma 2 / ") + (const char*)w.GetName();
            worlds.push_back({name, wrp, description});
        }
    }
    else
    {
        worlds = {
            {"Sara", "\\ca\\sara\\sara.wrp", "Arma 1 / Sahrani"},
            {"SaraLite", "\\ca\\saralite\\SaraLite.wrp", "Arma 1 / Southern Sahrani"},
            {"Sara_dbe1", "\\DBE1\\Sara_DBE1\\sara_dbe1.wrp", "Arma 1 / United Sahrani"},
            {"Porto", "\\ca\\Desert2\\porto.wrp", "Arma 1 / Porto"},
            // Arma 1's "Intro" desert collides with CWA's Intro island: it is registered
            // under a private name and chosen only for missions from an Arma 1 archive.
            {"A1_Intro", "\\ca\\desert\\intro.wrp", "Arma 1 / Rahmadi desert"},
        };
    }
    for (const World& world : worlds)
    {
        if (!QIFStreamB::FileExist(world.wrp.c_str() + 1))
            continue;
        if (ParamEntry* existing = cfgWorlds->FindEntryNoInheritance(world.name.c_str()))
        {
            const ParamEntry* owned = existing->FindEntry("opArma1World");
            if (!owned || static_cast<int>(*owned) != 1)
            {
                LOG_WARN(Mission, "{}: CfgWorlds already defines '{}', left unchanged", Label(), world.name);
                continue;
            }
        }
        const ParamAccessMode worldAccess = cfgWorlds->GetAccessMode();
        cfgWorlds->SetAccessMode(PAReadAndWrite);
        ParamClass* cls = cfgWorlds->AddClass(world.name.c_str());
        cfgWorlds->SetAccessMode(worldAccess);
        if (!cls)
            continue;
        // Write owned entries before attaching Eden as the base (see RegisterWorldClass).
        cls->SetBase(nullptr);
        cls->Add("opArma1World", 1);
        cls->Add("worldName", RString(world.wrp.c_str()));
        cls->Add("description", RString(world.description.c_str()));
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
            cfgList->AddClass(world.name.c_str());
            cfgList->SetAccessMode(listAccess);
        }
        LOG_INFO(Mission, "{}: world class '{}' -> {}", Label(), world.name, world.wrp);
    }
}

RString ResolveVehicleClass(const RString& className)
{
    if (!Active() || className.GetLength() == 0)
        return className;
    // A class generated earlier for this mission keeps resolving to itself.
    if (_strnicmp(className.Data(), "A1x_", 4) == 0)
        return className;
    RString cwa = ResolveFrom("CfgVehicles", className, VehicleAliases(), false);
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    if (Generation() == 2 && vehicles && !vehicles->FindEntry(cwa))
    {
        // Arma 2 vehicle names carry their type ("BRDM2_INS", "UralOpen_CDF",
        // "DSHKM_Ins"): the CWA vehicle of that type, before the generic fallback.
        static const std::vector<std::pair<const char*, std::vector<const char*>>> rules = {
            {"brdm", {"BRDM"}},           {"uralreammo", {"Ural", "Truck5t"}}, {"ural", {"Ural", "Truck5t"}},
            {"kamaz", {"Ural", "Truck5t"}}, {"uaz", {"UAZ"}},                 {"bmp", {"BMP", "BMP2"}},
            {"btr", {"BMP", "BMP2"}},     {"t72", {"T72"}},                   {"t90", {"T80", "T72"}},
            {"t34", {"T55", "T72"}},      {"t55", {"T55", "T72"}},            {"zsu", {"ZSU", "T72"}},
            {"2s6", {"ZSU", "T72"}},      {"m1a", {"M1Abrams"}},              {"m2a", {"M2A2", "M113"}},
            {"m6_", {"Vulcan", "M2A2"}},  {"aav", {"M113", "M2A2"}},          {"lav25", {"M2A2", "M113"}},
            {"m113", {"M113"}},           {"hmmwv", {"HMMWV", "Jeep"}},       {"mtvr", {"Truck5t"}},
            {"v3s", {"Truck5t", "Ural"}}, {"landrover", {"GJeep", "Jeep"}},   {"skoda", {"Skoda"}},
            {"lada", {"SkodaBlue", "Skoda"}}, {"car_", {"SkodaBlue", "Skoda"}}, {"volha", {"SkodaRed", "Skoda"}},
            {"vwgolf", {"SkodaGreen", "Skoda"}}, {"datsun", {"Skoda"}},         {"hilux", {"Skoda"}},
            {"ikarus", {"Bus"}},          {"bus", {"Bus"}},                   {"tractor", {"Tractor"}},
            {"mi17", {"Mi17"}},           {"mi24", {"Mi24", "Mi17"}},         {"ka52", {"Mi24", "Mi17"}},
            {"ka60", {"Mi17"}},           {"uh1", {"UH60"}},                  {"uh60", {"UH60"}},
            {"mh60", {"UH60"}},           {"mh6", {"OH58", "UH60"}},          {"ah6", {"OH58", "UH60"}},
            {"ah1", {"Cobra"}},           {"ah64", {"Cobra"}},                {"mv22", {"UH60"}},
            {"ch47", {"CH47D", "UH60"}},  {"c130", {"UH60"}},                 {"su25", {"Su25"}},
            {"su34", {"Su25"}},           {"su39", {"Su25"}},                 {"a10", {"A10"}},
            {"av8b", {"A10"}},            {"f35", {"A10"}},                   {"l39", {"Su25"}},
            {"an2", {"Cessna", "UH60"}},  {"zu23", {"M2StaticMG"}},           {"dshk", {"M2StaticMG"}},
            {"kord", {"M2StaticMG"}},     {"m2staticmg", {"M2StaticMG"}},     {"m2hd", {"M2StaticMG"}},
            {"ags", {"M2StaticMG"}},      {"mk19", {"M2StaticMG"}},           {"d30", {"M2StaticMG"}},
            {"m119", {"M2StaticMG"}},     {"2b14", {"M2StaticMG"}},           {"m252", {"M2StaticMG"}},
            {"searchlight", {"M2StaticMG"}}, {"igla", {"M2StaticMG"}},        {"stinger_pod", {"M2StaticMG"}},
            {"rhib", {"BoatW"}},          {"zodiac", {"BoatW"}},              {"pbx", {"BoatW"}},
            {"smallboat", {"BoatW"}},     {"fishing_boat", {"BoatW"}},        {"tt650", {"Kolo"}},
            {"m1030", {"Kolo"}},          {"motorcycle", {"Kolo"}},           {"old_bike", {"Kolo"}},
            {"mmt", {"Kolo"}},
        };
        const std::string lower = Lower(className.Data());
        std::string sim;
        if (const ParamEntry* a2 = FindConfigClass("CfgVehicles", className.Data()))
            if (const ParamEntry* e = a2->FindEntry("simulation"); e && !e->IsClass() && !e->IsArray())
                sim = Lower((const char*)(RStringB)*e);
        const bool vehicle = sim == "car" || sim == "tank" || sim == "helicopter" || sim == "airplane" ||
                             sim == "ship" || sim == "motorcycle" || sim == "bicycle" || sim == "parachute";
        for (const auto& [word, candidates] : rules)
        {
            if (!vehicle)
                break;
            if (lower.find(word) == std::string::npos)
                continue;
            for (const char* c : candidates)
                if (vehicles->FindEntry(c))
                {
                    cwa = RString(c);
                    break;
                }
            if (vehicles->FindEntry(cwa))
                break;
        }
    }
    if (vehicles && !vehicles->FindEntry(cwa))
    {
        static std::set<std::string> reported;
        const RString fallback = FallbackCwaClass(className);
        const bool first = reported.insert(Lower(className.Data())).second;
        if (fallback.GetLength() > 0)
        {
            if (first)
                LOG_INFO(Mission, "{}: CfgVehicles '{}' is carried by CWA '{}' (by its Arma 1 kind)", Label(),
                         className.Data(), fallback.Data());
            cwa = fallback;
        }
        else if (first)
        {
            LOG_WARN(Mission, "{}: CfgVehicles '{}' has no CWA counterpart", Label(), className.Data());
        }
    }
    return Arma1ModelClass(className, cwa);
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
        // Arma 1 content patches are CA*, the expansion DBE1 and Warfare -- and anything
        // else Arma 1's own CfgPatches declares ("Sara", "SaraLite", "Porto", ...).
        const bool arma1Patch = FindConfigClass("CfgPatches", addOns[i].Data()) != nullptr;
        if (known)
            continue;
        if (arma1Patch || name.rfind("ca", 0) == 0 || name.rfind("dbe1", 0) == 0 || name == "warfare")
            LOG_INFO(Mission, "{}: addon '{}' is provided by the mounted Arma 1 archives", Label(), addOns[i].Data());
        else
            // Refusing the whole mission over one name helps nobody: a class it really
            // needs and cannot find is reported (and carried by a CWA class) on its own.
            LOG_WARN(Mission, "{}: addon '{}' is not installed; mission loaded without it", Label(), addOns[i].Data());
        addOns.DeleteAt(i);
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
    LOG_INFO(Mission, "{}: campaign key '{}' activated", Label(), key);
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
    // A full virtual path ("campaigns\ca\missions\m09convoyattack.sara") is used as is.
    if (mission.find('\\') != std::string::npos)
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
    // Anywhere below a mounted mission archive (Arma 2 keeps scenarios, campaigns and
    // bootcamp in subfolders of AddOns\missions*.pbo): match the mission folder name,
    // or a trailing part of its path.
    {
        std::string want = Lower(mission);
        const std::string suffix = "\\" + want + "\\mission.sqm";
        for (const std::string& file : MissionFiles())
        {
            const std::string lower = Lower(file);
            if (lower == want + "\\mission.sqm" ||
                (lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0))
                return ForwardSlashes(file);
        }
    }
    error = "mission '" + spec + "' was not found in the " + Label() + " campaign/mission archives";
    return {};
}

bool IsArma1MissionDirectory(const char* missionDirectory)
{
    if (!Active() || !missionDirectory || !*missionDirectory)
        return false;
    std::string dir = Lower(missionDirectory);
    std::replace(dir.begin(), dir.end(), '/', '\\');
    for (const std::string& prefix : AllBankPrefixesMutable())
    {
        const std::string p = Lower(prefix);
        if (!p.empty() && dir.rfind(p, 0) == 0)
            return true;
    }
    return false;
}

RString WorldClassForMission(const RString& world, const char* missionDirectory)
{
    if (!IsArma1MissionDirectory(missionDirectory))
        return world;
    const std::string alias = (Generation() == 2 ? "A2_" : "A1_") + std::string(world.Data());
    const ParamEntry* worlds = Pars.FindEntry("CfgWorlds");
    const ParamEntry* entry = worlds ? worlds->FindEntry(alias.c_str()) : nullptr;
    return entry ? RString(alias.c_str()) : world;
}

std::string CampaignBaseForMission(const std::string& missionFile)
{
    const std::string path = ForwardSlashes(missionFile);
    const std::string lower = Lower(path);
    if (lower.rfind("campaigns/", 0) == 0)
    {
        const size_t missions = lower.find("/missions/");
        if (missions == std::string::npos)
            return {};
        return path.substr(0, missions + 1);
    }
    // Arma 2: <archive prefix>/campaign/missions/<mission>/mission.sqm, with the campaign's
    // description.ext next to that missions folder.
    const size_t missions = lower.rfind("/missions/");
    if (Generation() == 2 && missions != std::string::npos && missions > 0)
    {
        const std::string base = path.substr(0, missions + 1);
        if (QIFStreamB::FileExist((base + "description.ext").c_str()))
            return base;
    }
    return {};
}
} // namespace Poseidon::Arma1Compat
