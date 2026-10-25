// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3's function library (CfgFunctions) and mission start.
//
// Every Arma 3 mission calls into the function library (BIS_fnc_*, and the BIN_fnc_* of
// Contact / Livonia): the tasks, the briefing, modules, respawn, conversations,
// cutscenes. Arma 3 compiles all of it when the game starts. Here the library is an index
// built from CfgFunctions -- of the Arma 3 config, the campaign's description.ext and the
// mission's, later ones winning -- and a function is read, preprocessed and compiled the
// first time a script names it (GameState::s_resolveGlobal). Its variable then holds the
// code like any other.
//
// The CfgFunctions layout, as Arma 3's data declares it:
//   class CfgFunctions { class <Tag class> { tag = "BIS"; file = "...";
//     class <Category> { file = "..."; class <name> { file = "..."; ext = ".sqf"|".fsm";
//       preInit = 1; postInit = 1; headerType = -1|0|1; }; }; }; };
//   variable  <tag>_fnc_<name>  (tag defaults to the tag class's name)
//   file      the function's own file, else <category file>\fn_<name><ext>, else
//             <tag file>\fn_<name><ext>, else (missions) functions\<category>\fn_<name><ext>
//   .fsm      a function that runs its FSM: _this execFSM <file>
//   header    0 (default): _fnc_scriptName / _fnc_scriptNameParent are set; 1: only the
//             parent; -1: nothing
//
// Mission start (Arma 3's order, single player): preInit functions (["preInit"] call),
// objects' init lines (done by the mission loader before this), then, scheduled one after
// the other: BIS_fnc_initModules, initServer.sqf, initPlayerLocal.sqf and
// initPlayerServer.sqf ([player, false]), the postInit functions (["postInit", false]),
// bis_fnc_init = true, init.sqf.
//
// Contact's own automatic functions (tag class "Contact", the optional Contact platform
// that Arma 3 loads only with -mod=contact) run only in Contact's missions.

#include <Poseidon/World/Simulation/DynamicSimulation.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Game/Commands/GameStateExtA3Rsc.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Poseidon;

bool PreprocessScriptFile(const char* name, std::string& out); // GameStateExtWorldConfig.cpp
namespace Poseidon
{
RString GetMissionDirectory();
}

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string StringEntry(const ParamEntry& cls, const char* key)
{
    const ParamEntry* e = cls.FindEntry(key);
    if (!e || e->IsClass() || e->IsArray())
        return {};
    return (const char*)e->GetValue();
}

int NumberEntry(const ParamEntry& cls, const char* key, int fallback)
{
    const ParamEntry* e = cls.FindEntry(key);
    if (!e || e->IsClass() || e->IsArray())
        return fallback;
    return toInt((float)*e);
}

enum class Origin
{
    Config,
    Campaign,
    Mission
};

struct Function
{
    std::string name; // as declared: BIS_fnc_taskCreate
    std::string path; // bank path, or relative to the mission / campaign directory
    std::string ext;
    std::string tagClass;
    std::string category;
    int header = 0;
    bool preInit = false;
    bool postInit = false;
    Origin origin = Origin::Config;
};

struct Library
{
    std::string missionDirectory; // the mission the index was built for
    std::string campaignDirectory;
    std::map<std::string, Function> byName; // lower-case variable name
    std::vector<std::string> preInit;        // lower-case names, in declaration order
    std::vector<std::string> postInit;
    std::unique_ptr<ParamFile> campaignConfig; // campaign description.ext, read here when needed
};

Library& Lib()
{
    static Library lib;
    return lib;
}

