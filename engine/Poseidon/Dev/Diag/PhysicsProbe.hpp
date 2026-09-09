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
// Mission actions for the optional, single-player showcase. Unknown commands are inert.
bool PhysicsShowcaseCommand(const char* command);
void EndPhysicsShowcase();
// Release draw resources before the renderer, including during an in-process remount.
void ShutdownPhysicsProbes();

/// Stacks `count` bricks in front of the camera using the current box settings.
/// Alternating layers are rotated a quarter turn, the way a real wall is laid --
/// a tower of aligned blocks is a column of separate stacks and falls apart at a
/// touch.
void SpawnBrickTower(int count);

/// Places one body at the crosshair. Bound to a key so a row can be laid by
/// walking and tapping, which a panel button cannot do -- the panel steals the
/// mouse the moment it is open.
void PlaceProbeAtCrosshair();

/// True while the panel's hotkey switch is on.
bool PhysicsHotkeysEnabled();

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
// Optional single-player showcase bridge; ordinary mission physics is unchanged.
bool PhysicsShowcaseProjectileContact(Vector3Par from, Vector3Par to, Vector3& hit, Vector3& normal);
void PhysicsShowcaseExplosion(Vector3Par position, float hit, float range);
Vector3 PhysicsShowcasePlayerMove(Vector3Par from, Vector3Par to, float height);

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
    /// Solid probe colour, RGB 0..1. Red by default: a probe has to be
    /// distinguishable from the scenery it is resting against, and Everon is
    /// green and sand from one end to the other. Model probes keep their own
    /// textures -- tinting a jeep red would hide the thing you spawned it to see.
    float solidColor[3] = {0.85f, 0.15f, 0.10f};
    /// PROBE-LIGHT: make each probe a light SOURCE, so a shape you can throw and
    /// roll becomes a movable lamp for checking dynamic lighting and the shadows it
    /// casts. Drop one down a dark street, throw one into a room, roll one past a
    /// wall -- the thing the engine's static test scenes cannot give you is a light
    /// that MOVES, and a physics probe already moves convincingly.
    ///
    /// OFF by default: a lit probe changes the scene it is being used to measure,
    /// and somebody dropping a domino row should not have the street light up.
    bool  emitLight = false;
    /// Also draw the probe itself unlit, at full colour, so it reads as the lamp it
    /// now is instead of as a dark ball sitting inside its own light.
    bool  selfIllum = true;
    /// Light colour, RGB 0..1, and how far it reaches. Brightness maps to
    /// LightPoint::SetBrightness, whose attenuation start is 50x this.
    //! The light shines in the probe's OWN colour. A red ball that casts warm white light
    //! is two unrelated controls pretending to be one thing: you set a colour and the lamp
    //! ignored it. Off decouples them and `lightColor` below applies instead.
    bool  lightUsesProbeColor = true;
    float lightColor[3] = {1.00f, 0.90f, 0.72f};
    // 0.05, the bottom of the slider, on the owner's instruction. SetBrightness is really
    // "set radius" -- LightPoint makes _startAtten 50 * this -- so 1.0 was a fifty-metre
    // core that lit a whole village from one thrown ball. 0.05 is a 2.5 m pool: a lamp you
    // can put somewhere, not a floodlight, and the slider goes up from there.
    float lightBrightness = 0.05f;
    //! Size of the visible halo the light draws around itself -- the glow, as opposed to
    //! what the light does to the world. LightPointVisible::SetSize; 0 hides it entirely.
    float lightGlow = 1.0f;
    //! Make the probe a downward CONE instead of a bare point. A point light shines in every
    //! direction and would need six shadow maps; a cone needs one, so this is the switch that
    //! makes a thrown probe able to cast a shadow at all (LGT-010).
    // ON as of LGT-012, because the owner's expectation is the right one: "die probe self
    // illuminating shapes machen auch keinen schattenwurf". They could not -- a point light
    // needs six shadow maps and only a cone needs one, so a probe that emits light has to be
    // a cone before it can occlude anything. Untick it for a bare glowing ball.
    // BACK OFF for the same reason the street lamps went back: a cone lights only inside its
    // cone, and a probe is meant to light the room it is thrown into. Tick it for a torch.
    bool  lightSpot = false;
    float lightConeOuter = 0.55f; //!< outer half-angle, radians (~31 degrees)
    //! Aim the cone along the VIEW direction instead of straight down -- a torch rather than a
    //! lamp. A downward cone puts its shadow under the caster where nothing can see it; a
    //! horizontal one throws a silhouette across the ground and onto walls, which is the only
    //! way to look at a shadow and judge it.
    bool  lightSpotAimView = false;
    /// Ambient the light adds regardless of surface orientation. Small on purpose:
    /// a big ambient term flattens exactly the shading you opened this to look at.
    float lightAmbient = 0.06f;
    /// Let the player shove probes by walking into them.
    /// OFF by default. Ctrl+B/N/M are ordinary game keys otherwise, and a dev
    /// tool has no business claiming them until it is asked to.
    bool  hotkeys = false;
    bool  playerProxy = true;
    float playerRadius = 0.35f;
    float playerHeight = 1.8f;
    /// Model probe: which loaded model to copy, and how heavy to make it.
    char  modelFilter[64] = "jeep";
    float modelMass = 1200.0f;
    /// Newton-seconds a hit transfers. A rifle bullet carries about 4 Ns, which
    /// moves a domino and does nothing at all to a jeep -- so this is a multiplier
    /// on reality, not reality, and it says so in the panel.
    /// Default 4, not 40. At 40 a domino left the county. The scale multiplies a
    /// figure that is ALREADY the projectile's speed, so it was compounding twice.
    float hitImpulseScale = 4.0f;
};

PhysicsProbeSettings& ProbeSettings();

/// Restores the probe settings to their compiled defaults.
void ResetProbeSettings();
void ForgetShowcaseTerrainCrater();

} // namespace Poseidon::Dev
