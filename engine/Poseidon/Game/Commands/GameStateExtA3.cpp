// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3 script commands (--arma3).
//
// Arma 3's scripts -- the missions, and above all the function library every mission
// leans on (BIS_fnc_*) -- are written in the SQF of 2013-2024: params, pushBack, apply,
// findIf, isEqualTo, select with ranges and filters, hash maps, vector maths. This file
// adds that language core on top of OFP's evaluator (express.cpp), plus the world and
// object commands Arma 3 content uses most, implemented on CWA's own systems.
//
// Written from the public description of each command (what it takes, what it returns),
// checked against how Arma 3's own scripts call them; no Bohemia code is used.
//
// Three kinds of command live here:
//   - the language core, in C++ with Arma 3's semantics (arrays are references and are
//     changed in place; params declares private variables in the caller's scope);
//   - "macro" commands: an Arma 3 command written as a line of SQF over commands this
//     engine has (setPosWorld = setPosASL, createVehicleCrew = createGroup + createUnit +
//     moveIn*, ...). They run with _this = the operand, or [left, right] for a binary one;
//   - presentation-only commands (UI, curator, Eden editor, cosmetics) are in
//     GameStateExtA3Stubs.cpp.
//
// Code ({...}) is a GameDataCode with --arma3 (typeName "CODE"), still a string to
// every command that runs it.

#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Commands/GameStateExtA3Rsc.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AICore.hpp>
#include <Poseidon/AI/Path/ArcadeWaypoint.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Poseidon;

GameValue SideResistance(const GameState* state); // GameStateExtUi.cpp

GameValue A1TypeName(const GameState* state, GameValuePar oper1); // GameStateExtA1Config.cpp
GameValue ObjGetVectorDir(const GameState* state, GameValuePar oper1);
GameValue ObjGetVectorUp(const GameState* state, GameValuePar oper1);

namespace
{
// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

GameValue Nil()
{
    return GameValue(new GameDataNil(GameVoid));
}

bool IsCode(GameValuePar v)
{
    return v.GetType() == GameString && v.GetData() && v.GetData()->IsCode();
}

GameValue Code(const RString& text)
{
    if (GameState::s_codeValues)
        return GameValue(new GameDataCode(text));
    return GameValue(text);
}

GameValue Array(const GameState* state)
{
    return state->CreateGameValue(GameArray);
}

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Upper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The type name Arma 3 gives a value ("SCALAR", "CODE", "OBJECT", "HASHMAP", ...).
std::string TypeKey(const GameState* state, GameValuePar v)
{
    if (v.GetNil())
        return "NIL";
    if (v.GetData() && strcmp(v.GetData()->GetTypeName(), "hashmap") == 0)
        return "HASHMAP";
    return (const char*)(RString)A1TypeName(state, v);
}

bool ScalarOf(GameValuePar v, float& out)
{
    if (v.GetType() != GameScalar)
        return false;
    out = (float)v;
    return true;
}

bool Vec3(GameValuePar v, float out[3])
{
    if (v.GetType() != GameArray)
        return false;
    const GameArrayType& a = v;
    if (a.Size() < 2)
        return false;
    for (int i = 0; i < 3; i++)
        out[i] = i < a.Size() && a[i].GetType() == GameScalar ? (float)a[i] : 0.0f;
    return true;
}

GameValue MakeVec(const GameState* state, float x, float y, float z)
{
    GameValue r = Array(state);
    GameArrayType& a = r;
    a.Add(GameValue(x));
    a.Add(GameValue(y));
    a.Add(GameValue(z));
    return r;
}

// A position from an object (ASL) or a position array ([x, y] or [x, y, z]).
bool PositionOf(GameValuePar v, float out[3])
{
    if (v.GetType() == GameObject)
    {
        Object* obj = GetObject(v);
        if (!obj)
            return false;
        const Vector3 p = obj->Position();
        out[0] = p.X();
        out[1] = p.Z();
        out[2] = p.Y();
        return true;
    }
    return Vec3(v, out);
}

// Runs `code` with _x (and _y, _forEachIndex) set in a scope of its own.
GameValue RunWithX(const GameState* state, const RString& code, GameValuePar x, const GameValue* y = nullptr,
                   int index = -1)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    state->VarSetLocal("_x", x, false, true);
    if (y)
        state->VarSetLocal("_y", *y, false, true);
    if (index >= 0)
        state->VarSetLocal("_forEachIndex", GameValue(static_cast<float>(index)), false, true);
    GameValue result = state->EvaluateMultiple(code);
    state->EndContext();
    return result;
}

bool Failed(const GameState* state)
{
    return state->GetLastError() != EvalOK;
}

bool Writable(const GameState* state, GameValuePar array)
{
    if (array.GetReadOnly())
    {
        state->SetError(EvalBadVar);
        return false;
    }
    return true;
}

GameArrayType& MutableArray(GameValuePar v)
{
    const GameArrayType& a = v;
    return const_cast<GameArrayType&>(a);
}

std::mt19937& Rng()
{
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

// ---------------------------------------------------------------------------------------
// params / param
// ---------------------------------------------------------------------------------------

// Whether `value` passes an Arma 3 type/size filter ([name, default, types, sizes]).
bool PassesFilter(const GameState* state, GameValuePar value, const GameArrayType& spec)
{
    if (spec.Size() > 2 && spec[2].GetType() == GameArray)
    {
        const GameArrayType& types = spec[2];
        if (types.Size() > 0)
        {
            const std::string key = TypeKey(state, value);
            bool ok = false;
            for (int i = 0; i < types.Size() && !ok; i++)
                ok = TypeKey(state, types[i]) == key;
            if (!ok)
                return false;
        }
    }
    if (spec.Size() > 3 && value.GetType() == GameArray)
    {
        const int size = static_cast<const GameArrayType&>(value).Size();
        if (spec[3].GetType() == GameScalar)
            return toInt((float)spec[3]) == size;
        if (spec[3].GetType() == GameArray)
        {
            const GameArrayType& sizes = spec[3];
            if (sizes.Size() == 0)
                return true;
            for (int i = 0; i < sizes.Size(); i++)
                if (sizes[i].GetType() == GameScalar && toInt((float)sizes[i]) == size)
                    return true;
            return false;
        }
    }
    return true;
}

GameValue ParamsOf(const GameState* state, GameValuePar input, GameValuePar oper)
{
    GameArrayType wrapped;
    const GameArrayType* in = nullptr;
    if (input.GetType() == GameArray)
        in = &static_cast<const GameArrayType&>(input);
    else
    {
        if (!input.GetNil())
            wrapped.Add(input);
        in = &wrapped;
    }
    const GameArrayType& specs = oper;
    bool allValid = true;
    for (int i = 0; i < specs.Size(); i++)
    {
        const GameValue& spec = specs[i];
        GameValue value = i < in->Size() ? (*in)[i] : Nil();
        if (spec.GetType() == GameString)
        {
            const RString name = spec;
            if (name.GetLength() == 0)
                continue;
            state->VarSetLocal(name, value, false, true);
            continue;
        }
        if (spec.GetType() != GameArray)
            continue;
        const GameArrayType& s = spec;
        if (s.Size() < 1 || s[0].GetType() != GameString)
            continue;
        const RString name = s[0];
        const GameValue fallback = s.Size() > 1 ? s[1] : Nil();
        if (value.GetNil() || value.GetType() == GameNothing)
            value = fallback;
        else if (!PassesFilter(state, value, s))
        {
            allValid = false;
            value = fallback;
        }
        if (name.GetLength() > 0)
            state->VarSetLocal(name, value, false, true);
    }
    return GameValue(allValid);
}

GameValue A3Params(const GameState* state, GameValuePar oper1)
{
    return ParamsOf(state, state->VarGet("_this"), oper1);
}

GameValue A3ParamsBinary(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return ParamsOf(state, oper1, oper2);
}

GameValue ParamOf(const GameState* state, GameValuePar input, GameValuePar oper)
{
    GameArrayType wrapped;
    const GameArrayType* in = nullptr;
    if (input.GetType() == GameArray)
        in = &static_cast<const GameArrayType&>(input);
    else
    {
        if (!input.GetNil())
            wrapped.Add(input);
        in = &wrapped;
    }
    GameArrayType spec;
    if (oper.GetType() == GameArray)
        spec = static_cast<const GameArrayType&>(oper);
    else
        spec.Add(oper);
    const int index = spec.Size() > 0 && spec[0].GetType() == GameScalar ? toInt((float)spec[0]) : 0;
    const GameValue fallback = spec.Size() > 1 ? spec[1] : Nil();
    GameValue value = index >= 0 && index < in->Size() ? (*in)[index] : Nil();
    if (value.GetNil() || value.GetType() == GameNothing)
        return fallback;
    GameArrayType filter;
    filter.Add(GameValue(RString("")));
    filter.Add(fallback);
    for (int i = 2; i < spec.Size(); i++)
        filter.Add(spec[i]);
    return PassesFilter(state, value, filter) ? value : fallback;
}

GameValue A3Param(const GameState* state, GameValuePar oper1)
{
    return ParamOf(state, state->VarGet("_this"), oper1);
}

GameValue A3ParamBinary(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return ParamOf(state, oper1, oper2);
}

// ---------------------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------------------

GameValue A3PushBack(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return GameValue(-1.0f);
    GameArrayType& a = MutableArray(oper1);
    a.Add(oper2);
    return GameValue(static_cast<float>(a.Size() - 1));
}

GameValue A3PushBackUnique(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return GameValue(-1.0f);
    GameArrayType& a = MutableArray(oper1);
    for (int i = 0; i < a.Size(); i++)
        if (a[i].IsEqualTo(oper2))
            return GameValue(-1.0f);
    a.Add(oper2);
    return GameValue(static_cast<float>(a.Size() - 1));
}

GameValue A3Append(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return NOTHING;
    const GameArrayType src = static_cast<const GameArrayType&>(oper2); // copy: a append a
    GameArrayType& a = MutableArray(oper1);
    for (int i = 0; i < src.Size(); i++)
        a.Add(src[i]);
    return NOTHING;
}

GameValue A3Apply(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const RString code = oper2;
    GameValue result = Array(state);
    GameArrayType& out = result;
    for (int i = 0; i < a.Size(); i++)
    {
        const GameValue v = RunWithX(state, code, a[i]);
        if (Failed(state))
            break;
        out.Add(v.GetType() == GameNothing ? Nil() : v);
    }
    return result;
}

GameValue A3FindIf(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const RString code = oper2;
    for (int i = 0; i < a.Size(); i++)
    {
        const GameValue v = RunWithX(state, code, a[i]);
        if (Failed(state))
            break;
        if (v.GetType() == GameBool && (bool)v)
            return GameValue(static_cast<float>(i));
    }
    return GameValue(-1.0f);
}

// array select [start, count] / array select {condition}
GameValue A3SelectRange(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const GameArrayType& r = oper2;
    GameValue result = Array(state);
    GameArrayType& out = result;
    if (r.Size() < 1 || r[0].GetType() != GameScalar)
        return result;
    const int start = std::max(0, toInt((float)r[0]));
    const int count = r.Size() > 1 && r[1].GetType() == GameScalar ? toInt((float)r[1]) : a.Size();
    for (int i = start; i < a.Size() && i < start + count; i++)
        out.Add(a[i]);
    return result;
}

GameValue A3SelectFilter(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const RString code = oper2;
    GameValue result = Array(state);
    GameArrayType& out = result;
    for (int i = 0; i < a.Size(); i++)
    {
        const GameValue v = RunWithX(state, code, a[i]);
        if (Failed(state))
            break;
        if (v.GetType() == GameBool && (bool)v)
            out.Add(a[i]);
    }
    return result;
}

GameValue A3DeleteAt(const GameState* state, GameValuePar oper1, GameValuePar oper2);  // hash maps too, below
GameValue A3DeleteRange(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return NOTHING;
    GameArrayType& a = MutableArray(oper1);
    const GameArrayType& r = oper2;
    if (r.Size() < 2)
        return NOTHING;
    const int from = std::clamp(toInt((float)r[0]), 0, a.Size());
    const int count = std::clamp(toInt((float)r[1]), 0, a.Size() - from);
    for (int i = 0; i < count; i++)
        a.Delete(from);
    return NOTHING;
}

GameValue A3Insert(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return NOTHING;
    GameArrayType& a = MutableArray(oper1);
    const GameArrayType& args = oper2;
    if (args.Size() < 2 || args[1].GetType() != GameArray)
        return NOTHING;
    int at = args[0].GetType() == GameScalar ? toInt((float)args[0]) : a.Size();
    if (at < 0 || at > a.Size())
        at = a.Size();
    const bool unique = args.Size() > 2 && args[2].GetType() == GameBool && (bool)args[2];
    const GameArrayType items = static_cast<const GameArrayType&>(args[1]);
    for (int i = 0; i < items.Size(); i++)
    {
        if (unique)
        {
            bool found = false;
            for (int k = 0; k < a.Size() && !found; k++)
                found = a[k].IsEqualTo(items[i]);
            if (found)
                continue;
        }
        a.Insert(at++, items[i]);
    }
    return NOTHING;
}

// try {code} catch {code}: the code runs where it stands (its _this is the caller's, which
// `params` inside it reads). throw has no unwinding to go with it here: it is accepted and the
// code goes on, and catch hands on what try returned -- Arma 3's own functions throw only on
// their error paths.
GameValue A3Try(const GameState* state, GameValuePar oper1)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    const RString code = oper1;
    const GameValue ret = state->EvaluateMultiple(code);
    state->EndContext();
    return ret;
}

