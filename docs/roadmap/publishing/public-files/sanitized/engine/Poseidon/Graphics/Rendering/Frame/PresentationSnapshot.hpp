#pragma once

// RenderSnapshot slice S1 (design notes §3): the ENVIRONMENT
// block of the presentation snapshot -- every scalar the WGPU render path used to pull
// from live global state mid-frame (GScene's main light, fog ranges, camera pose,
// Glob's two clocks, the landscape's weather, GWind, GRain), captured ONCE per frame
// into a value the renderer then consumes.
//
// Why a value and not "just read the globals": the reads are scattered across ~30
// sites in three renderer files, several of them repeated per frame (NightEffect was
// read five times), and each carries its own null-check spelling. Collapsing them here
// makes the renderer's environment input auditable, makes the frame's values constant
// by construction, and turns the eventual move to a world-built snapshot (Phase 5
// step 1 proper) into relocating ONE call instead of re-touching every site.
//
// The has* booleans mirror the exact null-checks the sites used to perform (GScene
// set but MainLight missing is a real state in tools), so behaviour with partial
// worlds is unchanged. Main thread only; captured by the renderer at InitDraw today
// -- before Landscape::Draw, so the terrain/water submodules read the same frame's
// values -- and by World::SimulateAndDraw in a later slice.

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>

#include <cstdint>

namespace Poseidon::render::frame
{

struct PresentationSnapshot
{
    bool valid = false;

    // --- Identity (roadmap 5.1: "snapshot generation and resource-lifetime epoch") ----

    // Monotonic count of publishes since process start. 0 means "never published"; the
    // first published snapshot is generation 1, and every later publish is strictly
    // greater. This is what lets a consumer say WHICH frame's values it is holding
    // rather than only "the current ones" -- the property the ring below exists to make
    // meaningful, and the one a render thread will key its temporal history off when
    // step 6 stops guaranteeing one publish per consume.
    uint64_t generation = 0;

    // Resource-lifetime epoch. Bumped when the set of GPU-visible resources a snapshot
    // may reference is invalidated WHOLESALE -- world unload, device loss, renderer
    // restart -- so an in-flight snapshot stamped with an older epoch can be rejected
    // instead of dereferencing handles that no longer name anything.
    //
    // SEAM, NOT YET FED: nothing bumps this today, deliberately. The snapshot currently
    // carries no resource handles at all -- every field is a scalar or a by-value light
    // / rotor record, copied out of live state at capture -- so there is nothing an
    // epoch could invalidate, and wiring a producer now would mean inventing a fake
    // one. The field is stamped on every publish and echoed into the diagnostics so
    // that when §4's stable render-object IDs land and the snapshot starts carrying
    // handles, the only work is calling BumpPresentationResourceEpoch() from the
    // invalidation points; the ABI and the plumbing are already here.
    uint32_t resourceEpoch = 0;

    // GScene: distance fog, plus the object-cull/LOD inputs PushSceneCamera feeds the
    // GPU cull (Scene::LevelFromDistance2's shared values). Frame-constant; the camera
    // matrices themselves stay live pulls because UpdateProjection() legitimately
    // changes the projection mid-draw (MAT-052 cockpit zSpace).
    bool hasScene = false;
    float fogMinRange = 0.0f;
    float fogMaxRange = 0.0f;
    float objectDrawDistance = 0.0f;
    float lodInvWidth = 0.0f;

    // GScene->GetCamera(): the pose the environment passes key off (sky raymarch
    // origin, sun-glare focus). The pushed per-camera matrices are untouched.
    bool hasCamera = false;
    Vector3 cameraPos = VZero;
    Vector3 cameraDir = VForward;

    // GScene->MainLight() (LightSun). sunDiffuse/sunAmbient are the RAW colours;
    // eye accommodation is engine-side state and stays with the consumer.
    bool hasMainLight = false;
    Color sunDiffuse = HWhite;
    Color sunAmbient = HWhite;
    Vector3 sunTravelDir = Vector3(-0.4f, -0.85f, -0.3f); // Direction(): light travel (moon-swung at night)
    Vector3 sunDirection = VUp;                           // SunDirection(): always astronomical
    float nightEffect = 0.0f;
    Vector3 moonDirection = VUp;
    float moonLightAmount = 0.0f;
    float moonPhase = 0.5f;
    float moonIllumination = 1.0f;
    float moonAngularRadius = 0.00452f;
    float moonBrightness = 0.0f;
    Vector3 moonSunDirection = VUp;

    // Glob's two clocks.
    float timeSeconds = 0.0f; // Glob.time.toFloat(): the water/cloud/sway clock
    float timeOfDay = 0.5f;   // Glob.clock.GetTimeOfDay(): 0..1, drives ToD presets

