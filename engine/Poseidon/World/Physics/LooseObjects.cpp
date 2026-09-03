// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Physics/LooseObjects.hpp>

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <cstdlib>
#include <cstring>
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

void Reset() { g_settings = Settings{}; }

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
