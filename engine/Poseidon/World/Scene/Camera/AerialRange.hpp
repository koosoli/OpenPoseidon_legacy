#pragma once

// FAR-001: the camera far plane, decoupled from the fog range.
//
// THE DEFECT THIS ANSWERS. Until now the two were literally one number.
// `World.cpp` handed `Scene::GetFogMaxRange()` straight to
// `Camera::SetPerspectiveForView` as the far plane, and `Scene::ResetFog()`
// derives that fog range from `ENGINE_CONFIG.horizontZ` -- the view-distance
// slider, which DEFAULTS TO 900 m. So the answer to "what may be drawn at all"
// and the answer to "what has faded into haze" were the same 880 m.
//
// On the ground that coincidence is harmless and even convenient: geometry is
// clipped exactly where it had already gone fully opaque with fog, so nothing
// visibly pops at the clip plane. In the air it is the whole bug. Ground 5 km
// below an aircraft is 5 km away, so at the default setting the world stops
// being drawn from about 900 m of altitude upwards. FLY-001 measured it by
// freefly capture on Malden looking down 35 degrees:
//
//   1,500 m, default view distance -> terrain, coastline, roads, all correct.
//   5,000 m, default view distance -> NO GROUND AT ALL. Cloud and sky only.
//   5,000 m, --vd 10000            -> sea and terrain return, heavily fogged.
//  14,000 m, --vd 10000            -> no ground; 14 km is past the 10 km cap.
//
// and FLY-001 had just raised the A-10's ceiling to ~14 km, so the aircraft now
// climbs comfortably past the altitude at which its world disappears.
//
// WHY NOT SIMPLY RAISE THE VIEW DISTANCE. Because the view distance is the wrong
// lever twice over. It is a GLOBAL setting, so raising it to suit an aircraft
// changes what an infantryman sees, what the AI can see (VehicleAICombat reads
// `horizontZ`), how many shadows are budgeted and how much terrain is resident;
// and VD-001 measured that going 5 km -> 10 km costs +2.35 GPU ms of which 74%
// is the WATER surface reaching further, not anything the pilot wanted. It is
// also the lever that sets the fog ramp: `MAX_FOG` is `horizontZ - 20`, so a
// 10 km view distance stretches the haze ramp over 10 km and the OFP atmosphere
// effectively vanishes at ground level. The far plane has to be able to move
// without dragging any of that with it.
//
// WHAT THIS DOES INSTEAD. One altitude-driven policy, resolved per frame, that
// returns TWO numbers where there was one:
//
//   farPlane    -- what may be drawn. Opens with altitude.
//   fogMaxRange -- what has faded into haze. Also opens with altitude, but by a
//                  law that depends only on altitude and never on the player's
//                  view-distance slider, and always stays just inside the far
//                  plane so geometry finishes fading before it is clipped.
//
// THE HAZE STILL HAS TO BE THERE. A far plane opened to 24 km over a fog range
// still pinned at 880 m does not show you the ground: it shows you 24 km of
// solid fog-coloured nothing, which looks exactly like the empty horizon it was
// meant to fix. So the fog range must open too -- the point of the decoupling is
// not that fog stays put, it is that fog is no longer computed from the view
// distance. Ground-level haze is untouched (see the inert case below); what
// opens is the haze seen from an aircraft, which is physically right, since an
// aircraft at 8 km is looking down through the thin top of the atmosphere rather
// than sideways along its dense bottom.
//
// GROUND PLAY MUST NOT MOVE, AND IS PROVED NOT TO. Below `startAlt` the resolver
// returns the caller's own fog range for BOTH numbers, bit for bit, and reports
// `active == false`; every caller then takes the path it took before this
// existed. No new arithmetic touches the ground-level frame, the fog tables are
// not even rebuilt, and the terrain LOD ring is not armed. That is what the
// ground-level control capture is for.
//
// GL33. On wgpu the projection is overridden to infinite-far reversed-Z
// (EngineWgpu.cpp), where depth precision is governed by the NEAR plane and is
// completely insensitive to the far plane -- a 24 km far plane costs nothing
// there. GL33 keeps a finite forward-Z projection, where precision IS a ratio of
// far to near and a 24 km far plane over a 0.08 m near plane is exactly the
// configuration that z-fights. So the resolver takes `infiniteFarZ` and, when it
// is false, clamps the reach to `finiteFarReach` -- a far smaller opening that
// still gets an aircraft its ground back at a few kilometres without wrecking
// the depth buffer it has. GL33 is fallback-only here (see the standing note),
// so it gets a smaller, safe version of the feature rather than a broken copy of
// the wgpu one.

namespace Poseidon::Aerial
{

/// Runtime-switchable parameters. Plain struct behind an accessor rather than
/// loose globals: the call sites are in three translation units (World, Scene
/// fog, Landscape) and the dev panel edits the same values they read.
struct AerialRangeSettings
{
    /// Master switch. false restores the exact pre-FAR-001 behaviour everywhere:
    /// far plane == fog range == the view-distance-derived number.
    bool enabled = true;

    /// Altitude ABOVE GROUND, in metres, below which the policy is completely
    /// inert. Above ground rather than above sea level on purpose: 50 m over a
    /// 900 m mountain is low flying and must look like low flying, not like
    /// high-altitude flight that happens to be near rock.
    float startAlt = 400.0f;

