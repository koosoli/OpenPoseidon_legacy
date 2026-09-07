#pragma once

// SMK-037 and up: the levers for the smoke LOOK work that lives in the RENDERER rather
// than in the particle simulation. Smokes.hpp owns everything that changes what a cloudlet
// IS (how long it lives, how fast it rises, what colour it is shaded); this file owns what
// happens to it once it reaches a pipeline.
//
// Deliberately a separate header. Smokes.hpp is the 2001 particle class plus five
// generations of gameplay levers on top of it, and none of what follows belongs to the
// simulation at all -- it is renderer state that the dev panel edits and the wgpu backend
// reads once per frame.

namespace Poseidon
{

// SMK-037: SOFT PARTICLES. The single most visible thing wrong with the game's smoke.
//
// Every cloudlet is a screen-space billboard (EngineWgpu::DrawDecal): a flat quad with the
// puff's projected depth on all four corners, depth-TESTED against the world and writing
// nothing. So where the quad's plane crosses a wall, the ground, or a soldier, the depth
// test kills the fragments on the far side and keeps the ones in front -- and the boundary
// between the two is a HARD LINE across the sprite. A puff resting on the ground is cut
// off by a razor edge along the terrain; a plume against a house wall has a straight seam
// down it. Nothing in this renderer fades a sprite as it approaches what is behind it.
//
// The fix is the standard one and it is why volumetric-looking smoke in modern engines
// looks the way it does even where it is still billboards: fade the sprite's alpha by how
// close its own depth is to the scene depth already in the buffer. A puff whose depth is
// `fade` metres or more in front of the wall is drawn at full strength; one at the wall is
// drawn at nothing; between the two it ramps. The hard line becomes a soft intersection,
// which is what makes a billboard read as a body of gas touching the geometry instead of a
// decal glued in front of it.
//
// COST, and why it is not free: the fragment shader must READ the scene depth, and at the
// point cloudlets are drawn the depth buffer is bound as a writable attachment of the very
// same pass -- which cannot also be sampled. So the renderer takes a snapshot of the depth
// into an R32Float target once per frame, after the opaque world and before the transparent
// replay. That is one full-screen pass, and it is recorded ONLY while this is enabled.
//
// Seeded from POSEIDON_SMOKE_SOFT / POSEIDON_SMOKE_SOFT_FADE; the Smoke tab edits both
// live, and both reach the renderer on the next frame.
struct SmokeSoftParticles
{
    // OFF until measured, per the project's rule for a new render path. The measurement
    // and the capture that justify the default live in the commit message.
    // ON as of SMK-039. It was landed off because a new render path ships off until it is
    // measured; it now is, on both counts. Cost: +0.08 ms with smoke on screen, +0.015 ms
    // with none (that second number isolates the per-frame depth snapshot, which is the
    // only part that runs when nothing is drawing). Look: the straight scissor cut where a
    // plume crosses grass, a bush or a wall is gone, verified in the capture -- this only
    // ever REMOVES an artefact, it cannot introduce one.
    bool enabled = true;     //!< POSEIDON_SMOKE_SOFT=0 turns it off
    //! Metres of separation at which the sprite is at full opacity. Smaller = a tighter,
    //! harder contact; larger = a longer, mistier fade.
    //!
    //! 0.5 m, and the number is MEASURED, not guessed. The obvious choice is a puff radius
    //! (~1.5 m), and at 1.5 m the plume is gutted -- because a camera looking DOWN a lawn at
    //! a shallow angle has the ground only a metre or two behind almost every puff along the
    //! view ray, so almost every puff is inside the fade band. 0.05 m reproduces the legacy
    //! image almost exactly (which is how the maths was verified); 0.5 m keeps the plume's
    //! body while removing the razor edges against the ground, a bush and a fence.
    float fade = 0.5f;
    //! Decal batches submitted through the soft path since the lever was last turned on.
    //! The one counter that can distinguish "enabled but no smoke reached the renderer"
    //! from "enabled and drawing", which a screenshot alone cannot.
    long long batches = 0;
};
SmokeSoftParticles& GSmokeSoftParticles();

// SMK-038: THE VOLUMETRIC SYSTEM, selectable alongside the legacy cloudlets.
//
// What soft particles cannot fix. SMK-037 repairs the EDGE of a billboard where it meets
// geometry; it cannot give the sprite an interior, cannot stop the plume's silhouette
// popping as the camera moves around it, and cannot light it by anything but the per-particle
// approximation SMK-035 added -- which guesses optical depth from a particle's AGE, because a
// sprite has no density field to look through. And every sprite is still the same round
// texture at the same screen-space roll, so a column reads as repeated identical puffs.
//
// This is the other approach: a camera-anchored 3D density grid (128 x 64 x 128 froxels; the
// cell size sets the reach) that the frame's smoke particles are SCATTERED into by a compute
// pass, and a raymarch that integrates it front to back, stops at the scene depth, and
// composites by transmittance. Stopping at scene depth is what gives soft intersections for
// free -- there is no per-sprite trick left to get wrong. The self-shadow is a short second
// march toward the sun per sample, which is light actually travelling through the medium
// rather than a guess from a particle's age.
//
// The particles it eats are the ones SampleCloudletShadow already collects for the
// ground-shadow pass, so gameplay smoke feeds it with no new plumbing in the simulation.
//
// MODE 1 ALSO STOPS THE BILLBOARDS DRAWING. Anything injected into the field must not also
// be drawn as a sprite or the smoke is rendered twice. Mode 2 keeps both deliberately: it is
// the A/B view, not a shipping setting.
//
// REACH IS THE TRADE, and it is the honest limitation of any camera-anchored volume. At the
// default 1 m cell the box is 128 x 64 x 128 metres around the camera, and smoke outside it
// is not rendered at all in mode 1. Raise the cell size for reach, lower it for detail.
enum class SmokeSystemMode : int
{
    Legacy = 0,     //!< the 2001 billboards, unchanged; the volumetric subsystem is inert
    Volumetric = 1, //!< the froxel field; the injected billboards stop drawing
    Both = 2,       //!< debug A/B: double-counts the smoke on purpose
};

struct SmokeVolumetricParams
{
    // Legacy, per the standing rule for a new render path. POSEIDON_SMOKE_SYSTEM=1 selects
    // the volumetric system, =2 draws both.
    SmokeSystemMode mode = SmokeSystemMode::Legacy;
    float cellSize = 1.0f;     //!< POSEIDON_SMOKE_VOL_CELL: metres per froxel
    int marchSteps = 48;       //!< POSEIDON_SMOKE_VOL_STEPS: samples along the view ray
    int sunSteps = 4;          //!< POSEIDON_SMOKE_VOL_SUN_STEPS: 0 = no self-shadow march
    float sunStepLen = 1.5f;   //!< metres, first sun step; they widen geometrically
    // 0.3, and the number is measured, not chosen. The injected sigma is a SUM over every
    // particle covering a cell, and a smoke grenade puts hundreds of cloudlets into a few
    // cubic metres -- at 1.0 the field saturates to a featureless white slab with the grid's
    // own box edges showing through it. 0.03 is thin mist, 0.1 a haze, 0.3 a body of smoke
    // that veils a house and a bush without going flat. Halving the cell size roughly
    // doubles the density needed for the same look, because the same particles land in
    // eight times as many cells.
    float density = 0.3f;      //!< POSEIDON_SMOKE_VOL_DENSITY
    float extinction = 1.0f;   //!< multiplies sigma_t at march time
    float albedo = 0.9f;       //!< single-scattering albedo: soot is dark, steam near 1
    float anisotropy = 0.35f;  //!< Henyey-Greenstein g; >0 rims a backlit plume
    float selfShadow = 1.0f;   //!< POSEIDON_SMOKE_VOL_SELFSHADOW, 0..1
    float jitter = 1.0f;       //!< step jitter: trades banding for noise
    int scale = 2;             //!< POSEIDON_SMOKE_VOL_SCALE: 1 = render res, 2 = half
    //! Counters, read back from the renderer so the panel can separate "on but nothing
    //! arrived" from "on and marching" -- the one thing a screenshot cannot tell you.
    long long blobs = 0;
    long long framesMarched = 0;
};
SmokeVolumetricParams& GSmokeVolumetric();

//! True when the legacy billboard for an INJECTED particle must not be drawn as well.
inline bool SmokeBillboardsSuppressed()
{
    return GSmokeVolumetric().mode == SmokeSystemMode::Volumetric;
}

} // namespace Poseidon