GameValue A3Catch(const GameState* state, GameValuePar oper1, GameValuePar)
{
    return oper1;
}

GameValue A3Throw(const GameState* state, GameValuePar oper1)
{
    LOG_DEBUG(Script, "Arma 3: throw {} (no exception handling here; the script goes on)",
              (const char*)oper1.GetText());
    return NOTHING;
}

GameValue A3Reverse(const GameState* state, GameValuePar oper1)
{
    if (!Writable(state, oper1))
        return NOTHING;
    GameArrayType& a = MutableArray(oper1);
    for (int i = 0, j = a.Size() - 1; i < j; i++, j--)
    {
        const GameValue t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    return NOTHING;
}

// Arma 3 orders numbers, strings (case-insensitively) and arrays (by their first element,
// then the next ...); mixed types keep their order.
int CompareSort(const GameValue& a, const GameValue& b)
{
    if (a.GetType() == GameScalar && b.GetType() == GameScalar)
    {
        const float x = a, y = b;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (a.GetType() == GameString && b.GetType() == GameString)
        return _stricmp((const char*)(RString)a, (const char*)(RString)b);
    if (a.GetType() == GameArray && b.GetType() == GameArray)
    {
        const GameArrayType& x = a;
        const GameArrayType& y = b;
        for (int i = 0; i < x.Size() && i < y.Size(); i++)
            if (const int c = CompareSort(x[i], y[i]); c != 0)
                return c;
        return x.Size() < y.Size() ? -1 : x.Size() > y.Size() ? 1 : 0;
    }
    return 0;
}

GameValue A3Sort(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!Writable(state, oper1))
        return NOTHING;
    GameArrayType& a = MutableArray(oper1);
    const bool ascending = (bool)oper2;
    std::vector<GameValue> v;
    for (int i = 0; i < a.Size(); i++)
        v.push_back(a[i]);
    std::stable_sort(v.begin(), v.end(), [&](const GameValue& x, const GameValue& y)
                     { return ascending ? CompareSort(x, y) < 0 : CompareSort(x, y) > 0; });
    for (int i = 0; i < a.Size(); i++)
        a[i] = v[static_cast<size_t>(i)];
    return NOTHING;
}

GameValue A3ArrayIntersect(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const GameArrayType& b = oper2;
    GameValue result = Array(state);
    GameArrayType& out = result;
    for (int i = 0; i < a.Size(); i++)
    {
        bool inB = false, already = false;
        for (int k = 0; k < b.Size() && !inB; k++)
            inB = a[i].IsEqualTo(b[k]);
        for (int k = 0; k < out.Size() && !already; k++)
            already = out[k].IsEqualTo(a[i]);
        if (inB && !already)
            out.Add(a[i]);
    }
    return result;
}

GameValue A3SelectRandom(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() == 0)
        return Nil();
    std::uniform_int_distribution<int> d(0, a.Size() - 1);
    return a[d(Rng())];
}

GameValue A3SelectRandomWeighted(const GameState*, GameValuePar oper1)
{
    // [item, weight, item, weight, ...]
    const GameArrayType& a = oper1;
    float total = 0;
    for (int i = 1; i < a.Size(); i += 2)
        if (a[i].GetType() == GameScalar)
            total += std::max(0.0f, (float)a[i]);
    if (total <= 0)
        return Nil();
    std::uniform_real_distribution<float> d(0.0f, total);
    float pick = d(Rng());
    for (int i = 1; i < a.Size(); i += 2)
    {
        pick -= a[i].GetType() == GameScalar ? std::max(0.0f, (float)a[i]) : 0.0f;
        if (pick <= 0)
            return a[i - 1];
    }
    return a[a.Size() - 2];
}

GameValue A3SelectRandomWeightedBinary(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // items selectRandomWeighted weights
    const GameArrayType& items = oper1;
    const GameArrayType& weights = oper2;
    GameValue pairs = Array(state);
    GameArrayType& p = pairs;
    for (int i = 0; i < items.Size() && i < weights.Size(); i++)
    {
        p.Add(items[i]);
        p.Add(weights[i]);
    }
    return A3SelectRandomWeighted(state, pairs);
}

GameValue SelectExtreme(GameValuePar oper1, bool max)
{
    const GameArrayType& a = oper1;
    int best = -1;
    for (int i = 0; i < a.Size(); i++)
    {
        if (a[i].GetType() != GameScalar && a[i].GetType() != GameBool)
            continue;
        if (best < 0 || (max ? (float)a[i] > (float)a[best] : (float)a[i] < (float)a[best]))
            best = i;
    }
    return best < 0 ? Nil() : a[best];
}

GameValue A3SelectMax(const GameState*, GameValuePar oper1)
{
    return SelectExtreme(oper1, true);
}

GameValue A3SelectMin(const GameState*, GameValuePar oper1)
{
    return SelectExtreme(oper1, false);
}

// ---------------------------------------------------------------------------------------
// Comparison and types
// ---------------------------------------------------------------------------------------

bool DeepEqual(const GameState* state, GameValuePar a, GameValuePar b)
{
    if (a.GetNil() || b.GetNil())
        return a.GetNil() && b.GetNil();
    if (TypeKey(state, a) != TypeKey(state, b))
        return false;
    if (a.GetType() == GameArray)
    {
        const GameArrayType& x = a;
        const GameArrayType& y = b;
        if (x.Size() != y.Size())
            return false;
        for (int i = 0; i < x.Size(); i++)
            if (!DeepEqual(state, x[i], y[i]))
                return false;
        return true;
    }
    if (a.GetType() == GameString)
        return strcmp((const char*)(RString)a, (const char*)(RString)b) == 0; // case-sensitive
    return a.IsEqualTo(b);
}

GameValue A3IsEqualTo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(DeepEqual(state, oper1, oper2));
}

GameValue A3IsNotEqualTo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(!DeepEqual(state, oper1, oper2));
}

GameValue A3IsEqualType(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return GameValue(TypeKey(state, oper1) == TypeKey(state, oper2));
}

GameValue A3IsEqualTypeAll(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    if (a.Size() == 0)
        return GameValue(false);
    const std::string key = TypeKey(state, oper2);
    for (int i = 0; i < a.Size(); i++)
        if (TypeKey(state, a[i]) != key)
            return GameValue(false);
    return GameValue(true);
}

GameValue A3IsEqualTypeAny(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& types = oper2;
    const std::string key = TypeKey(state, oper1);
    for (int i = 0; i < types.Size(); i++)
        if (TypeKey(state, types[i]) == key)
            return GameValue(true);
    return GameValue(false);
}

GameValue A3IsEqualTypeArray(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (oper1.GetType() != GameArray)
        return GameValue(false);
    const GameArrayType& a = oper1;
    const GameArrayType& t = oper2;
    if (a.Size() != t.Size())
        return GameValue(false);
    for (int i = 0; i < a.Size(); i++)
        if (TypeKey(state, a[i]) != TypeKey(state, t[i]))
            return GameValue(false);
    return GameValue(true);
}

GameValue A3IsEqualTypeParams(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // like isEqualTypeArray, but `value` may be longer, and nil in the template is any
    if (oper1.GetType() != GameArray)
        return GameValue(false);
    const GameArrayType& a = oper1;
    const GameArrayType& t = oper2;
    if (a.Size() < t.Size())
        return GameValue(false);
    for (int i = 0; i < t.Size(); i++)
        if (!t[i].GetNil() && TypeKey(state, a[i]) != TypeKey(state, t[i]))
            return GameValue(false);
    return GameValue(true);
}

// ---------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------

GameValue A3JoinString(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& a = oper1;
    const std::string sep = (const char*)(RString)oper2;
    std::string out;
    for (int i = 0; i < a.Size(); i++)
    {
        if (i)
            out += sep;
        if (a[i].GetType() == GameString)
            out += (const char*)(RString)a[i];
        else
            out += (const char*)a[i].GetText();
    }
    return GameValue(RString(out.c_str()));
}

GameValue A3SplitString(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const std::string text = (const char*)(RString)oper1;
    const std::string seps = (const char*)(RString)oper2;
    GameValue result = Array(state);
    GameArrayType& out = result;
    if (seps.empty())
    {
        for (char c : text)
            out.Add(GameValue(RString(std::string(1, c).c_str())));
        return result;
    }
    std::string cur;
    for (char c : text)
    {
        if (seps.find(c) != std::string::npos)
        {
            if (!cur.empty())
                out.Add(GameValue(RString(cur.c_str())));
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty())
        out.Add(GameValue(RString(cur.c_str())));
    return result;
}

GameValue A3Trim(const GameState*, GameValuePar oper1)
{
    std::string s = (const char*)(RString)oper1;
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back()))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && ws(s[start]))
        start++;
    return GameValue(RString(s.substr(start).c_str()));
}

GameValue A3ToLowerAnsi(const GameState*, GameValuePar oper1)
{
    return GameValue(RString(Lower((const char*)(RString)oper1).c_str()));
}

GameValue A3ToUpperAnsi(const GameState*, GameValuePar oper1)
{
    return GameValue(RString(Upper((const char*)(RString)oper1).c_str()));
}

GameValue A3StringFind(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const std::string hay = (const char*)(RString)oper1;
    const std::string needle = (const char*)(RString)oper2;
    const size_t at = hay.find(needle);
    return GameValue(at == std::string::npos ? -1.0f : static_cast<float>(at));
}

GameValue A3StringFindFrom(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    // string find [needle, startIndex]
    const std::string hay = (const char*)(RString)oper1;
    const GameArrayType& a = oper2;
    if (a.Size() < 1 || a[0].GetType() != GameString)
        return GameValue(-1.0f);
    const std::string needle = (const char*)(RString)a[0];
    const size_t from = a.Size() > 1 && a[1].GetType() == GameScalar ? static_cast<size_t>(std::max(0, toInt((float)a[1]))) : 0;
    const size_t at = from <= hay.size() ? hay.find(needle, from) : std::string::npos;
    return GameValue(at == std::string::npos ? -1.0f : static_cast<float>(at));
}

