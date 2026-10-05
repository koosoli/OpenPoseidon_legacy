// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/LocalMapWorlds.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <cjson/cJSON.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Poseidon
{
namespace
{
namespace fs = std::filesystem;
constexpr size_t MaxDescriptorBytes = 256 * 1024;
constexpr size_t MaxRoots = 128;
std::map<std::string, LocalMap> worlds;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string CanonicalVirtual(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
    return value;
}

std::string AliasKey(const std::string& value)
{
    std::string key = CanonicalVirtual(value);
    if (key.starts_with("worlds/") && key.ends_with(".wrp"))
        key = key.substr(7, key.size() - 11);
    return key.size() == 25 && key.starts_with("op_local_") &&
                   std::all_of(key.begin() + 9, key.end(), [](unsigned char c) { return std::isxdigit(c); })
               ? key
               : std::string();
}

bool PlainString(const std::string& value, size_t limit = 4096)
{
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32; });
}

bool VirtualPath(const std::string& value)
{
    if (!PlainString(value) || value.find_first_of(":;") != std::string::npos)
        return false;
    const std::string path = CanonicalVirtual(value);
    if (path.front() == '/' || path.back() == '/')
        return false;
    std::istringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/'))
        if (part.empty() || part == "." || part == "..")
            return false;
    return true;
}

bool MetadataValid(const LocalMap& map)
{
    if (!PlainString(map.game, 256) || !PlainString(map.name, 256) || !PlainString(map.worldPath) ||
        map.archiveRoots.size() > MaxRoots)
        return false;
    if (map.enfusion)
    {
        if (!VirtualPath(map.worldPath) || !CanonicalVirtual(map.worldPath).starts_with("worlds/") ||
            !PlainString(map.archiveRoot) || !fs::path(map.archiveRoot).is_absolute())
            return false;
    }
    else if (!LocalMapRevisionSupported(map.revision) || !CanonicalVirtual(map.worldPath).ends_with(".wrp") ||
             (!fs::path(map.worldPath).is_absolute() && !VirtualPath(map.worldPath)))
        return false;
    if (!map.archiveRoot.empty() && (!PlainString(map.archiveRoot) || !fs::path(map.archiveRoot).is_absolute() ||
                                     map.archiveRoot.find(';') != std::string::npos))
        return false;
    for (const std::string& root : map.archiveRoots)
        if (!PlainString(root) || !fs::path(root).is_absolute() || root.find(';') != std::string::npos)
            return false;
    return true;
}

std::string SourceIdentity(const LocalMap& map)
{
    // Labels and dependency scan order are deliberately excluded: renaming the browser
    // row or discovering another asset bank must not move existing mission folders.
    return std::string(map.enfusion ? "native\n" : "wrp\n") +
           CanonicalVirtual(fs::path(map.archiveRoot).lexically_normal().generic_string()) + "\n" +
           CanonicalVirtual(map.worldPath);
}

std::string StableAlias(const LocalMap& map)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : SourceIdentity(map))
        hash = (hash ^ c) * 1099511628211ull;
    std::ostringstream result;
    result << "OP_Local_" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return result.str();
}

fs::path DescriptorDirectory()
{
    return fs::path(Foundation::GamePaths::Instance().UserDir()) / "LocalMapWorlds";
}

bool ReadString(const cJSON* object, const char* name, std::string& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring)
        return false;
    out = item->valuestring;
    return true;
}

bool ReadDescriptor(const fs::path& path, LocalMap& map)
{
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes == 0 || bytes > MaxDescriptorBytes)
        return false;
    std::ifstream file(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad())
        return false;
    Json root(cJSON_ParseWithOpts(text.c_str(), nullptr, true), &cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()))
        return false;
    const cJSON* schema = cJSON_GetObjectItemCaseSensitive(root.get(), "schema");
    const cJSON* native = cJSON_GetObjectItemCaseSensitive(root.get(), "enfusion");
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(root.get(), "revision");
    const cJSON* roots = cJSON_GetObjectItemCaseSensitive(root.get(), "archiveRoots");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1 || !cJSON_IsBool(native) || !cJSON_IsNumber(revision) ||
        revision->valuedouble < 0 || revision->valuedouble > 1000 || revision->valuedouble != revision->valueint ||
        !cJSON_IsArray(roots) || cJSON_GetArraySize(roots) > static_cast<int>(MaxRoots) ||
        !ReadString(root.get(), "game", map.game) || !ReadString(root.get(), "name", map.name) ||
        !ReadString(root.get(), "worldPath", map.worldPath) || !ReadString(root.get(), "archiveRoot", map.archiveRoot))
        return false;
    map.enfusion = cJSON_IsTrue(native);
    map.revision = revision->valueint;
    map.supported = map.enfusion || LocalMapRevisionSupported(map.revision);
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, roots)
    {
        if (!cJSON_IsString(item) || !item->valuestring)
            return false;
        map.archiveRoots.emplace_back(item->valuestring);
    }
    return MetadataValid(map) && AliasKey(path.stem().string()) == AliasKey(StableAlias(map));
}

