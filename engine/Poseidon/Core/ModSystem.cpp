#include <Poseidon/Core/ModSystem.hpp>
#include <Poseidon/Core/Application.hpp> // For logging macros
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <cstring>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

// The mod-path global (GameState.cpp) consumed by EnumModDirectories for
// addon-bank loading and mission discovery. It is a separate store from s_modPath
// (which drives config/stringtable loading), so SetModPath must update both to keep
// an in-process re-mount loading the *current* mod set from scratch.
Poseidon::Foundation::RString& GetModPathInternal();

namespace Poseidon
{
ModCollection ModSystem::s_active;
ModCollection ModSystem::s_defaultContent;

void ModSystem::ConfigureDefaultContent(const RStringB& mountPath)
{
    s_defaultContent = ActiveModsFromMountPath((const char*)mountPath);
}

void ModSystem::SetModPath(const RStringB& modPath)
{
    const ModCollection requested = ActiveModsFromMountPath((const char*)modPath);
    s_active = {};
    // RES is original expansion content, below our shipped additions. All
    // explicitly requested non-RES mods remain above the default packs.
    if (!s_defaultContent.Empty())
    {
        if (const Mod* res = requested.Find("res"))
            s_active.Add(*res);
    }
    for (const Mod& mod : s_defaultContent.All())
    {
        if (!requested.Contains(mod.id))
            s_active.Add(mod);
    }
    for (const Mod& mod : requested.All())
    {
        if (s_defaultContent.Empty() || !s_active.Contains(mod.id))
            s_active.Add(mod);
    }
    // Keep the addon-bank loader's mod path in sync with the (normalized) mount paths.
    const std::string mountPath = s_active.FormatMountPath();
    ::GetModPathInternal() = mountPath.c_str();

    if (!mountPath.empty())
    {
        LOG_INFO(Core, "Mod path set: {}", mountPath.c_str());
        for (const Mod& mod : s_active.All())
        {
            LOG_INFO(Core, "Loaded mod: '{}' from '{}'", mod.id.c_str(), mod.path.c_str());
        }
    }
    else
    {
        LOG_DEBUG(Core, "Mod path cleared (using base game files only)");
    }
}

RStringB ModSystem::GetModList()
{
    return s_active.FormatMountPath().c_str();
}

RStringB ModSystem::GetModNames()
{
    return s_active.FormatNames().c_str();
}

bool ModSystem::EnumDirectories(ModDirectoryCallback callback, void* context)
{
    const std::vector<Mod>& mods = s_active.All();
    // Reverse order (last-listed mod's directory checked first), matching the original
    // strrchr tokenization, then always the base game directory ("").
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
    {
        LOG_DEBUG(Core, "  -> Checking mod directory: '{}'", it->path.c_str());
        if (callback(it->path.c_str(), context))
        {
            LOG_DEBUG(Core, "  -> Callback returned true, stopping enumeration");
            return true;
        }
    }

    LOG_DEBUG(Core, "  -> Checking base directory: ''");
    return callback("", context);
}
} // namespace Poseidon