GameValue A3StringIn(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const std::string needle = (const char*)(RString)oper1;
    const std::string hay = (const char*)(RString)oper2;
    return GameValue(hay.find(needle) != std::string::npos);
}

// string select [start, length]
GameValue A3StringSelectRange(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    const std::string s = (const char*)(RString)oper1;
    const GameArrayType& r = oper2;
    if (r.Size() < 1 || r[0].GetType() != GameScalar)
        return GameValue(RString(""));
    const int start = std::max(0, toInt((float)r[0]));
    if (start >= static_cast<int>(s.size()))
        return GameValue(RString(""));
    const int len = r.Size() > 1 && r[1].GetType() == GameScalar ? std::max(0, toInt((float)r[1]))
                                                                  : static_cast<int>(s.size()) - start;
    return GameValue(RString(s.substr(static_cast<size_t>(start), static_cast<size_t>(len)).c_str()));
}

GameValue A3ToFixed(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", std::clamp(toInt((float)oper2), 0, 20), (float)oper1);
    return GameValue(RString(buf));
}

GameValue A3Endl(const GameState*)
{
    return GameValue(RString("\r\n"));
}

// ---------------------------------------------------------------------------------------
// Numbers and vectors
// ---------------------------------------------------------------------------------------

GameValue A3LinearConversion(const GameState*, GameValuePar oper1)
{
    // [minFrom, maxFrom, value, minTo, maxTo, clip]
    const GameArrayType& a = oper1;
    if (a.Size() < 5)
        return GameValue(0.0f);
    const float f0 = a[0], f1 = a[1], v = a[2], t0 = a[3], t1 = a[4];
    const bool clip = a.Size() > 5 && a[5].GetType() == GameBool && (bool)a[5];
    float k = std::fabs(f1 - f0) > 1e-12f ? (v - f0) / (f1 - f0) : 0.0f;
    if (clip)
        k = std::clamp(k, 0.0f, 1.0f);
    return GameValue(t0 + (t1 - t0) * k);
}

#define VEC_BINARY(Name, Expr)                                                         \
    GameValue Name(const GameState* state, GameValuePar oper1, GameValuePar oper2)       \
    {                                                                                  \
        float a[3], b[3];                                                              \
        if (!Vec3(oper1, a) || !Vec3(oper2, b))                                        \
            return MakeVec(state, 0, 0, 0);                                            \
        Expr                                                                           \
    }

VEC_BINARY(A3VectorAdd, return MakeVec(state, a[0] + b[0], a[1] + b[1], a[2] + b[2]);)
VEC_BINARY(A3VectorDiff, return MakeVec(state, a[0] - b[0], a[1] - b[1], a[2] - b[2]);)
VEC_BINARY(A3VectorCross,
           return MakeVec(state, a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);)

GameValue A3VectorDot(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!Vec3(oper1, a) || !Vec3(oper2, b))
        return GameValue(0.0f);
    return GameValue(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

GameValue A3VectorMultiply(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float a[3];
    if (!Vec3(oper1, a))
        return MakeVec(state, 0, 0, 0);
    const float k = oper2;
    return MakeVec(state, a[0] * k, a[1] * k, a[2] * k);
}

float Length(const float a[3])
{
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

GameValue A3VectorMagnitude(const GameState*, GameValuePar oper1)
{
    float a[3];
    return GameValue(Vec3(oper1, a) ? Length(a) : 0.0f);
}

GameValue A3VectorMagnitudeSqr(const GameState*, GameValuePar oper1)
{
    float a[3];
    return GameValue(Vec3(oper1, a) ? a[0] * a[0] + a[1] * a[1] + a[2] * a[2] : 0.0f);
}

GameValue A3VectorNormalized(const GameState* state, GameValuePar oper1)
{
    float a[3];
    if (!Vec3(oper1, a))
        return MakeVec(state, 0, 0, 0);
    const float l = Length(a);
    if (l < 1e-12f)
        return MakeVec(state, 0, 0, 0);
    return MakeVec(state, a[0] / l, a[1] / l, a[2] / l);
}

GameValue A3VectorDistance(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!Vec3(oper1, a) || !Vec3(oper2, b))
        return GameValue(0.0f);
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return GameValue(Length(d));
}

GameValue A3VectorDistanceSqr(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!Vec3(oper1, a) || !Vec3(oper2, b))
        return GameValue(0.0f);
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return GameValue(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

GameValue A3VectorFromTo(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!Vec3(oper1, a) || !Vec3(oper2, b))
        return MakeVec(state, 0, 0, 0);
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float l = Length(d);
    if (l < 1e-12f)
        return MakeVec(state, 0, 0, 0);
    return MakeVec(state, d[0] / l, d[1] / l, d[2] / l);
}

GameValue A3VectorCos(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!Vec3(oper1, a) || !Vec3(oper2, b))
        return GameValue(0.0f);
    const float la = Length(a), lb = Length(b);
    if (la < 1e-12f || lb < 1e-12f)
        return GameValue(0.0f);
    return GameValue((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb));
}

GameValue A3VectorLinearConversion(const GameState* state, GameValuePar oper1)
{
    // [rangeFrom, rangeTo, value, vectorFrom, vectorTo, clip]
    const GameArrayType& a = oper1;
    float v0[3], v1[3];
    if (a.Size() < 5 || !Vec3(a[3], v0) || !Vec3(a[4], v1))
        return MakeVec(state, 0, 0, 0);
    const float f0 = a[0], f1 = a[1], v = a[2];
    float k = std::fabs(f1 - f0) > 1e-12f ? (v - f0) / (f1 - f0) : 0.0f;
    if (a.Size() > 5 && a[5].GetType() == GameBool && (bool)a[5])
        k = std::clamp(k, 0.0f, 1.0f);
    return MakeVec(state, v0[0] + (v1[0] - v0[0]) * k, v0[1] + (v1[1] - v0[1]) * k, v0[2] + (v1[2] - v0[2]) * k);
}

GameValue A3Distance2D(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!PositionOf(oper1, a) || !PositionOf(oper2, b))
        return GameValue(1e10f);
    return GameValue(std::hypot(a[0] - b[0], a[1] - b[1]));
}

