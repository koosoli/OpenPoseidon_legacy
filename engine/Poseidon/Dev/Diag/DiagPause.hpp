#pragma once

// ---------------------------------------------------------------------------
// Diagnostic pause — freeze the simulation, keep the camera flying.
//
// WHAT ALREADY EXISTED (verified, not designed here)
//   World::IsSimulationEnabled() (World/WorldImpl.cpp:194) is the engine's one
//   authority on whether the world steps.  Every menu Display carries
//   `_enableSimulation = false`, which is why opening the Escape menu already
//   stops the world.  When it returns false:
//     * Glob.time stops advancing            (World.cpp:334)
//     * PerformAI + SimulateAllVehicles skip (World.cpp:1714)
//     * sound advances in `paused` mode      (World.cpp:1738)
//     * title / cut effects stop             (World.cpp:843)
//   What it does NOT stop is the camera, because the camera transform is
//   applied unconditionally at World.cpp:1254 — but the free-fly camera is a
//   CameraVehicle entity, and entities are what SimulateAllVehicles steps.  So
//   the menu pause freezes the world AND the free camera together.  Fixing that
//   is the whole of the C++ change: step the manual camera holder separately.
//
// WHY A SEPARATE FLAG RATHER THAN World::SetSimulationEnabled(false)
//   `_enableSimulation` is mission state — it is what triPauseGame drives and
//   what WorldInit sets true at mission start.  Overloading it would make the
//   diagnostic pause indistinguishable from a scripted one, and any code that
//   restored it would silently un-pause the developer.  A separate flag also
//   makes "prove it is inert when off" trivial: one bool, default false, read
//   in exactly one place.
//
// MULTIPLAYER
//   IsSimulationEnabled() for GModeNetware returns purely
//   `GetNetworkManager().GetGameState() >= NGSPlay` and never consults the
//   local pause at all.  The gate below is installed in the single-player
//   branch only, so the pause is structurally unreachable in MP — it cannot
//   desync because it cannot engage.  Available() reports that to the UI so the
//   control is disabled rather than dishonest.
// ---------------------------------------------------------------------------

namespace Poseidon::Dev
{

/// Definition in DiagPause.cpp.  Read directly by World::IsSimulationEnabled
/// through the inline accessor below, so the added cost when off is one load
/// of a bool that is in cache because the same function already touched it.
extern bool g_diagPauseActive;

inline bool DiagPauseActive()
{
    return g_diagPauseActive;
}

/// True when the pause may be engaged at all: a world exists and it is not a
/// network game.  Engaging it elsewhere is refused by SetDiagPause.
bool DiagPauseAvailable();

/// Engage / release.  A no-op (and returns the unchanged state) when
/// !DiagPauseAvailable().
bool SetDiagPause(bool paused);
bool ToggleDiagPause();

// NOTE ON VISUAL TIME — no switch is offered because none is needed.
// Everything time-varying already hangs off the same clock:
//   * World::SimulateLandscape zeroes its own time delta when the simulation is
//     disabled (WorldSetup.cpp:1213), so sun angle, time of day, overcast and
//     fog hold still;
//   * GWind.Update is fed Glob.NetTime (WorldSetup.cpp:1304), which is derived
//     from the frozen Glob.time, so the wind vector holds;
//   * the wgpu renderer drives the cloud-deck scroll and the water sim clock
//     from Glob.time as well (EngineWgpu.cpp:5150), so the sky and sea stop too.
// A paused frame is therefore reproducible without any extra machinery.

} // namespace Poseidon::Dev
