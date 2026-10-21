// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Evaluator/express.hpp>

#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

void Arma2StartStallMonitor(); // GameStateExtA2.cpp (Arma 2 test mode only)
void Arma3InstallFunctionLibrary(); // GameStateExtA3Functions.cpp
void Arma3RegisterScriptCommands(); // GameStateExtA3.cpp

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

bool HasPbos(const fs::path& folder)
{
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".pbo")
            return true;
    return false;
}

// Every configured root. --arma2 names one install folder. --arma1 names one Arma 1 folder or
// several separated by ';': a retail install keeps its archives in AddOns\ and Campaigns\, while
// Bohemia's licensed data packages (ALDP_A1_PBOs_*) split the same archives over several folders
// with the add-on PBOs at each folder's top level (the ARMA CONTENT browser passes those).
const std::vector<fs::path>& Roots()
{
    static std::vector<fs::path> roots;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        const std::string& configured = Foundation::AppConfig::Instance().GetArma1Root();
        const int generation = Foundation::AppConfig::Instance().GetArmaGeneration();
        size_t start = 0;
        while (start <= configured.size() && !configured.empty())
        {
            // ';' joins the parts of a data package (Arma 1's and Arma 2's ALDP sets ship one game as
            // several folders of PBOs); the ARMA CONTENT browser passes them that way.
            const size_t end = std::min(configured.find(';', start), configured.size());
            const std::string item = configured.substr(start, end - start);
            start = end + 1;
            if (item.empty())
                continue;
            std::error_code ec;
            const fs::path candidate = fs::absolute(fs::path(item), ec);
            const bool ok = !ec && (fs::is_directory(candidate / "AddOns", ec) || HasPbos(candidate) ||
                                    (generation != 2 && (fs::is_directory(candidate / "Campaigns", ec) ||
                                                         fs::is_directory(candidate / "Missions", ec))));
            if (ok)
                roots.push_back(candidate);
            else
                LOG_ERROR(Mission, "--arma{} '{}' holds no Arma archives (AddOns, Campaigns, Missions or PBOs)",
                          generation, item);
        }
    }
    return roots;
}

// The first root: the install the Dta folder, Arma 2's sibling folders and the log line refer to.
const fs::path& RootPath()
{
    static const fs::path none;
    return Roots().empty() ? none : Roots().front();
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

// Every folder Arma 2 content is read from: --arma2, --arma2oa, and, next to the
// --arma2 folder, any "Arma 2 Operation Arrowhead*" folder (Steam installs OA and each
// of its DLCs -- BAF, PMC, ACR -- into a folder of its own).
const std::vector<fs::path>& Arma2Roots()
{
    static std::vector<fs::path> roots;
    static bool resolved = false;
    if (resolved)
        return roots;
    resolved = true;
    // Every --arma2 part (a data package's folders), then the Arrowhead folders.
    roots = Roots();
    if (!OaRootPath().empty() && std::find(roots.begin(), roots.end(), OaRootPath()) == roots.end())
        roots.push_back(OaRootPath());
    // OP_A2_ONLY=1: plain Arma 2, without the Operation Arrowhead folders next to it.
    if (const char* only = std::getenv("OP_A2_ONLY"); only && *only == '1')
        return roots;
    std::error_code ec;
    const fs::path parent = RootPath().parent_path();
    std::vector<fs::path> siblings;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        const std::string name = Lower(it->path().filename().string());
        if (name.rfind("arma 2 operation arrowhead", 0) == 0 || name.rfind("arma2oa", 0) == 0)
            siblings.push_back(it->path());
    }
    std::sort(siblings.begin(), siblings.end());
    for (const fs::path& s : siblings)
        if (std::find(roots.begin(), roots.end(), s) == roots.end())
            roots.push_back(s);
    return roots;
}

// The archive folders of one Arma 2 root: AddOns, Expansion\AddOns and <DLC>\AddOns.
// The Steam DLC payloads under DLCsetup\<DLC>\<DLC>_data are not read: their archives
// are copy-protected ("hprotect") and only the original game unlocks them.
std::vector<fs::path> Arma2AddonFolders(const fs::path& r)
{
    // A data package part keeps its PBOs at its top level (ALDP_A2_PBOs_*); an install in AddOns.
    std::vector<fs::path> out{r, r / "AddOns"};
    for (const char* dlc : {"Expansion", "BAF", "PMC", "ACR"})
        out.push_back(r / dlc / "AddOns");
    return out;
}

