// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Compat/ArmaOriginalUi.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace Poseidon::ArmaOriginalUi
{
namespace
{
// The screens drawn from the Arma copy. Everything else (Options, ARMA CONTENT, the
// multiplayer and editor dialogs) keeps CWA's resource: their code expects CWA's controls.
constexpr const char* kScreens[] = {
    "RscInGameUI",          "RscDisplayMain",        "RscDisplayInterrupt",
    "RscDisplayMPInterrupt", "RscDisplayMainMap",     "RscDisplayGetReady",
    "RscDisplayServerGetReady", "RscDisplayClientGetReady", "RscDisplayDebriefing",
    "RscDisplayMissionEnd",
};
constexpr const char* kConfigs[] = {"CfgInGameUI"};

ParamFile& UiRes()
{
    static ParamFile file;
    return file;
}

ParamFile& UiCfg()
{
    static ParamFile file;
    return file;
}

bool Listed(const char* name, const char* const* begin, const char* const* end)
{
    for (const char* const* it = begin; it != end; ++it)
        if (_stricmp(*it, name) == 0)
            return true;
    return false;
}

// A copy of `source`'s own members (and its base) as class `name` of `parent`.
ParamClass* CopyClass(ParamClass& parent, const char* name, const ParamClass& source)
{
    ParamClass* copy = parent.AddClass(name);
    if (const ParamClass* base = source.GetBase())
        copy->SetBase(base);
    copy->Update(source);
    return copy;
}

// The CWA copies ListToClass made for Arma controls, with the Arma control each stands for.
std::vector<std::pair<ParamClass*, const ParamClass*>>& Copies()
{
    static std::vector<std::pair<ParamClass*, const ParamClass*>> copies;
    return copies;
}

// A copied CWA member is the copy's own, so it beat what Arma's base class says: Arma 1's mission
// list (MissionsList: RscListBox) kept CWA's `type` and became a 3D list box. Where the Arma
// control names a base that defines the member and does not set it itself, the base's value is
// Arma's: the copy gives way. The idc stays -- Arma's bases carry none worth having (-1).
int YieldToArmaBases()
{
    int removed = 0;
    for (const auto& [copy, arma] : Copies())
    {
        const ParamClass* base = copy->GetBase();
        if (!base || !arma->GetBaseName())
            continue;
        std::vector<RStringB> shadowing;
        for (int i = 0; i < copy->GetEntryCount(); i++)
        {
            const ParamEntry& e = copy->GetEntry(i);
            if (e.IsClass() || _stricmp(e.GetName(), "idc") == 0 || arma->FindEntryNoInheritance(e.GetName()))
                continue;
            if (base->FindEntry(e.GetName()))
                shadowing.push_back(e.GetName());
        }
        for (const RStringB& name : shadowing)
            copy->Delete(name);
        removed += static_cast<int>(shadowing.size());
    }
    Copies().clear();
    return removed;
}

// CWA's `controls[]={"Player",...}` names sibling classes of the display; Arma 1's `class
// controls { class Player: RscTextMainMenu {...}; }` nests them, and leaves out what its base
// config (absent here) supplied -- idc, type, model. Each nested class therefore starts as a copy
// of CWA's sibling of the same name, which Arma's members then patch. A list of 3D objects (the
// map's notepad, which also carries the briefing, the walkie-talkie...) keeps the CWA objects Arma
// does not list: the map code needs every one of them.
void ListToClass(ParamClass& display, const ParamEntry& list, const ParamClass& armaList)
{
    const RStringB listName = list.GetName();
    std::vector<RStringB> cwaNames;
    for (int k = 0; k < list.GetSize(); k++)
        cwaNames.push_back(list[k].GetValue());
    display.Delete(listName);
    ParamClass* nested = display.AddClass(listName);
    const auto sibling = [&](const RStringB& name) -> const ParamClass*
    {
        const ParamEntry* e = display.FindEntryNoInheritance(name);
        return e && e->IsClass() ? e->GetClassInterface() : nullptr;
    };
    for (int i = 0; i < armaList.GetEntryCount(); i++)
    {
        const ParamEntry& child = armaList.GetEntry(i);
        if (child.IsClass())
            if (const ParamClass* cwa = sibling(child.GetName()))
                Copies().emplace_back(CopyClass(*nested, child.GetName(), *cwa), child.GetClassInterface());
    }
    if (_stricmp(listName, "objects") == 0)
        for (const RStringB& name : cwaNames)
            if (!armaList.FindEntryNoInheritance(name))
                if (const ParamClass* cwa = sibling(name))
                    CopyClass(*nested, name, *cwa);
}

// Arma 1 lays the controls of a 3D map object (notepad, GPS) out in `class Areas`, 2D regions of
// its model; this engine draws them on named selections of the model (`papir`, `zalozka1`, ...),
// which Arma's models do not have -- the notepad came out as a blank sheet without the briefing.
// Such an object stays CWA's whole; Arma's watch and compass, plain models, are kept.
int RestoreAreaObjects(ParamFile& file)
{
    int restored = 0;
    for (const char* screen : kScreens)
    {
        ParamEntry* display = file.FindEntryNoInheritance(screen);
        ParamEntry* objects = display ? display->FindEntryNoInheritance("objects") : nullptr;
        if (!objects || !objects->IsClass())
            continue;
        std::vector<RStringB> names;
        for (int i = 0; i < objects->GetEntryCount(); i++)
        {
            const ParamEntry& object = objects->GetEntry(i);
            if (object.IsClass() && object.FindEntryNoInheritance("Areas"))
                names.push_back(object.GetName());
        }
        for (const RStringB& name : names)
        {
            const ParamEntry* cwa = display->FindEntryNoInheritance(name);
            if (!cwa || !cwa->IsClass())
                continue;
            objects->Delete(name);
            CopyClass(*objects->GetClassInterface(), name, *cwa->GetClassInterface());
            restored++;
        }
    }
    return restored;
}

// ParamClass::Update cannot turn an array into a class or back: CWA lists a display's
// controls as `controls[]={...}`, Arma 1 as `class controls {...}`. Arma's definition is the
// one being applied, so CWA's array or value gives way. A CWA class is never deleted (other
// classes may inherit it); that rare mismatch is reported and its member is not applied.
void Scrub(ParamClass& dst, const ParamClass& src, int& replaced, int& conflicts)
{
    for (int i = 0; i < src.GetEntryCount(); i++)
    {
        const ParamEntry& s = src.GetEntry(i);
        ParamEntry* d = dst.FindEntryNoInheritance(s.GetName());
        if (!d)
            continue;
        if (s.IsClass() && d->IsClass())
            Scrub(*d->GetClassInterface(), *s.GetClassInterface(), replaced, conflicts);
        else if (d->IsClass())
        {
            LOG_WARN(Mission, "Original UI: {} is a class in CWA's resource, not in the Arma one; kept CWA's",
                     (const char*)d->GetContext());
            conflicts++;
        }
        else if (s.IsClass() && d->IsArray())
        {
            ListToClass(dst, *d, *s.GetClassInterface());
            replaced++;
            // the copied CWA classes hold lists of their own (the notepad's paper controls)
            if (ParamEntry* nested = dst.FindEntryNoInheritance(s.GetName()))
                Scrub(*nested->GetClassInterface(), *s.GetClassInterface(), replaced, conflicts);
        }
        else if (s.IsClass() || s.IsArray() != d->IsArray())
        {
            dst.Delete(s.GetName());
            replaced++;
        }
    }
}

// Copies `source` (a class of the live CWA resource or config) into `target` through its text
// form, unlocked: CWA's resource classes carry access=3, which refuses every Update.
void CopyInto(ParamFile& target, const ParamEntry& source)
{
    QOStream out;
    source.Save(out, 0); // a ParamFile writes its members, a class writes itself
    QIStream in(out.str(), out.tellp());
    target.Parse(in);
    target.SetAccessModeForAll(PAReadAndWrite);
}

// Merges the Arma UI class `src` over the copy's class of the same name, resolving a base
// named by Arma in the copy (where Arma's own earlier classes have already been merged).
void MergeTop(ParamFile& file, const ParamClass& src, int& replaced, int& conflicts)
{
    ParamEntry* existing = file.FindEntryNoInheritance(src.GetName());
    if (existing && !existing->IsClass())
    {
        file.Delete(src.GetName());
        existing = nullptr;
    }
    ParamClass* dst = existing ? existing->GetClassInterface() : file.AddClass(src.GetName());
    if (const char* base = src.GetBaseName())
    {
        ParamEntry* baseEntry = file.FindEntryNoInheritance(base);
        if (baseEntry && baseEntry->IsClass() && baseEntry != dst)
            dst->SetBase(baseEntry->GetClassInterface());
    }
    Scrub(*dst, src, replaced, conflicts);
    dst->Update(src);
}

// The control a screen cannot work without: Quit on the main menu, Abort in the pause menu, Continue
// on the briefing. Arma 2 builds its menus from shortcut buttons (type 16), which this engine cannot
// create; such a screen would have nothing to press, so it stays CWA's. -1: no such control.
constexpr int kButton = 1, kActiveText = 11;
int KeyControl(const char* screen)
{
    if (_stricmp(screen, "RscDisplayMain") == 0)
        return 106; // IDC_MAIN_QUIT
    if (_stricmp(screen, "RscDisplayInterrupt") == 0 || _stricmp(screen, "RscDisplayMPInterrupt") == 0)
        return 104; // IDC_INT_ABORT
    if (_strnicmp(screen, "RscDisplay", 10) == 0 && strstr(screen, "GetReady"))
        return 1; // IDC_OK
    return -1;
}

// The type of the control with `idc` in a screen (class or list form, foreground or background), or
// -1 when the screen has none.
int ControlType(const ParamEntry& screen, int idc)
{
    for (const char* listName : {"controls", "controlsBackground"})
    {
        const ParamEntry* list = screen.FindEntry(listName);
        if (!list)
            continue;
        const auto check = [&](const ParamEntry* ctrl) -> int
        {
            if (!ctrl || !ctrl->IsClass())
                return -1;
            const ParamEntry* id = ctrl->FindEntry("idc");
            const ParamEntry* type = ctrl->FindEntry("type");
            return id && type && !id->IsClass() && !type->IsClass() && int(*id) == idc ? int(*type) : -1;
        };
        if (list->IsClass())
            for (int i = 0; i < list->GetEntryCount(); i++)
            {
                if (const int type = check(&list->GetEntry(i)); type >= 0)
                    return type;
            }
        else
            for (int i = 0; i < list->GetSize(); i++)
                if (const int type = check(screen.FindEntry((*list)[i].GetValue())); type >= 0)
                    return type;
    }
    return -1;
}

bool Built()
{
    static bool built = false;
    static bool tried = false;
    if (tried)
        return built;
    tried = true;
    Arma1Compat::LoadConfig();
    const ParamEntry* root = Arma1Compat::ConfigRoot();
    const ParamClass* arma = root ? root->GetClassInterface() : nullptr;
    if (!arma)
    {
        LOG_WARN(Mission, "Original UI: {} config not loaded; keeping CWA's HUD and menus", Arma1Compat::Label());
        return false;
    }

    CopyInto(UiRes(), Res);
    const int cwaClasses = UiRes().GetEntryCount();
    int merged = 0, replaced = 0, conflicts = 0;
    // Every Arma UI class (Rsc*, CA_*, ...), in Arma's order so a base is merged before the
    // classes built on it. Cfg* classes are configuration, not resource.
    for (int i = 0; i < arma->GetEntryCount(); i++)
    {
        const ParamEntry& e = arma->GetEntry(i);
        if (!e.IsClass() || _strnicmp(e.GetName(), "Cfg", 3) == 0 || e.GetEntryCount() == 0)
            continue;
        MergeTop(UiRes(), *e.GetClassInterface(), replaced, conflicts);
        merged++;
    }
    // Screens drawn whole from Arma's layout (ArmaContentMenu): CWA's 3D notebook (objects[]) has
    // no place in them -- it covered Arma 1's mission list and answered its idc 101 first.
    for (const char* screen : {"RscDisplaySingleMission", "RscDisplayCampaignSelect", "RscDisplayCampaignLoad"})
    {
        ParamEntry* merged = UiRes().FindEntryNoInheritance(screen);
        const ParamEntry* own = arma->FindEntryNoInheritance(screen);
        if (merged && own && merged->IsClass() && !own->FindEntry("objects") &&
            merged->FindEntryNoInheritance("objects"))
            merged->GetClassInterface()->Delete("objects");
    }
    const int yielded = YieldToArmaBases();
    LOG_INFO(Mission, "Original UI: {} copied CWA members gave way to Arma's base classes", yielded);
    const int areaObjects = RestoreAreaObjects(UiRes());
    if (areaObjects > 0)
        LOG_INFO(Mission, "Original UI: {} map objects laid out in Arma's Areas kept as CWA's", areaObjects);
    UiRes().SetFile(&UiRes());

    for (const char* name : kConfigs)
    {
        const ParamEntry* cwa = Pars.FindEntry(name);
        const ParamEntry* own = arma->FindEntryNoInheritance(name);
        if (!cwa || !cwa->IsClass())
            continue;
        CopyInto(UiCfg(), *cwa);
        if (own && own->IsClass())
            MergeTop(UiCfg(), *own->GetClassInterface(), replaced, conflicts);
    }
    UiCfg().SetFile(&UiCfg());

    // POSEIDON_ORIGINAL_UI_DUMP=<folder>: write the merged resource and config as text, to see
    // what a screen was actually built from.
    if (const char* dump = std::getenv("POSEIDON_ORIGINAL_UI_DUMP"); dump && *dump)
    {
        UiRes().Save((std::string(dump) + "\\original-ui-resource.cpp").c_str());
        UiCfg().Save((std::string(dump) + "\\original-ui-config.cpp").c_str());
        LOG_INFO(Mission, "Original UI: merged resource and config written to {}", dump);
    }

    int screens = 0;
    for (const char* name : kScreens)
        if (arma->FindEntryNoInheritance(name))
            screens++;
    LOG_INFO(Mission,
             "Original UI: {} UI merged over CWA's resource ({} CWA classes, {} Arma classes, {} CWA arrays "
             "replaced by Arma classes, {} conflicts kept); {} of {} screens are {}'s",
             Arma1Compat::Label(), cwaClasses, merged, replaced, conflicts, screens, std::size(kScreens),
             Arma1Compat::Label());
    built = true;
    return true;
}
} // namespace

bool Enabled()
{
    static const bool enabled = []
    {
        if (!Foundation::AppConfig::Instance().GetArmaOriginalUi())
            return false;
        if (!Arma1Compat::Active())
        {
            LOG_WARN(Mission, "Original UI: --arma-original-ui needs --arma1 or --arma2; ignored");
            return false;
        }
        if (Arma1Compat::Generation() > 2)
        {
            // its screens are drawn by Arma3Menu, from Arma 3's own config
            LOG_INFO(Mission, "Original UI: {}'s menus, pause menu, briefing, map and infantry HUD are its own "
                              "(Arma3Menu); its other in-game screens stay CWA's",
                     Arma1Compat::Label());
            return false;
        }
        return true;
    }();
    return enabled;
}

bool Switched(const char* name)
{
    if (!Enabled() || !Listed(name, std::begin(kScreens), std::end(kScreens)) || !Built())
        return false;
    const ParamEntry* screen = UiRes().FindEntryNoInheritance(name);
    if (!screen || !screen->IsClass())
        return false;
    static std::map<std::string, bool> usable;
    const auto [it, inserted] = usable.try_emplace(name, true);
    // Arma 2 rebuilt its menus, map and briefing (diary panels, shortcut buttons) around script
    // and controls this engine does not have; captured, its briefing came out as empty panels.
    // Only its HUD is drawn from it.
    if (inserted && Arma1Compat::Generation() >= 2 && _stricmp(name, "RscInGameUI") != 0)
    {
        it->second = false;
        LOG_INFO(Mission, "Original UI: {} stays CWA's under {} (only the HUD is switched there)", name,
                 Arma1Compat::Label());
    }
    else if (inserted)
        if (const int idc = KeyControl(name); idc >= 0)
        {
            const int type = ControlType(*screen, idc);
            if (type != kButton && type != kActiveText)
            {
                it->second = false;
                LOG_WARN(Mission, "Original UI: {}'s {} uses control type {} for idc {}, which this engine cannot "
                                  "create; that screen stays CWA's",
                         Arma1Compat::Label(), name, type, idc);
            }
        }
    return it->second;
}

const ParamEntry* NativeScreen(const char* name, int keyIdc)
{
    if (!Enabled() || !Built())
        return nullptr;
    const ParamEntry* screen = UiRes().FindEntryNoInheritance(name);
    if (!screen || !screen->IsClass())
        return nullptr;
    const int type = ControlType(*screen, keyIdc);
    if (type != kButton && type != kActiveText)
    {
        LOG_INFO(Mission, "Original UI: {}'s {} has no usable control {} (type {}); drawn plain", Arma1Compat::Label(),
                 name, keyIdc, type);
        return nullptr;
    }
    return screen;
}

const ParamEntry& Resource(const char* name)
{
    if (Switched(name))
        if (const ParamEntry* e = UiRes().FindEntryNoInheritance(name))
            return *e;
    return Res >> name;
}

const ParamEntry& Config(const char* name)
{
    if (Enabled() && Listed(name, std::begin(kConfigs), std::end(kConfigs)) && Built())
        if (const ParamEntry* e = UiCfg().FindEntryNoInheritance(name))
            return *e;
    return Pars >> name;
}
} // namespace Poseidon::ArmaOriginalUi
