#pragma once

// AI-XRAY-004: a typed danger list per soldier (weights and priority rule after OpenXRay, MIT). CWA has one on/off
// "danger" per unit; this keeps what happened where, and a soldier with nothing better to do looks there.
//
// Ranges and memory are CWA's own, each taken from a quantity the engine already has:
//   type             weight  range  memory   fed from
//   Hit                2000  2000 m   45 s    EntityAI::HitBy (I was hit: where the shooter is). A shooter who hit
//                                             me is in range by definition; the cap only drops impossible values
//   Grenade            1000    15 m   10 s    GrenadeDanger (a live grenade near me): CWA's hand-grenade danger
//                                             radius (2 x 7 m blast + 1 m); the 4 s fuse + 6 s to look round
//   Corpse             2250    50 m   30 s    TrackTargets (a friendly body, newly found): a squad's spread
//   Gunfire            2500   150 m   15 s    TrackTargets (unseen gunfire heard; CWA's hearing range)
//   SquadmateDied      3000    50 m   20 s    EntityAI::HitBy (a squadmate killed near me): a squad's spread
//   SquadmateHit       2000    50 m   15 s    EntityAI::HitBy (a squadmate hit near me): a squad's spread
//   Ricochet           3000    40 m   15 s    ApplyNearMissSuppression (a round past me): 10x the 4 m near-miss
//                                             radius; remembered as long as heard gunfire
//
// Priority = weight x 10 + age in ms, and the LOWEST wins (X-Ray): by weight grenade 1000, hit and squadmate hit
// 2000, corpse 2250, gunfire 2500, squadmate died and ricochet 3000; among the same type the newest. A danger is
// noted only by a local AI soldier on foot outside a player-led group (stock keeps a player's squad out of the
// automatic danger reactions), and puts him on alert (AIUnit::SetDanger). Only hostile sources count.
// A soldier in the default watch mode (no doWatch / formation watch only) who is not looking at a known enemy
// turns to the top danger (AIUnit::SetWatch). Off by default: class CfgAIFork { dangerList = 1; }.
// POSEIDON_AI_DANGER_LOG=1 logs each danger noted.

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class AIUnit;
class AICenter;

namespace DangerList
{
enum class Type
{
    Hit,
    Grenade,
    Corpse,
    Gunfire,
    SquadmateDied,
    SquadmateHit,
    Ricochet,
    Count
};

//! CfgAIFork dangerList
bool Enabled();

//! `unit` learns of a danger at `pos` (ignored beyond the type's range from him, or for players / remote units /
//! soldiers in a player's group / crews). `error`: m of random displacement -- where a hidden shooter is, is a
//! guess (the engine places sounds at 0.4 x distance)
void Note(AIUnit* unit, Type type, Vector3Par pos, float error = 0.0f);

//! he has dealt with what is at `pos` (searched it): forget his dangers within `radius` of it
void Forget(const AIUnit* unit, Vector3Par pos, float radius);

//! the top danger he remembers (of the types `accept` takes, when given): false when none
bool Top(const AIUnit* unit, Vector3& pos, Type* type = nullptr, bool (*accept)(Type) = nullptr);

//! POSEIDON_AI_DANGER_LOG: he turned to look at the top danger (throttled per soldier)
void LogTurn(const AIUnit* unit, Vector3Par pos);

//! X-Ray's priority value of a danger of `type`, `ageMs` old: lower is more urgent
float Priority(Type type, float ageMs);
//! how far and how long a danger of `type` counts
float Range(Type type);
float Memory(Type type);
const char* Name(Type type);

// AI-XRAY-006: cover where one of a side's soldiers was hit is bad cover for that side for 120 s within 3 m
// (X-Ray CDangerCoverLocation, ai_stalker_fire.cpp). Off by default: class CfgAIFork { badCover = 1; }.
bool BadCoverEnabled();
//! a soldier of `side` was hit by the enemy while in cover at `pos`
void NoteBadCover(const AICenter* side, Vector3Par pos);
//! cover at `pos` is bad for `side` (EntityAI::FindHideBehind skips it)
bool IsBadCover(const AICenter* side, Vector3Par pos);
} // namespace DangerList
} // namespace Poseidon