bool WriteDescriptor(const std::string& alias, const LocalMap& map, std::string& error)
{
    Json root(cJSON_CreateObject(), &cJSON_Delete);
    if (!root)
    {
        error = "Cannot allocate the local terrain descriptor.";
        return false;
    }
    cJSON* roots = cJSON_AddArrayToObject(root.get(), "archiveRoots");
    bool ok = roots && cJSON_AddNumberToObject(root.get(), "schema", 1) &&
              cJSON_AddStringToObject(root.get(), "game", map.game.c_str()) &&
              cJSON_AddStringToObject(root.get(), "name", map.name.c_str()) &&
              cJSON_AddStringToObject(root.get(), "worldPath", map.worldPath.c_str()) &&
              cJSON_AddStringToObject(root.get(), "archiveRoot", map.archiveRoot.c_str()) &&
              cJSON_AddNumberToObject(root.get(), "revision", map.revision) &&
              cJSON_AddBoolToObject(root.get(), "enfusion", map.enfusion);
    for (const std::string& path : map.archiveRoots)
        ok = ok && cJSON_AddItemToArray(roots, cJSON_CreateString(path.c_str()));
    std::unique_ptr<char, decltype(&cJSON_free)> json(ok ? cJSON_Print(root.get()) : nullptr, &cJSON_free);
    if (!json)
    {
        error = "Cannot serialize the local terrain descriptor.";
        return false;
    }
    std::error_code ec;
    const fs::path directory = DescriptorDirectory();
    fs::create_directories(directory, ec);
    const fs::path destination = directory / (alias + ".json");
    const fs::path temporary = directory / (alias + ".json.tmp");
    if (!ec)
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << json.get() << '\n';
        output.close();
        if (!output)
            ec = std::make_error_code(std::errc::io_error);
    }
    if (!ec)
    {
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
        fs::rename(temporary, destination, ec);
#endif
    }
    if (ec)
    {
        error = "Cannot save the local terrain descriptor: " + ec.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

void RegisterWorldClass(const std::string& alias, const LocalMap& map)
{
    ParamEntry* cfgWorlds = Pars.FindEntry("CfgWorlds");
    ParamEntry* cfgList = Pars.FindEntry("CfgWorldList");
    ParamEntry* eden = cfgWorlds ? cfgWorlds->FindEntry("Eden") : nullptr;
    if (!cfgWorlds || !cfgList || !eden || !eden->IsClass())
        return; // Can persist before config bring-up; Restore replays after Glob_Init.
    const std::string sentinel = "worlds\\" + alias + ".wrp";
    ParamEntry* existing = cfgWorlds->FindEntry(alias.c_str());
    if (existing)
    {
        const ParamEntry* owned = existing->FindEntry("opLocalMapWorld");
        if (!owned || static_cast<int>(*owned) != 1)
        {
            LOG_WARN(Config, "Local terrain '{}' conflicts with an existing config world", alias);
            return;
        }
    }
    // Config access modes constrain addons. Temporarily permit our own generated class,
    // then restore the parent mode; no foreign class is edited or imported.
    const ParamAccessMode worldAccess = cfgWorlds->GetAccessMode();
    cfgWorlds->SetAccessMode(PAReadAndWrite);
    ParamClass* cls = cfgWorlds->AddClass(alias.c_str());
    cfgWorlds->SetAccessMode(worldAccess);
    if (!cls)
        return;
    // Add/AddClass search inherited entries, so write our overrides before attaching
    // Eden. Otherwise Add("worldName", ...) edits Eden's entry rather than creating
    // an owned entry and redirects the original OFP terrain too.
    cls->SetBase(nullptr);
    cls->Add("opLocalMapWorld", 1);
    cls->Add("worldName", RString(sentinel.c_str()));
    cls->Add("description", RString((map.game + " / " + map.name).c_str()));
    if (ParamClass* names = cls->AddClass("Names"))
    {
        names->SetBase(nullptr);
        names->Clear(); // Eden's towns are not landmarks on the selected foreign terrain.
    }
    cls->SetBase(eden->GetClassInterface());
    const ParamAccessMode listAccess = cfgList->GetAccessMode();
    cfgList->SetAccessMode(PAReadAndWrite);
    cfgList->AddClass(alias.c_str());
    cfgList->SetAccessMode(listAccess);
}

bool Available(const LocalMap& map)
{
    std::error_code ec;
    if (map.enfusion)
        return fs::is_directory(map.archiveRoot, ec);
    if (fs::path(map.worldPath).is_absolute())
    {
        std::ifstream file(map.worldPath, std::ios::binary);
        char magic[4]{};
        return file.read(magic, sizeof(magic)) && std::string(magic, sizeof(magic)) == "OPRW";
    }
    if (map.archiveRoots.empty())
        return false;
    for (const std::string& root : map.archiveRoots)
        if (!fs::is_directory(root, ec))
            return false;
    return map.archiveRoot.empty() || fs::is_directory(map.archiveRoot, ec);
}
} // namespace

