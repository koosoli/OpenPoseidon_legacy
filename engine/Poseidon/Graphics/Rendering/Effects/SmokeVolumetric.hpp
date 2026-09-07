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
    bool enabled = false;    //!< POSEIDON_SMOKE_SOFT=1 turns it on
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

} // namespace Poseidon