void IndexConfig(Library& lib, const ParamEntry* cfgFunctions, Origin origin, const std::string& baseDir)
{
    if (!cfgFunctions || !cfgFunctions->IsClass())
        return;
    for (int t = 0; t < cfgFunctions->GetEntryCount(); t++)
    {
        const ParamEntry& tagClass = cfgFunctions->GetEntry(t);
        if (!tagClass.IsClass())
            continue;
        std::string tag = StringEntry(tagClass, "tag");
        if (tag.empty())
            tag = (const char*)tagClass.GetName();
        const std::string tagFile = StringEntry(tagClass, "file");
        for (int c = 0; c < tagClass.GetEntryCount(); c++)
        {
            const ParamEntry& category = tagClass.GetEntry(c);
            if (!category.IsClass())
                continue;
            const std::string categoryFile = StringEntry(category, "file");
            for (int f = 0; f < category.GetEntryCount(); f++)
            {
                const ParamEntry& item = category.GetEntry(f);
                if (!item.IsClass())
                    continue;
                Function fn;
                const std::string itemName = (const char*)item.GetName();
                fn.name = tag + "_fnc_" + itemName;
                fn.ext = StringEntry(item, "ext");
                if (fn.ext.empty())
                    fn.ext = ".sqf";
                fn.path = StringEntry(item, "file");
                if (fn.path.empty() && !categoryFile.empty())
                    fn.path = categoryFile + "\\fn_" + itemName + fn.ext;
                if (fn.path.empty() && !tagFile.empty())
                    fn.path = tagFile + "\\fn_" + itemName + fn.ext;
                if (fn.path.empty())
                    fn.path = (origin == Origin::Config ? std::string() : std::string("functions\\")) +
                              (const char*)category.GetName() + "\\fn_" + itemName + fn.ext;
                if (origin != Origin::Config && !fn.path.empty() && fn.path[0] != '\\' && !baseDir.empty())
                {
                    // relative to the mission / campaign; a bank path names its archive
                    const std::string lowerPath = Lower(fn.path);
                    if (lowerPath.rfind("a3\\", 0) != 0 && lowerPath.rfind("ca\\", 0) != 0)
                        fn.path = baseDir + fn.path;
                }
                fn.header = NumberEntry(item, "headerType", 0);
                fn.preInit = NumberEntry(item, "preInit", 0) > 0;
                fn.postInit = NumberEntry(item, "postInit", 0) > 0;
                fn.tagClass = (const char*)tagClass.GetName();
                fn.category = (const char*)category.GetName();
                fn.origin = origin;
                const std::string key = Lower(fn.name);
                const bool known = lib.byName.count(key) > 0;
                // Arma 3 keeps the first config definition; a campaign's or mission's own wins
                if (known && origin == Origin::Config)
                    continue;
                if (!known)
                {
                    if (fn.preInit)
                        lib.preInit.push_back(key);
                    if (fn.postInit)
                        lib.postInit.push_back(key);
                }
                lib.byName[key] = std::move(fn);
            }
        }
    }
}

// (Re)builds the index when the mission changed.
Library& CurrentLibrary()
{
    Library& lib = Lib();
    const std::string mission = (const char*)GetMissionDirectory();
    if (!lib.byName.empty() && lib.missionDirectory == mission)
        return lib;
    lib.byName.clear();
    lib.preInit.clear();
    lib.postInit.clear();
    lib.missionDirectory = mission;
    lib.campaignDirectory = Arma1Compat::CampaignBaseForMission(mission + "mission.sqm");
    std::replace(lib.campaignDirectory.begin(), lib.campaignDirectory.end(), '/', '\\');
    if (const ParamEntry* root = Arma1Compat::ConfigRoot())
        IndexConfig(lib, root->FindEntry("CfgFunctions"), Origin::Config, std::string());
    // the campaign's description.ext
    const ParamEntry* campaignFunctions = ExtParsCampaign.FindEntry("CfgFunctions");
    if (!campaignFunctions && !lib.campaignDirectory.empty())
    {
        const std::string ext = lib.campaignDirectory + "description.ext";
        if (QIFStreamB::FileExist(ext.c_str()))
        {
            lib.campaignConfig = std::make_unique<ParamFile>();
            lib.campaignConfig->Parse(ext.c_str());
            campaignFunctions = lib.campaignConfig->FindEntry("CfgFunctions");
        }
    }
    IndexConfig(lib, campaignFunctions, Origin::Campaign, lib.campaignDirectory);
    IndexConfig(lib, ExtParsMission.FindEntry("CfgFunctions"), Origin::Mission, mission);
    LOG_INFO(Script, "Arma 3: function library indexed: {} functions ({} preInit, {} postInit)", lib.byName.size(),
             lib.preInit.size(), lib.postInit.size());
    return lib;
}

GameValue CodeValue(const std::string& text)
{
    if (GameState::s_codeValues)
        return GameValue(new GameDataCode(RString(text.c_str())));
    return GameValue(RString(text.c_str()));
}

std::string Header(const Function& fn)
{
    if (fn.header < 0)
        return {};
    if (fn.header == 1)
        return "private _fnc_scriptNameParent = if (isNil '_fnc_scriptName') then {'" + fn.name +
               "'} else {_fnc_scriptName}; scriptName '" + fn.name + "';\n";
    return "private _fnc_scriptNameParent = if (isNil '_fnc_scriptName') then {'" + fn.name +
           "'} else {_fnc_scriptName}; private _fnc_scriptName = '" + fn.name + "'; scriptName _fnc_scriptName;\n";
}