std::string RegisterLocalMapWorld(const LocalMap& source, std::string& error)
{
    error.clear();
    if (!source.supported || !Foundation::GamePaths::Instance().IsInitialized())
    {
        error = "The terrain is unsupported or user paths are not initialized.";
        return {};
    }
    LocalMap map = source;
    const auto absolute = [](const std::string& path)
    {
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(fs::absolute(path, ec), ec);
        return ec ? std::string() : canonical.string();
    };
    if (!map.archiveRoot.empty())
    {
        map.archiveRoot = absolute(map.archiveRoot);
        if (map.archiveRoot.empty())
        {
            error = "Cannot resolve the terrain archive folder.";
            return {};
        }
    }
    for (std::string& root : map.archiveRoots)
        root = absolute(root);
    if (!map.enfusion && fs::path(map.worldPath).is_absolute())
        map.worldPath = absolute(map.worldPath);
    else
        map.worldPath = CanonicalVirtual(map.worldPath);
    if (!MetadataValid(map) || !Available(map))
    {
        error = "The terrain source or its archive folders are unavailable or invalid.";
        return {};
    }
    const std::string alias = StableAlias(map);
    const auto found = worlds.find(AliasKey(alias));
    if (found != worlds.end() && SourceIdentity(found->second) != SourceIdentity(map))
    {
        error = "The terrain identifier conflicts with another saved terrain.";
        return {};
    }
    if (!WriteDescriptor(alias, map, error))
        return {};
    worlds.insert_or_assign(AliasKey(alias), map);
    RegisterWorldClass(alias, map);
    return alias;
}

void RestoreLocalMapWorldClasses()
{
    worlds.clear();
    if (!Foundation::GamePaths::Instance().IsInitialized())
        return;
    std::error_code ec;
    const fs::path directory = DescriptorDirectory();
    if (!fs::exists(directory, ec))
        return;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || CanonicalVirtual(it->path().extension().string()) != ".json")
            continue;
        LocalMap map;
        if (!ReadDescriptor(it->path(), map))
        {
            LOG_WARN(Config, "Ignoring invalid local terrain descriptor '{}'", it->path().string());
            continue;
        }
        const std::string alias = StableAlias(map);
        worlds.emplace(AliasKey(alias), map);
        RegisterWorldClass(alias, map);
    }
    if (ec)
        LOG_WARN(Config, "Cannot restore local terrain descriptors: {}", ec.message());
}

const LocalMap* FindLocalMapWorld(const std::string& worldOrSentinel)
{
    const auto found = worlds.find(AliasKey(worldOrSentinel));
    return found == worlds.end() ? nullptr : &found->second;
}

bool IsLocalMapWorldAvailable(const std::string& worldOrSentinel)
{
    const LocalMap* map = FindLocalMapWorld(worldOrSentinel);
    return map && Available(*map);
}

bool IsLocalMapWorldIdentifier(const std::string& worldOrSentinel)
{
    return !AliasKey(worldOrSentinel).empty();
}
} // namespace Poseidon
