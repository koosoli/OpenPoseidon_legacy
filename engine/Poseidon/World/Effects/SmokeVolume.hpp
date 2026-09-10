#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Effects/SmokeDensityGrid.hpp>

#include <vector>
#include <algorithm>
#include <unordered_map>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// A smoke plume: ONE scene entity that owns many particles.
//
// The legacy design (Graphics/Rendering/Effects/Smokes.cpp) makes every puff a
// full Vehicle in the world — its own LOD shape, its own entry in the scene's
// simulate and draw lists, its own per-puff sun-plus-point-lights solve inside
// Object::DrawDecal. That is where the cost is, and it is also why the legacy
// smoke has no collision: there is nowhere sensible to put a sweep when each
// particle is an independent entity being ticked by the generic simulator.
//
// Here the plume is the entity. Consequences that matter:
//
//   * One scene-list entry, one lighting solve, one texture bind per plume
//     instead of per particle.
//   * The particle loop is a flat array, so the collision budget can be
//     rationed across the whole plume rather than guessed at per puff.
//   * Wind is re-sampled every frame per particle instead of being baked in at
//     spawn. Legacy bakes it (Smokes.cpp CloudletSource::Simulate), which is
//     why changing the wind never moves smoke that is already in the air.
//
// This does NOT replace the legacy path. It runs alongside it, off by default,
// so the two can be spawned side by side and compared — see SmokeSystem.
// ---------------------------------------------------------------------------

/// Everything a plume's look and behaviour is made of. All of it is live-
/// editable from the dev panel; none of it is authored in config yet, because
/// the first consumers are dev-spawned emitters and smoke grenades.
struct SmokeParams
{
    // -- emission ---------------------------------------------------------
    float rate = 35.0f;             ///< particles per second
    float particleLifetime = 14.0f; ///< seconds
    float emitterRadius = 0.35f;    ///< nozzle radius, m
    float initialSpeed = 2.2f;      ///< upward jet at the nozzle, m/s
    float spread = 0.30f;           ///< lateral randomisation of the jet, 0..1

    // -- appearance -------------------------------------------------------
    float startRadius = 0.5f; ///< particle radius at birth, m
    float endRadius = 5.0f;   ///< particle radius at death, m
    /// Peak per-particle alpha. This is the "thickness" control: raising it
    /// makes the plume opaque without changing how it moves. Note that overlap
    /// compounds, so 0.35 already reads as solid smoke in the core of a plume.
    float opacity = 0.35f;
    float fadeIn = 0.12f;  ///< fraction of life spent fading up
    float fadeOut = 0.45f; ///< fraction of life spent fading out
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;

    // -- volumetric lighting ---------------------------------------------
    /// Self-shadow: each particle marches toward the sun through the plume's
    /// own particles and darkens by the density it passes (Beer-Lambert). This
    /// is what gives a thick column a dark underside and core with a bright
    /// sun-facing rim -- the difference between a lit volume and a sticker.
    /// 0 turns it off (flat, the first-pass look).
    float selfShadowStrength = 2.2f;
    /// How far the march reaches, in puff-sized cells. 4 covers a normal plume;
    /// more only matters for a very tall column and costs per particle.
    int selfShadowSteps = 6;
    /// Weight of the cool sky term on the plume's top. Smoke in sun is white on
    /// top and blue-grey in shadow; this is the blue-grey.
    float skyAmbient = 0.55f;
    /// How strongly this plume shadows the GROUND (and everything lit under
    /// it) via the renderer's shadow map. 1 = its opacity; 0 = casts none.
    float groundShadow = 1.6f;

