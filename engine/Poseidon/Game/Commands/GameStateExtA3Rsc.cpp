// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3 title resources as scripts see them (--arma3 only).
//
// This engine draws a title resource only when CWA has the class (the mission's own
// RscTitles, CWA's resource); it never ran a resource's onLoad / onUnload, and Arma 3's
// own classes (RscTitleDisplayEmpty, RscStatic, RscEstablishingShot, RscDynamicText, ...)
// do not exist for it at all. Arma 3's scripts, though, are built on those handlers: a
// display registers itself in uiNamespace from onLoad and the script around it waits for
// that -- and, when the display is gone, for the stored display to read as null.
//
// BIS_fnc_titlecard and BIS_fnc_establishingShot (every showcase's opening) wait exactly
// so. They never got past the wait, so the showcases kept the "0 fadeSound 0" of their
// first line for the whole mission: no sound at all.
//
// Here a cutRsc / titleRsc of a class that has handlers opens a stand-in display: a number
// from 100000 up (displays and controls are numbers in this engine; displayNull is -1).
// onLoad runs with _this = [display]; if it stored nothing under the class name (Arma 3's
// BIS_fnc_initDisplay does that first thing, but stops at `_display setVariable`, which a
// number does not take), the display is stored there for it. The display closes when its
// duration and fade-out have passed, when another resource or a text takes its layer, or
// with cutFadeOut; onUnload runs, and from then on isNull reads the number as null.
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtA3Rsc.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr int kFirstDisplay = 100000;

struct OpenDisplay
{
    std::string cls;
    std::string layer;
    std::string onUnload;
};

std::map<int, OpenDisplay>& Displays()
{
    static std::map<int, OpenDisplay> displays;
    return displays;
}

int& NextDisplay()
{
    static int next = kFirstDisplay;
    return next;
}

std::string Text(const ParamEntry& cls, const char* name)
{
    const ParamEntry* e = cls.FindEntry(name);
    if (!e || e->IsClass() || e->IsArray())
        return std::string();
    return std::string((const char*)(RStringB)*e);
}

float Number(const ParamEntry& cls, const char* name, float fallback)
{
    const ParamEntry* e = cls.FindEntry(name);
    if (!e || e->IsClass() || e->IsArray())
        return fallback;
    return static_cast<float>(*e);
}

// The mission's own class first (as the engine finds it to draw it), then Arma 3's.
const ParamEntry* FindTitle(const RString& name)
{
    for (const ParamFile* file : {&ExtParsMission, &ExtParsCampaign})
        if (const ParamEntry* titles = file->FindEntry("RscTitles"))
            if (const ParamEntry* e = titles->FindEntry(name); e && e->IsClass())
                return e;
    const ParamEntry* e = Poseidon::Arma1Compat::FindConfigClass("RscTitles", name);
    return e && e->IsClass() ? e : nullptr;
}

std::string Quoted(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

void Run(GameState* gs, const std::string& code, int display)
{
    if (code.empty())
        return;
    GameVarSpace local(gs->GetContext());
    gs->BeginContext(&local);
    GameArrayType args;
    args.Add(GameValue(static_cast<float>(display)));
    gs->VarSetLocal("_this", GameValue(args), false, true);
    gs->EvaluateMultiple(code.c_str());
    gs->EndContext();
}

void Close(GameState* gs, int display)
{
    auto it = Displays().find(display);
    if (it == Displays().end())
        return;
    const OpenDisplay closed = it->second;
    Displays().erase(it);
    Run(gs, closed.onUnload, display);
}

GameState* State() { return Poseidon::GWorld ? Poseidon::GWorld->GetGameState() : nullptr; }

// Named cut layers ("BIS_layerTitlecard" cutRsc ..., BIS_fnc_rscLayer): Arma 3 numbers a name
// the first time it is used; allCutLayers lists the names, each at its number. Layer 0 is the
// unnamed one the unary commands use.
std::vector<std::string>& LayerNames()
{
    static std::vector<std::string> names{""};
    return names;
}

int LayerNumber(const std::string& name)
{
    auto& names = LayerNames();
    for (size_t i = 0; i < names.size(); i++)
        if (_stricmp(names[i].c_str(), name.c_str()) == 0)
            return static_cast<int>(i);
    names.push_back(name);
    return static_cast<int>(names.size() - 1);
}

GameValue A3AllCutLayers(const GameState*)
{
    GameValue value = GGameState.CreateGameValue(GameArray);
    GameArrayType& list = value;
    for (const std::string& n : LayerNames())
        list.Add(GameValue(RString(n.c_str())));
    return value;
}

GameValue A3RscClose(const GameState*, GameValuePar oper1)
{
    if (GameState* gs = State())
        Close(gs, toInt(static_cast<float>(oper1)));
    return NOTHING;
}
} // namespace