GameValue A3DistanceSqr(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float a[3], b[3];
    if (!PositionOf(oper1, a) || !PositionOf(oper2, b))
        return GameValue(1e20f);
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return GameValue(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// object getRelDir position: the direction to the position, relative to the object's
float RelDir(Object* obj, const float target[3])
{
    const Vector3 p = obj->Position();
    const Vector3 dir = obj->Direction();
    const float to = std::atan2(target[0] - p.X(), target[1] - p.Z());
    const float heading = std::atan2(dir.X(), dir.Z());
    float deg = (to - heading) * 180.0f / 3.14159265f;
    deg = std::fmod(deg, 360.0f);
    if (deg < 0)
        deg += 360.0f;
    return deg;
}

GameValue A3GetRelDir(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    float t[3];
    if (!obj || !PositionOf(oper2, t))
        return GameValue(0.0f);
    return GameValue(RelDir(obj, t));
}

GameValue A3GetDirTo(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    // pos1 getDir pos2: compass direction from one to the other
    float a[3], b[3];
    if (!PositionOf(oper1, a) || !PositionOf(oper2, b))
        return GameValue(0.0f);
    float deg = std::atan2(b[0] - a[0], b[1] - a[1]) * 180.0f / 3.14159265f;
    if (deg < 0)
        deg += 360.0f;
    return GameValue(deg);
}

// object/position getPos [distance, heading]
GameValue A3GetPosRel(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float p[3];
    const GameArrayType& a = oper2;
    if (!PositionOf(oper1, p) || a.Size() < 2)
        return MakeVec(state, 0, 0, 0);
    const float dist = a[0], dir = (float)a[1] * 3.14159265f / 180.0f;
    const float x = p[0] + std::sin(dir) * dist, y = p[1] + std::cos(dir) * dist;
    return MakeVec(state, x, y, 0.0f);
}

GameValue A3GetRelPos(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // object getRelPos [distance, direction relative to the object]
    Object* obj = GetObject(oper1);
    const GameArrayType& a = oper2;
    if (!obj || a.Size() < 2)
        return MakeVec(state, 0, 0, 0);
    const Vector3 p = obj->Position();
    const Vector3 d = obj->Direction();
    const float heading = std::atan2(d.X(), d.Z());
    const float dir = heading + (float)a[1] * 3.14159265f / 180.0f;
    const float dist = a[0];
    return MakeVec(state, p.X() + std::sin(dir) * dist, p.Z() + std::cos(dir) * dist, 0.0f);
}

GameValue A3TerrainHeightASL(const GameState*, GameValuePar oper1)
{
    float p[3];
    if (!Vec3(oper1, p) || !GLandscape)
        return GameValue(0.0f);
    return GameValue(GLandscape->SurfaceY(p[0], p[1]));
}

GameValue A3WorldToModel(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    float p[3];
    if (!obj || !Vec3(oper2, p))
        return MakeVec(state, 0, 0, 0);
    // worldToModel takes AGL: lift it to world height first
    const float ground = GLandscape ? GLandscape->SurfaceYAboveWater(p[0], p[1]) : 0.0f;
    const Matrix4 inv = obj->GetInvTransform();
    Vector3 m;
    m.SetFastTransform(inv, Vector3(p[0], p[2] + ground, p[1]));
    return MakeVec(state, m.X(), m.Z(), m.Y());
}

GameValue A3VectorModelToWorld(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    float v[3];
    if (!obj || !Vec3(oper2, v))
        return MakeVec(state, 0, 0, 0);
    const Vector3 w = obj->DirectionModelToWorld(Vector3(v[0], v[2], v[1]));
    return MakeVec(state, w.X(), w.Z(), w.Y());
}

GameValue A3VectorWorldToModel(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    float v[3];
    if (!obj || !Vec3(oper2, v))
        return MakeVec(state, 0, 0, 0);
    const Matrix4 inv = obj->GetInvTransform();
    Vector3 m;
    m.SetRotate(inv, Vector3(v[0], v[2], v[1]));
    return MakeVec(state, m.X(), m.Z(), m.Y());
}

// position inArea [center, a, b, angle, isRectangle, c] / inArea marker / inArea trigger
bool InArea(const GameState* state, const float p[3], GameValuePar area)
{
    float c[3] = {0, 0, 0};
    float a = 0, b = 0, angle = 0;
    bool rect = false;
    if (area.GetType() == GameArray)
    {
        const GameArrayType& s = area;
        if (s.Size() < 3 || !PositionOf(s[0], c))
            return false;
        a = s[1];
        b = s[2];
        angle = s.Size() > 3 ? (float)s[3] : 0.0f;
        rect = s.Size() > 4 && s[4].GetType() == GameBool && (bool)s[4];
    }
    else if (area.GetType() == GameString)
    {
        // a marker: its position, size, direction and shape
        GameState* gs = const_cast<GameState*>(state);
        GameVarSpace local(state->GetContext());
        state->BeginContext(&local);
        state->VarSetLocal("_m", area, false, true);
        const GameValue info = gs->EvaluateMultiple(
            "[getMarkerPos _m, getMarkerSize _m, markerDir _m, markerShape _m]");
        state->EndContext();
        if (info.GetType() != GameArray)
            return false;
        const GameArrayType& i = info;
        if (i.Size() < 4 || !PositionOf(i[0], c) || i[1].GetType() != GameArray)
            return false;
        const GameArrayType& size = i[1];
        a = size.Size() > 0 ? (float)size[0] : 0.0f;
        b = size.Size() > 1 ? (float)size[1] : 0.0f;
        angle = i[2].GetType() == GameScalar ? (float)i[2] : 0.0f;
        rect = i[3].GetType() == GameString && _stricmp((const char*)(RString)i[3], "RECTANGLE") == 0;
    }
    else if (area.GetType() == GameObject)
    {
        // a trigger: [position, a, b, angle, rectangular] from triggerArea
        GameVarSpace local(state->GetContext());
        state->BeginContext(&local);
        state->VarSetLocal("_t", area, false, true);
        const GameValue info = const_cast<GameState*>(state)->EvaluateMultiple("[getPosASL _t] + triggerArea _t");
        state->EndContext();
        if (info.GetType() != GameArray)
            return false;
        const GameArrayType& i = info;
        if (i.Size() < 5 || !PositionOf(i[0], c))
            return false;
        a = i[1];
        b = i[2];
        angle = i[3];
        rect = i[4].GetType() == GameBool && (bool)i[4];
    }
    else
        return false;
    const float rad = -angle * 3.14159265f / 180.0f;
    const float dx = p[0] - c[0], dy = p[1] - c[1];
    const float x = dx * std::cos(rad) - dy * std::sin(rad);
    const float y = dx * std::sin(rad) + dy * std::cos(rad);
    if (a <= 0 || b <= 0)
        return false;
    if (rect)
        return std::fabs(x) <= a && std::fabs(y) <= b;
    return (x * x) / (a * a) + (y * y) / (b * b) <= 1.0f;
}

GameValue A3InArea(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    float p[3];
    if (!PositionOf(oper1, p))
        return GameValue(false);
    return GameValue(InArea(state, p, oper2));
}

GameValue A3InAreaArray(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& items = oper1;
    GameValue result = Array(state);
    GameArrayType& out = result;
    for (int i = 0; i < items.Size(); i++)
    {
        float p[3];
        if (PositionOf(items[i], p) && InArea(state, p, oper2))
            out.Add(items[i]);
    }
    return result;
}

GameValue A3InPolygon(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    float p[3];
    if (!PositionOf(oper1, p))
        return GameValue(false);
    const GameArrayType& poly = oper2;
    bool inside = false;
    for (int i = 0, j = poly.Size() - 1; i < poly.Size(); j = i++)
    {
        float a[3], b[3];
        if (!Vec3(poly[i], a) || !Vec3(poly[j], b))
            continue;
        if (((a[1] > p[1]) != (b[1] > p[1])) && (p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]))
            inside = !inside;
    }
    return GameValue(inside);
}

// ---------------------------------------------------------------------------------------
// Markers (the mission's marker list, markersMap)
// ---------------------------------------------------------------------------------------

const ArcadeMarkerInfo* FindMarker(GameValuePar name)
{
    const RString n = name;
    for (int i = 0; i < markersMap.Size(); i++)
        if (_stricmp(markersMap[i].name, n) == 0)
            return &markersMap[i];
    return nullptr;
}

GameValue A3MarkerShape(const GameState*, GameValuePar oper1)
{
    const ArcadeMarkerInfo* m = FindMarker(oper1);
    const char* shape = !m                                ? ""
                        : m->markerType == MTRectangle    ? "RECTANGLE"
                        : m->markerType == MTEllipse      ? "ELLIPSE"
                                                          : "ICON";
    return GameValue(RString(shape));
}

GameValue A3MarkerText(const GameState*, GameValuePar oper1)
{
    const ArcadeMarkerInfo* m = FindMarker(oper1);
    return GameValue(m ? m->text : RString(""));
}

GameValue A3MarkerBrush(const GameState*, GameValuePar oper1)
{
    const ArcadeMarkerInfo* m = FindMarker(oper1);
    return GameValue(m ? m->fillName : RString(""));
}

GameValue A3AllMapMarkers(const GameState* state)
{
    GameValue result = Array(state);
    GameArrayType& out = result;
    for (int i = 0; i < markersMap.Size(); i++)
        out.Add(GameValue(markersMap[i].name));
    return result;
}

// ---------------------------------------------------------------------------------------
// Hash maps
// ---------------------------------------------------------------------------------------

const GameType GameHashMap(0x8000);

struct HashEntry
{
    GameValue key;
    GameValue value;
};

class GameDataHashMap : public GameData
{
  public:
    std::map<std::string, HashEntry> entries;
    bool readOnly = false;

    GameType GetType() const override { return GameHashMap; }
    RString GetText() const override
    {
        std::string s = "[";
        bool first = true;
        for (const auto& [k, e] : entries)
        {
            s += std::string(first ? "" : ",") + "[" + (const char*)e.key.GetText() + "," + (const char*)e.value.GetText() + "]";
            first = false;
        }
        return RString((s + "]").c_str());
    }
    bool IsEqualTo(const GameData* data) const override { return data == this; }
    const char* GetTypeName() const override { return "hashmap"; }
    GameData* Clone() const override { return new GameDataHashMap(*this); }
};

GameDataHashMap* MapOf(GameValuePar v)
{
    if (v.GetType() != GameHashMap)
        return nullptr;
    return static_cast<GameDataHashMap*>(v.GetData());
}

// Arma 3 keys strings case-sensitively and compares numbers, sides, objects... by value.
std::string HashKey(const GameState* state, GameValuePar key)
{
    std::string k = TypeKey(state, key) + ":";
    if (key.GetType() == GameString)
        k += (const char*)(RString)key;
    else
        k += (const char*)key.GetText();
    return k;
}

GameData* CreateGameDataHashMap()
{
    return new GameDataHashMap();
}

GameValue A3CreateHashMap(const GameState*)
{
    return GameValue(new GameDataHashMap());
}

GameValue A3CreateHashMapFromArray(const GameState* state, GameValuePar oper1)
{
    auto* map = new GameDataHashMap();
    const GameArrayType& a = oper1;
    for (int i = 0; i < a.Size(); i++)
    {
        if (a[i].GetType() != GameArray)
            continue;
        const GameArrayType& pair = a[i];
        if (pair.Size() < 1)
            continue;
        map->entries[HashKey(state, pair[0])] = {pair[0], pair.Size() > 1 ? pair[1] : Nil()};
    }
    return GameValue(map);
}

GameValue A3CreateHashMapFromKeys(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    // keys createHashMapFromArray values
    auto* map = new GameDataHashMap();
    const GameArrayType& k = oper1;
    const GameArrayType& v = oper2;
    for (int i = 0; i < k.Size(); i++)
        map->entries[HashKey(state, k[i])] = {k[i], i < v.Size() ? v[i] : Nil()};
    return GameValue(map);
}

GameValue A3HashGet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    if (!map)
        return Nil();
    const auto it = map->entries.find(HashKey(state, oper2));
    return it == map->entries.end() ? Nil() : it->second.value;
}

GameValue A3HashGetOrDefault(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    const GameArrayType& a = oper2;
    if (!map || a.Size() < 1)
        return Nil();
    const auto it = map->entries.find(HashKey(state, a[0]));
    if (it != map->entries.end())
        return it->second.value;
    const GameValue fallback = a.Size() > 1 ? a[1] : Nil();
    if (a.Size() > 2 && a[2].GetType() == GameBool && (bool)a[2])
        map->entries[HashKey(state, a[0])] = {a[0], fallback};
    return fallback;
}

GameValue A3HashSet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    const GameArrayType& a = oper2;
    if (!map || a.Size() < 2)
        return GameValue(false);
    const std::string key = HashKey(state, a[0]);
    const bool existed = map->entries.count(key) > 0;
    const bool insertOnly = a.Size() > 2 && a[2].GetType() == GameBool && (bool)a[2];
    if (existed && insertOnly)
        return GameValue(true);
    map->entries[key] = {a[0], a[1]};
    return GameValue(existed);
}

GameValue A3HashKeys(const GameState* state, GameValuePar oper1)
{
    GameValue result = Array(state);
    GameArrayType& out = result;
    if (GameDataHashMap* map = MapOf(oper1))
        for (const auto& [k, e] : map->entries)
            out.Add(e.key);
    return result;
}

GameValue A3HashValues(const GameState* state, GameValuePar oper1)
{
    GameValue result = Array(state);
    GameArrayType& out = result;
    if (GameDataHashMap* map = MapOf(oper1))
        for (const auto& [k, e] : map->entries)
            out.Add(e.value);
    return result;
}

GameValue A3HashCount(const GameState*, GameValuePar oper1)
{
    GameDataHashMap* map = MapOf(oper1);
    return GameValue(map ? static_cast<float>(map->entries.size()) : 0.0f);
}

GameValue A3HashIn(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper2);
    return GameValue(map && map->entries.count(HashKey(state, oper1)) > 0);
}

GameValue A3HashToArray(const GameState* state, GameValuePar oper1)
{
    // [keys, values]
    GameValue result = Array(state);
    GameArrayType& out = result;
    out.Add(A3HashKeys(state, oper1));
    out.Add(A3HashValues(state, oper1));
    return result;
}

GameValue A3HashMerge(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* dst = MapOf(oper1);
    GameDataHashMap* src = nullptr;
    bool overwrite = false;
    if (oper2.GetType() == GameArray)
    {
        const GameArrayType& a = oper2;
        if (a.Size() > 0)
            src = MapOf(a[0]);
        overwrite = a.Size() > 1 && a[1].GetType() == GameBool && (bool)a[1];
    }
    else
        src = MapOf(oper2);
    if (!dst || !src)
        return NOTHING;
    for (const auto& [k, e] : src->entries)
        if (overwrite || !dst->entries.count(k))
            dst->entries[k] = e;
    return NOTHING;
}

GameValue A3HashForEach(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper2);
    if (!map)
        return NOTHING;
    const RString code = oper1;
    const auto copy = map->entries; // the code may change the map
    for (const auto& [k, e] : copy)
    {
        RunWithX(state, code, e.key, &e.value);
        if (Failed(state))
            break;
    }
    return NOTHING;
}

GameValue A3HashApply(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    GameValue result = Array(state);
    GameArrayType& out = result;
    if (!map)
        return result;
    const RString code = oper2;
    const auto copy = map->entries;
    for (const auto& [k, e] : copy)
    {
        const GameValue v = RunWithX(state, code, e.key, &e.value);
        if (Failed(state))
            break;
        out.Add(v);
    }
    return result;
}

GameValue A3HashSelectFilter(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    auto* out = new GameDataHashMap();
    GameValue result(out);
    if (!map)
        return result;
    const RString code = oper2;
    const auto copy = map->entries;
    for (const auto& [k, e] : copy)
    {
        const GameValue v = RunWithX(state, code, e.key, &e.value);
        if (Failed(state))
            break;
        if (v.GetType() == GameBool && (bool)v)
            out->entries[k] = e;
    }
    return result;
}

GameValue A3HashFindIf(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameDataHashMap* map = MapOf(oper1);
    if (!map)
        return Nil();
    const RString code = oper2;
    const auto copy = map->entries;
    for (const auto& [k, e] : copy)
    {
        const GameValue v = RunWithX(state, code, e.key, &e.value);
        if (Failed(state))
            break;
        if (v.GetType() == GameBool && (bool)v)
            return e.key;
    }
    return Nil();
}

GameValue A3DeleteAtImpl(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (GameDataHashMap* map = MapOf(oper1))
    {
        const auto it = map->entries.find(HashKey(state, oper2));
        if (it == map->entries.end())
            return Nil();
        const GameValue v = it->second.value;
        map->entries.erase(it);
        return v;
    }
    if (oper1.GetType() != GameArray || oper2.GetType() != GameScalar || !Writable(state, oper1))
        return Nil();
    GameArrayType& a = MutableArray(oper1);
    const int i = toInt((float)oper2);
    if (i < 0 || i >= a.Size())
        return Nil();
    const GameValue v = a[i];
    a.Delete(i);
    return v;
}