    // -- motion -----------------------------------------------------------
    /// Upward acceleration while the smoke is still hotter than the air, m/s^2.
    float buoyancy = 2.4f;
    /// Fraction of lifetime over which buoyancy decays to zero as the plume
    /// cools. Without this a plume rises forever and never forms a drifting
    /// layer, which is the single most obvious tell of fake smoke.
    float buoyancyDecay = 0.35f;
    /// How much LONGER the plume stays buoyant in dead-still air than in a
    /// breeze. Wind mixes and cools a plume; calm lets it climb. 1 = no
    /// difference; 3 = a calm-day column rises roughly three times as far.
    float calmRiseBoost = 3.0f;
    /// How hard the particle is dragged toward the local air velocity, 1/s.
    /// This is what makes wind work: the particle relaxes toward the air rather
    /// than having wind added to it, so a gust accelerates it and a lull lets it
    /// coast, both for free.
    float drag = 1.1f;
    float turbulence = 0.55f; ///< random acceleration magnitude, m/s^2
    /// TURBULENT DIFFUSION. A real plume does not just grow, it comes APART:
    /// eddies pull it into filaments and the puffs wander away from each other,
    /// which is what turns a column into a drifting cloud. Modelled as a random
    /// walk whose step grows with the square root of age -- Fickian diffusion,
    /// the same sqrt(t) spread a smoke puff actually has. 0 = the old rigid
    /// plume that keeps its shape forever.
    float diffusion = 1.1f;
    /// How much the puff THINS as it expands. Alpha was independent of radius,
    /// so a 10 m puff was as opaque as a 0.5 m one and a big plume read as a
    /// solid blob rather than a thinning cloud. Now alpha scales by
    /// (startRadius / radius)^densityFalloff -- a fixed amount of soot spread
    /// over a growing volume. 0 = the old behaviour; 1 = area-ish; 2+ thins
    /// very fast. This is the single strongest "it became a cloud" cue.
    float densityFalloff = 0.85f;
    /// Scales the PREVAILING wind only (0 = ignores weather entirely). Local
    /// airflow — rotor downwash — is not affected by this.
    float windResponse = 1.0f;

    // -- collision --------------------------------------------------------
    bool collide = true;
    /// Hard containment. Sweeps EVERY tick from the last position to this one
    /// and, if the segment crossed a surface, puts the particle back on the
    /// side it came from before responding -- a particle can be stopped, it
    /// cannot cross. Costs 7 rays per particle per frame. This is what makes
    /// smoke stay in a room and find the door; the reactive default lets a
    /// wide puff's rim through a thin wall.
    bool contain = true;
    /// 0 = pure slide along the surface, 1 = elastic. Smoke is not a ball;
    /// values above about 0.3 look wrong. The default is a hint of rebound.
    float restitution = 0.15f;
    /// Tangential speed lost at a contact, 0..1. This is what makes smoke crawl
    /// along a wall rather than skate down it.
    float friction = 0.30f;
    /// Fraction of the visual radius used for the collision sphere. Less than 1
    /// on purpose: a puff's visible edge is diffuse, and colliding on the full
    /// radius makes plumes stand visibly off walls. 0.35 let 3 m of a 5 m puff
    /// poke through a wall, which is what "clips through walls" was; 0.7 keeps
    /// the dense core inside with only the faint rim crossing.
    float collisionRadiusScale = 0.70f;

    // -- shelter ----------------------------------------------------------
    /// While sheltered, the prevailing wind is scaled by this. The requirement
    /// is that indoor smoke ignores the weather; a small residual keeps a
    /// draughty interior from looking dead.
    float shelteredWindScale = 0.05f;
    /// How far up the roof probe reaches, m.
    float shelterProbeHeight = 12.0f;

    // -- disturbance ------------------------------------------------------
    // A body moving through a plume drags the air with it: walking through
    // smoke should part it, driving through should leave a wake. Modelled as a
    // radial push around each mover, applied to particles inside its reach.
    bool disturb = true;
    /// Radial shove at the mover's surface, m/s of velocity change per second.
    float disturbStrength = 14.0f;
    /// Extra radius beyond the mover's own bounding sphere that gets pushed.
    float disturbReach = 1.6f;
    /// Fraction of the mover's speed below which it does not disturb at all --
    /// a man standing in a plume is not a gust.
    float disturbMinSpeed = 0.8f;