namespace Poseidon::Arma3Rsc
{
std::string LayerKey(GameValuePar layer)
{
    if (layer.GetType() == GameString)
        return std::to_string(LayerNumber(std::string((const char*)(RString)layer)));
    if (layer.GetType() == GameScalar)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", toInt(static_cast<float>(layer)));
        return buf;
    }
    return "0";
}

// A text, a cut effect or cutFadeOut on the layer ends the display shown there.
void CloseLayer(const std::string& layer)
{
    if (Arma1Compat::Generation() != 3)
        return;
    GameState* gs = State();
    if (!gs)
        return;
    for (auto it = Displays().begin(); it != Displays().end(); ++it)
        if (it->second.layer == layer)
        {
            Close(gs, it->first);
            return;
        }
}

void Open(const std::string& layer, GameValuePar resource)
{
    if (Arma1Compat::Generation() != 3)
        return;
    CloseLayer(layer);
    if (resource.GetType() != GameArray)
        return;
    const GameArrayType& args = resource;
    if (args.Size() < 1 || args[0].GetType() != GameString)
        return;
    const RString name = args[0];
    const ParamEntry* cls = FindTitle(name);
    GameState* gs = State();
    if (!cls || !gs)
        return;
    const std::string onLoad = Text(*cls, "onLoad");
    const std::string onUnload = Text(*cls, "onUnload");
    if (onLoad.empty() && onUnload.empty())
        return;
    const int display = NextDisplay()++;
    Displays()[display] = OpenDisplay{(const char*)name, layer, onUnload};
    // Arma 3 compiles each display's script before the game starts (BIS_fnc_initDisplay with
    // no arguments, at preStart) and BIS_fnc_initDisplay later calls uiNamespace's
    // "<class>_script"; there is no preStart here, so the script is compiled on first use.
    const std::string scriptName = Text(*cls, "scriptName");
    const std::string scriptPath = Text(*cls, "scriptPath");
    if (!scriptName.empty() && !scriptPath.empty())
    {
        const ParamEntry* root = Arma1Compat::ConfigRoot();
        const ParamEntry* paths = root ? root->FindEntry("CfgScriptPaths") : nullptr;
        const ParamEntry* folder = paths ? paths->FindEntry(scriptPath.c_str()) : nullptr;
        if (folder && !folder->IsClass() && !folder->IsArray())
        {
            const std::string file = std::string((const char*)(RStringB)*folder) + scriptName + ".sqf";
            const std::string var = Quoted(scriptName + "_script");
            Run(gs,
                "if (isNil {uiNamespace getVariable " + var + "}) then {uiNamespace setVariable [" + var +
                    ", compile (" + Quoted("scriptName '" + scriptName + "'; _fnc_scriptName = '" + scriptName + "'; ") +
                    " + preprocessFileLineNumbers " + Quoted(file) + ")]};",
                display);
        }
    }
    Run(gs, onLoad, display);
    const std::string clsName = (const char*)name;
    std::string code = "if (isNil {uiNamespace getVariable " + Quoted(clsName) + "}) then {uiNamespace setVariable [" +
                       Quoted(clsName) + ", _this select 0]};";
    // a resource shown for a time closes by itself (10e10 is "until replaced")
    const float duration = Number(*cls, "duration", 0) + Number(*cls, "fadeout", 0);
    if (duration < 1e6f)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", duration > 0 ? duration : 0.0f);
        code += " (_this select 0) spawn {sleep " + std::string(buf) + "; opA3RscClose _this};";
    }
    Run(gs, code, display);
    LOG_DEBUG(Script, "Arma 3: title resource {} opened as display {} on layer {}", clsName, display, layer);
}

