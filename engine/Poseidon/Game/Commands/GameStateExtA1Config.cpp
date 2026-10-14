// Arma 1 script access to the config tree (configFile >> "CfgVehicles" >> "M1A1",
// getText, getNumber, getArray, isClass, configName, count, select, inheritsFrom) and a
// few general Arma 1 helpers (min, max, typeName, surfaceIsWater).
//
// A config value is represented as a tagged string "::CFG::<a/b/c>" (configFile) or
// "::MCFG::<a/b/c>" (missionConfigFile). With --arma1 a configFile path resolves in Arma
// 1's own config first and falls back to CWA's.
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace Poseidon;

namespace
{
constexpr const char* kCfg = "::CFG::";
constexpr const char* kMissionCfg = "::MCFG::";

bool IsConfigValue(GameValuePar v)
{
    if (v.GetType() != GameString)
        return false;
    const RString s = v;
    return strncmp(s, kCfg, strlen(kCfg)) == 0 || strncmp(s, kMissionCfg, strlen(kMissionCfg)) == 0;
}

std::vector<std::string> Segments(const char* path)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = path; *p; p++)
    {
        if (*p == '/')
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(*p);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

const ParamEntry* Walk(const ParamEntry* root, const std::vector<std::string>& segs)
{
    const ParamEntry* e = root;
    for (const std::string& s : segs)
    {
        if (!e || !e->IsClass())
            return nullptr;
        e = e->FindEntry(s.c_str());
    }
    return e;
}

// The entry a config value names; null for configNull or a missing entry. `isRoot`
// tells a bare configFile apart (its root has no entry of its own).
const ParamEntry* Resolve(GameValuePar v, bool* isRoot = nullptr)
{
    if (isRoot)
        *isRoot = false;
    if (!IsConfigValue(v))
        return nullptr;
    const RString s = v;
    const bool mission = strncmp(s, kMissionCfg, strlen(kMissionCfg)) == 0;
    const char* path = (const char*)s + strlen(mission ? kMissionCfg : kCfg);
    const std::vector<std::string> segs = Segments(path);
    if (isRoot)
        *isRoot = segs.empty();
    if (mission)
        return Walk(&ExtParsMission, segs);
    if (const ParamEntry* a1 = Arma1Compat::ConfigRoot())
        if (const ParamEntry* e = Walk(a1, segs))
            return e;
    return Walk(&Pars, segs);
}

GameValue Child(GameValuePar parent, const char* name)
{
    const RString s = parent;
    std::string path = (const char*)s;
    if (path.back() != ':')
        path += '/';
    path += name;
    return GameValue(RString(path.c_str()));
}

GameValue ArrayOf(const ParamEntry& entry)
{
    GameArrayType out;
    for (int i = 0; i < entry.GetSize(); i++)
    {
        const IParamArrayValue& item = entry[i];
        if (item.IsArrayValue())
        {
            // nested arrays: rebuild from their items
            GameArrayType inner;
            for (int k = 0; k < item.GetItemCount(); k++)
            {
                const IParamArrayValue& sub = *item.GetItem(k);
                if (sub.IsFloatValue() || sub.IsIntValue())
                    inner.Add(GameValue((float)sub));
                else
                    inner.Add(GameValue(RString(sub.GetValue())));
            }
            out.Add(GameValue(inner));
        }
        else if (item.IsFloatValue() || item.IsIntValue())
            out.Add(GameValue((float)item));
        else
            out.Add(GameValue(RString(item.GetValue())));
    }
    return GameValue(out);
}
} // namespace

GameValue A1ConfigFile(const GameState*)
{
    return GameValue(RString(kCfg));
}

GameValue A1MissionConfigFile(const GameState*)
{
    return GameValue(RString(kMissionCfg));
}

GameValue A1ConfigChild(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!IsConfigValue(oper1))
        return GameValue(RString(""));
    return Child(oper1, RString(oper2));
}

GameValue A1GetText(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    if (!e || e->IsClass() || e->IsArray())
        return GameValue(RString(""));
    return GameValue(RString(e->GetValue()));
}

