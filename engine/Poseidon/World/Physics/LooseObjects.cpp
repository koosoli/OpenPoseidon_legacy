// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Physics/LooseObjects.hpp>

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <cstdlib>
#include <cstring>

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

    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return false;
    }

    // Without registered colliders there is no ground and no building -- a barrel
    // would fall through the world forever. Refusing is better than producing
    // that and calling it physics.
    const Physics::PhysicsStats stats = world->GetStats();
    if (!stats.terrainRegistered)
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