GameValue A3CountHashOrString(const GameState* state, GameValuePar oper1)
{
    return A3HashCount(state, oper1);
}

// ---------------------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------------------

GameValue A3CompileFinal(const GameState*, GameValuePar oper1)
{
    if (oper1.GetType() == GameString)
        return Code((RString)oper1);
    return oper1;
}

// Every entity of the mission -- units, vehicles, objects, logics -- for allMissionObjects
// (allUnits leaves out logics and empty objects).
GameValue A3AllEntities(const GameState* state)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& array = result;
    if (!GWorld)
        return result;
    const auto add = [&](Entity* e) {
        if (e && !e->IsDammageDestroyed() && dyn_cast<EntityAI>(e))
            array.Add(GameValueExt(static_cast<Object*>(e)));
    };
    for (int i = 0; i < GWorld->NVehicles(); i++)
        add(GWorld->GetVehicle(i));
    for (int i = 0; i < GWorld->NOutVehicles(); i++)
        add(GWorld->GetOutVehicle(i));
    return result;
}

GameValue A3ScriptNull(const GameState*)
{
    // script handles are numbers here; -1 is no script (scriptDone -1 is true)
    return GameValue(-1.0f);
}

// a && {b} / a || {b}: the right side runs only when it decides the result
GameValue RunCode(const GameState* state, const RString& code)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    const GameValue r = state->EvaluateMultiple(code);
    state->EndContext();
    return r;
}

GameValue A3LazyAnd(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if (!(bool)oper1)
        return GameValue(false);
    const GameValue r = RunCode(state, (RString)oper2);
    return GameValue(r.GetType() == GameBool && (bool)r);
}

GameValue A3LazyOr(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    if ((bool)oper1)
        return GameValue(true);
    const GameValue r = RunCode(state, (RString)oper2);
    return GameValue(r.GetType() == GameBool && (bool)r);
}

// ---------------------------------------------------------------------------------------
// Mission event handlers (addMissionEventHandler)
// ---------------------------------------------------------------------------------------

struct MissionHandler
{
    int id;
    std::string type; // lower case
    RString code;
    int errors = 0;
};

struct MissionHandlers
{
    std::string mission;
    std::vector<MissionHandler> list;
    int next = 0;
};

MissionHandlers& Handlers()
{
    static MissionHandlers h;
    return h;
}

GameValue A3AddMissionEventHandler(const GameState*, GameValuePar oper1)
{
    // [type, code(, arguments)]
    const GameArrayType& a = oper1;
    if (a.Size() < 2 || a[0].GetType() != GameString)
        return GameValue(-1.0f);
    MissionHandlers& h = Handlers();
    MissionHandler handler;
    handler.id = h.next++;
    handler.type = Lower((const char*)(RString)a[0]);
    handler.code = a[1].GetType() == GameString ? (RString)a[1] : RString("");
    h.list.push_back(handler);
    return GameValue(static_cast<float>(handler.id));
}

GameValue A3RemoveMissionEventHandler(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 2 || a[0].GetType() != GameString || a[1].GetType() != GameScalar)
        return NOTHING;
    const std::string type = Lower((const char*)(RString)a[0]);
    const int id = toInt((float)a[1]);
    auto& list = Handlers().list;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const MissionHandler& m) { return m.id == id && m.type == type; }),
               list.end());
    return NOTHING;
}

GameValue A3RemoveAllMissionEventHandlers(const GameState*, GameValuePar oper1)
{
    const std::string type = Lower((const char*)(RString)oper1);
    auto& list = Handlers().list;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const MissionHandler& m) { return m.type == type; }), list.end());
    return NOTHING;
}

GameValue A3True(const GameState*)
{
    return GameValue(true);
}

GameValue A3False(const GameState*)
{
    return GameValue(false);
}

GameValue A3Two(const GameState*)
{
    return GameValue(2.0f);
}

GameValue A3TwoUnary(const GameState*, GameValuePar)
{
    return GameValue(2.0f);
}

GameValue A3CanSuspend(const GameState*)
{
    // a scheduled script (spawn / execVM / a coroutine) may suspend
    return GameValue(GameState::s_scheduledYields != nullptr);
}

// ---------------------------------------------------------------------------------------
// "Macro" commands: Arma 3 commands as SQF over this engine's commands
// ---------------------------------------------------------------------------------------

struct Macro
{
    const char* name;
    char kind; // 'n' nular, 'u' unary, 'b' binary
    GameType retType;
    GameType left;  // binary: left operand
    GameType right; // unary: operand; binary: right operand
    const char* code;
};

