#pragma once

// Soldier-level hunting (after X-Ray's stalker combat planner): when the enemy a
// soldier knows about hasn't been seen for a while, he holds in cover for 5-10 s,
// then (no more than half the squad at once) flanks the last known position and
// searches it. Built to leave legacy missions alone:
//   * AI-led groups only; never the group leader (the formation stays intact)
//   * only in Combat or Stealth behaviour, and only when the combat mode lets
//     soldiers leave formation (White / Red "engage at will")
//   * never while stopped (stop / doStop / player Stop), disableAI "MOVE",
//     pinned down, fleeing, in a vehicle, or on a scripted waypoint
//   * never while the group is moving or attacking for its waypoint, and never
//     on Hold / Sentry / Get in / Load / Unload / Support / Join waypoints
//   * moves are ordinary auto orders on the soldier's own subgroup, so waypoint
//     completion never waits for them and he rejoins the group afterwards
// Tunable or switched off with class CfgAIFork { hunting = 0; ... }.

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class AIUnit;
class AIGroup;

namespace Hunt
{
//! per-unit update, called from AIUnit::Think (local, alive units); throttled inside
void Think(AIUnit* unit);

//! true when this soldier is allowed to hunt right now (all the gates above)
bool MayHunt(AIUnit* unit);

// ---- squad rules (X-Ray agent manager) -----------------------------------------

//! an AI soldier of this group has just thrown a hand grenade
void OnGrenadeThrown(const AIGroup* grp);

//! false while the group's grenade interval (CfgAIFork grenadeInterval, 5 s) runs
bool GrenadeAllowed(const AIGroup* grp);

//! false if a member of the firer's group (or his cover) is within the splash
//! danger radius of the aim point: max(5 m, 1.5 x indirectHitRange)
bool SplashSafe(const AIUnit* firer, Vector3Par aimPoint, float indirectHitRange);
} // namespace Hunt
} // namespace Poseidon
