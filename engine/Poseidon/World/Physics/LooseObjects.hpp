// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/World/Physics/PhysicsTypes.hpp>

namespace Poseidon
{
// Entity, not Vehicle: in this codebase `Vehicle` is a typedef FOR Entity
// (Vehicle.hpp, `typedef Entity Vehicle;`), so forward-declaring `class Vehicle`
// silently invents a second, unrelated type and the redefinition only surfaces
// wherever both headers meet.
class Entity;

/// Physics ownership for loose objects -- barrels, pallets, furniture.
///
/// `Thing` is the only class here that reads a pose BACK out of physics.
/// Everything else pushes transforms in and never looks -- which is what keeps
/// PHY-010's boundary intact for all of it.
///
/// MEASURED before it was built, on Noe: a stock map's 177,225 objects contain
/// ZERO Things -- barrels baked into terrain are decoration with no entity
/// behind them. Six placed by a mission came back as six Things, all local, none
/// with a network id in single player. So this affects mission props, and only
/// where a mission puts them.
namespace LooseObjects
{

struct Settings
{
    /// OFF by default. It changes how a shipping object behaves, and the two
    /// questions that decide whether it may ship -- multiplayer ownership and
    /// what a moved barrel does to a saved game -- are open.
    bool enabled = false;

    /// CfgVehicles gives a Thing no mass; 2001 had no use for one. This is what
    /// an oil drum weighs when it is not full, scaled by the slider so a barrel
    /// can be made to shove like an empty one or an anchor without a rebuild.
    float mass = 30.0f;
    float friction = 0.6f;
    // RFG-095: map objects within this many metres of a loose object become static bodies
    // when its body is spawned (0 = none), up to `staticCap` per world.
    float staticRadius = 30.0f;
    int   staticCap = 4000;
    float restitution = 0.05f;
};

Settings& Get();
void      Reset();

/// Hands one loose object to the physics world and reads its pose back.
///
/// Returns true when physics owns the object this tick and the caller must skip
/// its own integration. False means untouched -- the feature is off, the object
/// is not ours to move, or the world has no ground to fall onto -- and the
/// caller runs the 2001 simulation exactly as before.
///
/// `body` is the object's own handle and is created on first use.
bool Step(Entity& thing, Physics::BodyId& body);

/// Gives a body back. Safe on an invalid handle, which is the usual case.
void Release(Physics::BodyId& body);

} // namespace LooseObjects
} // namespace Poseidon
