// Arma 2 script commands about the world around a unit, needed by Arma 2's own FSMs and
// modules (ALICE, SILVIE, the ambient animals and combat, SOM, the missions' own FSMs):
//
//   pos isFlatEmpty [minDist, precisePos, maxGradient, gradientRadius, onWater, onShore, skip]
//       the position when the place is flat and free enough, else []
//   pos nearObjectsReady radius        true: the objects of the area are always loaded here
//   selectBestPlaces [pos, radius, expression, precision, sourcesCount]
//       [[pos, value], ...]: the places in the area that score best by the expression over
//       meadow, forest, trees, houses, sea, hills, night, rain, windy, deadBody, waterDepth
//   unit nearTargets range             [[pos, type, side, cost, object, accuracy], ...] of the
//                                      targets the unit's group knows within range
//   triggerText trigger, triggerActivation trigger
//   unit setUnconscious bool           kept per unit (no unconscious move set here yet)
//   group setGroupIcon [id, icon, ...] (group icons are not drawn by this engine)
//   removeAllItems unit                the special items (map, compass, watch, radio, GPS)
//
// Run 39 (2026-10-24) listed these as the commands Arma 2's FSMs lack once they run.

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/World/Detection/Detector.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace Poseidon;

namespace
{
std::string LowerShapeName(const Object* obj)
{
    const LODShapeWithShadow* shape = obj ? obj->GetShape() : nullptr;
    std::string n = shape ? std::string((const char*)shape->GetName()) : std::string();
    for (char& c : n)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n;
}

bool IsTree(const Object* obj)
{
    const std::string n = LowerShapeName(obj);
    return n.find("\\tree") != std::string::npos || n.find("\\str_") != std::string::npos ||
           n.find("\\les") != std::string::npos || n.find("\\t_") != std::string::npos;
}

template <class F> void ForObjectsNear(Vector3Par pos, float radius, F&& f)
{
    if (!GLandscape)
        return;
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, radius);
    for (int x = xMin; x <= xMax; x++)
        for (int z = zMin; z <= zMax; z++)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            for (int i = 0; i < list.Size(); i++)
                if (Object* obj = list[i])
                    f(obj);
        }
}

bool IsWater(float x, float z)
{
    return GLandscape && GLandscape->SurfaceY(x, z) < 0.0f;
}

float NumberOr(const GameArrayType& a, int i, float fallback)
{
    return i < a.Size() && a[i].GetType() == GameScalar ? static_cast<float>(a[i]) : fallback;
}

GameValue ScriptPos(const GameState* state, float x, float z)
{
    GameValue v = state->CreateGameValue(GameArray);
    GameArrayType& a = v;
    a.Add(GameValue(x));
    a.Add(GameValue(z));
    a.Add(GameValue(0.0f));
    return v;
}

// pos isFlatEmpty [minDist, precisePos, maxGradient, gradientRadius, onWater, onShore, skipObj]
GameValue A2IsFlatEmpty(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue empty = state->CreateGameValue(GameArray);
    Vector3 pos;
    if (!GLandscape || !GetPos(state, pos, oper1))
        return empty;
    const GameArrayType& p = oper2;
    const float minDist = NumberOr(p, 0, -1.0f);
    const float maxGradient = NumberOr(p, 2, -1.0f);
    const float gradientRadius = NumberOr(p, 3, 0.0f);
    const int onWater = static_cast<int>(NumberOr(p, 4, 0.0f));
    const bool onShore = p.Size() > 5 && p[5].GetType() == GameBool && static_cast<bool>(p[5]);
    Object* skip = p.Size() > 6 && p[6].GetType() == GameObject ? GetObject(p[6]) : nullptr;

    const bool water = IsWater(pos.X(), pos.Z());
    if ((onWater == 0 && water) || (onWater == 2 && !water))
        return empty;
    if (onShore)
    {
        bool shore = water;
        for (int k = 0; k < 8 && !shore; k++)
        {
            const float a = k * 0.785398f;
            shore = IsWater(pos.X() + 25.0f * std::cos(a), pos.Z() + 25.0f * std::sin(a));
        }
        if (!shore)
            return empty;
    }
    if (maxGradient >= 0.0f && gradientRadius > 0.0f)
    {
        const float h0 = GLandscape->SurfaceY(pos.X(), pos.Z());
        for (int k = 0; k < 8; k++)
        {
            const float a = k * 0.785398f;
            const float h = GLandscape->SurfaceY(pos.X() + gradientRadius * std::cos(a),
                                                 pos.Z() + gradientRadius * std::sin(a));
            if (std::abs(h - h0) > maxGradient)
                return empty;
        }
    }
    if (minDist >= 0.0f)
    {
        bool blocked = false;
        ForObjectsNear(pos, minDist + 30.0f, [&](Object* obj) {
            if (blocked || obj == skip)
                return;
            const float r = obj->GetRadius();
            if (r <= 0.05f)
                return;
            Vector3 d = obj->Position() - pos;
            d[1] = 0;
            if (d.Size() - r < minDist)
                blocked = true;
        });
        if (blocked)
            return empty;
    }
    return ScriptPos(state, pos.X(), pos.Z());
}

