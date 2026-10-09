#pragma once

// AI-XRAY-003: soldiers react to live grenades near them (a grenade is a danger within its blast reach; take cover,
// wait for the explosion, look round). Needs grenades
// that do not go off on contact: GrenadeFuse gives stock hand grenades a fuse.
//
//   * Every fused round reports itself while it lives (ShotShell::Simulate): where it is or will land, the
//     time left on the fuse, its blast radius.
//   * An AI soldier on foot within `dodgeRadius` (when set) of where one lies or will land, and inside its danger radius
//     (2x its blast range + 1 m, at least 10 m: CWA's hand grenade still killed at 9 m), runs directly away from
//     it to the edge of that radius, and is held there until it goes off. Leaders too: blast damage falls with
//     distance and cover only (Landscape::ExplosionDammage), so going prone would save nobody.
//   * When it has gone off his order is withdrawn (he rejoins the group) and he is put on alert.
// Friendly grenades count: a soldier gets away from his own side's too. Players are never moved.
// Off by default: class CfgAIFork { grenadeDodge = 1; dodgeRadius = 0; }. POSEIDON_AI_HUNT_LOG=1 logs reactions.

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class AIUnit;

namespace GrenadeDanger
{
//! a live fused round, once a simulation step: `shot` identifies it (never dereferenced)
void Report(const void* shot, Vector3Par position, Vector3Par speed, float fuseLeft, float blastRadius, bool atRest);

//! per-unit reaction, from AIUnit::Think (local, alive units); throttled inside
void React(AIUnit* unit);

//! he is getting clear of a grenade right now (hunting leaves him alone meanwhile)
bool IsRunning(const AIUnit* unit);
} // namespace GrenadeDanger
} // namespace Poseidon
