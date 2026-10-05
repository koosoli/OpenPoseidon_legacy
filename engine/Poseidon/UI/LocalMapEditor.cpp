// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/LocalMapEditor.hpp>
#include <Poseidon/UI/LocalMapWorlds.hpp>
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <Poseidon/UI/DisplayUICommon.hpp>
#include <Poseidon/UI/OptionsUICommon.hpp>
#include <Poseidon/UI/OptionsUI.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Common/Win.h>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
namespace Poseidon
{
namespace
{
std::string pendingWorld, originalRoots, originalWorld;
bool editing = false, restore = false;
} // namespace
bool RequestLocalMapEditor(const LocalMap& map, std::string& error)
{
    if (!GWorld || GWorld->GetMode() != GModeIntro || editing || !pendingWorld.empty())
    {
        error = "Return to the main menu before selecting a terrain.";
        return false;
    }
    const auto id = RegisterLocalMapWorld(map, error);
    if (id.empty())
        return false;
    auto& cfg = Foundation::AppConfig::Instance();
    originalRoots = cfg.GetMapArchivePaths().Data();
    originalWorld = cfg.GetLocalMapWorldId();
    std::string roots;
    for (const auto& root : map.archiveRoots)
    {
        if (!roots.empty())
            roots += ';';
        roots += root;
    }
    pendingWorld = id;
    GApp->RequestRemountWithMapArchives(roots, id);
    return true;
}
bool CloseLocalMapEditor()
{
    if (!editing)
        return false;
    editing = false;
    restore = true;
    return true;
}
bool ServiceLocalMapEditor()
{
    if (restore)
    {
        restore = false;
        // Between frames: no display callback remains on the stack.
        GWorld->StartIntro();
        GApp->RequestRemountWithMapArchives(originalRoots, originalWorld);
        return true;
    }
    if (pendingWorld.empty())
        return false;
    if (GApp->m_remountFailed)
    {
        pendingWorld.clear();
        return false;
    }
    if (!GWorld || GWorld->GetMode() != GModeIntro || !GWorld->Options())
        return false;
    auto* parent = dynamic_cast<ControlsContainer*>(GWorld->Options());
    if (!parent)
        return false;
    const auto world = pendingWorld;
    pendingWorld.clear();
    CurrentTemplate.Clear();
    CurrentCampaign = "";
    CurrentBattle = "";
    CurrentMission = "";
    SetBaseDirectory(true, GetUserMissionsBase());
    ::CreateDirectory(GetBaseDirectory() + RString("missions"), nullptr);
    SetMission(world.c_str(), "");
    GWorld->SwitchLandscape(GetWorldName(world.c_str()));
    CreateEditor(parent);
    editing = true;
    return true;
}
} // namespace Poseidon
