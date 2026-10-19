// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class Object;

// BOOM-002: a rocket or shell that hits a building punches a hole in the wall instead of only
// adding damage until the house collapses.
//
// The hole is real geometry. The building gets its own copy of its model (the shared model
// of every other house of that type is untouched), and in each visual LOD the wall faces the
// blast reaches are re-triangulated around a ragged hole: a ring of triangles between the
// face's own edges and the hole outline, plus a short rim extruded into the wall so the
// breach shows the wall's thickness. Only the faces the hole reaches change -- a few dozen
// triangles, no per-frame cost -- and the face order of the model is kept, so door and
// window animations (which index faces) stay valid.
//
// Chunks of the wall fly out as Box3D boxes (capped, and removed after a while), and a grey
// volumetric dust burst (the fire solver's dirt kind) comes out of the hole.
namespace BuildingBreach
{
struct Settings
{
    bool enabled = true;
    bool debris = true;
    int maxHolesPerBuilding = 8;
    int maxLiveChunks = 120;
    float chunkLifeSeconds = 25.0f;
};
Settings& Get();

/// The hole's radius for a blast (config indirectHit, LAW = 150), metres.
float HoleRadius(float indirectHit);

/// Punches a hole where an explosive round hit `building` (world position, flight direction).
/// Returns false (and changes nothing) for anything that is not a standing building, or when
/// the building has had its share of holes.
bool Breach(Object* building, Vector3Par worldPos, Vector3Par worldDir, float indirectHit, float indirectHitRange);

/// The share of an explosive direct hit's damage that reaches `building` itself. A hit that
/// can breach spends most of itself on the hole (owner, 2026-10-19: a LAW "just destroyed the
/// building and did not make a hole" -- the same explosion cut the hole and then the full
/// damage swapped the house for its ruin). 1 for anything that cannot be breached.
float StructureDamageShare(const Object* building, float indirectHit, bool explosive);

/// The building's damage after one more hit, capped: all the damage one blast does to a building
/// that can still be breached (direct hit, blast wave, the explosion entity -- several calls in
/// the same instant) adds at most 0.12. The 2001 house died of one LAW, so the hole it had just
/// cut vanished with it into the ruin (owner, 2026-10-19). Eight heavy hits bring it down.
float CapBlastDamage(const Object* building, float before, float after);

/// Gameplay through the hole: does the stretch of a line inside one of `building`'s geometry
/// components (entry `bModel`, exit `eModel`, model space) run through a breach -- both ends in
/// the hole's tunnel through the wall? Object::Intersect then ignores that component, so
/// bullets, shells and AI sight pass. False at once for a building without holes.
bool PassesThroughHole(const Object* building, Vector3Par bModel, Vector3Par eModel);
/// Any breach at all (the fast gate for the per-intersection test).
inline bool AnyBreach();

/// BOOM-007 (owner, 2026-10-19: "if i rifle shoot a wall that the wall will be impacted by it
/// in a realistic manner"): a bullet that stops in (or goes through) a static object leaves a
/// mark there -- a dark puncture in a crater of spalled material -- and knocks out a puff of the
/// wall's dust and a few chips. `surface` is the face's texture (the dust takes its colour);
/// `normal` the surface normal (world), `dir` the round's flight direction (world).
void BulletImpact(Object* object, Vector3Par worldPos, Vector3Par normal, Vector3Par dir, float hit, Texture* surface);
int BulletMarks();

/// Per frame, inside the scene's BeginObjects/EndObjects window: draw the live chunks.
void DrawDebris();
/// Mission start/restart: forget every breach and chunk.
void Reset();
int LiveChunks();
int BreachedBuildings();
extern int g_breachedCount;
inline bool AnyBreach() { return g_breachedCount > 0; }
} // namespace BuildingBreach
} // namespace Poseidon
