#pragma once

#include <Poseidon/Core/Types.hpp>

// PHY-020: the dev probe. Spawn a dynamic sphere, watch it fall onto real Everon
// terrain and off real buildings.
//
// The probe is owned by physics outright and drawn from a probe-only readback,
// so nothing here weakens PHY-010's rule that gameplay positions never come back
// out of the simulation. It is a probe, not an entity.

namespace Poseidon::Dev
{

/// Spawns one sphere a short way in front of the camera. `throwSpeed` of 0 drops
/// it; anything else launches it along the view direction.
void SpawnPhysicsProbe(float throwSpeed);
void ClearPhysicsProbes();

/// Draws every live probe as three great circles. Called from the frame path, not
/// the fixed tick -- drawing is presentation.
void DrawPhysicsProbes();

/// POSEIDON_PHYSICS_PROBE=<n>: drops n spheres once the world has settled, so the
/// PHY-020 acceptance check can be captured without pressing a dev button.
void AutoDropPhysicsProbes();

/// Pushes the world's wind into the physics world, or clears it. Called once per
/// frame so a change in weather reaches the probes without a respawn.
void UpdatePhysicsProbeWind();

// Dev-panel state. Plain globals: this is a probe UI, and threading a settings
// struct through three files would be more machinery than the thing it configures.
struct PhysicsProbeSettings
{
    float radius = 0.2f;
    float mass = 0.5f;
    float friction = 0.5f;
    float restitution = 0.4f;
    float rollingResistance = 0.05f;
    float throwSpeed = 15.0f;
    bool  applyWind = false;
    /// Multiplies the world wind before it reaches the probe. World wind is a few
    /// m/s, which moves a 0.5 kg ball very little -- this exists so the effect can
    /// be made obvious enough to SEE during a check, then turned back down.
    float windScale = 1.0f;
    float windDrag = 1.0f;
    bool  draw = true;
};

PhysicsProbeSettings& ProbeSettings();

} // namespace Poseidon::Dev