    // -- balloons ---------------------------------------------------------
    // Party mode, and a genuinely useful test rig: balloons make every part of
    // the pipeline legible at a glance. They are discrete, so you can COUNT
    // them; they hold their size, so the growth curve is out of the picture;
    // they are vivid, so the lighting and the wind response are obvious; and
    // they bounce, so collision is visible without a smoke cloud hiding it.
    //
    // Physically they are the opposite of smoke: a balloon does not diffuse,
    // does not thin, and is not self-shadowed by its neighbours -- it is a
    // small solid object with lift. So this mode overrides those terms rather
    // than being tuned into them.
    bool balloons = false;
    float balloonRise = 2.4f;  ///< steady climb, m/s (drag pulls toward it)
    float balloonSway = 0.55f; ///< lateral bob amplitude, m/s
    float balloonSize = 0.42f; ///< radius, m — held for the whole life
    /// 0 = every balloon takes the plume's tint; 1 = the full hue wheel.
    float balloonHueSpread = 1.0f;
    /// Replaces the smoke opacity entirely in balloon mode: a balloon reads as
    /// a solid, so its alpha cannot come off the smoke thickness dial (~0.35).
    float balloonOpacity = 0.95f;
    /// Strength of the sun highlight spot -- the single strongest cue that
    /// these are glossy balloons and not coloured blobs.
    float balloonSpecular = 0.85f;
    /// Highlight tightness (the specular exponent). Higher = a tighter hot spot.
    float balloonShininess = 28.0f;
    /// How much the silhouette edge darkens (0 = flat disc). This is what makes
    /// the sprite read as a SPHERE with a terminator instead of a sticker.
    float balloonLimbDarken = 0.45f;
    /// Sky-coloured rim on the side away from the sun, picking the silhouette
    /// out against dark backgrounds.
    float balloonRimLight = 0.25f;

    // -- fire -------------------------------------------------------------
    // A second, short-lived particle layer at the base of the plume: flames.
    // Drawn ADDITIVE with the engine's flame billboard (CloudletFire) and a
    // temperature colour ramp, white-hot at birth cooling through yellow and
    // orange to a dull red as they die. The smoke above them is unchanged; a
    // burning wreck is "fire at the bottom, smoke rising off it", and this is
    // that. Fire also raises the smoke's buoyancy at the source (hot air).
    bool fire = false;
    float fireRate = 60.0f;       ///< flames per second
    float fireLifetime = 0.9f;    ///< seconds; flames are brief
    float fireStartRadius = 0.6f; ///< m
    float fireEndRadius = 1.4f;   ///< m — a flame grows a little then dies
    float fireRiseSpeed = 3.0f;   ///< upward speed at birth, m/s
    float fireIntensity = 1.6f;   ///< brightness multiplier on the ramp (HDR headroom)
    /// Extra buoyancy the SMOKE gets while fire is on, m/s^2 — hot air rises
    /// harder off a fire than off a smoke grenade.
    float fireSmokeLift = 2.0f;
    /// Fraction of dying flames that become a smoke particle. With fire on the
    /// nozzle stops emitting and THIS is the only smoke source: the plume is
    /// literally what the fire produces. 1 = every flame smokes (a heavy oil
    /// fire); 0.2 = a clean hot burn with only wisps.
    float fireSmokeYield = 0.6f;
    /// Vertical stretch of a flame sprite at full rise speed. 0 = round puffs
    /// (the first pass, which read as cotton wool); ~1.6 = tongues.
    float fireStretch = 1.6f;
    /// A CONTINUOUS fire column instead of independent puffs. Flames are born
    /// throughout a column of this height above the emitter (not only at the
    /// base), pulled back toward the column axis so they do not scatter, and
    /// stay lit for the whole rise. 0 = the puff behaviour; 6-12 = a roaring
    /// column that reads as one body of flame with tongues on the outside.
    float fireColumnHeight = 0.0f;
    /// How hard flames are pulled toward the column axis (1/s). Only with
    /// fireColumnHeight > 0. Higher = a tighter, more coherent column.
    float fireColumnCohesion = 3.0f;
    /// Wind response of the FLAMES: a fire leans downwind. 0 = rigid, 1 = the
    /// tips fully follow the air. Real flames lean noticeably in a breeze.
    float fireWindLean = 0.6f;
    /// The fire lights its surroundings: a point light at the flame centroid,
    /// warm, driven by the same flicker as the flames so the ground and walls
    /// around it breathe with the fire. 0 disables it. Point lights in this
    /// renderer do not cast shadow maps, so "flickering shadows" is what a
    /// flickering light DOES to a scene -- near surfaces pulse, far ones do
    /// not, and anything between fire and wall reads dark against it.
    float fireLightIntensity = 1.0f;
    float fireLightRadius = 10.0f; ///< reach, m (attenuation start)
    /// Radiance multiplier so the light competes with DAYLIGHT. The renderer
    /// is HDR and the sun is far above the legacy LDR unit; at 1.0 the fire's
    /// glow vanished by day. 6 reads at noon and the tonemapper's night
    /// exposure keeps it from blowing out after dark. Eases to 1x at night
    /// (scaled by NightEffect in UpdateFireLight); this is the DAYTIME value.
    float fireLightHdrScale = 2.5f;