GameValue A1GetNumber(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    if (!e || e->IsClass() || e->IsArray())
        return GameValue(0.0f);
    return GameValue((float)*e);
}

GameValue A1GetArray(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    if (!e || !e->IsArray())
        return GameValue(GameArrayType());
    return ArrayOf(*e);
}

GameValue A1IsClass(const GameState*, GameValuePar oper1)
{
    bool root = false;
    const ParamEntry* e = Resolve(oper1, &root);
    return GameValue(root || (e && e->IsClass()));
}

GameValue A1IsArray(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    return GameValue(e && e->IsArray());
}

GameValue A1IsText(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    return GameValue(e && !e->IsClass() && !e->IsArray() && e->IsTextValue());
}

GameValue A1IsNumber(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    return GameValue(e && !e->IsClass() && !e->IsArray() && (e->IsFloatValue() || e->IsIntValue()));
}

GameValue A1ConfigName(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    return GameValue(RString(e ? (const char*)e->GetName() : ""));
}

// A config entry that names nothing (configNull, or a path to a missing entry).
bool A1ConfigIsNull(GameValuePar v)
{
    bool root = false;
    const ParamEntry* e = Resolve(v, &root);
    return !root && !e;
}

GameValue A1ConfigCount(const GameState*, GameValuePar oper1)
{
    // Arma 3: count "text" is the length of a string that is no config entry
    if (!IsConfigValue(oper1))
        return GameValue(static_cast<float>(((RString)oper1).GetLength()));
    bool root = false;
    const ParamEntry* e = Resolve(oper1, &root);
    if (root && !e)
        e = Arma1Compat::ConfigRoot() ? Arma1Compat::ConfigRoot() : &Pars;
    return GameValue(e && e->IsClass() ? (float)e->GetEntryCount() : 0.0f);
}

GameValue A1ConfigSelect(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    // Arma 3: "text" select n is the rest of a string from n on
    if (!IsConfigValue(oper1))
    {
        const RString s = oper1;
        const int from = std::max(0, toInt((float)oper2));
        return GameValue(from < s.GetLength() ? RString((const char*)s + from) : RString(""));
    }
    bool root = false;
    const ParamEntry* e = Resolve(oper1, &root);
    if (root && !e)
        e = Arma1Compat::ConfigRoot() ? Arma1Compat::ConfigRoot() : &Pars;
    const int i = toInt((float)oper2);
    if (!e || !e->IsClass() || i < 0 || i >= e->GetEntryCount())
        return GameValue(RString(""));
    return Child(oper1, e->GetEntry(i).GetName());
}

GameValue A1InheritsFrom(const GameState*, GameValuePar oper1)
{
    const ParamEntry* e = Resolve(oper1);
    const ParamClass* c = e ? e->GetClassInterface() : nullptr;
    const char* base = c ? c->GetBaseName() : nullptr;
    if (!base || !*base)
        return GameValue(RString(""));
    // the base class lives beside the class: same parent path
    const RString s = oper1;
    std::string path = (const char*)s;
    const size_t slash = path.find_last_of('/');
    const size_t colon = path.find_last_of(':');
    path.resize(slash != std::string::npos && (colon == std::string::npos || slash > colon) ? slash : colon + 1);
    if (path.back() != ':')
        path += '/';
    path += base;
    return GameValue(RString(path.c_str()));
}

GameValue A1Min(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(std::min((float)oper1, (float)oper2));
}

GameValue A1Max(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(std::max((float)oper1, (float)oper2));
}