bool MissionFileExists(const char* name); // below

bool Compile(const Function& fn, GameValue& value)
{
    std::string path = fn.path;
    if (Lower(fn.ext) == ".fsm")
    {
        value = CodeValue("_this execFSM \"" + path + "\"");
        return true;
    }
    // a bank path is read as such, not relative to the mission -- except a bare file name of Arma 3's
    // own config (BIS_fnc_missionConversationsLocal: "missionConversations.sqf", BIS_fnc_missionTasksLocal),
    // which names the running mission's file: each mission brings its own
    if (fn.origin == Origin::Config && !path.empty() && path[0] != '\\')
    {
        const std::string inMission = std::string((const char*)GetMissionDirectory()) + path;
        path = path.find('\\') == std::string::npos && MissionFileExists(path.c_str()) ? inMission : "\\" + path;
    }
    std::string text;
    if (!PreprocessScriptFile(path.c_str(), text))
    {
        static std::set<std::string> reported;
        if (reported.insert(Lower(fn.name)).second)
        {
            const std::string bare = path[0] == '\\' ? path.substr(1) : path;
            LOG_WARN(Script, "Arma 3: function {}: file {} {}", fn.name, fn.path,
                     QIFStreamB::FileExist(bare.c_str()) ? "could not be preprocessed" : "not found");
        }
        return false;
    }
    value = CodeValue(Header(fn) + text);
    return true;
}

// GameState::s_resolveGlobal: a function the first time a script names it.
bool ResolveFunction(const char* lowerName, GameValue& value)
{
    if (Arma1Compat::Generation() != 3 || !lowerName || !strstr(lowerName, "_fnc_"))
        return false;
    Library& lib = CurrentLibrary();
    std::string key = lowerName;
    // <function>_meta: [path, ext, header, preInit, postInit, recompile, tag, category, name]
    if (key.size() > 5 && key.compare(key.size() - 5, 5, "_meta") == 0)
    {
        const auto it = lib.byName.find(key.substr(0, key.size() - 5));
        if (it == lib.byName.end())
            return false;
        const Function& fn = it->second;
        const std::string shortName = fn.name.substr(fn.name.find("_fnc_") + 5);
        value = CodeValue("[\"" + fn.path + "\",\"" + fn.ext + "\"," + std::to_string(fn.header) + "," +
                          (fn.preInit ? "true" : "false") + "," + (fn.postInit ? "true" : "false") + ",false,\"" +
                          fn.tagClass + "\",\"" + fn.category + "\",\"" + shortName + "\"]");
        return true;
    }
    const auto it = lib.byName.find(key);
    if (it == lib.byName.end())
        return false;
    return Compile(it->second, value);
}

void RunUnscheduled(GameState* gs, const std::string& code, const char* what)
{
    gs->Execute(code.c_str());
    if (gs->GetLastError() != EvalOK)
        LOG_WARN(Script, "Arma 3: {}: script error {}", what, (const char*)gs->GetLastErrorText());
}

bool MissionFileExists(const char* name)
{
    const std::string path = std::string((const char*)GetMissionDirectory()) + name;
    return QIFStreamB::FileExist(path.c_str());
}

bool IsContactMission()
{
    return Lower((const char*)GetMissionDirectory()).find("missions_f_contact") != std::string::npos;
}
} // namespace

// Installs the function resolver (called when --arma3 mounts its archives).
void Arma3InstallFunctionLibrary()
{
    GameState::s_resolveGlobal = &ResolveFunction;
}