    // -- budget -----------------------------------------------------------
    //
    // Has to cover rate x particleLifetime, or the cap throttles emission and
    // the rate slider stops meaning anything. The stock 35/s over 14 s wants
    // ~490, so the old 400 was already clipping its own defaults -- the plume
    // never reached the density the other defaults were asking for. 700 leaves
    // headroom for the +-20% lifetime jitter on top.
    //
    // The panel prints the arithmetic live and turns it orange when the cap
    // bites, so this being wrong again is visible rather than silent.
    int maxParticles = 700;
    int maxFlames = 120;
    /// Seconds between real sphere sweeps for a given particle. Between sweeps
    /// the cached contact plane still blocks it, so this trades exactness for
    /// cost rather than trading away the collision itself.
    float sweepInterval = 0.10f;
};

/// One live particle. Deliberately a flat struct in a flat vector: the whole
/// point of this system over the legacy one is that a plume is an array walk.
struct SmokeParticle
{
    Vector3 position{VZero};
    Vector3 velocity{VZero};
    float age = 0.0f;
    float lifetime = 1.0f;
    float exposure = 1.0f;      ///< 0 = fully sheltered, 1 = open sky (smoothed)
    float shelterTimer = 0.0f;  ///< seconds until the next roof probe
    float sweepTimer = 0.0f;    ///< seconds until the next sphere sweep
    Vector3 planeNormal{VZero}; ///< cached contact plane from the last sweep
    float planeOffset = 0.0f;   ///< plane is { p : dot(n, p) >= planeOffset }
    float planePadding = 0.0f;  ///< padding when this contact was acquired, not the growing puff radius
    bool planeValid = false;
    float sizeJitter = 1.0f;
    float alphaJitter = 1.0f;
    float hue = 0.0f;       ///< balloon colour, 0..1 around the wheel
    float swayPhase = 0.0f; ///< balloon bob phase, so they do not move as one
    float groundY = 0.0f;   ///< floor height under the particle, from the last clamp
    int roomId = -1;        ///< room id from BuildingInterior, -1 = outdoors / untracked
    mutable float renderRadius = -1.0f; ///< shared by sprite, shadow and volumetric collection
};

class ISmokeWorldQuery;
float SmokeParticleOpacity(const SmokeParticle& particle, const SmokeParams& params);
float SmokeParticleRenderRadius(const SmokeParticle& particle, const SmokeParams& params,
                               const ISmokeWorldQuery& query);
void BuildSmokeDrawOrder(const std::vector<SmokeParticle>& particles, const Matrix4& toView,
                         std::vector<float>& depths, std::vector<int>& order);
bool SmokeContactPlaneStillPresent(const SmokeParticle& particle, float radius,
                                   const ISmokeWorldQuery& query);