    // GLandscape weather + sea state.
    bool hasLandscape = false;
    float overcast = 0.0f;
    float weatherFog = 0.0f;
    float seaLevel = 0.0f;
    float landscapeRainDensity = 0.0f;

    // GRain (the visible-drops system) and GWind.
    float rainEffectiveDensity = 0.0f;
    bool rainParticleSnowflakes = false; // Effective particle density may be snow, not liquid rain.
    float seaRippleGain = 0.0f;
    bool windActive = false;
    float windSpeed = 0.0f;
    float windDirectionRad = 0.0f;
    float windGustFraction = 0.0f;
    float windMeanSpeed = 0.0f;        // gust-free mean: the cloud deck / sea spectrum inputs
    float windMeanDirectionRad = 0.0f;

    // --- Slice S2: interactors -------------------------------------------------------
    // The camera-attached entity (player on foot, or the occupied vehicle): the grass
    // crush field. Raw facts only; the consumers keep their radius/strength formulas.
    bool hasInteractor = false;
    Vector3 interactorPos = VZero;
    float interactorVisibleSize = 0.0f;
    bool interactorRotor = false;       // the occupied vehicle is a helicopter with blades turning
    float interactorRotorSpeed = 0.0f;  // 0..1

    // Every helicopter in the world with its rotor still turning (the occupied one
    // included, so the just-dismounted handoff needs no weak link on the renderer),
    // nearest-to-camera first when more exist than fit. groundY is the terrain height
    // under it, captured here so the consumers stop reading GLandscape per candidate.
    struct RotorSource
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float rotorSpeed = 0.0f; // 0..1
        float groundY = 0.0f;
    };
    static constexpr int kMaxRotorSources = 8;
    int rotorSourceCount = 0;
    RotorSource rotorSources[kMaxRotorSources] = {};

    // TW-WATER W6: the boats nearest the camera, for the Tidewater wake simulation (one
    // simulated wake per boat; the renderer keeps the ids stable across frames). Hull size from
    // the shape's bounding box, draft from its bottom against the sea level.
    struct WakeBoat
    {
        float x = 0.0f, z = 0.0f;       // hull centre (world)
        float fwdX = 0.0f, fwdZ = 1.0f; // heading (world, unnormalised XZ of the model forward)
        float velX = 0.0f, velZ = 0.0f; // world velocity
        float thrust = 0.0f;            // 0..1
        float halfBeam = 1.0f, halfLength = 3.0f, draft = 0.5f;
        uint32_t id = 0;
        bool driven = false;
        float distance2 = 0.0f; // to the camera (selection only)
        // W6i (bow spray): the full pose -- origin = hull centre at the design waterline (world),
        // the model forward and up axes (world, unit) -- the vertical velocity, the freeboard
        // (sheer height above the waterline), and the CPU sea height (QueryWaterSurfaceScaled, the
        // sea the boat floats on) at the bow contact points and at the propeller. The contact
        // points follow the rule in WakeSprayContactPoint (PresentationSnapshot.cpp) and
        // boat_spray.rs `contact_points`: the two must match.
        float originY = 0.0f;
        float fwdY = 0.0f;
        float upX = 0.0f, upY = 1.0f, upZ = 0.0f;
        float velY = 0.0f;
        float freeboard = 0.8f;
        float hwContact[6] = {};
        float hwProp = 0.0f;
    };
    static constexpr int kMaxWakeBoats = 2;
    int wakeBoatCount = 0;
    WakeBoat wakeBoats[kMaxWakeBoats] = {};

    // --- Slice S3: local lights --------------------------------------------------------
    // The scored, camera-sorted point/spot light set for the frame, produced by ONE walk
    // of the scene's light list here instead of two walks inside the renderer's NextFrame
    // (a daylight-visible probe plus the scoring pass). Colours arrive pre-scaled by the
    // per-light night gate, exactly as the renderer's walk produced them; the renderer
    // only converts to its ABI struct and applies its own capacity/env clamp. Sorted
    // nearest-fit first (the score prefers lights whose attenuation volume contains the
    // camera). `lightCandidates` is the pre-cap count for the Forward+ gauges.
    struct LocalLight
    {
        Vector3 pos = VZero;
        float startAtten = 0.0f;
        Color diffuse = HBlack;
        Color ambient = HBlack;
        Vector3 dir = VForward;
        bool spot = false;
        // LGT-010: the OUTER cone half-angle, radians. Dropped until now because nothing
        // downstream needed it -- the shader's cone term is a fixed pair of constants. A
        // shadow view does need it: it is the field of view of the camera standing at the
        // bulb, and getting it wrong either wastes the whole map on empty cone or clips the
        // beam's own edge away.
        float coneOuter = 0.0f;
        float coneInner = 0.0f; //!< inner half-angle, radians: full brightness inside it
        //! LGT-014: reach as a multiple of startAtten. Also the score's range, below, which
        //! is why a lamp with a deliberately small core used to fall out of the light budget
        //! at a distance as well as fading out of the shader.
        float endAttenScale = 10.0f;
    };
    static constexpr int kMaxLights = 256;
    int lightCount = 0;
    uint32_t lightCandidates = 0;
    LocalLight lights[kMaxLights] = {};
};