// _this: the operand (unary), [left, right] (binary), nothing (nular).
const Macro kMacros[] = {
    // positions
    {"setPosWorld", 'b', GameNothing, GameObject, GameArray, "(_this select 0) setPosASL (_this select 1)"},
    {"getPosWorld", 'u', GameArray, 0, GameObject, "getPosASL _this"},
    {"getPosWorldVisual", 'u', GameArray, 0, GameObject, "getPosASL _this"},
    {"getPosASLVisual", 'u', GameArray, 0, GameObject, "getPosASL _this"},
    {"visiblePositionASL", 'u', GameArray, 0, GameObject, "getPosASL _this"},
    {"getPosATLVisual", 'u', GameArray, 0, GameObject, "getPosATL _this"},
    {"getPosVisual", 'u', GameArray, 0, GameObject, "getPos _this"},
    {"visiblePosition", 'u', GameArray, 0, GameObject, "getPos _this"},
    {"getDirVisual", 'u', GameScalar, 0, GameObject, "getDir _this"},
    {"vectorDirVisual", 'u', GameArray, 0, GameObject, "vectorDir _this"},
    {"vectorUpVisual", 'u', GameArray, 0, GameObject, "vectorUp _this"},
    {"modelToWorldVisual", 'b', GameArray, GameObject, GameArray, "(_this select 0) modelToWorld (_this select 1)"},
    {"modelToWorldWorld", 'b', GameArray, GameObject, GameArray,
     "AGLToASL ((_this select 0) modelToWorld (_this select 1))"},
    {"modelToWorldVisualWorld", 'b', GameArray, GameObject, GameArray,
     "AGLToASL ((_this select 0) modelToWorld (_this select 1))"},
    {"worldToModelVisual", 'b', GameArray, GameObject, GameArray, "(_this select 0) worldToModel (_this select 1)"},
    {"AGLToASL", 'u', GameArray, 0, GameArray,
     "[_this select 0, _this select 1, ((_this + [0]) select 2) + (getTerrainHeightASL _this max 0)]"},
    {"ASLToAGL", 'u', GameArray, 0, GameArray,
     "[_this select 0, _this select 1, ((_this + [0]) select 2) - (getTerrainHeightASL _this max 0)]"},
    {"ATLToASL", 'u', GameArray, 0, GameArray,
     "[_this select 0, _this select 1, ((_this + [0]) select 2) + getTerrainHeightASL _this]"},
    {"ASLToATL", 'u', GameArray, 0, GameArray,
     "[_this select 0, _this select 1, ((_this + [0]) select 2) - getTerrainHeightASL _this]"},
    {"eyePos", 'u', GameArray, 0, GameObject,
     "private _p = getPosASL _this; [_p select 0, _p select 1, (_p select 2) + (if (_this isKindOf 'Man') then {1.6} else {1.0})]"},
    {"aimPos", 'u', GameArray, 0, GameObject, "private _p = getPosASL _this; [_p select 0, _p select 1, (_p select 2) + 1.2]"},
    {"boundingBoxReal", 'u', GameArray, 0, GameObject, "boundingBox _this"},
    {"boundingCenter", 'u', GameArray, 0, GameObject, "[0,0,0]"},
    {"isTouchingGround", 'u', GameBool, 0, GameObject, "((getPosATL _this) select 2) < 0.5"},
    {"surfaceNormal", 'u', GameArray, 0, GameArray, "[0,0,1]"},
    {"setVehiclePosition", 'b', GameNothing, GameObject, GameArray,
     "(_this select 0) setPos (((_this select 1) select 0) call {if (typeName _this == 'STRING') then {getMarkerPos _this} else {_this}})"},
    {"setVelocityModelSpace", 'b', GameNothing, GameObject, GameArray,
     "(_this select 0) setVelocity ((_this select 0) vectorModelToWorld (_this select 1))"},
    {"velocityModelSpace", 'u', GameArray, 0, GameObject, "_this vectorWorldToModel (velocity _this)"},
    {"positionCameraToWorld", 'u', GameArray, 0, GameArray,
     "private _p = getPos (vehicle player); [_p select 0, _p select 1, (_p select 2) + 1.6]"},
    {"screenToWorld", 'u', GameArray, 0, GameArray, "getPos (vehicle player)"},
    {"worldToScreen", 'u', GameArray, 0, GameArray, "[]"},
    {"getTerrainGrid", 'n', GameScalar, 0, 0, "12.5"},

    // objects, world
    {"hideObjectGlobal", 'b', GameNothing, GameObject, GameBool, "(_this select 0) hideObject (_this select 1)"},
    {"hideObjectGlobal", 'u', GameNothing, 0, GameObject, "_this hideObject true"},
    {"enableSimulationGlobal", 'b', GameNothing, GameObject, GameBool,
     "(_this select 0) enableSimulation (_this select 1)"},
    {"animateSource", 'b', GameNothing, GameObject, GameArray,
     "(_this select 0) animate [(_this select 1) select 0, (_this select 1) select 1]"},
    {"animateDoor", 'b', GameNothing, GameObject, GameArray,
     "(_this select 0) animate [(_this select 1) select 0, (_this select 1) select 1]"},
    {"animationSourcePhase", 'b', GameScalar, GameObject, GameString, "(_this select 0) animationPhase (_this select 1)"},
    {"doorPhase", 'b', GameScalar, GameObject, GameString, "(_this select 0) animationPhase (_this select 1)"},
    {"objectParent", 'u', GameObject, 0, GameObject, "if (vehicle _this == _this) then {objNull} else {vehicle _this}"},
    // enableEnvironment [ambient life, ambient sound] (Arma 3's array form); one switch here
    {"enableEnvironment", 'u', GameNothing, 0, GameArray, "enableEnvironment ((_this select 0) || (_this select 1))"},
    // "enableWeaponDisassembly false" without its object (seen in Eden init lines): nothing to do
    {"enableWeaponDisassembly", 'u', GameNothing, 0, GameBool, ""},
    {"lifeState", 'u', GameString, 0, GameObject, "if (alive _this) then {'HEALTHY'} else {'DEAD'}"},
    {"incapacitatedState", 'u', GameString, 0, GameObject, "''"},
    {"getHitPointDamage", 'b', GameScalar, GameObject, GameString, "damage (_this select 0)"},
    {"getHitIndex", 'b', GameScalar, GameObject, GameScalar, "damage (_this select 0)"},
    {"cursorObject", 'n', GameObject, 0, 0, "cursorTarget"},
    // parseNumber true / false (BIS_fnc_initDisplay: parseNumber _register)
    {"parseNumber", 'u', GameScalar, 0, GameBool, "if (_this) then {1} else {0}"},
    // compileScript [path, final, prefix]: the file, preprocessed, behind its prefix
    {"compileScript", 'u', GameAny, 0, GameArray,
     "compile ((_this param [2, '']) + (preprocessFileLineNumbers (_this select 0)))"},
    {"allPlayers", 'n', GameArray, 0, 0, "if (isNull player) then {[]} else {[player]}"},
    {"allDeadMen", 'n', GameArray, 0, 0, "[]"},
    {"allDead", 'n', GameArray, 0, 0, "[]"},
    {"playableSlotsNumber", 'u', GameScalar, 0, GameSide, "0"},
    {"fullCrew", 'u', GameArray, 0, GameObject,
     "private _v = _this; private _r = []; "
     "if (!isNull driver _v) then {_r pushBack [driver _v, 'driver', -1, [], false]}; "
     "if (!isNull gunner _v) then {_r pushBack [gunner _v, 'gunner', -1, [0], false]}; "
     "if (!isNull commander _v) then {_r pushBack [commander _v, 'commander', -1, [0,0], false]}; "
     "private _n = 0; {if !(_x in [driver _v, gunner _v, commander _v]) then {_r pushBack [_x, 'cargo', _n, [], false]; _n = _n + 1}} forEach crew _v; _r"},
    {"fullCrew", 'b', GameArray, GameObject, GameArray,
     "private _all = fullCrew (_this select 0); private _role = toLower (((_this select 1) + ['']) select 0); "
     "if (_role == '') then {_all} else {_all select {(_x select 1) == _role}}"},
    {"createVehicleCrew", 'u', GameGroup, 0, GameObject,
     "private _v = _this; private _cls = getText (configFile >> 'CfgVehicles' >> typeOf _v >> 'crew'); "
     "private _sn = getNumber (configFile >> 'CfgVehicles' >> typeOf _v >> 'side'); "
     "private _side = [east, west, resistance, civilian] select ((_sn max 0) min 3); "
     "if (_cls == '') then {_cls = ['O_crew_F','B_crew_F','I_crew_F','C_man_1'] select ((_sn max 0) min 3)}; "
     "private _g = if (isNull group driver _v) then {createGroup _side} else {group driver _v}; "
     "if (isNull driver _v) then {private _u = _g createUnit [_cls, getPos _v, [], 0, 'NONE']; _u moveInDriver _v}; "
     "if (isNull gunner _v) then {private _u = _g createUnit [_cls, getPos _v, [], 0, 'NONE']; _u moveInGunner _v; if (vehicle _u == _u) then {deleteVehicle _u}}; "
     "if (isNull commander _v) then {private _u = _g createUnit [_cls, getPos _v, [], 0, 'NONE']; _u moveInCommander _v; if (vehicle _u == _u) then {deleteVehicle _u}}; "
     "_g"},
    {"deleteVehicleCrew", 'b', GameNothing, GameObject, GameObject, "deleteVehicle (_this select 1)"},
    {"deleteVehicleCrew", 'u', GameNothing, 0, GameObject, "{deleteVehicle _x} forEach crew _this"},
    {"moveInAny", 'b', GameBool, GameObject, GameObject,
     "private _u = _this select 0; private _v = _this select 1; "
     "if (isNull driver _v) then {_u moveInDriver _v} else {if (isNull gunner _v) then {_u moveInGunner _v} else {_u moveInCargo _v}}; "
     "vehicle _u == _v"},
    {"assignAsCargoIndex", 'b', GameNothing, GameObject, GameArray, "(_this select 0) assignAsCargo ((_this select 1) select 0)"},
    {"moveInCargoIndex", 'b', GameNothing, GameObject, GameArray, "(_this select 0) moveInCargo ((_this select 1) select 0)"},
    {"createSimpleObject", 'u', GameObject, 0, GameArray,
     "private _c = _this select 0; private _p = _this select 1; "
     "if (isClass (configFile >> 'CfgVehicles' >> _c)) then {private _o = createVehicle [_c, [_p select 0, _p select 1, 0], [], 0, 'CAN_COLLIDE']; _o setPosASL _p; _o} else {objNull}"},
    {"createSimpleObject", 'b', GameObject, GameString, GameArray,
     "private _o = createVehicle [_this select 0, [0,0,0], [], 0, 'CAN_COLLIDE']; _o setPosASL (_this select 1); _o"},
    {"isSimpleObject", 'u', GameBool, 0, GameObject, "false"},
    {"createMine", 'u', GameObject, 0, GameArray,
     "createVehicle [_this select 0, _this select 1, (_this + [[]]) select 2, (_this + [[], 0]) select 3, 'NONE']"},
    {"mineActive", 'u', GameBool, 0, GameObject, "alive _this"},
    {"say3D", 'b', GameNothing, GameObject, GameString | GameArray,
     "(_this select 0) say (if (typeName (_this select 1) == 'ARRAY') then {(_this select 1) select 0} else {_this select 1})"},
    {"say2D", 'b', GameNothing, GameObject, GameString | GameArray,
     "(_this select 0) say (if (typeName (_this select 1) == 'ARRAY') then {(_this select 1) select 0} else {_this select 1})"},
    {"systemChat", 'u', GameNothing, 0, GameString, "if (!isNull player) then {player globalChat _this}"},
    {"limitSpeed", 'b', GameNothing, GameObject, GameScalar, "(_this select 0) forceSpeed ((_this select 1) / 3.6)"},
    {"setUnitPosWeak", 'b', GameNothing, GameObject, GameString, "(_this select 0) setUnitPos (_this select 1)"},
    {"setBehaviourStrong", 'b', GameNothing, GameObject | GameGroup, GameString,
     "(_this select 0) setBehaviour (_this select 1)"},
    {"setGroupIdGlobal", 'b', GameNothing, GameGroup, GameArray, "(_this select 0) setGroupId (_this select 1)"},
    {"joinAsSilent", 'b', GameNothing, GameObject, GameArray, "[_this select 0] joinSilent ((_this select 1) select 0)"},
    {"groupId", 'u', GameString, 0, GameGroup, "str _this"},
    {"units", 'u', GameArray, 0, GameSide, "private _s = _this; allUnits select {side _x == _s}"},
    {"groups", 'u', GameArray, 0, GameSide, "private _s = _this; allGroups select {side _x == _s}"},
    {"allUnitsUAV", 'n', GameArray, 0, 0, "[]"},
    {"vehicles", 'n', GameArray, 0, 0,
     "private _r = []; {if (vehicle _x != _x) then {_r pushBackUnique vehicle _x}} forEach allUnits; _r"},
    {"entities", 'u', GameArray, 0, GameString,
     "private _t = _this; private _r = []; {_r pushBackUnique vehicle _x; _r pushBackUnique _x} forEach allUnits; _r select {_x isKindOf _t}"},
    {"entities", 'u', GameArray, 0, GameArray,
     "private _t = (_this select 0) apply {_x}; private _r = []; {_r pushBackUnique vehicle _x; _r pushBackUnique _x} forEach allUnits; "
     "if (count _t == 0) then {_r} else {_r select {private _o = _x; _t findIf {_o isKindOf _x} >= 0}}"},
    {"allMissionObjects", 'u', GameArray, 0, GameString,
     "private _t = _this; private _r = opA3AllEntities; {_r pushBackUnique vehicle _x; _r pushBackUnique _x} forEach allUnits; if (_t == '') then {_r} else {_r select {_x isKindOf _t}}"},
    {"nearestTerrainObjects", 'u', GameArray, 0, GameArray, "[]"},
    {"nearestTerrainObjects", 'b', GameArray, GameObject | GameArray, GameArray, "[]"},
    {"findEmptyPosition", 'b', GameArray, GameArray | GameObject, GameArray,
     "private _p = _this select 0; if (typeName _p == 'OBJECT') then {getPos _p} else {_p}"},
    {"isFlatEmpty", 'b', GameArray, GameArray, GameArray, "_this select 0"},
    {"selectBestPlaces", 'u', GameArray, 0, GameArray, "[[_this select 0, 1]]"},
    {"lineIntersects", 'u', GameBool, 0, GameArray, "false"},
    {"lineIntersectsWith", 'u', GameArray, 0, GameArray, "[]"},
    {"lineIntersectsObjs", 'u', GameArray, 0, GameArray, "[]"},
    {"lineIntersectsSurfaces", 'u', GameArray, 0, GameArray, "[]"},
    {"terrainIntersect", 'u', GameBool, 0, GameArray, "false"},
    {"terrainIntersectASL", 'u', GameBool, 0, GameArray, "false"},
    {"terrainIntersectAtASL", 'u', GameArray, 0, GameArray, "[0,0,0]"},
    {"checkVisibility", 'b', GameScalar, GameObject | GameArray, GameArray, "1"},
    {"intersect", 'b', GameArray, GameArray, GameArray, "[]"},

    // inventory: what this engine can carry is weapons and magazines
    {"addItem", 'b', GameNothing, GameObject, GameString,
     "if (isClass (configFile >> 'CfgMagazines' >> (_this select 1))) then {(_this select 0) addMagazine (_this select 1)}"},
    {"addItemToUniform", 'b', GameNothing, GameObject, GameString,
     "if (isClass (configFile >> 'CfgMagazines' >> (_this select 1))) then {(_this select 0) addMagazine (_this select 1)}"},
    {"addItemToVest", 'b', GameNothing, GameObject, GameString,
     "if (isClass (configFile >> 'CfgMagazines' >> (_this select 1))) then {(_this select 0) addMagazine (_this select 1)}"},
    {"addItemToBackpack", 'b', GameNothing, GameObject, GameString,
     "if (isClass (configFile >> 'CfgMagazines' >> (_this select 1))) then {(_this select 0) addMagazine (_this select 1)}"},
    {"addMagazines", 'b', GameNothing, GameObject, GameArray,
     "for '_i' from 1 to (((_this select 1) select 1) min 20) do {(_this select 0) addMagazine ((_this select 1) select 0)}"},
    {"addMagazineGlobal", 'b', GameNothing, GameObject, GameString, "(_this select 0) addMagazine (_this select 1)"},
    {"addWeaponGlobal", 'b', GameNothing, GameObject, GameString, "(_this select 0) addWeapon (_this select 1)"},
    {"removeMagazineGlobal", 'b', GameNothing, GameObject, GameString, "(_this select 0) removeMagazine (_this select 1)"},
    {"removeWeaponGlobal", 'b', GameNothing, GameObject, GameString, "(_this select 0) removeWeapon (_this select 1)"},
    {"handgunWeapon", 'u', GameString, 0, GameObject, "''"},
    {"binocular", 'u', GameString, 0, GameObject, "''"},
    {"currentMagazine", 'u', GameString, 0, GameObject, "private _m = magazines _this; if (count _m > 0) then {_m select 0} else {''}"},
    {"primaryWeaponMagazine", 'u', GameArray, 0, GameObject, "private _m = magazines _this; if (count _m > 0) then {[_m select 0]} else {[]}"},
    {"magazinesAmmo", 'u', GameArray, 0, GameObject, "(magazines _this) apply {[_x, 30]}"},
    {"magazinesAmmoFull", 'u', GameArray, 0, GameObject, "(magazines _this) apply {[_x, 30, false, -1, '']}"},
    {"getUnitLoadout", 'u', GameArray, 0, GameObject,
     "private _w = weapons _this; [[(_w + ['']) select 0, '', '', '', [], [], ''], [], [], [], [], [], '', '', [], ['','','','','','']]"},
    {"getUnitLoadout", 'u', GameArray, 0, GameString,
     "[[], [], [], [], [], [], '', '', [], ['','','','','','']]"},
    {"uiSleep", 'u', GameNothing, 0, GameScalar, "sleep _this"},
    {"enginesIsOnRTD", 'u', GameBool, 0, GameObject, "isEngineOn _this"},
    {"getMissionLayerEntities", 'u', GameArray, 0, GameString | GameScalar,
     "private _n = toLower (if (typeName _this == 'STRING') then {_this} else {str _this}); "
     "[(missionNamespace getVariable ['op_eden_layer_' + _n, []]) select {!isNull _x}, "
     "missionNamespace getVariable ['op_eden_layer_markers_' + _n, []]]"},
};