class SmokeVolume : public Entity
{
    typedef Entity base;

  public:
    /// `shape` is the smoke billboard (CloudletBasic); `fireShape` the flame
    /// billboard (CloudletFire), may be null in which case fire is silently off.
    SmokeVolume(LODShapeWithShadow* shape, LODShapeWithShadow* fireShape, const SmokeParams& params);
    ~SmokeVolume() override;

    // -- control ----------------------------------------------------------
    const SmokeParams& Params() const { return _params; }
    /// Live edit. Existing particles keep their assigned lifetime but pick up
    /// every other change on their next tick, which is what makes the dev panel
    /// sliders useful rather than only applying to new smoke.
    void SetParams(const SmokeParams& params)
    {
        _params = params;
        for (SmokeParticle& particle : _particles)
            particle.renderRadius = -1.0f;
    }

    void SetEmitterPosition(Vector3Par position);
    Vector3 EmitterPosition() const { return _emitterPosition; }

    /// Stop emitting; existing particles live out their lifetimes and the
    /// volume then deletes itself. This is what "despawn" should mean — a plume
    /// that vanishes instantly is a much worse artefact than one that thins out.
    void Extinguish() { _emitting = false; }
    bool IsEmitting() const { return _emitting; }

    /// Emit for a fixed time then stop. Negative means forever.
    void SetDuration(float seconds) { _duration = seconds; }

    std::size_t ParticleCount() const { return _particles.size(); }
    std::size_t FlameCount() const { return _flames.size(); }

    /// Append this volume's particles as ground-shadow blobs. Density is the
    /// particle's current opacity (fade included) times the plume's shadow
    /// weight; height above ground is what the shadow pass slides along the
    /// sun ray. Forward-declared struct to keep the renderer type out of here.
    template <typename Blob>
    void AppendShadowBlobs(std::vector<Blob>& out, bool forShadow = true) const
    {
        for (const SmokeParticle& p : _particles)
        {
            const float density = SmokeParticleOpacity(p, _params) * (forShadow ? _params.groundShadow : 1.0f);
            if (density <= 0.004f)
            {
                continue;
            }
            const float radius = RenderRadius(p);
            // The volume injector has a 5 cm minimum radius. Do not inflate a
            // clipped-away puff back through its contact surface there.
            if (radius < 0.05f)
                continue;
            Blob b{};
            b.x = p.position.X();
            b.y = p.position.Y();
            b.z = p.position.Z();
            b.radius = radius;
            b.density = density;
            b.heightAboveGround = std::max(p.position.Y() - p.groundY, 0.0f);
            out.push_back(b);
        }
    }

    /// Microseconds spent in the last Simulate / Draw. Read by the dev panel's
    /// benchmark section; costs one clock read per call.
    double LastSimulateMicroseconds() const { return _lastSimulateUs; }
    double LastDrawMicroseconds() const { return _lastDrawUs; }

    // -- Entity -----------------------------------------------------------
    void Simulate(float deltaT, SimulationImportance prec) override;
    void Draw(int level, ClipFlags clipFlags, const FrameBase& frame) override;

    bool IsAnimated(int level) const override { return true; }
    bool IsAnimatedShadow(int level) const override { return false; }
    void Animate(int level) override {}
    void Deanimate(int level) override {}

    /// NOT the legacy 10. Scene::CloudletForDrawing drops the whole object when
    /// its centre is closer than Near() * this coefficient. For a legacy
    /// cloudlet that is one puff, so it only culls a puff you are standing in.
    /// For a volume it is the ENTIRE PLUME: stand inside the smoke, or beside it
    /// with its centroid a little behind you, and every particle vanishes at
    /// once — which is exactly the "not visible from every angle indoors"
    /// symptom. A coefficient below 1 lets the centre pass the near plane; each
    /// particle still does its own per-billboard near test in Draw().
    float CloudletClippingCoef() const override { return 0.0f; }
    SimulationImportance WorstImportance() const override;
    SimulationImportance BestImportance() const override;

