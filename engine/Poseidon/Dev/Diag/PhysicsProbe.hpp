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

/// Stacks `count` bricks in front of the camera using the current box settings.
/// Alternating layers are rotated a quarter turn, the way a real wall is laid --
/// a tower of aligned blocks is a column of separate stacks and falls apart at a
/// touch.
void SpawnBrickTower(int count);

/// Places one body at the crosshair. Bound to a key so a row can be laid by
/// walking and tapping, which a panel button cannot do -- the panel steals the
/// mouse the moment it is open.
void PlaceProbeAtCrosshair();

/// Draws every live probe as three great circles. Called from the frame path, not
/// the fixed tick -- drawing is presentation.
void DrawPhysicsProbes();

/// POSEIDON_PHYSICS_PROBE=<n>: drops n spheres once the world has settled, so the
/// PHY-020 acceptance check can be captured without pressing a dev button.
void AutoDropPhysicsProbes();

/// Spawns a DYNAMIC copy of a model already loaded in the world, matched by a
/// substring of its path (default "jeep"). A separate instance: the physics owns
/// it, the mission never hears about it. Returns false if no loaded model matches.
bool SpawnModelProbe(const char* nameFragment, float throwSpeed = 0.0f);

/// Pushes whatever physics body a shot passed through. Called from the impact
/// path; does nothing when no physics world exists.
void PhysicsProbeOnProjectileHit(Vector3Par from, Vector3Par to, float momentum);

/// Pushes the world's wind into the physics world, or clears it. Called once per
/// frame so a change in weather reaches the probes without a respawn.
void UpdatePhysicsProbeWind();

/// Moves the kinematic player proxy to `position`. Called from the fixed tick so
/// the proxy sweeps at the same rate the bodies it pushes are solved at.
void UpdatePhysicsPlayerProxy(Vector3Par position);

// Dev-panel state. Plain globals: this is a probe UI, and threading a settings
// struct through three files would be more machinery than the thing it configures.
struct PhysicsProbeSettings
{
    /// 0 = sphere, 1 = capsule. Plain int so the tab can drive a combo without
    /// dragging the physics headers into the UI include set.
    /// 0 sphere, 1 capsule, 2 box, 3 cylinder, 4 model.
    ///
    /// Model sits in the same list rather than behind its own button so Drop and
    /// Throw work on it like anything else -- a separate Spawn button meant a jeep
    /// could only ever be placed, never thrown, which was the one thing the owner
    /// asked for.
    int   shape = 0;
    float halfLength = 0.6f;
    float boxX = 0.10f;
    float boxY = 0.40f;
    float boxZ = 0.25f;
    /// Place the body on the ground under the crosshair instead of dropping it
    /// from in front of the camera. Without this a domino row is unbuildable:
    /// every piece arrives falling and lands wherever it bounces.
    bool placeOnGround = true;
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
    /// Solid meshes instead of wireframe. Wireframe shows WHERE a body is; solid
    /// shows what it looks like resting against something, which is what you
    /// actually judge when a domino row half-falls.
    bool  drawSolid = true;
    /// Wireframe on top of the solid, so an edge stays readable against scenery
    /// of a similar colour.
    bool  drawOutline = false;
    /// Let the player shove probes by walking into them.
    bool  playerProxy = true;
    float playerRadius = 0.35f;
    float playerHeight = 1.8f;
    /// Model probe: which loaded model to copy, and how heavy to make it.
    char  modelFilter[64] = "jeep";
    float modelMass = 1200.0f;
    /// Newton-seconds a hit transfers. A rifle bullet carries about 4 Ns, which
    /// moves a domino and does nothing at all to a jeep -- so this is a multiplier
    /// on reality, not reality, and it says so in the panel.
    float hitImpulseScale = 40.0f;
};

PhysicsProbeSettings& ProbeSettings();

} // namespace Poseidon::Dev