// pos nearObjectsReady radius: every object of the world is loaded with it here
GameValue A2NearObjectsReady(const GameState*, GameValuePar, GameValuePar)
{
    return GameValue(true);
}

// selectBestPlaces [pos, radius, expression, precision, sourcesCount]
GameValue A2SelectBestPlaces(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    const GameArrayType& p = oper1;
    Vector3 center;
    if (!GLandscape || p.Size() < 3 || !GetPos(state, center, p[0]) || p[2].GetType() != GameString)
        return result;
    const float radius = std::max(1.0f, NumberOr(p, 1, 50.0f));
    float precision = std::max(1.0f, NumberOr(p, 3, 10.0f));
    const int count = std::max(1, static_cast<int>(NumberOr(p, 4, 1.0f)));
    // at most 24 x 24 samples over the area, however fine the precision asked
    precision = std::max(precision, 2.0f * radius / 24.0f);
    const RString expression = p[2];

    GameState* gs = const_cast<GameState*>(state);
    const float daytime = [&] {
        const GameValue v = gs->EvaluateMultiple("daytime");
        return v.GetType() == GameScalar ? static_cast<float>(v) : 12.0f;
    }();
    const float rain = [&] {
        const GameValue v = gs->EvaluateMultiple("rain");
        return v.GetType() == GameScalar ? static_cast<float>(v) : 0.0f;
    }();
    const float night = daytime < 5.0f || daytime > 20.0f ? 1.0f : 0.0f;

    struct Place
    {
        float x, z, value;
    };
    std::vector<Place> places;
    for (float dz = -radius; dz <= radius; dz += precision)
        for (float dx = -radius; dx <= radius; dx += precision)
        {
            if (dx * dx + dz * dz > radius * radius)
                continue;
            const float x = center.X() + dx, z = center.Z() + dz;
            const float y = GLandscape->SurfaceY(x, z);
            const float sea = y < 0.0f ? 1.0f : 0.0f;
            int trees = 0, forestTrees = 0, houses = 0;
            const Vector3 at(x, y, z);
            ForObjectsNear(at, 50.0f, [&](Object* obj) {
                Vector3 d = obj->Position() - at;
                d[1] = 0;
                const float dist2 = d.SquareSize();
                if (IsTree(obj))
                {
                    if (dist2 < 50.0f * 50.0f)
                        forestTrees++;
                    if (dist2 < 15.0f * 15.0f)
                        trees++;
                }
                else if (dist2 < 25.0f * 25.0f && dyn_cast<Building>(obj) && obj->GetRadius() > 2.0f)
                    houses++;
            });
            const float vTrees = std::min(1.0f, trees / 4.0f);
            const float vForest = std::min(1.0f, forestTrees / 40.0f);
            const float vHouses = std::min(1.0f, houses / 3.0f);
            float hills = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                const float a = k * 1.5708f;
                hills = std::max(hills, std::abs(GLandscape->SurfaceY(x + 10.0f * std::cos(a), z + 10.0f * std::sin(a)) - y));
            }
            hills = std::min(1.0f, hills / 5.0f);
            const float meadow = (1.0f - vTrees) * (1.0f - vForest) * (1.0f - vHouses) * (1.0f - sea);

            GameVarSpace local(state->GetContext());
            state->BeginContext(&local);
            state->VarSetLocal("meadow", GameValue(meadow), false, true);
            state->VarSetLocal("forest", GameValue(vForest), false, true);
            state->VarSetLocal("trees", GameValue(vTrees), false, true);
            state->VarSetLocal("houses", GameValue(vHouses), false, true);
            state->VarSetLocal("sea", GameValue(sea), false, true);
            state->VarSetLocal("hills", GameValue(hills), false, true);
            state->VarSetLocal("night", GameValue(night), false, true);
            state->VarSetLocal("rain", GameValue(rain), false, true);
            state->VarSetLocal("windy", GameValue(0.0f), false, true);
            state->VarSetLocal("deadbody", GameValue(0.0f), false, true);
            state->VarSetLocal("waterdepth", GameValue(std::max(0.0f, -y)), false, true);
            state->VarSetLocal("camdepth", GameValue(0.0f), false, true);
            const GameValue v = gs->EvaluateMultiple(expression);
            state->EndContext();
            places.push_back({x, z, v.GetType() == GameScalar ? static_cast<float>(v) : 0.0f});
        }
    std::stable_sort(places.begin(), places.end(), [](const Place& a, const Place& b) { return a.value > b.value; });
    GameArrayType& out = result;
    for (int i = 0; i < count && i < static_cast<int>(places.size()); i++)
    {
        GameValue pair = state->CreateGameValue(GameArray);
        GameArrayType& pa = pair;
        GameValue pos = state->CreateGameValue(GameArray);
        GameArrayType& xy = pos;
        xy.Add(GameValue(places[i].x));
        xy.Add(GameValue(places[i].z));
        pa.Add(pos);
        pa.Add(GameValue(places[i].value));
        out.Add(pair);
    }
    return result;
}