// Arma 3: <root>\Addons plus every DLC folder beside it that holds an Addons folder of
// its own (Expansion = Apex, Heli, Mark, Jets, Orange = Laws of War, Argo = Malden, Tacops,
// Tank, Enoch + Contact, AoW, Kart, Curator = Zeus, ...). Steam installs them all into the
// Arma 3 folder. Creator DLCs (GM, EF, RF, WS, ...) ship encrypted .ebo archives only the
// original game opens: those are counted and logged, never read. Workshop and @mod folders
// are not Arma 3's own content and are left alone.
std::vector<fs::path> Arma3AddonFolders(const fs::path& r)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(r / "Addons", ec))
        out.push_back(r / "Addons");
    std::vector<fs::path> dlcs;
    for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        const std::string name = Lower(it->path().filename().string());
        if (name.empty() || name[0] == '@' || name[0] == '!' || name == "addons" || name == "dta" || name == "keys" ||
            name == "missions" || name == "mpmissions" || name == "battleye" || name == "launcher" || name == "dll" ||
            name == "logs" || name == "reshade-shaders")
            continue;
        for (const char* sub : {"Addons", "addons"})
            if (fs::is_directory(it->path() / sub, ec))
            {
                dlcs.push_back(it->path() / sub);
                break;
            }
    }
    std::sort(dlcs.begin(), dlcs.end());
    out.insert(out.end(), dlcs.begin(), dlcs.end());
    return out;
}

// Encrypted archives (.ebo) in an Arma 3 folder: reported once, never opened.
void ReportEncryptedArchives(const fs::path& folder)
{
    std::error_code ec;
    int count = 0;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".ebo")
            count++;
    if (count > 0)
        LOG_INFO(Mission, "Arma 3: {} holds {} encrypted .ebo archives (Creator DLC); they are not read",
                 folder.string(), count);
}

// `missionRoot`: single/multiplayer mission archives declare only "M09ConvoyAttack"
// as their prefix, while the mission loader needs the world suffix that is in the
// FILE name ("m09convoyattack.sara"). They are mounted as <missionRoot>\<file stem>\.
// Whether one of the --arma1 roots is the Cold War Rearmed mod folder (@CWR).
bool CwrModMounted()
{
    for (const fs::path& r : Roots())
        if (Lower(r.filename().string()).rfind("@cwr", 0) == 0)
            return true;
    return false;
}