GameValue A1TypeName(const GameState*, GameValuePar oper1)
{
    const GameType t = oper1.GetType();
    if (t == GameString && oper1.GetData() && oper1.GetData()->IsCode())
        return GameValue(RString("CODE"));
    // Arma 2 handles that are tagged strings here.
    if (t == GameString)
    {
        const RString s = oper1;
        static const struct
        {
            const char* tag;
            const char* name;
        } tagged[] = {{"::LOC::", "LOCATION"}, {"::TASK::", "TASK"}, {"::DIARY::", "DIARY_RECORD"}, {"::NS::", "NAMESPACE"}};
        for (const auto& k : tagged)
            if (strncmp(s, k.tag, strlen(k.tag)) == 0)
                return GameValue(RString(k.name));
    }
    const char* name = t == GameScalar   ? "SCALAR"
                       : t == GameBool   ? "BOOL"
                       : t == GameArray  ? "ARRAY"
                       : t == GameString ? (IsConfigValue(oper1) ? "CONFIG" : "STRING")
                       : t == GameObject ? "OBJECT"
                       : t == GameGroup  ? "GROUP"
                       : t == GameSide   ? "SIDE"
                                         : "ANY";
    return GameValue(RString(name));
}

GameValue A1SurfaceIsWater(const GameState* state, GameValuePar oper1)
{
    Vector3 pos;
    if (!GetRelPos(state, pos, oper1))
        return GameValue(false);
    return GameValue(GLandscape->SurfaceY(pos.X(), pos.Z()) < 0.0f);
}

// ---------------------------------------------------------------------------------------
// Arma 3 config commands
// ---------------------------------------------------------------------------------------

namespace
{
// The entries of a class, own and (when `inherit`) inherited, in Arma 3's order: the
// class's own first, then each base's that the class does not override.
void CollectEntries(const ParamEntry& cls, bool inherit, std::vector<const ParamEntry*>& out)
{
    std::vector<std::string> seen;
    const auto has = [&](const char* n)
    {
        for (const std::string& s : seen)
            if (_stricmp(s.c_str(), n) == 0)
                return true;
        return false;
    };
    const ParamEntry* c = &cls;
    for (int guard = 0; c && guard < 64; guard++)
    {
        for (int i = 0; i < c->GetEntryCount(); i++)
        {
            const ParamEntry& e = c->GetEntry(i);
            if (has(e.GetName()))
                continue;
            seen.emplace_back((const char*)e.GetName());
            out.push_back(&e);
        }
        if (!inherit)
            break;
        const ParamClass* pc = c->GetClassInterface();
        const ParamClass* base = pc ? pc->GetBase() : nullptr;
        c = base;
    }
}

GameValue FilterEntries(const GameState* state, GameValuePar config, const RString& condition, bool inherit,
                        bool classesOnly)
{
    GameArrayType out;
    bool root = false;
    const ParamEntry* e = Resolve(config, &root);
    if (root && !e)
        e = Arma1Compat::ConfigRoot() ? Arma1Compat::ConfigRoot() : &Pars;
    if (!e || !e->IsClass())
        return GameValue(out);
    std::vector<const ParamEntry*> entries;
    CollectEntries(*e, inherit, entries);
    for (const ParamEntry* entry : entries)
    {
        if (classesOnly && !entry->IsClass())
            continue;
        const GameValue child = Child(config, entry->GetName());
        if (condition.GetLength() > 0)
        {
            GameVarSpace local(state->GetContext());
            state->BeginContext(&local);
            state->VarSetLocal("_x", child, false, true);
            const GameValue keep = state->EvaluateMultiple(condition);
            state->EndContext();
            if (state->GetLastError() != EvalOK)
                break;
            if (keep.GetType() != GameBool || !(bool)keep)
                continue;
        }
        out.Add(child);
    }
    return GameValue(out);
}
} // namespace

// "condition" configClasses config: the classes of a config class (inherited ones too)
// for which the condition (with _x = the class) holds.
GameValue A3ConfigClasses(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return FilterEntries(state, oper2, (RString)oper1, true, true);
}

// configProperties [config, condition, inherit]
GameValue A3ConfigProperties(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 1)
        return GameValue(GameArrayType());
    const RString condition = a.Size() > 1 && a[1].GetType() == GameString ? (RString)a[1] : RString("true");
    const bool inherit = a.Size() > 2 && a[2].GetType() == GameBool ? (bool)a[2] : true;
    return FilterEntries(state, a[0], condition, inherit, false);
}