    // Smoke must not block line of fire or line of sight as a solid; gameplay
    // visibility through smoke is a separate, coarser question that this system
    // deliberately does not answer yet (roadmap FX-130 keeps that server-side).
    bool OcclusionFire() const override { return false; }
    bool OcclusionView() const override { return false; }
    bool MustBeSaved() const override { return false; }

    LSError Serialize(ParamArchive& ar) override { return LSOK; }

    void Sound(bool inside, float deltaT) override {}
    void UnloadSound() override {}

    USE_FAST_ALLOCATOR;

  private:
    void Emit(float deltaT);
    /// The one place a smoke particle is born. The nozzle calls it, and so does
    /// a dying flame -- which is how fire produces its own smoke.
    void SpawnSmoke(Vector3Par position, Vector3Par velocity, float sizeScale);
    void EmitFire(float deltaT);
    void SimulateFire(float deltaT);
    void DrawFire(int level);
    void Integrate(SmokeParticle& particle, float deltaT);
    void Collide(SmokeParticle& particle, Vector3Par previousPosition, float deltaT);
    /// Gather the movers (scene camera, the entity the camera is on) and push
    /// particles radially out of their way. Velocity is estimated from last
    /// frame's position; one frame of latency is invisible at plume scales.
    void ApplyDisturbance(float deltaT);
    float RenderRadius(const SmokeParticle& particle) const;
    void UpdateBounds();

    // Disturbance scratch: previous positions for the velocity estimate, and
    // whether they are valid yet. Two tracked movers: the scene camera (free
    // fly included) and the entity the camera is on (the walking player, the
    // vehicle being driven).
    Vector3 _prevCamPos{VZero};
    Vector3 _prevCamOnPos{VZero};
    bool _prevCamValid = false;
    bool _prevCamOnValid = false;

    struct Disturber
    {
        Vector3 position{VZero};
        float radius = 1.0f;
    };

    /// Collects up to four movers near this plume. Out-param keeps it
    /// allocation-free.
    int CollectDisturbers(Disturber* out, int max) const;

    SmokeParams _params{};
    std::vector<SmokeParticle> _particles;
    /// Per-particle transmittance toward the sun, rebuilt each Draw. Scratch,
    /// kept as a member so the allocation is not paid every frame.
    std::vector<float> _shadowScratch;
    SmokeDensityGrid _hash;

    /// Flame particles. Same struct, far fewer fields used: no collision, no
    /// shelter, they live under a second. Kept separate so the smoke loop's
    /// cost is unchanged when fire is off.
    std::vector<SmokeParticle> _flames;
    Ref<LODShapeWithShadow> _fireShape;
    float _fireAccumulator = 0.0f;
    /// The fire's point light. Created on the first tick with fire on, freed
    /// when the flames are gone; scene-owned once added.
    Ref<LightPoint> _fireLight;
    float _fireLightPhase = 0.0f;
    void UpdateFireLight(float deltaT);
    std::vector<int> _drawOrder;
    std::vector<float> _drawDepth;

    Vector3 _emitterPosition{VZero};
    float _emitAccumulator = 0.0f;
    float _duration = -1.0f;
    bool _emitting = true;

    double _lastSimulateUs = 0.0;
    double _lastDrawUs = 0.0;

    /// Rate limit for the draw diagnostics. Without it a plume logs once per
    /// particle per frame and the log becomes the performance problem.
    Foundation::Time _nextDiagTime;
    /// Rate limit for the room/portal dev overlay, separate from the diag log
    /// so enabling one does not disturb the other's cadence.
    Foundation::Time _nextOverlayTime;
    long long _diagSweeps = 0; ///< sweeps since the last diag line
    long long _diagHits = 0;   ///< of which hit geometry
    int _diagHitLog = 0;       ///< first-N hit dump, once per volume

    unsigned _rand = 0x1234567u;
    float RandUnit();      ///< [0, 1)
    float RandSymmetric(); ///< [-1, 1)
};

} // namespace Poseidon
