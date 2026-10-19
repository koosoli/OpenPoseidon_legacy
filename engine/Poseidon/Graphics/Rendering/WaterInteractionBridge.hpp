#pragma once

#include <cstdint>

namespace Poseidon
{
constexpr uint32_t HydroMaxWaterInteractions = 48;

enum HydroWaterInteractionKind : uint32_t
{
    HydroWaterInteractionBullet = 0,
    HydroWaterInteractionObject = 1,
    HydroWaterInteractionPlayer = 2,
    HydroWaterInteractionExplosion = 3,
    HydroWaterInteractionFootstep = 4,
    HydroWaterInteractionContinuous = 5,
    // Sinkhole W3: a swimmer's hand or foot cutting the surface. positionRadius = (x, z, radius, surface y),
    // velocityKind = (limb vx, vz, vy over the water, kind), timeLifeFoamMass[3] = splash height (m).
    // Drawn by the Tidewater water (water_tw/impacts.rs); the Current OP water ignores the kind.
    HydroWaterInteractionSwimSplash = 6,
    // (OP) a whale's blow (Whale.cpp): a column of spray and mist thrown up for ~1.4 s. positionRadius = (x, z,
    // radius, the blowholes' y), velocityKind = (drift vx, vz, 0, kind), timeLifeFoamMass[3] = column height (m).
    // Drawn by the Tidewater water (water_tw/impacts.rs).
    HydroWaterInteractionWhaleBlow = 7,
    // (OP) blood in the water (a wounded whale, Whale.cpp): positionRadius = (x, z, 0, surface y),
    // timeLifeFoamMass[3] = amount (a rifle round ~0.3, a shell's blast 3). Pools into a red stain in the
    // Tidewater water that spreads and fades over two minutes (water_tw/impacts.rs, tw_impact.wgsl).
    HydroWaterInteractionBlood = 8,
};

enum HydroWaterInteractionFlags : uint32_t
{
    HydroWaterInteractionPendingImpulse = 1u << 0,
    HydroWaterInteractionCapsule = 1u << 8,
    HydroWaterInteractionPlayerWading = 1u << 9,
    HydroWaterInteractionPlayerSwimming = 1u << 10,
    HydroWaterInteractionLeftSide = 1u << 11,
    HydroWaterInteractionLargeBody = 1u << 12,
};

// This mirrors the renderer ABI without making simulation code depend on wgpu_renderer.hpp.
struct alignas(16) HydroWaterInteractionEvent
{
    float positionRadius[4];
    float velocityKind[4];
    float timeLifeFoamMass[4];
    float directionDepthFlags[4];
};

static_assert(sizeof(HydroWaterInteractionEvent) == 64 && alignof(HydroWaterInteractionEvent) == 16,
              "Hydro water event must match the renderer ABI");

// Safe from simulation threads. Events are consumed only by the render path.
void SubmitWaterInteraction(const HydroWaterInteractionEvent& event);
uint32_t DrainWaterInteractions(HydroWaterInteractionEvent* events, uint32_t capacity);

// Player immersion is visual-only. It lets the renderer distinguish a camera above
// water from an infantry body merely standing in shallow water.
void SetPlayerWaterDepth(float depth);
float GetPlayerWaterDepth();

// Water-tab diagnostics. Running total of events handed to the bridge, and how many the render
// path took on its last drain. Together with the player water depth these localise a dead ripple
// to one of three places: depth never rises (collision), depth rises but nothing is submitted
// (the emit conditions), or events are submitted and drained but nothing appears (the solver).
uint32_t TotalWaterInteractionsSubmitted();
uint32_t LastWaterInteractionsDrained();

// Dev-only visual control for the legacy CPU droplet emitter used by ordinary
// rifle impacts. The water ripple/foam event remains independent of this switch.
void SetRifleWaterImpactSprayEnabled(bool enabled);
bool RifleWaterImpactSprayEnabled();

// TW-WATER W8a: in the Tidewater water mode an explosion on open water (sea bed more than
// 0.5 m down, the burst at the surface) is drawn by the renderer as a water plume sized from the
// ammo (the explosion's water-interaction event carries indirectHit and indirectHitRange), so the
// legacy fireball / smoke Explosion and the CPU droplets are not spawned there.
// WGR_TW_LEGACY_WATER_FX=1 keeps them; the Current OP water mode is unchanged.
bool TidewaterWaterBurst(float x, float y, float z);

// Sinkhole W3: the water surface around the camera as the Tidewater water draws it (its W9a probe grid,
// n x n displaced vertices x, y, z, 1-3 frames late; lanes = first lattice x, z, step, n), handed over
// by the renderer each frame (n = 0: none). The CPU wave predictor matches the drawn sea in scale, not
// crest for crest; a swimmer near the camera, and the camera following him, ride this instead.
// Presentation of the local view only. Safe from any thread.
void SetDrawnWaterGrid(const float* xyz, int n, const float lanes[4]);
// `edgeWeight` (optional): 1 inside the grid, falling to 0 over its outer 2 m, so a caller can blend
// to the predictor instead of stepping onto a different sea at the edge (a boat straddling it was
// thrown about in a storm, owner 2026-10-13).
bool DrawnWaterHeightAt(float x, float z, float& y, float* edgeWeight = nullptr);

// (OP) Water height queries at the simulation's own points, anywhere on the sea (the renderer's
// water_tw/query.rs; Tidewater's WaterQuery): a boat hands its hull samples every step, keyed by the
// object; the renderer evaluates the drawn surface there and the results come back 1-3 frames late.
// Presentation of the local view, as the drawn grid above: a server, another water mode or a stale
// result answers false and the caller keeps the CPU predictor.
constexpr int WaterQueryMaxPoints = 128;
void RequestWaterQueries(uint64_t key, const float* xz, int n);
// Render side: the requested points (x, z pairs, at most `cap`), clearing the requests.
int TakeWaterQueryRequests(float* xz, int cap);
// Render side: the latest results (n = 0: none), with the points they were asked at.
void SetWaterQueryResults(const float* xz, const float* values, int n);
// The drawn surface at (x, z) from the nearest result point within 1.5 m (its height carried along
// its slope); `version` changes whenever a new set of results arrived and `seconds` is when (a steady
// clock), so a caller can difference heights between results rather than between frames.
bool DrawnWaterQuery(float x, float z, float& y, float& slopeX, float& slopeZ, uint32_t* version = nullptr,
                     double* seconds = nullptr);

} // namespace Poseidon
