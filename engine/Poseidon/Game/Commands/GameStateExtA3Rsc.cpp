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
#include <map>
#include <string>

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
        return std::string((const char*)(RString)layer);
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

void Register()
{
    GGameState.NewFunction(GameFunction(GameNothing, "opA3RscClose", A3RscClose, GameScalar));
}
} // namespace Poseidon::Arma3Rsc