// unit nearTargets range
GameValue A2NearTargets(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue result = state->CreateGameValue(GameArray);
    AIUnit* unit = GetUnit(state, oper1);
    AIGroup* grp = unit ? unit->GetGroup() : nullptr;
    EntityAI* veh = unit ? unit->GetVehicle() : nullptr;
    if (!grp || !veh)
        return result;
    const float range = static_cast<float>(oper2);
    GameArrayType& out = result;
    const TargetList& list = grp->GetTargetList();
    for (int i = 0; i < list.Size(); i++)
    {
        const Target* t = list[i];
        if (!t || !t->IsKnown() || t->destroyed || t->vanished)
            continue;
        if (t->position.Distance(veh->Position()) > range)
            continue;
        GameValue entry = state->CreateGameValue(GameArray);
        GameArrayType& e = entry;
        GameValue pos = state->CreateGameValue(GameArray);
        GameArrayType& xyz = pos;
        xyz.Add(GameValue(t->position.X()));
        xyz.Add(GameValue(t->position.Z()));
        xyz.Add(GameValue(t->position.Y() - GLandscape->SurfaceY(t->position.X(), t->position.Z())));
        e.Add(pos);
        e.Add(GameValue(t->type ? RString((const char*)t->type->GetName()) : RString()));
        e.Add(CreateGameSide(t->side));
        e.Add(GameValue(t->subjectiveCost));
        e.Add(GameValueExt(static_cast<Object*>(t->idExact.GetLink())));
        e.Add(GameValue(t->posError.Size()));
        out.Add(entry);
    }
    return result;
}

GameValue A2TriggerText(const GameState*, GameValuePar oper1)
{
    Detector* det = dyn_cast<Detector>(GetObject(oper1));
    return GameValue(det ? det->GetText() : RString());
}

GameValue A2TriggerActivation(const GameState* state, GameValuePar oper1)
{
    GameValue result = state->CreateGameValue(GameArray);
    Detector* det = dyn_cast<Detector>(GetObject(oper1));
    if (!det)
        return result;
    GameArrayType& a = result;
    a.Add(GameValue(RString(Foundation::FindEnumName((ArcadeSensorActivation)det->GetActivationBy()))));
    a.Add(GameValue(RString(Foundation::FindEnumName((ArcadeSensorActivationType)det->GetActivationType()))));
    a.Add(GameValue(det->IsRepeating()));
    return result;
}

// unit setUnconscious bool: kept per unit (lifeState-like queries can read it later); this
// engine has no unconscious move set for it yet, so the unit itself does not fall.
std::map<const Object*, bool>& Unconscious()
{
    static std::map<const Object*, bool> u;
    return u;
}

GameValue A2SetUnconscious(const GameState*, GameValuePar oper1, GameValuePar oper2)
{
    Object* obj = GetObject(oper1);
    if (obj)
    {
        Unconscious()[obj] = static_cast<bool>(oper2);
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG_INFO(Mission, "Arma 2: setUnconscious is kept per unit; the unit does not fall unconscious here yet");
        }
    }
    return NOTHING;
}

GameValue A2SetGroupIcon(const GameState*, GameValuePar, GameValuePar)
{
    return NOTHING; // group icons (high command) are not drawn by this engine
}

GameValue A2RemoveAllItems(const GameState* state, GameValuePar oper1)
{
    if (!GetObject(oper1))
        return NOTHING;
    GameVarSpace local(state->GetContext());
    state->BeginContext(&local);
    state->VarSetLocal("_u", oper1, false, true);
    const_cast<GameState*>(state)->EvaluateMultiple(
        "{_u removeWeapon _x} forEach [\"ItemMap\",\"ItemCompass\",\"ItemWatch\",\"ItemRadio\",\"ItemGPS\"]");
    state->EndContext();
    return NOTHING;
}
} // namespace

// Called from GameStateExtA2's module start: an INIT_MODULE of its own here was dropped by
// the linker with this otherwise unreferenced object file (run 41 still had none of these).
void RegisterA2WorldCommands()
{
    GGameState.NewOperator(GameOperator(GameArray, "isFlatEmpty", function, A2IsFlatEmpty, GameArray, GameArray));
    GGameState.NewOperator(
        GameOperator(GameBool, "nearObjectsReady", function, A2NearObjectsReady, GameArray | GameObject, GameScalar));
    GGameState.NewFunction(GameFunction(GameArray, "selectBestPlaces", A2SelectBestPlaces, GameArray));
    GGameState.NewOperator(GameOperator(GameArray, "nearTargets", function, A2NearTargets, GameObject, GameScalar));
    GGameState.NewFunction(GameFunction(GameString, "triggerText", A2TriggerText, GameObject));
    GGameState.NewFunction(GameFunction(GameArray, "triggerActivation", A2TriggerActivation, GameObject));
    GGameState.NewOperator(GameOperator(GameNothing, "setUnconscious", function, A2SetUnconscious, GameObject, GameBool));
    GGameState.NewOperator(GameOperator(GameNothing, "setGroupIcon", function, A2SetGroupIcon, GameGroup, GameArray));
    GGameState.NewFunction(GameFunction(GameNothing, "removeAllItems", A2RemoveAllItems, GameObject));
}
