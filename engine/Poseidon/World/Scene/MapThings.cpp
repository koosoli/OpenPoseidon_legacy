// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Scene/MapThings.hpp>

#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Scene/Thing.hpp>
#include <Poseidon/World/World.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon::MapThings
{
namespace
{

// COST, so the number is next to the code that incurs it. Everon (abel.wrp)
// qualifies 14 pallets + 39 x 2 hruzdum chairs + 3 x 2 dum_mesto2 pieces = ~98
// Things, all in `World::_vehicles`, all simulated every tick at
// SimulateVisibleNear precision. With LooseObjects OFF (its default) each is the
// 2001 `Thing::Simulate`, which starts `_isStopped` and applies no force until
// something hits it: cheap. With LooseObjects ON each becomes a Box3D dynamic
// body on its first tick, and PHY-030 measured that these bodies never sleep
// (0.10-0.21 m/s four seconds after settling). ~100 awake hulls is fine; the
// solver tuning is the thing to fix before a world with a thousand qualifies.

Settings g_settings = []
{
    Settings s;
    if (const char* v = std::getenv("POSEIDON_MAP_THINGS"))
    {
        s.enabled = std::strcmp(v, "0") != 0;
    }
    if (const char* v = std::getenv("POSEIDON_MAP_THINGS_PROXIES"))
    {
        s.proxies = std::strcmp(v, "0") != 0;
    }
    return s;
}();

// Shape -> type. A null entry is a remembered miss; misses are the common case
// (every tree, road and wall asks once).
std::unordered_map<const LODShape*, EntityAIType*> g_typeByShape;

struct Replaced
{
    EntityAIType* type;
    Matrix4       transform;
    int           id;
};
std::string           g_recordsWorld;
std::vector<Replaced> g_records;

std::unordered_map<const LODShape*, std::unordered_set<const LODShape*>> g_hidden;

} // namespace

Settings& Get() { return g_settings; }

EntityAIType* ThingTypeForShape(const LODShape* shape)
{
    if (!shape)
    {
        return nullptr;
    }
    if (const auto hit = g_typeByShape.find(shape); hit != g_typeByShape.end())
    {
        return hit->second;
    }
    // Both names are `data3d\<model>.p3d`, lower-case, backslash: the type's from
    // GetShapeName(model) (Simul.cpp:208), the map object's from the world file
    // through ShapeBank. FindShapeAndSimulation compares with strcmpi, so case is
    // not a way for this to miss; a leading separator or a forward slash would
    // be, and neither occurs in OFP-era world files. The log line in
    // InitObjectVehicles says how many hit; zero on a world with hruzdum on it
    // means this comparison, not the world.
    EntityType*   found = VehicleTypes.FindShapeAndSimulation(shape->Name(), "thing");
    EntityAIType* type = found ? dynamic_cast<EntityAIType*>(found) : nullptr;
    // The type bank hands out unowned pointers to types it keeps alive for the
    // process; caching the pointer is what the bank itself does.
    g_typeByShape.emplace(shape, type);
    return type;
}

Thing* Spawn(EntityAIType* type, const Matrix4& transform, int id)
{
    if (!type || !GWorld)
    {
        return nullptr;
    }
    Ref<EntityAI> veh = NewVehicle(type);
    Thing*        thing = veh ? dyn_cast<Thing>(veh.GetRef()) : nullptr;
    if (!thing)
    {
        LOG_WARN(World, "MAPTHINGS: '{}' has simulation=thing but did not create a Thing", (const char*)type->GetName());
        return nullptr;
    }
    // A proxy transform can carry the modeller's scale; a Thing's frame must
    // not, or its physics hull and its drawn size disagree.
    Matrix4 frame = transform;
    frame.Orthogonalize();
    thing->SetTransform(frame);
    thing->Init(frame);
    if (id >= 0)
    {
        thing->SetID(id);
    }
    // AddVehicle, not AddBuilding: `World::SimulateVehicles` runs `_vehicles`,
    // and `_buildings` gets `SimulateBuildings`, which a chair that is meant to
    // fall over must not be in. VehicleList::Add also inserts the entity into the
    // landscape cell, so no Landscape::AddObject here.
    GWorld->AddVehicle(thing);
    return thing;
}

void RememberReplaced(const RString& worldName, EntityAIType* type, const Matrix4& transform, int id)
{
    const char* name = (const char*)worldName;
    if (!name)
    {
        name = "";
    }
    if (g_recordsWorld != name)
    {
        g_recordsWorld = name;
        g_records.clear();
    }
    g_records.push_back(Replaced{type, transform, id});
}

long long ReplayReplaced(const RString& worldName, Promotion& out)
{
    const char* name = (const char*)worldName;
    if (!name)
    {
        name = "";
    }
    if (g_recordsWorld != name)
    {
        g_recordsWorld = name;
        g_records.clear();
        return 0;
    }
    long long replayed = 0;
    for (const Replaced& r : g_records)
    {
        if (Spawn(r.type, r.transform, r.id))
        {
            ++replayed;
        }
        else
        {
            ++out.failed;
        }
    }
    out.objectsReplayed += replayed;
    return replayed;
}

void ResetHiddenProxies() { g_hidden.clear(); }

void HideProxy(const LODShape* parent, const LODShape* proxyShape)
{
    if (parent && proxyShape)
    {
        g_hidden[parent].insert(proxyShape);
    }
}

bool HasHiddenProxies(const LODShape* parent)
{
    return !g_hidden.empty() && g_hidden.find(parent) != g_hidden.end();
}

bool ProxyHidden(const LODShape* parent, const LODShape* proxyShape)
{
    const auto hit = g_hidden.find(parent);
    return hit != g_hidden.end() && hit->second.count(proxyShape) != 0;
}

} // namespace Poseidon::MapThings
