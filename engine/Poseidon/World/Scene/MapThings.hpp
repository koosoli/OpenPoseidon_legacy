// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

namespace Poseidon
{
class EntityAIType;
class LODShape;
class Object;
class Thing;

/// Map furniture that a config `thing` class exists for becomes a `Thing`.
///
/// WHY. A `.wrp` object is created by `NewObject`, which branches on the model's
/// `class` property only (forest/road/streetlamp/house/vehicle/church) and makes
/// everything else an `ObjectPlain`. Box3D reaches exactly one class -- `Thing`,
/// via `Thing::Simulate` -> `LooseObjects::Step` -- and a `Thing` is only ever
/// born from a config type with `simulation="thing"`. So a chair the world file
/// places is scenery, however many `class Chair { model="Zidle.p3d"; }` entries
/// the config carries. This bridges the two by SHAPE NAME, which is the one thing
/// both sides agree on: `EntityType::_shapeName` is `GetShapeName(model)` and a
/// map object's shape is `Shapes.New(<wrp name>)`, both `data3d\<name>.p3d`,
/// lower-case, backslash (Simul.cpp:208, Landscape::ObjectCreate).
///
/// MEASURED before it was built, on Everon's abel.wrp (PoseidonTools terrain
/// objects, 103,114 placements): furniture is NOT placed by the world file. Of
/// the 26 config classes with `simulation="thing"`, exactly ONE model appears as
/// a map object at all -- `paletyc.p3d`, 14 times. Every chair, table and radio
/// is a PROXY inside a building model: `hruzdum.p3d` (39 placed) carries two
/// `zidle` chairs, `dum_mesto2.p3d` (3 placed) a `zidlicka` and a
/// `stulsuplik_proxy`, `hangar.p3d` (1) a `hangar_radio` and a
/// `hangar_psacistul`; `dum_mesto_in.p3d` -- the model whose proxy list matches
/// the owner's cafe (13 `hangar_zidle`, `hangar_radio`, `hangar_psacistul`,
/// `stulsuplik_proxy`, `zidlicka`) -- is not placed on abel.wrp at all. So the
/// object-level promotion below is nearly inert on stock worlds, and the
/// proxy-level one is where the chairs come from.
///
/// Two promotions, one switch each:
///   objects -- an `ObjectPlain` whose shape names a `thing` type is replaced
///              by a `Thing` at its transform (keeps the map id).
///   proxies -- every LOD-0 proxy of a map object whose shape names a `thing`
///              type spawns a `Thing` at parent x proxy, and the proxy is hidden
///              in the parent's draw so the furniture is not painted twice.
///
/// Neither makes the furniture MOVE by itself: a `Thing` runs the 2001
/// integration until `LooseObjects` is switched on, and then it is a Box3D body
/// that (PHY-030, measured) never sleeps. See Cost in the .cpp.
namespace MapThings
{

struct Settings
{
    /// ON by default so the owner can A/B it; `POSEIDON_MAP_THINGS=0` turns it
    /// off before the world loads. Read at `Landscape::InitObjectVehicles`, so
    /// the panel checkbox applies on the next world load or mission restart.
    bool enabled = true;
    /// The proxy promotion. `POSEIDON_MAP_THINGS_PROXIES=0` keeps only the
    /// object-level one.
    bool proxies = true;
};

Settings& Get();

/// Counters for the one log line that says what happened. Zero everywhere means
/// "nothing qualified", which on a world without furniture-bearing models is
/// the correct answer and not a failure.
struct Promotion
{
    long long objectsReplaced = 0;   // ObjectPlain -> Thing
    long long objectsReplayed = 0;   // re-spawned after a mission restart
    long long proxiesPromoted = 0;   // proxy -> Thing
    long long parentsWithProxies = 0; // map objects that lost at least one proxy
    long long distinctModels = 0;    // distinct thing models involved
    long long failed = 0;            // type found, NewVehicle refused
    RString   first;                 // first model promoted, for the log
};

/// The `thing` type for a shape, or null. Cached per shape pointer: the bank
/// lookup is a linear `strcmpi` over every loaded type (VehicleTypes.cpp:143)
/// and a world walks 100k objects.
EntityAIType* ThingTypeForShape(const LODShape* shape);

/// Creates the `Thing`, places it, registers it as a MOVING vehicle
/// (`World::AddVehicle`, which is the list `Thing::Simulate` runs from -- not
/// `AddBuilding`) and, for `id >= 0`, gives it the map object's id. Null when the
/// factory refused.
Thing* Spawn(EntityAIType* type, const Matrix4& transform, int id);

/// A replaced map object is gone from the landscape for good, but the `Thing`
/// that stands in for it dies with every `World::CleanUp` (mission restart
/// clears `_vehicles`). Remember what was replaced so the next
/// `InitObjectVehicles` can put it back. Keyed by world name: a new world drops
/// the previous world's records.
void RememberReplaced(const RString& worldName, EntityAIType* type, const Matrix4& transform, int id);
/// Re-spawns the records for this world. Returns how many.
long long ReplayReplaced(const RString& worldName, Promotion& out);

/// Proxy hiding. `HideProxy` is called by the promotion for (parent shape,
/// proxy shape); `ProxyHidden` is asked by `Object::DrawProxies` for every proxy
/// it is about to draw. Keyed by shape POINTER on both sides: every instance of
/// a parent model on the map is promoted, and the proxy `Object`s of every LOD
/// share one bank shape (`NewProxyObject` -> `Shapes.New`), so identity is the
/// right equality and costs one hash probe.
void ResetHiddenProxies();
void HideProxy(const LODShape* parent, const LODShape* proxyShape);
/// Cheap pre-check for the draw loop: false unless this parent lost a proxy.
bool HasHiddenProxies(const LODShape* parent);
bool ProxyHidden(const LODShape* parent, const LODShape* proxyShape);

} // namespace MapThings
} // namespace Poseidon