namespace
{
// A colour of Arma 3's interface, "<section>_<variable>_<r|g|b|a>" (IGUI_BCG_RGB_R): its profile
// keeps what the player chose in Options, and before any choice the preset CfgUIColors marks
// `default = 1` (IGUI: grey panels, near-white text). This engine has no such profile, so the
// default preset answers.
bool ResolveUiColor(const char* lowerName, GameValue& value)
{
    const std::string name = lowerName;
    const size_t first = name.find('_'), last = name.rfind('_');
    if (first == std::string::npos || last <= first + 1 || last + 2 != name.size())
        return false;
    const int component = std::string("rgba").find(name[last + 1]);
    if (component < 0)
        return false;
    const ParamEntry* root = Arma1Compat::ConfigRoot();
    const ParamEntry* colors = root ? root->FindEntry("CfgUIColors") : nullptr;
    const ParamEntry* section = colors && colors->IsClass() ? colors->FindEntry(name.substr(0, first).c_str()) : nullptr;
    const ParamEntry* presets = section && section->IsClass() ? section->FindEntry("Presets") : nullptr;
    if (!presets || !presets->IsClass())
        return false;
    for (int i = 0; i < presets->GetEntryCount(); i++)
    {
        const ParamEntry& preset = presets->GetEntry(i);
        const ParamEntry* isDefault = preset.IsClass() ? preset.FindEntry("default") : nullptr;
        if (!isDefault || static_cast<float>(*isDefault) == 0)
            continue;
        const ParamEntry* vars = preset.FindEntry("Variables");
        const ParamEntry* rgba =
            vars && vars->IsClass() ? vars->FindEntry(name.substr(first + 1, last - first - 1).c_str()) : nullptr;
        if (!rgba || !rgba->IsArray() || rgba->GetSize() <= component)
            return false;
        value = GameValue(static_cast<float>((*rgba)[component]));
        return true;
    }
    return false;
}
} // namespace

// uiNamespace / profileNamespace getVariable for a name no script set: Arma 3 keeps the library
// there too, and its profile the interface's colours.
bool Arma3ResolveUiVariable(const char* lowerName, GameValue& value)
{
    return ResolveFunction(lowerName, value) || ResolveUiColor(lowerName, value);
}

// Arma 3's mission start, after the objects' init lines ran (see the file comment).
void Arma3MissionStart()
{
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs || Arma1Compat::Generation() != 3)
        return;
    Arma3Rsc::SetHudShown(true);
    DynSim::Reset();
    Library& lib = CurrentLibrary();
    const bool contact = IsContactMission();
    const auto runs = [&](const std::string& key)
    {
        const auto it = lib.byName.find(key);
        return it != lib.byName.end() && (contact || Lower(it->second.tagClass) != "contact");
    };

    // the logic Arma 3's functions keep their state on
    RunUnscheduled(gs,
                   "if (isNil 'bis_functions_mainscope') then {private _g = createGroup sideLogic; "
                   "bis_functions_mainscope = _g createUnit ['Logic', [9,9,9], [], 0, 'NONE']}; "
                   "missionNamespace setVariable ['bis_fnc_init', false]",
                   "functions main scope");
    for (const std::string& key : lib.preInit)
    {
        if (!runs(key))
            continue;
        const std::string& name = lib.byName[key].name;
        LOG_INFO(Script, "Arma 3: preInit {}", name);
        RunUnscheduled(gs, "['preInit'] call " + name, name.c_str());
    }

    // scheduled: one script per step, each after the one before, so an error in one step
    // does not keep the next from running
    std::vector<std::string> steps;
    steps.push_back("[] call BIS_fnc_initModules");
    if (MissionFileExists("initServer.sqf"))
        steps.push_back("[] execVM 'initServer.sqf'");
    if (MissionFileExists("initPlayerLocal.sqf"))
        steps.push_back("[player, false] execVM 'initPlayerLocal.sqf'");
    if (MissionFileExists("initPlayerServer.sqf"))
        steps.push_back("[player, false] execVM 'initPlayerServer.sqf'");
    for (const std::string& key : lib.postInit)
        if (runs(key))
            steps.push_back("['postInit', false] call " + lib.byName[key].name);
    steps.push_back("missionNamespace setVariable ['bis_fnc_init', true]");
    if (MissionFileExists("init.sqf"))
        steps.push_back("[] execVM 'init.sqf'");

    std::string script = "[] spawn {";
    for (const std::string& step : steps)
    {
        // a step that is itself a mission script (execVM) is waited for as well; what a function
        // spawns is not: BIS_fnc_initModules hands back a script that waits for the mission to run,
        // and init.sqf (which writes the briefing's diary) runs before it does
        std::string quoted;
        for (char c : step)
            quoted += c == '"' ? std::string("\"\"") : std::string(1, c);
        const bool waits = step.find("execVM") != std::string::npos;
        script += "private _h = [] spawn {private _r = call compile \"" + quoted + "\"; " +
                  (waits ? "if (!isNil '_r') then {if (typeName _r == 'SCALAR') then {waitUntil {scriptDone _r}}}" : "") +
                  "}; waitUntil {scriptDone _h}; ";
    }
    script += "}";
    LOG_INFO(Script, "Arma 3: mission start: {} preInit, {} scheduled steps", lib.preInit.size(), steps.size());
    RunUnscheduled(gs, script, "mission start");
}
