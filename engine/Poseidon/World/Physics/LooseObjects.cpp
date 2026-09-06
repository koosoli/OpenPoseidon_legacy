// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Physics/LooseObjects.hpp>

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <Poseidon/World/Scene/Thing.hpp>
#include <Poseidon/World/World.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace Poseidon::LooseObjects
{
namespace
{
Settings g_settings = []
{
    Settings s;
    // An env switch as well as the panel one, so a capture run can prove this
    // does something without a human clicking. A feature that can only be
    // demonstrated by hand is a feature nobody re-checks.
    const char* v = std::getenv("POSEIDON_LOOSE_OBJECTS");
    s.enabled = v && std::strcmp(v, "0") != 0;
    return s;
}();

/// One line per second while objects are owned, so a run leaves evidence that
/// the solver actually moved something rather than merely being asked to.
int   g_owned = 0;
float g_reportIn = 0.0f;

// RFG-095: the physics world knew the terrain and nothing else, so a chair shot in the
// pub fell through the floor and flew through the walls: every wall it met was a
// drawing, not a body. Static bodies are registered for the map objects around a loose
// object when its body is spawned -- the pub around the chair, the fence beside the
// table -- once per object, from the same geometry / fire / roadway levels the engine
// collides against. Radius and a per-world cap keep it bounded; both are settings.
std::unordered_set<const Object*> g_staticRegistered;
int                               g_staticBodies = 0;
double                            g_staticMs = 0.0;

void RegisterStaticsAround(Physics::PhysicsWorld& world, const Entity& thing, float radius)
{
    if (!GLandscape || radius <= 0.0f)
        return;
    const auto began = std::chrono::steady_clock::now();
    const Vector3 pos = thing.WorldPosition();
    int xMin = 0, xMax = 0, zMin = 0, zMax = 0;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, radius);
    const int range = GLandscape->GetLandRange();
    int added = 0;
    int candidates = 0, noGeometry = 0, degenerate = 0, refused = 0, buildingsSeen = 0;
    const auto tryRegister = [&](Object* obj, LODShapeWithShadow* shape)
    {
        ++candidates;
        const Physics::RegisterResult result = world.RegisterStaticDetailed(shape, obj->Transform());
        switch (result.outcome)
        {
            case Physics::RegisterResult::Outcome::Registered: ++g_staticBodies; ++added; break;
            case Physics::RegisterResult::Outcome::NoGeometryLod: ++noGeometry; break;
            case Physics::RegisterResult::Outcome::AllComponentsDegenerate: ++degenerate; break;
            case Physics::RegisterResult::Outcome::BackendRefused: ++refused; break;
        }
    };
    for (int z = std::max(zMin, 0); z <= zMax && z < range; ++z)
        for (int x = std::max(xMin, 0); x <= xMax && x < range; ++x)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            for (int i = 0; i < list.Size(); ++i)
            {
                Object* obj = list[i];
                if (obj == nullptr || obj == &thing || !obj->Static() || obj->GetType() == Temporary ||
                    obj->GetType() == TypeTempVehicle)
                    continue;
                if (dyn_cast<Thing>(obj) != nullptr)
                    continue; // another loose object: a body of its own, never a wall
                LODShapeWithShadow* shape = obj->GetShape();
                if (shape == nullptr)
                    continue;
                if ((obj->WorldPosition() - pos).SquareSize() > radius * radius)
                    continue;
                if (g_staticBodies >= g_settings.staticCap || !g_staticRegistered.insert(obj).second)
                    continue;
                tryRegister(obj, shape);
            }
        }
    // The BUILDINGS are not in the cell lists: a house-class model is promoted to a
    // Building entity and removed from them at world load (the RFG-080 trap, met again),
    // and the pub around the chair is exactly such a model. They live on the world's
    // building list.
    if (GWorld != nullptr)
    {
        for (int i = 0; i < GWorld->NBuildings(); ++i)
        {
            Entity* building = GWorld->GetBuilding(i);
            if (building == nullptr || building == &thing)
                continue;
            LODShapeWithShadow* shape = building->GetShape();
            if (shape == nullptr)
                continue;
            if ((building->WorldPosition() - pos).SquareSize() > radius * radius)
                continue;
            ++buildingsSeen;
            if (g_staticBodies >= g_settings.staticCap || !g_staticRegistered.insert(building).second)
                continue;
            tryRegister(building, shape);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    g_staticMs += ms;
    static int reported = 0;
    if (added > 0 || reported < 6)
    {
        ++reported;
        LOG_INFO(World,
                 "LOOSEOBJ: {} static bodies registered around {} at [{:.0f} {:.0f} {:.0f}] within {:.0f} m ({} candidates: "
                 "{} no geometry LOD, {} degenerate, {} refused; {} buildings in range of {} on the world list; {} total, "
                 "{:.1f} ms, {:.1f} ms cumulative)",
                 added, thing.GetDebugName().Data(), pos.X(), pos.Y(), pos.Z(), radius, candidates, noGeometry, degenerate,
                 refused, buildingsSeen, GWorld != nullptr ? GWorld->NBuildings() : -1, g_staticBodies, ms, g_staticMs);
    }
}

/// PHY-030: bring the world up ourselves.
///
/// Until now nothing here created a physics world or gave it a ground. Both were
/// side effects of the dev PROBE tool, so the loose-objects switch on its own did
/// exactly nothing -- `Step` bailed on `!world->IsCreated()`, then on
/// `!stats.terrainRegistered`, silently, and a barrel sat there being 2001.
/// A feature whose only working path is another feature's setup is not a feature.
///
/// Tried once and remembered, so a failed bring-up cannot re-run the island scan
/// every tick for the rest of the session.
bool TerrainBroughtUp()
{
    static bool tried = false;
    static bool ok = false;
    if (tried)
    {
        return ok;
    }
    tried = true;
    if (!GLandscape)
    {
        return false;
    }
    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
    if (!world.Create())
    {
        LOG_WARN(World, "LOOSEOBJ: physics backend refused to start; loose objects stay classic");
        return false;
    }
    const int range = GLandscape->GetLandRange();
    if (range <= 1)
    {
        return false;
    }
    // Subdivided for the same reason PhysicsCorpus subdivides: the landscape grid
    // is 50 m and a straight chord between two samples cuts the corner of a
    // hillside by metres, so an object sinks into a slope it should rest on.
    // 4x is 12.5 m spacing; on Everon that is 1021^2 samples, about 4 MB, once.
    const int   subdiv = 4;
    const int   grid = (range - 1) * subdiv + 1;
    const float spacing = LandGrid / static_cast<float>(subdiv);
    std::vector<float> heights(static_cast<std::size_t>(grid) * grid);
    for (int z = 0; z < grid; ++z)
    {
        for (int x = 0; x < grid; ++x)
        {
            heights[static_cast<std::size_t>(z) * grid + x] =
                GLandscape->SurfaceY(static_cast<float>(x) * spacing, static_cast<float>(z) * spacing);
        }
    }
    ok = world.SetTerrain(heights.data(), grid, grid, spacing, 0.0f, 0.0f);
    LOG_INFO(World, "LOOSEOBJ: terrain {}x{} at {:.2f} m -> {}", grid, grid, spacing,
             ok ? "registered" : "REFUSED");
    return ok;
}
}

Settings& Get() { return g_settings; }

void Reset()
{
    g_settings = Settings{};
    g_staticRegistered.clear();
    g_staticBodies = 0;
    g_staticMs = 0.0;
}

void Release(Physics::BodyId& body)
{
    if (!body.IsValid())
    {
        return;
    }
    if (Physics::PhysicsWorld* world = Physics::GetPhysicsWorld())
    {
        world->Remove(body);
    }
    body = Physics::BodyId{};
}

bool Step(Entity& thing, Physics::BodyId& body)
{
    if (!g_settings.enabled)
    {
        // Not merely "do nothing": give the body back, so switching the feature
        // off mid-session leaves no orphans behind holding shapes in the solver.
        Release(body);
        return false;
    }

    // Only what belongs to this machine. In single player that is everything; in
    // multiplayer it is the contract that makes this safe at all, because one
    // authority per object means two clients never have to agree on float maths.
    if (!thing.IsLocal())
    {
        Release(body);
        return false;
    }

    // Without a ground a barrel falls through the world forever, so bring one up
    // rather than refusing: see TerrainBroughtUp for why this used to be somebody
    // else's job. Still refuses if the backend or the landscape says no.
    if (!TerrainBroughtUp())
    {
        return false;
    }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return false;
    }

    if (!body.IsValid())
    {
        LODShape* shape = thing.GetShape();
        if (!shape)
        {
            return false;
        }
        body = world->SpawnModelProbe(shape, thing.WorldTransform(), g_settings.mass, g_settings.friction,
                                      g_settings.restitution, thing.Speed());
        if (!body.IsValid())
        {
            return false;
        }
        RegisterStaticsAround(*world, thing, g_settings.staticRadius);
    }

    Physics::BodyMotion motion;
    if (!world->GetBodyMotion(body, motion))
    {
        // The handle went stale -- the world was destroyed and rebuilt under us.
        // Drop it and let the next tick make a new one.
        body = Physics::BodyId{};
        return false;
    }

    thing.SetPosition(motion.position);
    Matrix3 orientation;
    orientation.SetDirectionAndUp(motion.axisZ, motion.axisY);
    thing.SetOrient(orientation);
    thing.SetSpeed(motion.linearVelocity);

    ++g_owned;
    if (motion.linearVelocity.SquareSize() > 0.01f)
    {
        LOG_INFO(World, "LOOSEOBJ: {} at [{:.2f} {:.2f} {:.2f}] moving {:.2f} m/s, {}",
                 thing.GetDebugName().Data(), motion.position.X(), motion.position.Y(),
                 motion.position.Z(), motion.linearVelocity.Size(), motion.awake ? "awake" : "asleep");
    }
    return true;
}

} // namespace Poseidon::LooseObjects