void MountFolder(const fs::path& folder, bool missionBanks, const char* remountPrefix = nullptr,
                 const char* missionRoot = nullptr)
{
    std::error_code ec;
    if (!fs::is_directory(folder, ec))
        return;
    std::vector<fs::path> pbos;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it.depth() >= (missionBanks ? 1 : 0))
            it.disable_recursion_pending();
        if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".pbo")
            continue;
        // CWR_* are the bundled Cold War Rearmed conversions: they need the @CWR mod, so
        // they are offered only when it is mounted (--arma1 "<Arma 1>;<Arma 1>\@CWR", as
        // Arma 1's own cwr.bat starts the game with -mod=@CWR).
        if (Lower(it->path().filename().string()).rfind("cwr_", 0) == 0 && !CwrModMounted())
            continue;
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
            QFBank& bank = GFileBanks[GFileBanks.Size() - 1];
            // Copy-protected archives (DLC payloads only the original game unlocks) are
            // left alone.
            if (bank.GetProperty(RString("hprotect")).GetLength() > 0)
            {
                LOG_INFO(Mission, "{}: {} is copy-protected; not read", Label(), pbo.string());
                GFileBanks.Delete(GFileBanks.Size() - 1);
                continue;
            }
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
        {"m136", {"AT4Launcher", "LAWLauncher"}}, {"smaw", {"CarlGustavLauncher", "LAWLauncher"}},
        {"javelin", {"CarlGustavLauncher", "LAWLauncher"}},
        {"stinger", {"AALauncher"}}, {"strela", {"9K32Launcher", "AALauncher"}},
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
        // G36 rounds on CWA's own G36a (its magazine, by the weapon-carrier rule in
        // ResolveMagazineClass): on M16 rounds the G36a stayed empty -- two modes, the
        // G36a muzzle has three. The G36K and G36C on M16 take them through their listing.
        {"30rnd_556x45_g36", {"G36a", "M16"}}, {"30rnd_556x45_g36sd", {"G36a", "M16"}},
        {"200rnd_556x45_m249", {"M60"}},    {"100rnd_762x51_m240", {"M60"}},
        {"5rnd_762x51_m24", {"M21"}},       {"10rnd_127x99_m107", {"M21"}},
        {"30rnd_9x19_mp5", {"HK", "M16"}},  {"30rnd_9x19_mp5sd", {"HK", "M16"}},
        {"15rnd_9x19_m9", {"Beretta", "Revolver"}}, {"15rnd_9x19_m9sd", {"Beretta", "Revolver"}},
        {"7rnd_45acp_1911", {"Revolver"}},  {"8rnd_9x18_makarov", {"Tokarev", "Revolver"}},
        {"8rnd_9x18_makarovsd", {"Tokarev", "Revolver"}}, {"5rnd_127x108_ksvk", {"SVDDragunov", "M21"}},
        {"30rnd_545x39_ak", {"AK74"}},      {"30rnd_762x39_ak47", {"AK47"}},
        {"10rnd_762x54_svd", {"SVDDragunov"}}, {"100rnd_762x54_pk", {"PK"}},
        {"m136", {"AT4Launcher", "LAWLauncher"}}, {"smaw_he", {"CarlGustavLauncher", "LAWLauncher"}},
        {"smaw_hedp", {"CarlGustavLauncher", "LAWLauncher"}},
        {"javelin", {"CarlGustavLauncher", "LAWLauncher"}},
        {"stinger", {"AALauncher"}},        {"strela", {"9K32Launcher", "AALauncher"}},
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

