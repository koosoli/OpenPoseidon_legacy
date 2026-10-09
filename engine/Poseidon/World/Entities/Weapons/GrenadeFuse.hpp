// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon::GrenadeFuse
{

/// How a fused round loses energy when it hits something.
///
/// These are NOT measurements. A hand grenade is an irregular lump with a lever
/// on it, and how far it travels after landing depends on which face it lands
/// on. They are exposed at runtime rather than compiled in because the only
/// useful test is somebody throwing one and saying whether it looked right --
/// which is also the review this got: "grenades aren't ideally round so they
/// roll way less in comparison to the sphere that is the collider" (Wetzer,
/// 2026-08-29). That comment is about `tangentialKeep`.
struct Settings
{
    /// Master switch. Off restores contact detonation for everything, whatever
    /// the configs say, so a bad set of numbers is one click from harmless.
    bool enabled = true;

    /// Fraction of the speed INTO the surface that comes back out.
    float restitution = 0.32f;

    /// Fraction of the speed ALONG the surface that survives the scrape. This is
    /// the one that decides whether a grenade sits down where it lands or skates
    /// on. At 0.62 one arriving flat at 15 m/s keeps about 9 m/s and slides
    /// several metres; lower makes it stick.
    float tangentialKeep = 0.32f;

    /// Below this the remainder is not worth simulating and the round is left to
    /// lie and cook off. Raising it makes grenades settle sooner.
    float restSpeed = 2.0f;

    /// Overrides every config `explosionTime` when above zero, so the delay can
    /// be tried at 2 s and 5 s without rebuilding or touching CONFIG.BIN. Zero
    /// means the ammo config decides, which is the shipping behaviour.
    float fuseOverrideSeconds = 0.0f;

    /// AI-XRAY-003 (owner 2026-10-09: "make fuses work"): the fuse a THROWN hand grenade gets when its
    /// config declares none. Stock CWA's GrenadeHand declares none and went off the instant it touched
    /// anything, so a grenade landing at a soldier's feet left nothing to react to. 4 s is about an M67 or
    /// an RGD-5. Zero gives back the stock contact detonation. Launched and fired rounds (M203, mortars)
    /// are not thrown and are never given one.
    float handGrenadeFuseSeconds = 4.0f;
};

/// The live settings. Mutable on purpose: the dev panel binds sliders straight
/// to these, so unlike a tab that edits a stack-local copy of a settings struct
/// this can be reset safely from outside.
Settings& Get();

/// Back to the values written above.
void Reset();

/// Seconds on the fuse of a new round, 0 for contact detonation: the config's `explosionTime`, else
/// handGrenadeFuseSeconds for a thrown grenade; the panel override replaces either. Off (`enabled` false):
/// always 0.
float FuseSeconds(float explosionTime, bool thrown);

/// What a bounce does to a velocity.
struct BounceResult
{
    /// The velocity after the surface has taken its share.
    Vector3 speed;
    /// True when what is left is too slow to be worth simulating, and the caller
    /// should let the round lie where it is and count down.
    bool atRest = false;
};

/// Reflects `speed` off a unit `surfaceNormal` and bleeds energy per `settings`.
///
/// A free function rather than a method because this is the only part of the
/// bounce that is arithmetic -- finding the surface is the shell's problem, and
/// keeping the two apart is what makes this testable without a world, a
/// landscape and a live projectile.
///
/// `surfaceNormal` must be unit length and point AWAY from the surface. Callers
/// that get their normal from CollisionInfo have to flip it themselves; the
/// convention there is documented at the struct and is not this function's to
/// second-guess.
BounceResult Bounce(Vector3Par speed, Vector3Par surfaceNormal, const Settings& settings);

} // namespace Poseidon::GrenadeFuse