template <size_t I>
GameValue MacroNular(const GameState* state)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    const GameValue r = state->EvaluateMultiple(kMacros[I].code);
    state->EndContext();
    return r;
}

template <size_t I>
GameValue MacroUnary(const GameState* state, GameValuePar oper1)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    state->VarSetLocal("_this", oper1, false, true);
    const GameValue r = state->EvaluateMultiple(kMacros[I].code);
    state->EndContext();
    return r;
}

template <size_t I>
GameValue MacroBinary(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    GameArrayType args;
    args.Add(oper1);
    args.Add(oper2);
    state->VarSetLocal("_this", GameValue(args), false, true);
    const GameValue r = state->EvaluateMultiple(kMacros[I].code);
    state->EndContext();
    return r;
}

constexpr size_t kMacroCount = sizeof(kMacros) / sizeof(kMacros[0]);

template <size_t... I>
void RegisterMacros(std::index_sequence<I...>)
{
    static const ProcNular nulars[] = {&MacroNular<I>...};
    static const ProcUnary unaries[] = {&MacroUnary<I>...};
    static const ProcBinary binaries[] = {&MacroBinary<I>...};
    for (size_t i = 0; i < sizeof...(I); i++)
    {
        const Macro& m = kMacros[i];
        if (m.kind == 'n')
            GGameState.NewNularOp(GameNular(m.retType, m.name, nulars[i]));
        else if (m.kind == 'u')
            GGameState.NewFunction(GameFunction(m.retType, m.name, unaries[i], m.right));
        else
            GGameState.NewOperator(GameOperator(m.retType, m.name, function, binaries[i], m.left, m.right));
    }
}


// ---------------------------------------------------------------------------------------
// More macros
// ---------------------------------------------------------------------------------------

// setUnitLoadout: what this engine carries are weapons and magazines -- the three weapons'
// magazines, the magazines in uniform, vest and backpack, then the weapons themselves.
const char* const kSetUnitLoadout =
    "private _u = _this select 0; private _l = _this select 1; "
    "if (typeName _l == 'ARRAY' && {count _l >= 6}) then { "
    "removeAllWeapons _u; "
    "{ if (typeName _x == 'ARRAY' && {count _x > 5}) then { "
    "  { if (typeName _x == 'ARRAY' && {count _x > 0} && {isClass (configFile >> 'CfgMagazines' >> (_x select 0))}) then {_u addMagazine (_x select 0)} } forEach [_x select 4, _x select 5] } } forEach [_l select 0, _l select 1, _l select 2]; "
    "{ if (typeName _x == 'ARRAY' && {count _x > 1} && {typeName (_x select 1) == 'ARRAY'}) then { "
    "  { if (typeName _x == 'ARRAY' && {count _x > 1} && {typeName (_x select 0) == 'STRING'} && {isClass (configFile >> 'CfgMagazines' >> (_x select 0))}) then { "
    "    for '_i' from 1 to (((_x select 1) max 0) min 12) do {_u addMagazine (_x select 0)} } } forEach (_x select 1) } } forEach [_l select 3, _l select 4, _l select 5]; "
    "{ if (typeName _x == 'ARRAY' && {count _x > 0} && {typeName (_x select 0) == 'STRING'} && {(_x select 0) != ''}) then {_u addWeapon (_x select 0)} } forEach [_l select 0, _l select 1, _l select 2]; "
    "if (count weapons _u > 0) then {_u selectWeapon ((weapons _u) select 0)} }";

