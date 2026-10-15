// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Poseidon
{
namespace
{
namespace fs = std::filesystem;
std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::vector<std::string> Tokens(const std::string& source)
{
    std::vector<std::string> result;
    for (size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] != '"')
            continue;
        std::string token;
        for (++i; i < source.size() && source[i] != '"'; ++i)
        {
            if (source[i] == '\\' && i + 1 < source.size() && (source[i + 1] == '\\' || source[i + 1] == '"'))
                ++i;
            token += source[i];
        }
        result.push_back(std::move(token));
    }
    return result;
}
std::string ReadText(const fs::path& path)
{
    std::error_code ec;
    if (fs::file_size(path, ec) > 2 * 1024 * 1024 || ec)
        return {};
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
uint32_t ReadU32(std::istream& f)
{
    std::array<unsigned char, 4> b{};
    if (!f.read(reinterpret_cast<char*>(b.data()), 4))
        throw std::runtime_error("short archive header");
    return b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
std::string ReadString(std::istream& f)
{
    std::string s;
    for (size_t i = 0; i < 4096; ++i)
    {
        char c = 0;
        if (!f.get(c))
            throw std::runtime_error("short archive string");
        if (!c)
            return s;
        s += c;
    }
    throw std::runtime_error("oversize archive string");
}
void ScanPbo(const fs::path& path, const std::string& game, const std::vector<std::string>& roots,
             const std::string& missionRoot, LocalMapsCatalog& result)
{
    struct Entry
    {
        std::string path;
        uint32_t method, size;
        uint64_t offset;
    };
    std::ifstream f(path, std::ios::binary);
    std::string prefix = path.stem().string();
    std::vector<Entry> worlds;
    std::vector<std::string> missionFolders;
    uint64_t payload = 0;
    for (size_t count = 0; count < 100000; ++count)
    {
        std::string name = ReadString(f);
        const uint32_t method = ReadU32(f);
        ReadU32(f);
        ReadU32(f);
        ReadU32(f);
        const uint32_t size = ReadU32(f);
        if (name.empty() && method == 0x56657273)
        {
            for (size_t n = 0; n < 4096; ++n)
            {
                const auto key = ReadString(f);
                if (key.empty())
                    break;
                const auto value = ReadString(f);
                if (!value.empty() && Lower(key) == "prefix" && value.find("..") == std::string::npos &&
                    value.find(':') == std::string::npos)
                    prefix = value;
            }
            continue;
        }
        if (name.empty())
            break;
        if (name.find("..") == std::string::npos && name.find(':') == std::string::npos &&
            Lower(fs::path(name).extension().string()) == ".wrp")
            worlds.push_back({name, method, size, payload});
        if (name.find("..") == std::string::npos && name.find(':') == std::string::npos)
        {
            const std::string lower = Lower(name);
            if (lower == "mission.sqm")
                missionFolders.emplace_back(); // the archive itself is the mission folder
            else if (lower.size() > 12 && lower.ends_with("\\mission.sqm"))
                missionFolders.push_back(name.substr(0, name.size() - 12));
        }
        payload += size;
        if (count == 99999)
            throw std::runtime_error("too many archive entries");
    }
    const uint64_t start = static_cast<uint64_t>(f.tellg());
    std::error_code ec;
    const auto fileSize = fs::file_size(path, ec);
    if (ec || start > fileSize || payload > fileSize - start)
        throw std::runtime_error("invalid archive extents");
    for (const auto& entry : worlds)
    {
        LocalMap map;
        map.game = game;
        map.name = fs::path(entry.path).stem().string();
        map.archiveRoot = path.parent_path().string();
        map.archiveRoots = roots;
        map.worldPath = prefix + "\\" + entry.path;
        if (entry.method == 0 && entry.size >= 8)
        {
            f.seekg(static_cast<std::streamoff>(start + entry.offset));
            if (ReadU32(f) == 0x5752504f)
                map.revision = static_cast<int>(ReadU32(f)); // OPRW
            map.supported = LocalMapRevisionSupported(map.revision);
        }
        result.maps.push_back(std::move(map));
    }
    // Multiplayer missions need a session; the browser starts single-player only.
    // Editor templates are not missions either.
    const std::string parentDir = Lower(path.parent_path().filename().string());
    if (parentDir == "mpmissions" || parentDir == "templates" || parentDir == "mptemplates")
        return;
    // Only what a player starts: campaign and mission archives (Campaigns\, Missions\ and its
    // subfolders, an addon's missions bank). Add-ons also carry cutscenes and the main-menu
    // backgrounds as mission folders (ca\desert\data\scenes\intro.intro); those are not listed.
    const std::string grandDir = Lower(path.parent_path().parent_path().filename().string());
    const bool missionArchive = parentDir == "campaigns" || parentDir == "missions" || grandDir == "campaigns" ||
                                grandDir == "missions" || Lower(prefix).find("missions") != std::string::npos;
    for (const std::string& folder : missionFolders)
    {
        if (!missionArchive && Lower(folder).find("missions") == std::string::npos)
            continue;
        LocalMission mission;
        mission.game = game;
        // Arma1Compat mounts a single-mission archive with a bare declared prefix under its file name.
        const bool bareSingle = folder.empty() && prefix.find_first_of("\\/.") == std::string::npos &&
                                path.stem().has_extension();
        mission.virtualPath = bareSingle ? path.stem().string() : folder.empty() ? prefix : prefix + "\\" + folder;
        // A single-mission archive (Missions\m02minerconvoy.sara.pbo) may declare a prefix without
        // the terrain extension ("M02MinerConvoy"); its file name always carries it.
        mission.name = folder.empty() ? path.stem().string() : fs::path(folder).filename().string();
        mission.world = Lower(fs::path(mission.name).extension().string());
        if (!mission.world.empty())
            mission.world.erase(0, 1);
        // The campaign (campaigns\ca\missions\x -> "ca"), the folder a single-mission archive lies
        // in ("Missions", "tutorial"), or else the archive the missions ship in.
        mission.collection = Lower(prefix).starts_with("campaigns\\") ? prefix.substr(10)
                             : folder.empty()                         ? path.parent_path().filename().string()
                                                                      : path.stem().string();
        // Campaigns get a group of their own, named, ahead of the archive's other missions: Arma 2
        // and 3 keep theirs inside add-on archives beside scenarios and MP templates.
        if (const std::string campaign = LocalMissionCampaign(mission.virtualPath); !campaign.empty())
            mission.collection = campaign;
        mission.installRoot = missionRoot;
        mission.supported = LocalMissionSupported(game, mission.world, mission.reason);
        result.missions.push_back(std::move(mission));
    }
}
// One game's content, possibly split over several folders (a licensed data package ships one
// game as several ALDP_* parts): maps see every part's archive folders as dependencies, and a
// mission is started with all parts mounted (missionRoot = the parts joined with ';').
void ScanInstall(const std::vector<fs::path>& installs, const std::string& game, LocalMapsCatalog& result)
{
    std::error_code ec;
    if (installs.empty())
        return;
    const fs::path& install = installs.front();
    std::string missionRoot;
    std::vector<fs::path> pbos, paks;
    std::set<std::string> rootSet;
    for (const fs::path& part : installs)
    {
        if (!fs::is_directory(part, ec))
            continue;
        missionRoot += (missionRoot.empty() ? "" : ";") + part.string();
        for (fs::recursive_directory_iterator it(part, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (it.depth() >= 4)
                it.disable_recursion_pending();
            if (!it->is_regular_file(ec))
                continue;
            const auto extension = Lower(it->path().extension().string());
            if (extension == ".pbo")
            {
                pbos.push_back(it->path());
                rootSet.insert(it->path().parent_path().string());
            }
            if (extension == ".pak")
                paks.push_back(it->path());
        }
        ec.clear();
    }
    const std::vector<std::string> roots(rootSet.begin(), rootSet.end());
    for (const auto& pbo : pbos)
    {
        try
        {
            ScanPbo(pbo, game, roots, missionRoot, result);
        }
        catch (const std::exception& e)
        {
            result.diagnostics.push_back(pbo.filename().string() + ": " + e.what());
        }
    }
    // Native PAK reader parses directories, not terrain/model payloads. No global mount is touched.
    std::set<std::string> seen;
    for (const auto& pak : paks)
    {
        Asset::Formats::Enfusion::PakArchive archive;
        if (!archive.Open(pak.string()))
        {
            result.diagnostics.push_back(pak.filename().string() + ": " + archive.Error());
            continue;
        }
        for (const auto& entry : archive.Entries())
        {
            if (!Lower(entry.path).starts_with("worlds/") ||
                Lower(fs::path(entry.path).extension().string()) != ".terr")
                continue;
            const auto slash = entry.path.find('/', 7);
            const std::string world = entry.path.substr(0, slash);
            if (!seen.insert(world).second)
                continue;
            LocalMap map;
            map.game = game;
            map.name = fs::path(world).filename().string();
            // Reforger calls Everon's asset directory Eden; show the island's name in the browser.
            if (Lower(world) == "worlds/eden")
                map.name = "Everon";
            map.worldPath = world;
            map.archiveRoot =
                fs::is_directory(install / "addons", ec) ? (install / "addons").string() : pak.parent_path().string();
            map.enfusion = true;
            map.supported =
                Lower(world) == "worlds/eden" || Lower(world) == "worlds/arland" || Lower(world) == "worlds/cain";
            result.maps.push_back(std::move(map));
        }
    }
}
} // namespace
bool LocalMissionSupported(const std::string& game, const std::string& world, std::string& reason)
{
    reason.clear();
    // Arma 2 / Operation Arrowhead missions are offered as an experiment (owner, 2026-10-13: "let me
    // select them; if they fail, they fail"): they start through --arma2 (MrDudeX-arma2's bridge),
    // whatever their terrain, and may not load or play correctly.
    if (game.rfind("Arma 2", 0) == 0)
    {
        reason = "experimental: Arma 2 missions may fail to start or play";
        return true;
    }
    // Arma 3 the same way (owner, 2026-10-15: "make them testable, even if the game crashes"):
    // started through --arma3 (MrDudeX-arma3's bridge) in their own process, so a mission that
    // fails or crashes takes only that process with it.
    if (game == "Arma 3")
    {
        reason = "experimental: Arma 3 missions may fail to start, play wrongly or crash";
        return true;
    }
    if (game != "Arma 1")
    {
        reason = "only Arma 1 missions are playable so far (Arma 2 and Arma 3 experimentally)";
        return false;
    }
    // The terrains Arma1Compat::RegisterWorldClasses generates a CfgWorlds class for.
    static const std::set<std::string> worlds = {"sara", "saralite", "sara_dbe1", "porto"};
    if (worlds.contains(Lower(world)))
        return true;
    reason = world.empty() ? "no terrain named" : "terrain '" + world + "' is not registered yet";
    return false;
}
std::string LocalMissionCampaign(const std::string& virtualPath)
{
    std::string path = Lower(virtualPath);
    std::replace(path.begin(), path.end(), '/', '\\');
    std::string key, fallback;
    if (path.rfind("campaigns\\", 0) == 0)
    {
        // Arma 1 (and OFP-style) campaigns: campaigns\<name>\missions\<mission>.
        const size_t end = path.find('\\', 10);
        fallback = path.substr(10, end == std::string::npos ? std::string::npos : end - 10);
        key = "campaigns\\" + fallback;
    }
    else
    {
        // Arma 2 / 3: <archive prefix>\campaign[_n]\missions|cutscenes\<mission>.
        std::vector<std::string> parts;
        for (size_t from = 0; from <= path.size();)
        {
            const size_t to = std::min(path.find('\\', from), path.size());
            parts.push_back(path.substr(from, to - from));
            from = to + 1;
        }
        for (size_t i = 1; i + 1 < parts.size(); ++i)
        {
            if (parts[i].rfind("campaign", 0) != 0 || (parts[i + 1] != "missions" && parts[i + 1] != "cutscenes"))
                continue;
            for (size_t j = 0; j <= i; ++j)
                key += (j ? "\\" : "") + parts[j];
            fallback = parts[i - 1] + (parts[i] == "campaign" ? "" : " " + parts[i].substr(8));
            break;
        }
    }
    if (key.empty())
        return {};
    // The official campaigns by their own names; anything else by its archive.
    static const std::map<std::string, std::string> names = {
        {R"(campaigns\ca)", "Armed Assault"},
        {R"(campaigns\ra)", "Rahmadi Conflict"},
        {R"(campaigns\merc)", "Queen's Gambit"},
        {R"(ca\missions\campaign)", "Harvest Red"},
        {R"(ca\missions_e\campaign)", "Operation Arrowhead"},
        {R"(ca\missions_ew\campaign)", "Eagle Wing"},
        {R"(ca\missions_baf\campaign)", "British Armed Forces"},
        {R"(ca\missions_pmc\campaign)", "Private Military Company"},
        {R"(a3\missions_f_bootcamp\campaign)", "Bootcamp"},
        {R"(a3\missions_f_epa\campaign)", "The East Wind"},
        {R"(a3\missions_f_exp\campaign)", "Apex Protocol"},
        {R"(a3\missions_f_oldman\campaign)", "Old Man"},
        {R"(a3\missions_f_orange\campaign)", "Laws of War: The Remnants of War"},
        {R"(a3\missions_f_tacops\campaign_1)", "Tac-Ops: Beyond Hope"},
        {R"(a3\missions_f_tacops\campaign_2)", "Tac-Ops: Stepping Stone"},
        {R"(a3\missions_f_tacops\campaign_3)", "Tac-Ops: Steel Pegasus"},
        {R"(a3\missions_f_tank\campaign)", "Tanks: Tank Destroyers"},
    };
    const auto known = names.find(key);
    return "Campaign: " + (known != names.end() ? known->second : fallback);
}
bool LocalMapRevisionSupported(int revision)
{
    return revision == 18 || revision == 20 || revision == 24 || revision == 25 || revision == 29;
}
// The game a folder holds, by its executable, so a copy outside Steam (or a folder typed into the
// browser) is labelled -- and its missions judged -- like the Steam install would be.
std::string DetectLocalGame(const std::string& folder, const std::string& fallback)
{
    struct Exe
    {
        const char* file;
        const char* game;
    };
    static constexpr Exe exes[] = {{"arma.exe", "Arma 1"},  {"arma2.exe", "Arma 2"},     {"arma2oa.exe", "Arma 2 OA"},
                                   {"arma3.exe", "Arma 3"}, {"arma3_x64.exe", "Arma 3"}, {"dayz_x64.exe", "DayZ"}};
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = Lower(it->path().filename().string());
        for (const Exe& exe : exes)
            if (name == exe.file)
                return exe.game;
    }
    return fallback;
}
namespace
{
// A folder the user points the browser at. Bohemia's licensed data packages
// (bohemia.net/en/licenses/licensed-data-packages) ship each game as several
// ALDP_<game>_PBOs_<licence>[_partN] folders with the archives one level down; each game's parts
// are scanned as ONE install. Anything else is one install, labelled by its executable.
void ScanFolder(const fs::path& folder, LocalMapsCatalog& result)
{
    struct Family
    {
        const char* prefix;
        const char* game;
    };
    static constexpr Family families[] = {
        {"aldp_a1_", "Arma 1"}, {"aldp_a2oadlc_", "Arma 2 OA"}, {"aldp_a2oa_", "Arma 2 OA"}, {"aldp_a2_", "Arma 2"}};
    std::map<std::string, std::vector<fs::path>> parts;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
            continue;
        const std::string name = Lower(it->path().filename().string());
        for (const Family& family : families)
        {
            if (!name.starts_with(family.prefix))
                continue;
            std::vector<fs::path> inner;
            std::error_code innerEc;
            for (fs::directory_iterator sub(it->path(), innerEc), subEnd; !innerEc && sub != subEnd;
                 sub.increment(innerEc))
                if (sub->is_directory(innerEc))
                    inner.push_back(sub->path());
            auto& list = parts[family.game];
            if (inner.empty())
                list.push_back(it->path());
            else
                list.insert(list.end(), inner.begin(), inner.end());
            break;
        }
    }
    if (parts.empty())
    {
        ScanInstall({folder}, DetectLocalGame(folder.string(), "Local folder"), result);
        return;
    }
    for (auto& [game, dirs] : parts)
    {
        std::sort(dirs.begin(), dirs.end());
        ScanInstall(dirs, game, result);
    }
}
} // namespace
LocalMapsCatalog ScanLocalMapsFolder(const std::string& folder)
{
    LocalMapsCatalog result;
    ScanFolder(fs::path(folder), result);
    return result;
}
std::vector<std::string> ParseSteamLibraryPaths(const std::string& vdf)
{
    const auto tokens = Tokens(vdf);
    std::vector<std::string> roots;
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        const bool oldKey = !tokens[i].empty() && std::all_of(tokens[i].begin(), tokens[i].end(),
                                                              [](unsigned char c) { return std::isdigit(c); });
        if (Lower(tokens[i]) == "path" ||
            (oldKey && (tokens[i + 1].find(':') != std::string::npos || tokens[i + 1].starts_with('/'))))
            roots.push_back(tokens[i + 1]);
    }
    return roots;
}
LocalMapsCatalog ScanLocalMaps(const std::string& extraFolder)
{
    LocalMapsCatalog result;
    std::vector<fs::path> libraries = {"C:/SteamLibrary", "D:/SteamLibrary", "C:/Program Files (x86)/Steam"};
    if (const char* steam = std::getenv("STEAM_PATH"))
        libraries.emplace_back(steam);
    // Iterate to include additional libraries discovered from each Steam root.
    std::set<std::string> scannedLibraries, installs;
    struct Game
    {
        const char* id;
        const char* name;
    };
    // Arma: Gold Edition is Steam app 65780 (store.steampowered.com/app/65780); 33910 was listed as
    // Arma 1 here and never matched an Arma 1 install.
    constexpr Game games[] = {{"65780", "Arma 1"},  {"33900", "Arma 2"}, {"33930", "Arma 2 OA"},
                              {"107410", "Arma 3"}, {"221100", "DayZ"},  {"1874880", "Arma Reforger"}};
    for (size_t i = 0; i < libraries.size() && i < 64; ++i)
    {
        std::error_code ec;
        const auto library = fs::weakly_canonical(libraries[i], ec);
        if (ec || !scannedLibraries.insert(Lower(library.string())).second)
            continue;
        const fs::path steamapps = Lower(library.filename().string()) == "steamapps" ? library : library / "steamapps";
        for (const auto& root : ParseSteamLibraryPaths(ReadText(steamapps / "libraryfolders.vdf")))
            libraries.emplace_back(root);
        for (const auto& game : games)
        {
            const auto tokens = Tokens(ReadText(steamapps / (std::string("appmanifest_") + game.id + ".acf")));
            for (size_t n = 0; n + 1 < tokens.size(); ++n)
            {
                if (Lower(tokens[n]) != "installdir")
                    continue;
                const auto install = fs::weakly_canonical(steamapps / "common" / tokens[n + 1], ec);
                if (!ec && installs.insert(Lower(install.string())).second)
                    ScanInstall({install}, game.name, result);
                break;
            }
        }
    }
    if (!extraFolder.empty())
        ScanFolder(fs::path(extraFolder), result);
    std::sort(result.maps.begin(), result.maps.end(),
              [](const auto& a, const auto& b) { return a.game + a.name < b.game + b.name; });
    // Playable first, then by game, collection and name.
    std::sort(result.missions.begin(), result.missions.end(),
              [](const auto& a, const auto& b)
              {
                  // ... and within a game its campaigns ahead of its other collections.
                  const bool ca = a.collection.rfind("Campaign: ", 0) == 0;
                  const bool cb = b.collection.rfind("Campaign: ", 0) == 0;
                  return std::tuple(!a.supported, a.game, !ca, Lower(a.collection), Lower(a.name)) <
                         std::tuple(!b.supported, b.game, !cb, Lower(b.collection), Lower(b.name));
              });
    return result;
}
} // namespace Poseidon
