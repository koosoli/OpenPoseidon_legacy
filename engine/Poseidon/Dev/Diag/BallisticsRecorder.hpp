#pragma once

// ---------------------------------------------------------------------------
// Ballistics diagnostic — engine-facing half.
//
// WHAT IT DOES
//   Once per frame it walks GWorld's fast-vehicle list (the list every
//   projectile is added to: EntityAI::FireShell / FireMGun / FireMissile all
//   end in World::AddFastVehicle) and appends the position of every live Shot
//   to a bounded store.  Projectiles that vanish are closed out; the exact
//   impact point and what was struck arrive through NotifyImpact, called from
//   the three terminal sites in ShotShell::Simulate.
//
// WHY POLLING RATHER THAN A FIRE HOOK
//   A fire hook would need edits in three EntityAI paths and would still not
//   give the trajectory.  Polling gives one sample per rendered frame, which
//   at 60 fps is ~15 m of travel for a 900 m/s rifle round — roughly 40
//   samples over a 600 m shot, plenty to show curvature — and it costs a walk
//   of a list that is normally a handful of entries.
//
// COST WHEN OFF
//   `Enabled()` is a load of one file-scope bool.  Sample() and Draw() return
//   on it before touching anything; NotifyImpact is inlined around it so the
//   Shots.cpp call sites compile to a predictable-not-taken branch.  Nothing
//   is allocated and no track exists until the dev panel switch is turned on,
//   and turning it off frees the store.
//
// MULTIPLAYER
//   Recording is read-only and local; it changes no simulation state and sends
//   nothing.  It is safe in MP.  (The *pause* is a different matter — see
//   DiagPause.hpp.)
// ---------------------------------------------------------------------------

#include <Poseidon/Dev/Diag/BallisticsTrackStore.hpp>

namespace Poseidon::Dev::Ballistics
{

/// Master switch.  Read it before doing anything; see the cost note above.
/// Definition lives in BallisticsRecorder.cpp.
extern bool g_enabled;

inline bool Enabled()
{
    return g_enabled;
}

void SetEnabled(bool enabled);

/// Per-frame projectile sampling.  Called once from World::Simulate.
void Sample();

/// Per-frame trail submission.  MUST be called between Scene::BeginObjects and
/// Scene::EndObjects, the same window VehicleAI's path diagnostic draws in.
void Draw();

/// Exact terminus, reported by the projectile itself.  `shot` is the Shot
/// pointer, used only as an identity key — it is never dereferenced here.
void NotifyImpactImpl(const void* shot, float x, float y, float z, BallisticTerminus terminus, const char* hitName);

inline void NotifyImpact(const void* shot, float x, float y, float z, BallisticTerminus terminus, const char* hitName)
{
    if (g_enabled)
    {
        NotifyImpactImpl(shot, x, y, z, terminus, hitName);
    }
}

BallisticsTrackStore& Store();

/// Drop every recorded track and every live association.
void ClearAll();

// -- view state, owned here so the ImGui tab stays a pure view ---------------

/// Track id the tab has selected, or 0.  A selected track draws brighter and
/// wider so it can be picked out of a crowded engagement.
uint32_t SelectedTrack();
void SetSelectedTrack(uint32_t id);

bool DrawTrails();
void SetDrawTrails(bool draw);

/// Trail segments actually submitted on the last Draw() — the honest number to
/// show next to the retention sliders.
int LastSegmentsDrawn();

/// Hard ceiling on submitted segments per frame, so a magazine dump into the
/// store cannot turn the diagnostic into the frame cost.
int SegmentBudget();
void SetSegmentBudget(int segments);

} // namespace Poseidon::Dev::Ballistics