const Macro kMacros2[] = {
    {"setUnitLoadout", 'b', GameNothing, GameObject, GameArray | GameString, kSetUnitLoadout},
    {"addMPEventHandler", 'b', GameScalar, GameObject, GameArray,
     "private _t = ((_this select 1) select 0); if ((toLower _t) find 'mp' == 0) then {_t = _t select [2]}; "
     "(_this select 0) addEventHandler [_t, (_this select 1) select 1]"},
    {"removeMPEventHandler", 'b', GameNothing, GameObject, GameArray, ""},
    {"removeAllMPEventHandlers", 'b', GameNothing, GameObject, GameString, ""},
    // Arma 3's control forms of list, slider and display commands this engine knows only
    // with an [idc, ...] argument (counted over Arma 3's own scripts: lbSetCurSel 227 times,
    // lbData 207, lbAdd 174, ...). Controls are numbers here; the dialogs they fill are not
    // shown, so the commands take effect on nothing and answer neutrally.
    {"lbSetCurSel", 'b', GameNothing, GameAny, GameScalar, ""},
    {"lbData", 'b', GameString, GameAny, GameScalar, "''"},
    {"lbAdd", 'b', GameScalar, GameAny, GameString, "0"},
    {"lbValue", 'b', GameScalar, GameAny, GameScalar, "0"},
    {"lbText", 'b', GameString, GameAny, GameScalar, "''"},
    {"lbPicture", 'b', GameString, GameAny, GameScalar, "''"},
    {"lbSetData", 'b', GameNothing, GameAny, GameArray, ""},
    {"lbSetValue", 'b', GameNothing, GameAny, GameArray, ""},
    {"lbSetPicture", 'b', GameNothing, GameAny, GameArray, ""},
    {"lbSetColor", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbAddRow", 'b', GameScalar, GameAny, GameArray, "0"},
    {"lnbData", 'b', GameString, GameAny, GameArray, "''"},
    {"lnbText", 'b', GameString, GameAny, GameArray, "''"},
    {"lnbValue", 'b', GameScalar, GameAny, GameArray, "0"},
    {"lnbSetData", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbSetText", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbSetValue", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbSetPicture", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbSetColor", 'b', GameNothing, GameAny, GameArray, ""},
    {"lnbSetCurSelRow", 'b', GameNothing, GameAny, GameScalar, ""},
    {"sliderSetPosition", 'b', GameNothing, GameAny, GameScalar, ""},
    {"sliderSetRange", 'b', GameNothing, GameAny, GameArray, ""},
    {"closeDisplay", 'b', GameNothing, GameAny, GameScalar, ""},
    // unit skill "aimingAccuracy": this engine has one skill per unit
    {"skill", 'b', GameScalar, GameObject, GameString, "skill (_this select 0)"},
    {"getGroupIcon", 'b', GameArray, GameGroup, GameScalar, "[]"},
    // position nearRoads radius: the road network is not exposed to scripts yet
    {"nearRoads", 'b', GameArray, GameArray | GameObject, GameScalar, "[]"},
    // forms Arma 3 adds to older commands (seen in the showcases): a skill by name (the
    // engine has one skill per unit), fog with decay and base, a named or numbered cut layer
    {"setSkill", 'b', GameNothing, GameObject, GameArray, ""},
    // addMagazine [name, rounds]: the magazine (a full one)
    {"addMagazine", 'b', GameNothing, GameObject, GameArray, "(_this select 0) addMagazine ((_this select 1) select 0)"},
    // reveal [target, knowledge]: the knowledge level has no counterpart, the target is revealed
    {"reveal", 'b', GameNothing, GameObject | GameGroup, GameArray, "(_this select 0) reveal ((_this select 1) select 0)"},
    {"setFog", 'b', GameNothing, GameScalar, GameArray, "(_this select 0) setFog ((_this select 1) select 0)"},
    {"cutText", 'b', GameNothing, GameString | GameScalar, GameArray, "cutText (_this select 1)"},
    {"titleText", 'b', GameNothing, GameString | GameScalar, GameArray, "titleText (_this select 1)"},
    {"enableWeaponDisassembly", 'b', GameNothing, GameObject, GameBool, ""},
    // vehicle lock 0..3 (Arma 3: unlocked, default, locked, locked for the player)
    {"lock", 'b', GameNothing, GameObject, GameScalar, "(_this select 0) lock ((_this select 1) >= 2)"},
    // setDamage [damage, useEffects]: the effects flag has no counterpart
    {"setDamage", 'b', GameNothing, GameObject, GameArray, "(_this select 0) setDamage ((_this select 1) select 0)"},
};

template <size_t I>
GameValue Macro2Binary(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    GameArrayType args;
    args.Add(oper1);
    args.Add(oper2);
    state->VarSetLocal("_this", GameValue(args), false, true);
    const GameValue r = state->EvaluateMultiple(kMacros2[I].code);
    state->EndContext();
    return r;
}

template <size_t... I>
void RegisterMacros2(std::index_sequence<I...>)
{
    (GGameState.NewOperator(GameOperator(kMacros2[I].retType, kMacros2[I].name, function, &Macro2Binary<I>,
                                         kMacros2[I].left, kMacros2[I].right)),
     ...);
}

// ---------------------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------------------

const GameNular A3Nular[] = {
    GameNular(GameHashMap, "createHashMap", A3CreateHashMap),
    GameNular(GameString, "endl", A3Endl),
    GameNular(GameScalar, "scriptNull", A3ScriptNull),
    GameNular(GameArray, "opA3AllEntities", A3AllEntities),
    // Arma 3's name for the resistance side (BIS_fnc_sideColor, BIS_fnc_sideID, ...)
    GameNular(GameSide, "independent", SideResistance),
    GameNular(GameBool, "canSuspend", A3CanSuspend),
    GameNular(GameBool, "hasInterface", A3True),
    GameNular(GameBool, "isDedicated", A3False),
    GameNular(GameBool, "isMultiplayer", A3False),
    GameNular(GameBool, "isMultiplayerSolo", A3False),
    GameNular(GameBool, "didJIP", A3False),
    GameNular(GameScalar, "clientOwner", A3Two),
    GameNular(GameScalar, "remoteExecutedOwner", A3Two),
    GameNular(GameBool, "isRemoteExecuted", A3False),
    GameNular(GameBool, "isRemoteExecutedJIP", A3False),
    GameNular(GameBool, "is3DEN", A3False),
    GameNular(GameBool, "is3DENMultiplayer", A3False),
    GameNular(GameBool, "is3DENPreview", A3False),
    GameNular(GameBool, "isGamePaused", A3False),
    GameNular(GameBool, "isFilePatchingEnabled", A3False),
    GameNular(GameBool, "isSteamMission", A3False),
    GameNular(GameArray, "allMapMarkers", A3AllMapMarkers),
};

const GameFunction A3Unary[] = {
    GameFunction(GameBool, "params", A3Params, GameArray),
    GameFunction(GameAny, "param", A3Param, GameArray | GameScalar),
    GameFunction(GameNothing, "reverse", A3Reverse, GameArray),
    GameFunction(GameAny, "try", A3Try, GameString),
    GameFunction(GameNothing, "throw", A3Throw, GameVoid),
    GameFunction(GameAny, "selectRandom", A3SelectRandom, GameArray),
    GameFunction(GameAny, "selectRandomWeighted", A3SelectRandomWeighted, GameArray),
    GameFunction(GameAny, "selectMax", A3SelectMax, GameArray),
    GameFunction(GameAny, "selectMin", A3SelectMin, GameArray),
    GameFunction(GameString, "trim", A3Trim, GameString),
    GameFunction(GameString, "toLowerANSI", A3ToLowerAnsi, GameString),
    GameFunction(GameString, "toUpperANSI", A3ToUpperAnsi, GameString),
    GameFunction(GameScalar, "linearConversion", A3LinearConversion, GameArray),
    GameFunction(GameArray, "vectorLinearConversion", A3VectorLinearConversion, GameArray),
    GameFunction(GameScalar, "vectorMagnitude", A3VectorMagnitude, GameArray),
    GameFunction(GameScalar, "vectorMagnitudeSqr", A3VectorMagnitudeSqr, GameArray),
    GameFunction(GameArray, "vectorNormalized", A3VectorNormalized, GameArray),
    GameFunction(GameScalar, "getTerrainHeightASL", A3TerrainHeightASL, GameArray),
    GameFunction(GameHashMap, "createHashMapFromArray", A3CreateHashMapFromArray, GameArray),
    GameFunction(GameArray, "keys", A3HashKeys, GameHashMap),
    GameFunction(GameArray, "values", A3HashValues, GameHashMap),
    GameFunction(GameScalar, "count", A3CountHashOrString, GameHashMap),
    GameFunction(GameArray, "toArray", A3HashToArray, GameHashMap),
    GameFunction(GameString, "compileFinal", A3CompileFinal, GameString),
    GameFunction(GameScalar, "owner", A3TwoUnary, GameObject | GameGroup),
    GameFunction(GameString, "markerShape", A3MarkerShape, GameString),
    GameFunction(GameScalar, "addMissionEventHandler", A3AddMissionEventHandler, GameArray),
    GameFunction(GameNothing, "removeMissionEventHandler", A3RemoveMissionEventHandler, GameArray),
    GameFunction(GameNothing, "removeAllMissionEventHandlers", A3RemoveAllMissionEventHandlers, GameString),
    GameFunction(GameString, "markerText", A3MarkerText, GameString),
    GameFunction(GameString, "markerBrush", A3MarkerBrush, GameString),
};

// array # index: select with the highest precedence (Arma 3 2.x); a string gives its
// character; out of range gives nil
GameValue A3HashIndex(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const int i = toIntFloor((float)oper2 + 0.5f);
    if (oper1.GetType() == GameString)
    {
        const RString s = oper1;
        if (i < 0 || i >= s.GetLength())
            return GameValue(RString());
        return GameValue(RString(s.Data() + i, 1));
    }
    const GameArrayType& a = oper1;
    if (i < 0 || i >= a.Size())
        return GameValue();
    return a[i];
}

const GameOperator A3Binary[] = {
    GameOperator(GameAny, "#", mocnina, A3HashIndex, GameArray | GameString, GameScalar),
    GameOperator(GameBool, "params", function, A3ParamsBinary, GameVoid, GameArray),
    GameOperator(GameAny, "catch", function, A3Catch, GameVoid, GameString),
    GameOperator(GameBool, "&&", logicAnd, A3LazyAnd, GameBool, GameString),
    GameOperator(GameBool, "and", logicAnd, A3LazyAnd, GameBool, GameString),
    GameOperator(GameBool, "||", logicOr, A3LazyOr, GameBool, GameString),
    GameOperator(GameBool, "or", logicOr, A3LazyOr, GameBool, GameString),
    GameOperator(GameAny, "param", function, A3ParamBinary, GameVoid, GameArray | GameScalar),
    GameOperator(GameScalar, "pushBack", function, A3PushBack, GameArray, GameVoid),
    GameOperator(GameScalar, "pushBackUnique", function, A3PushBackUnique, GameArray, GameVoid),
    GameOperator(GameNothing, "append", function, A3Append, GameArray, GameArray),
    GameOperator(GameArray, "apply", function, A3Apply, GameArray, GameString),
    GameOperator(GameArray, "apply", function, A3HashApply, GameHashMap, GameString),
    GameOperator(GameScalar, "findIf", function, A3FindIf, GameArray, GameString),
    GameOperator(GameAny, "findIf", function, A3HashFindIf, GameHashMap, GameString),
    GameOperator(GameArray, "select", function, A3SelectRange, GameArray, GameArray),
    GameOperator(GameArray, "select", function, A3SelectFilter, GameArray, GameString),
    GameOperator(GameHashMap, "select", function, A3HashSelectFilter, GameHashMap, GameString),
    GameOperator(GameString, "select", function, A3StringSelectRange, GameString, GameArray),
    GameOperator(GameAny, "deleteAt", function, A3DeleteAtImpl, GameArray, GameScalar),
    GameOperator(GameAny, "deleteAt", function, A3DeleteAtImpl, GameHashMap, GameVoid),
    GameOperator(GameNothing, "deleteRange", function, A3DeleteRange, GameArray, GameArray),
    GameOperator(GameNothing, "insert", function, A3Insert, GameArray, GameArray),
    GameOperator(GameNothing, "sort", function, A3Sort, GameArray, GameBool),
    GameOperator(GameArray, "arrayIntersect", function, A3ArrayIntersect, GameArray, GameArray),
    GameOperator(GameAny, "selectRandomWeighted", function, A3SelectRandomWeightedBinary, GameArray, GameArray),
    GameOperator(GameBool, "isEqualTo", function, A3IsEqualTo, GameVoid, GameVoid),
    GameOperator(GameBool, "isNotEqualTo", function, A3IsNotEqualTo, GameVoid, GameVoid),
    GameOperator(GameBool, "isEqualType", function, A3IsEqualType, GameVoid, GameVoid),
    GameOperator(GameBool, "isEqualTypeAll", function, A3IsEqualTypeAll, GameArray, GameVoid),
    GameOperator(GameBool, "isEqualTypeAny", function, A3IsEqualTypeAny, GameVoid, GameArray),
    GameOperator(GameBool, "isEqualTypeArray", function, A3IsEqualTypeArray, GameVoid, GameArray),
    GameOperator(GameBool, "isEqualTypeParams", function, A3IsEqualTypeParams, GameVoid, GameArray),
    GameOperator(GameString, "joinString", function, A3JoinString, GameArray, GameString),
    GameOperator(GameArray, "splitString", function, A3SplitString, GameString, GameString),
    GameOperator(GameScalar, "find", function, A3StringFind, GameString, GameString),
    GameOperator(GameScalar, "find", function, A3StringFindFrom, GameString, GameArray),
    GameOperator(GameBool, "in", function, A3StringIn, GameString, GameString),
    GameOperator(GameBool, "in", function, A3HashIn, GameVoid, GameHashMap),
    GameOperator(GameString, "toFixed", function, A3ToFixed, GameScalar, GameScalar),
    GameOperator(GameArray, "vectorAdd", function, A3VectorAdd, GameArray, GameArray),
    GameOperator(GameArray, "vectorDiff", function, A3VectorDiff, GameArray, GameArray),
    GameOperator(GameArray, "vectorCrossProduct", function, A3VectorCross, GameArray, GameArray),
    GameOperator(GameScalar, "vectorDotProduct", function, A3VectorDot, GameArray, GameArray),
    GameOperator(GameArray, "vectorMultiply", function, A3VectorMultiply, GameArray, GameScalar),
    GameOperator(GameScalar, "vectorDistance", function, A3VectorDistance, GameArray, GameArray),
    GameOperator(GameScalar, "vectorDistanceSqr", function, A3VectorDistanceSqr, GameArray, GameArray),
    GameOperator(GameArray, "vectorFromTo", function, A3VectorFromTo, GameArray, GameArray),
    GameOperator(GameScalar, "vectorCos", function, A3VectorCos, GameArray, GameArray),
    GameOperator(GameScalar, "distance2D", function, A3Distance2D, GameObject | GameArray, GameObject | GameArray),
    GameOperator(GameScalar, "distanceSqr", function, A3DistanceSqr, GameObject | GameArray, GameObject | GameArray),
    GameOperator(GameScalar, "getRelDir", function, A3GetRelDir, GameObject, GameObject | GameArray),
    GameOperator(GameScalar, "getDir", function, A3GetDirTo, GameObject | GameArray, GameObject | GameArray),
    GameOperator(GameArray, "getPos", function, A3GetPosRel, GameObject | GameArray, GameArray),
    GameOperator(GameArray, "getRelPos", function, A3GetRelPos, GameObject, GameArray),
    GameOperator(GameArray, "worldToModel", function, A3WorldToModel, GameObject, GameArray),
    GameOperator(GameArray, "vectorModelToWorld", function, A3VectorModelToWorld, GameObject, GameArray),
    GameOperator(GameArray, "vectorWorldToModel", function, A3VectorWorldToModel, GameObject, GameArray),
    GameOperator(GameArray, "vectorModelToWorldVisual", function, A3VectorModelToWorld, GameObject, GameArray),
    GameOperator(GameArray, "vectorWorldToModelVisual", function, A3VectorWorldToModel, GameObject, GameArray),
    GameOperator(GameBool, "inArea", function, A3InArea, GameObject | GameArray, GameArray | GameString | GameObject),
    GameOperator(GameArray, "inAreaArray", function, A3InAreaArray, GameArray, GameArray | GameString | GameObject),
    GameOperator(GameBool, "inPolygon", function, A3InPolygon, GameObject | GameArray, GameArray),
    GameOperator(GameHashMap, "createHashMapFromArray", function, A3CreateHashMapFromKeys, GameArray, GameArray),
    GameOperator(GameAny, "get", function, A3HashGet, GameHashMap, GameVoid),
    GameOperator(GameAny, "getOrDefault", function, A3HashGetOrDefault, GameHashMap, GameArray),
    GameOperator(GameBool, "set", function, A3HashSet, GameHashMap, GameArray),
    GameOperator(GameNothing, "merge", function, A3HashMerge, GameHashMap, GameHashMap | GameArray),
    GameOperator(GameNothing, "forEach", function, A3HashForEach, GameString, GameHashMap),
};
} // namespace

GameValue A3DeleteAt(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    return A3DeleteAtImpl(state, oper1, oper2);
}

namespace Poseidon
{
RString GetMissionDirectory();
}

// Called once a simulation step (from Arma2RuntimeSimulate): EachFrame mission event
// handlers. A handler that keeps failing is dropped after a few errors (logged).
void Arma3RuntimeSimulate()
{
    if (Arma1Compat::Generation() != 3 || !GWorld || !GWorld->GetGameState())
        return;
    MissionHandlers& h = Handlers();
    const std::string mission = (const char*)GetMissionDirectory();
    if (mission != h.mission)
    {
        h.mission = mission;
        h.list.clear();
        h.next = 0;
    }
    GameState* gs = GWorld->GetGameState();
    const std::vector<MissionHandler> current = h.list;
    for (const MissionHandler& m : current)
    {
        if (m.type != "eachframe")
            continue;
        GameVarSpace local(gs->GetContext());
        gs->BeginContext(&local);
        gs->VarSetLocal("_thisEventHandler", GameValue(static_cast<float>(m.id)), false, true);
        gs->VarSetLocal("_thisEvent", GameValue(RString("EachFrame")), false, true);
        gs->EvaluateMultiple(m.code);
        const bool failed = gs->GetLastError() != EvalOK;
        gs->EndContext();
        if (!failed)
            continue;
        for (MissionHandler& live : h.list)
            if (live.id == m.id && ++live.errors >= 3)
            {
                LOG_WARN(Script, "Arma 3: EachFrame handler {} dropped after repeated script errors", m.id);
                live.type = "dropped";
            }
    }
}

void RegisterArma3ConfigCommands(); // GameStateExtA1Config.cpp
void RegisterArma3Stubs();          // GameStateExtA3Stubs.cpp

// Registered only when --arma3 mounts its archives (Arma1Compat::MountArchives): Arma 3
// names commands with words older missions use as variables ("get", "keys", "load").
void Arma3RegisterScriptCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    GGameState.NewType("HASHMAP", GameHashMap, CreateGameDataHashMap, "HashMap");
    for (const GameNular& n : A3Nular)
        GGameState.NewNularOp(n);
    for (const GameFunction& f : A3Unary)
        GGameState.NewFunction(f);
    for (const GameOperator& o : A3Binary)
        GGameState.NewOperator(o);
    Poseidon::Arma3Rsc::Register();
    RegisterMacros(std::make_index_sequence<kMacroCount>{});
    RegisterMacros2(std::make_index_sequence<sizeof(kMacros2) / sizeof(kMacros2[0])>{});
    RegisterArma3ConfigCommands();
    RegisterArma3Stubs();
    LOG_INFO(Script, "Arma 3: script commands registered");
}