    /// Altitude above ground at which the policy is at full strength. Between
    /// `startAlt` and this the reach is ramped in smoothly, because a step
    /// change in the far plane and the fog range is a visible pop.
    float fullAlt = 1400.0f;

    /// Slant reach wanted, as a multiple of altitude above ground. 3.0 means an
    /// eye at 5 km asks for 15 km, which is roughly the whole of Everon
    /// (12.8 km across) and reads as "I can see the island I am flying over".
    float reachFactor = 3.0f;

    /// Hard ceiling on the reach, metres. Bounds the terrain rectangle, the
    /// segment cache and the water surface however high the camera goes.
    float maxReach = 24000.0f;

    /// The same ceiling for a backend with a FINITE far plane (GL33). Much
    /// lower, because there the far plane is what costs depth precision.
    float finiteFarReach = 6000.0f;

    /// The far plane is this multiple of the fog range. Slightly greater than 1
    /// so that geometry is fully hazed BEFORE it reaches the clip plane -- if
    /// the two coincide exactly, terrain pops out of existence at a visible
    /// circular edge instead of dissolving.
    float clipMargin = 1.15f;

    /// Terrain beyond the ordinary (ground-level) fog range is generated at this
    /// LOD instead of LOD 0. The land tree already implements LOD 1 and 2 in
    /// `GenerateSegmentInto` but has always been called with 0 ("All segments
    /// generated at LOD 0 to prevent T-junction gaps"); this is what finally
    /// uses it, and only out in the ring where a T-junction is kilometres away
    /// and behind most of the haze. Set to 0 to draw the far ring at full
    /// detail -- which is the measurement that says what the coarse tier buys.
    int coarseLod = 2;

    /// false leaves the terrain ring at LOD 0 everywhere (the pre-FAR-001
    /// behaviour) while still opening the far plane and the fog. The A/B that
    /// prices the coarse tier.
    bool coarseTerrain = true;

    /// Half-width, in metres, of the rectangle handed to the WATER renderer. 0
    /// (the default) means "follow the terrain rectangle", i.e. no cap.
    ///
    /// A DIAGNOSTIC LEVER, NOT A PERFORMANCE ONE -- and this note says so because
    /// the first draft of it claimed the opposite and the measurement refuted it.
    ///
    /// The water IS where the whole cost of opening the far plane lives. Malden,
    /// 5,000 m, 15 km reach: GPU frame 6.18 -> 11.17 ms, of which terrain was
    /// +0.23 ms and water draw was +5.17 ms (1.08 -> 6.25). VD-001 saw the same
    /// shape going 5 km -> 10 km of view distance, where 74% of the cost was
    /// water. It is entirely reasonable to conclude from that that a smaller
    /// water rectangle would be cheaper.
    ///
    /// It is not. Measured at 8,000 m, changing ONLY this cap:
    ///     no cap  -> water draw 5.44 ms
    ///     12 km   -> 5.54 ms
    ///      6 km   -> 4.96 ms
    /// which is run-to-run spread, not a trend. The cost is FILL, not area: from
    /// altitude the sea covers the same screen no matter how far the rectangle
    /// extends, and the shading of those pixels is the bill. Shrinking the
    /// rectangle just moves the horizon nearer without unshading anything.
    ///
    /// So the ~5 ms is not reducible from here. Making it cheaper means cheaper
    /// per-pixel water at distance -- a job for the wgpu water renderer, which
    /// this change is not permitted to touch.
    ///
    /// The cap is kept because it is the one handle on the water from this side,
    /// and it was the instrument that ruled the rectangle out as the cause of the
    /// high-altitude scanline artefact (it is not: capping to 6 km cleaned the
    /// 8,000 m frame but not the 14,000 m one).
    float waterReach = 0.0f;
};

/// The two numbers, resolved.
struct AerialRange
{
    /// Camera far plane, metres.
    float farPlane = 0.0f;
    /// Fog max range, metres. Always <= farPlane.
    float fogMaxRange = 0.0f;
    /// false means "nothing changed": both fields are the caller's own input and
    /// every caller should take its original path.
    bool active = false;
};

/// The live settings. Mutated by the dev panel and by environment overrides
/// applied on first use (POSEIDON_AERIAL_RANGE, POSEIDON_AERIAL_REACH,
/// POSEIDON_AERIAL_COARSE_LOD, POSEIDON_AERIAL_COARSE_TERRAIN).
AerialRangeSettings& Settings();

/// Resolve the far plane and the fog range for a camera `altAboveGround` metres
/// above the terrain under it.
///
/// `fogBaseMaxRange` is the fog range the engine would have used without this --
/// i.e. the view-distance-derived one. `infiniteFarZ` is true on a backend whose
/// projection is infinite-far reversed-Z (wgpu) and false on a finite forward-Z
/// one (GL33); it selects which reach ceiling applies.
///
/// Returns `{fogBaseMaxRange, fogBaseMaxRange, false}` unchanged whenever the
/// policy is inert, which is every frame of ordinary ground play.
AerialRange Resolve(float fogBaseMaxRange, float altAboveGround, bool infiniteFarZ,
                    const AerialRangeSettings& s = Settings());

} // namespace Poseidon::Aerial
