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

GameValue A1ConfigCount(const GameState*, GameValuePar oper1)
{
    bool root = false;
    const ParamEntry* e = Resolve(oper1, &root);
    if (root && !e)
        e = Arma1Compat::ConfigRoot() ? Arma1Compat::ConfigRoot() : &Pars;
    return GameValue(e && e->IsClass() ? (float)e->GetEntryCount() : 0.0f);
}

GameValue A1ConfigSelect(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
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