// A stand-in display that has been closed reads as null.
bool IsClosedDisplay(float value)
{
    const int id = toInt(value);
    return id >= kFirstDisplay && id < NextDisplay() && Displays().find(id) == Displays().end();
}

// layer cutFadeOut time: ends what the layer shows; a named layer answers its number (which
// BIS_fnc_rscLayer returns -- as a type error it returned nothing, and every title the
// showcases show went to no layer).
// "layer" cutRsc [class, type, ...]: a named layer (BIS_fnc_showSubtitle, ...)
GameValue CutRscEffectA3(const GameState* state, GameValuePar oper1); // below
GameValue A3CutRscNamed(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Open(LayerKey(oper1), oper2);
    return CutRscEffectA3(state, oper2);
}

GameValue A3CutFadeOutNamed(const GameState*, GameValuePar oper1, GameValuePar)
{
    const std::string key = LayerKey(oper1);
    CloseLayer(key);
    return GameValue(static_cast<float>(std::atoi(key.c_str())));
}

// display / control setVariable and getVariable: displays and controls are numbers here, and
// Arma 3's user-interface scripts keep their state on them (BIS_fnc_initDisplay stores its
// class, BIS_fnc_playVideo its video control). Without the forms the first such line
// stopped the script.
// Never destroyed: the values are script values, and at exit the evaluator is gone before
// static maps are torn down (Showcase_Night crashed after "Shutdown complete").
std::map<int, std::map<std::string, GameValue>>& HandleVars()
{
    static auto* vars = new std::map<int, std::map<std::string, GameValue>>();
    return *vars;
}

std::string LowerName(const char* s)
{
    std::string out = s ? s : "";
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

GameValue A3HandleSetVariable(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& args = oper2;
    if (args.Size() < 2 || args[0].GetType() != GameString)
        return NOTHING;
    HandleVars()[toInt(static_cast<float>(oper1))][LowerName((const char*)(RString)args[0])] = args[1];
    return NOTHING;
}

GameValue A3HandleGetVariable(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    GameValue fallback;
    RString name;
    if (oper2.GetType() == GameString)
        name = oper2;
    else if (oper2.GetType() == GameArray)
    {
        const GameArrayType& args = oper2;
        if (args.Size() < 1 || args[0].GetType() != GameString)
            return NOTHING;
        name = args[0];
        if (args.Size() > 1)
            fallback = args[1];
    }
    else
        return NOTHING;
    const auto h = HandleVars().find(toInt(static_cast<float>(oper1)));
    if (h != HandleVars().end())
    {
        const auto v = h->second.find(LowerName((const char*)name));
        if (v != h->second.end())
            return v->second;
    }
    return fallback;
}

// Registered before the macros and stubs: of two forms with the same arguments the first
// registered is the one used.
void Register()
{
    GGameState.NewFunction(GameFunction(GameNothing, "opA3RscClose", A3RscClose, GameScalar));
    GGameState.NewNularOp(GameNular(GameArray, "allCutLayers", A3AllCutLayers));
    GGameState.NewOperator(GameOperator(GameScalar, "cutFadeOut", function, A3CutFadeOutNamed, GameString, GameScalar));
    GGameState.NewOperator(GameOperator(GameNothing, "cutRsc", function, A3CutRscNamed, GameString, GameArray));
    GGameState.NewOperator(GameOperator(GameNothing, "setVariable", function, A3HandleSetVariable, GameScalar, GameArray));
    GGameState.NewOperator(
        GameOperator(GameAny, "getVariable", function, A3HandleGetVariable, GameScalar, GameString | GameArray));
}
} // namespace Poseidon::Arma3Rsc

GameValue CutRscEffect(const GameState* state, GameValuePar oper1); // GameStateExtUi.cpp

namespace Poseidon::Arma3Rsc
{
GameValue CutRscEffectA3(const GameState* state, GameValuePar oper1)
{
    return ::CutRscEffect(state, oper1);
}
} // namespace Poseidon::Arma3Rsc