// weaponModels: a resolved weapon gets the A1x_ class with the Arma model; a magazine
// never does (its name must stay the CWA magazine the weapon's magazines[] lists).
RString ResolveFrom(const char* configClass, const RString& name,
                    const std::map<std::string, std::vector<const char*>>& table, bool warn = true,
                    bool weaponModels = false)
{
    if (!Active() || name.GetLength() == 0)
        return name;
    const ParamEntry* config = Pars.FindEntry(configClass);
    if (!config)
        return name;
    if (config->FindEntry(name))
    {
        // A weapon both games name alike (AK74, PK, M16A2 ...): CWA's class keeps the
        // magazines and ballistics, the Arma model is drawn on it like for any other.
        // Arma 2 hand weapons named like CWA's (PK, Strela, Stinger) take the Arma 2 model too.
        if ((weaponModels && Generation() >= 1 && _strnicmp(name.Data(), "A1x_", 4) != 0) ||
            (Generation() == 2 && strcmp(configClass, "CfgWeapons") == 0 && Arma2IsHandWeapon(name)))
            return Arma2WeaponModelClass(name, name);
        return name;
    }
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
                if (weaponModels && Generation() >= 1)
                    return Arma2WeaponModelClass(name, RString(candidate));
                return RString(candidate);
            }
        }
    }
    // Arma 1 and 2 name hundreds of weapons and magazines; the ones not in the table are
    // carried by the CWA weapon of the same kind and calibre (Arma2WeaponFallback).
    if (Generation() >= 1 && strcmp(configClass, "CfgWeapons") == 0)
    {
        const RString fallback = Arma2WeaponFallback(name);
        if (fallback.GetLength() > 0 && config->FindEntry(fallback))
        {
            if (reported.insert(std::string(configClass) + key).second)
                LOG_INFO(Mission, "{}: {} '{}' is carried by CWA '{}' (by kind and calibre)", Label(), configClass,
                         name.Data(), fallback.Data());
            return weaponModels ? Arma2WeaponModelClass(name, fallback) : fallback;
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
    return Generation() == 3 ? "Arma 3" : Generation() == 2 ? "Arma 2" : "Arma 1";
}

std::vector<std::string> DtaFolders()
{
    std::vector<std::string> out;
    if (!Active())
        return out;
    out.push_back((RootPath() / "Dta").string());
    if (Generation() == 3)
    {
        // Arma 3's DLC folders keep no Dta of their own; Contact adds one.
        std::error_code ec;
        if (fs::is_directory(RootPath() / "Contact" / "Dta", ec))
            out.push_back((RootPath() / "Contact" / "Dta").string());
    }
    if (Generation() == 2)
    {
        for (const fs::path& r : Arma2Roots())
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
    ::Arma2StartStallMonitor();
    MissionBankPrefixes().clear();
    AllBankPrefixesMutable().clear();
    const fs::path root = RootPath();
    // Dta\bin.pbo / core.pbo are NOT mounted: their declared prefixes ("bin", "core")
    // would shadow CWA's own UI resources. Arma1Config reads bin.pbo privately.
    GameState::s_singleQuoteStrings = Generation() >= 2;
    // Arma 3: {...} is code (typeName "CODE"), and the function library compiles on use.
    GameState::s_codeValues = Generation() == 3;
    if (Generation() == 3)
    {
        ::Arma3RegisterScriptCommands();
        ::Arma3InstallFunctionLibrary();
    }
    GameState::s_emptyStatementKeepsValue = true;
    if (Generation() == 3)
    {
        // Arma 3: every scenario, showcase, campaign and challenge sits in
        // Addons\missions_f*.pbo (and the DLC folders' own); the player's missions in
        // Missions / MPMissions are archives of their own.
        for (const fs::path& folder : Arma3AddonFolders(root))
        {
            ReportEncryptedArchives(folder);
            MountFolder(folder, false);
        }
        MountFolder(root / "Missions", true, nullptr, "arma3\\missions");
        MountFolder(root / "MPMissions", true, nullptr, "arma3\\mpmissions");
    }
    else if (Generation() == 2)
    {
        // Arma 2 keeps its campaigns and scenarios inside AddOns\missions*.pbo. Operation
        // Arrowhead and its DLCs add folders of their own; later mounts are searched first.
        const std::vector<fs::path>& roots = Arma2Roots();
        for (const fs::path& r : roots)
            for (const fs::path& folder : Arma2AddonFolders(r))
                MountFolder(folder, false);
        for (const fs::path& r : roots)
        {
            MountFolder(r / "Missions", true, nullptr, "arma2\\missions");
            MountFolder(r / "MPMissions", true, nullptr, "arma2\\mpmissions");
        }
    }
    else
    {
        for (const fs::path& r : Roots())
        {
            MountFolder(r, false); // a data package's add-on PBOs lie at its top level
            MountFolder(r / "AddOns", false);
            MountFolder(r / "DBE1" / "AddOns", false);
            MountFolder(r / "Campaigns", true);
            MountFolder(r / "DBE1" / "Campaigns", true);
            MountFolder(r / "Missions", true, nullptr, "arma1\\missions");
            MountFolder(r / "MPMissions", true, nullptr, "arma1\\mpmissions");
        }
    }
    std::string from = root.string();
    if (Generation() == 2)
        for (size_t i = 1; i < Arma2Roots().size(); i++)
            from += " + " + Arma2Roots()[i].string();
    else
        for (size_t i = 1; i < Roots().size(); i++)
            from += " + " + Roots()[i].string();
    LOG_INFO(Mission, "{}: mounted {} archives read-only from {} ({} mission archives)", Label(),
             AllBankPrefixesMutable().size(), from, MissionBankPrefixes().size());
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
        // Arma 3: where the world lies, for the sun and the stars (Eden's are Everon's)
        bool located = false;
        float latitude = 0.0f;
        float longitude = 0.0f;
    };
    std::vector<World> worlds;
    if (Generation() >= 2)
    {
        // Arma 2 / OA / Arma 3: every CfgWorlds class of the Arma 2 config whose world file is
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
                    name = (Generation() == 3 ? "A3_" : "A2_") + name;
            }
            std::string description = std::string(Label()) + " / " + (const char*)w.GetName();
            World world{name, wrp, description};
            const ParamEntry* lat = w.FindEntry("latitude");
            const ParamEntry* lon = w.FindEntry("longitude");
            if (Generation() >= 3 && lat && lon && !lat->IsClass() && !lat->IsArray() && !lon->IsClass() &&
                !lon->IsArray())
            {
                world.located = true;
                world.latitude = static_cast<float>(*lat);
                world.longitude = static_cast<float>(*lon);
            }
            worlds.push_back(world);
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
        // Stratis and Altis lie at 35 N, Tanoa at 18 S (Arma's sign: negative is north);
        // left at Eden's, a Tanoa dawn came up with Everon's sun.
        if (world.located)
        {
            cls->Add("latitude", world.latitude);
            cls->Add("longitude", world.longitude);
        }
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

// Whether an Arma 2 vehicle class mounts a gun: any turret under Turrets with a weapon that
// is more than a horn or a smoke launcher.
static bool Arma2HasTurretWeapon(const RString& className)
{
    const ParamEntry* cls = FindConfigClass("CfgVehicles", className.Data());
    const ParamEntry* turrets = cls ? cls->FindEntry("Turrets") : nullptr;
    if (!turrets || !turrets->IsClass())
        return false;
    for (int t = 0; t < turrets->GetEntryCount(); t++)
    {
        const ParamEntry& turret = turrets->GetEntry(t);
        if (!turret.IsClass())
            continue;
        const ParamEntry* weapons = turret.FindEntry("weapons");
        if (!weapons || !weapons->IsArray())
            continue;
        for (int i = 0; i < weapons->GetSize(); i++)
        {
            const std::string w = Lower(((RString)(RStringB)(*weapons)[i]).Data());
            if (!w.empty() && w.find("horn") == std::string::npos && w.find("smoke") == std::string::npos)
                return true;
        }
    }
    return false;
}

RString ResolveVehicleClass(const RString& className)
{
    if (!Active() || className.GetLength() == 0)
        return className;
    // A class generated earlier for this mission keeps resolving to itself.
    if (_strnicmp(className.Data(), "A1x_", 4) == 0)
        return className;
    // Animals (Arma 2's sheep, cows, goats, dogs; Arma 3's Animal_Base_F: goats, sheep,
    // dogs, snakes, rabbits, fish) derive from Man; carried by a CWA class they became
    // walking civilians in the fields. Until animal models and moves are in, they are left
    // out -- Arma 3's first, then Arma 2's.
    // Arma 3's are now placed with their own model as a still object (a static CWA thing
    // carries it, so it does not walk the fields as a civilian); their moves are not in yet.
    if (Arma3IsAnimal(className))
    {
        const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
        const char* still = vehicles && vehicles->FindEntry("House") ? "House" : nullptr;
        static std::set<std::string> reported;
        const bool first = reported.insert(Lower(className.Data())).second;
        if (!still)
        {
            if (first)
                LOG_INFO(Mission, "{}: animal '{}' left out (no animal models yet)", Label(), className.Data());
            return RString();
        }
        if (first)
            LOG_INFO(Mission, "{}: animal '{}' stands still with its own model on CWA '{}' (no animal moves yet)",
                     Label(), className.Data(), still);
        return Arma1ModelClass(className, RString(still));
    }
    if (Generation() == 2)
    {
        const ParamEntry* cls = FindConfigClass("CfgVehicles", className.Data());
        const ParamEntry* animal = FindConfigClass("CfgVehicles", "CAAnimalBase");
        const ParamClass* c = cls ? cls->GetClassInterface() : nullptr;
        const ParamClass* a = animal ? animal->GetClassInterface() : nullptr;
        if (c && a && (c == a || c->IsDerivedFrom(*a)))
        {
            static std::set<std::string> reported;
            if (reported.insert(Lower(className.Data())).second)
                LOG_INFO(Mission, "{}: animal '{}' left out (no animal models yet)", Label(), className.Data());
            return RString();
        }
    }
    RString cwa = ResolveFrom("CfgVehicles", className, VehicleAliases(), false);
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    if (Generation() == 3 && vehicles && !vehicles->FindEntry(cwa))
    {
        // Arma 3: by the class's kind in the Arma 3 config (tank, APC, truck, attack
        // helicopter, ...); men and things go on to the generic fallback below.
        if (const RString carrier = Arma3VehicleCarrier(className); carrier.GetLength() > 0)
        {
            static std::set<std::string> reported;
            if (reported.insert(Lower(className.Data())).second)
                LOG_INFO(Mission, "{}: CfgVehicles '{}' is carried by CWA '{}' (by its Arma 3 kind)", Label(),
                         className.Data(), carrier.Data());
            cwa = carrier;
        }
    }
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
                sim = SimKind((const char*)(RStringB)*e);
        const bool vehicle = sim == "car" || sim == "tank" || sim == "helicopter" || sim == "airplane" ||
                             sim == "ship" || sim == "motorcycle" || sim == "bicycle" || sim == "parachute";
        // An armed Arma 2 car (HMMWV M2/Mk19/TOW, UAZ DShKM/AGS, Land Rover M2, pickups with
        // a PK) is carried by CWA's jeep with the mounted M2, not by the unarmed jeep of its
        // name: on the HMMWV the gunner had no seat and no gun (census: weapons=CarHorn).
        // These models keep OFP's turret selections (otocvez/otochlaven in the LODs,
        // osaveze/osahlavne/usti hlavne/konec hlavne in memory), which JeepMG animates.
        if (sim == "car" && vehicles->FindEntry("JeepMG") && Arma2HasTurretWeapon(className))
            cwa = RString("JeepMG");
        for (const auto& [word, candidates] : rules)
        {
            if (!vehicle || stricmp(cwa.Data(), "JeepMG") == 0)
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
    // Arma 1: an armed car (HMMWV M2, UAZ DShKM, PK pickups) rides CWA's armed jeep rather
    // than the unarmed jeep its name resolved to, so its gunner has a gun.
    if (Generation() == 1)
        cwa = Arma1ArmedCarCarrier(className, cwa);
    return Arma1ModelClass(className, cwa);
}

RString ResolveWeaponClass(const RString& className)
{
    return ResolveFrom("CfgWeapons", className, WeaponAliases(), true, true);
}

namespace
{
// CWA magazine (lower case) -> the first Arma magazine name resolved to it this session.
std::map<std::string, std::string>& ResolvedMagazineNames()
{
    static std::map<std::string, std::string> names;
    return names;
}
} // namespace

RString ResolveMagazineClass(const RString& className)
{
    // OFP/CWA keeps magazines in CfgWeapons as well.
    RString cwa = ResolveFrom("CfgWeapons", className, MagazineAliases());
    // A carrier that is a weapon with magazines of its own (CWA's Beretta and Tokarev take
    // BerettaMag and TokarevMag) is no magazine: its first magazine carries the round. A
    // magazine built on the pistol class itself had no ammunition mode, and once loaded
    // the AI's fire evaluation read a null mode (x05warreporter, 24175174).
    if (Active() && cwa.GetLength() > 0 && _stricmp(cwa.Data(), className.Data()) != 0)
        if (const ParamEntry* weapons = Pars.FindEntry("CfgWeapons"))
            if (const ParamEntry* carrier = weapons->FindEntry(cwa); carrier && carrier->IsClass())
                if (const ParamEntry* mags = carrier->FindEntry("magazines");
                    mags && mags->IsArray() && mags->GetSize() > 0)
                {
                    bool own = false;
                    for (int i = 0; i < mags->GetSize() && !own; i++)
                    {
                        const RString m = (RStringB)(*mags)[i];
                        own = _stricmp(m.Data(), "this") == 0 || _stricmp(m.Data(), cwa.Data()) == 0;
                    }
                    const RString first = (RStringB)(*mags)[0];
                    if (!own && weapons->FindEntry(first))
                        cwa = first;
                }
    if (Active() && cwa.GetLength() > 0 && className.GetLength() > 0 && _stricmp(cwa.Data(), className.Data()) != 0)
    {
        // An Arma 1 magazine of another size than its carrier gets a class of its own.
        const RString own = Arma1MagazineClass(className, cwa);
        if (_stricmp(own.Data(), cwa.Data()) != 0)
            return own;
        ResolvedMagazineNames().try_emplace(Lower(cwa.Data()), std::string(className.Data()));
    }
    return cwa;
}

RString ScriptClassName(const RString& engineName)
{
    if (!Active() || (_strnicmp(engineName.Data(), "A1x_", 4) != 0 && _strnicmp(engineName.Data(), "A1v_", 4) != 0))
        return engineName;
    return RString(engineName.Data() + 4);
}

RString ScriptMagazineName(const RString& unitClass, const RString& cwaMagazine)
{
    // A generated magazine is shown under its Arma name on any unit.
    if (Active() && _strnicmp(cwaMagazine.Data(), "A1x_", 4) == 0)
        return RString(cwaMagazine.Data() + 4);
    if (!Active() || cwaMagazine.GetLength() == 0 || _strnicmp(unitClass.Data(), "A1x_", 4) != 0)
        return cwaMagazine;
    // Per generated unit class: CWA magazine (lower case) -> the first Arma magazine of
    // that class's own magazines[], then of its weapons' magazines[], that resolves to it.
    static std::map<std::string, std::map<std::string, std::string>> cache;
    auto [it, inserted] = cache.try_emplace(Lower(unitClass.Data()));
    if (inserted)
    {
        std::map<std::string, std::string>& names = it->second;
        const auto addList = [&](const ParamEntry* list)
        {
            if (!list || !list->IsArray())
                return;
            for (int i = 0; i < list->GetSize(); i++)
            {
                const RString name = (RStringB)(*list)[i];
                if (name.GetLength() == 0 || _stricmp(name.Data(), "this") == 0)
                    continue;
                const RString cwa = ResolveFrom("CfgWeapons", name, MagazineAliases(), false);
                if (cwa.GetLength() > 0)
                    names.try_emplace(Lower(cwa.Data()), std::string(name.Data()));
            }
        };
        if (const ParamEntry* cls = FindConfigClass("CfgVehicles", unitClass.Data() + 4); cls && cls->IsClass())
        {
            addList(cls->FindEntry("magazines"));
            if (const ParamEntry* weapons = cls->FindEntry("weapons"); weapons && weapons->IsArray())
                for (int i = 0; i < weapons->GetSize(); i++)
                {
                    const RString weapon = (RStringB)(*weapons)[i];
                    if (const ParamEntry* w = FindConfigClass("CfgWeapons", weapon.Data()); w && w->IsClass())
                        addList(w->FindEntry("magazines"));
                }
        }
    }
    const std::string key = Lower(cwaMagazine.Data());
    if (const auto found = it->second.find(key); found != it->second.end())
        return RString(found->second.c_str());
    // A magazine outside the unit's own loadout (picked up, added by a script): the Arma
    // name it was first resolved from.
    if (const auto found = ResolvedMagazineNames().find(key); found != ResolvedMagazineNames().end())
        return RString(found->second.c_str());
    return cwaMagazine;
}

RString MagazineDisplayName(const RString& ownerClass, const RString& cwaMagazine)
{
    if (!Active() || cwaMagazine.GetLength() == 0)
        return RString();
    RString arma = ScriptMagazineName(ownerClass, cwaMagazine);
    if (_strnicmp(ownerClass.Data(), "A1x_", 4) != 0)
    {
        const auto found = ResolvedMagazineNames().find(Lower(cwaMagazine.Data()));
        if (found == ResolvedMagazineNames().end())
            return RString();
        arma = RString(found->second.c_str());
    }
    const ParamEntry* magazine = FindConfigClass("CfgMagazines", arma.Data());
    const ParamEntry* name = magazine && magazine->IsClass() ? magazine->FindEntry("displayName") : nullptr;
    if (!name || name->IsClass() || name->IsArray())
        return RString();
    const RString text = name->GetValue();
    if (text.GetLength() > 1 && (text[0] == '$' || text[0] == '@'))
        return Localize(text);
    return text;
}

bool ScriptWeaponMatches(const char* engineName, const RString& scriptName)
{
    if (!engineName || scriptName.GetLength() == 0)
        return false;
    if (_stricmp(engineName, scriptName.Data()) == 0)
        return true;
    if (!Active())
        return false;
    if ((_strnicmp(engineName, "A1x_", 4) == 0 || _strnicmp(engineName, "A1v_", 4) == 0) &&
        _stricmp(engineName + 4, scriptName.Data()) == 0)
        return true;
    // An Arma weapon carried by a CWA class without a model of its own ("M9" on CWA's
    // pistol): the script's name resolves to it. Muzzle names (M203Muzzle) are no
    // top-level weapons and are left alone, so this logs nothing for them.
    if (!FindConfigClass("CfgWeapons", scriptName.Data()))
        return false;
    const RString resolved = ResolveFrom("CfgWeapons", scriptName, WeaponAliases(), false, true);
    return _stricmp(engineName, resolved.Data()) == 0;
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

bool IsArmaBankPath(const char* name)
{
    if (!Active() || !name || !*name)
        return false;
    const char* slash = name;
    while (*slash && *slash != '\\' && *slash != '/')
        slash++;
    if (!*slash || slash == name)
        return false;
    static size_t knownBanks = 0;
    static std::set<std::string> tops;
    const auto& prefixes = AllBankPrefixesMutable();
    if (knownBanks != prefixes.size())
    {
        knownBanks = prefixes.size();
        tops.clear();
        for (const std::string& prefix : prefixes)
        {
            std::string p = Lower(prefix);
            const size_t cut = p.find_first_of("\\/");
            if (cut != std::string::npos && cut > 0)
                tops.insert(p.substr(0, cut));
        }
    }
    return tops.count(Lower(std::string(name, slash))) > 0;
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
    const std::string alias =
        (Generation() == 3 ? "A3_" : Generation() == 2 ? "A2_" : "A1_") + std::string(world.Data());
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
    if (Generation() >= 2 && missions != std::string::npos && missions > 0)
    {
        const std::string base = path.substr(0, missions + 1);
        if (QIFStreamB::FileExist((base + "description.ext").c_str()))
            return base;
    }
    return {};
}

namespace
{
std::map<std::string, std::string>& CampaignBases()
{
    static std::map<std::string, std::string> bases; // name -> folder
    return bases;
}
} // namespace

std::string CampaignName(const std::string& base)
{
    const std::string path = ForwardSlashes(base);
    if (Lower(path).rfind("campaigns/", 0) == 0) // Arma 1: campaigns/<name>/
        return path.size() > 11 ? path.substr(10, path.size() - 11) : std::string();
    std::string name = Lower(path);
    while (!name.empty() && name.back() == '/')
        name.pop_back();
    std::replace(name.begin(), name.end(), '/', '_');
    if (!name.empty())
        CampaignBases()[name] = path.back() == '/' ? path : path + "/";
    return name;
}

std::string CampaignBase(const std::string& name)
{
    const auto it = CampaignBases().find(Lower(name));
    return it != CampaignBases().end() ? it->second : std::string();
}

RString FindArmaSoundFile(const char* configName)
{
    if (!Active() || !configName)
        return RString();
    std::string name = Lower(configName);
    std::replace(name.begin(), name.end(), '/', '\\');
    const size_t start = name.find_first_not_of("@\\");
    if (start == std::string::npos)
        return RString();
    name.erase(0, start);
    const size_t slash = name.rfind('\\');
    const size_t dot = name.rfind('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string stem = hasExt ? name.substr(0, dot) : name;
    // The name as an archive path ("ca\sounds\ambient\forest\forest-day-2" + ".wss"), then
    // the same with .ogg for .wss.
    std::vector<std::string> candidates{hasExt ? name : stem + ".wss"};
    if (!hasExt || name.compare(dot, std::string::npos, ".wss") == 0)
        candidates.push_back(stem + ".ogg");
    for (const std::string& candidate : candidates)
    {
        if (!QIFStreamB::FileExist(candidate.c_str()))
            continue;
        static std::mutex reportedLock;
        static std::set<std::string> reported;
        std::lock_guard<std::mutex> lock(reportedLock);
        if (reported.size() < 64 && reported.insert(name).second)
            LOG_INFO(Mission, "{}: sound '{}' is read from {}", Label(), configName, candidate);
        return RString(candidate.c_str());
    }
    return RString();
}
} // namespace Poseidon::Arma1Compat