// --- The ring (roadmap Phase 5 step 5, "double- or triple-buffer the snapshots") -----
//
// WHY TWO AND NOT THREE. Publishing used to overwrite one global instance in place,
// which means a consumer holding a reference to it was reading the same storage the
// next publish would scribble on. That is fine while the producer (World::Simulate) and
// the consumer (the render path) are the same thread and strictly alternate -- but it
// is the exact invariant step 6 breaks, and it cannot be reasoned about locally because
// the reference escapes to ~30 read sites across three renderer files.
//
// Two slots make the writer's target disjoint from the reader's source: a publish fills
// the slot that is NOT current, then flips the index, so a `const PresentationSnapshot&`
// taken at the top of a frame stays valid and unmodified for that whole frame no matter
// what the producer does. That is the property worth having now.
//
// A third slot buys nothing until the producer may run AHEAD of the consumer -- i.e.
// until step 7's simulation/render overlap, where a producer that is one frame ahead
// needs a slot to write while the consumer holds one and a third holds the newest
// complete result. Today the producer cannot run ahead (it calls InitDraw itself, on the
// same thread), so a third slot would only add a snapshot's worth of storage and, worse,
// absorb the "published without being consumed" event that 5.3 asks us to keep VISIBLE.
// Raise this when the threads split and the diagnostics show the drop; not before.
inline constexpr int kPresentationSnapshotRingSize = 2;

// The current published snapshot. Const: consumers read, they never write, and taking
// one reference for the whole frame is the intended usage. Before any publish this
// returns a default-constructed snapshot (valid == false, generation == 0), which is
// exactly the state the consuming sites already handled for "no world up".
const PresentationSnapshot& GPresentationSnapshot();

// Publish a snapshot the caller has already filled: writes the next ring slot, stamps
// generation and the resource epoch, then flips the current index. Separated from the
// capture below so a test (and, later, a world-side producer that builds the value
// itself) can drive the ring without live globals.
void PublishPresentationSnapshot(const PresentationSnapshot& snap);

// Fill from live globals and publish. Null-safe: with no world/scene up, the has*
// flags stay false and every field keeps the default the consuming sites used.
// Phase 5 step 1: the WORLD calls this at the simulation/draw boundary
// (World::Simulate, right before GEngine->InitDraw) — simulation publishes,
// the renderer consumes.
void CapturePresentationSnapshot();

// Renderer-side fallback for frames the world does not drive (main menu with no
// world, tools): captures only when no capture has happened since the last
// RetirePresentationSnapshot(). Called by EngineWgpu::InitDraw.
void CapturePresentationSnapshotIfStale();

// Marks the frame's snapshot consumed; the next InitDraw with no world-side capture
// re-captures instead of serving a previous frame's values. Called at FinishDraw.
void RetirePresentationSnapshot();

// Bump the resource-lifetime epoch: every snapshot published after this call is stamped
// with the new value, and any snapshot still in flight with the old one names resources
// that may no longer exist. See PresentationSnapshot::resourceEpoch — NOTHING CALLS THIS
// YET, by design; it is the declared producer end of that seam so the invalidation
// points have something to call the day the snapshot starts carrying handles.
void BumpPresentationResourceEpoch();

// Ring introspection. For tests and diagnostics only — the frame path uses
// GPresentationSnapshot(). `PresentationSnapshotSlot` indexes the raw ring so a test can
// prove slot REUSE (that publish N+kRingSize lands back in publish N's storage) rather
// than only that the accessor returns something plausible.
int PresentationSnapshotSlotIndex();
const PresentationSnapshot& PresentationSnapshotSlot(int index);

// Tests only: clears the ring, the generation counter, the epoch, the freshness flag
// and the diagnostics counters, so ordering assertions do not depend on what ran first.
void ResetPresentationSnapshotRingForTest();

// POSEIDON_SNAPSHOT_VALIDATE=1: re-read the live values at end of draw and log every
// field that changed since capture — the intra-frame-constancy oracle. No-op when the
// env var is unset. Called by the renderer's FinishDraw.
void ValidatePresentationSnapshot();

} // namespace Poseidon::render::frame
