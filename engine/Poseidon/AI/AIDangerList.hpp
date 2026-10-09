#pragma once

// AI-XRAY-004: a typed danger list per soldier (after X-Ray's CDangerManager, danger_manager.cpp; Lost Alpha's
// xr_danger.script for the ranges and memory -- AI-MALPRAVE-01 §5). CWA has one on/off "danger" per unit; X-Ray
// keeps what happened where, and a soldier with nothing better to do looks there.
//
//   type             weight  range  memory   fed from
//   Hit                2000   150 m   60 s    EntityAI::HitBy (I was hit: where the shooter is)
//   Grenade            1000    15 m   20 s    GrenadeDanger (a live grenade near me)
//   Corpse             2250    20 m   10 s    TrackTargets (a friendly body, newly found)
//   Gunfire            2500   150 m   15 s    TrackTargets (unseen gunfire heard; Lost Alpha: 40 m -- CWA
//                                             fights at longer range, so the range is CWA's hearing)
//   SquadmateDied      3000    20 m   15 s    EntityAI::HitBy (a squadmate killed near me)
//   SquadmateHit       2000    20 m   15 s    EntityAI::HitBy (a squadmate hit near me)
//   Ricochet           3000    10 m   30 s    ApplyNearMissSuppression (a round past me)
//
// Priority = weight x 10 + age in ms, and the LOWEST wins (X-Ray): grenades and gunfire first, and among the
// same type the newest. A danger is noted only by a local AI soldier, and puts him on alert (AIUnit::SetDanger).
// A soldier in the default watch mode (no doWatch / formation watch only) who is not looking at a known enemy
// turns to the top danger (AIUnit::SetWatch). Off by default: class CfgAIFork { dangerList = 1; }.
// POSEIDON_AI_DANGER_LOG=1 logs each danger noted.

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class AIUnit;

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

//! `unit` learns of a danger at `pos` (ignored beyond the type's range from him, or for players / remote units)
void Note(AIUnit* unit, Type type, Vector3Par pos);

//! the top danger he remembers: false when none
bool Top(const AIUnit* unit, Vector3& pos, Type* type = nullptr);

//! POSEIDON_AI_DANGER_LOG: he turned to look at the top danger (throttled per soldier)
void LogTurn(const AIUnit* unit, Vector3Par pos);

//! X-Ray's priority value of a danger of `type`, `ageMs` old: lower is more urgent
float Priority(Type type, float ageMs);
//! how far and how long a danger of `type` counts
float Range(Type type);
float Memory(Type type);
const char* Name(Type type);
} // namespace DangerList
} // namespace Poseidon