// configHierarchy config: [configFile, configFile >> "CfgVehicles", ..., config]
GameValue A3ConfigHierarchy(const GameState*, GameValuePar oper1)
{
    GameArrayType out;
    if (!IsConfigValue(oper1))
        return GameValue(out);
    const RString s = oper1;
    const bool mission = strncmp(s, kMissionCfg, strlen(kMissionCfg)) == 0;
    const char* tag = mission ? kMissionCfg : kCfg;
    GameValue cur = GameValue(RString(tag));
    out.Add(cur);
    for (const std::string& seg : Segments((const char*)s + strlen(tag)))
    {
        cur = Child(cur, seg.c_str());
        out.Add(cur);
    }
    return GameValue(out);
}

GameValue A3ConfigNull(const GameState*)
{
    return GameValue(RString("::CFG::\x01null"));
}

// getMissionConfigValue "name" / [name, default]: description.ext's value, else the
// mission.sqm scenario attribute (not kept here), else the default.
GameValue A3GetMissionConfigValue(const GameState*, GameValuePar oper1)
{
    RString name;
    GameValue fallback;
    if (oper1.GetType() == GameArray)
    {
        const GameArrayType& a = oper1;
        if (a.Size() < 1 || a[0].GetType() != GameString)
            return GameValue();
        name = a[0];
        if (a.Size() > 1)
            fallback = a[1];
    }
    else
        name = oper1;
    const ParamEntry* e = ExtParsMission.FindEntry(name);
    if (!e)
        return fallback;
    if (e->IsArray())
        return ArrayOf(*e);
    if (e->IsClass())
        return Child(GameValue(RString(kMissionCfg)), name);
    if (e->IsFloatValue() || e->IsIntValue())
        return GameValue((float)*e);
    return GameValue(RString(e->GetValue()));
}

GameValue A3GetMissionConfig(const GameState*, GameValuePar oper1)
{
    return Child(GameValue(RString(kMissionCfg)), (RString)oper1);
}

GameValue A3IsLocalized(const GameState*, GameValuePar oper1)
{
    RString key = oper1;
    const char* k = key;
    if (*k == '@')
        k++;
    const RString text = LocalizeString(k);
    return GameValue(text.GetLength() > 0 && _stricmp(text, k) != 0);
}

GameValue A3ConfigSourceMod(const GameState*, GameValuePar)
{
    return GameValue(RString(""));
}

GameValue A3ConfigSourceAddonList(const GameState*, GameValuePar)
{
    return GameValue(GameArrayType());
}

// Arma 3 config commands, registered with the other Arma 3 commands (--arma3 only).
void RegisterArma3ConfigCommands()
{
    GGameState.NewOperator(GameOperator(GameArray, "configClasses", function, A3ConfigClasses, GameString, GameString));
    GGameState.NewFunction(GameFunction(GameArray, "configProperties", A3ConfigProperties, GameArray));
    GGameState.NewFunction(GameFunction(GameArray, "configHierarchy", A3ConfigHierarchy, GameString));
    GGameState.NewNularOp(GameNular(GameString, "configNull", A3ConfigNull));
    GGameState.NewFunction(GameFunction(GameAny, "getMissionConfigValue", A3GetMissionConfigValue, GameString | GameArray));
    GGameState.NewFunction(GameFunction(GameString, "getMissionConfig", A3GetMissionConfig, GameString));
    GGameState.NewFunction(GameFunction(GameString, "getTextRaw", A1GetText, GameString));
    GGameState.NewFunction(GameFunction(GameBool, "isLocalized", A3IsLocalized, GameString));
    GGameState.NewFunction(GameFunction(GameString, "configSourceMod", A3ConfigSourceMod, GameString));
    GGameState.NewFunction(GameFunction(GameArray, "configSourceAddonList", A3ConfigSourceAddonList, GameString));
    GGameState.NewFunction(GameFunction(GameArray, "configSourceModList", A3ConfigSourceAddonList, GameString));
}
