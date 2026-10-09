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
//   * only for a group on a SEEK AND DESTROY or GUARD waypoint (owner 2026-10-09): a soldier or squad placed
//     with no waypoint, or on Move / Hold / Sentry / Cycle / ..., stays where the mission put it; and never
//     while the group is moving or attacking for its waypoint
//   * AI-XRAY-004: with the danger list on, such a soldier with no enemy to chase searches the position of a
//     danger worth a look (a body, unseen gunfire, where he was hit from); Aware is enough for that
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

//! AI-XRAY-006: every look-out ends now (stance and watch given back) -- before a save: the hunt's own state is
//! not saved, so a soldier saved mid-look would stay standing after the load
void EndAllPeeks();

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
