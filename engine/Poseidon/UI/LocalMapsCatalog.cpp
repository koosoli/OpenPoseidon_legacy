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
#include <set>
#include <stdexcept>

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
             LocalMapsCatalog& result)
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
}
void ScanInstall(const fs::path& install, const std::string& game, LocalMapsCatalog& result)
{
    std::error_code ec;
    if (!fs::is_directory(install, ec))
        return;
    std::vector<fs::path> pbos, paks;
    std::set<std::string> rootSet;
    for (fs::recursive_directory_iterator it(install, fs::directory_options::skip_permission_denied, ec), end;
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
    const std::vector<std::string> roots(rootSet.begin(), rootSet.end());
    for (const auto& pbo : pbos)
    {
        try
        {
            ScanPbo(pbo, game, roots, result);
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
bool LocalMapRevisionSupported(int revision)
{
    return revision == 18 || revision == 20 || revision == 24 || revision == 25 || revision == 29;
}
LocalMapsCatalog ScanLocalMapsFolder(const std::string& folder)
{
    LocalMapsCatalog result;
    ScanInstall(fs::path(folder), "Local folder", result);
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
    constexpr Game games[] = {{"33910", "Arma 1"},  {"33900", "Arma 2"}, {"33930", "Arma 2 OA"},
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
                    ScanInstall(install, game.name, result);
                break;
            }
        }
    }
    if (!extraFolder.empty())
        ScanInstall(fs::path(extraFolder), "Local folder", result);
    std::sort(result.maps.begin(), result.maps.end(),
              [](const auto& a, const auto& b) { return a.game + a.name < b.game + b.name; });
    return result;
}
} // namespace Poseidon
