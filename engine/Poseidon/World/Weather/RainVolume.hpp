#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>

#include <vector>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// Particle rain (roadmap WTR-245, "precipitation: occluded rain").
//
// The legacy rain is a full-screen scrolling TextureRain overlay (Scene::DrawRain):
// cheap, but it rains at the same strength indoors, under bridges and in the
// driver's seat. The world-space replacement is the DEFAULT: a camera-following
// pool of wind-advected drop streaks that die on the ground with a splash, on
// any shell, under every overhang, and -- through the SAME room query and
// portals the smoke containment built -- enter buildings only where smoke can
// leave them. One query boundary, two consumers, exactly as the
// rooms-and-portals handover asked.
//
// Purely visual and client-local: nothing here is simulated, replicated or
// serialized. The dev panel's Weather tab owns the toggles, including a master
// "Rain ON" switch that forces the weather overcast with it.
// ---------------------------------------------------------------------------

enum RainMode
{
	RainOff,       ///< no rain drawn at all (even the legacy overlay is muted)
	RainLegacy,    ///< the legacy screen-space overlay only
	RainParticle,  ///< the new world-space drops only
	RainBoth,      ///< both at once, for A/B comparison side by side
};

enum RainOcclusion
{
	RainOcclusionNone,      ///< rain falls through roofs (diagnostic)
	RainOcclusionSheltered, ///< IsSheltered roof probe per drop, staggered -- the cheap fallback
	///< Rooms is the voxel sweep PLUS a staggered sky probe: the sweep only knows
	///< buildings with usable shell data; the probe catches every other surface
	///< overhead (no-data houses, bridge decks, rock overhangs, tree canopies).
	RainOcclusionRooms,
};

struct RainParams
{
    bool snowflakes = false;
	// -- field ------------------------------------------------------------
	// Defaults are what a mid-range GPU holds at frame time WITH occlusion on.
	// Density is the first thing to raise and the last thing you miss; the
	// previous defaults spent thousands of world queries a second for rain
	// nobody was counting.
	int targetDrops = 1800;      ///< steady-state drop count the spawner aims for
	int maxDrops = 4000;         ///< hard cap of the pool
	float areaRadius = 20.0f;    ///< radius of the spawn disc around the camera, m
	float spawnHeight = 18.0f;   ///< drops are born this far above the camera, m
	float densityOverride = -1.0f; ///< negative follows weather; 0..1 is a dev/benchmark override

	// -- motion -----------------------------------------------------------
	float fallSpeed = 9.0f;      ///< terminal velocity, m/s
	float speedJitter = 0.25f;   ///< +- fraction of per-drop speed variation
	float windResponse = 0.85f;  ///< 0 = falls straight, 1 = fully advected by the wind
	float windAcceleration = 5.0f; ///< horizontal response rate, 1/s; gusts alter drops already in flight

	// -- look -------------------------------------------------------------
	float streakLength = 1.1f;   ///< vertical size of a drop sprite, m
	float streakWidth = 0.045f;  ///< horizontal size, m
	///< Daylight-legible defaults: a half-alpha LIGHT sprite vanishes against a
	///< bright sky -- an owner smoke could not see outdoor rain at all. Falling
	///< rain reads as slightly DARKER than the background, so the tint sits
	///< below white and the opacity runs high; the Weather tab can restyle it.
	float opacity = 1.0f;
	float red = 0.45f;
	float green = 0.52f;
	float blue = 0.62f;

	// -- ground interaction -----------------------------------------------
	bool splashes = true;        ///< brief expanding sprite where a drop lands
	float splashSize = 0.35f;    ///< splash radius at end of life, m
	float splashLifetime = 0.30f;///< s

	// -- occlusion --------------------------------------------------------
	RainOcclusion occlusion = RainOcclusionRooms;
	/// Probe cadence for the Sheltered mode and for the shelter probe that
	/// accompanies Rooms mode (jittered per drop, so probes do not fire in
	/// lockstep bursts).
	float occlusionCadence = 0.40f;

	// -- occlusion tolerances ---------------------------------------------
	// Every number below buys tightness with query cost or with rain that
	// stops short of a surface. The defaults are the strict end of what still
	// looks right, because the owner's call on the trade is explicit: rain
	// stopping a little early outdoors beats rain falling inside a house.

	/// The shelter probe LOOKS AHEAD along the drop's own path by this multiple
	/// of the distance it will cover before its next probe, instead of asking
	/// "is a roof already above me". That is the whole difference between a
	/// drop dying AT the roofline and a drop dying a probe-interval below it --
	/// at 9 m/s and a 0.4 s cadence, 3.6 m of rain inside the room. 1 probes
	/// exactly the next interval; above 1 is slack for a gust or a frame spike.
	float probeLookahead = 1.35f;
	/// Kill a drop at spawn if anything at all is overhead within this many
	/// metres. Look-ahead cannot see a canopy a drop was BORN under, and this
	/// is the one probe that catches it. Costs one ray per spawned drop.
	bool probeAtSpawn = true;
	float spawnProbeHeight = 30.0f;
	/// Probe height for the RainOcclusionSheltered fallback mode's upward ray.
	float shelterProbeHeight = 8.0f;

	/// No opening EVER forgives a shell crossing: rain simply cannot get in.
	/// The blunt instrument, for when a particular building's portals are wrong
	/// and no radius is small enough.
	bool strictIndoor = false;
	/// Fraction of a portal's inferred radius a crossing has to fall within to
	/// count as coming in through the opening. Smaller = stricter.
	float portalRadiusScale = 0.50f;
	/// Windows and breaches come from the heuristic voxel scan and are off by
	/// default -- see InteriorPortalFilter.
	bool portalWindows = false;
	bool portalBreaches = false;

	// -- sea surface -------------------------------------------------------
	/// Gain on the wgpu water interaction pass's rain-strike term (WaterWgpu
	/// packs it into the interaction UBO). The ripples cost nothing extra --
	/// the strike is a hashed branch inside a compute pass that already runs
	/// every frame over the whole field -- so this is a look knob, not a
	/// budget one. 0 turns sea ripples off.
	float seaRippleGain = 1.0f;
};

struct RainStats
{
	int liveDrops = 0;        ///< drops currently falling
	int liveSplashes = 0;
	int volumeCulls = 0;     ///< camera-exited liquid visuals; never ground/shelter impacts
	int populationCulls = 0; ///< liquid visuals retired when weather/target decreases
	int populationLimit = 0; ///< current density-scaled liquid visual cap
	int indoorKills = 0;      ///< drops that entered a room since ResetStats
	int roofKills = 0;        ///< drops that hit the shell (roofline/wall) since ResetStats
	int probeKills = 0;       ///< drops killed by the Rooms-mode sky probe since ResetStats
	int groundHits = 0;       ///< drops that landed on terrain since ResetStats
	int spawned = 0;          ///< drops spawned since ResetStats
	int spawnKills = 0;       ///< drops that were born under cover and never fell
	int nearbyBuildings = 0; ///< valid cached interiors considered this frame
	int occlusionTests = 0;  ///< cheap voxel segment tests performed this frame
	int drawnDrops = 0;      ///< visible drops submitted this frame
	int streakDecals = 0;    ///< decal submissions (several short beads form one wind-aligned streak)
	double spawnMicroseconds = 0.0;
	double occlusionMicroseconds = 0.0;
	double simulateMicroseconds = 0.0;
	double drawMicroseconds = 0.0;
	double lastUpdateUs = 0.0;
};

/// Pure motion helpers, kept public so the wind contract has a focused unit
/// test without constructing a world or renderer.
Vector3 RainTargetVelocity(float fallSpeed, float windX, float windZ, float windResponse);
Vector3 RainRelaxVelocity(Vector3Par current, Vector3Par target, float responsePerSecond, float deltaT);
/// Far end of the shelter probe: the drop's own path over `interval` seconds --
/// the time until its next probe -- scaled by `lookahead`. This is the whole
/// reason rain no longer falls a probe-interval into a room before dying, so it
/// is a named function with a test rather than an expression in the loop.
/// `lookahead` below 1 would leave a gap the probe never covers and is clamped.
Vector3 RainProbeTarget(Vector3Par position, Vector3Par velocity, float interval, float lookahead);
float PrecipitationSpriteLength(bool snowflake, float projectedMotion, float projectedWidth);
float SnowflakeAlpha(float x, float y);
bool SnowParticleInViewVolume(Vector3Par particle, Vector3Par camera, float radius, float height);

class RainSystem
{
public:
	RainSystem();

	RainMode Mode() const { return _mode; }
	void SetMode(RainMode mode) { _mode = mode; }

	const RainParams &Params() const { return _params; }
	void SetParams(const RainParams &params) { _params = params; }

	/// The density the particle layer is ACTUALLY raining at: the dev/benchmark
	/// override when one is set, the live weather otherwise, and zero when the
	/// particle layer is not drawing at all.
	///
	/// This exists because the two rain layers and the sea disagreed. The panel's
	/// presets pin `densityOverride` to 1 and produce a downpour, while
	/// Landscape's own rain density -- which the ocean's ripple term reads -- is
	/// a slow random walk that tops out near 0.4 under a 0.95 overcast and takes
	/// the better part of a minute to get anywhere. So the sea was being rained
	/// on at a tenth of the strength of the rain you could see falling into it.
	float EffectiveDensity() const;

	/// Drive Landscape's own rain density to full, once a second, for as long as
	/// this is set. The dev panel's master switch and POSEIDON_TEST_RAIN both use
	/// it: consumers beyond the two render layers (the sea's ripples, wet-surface
	/// effects, visibility) read GetRainDensity, not GRain, and an overcast alone
	/// only random-walks it upward over tens of seconds.
	bool ForceWeatherRain() const { return _forceWeatherRain; }
	void SetForceWeatherRain(bool force) { _forceWeatherRain = force; }

	/// Local, unreplicated override for the LEGACY overlay's density, so the old
	/// rain can be looked at on a clear day without dragging the weather about.
	/// Negative = follow Landscape::GetRainDensity as usual.
	float LegacyDensityOverride() const { return _legacyDensityOverride; }
	void SetLegacyDensityOverride(float density) { _legacyDensityOverride = density; }

	/// Tick + draw the particle layer. Called from the render side
	/// (Scene::ObjectsDrawn) once per frame when the mode includes it; computes
	/// its own deltaT from Glob.time. Safe to call with no world/scene: no-ops.
	void UpdateAndDraw();

	const RainStats &Stats() const { return _stats; }
	void ResetStats() { _stats = RainStats{}; }
	void ClearParticles()
	{
		_drops.clear();
		_splashes.clear();
		_spawnAccumulator = 0.0f;
		_snowTexture = nullptr;
        _snowVolumeReady = false;
        _snowTarget = 0;
	}

private:
	void Spawn(float deltaT);
	void Simulate(float deltaT);
	void Draw();
	/// Resolve every building overlapping the camera-following rain disc once
	/// per frame (Rooms mode). The previous design cached only the building
	/// containing the camera, so rain crossed every nearby roof while outdoors.
	/// The per-drop path is then a cheap broad phase plus cached voxel sweep.
	void RefreshOcclusion();

	// The shipping default is particle rain: it follows the wind, dies on
	// roofs, walls, floors, the sea's skin and under every overhang, and
	// enters through doors and windows where the legacy overlay could only
	// rain indoors. RainLegacy remains one radio away for A/B comparison.
	RainMode _mode = RainParticle;
	RainParams _params{};
	Ref<Texture> _snowTexture;
	RainStats _stats{};
	float _legacyDensityOverride = -1.0f;

	struct Drop
	{
		Vector3 position{VZero};
		float speed = 9.0f;       ///< this drop's terminal velocity, m/s
        float phase = 0.0f;
        float size = 1.0f;
		float velocityX = 0.0f;   ///< retained horizontal velocity, responds to gust changes
		float velocityZ = 0.0f;
		float occlusionTimer = 0.0f; ///< s until its next occlusion probe (Sheltered mode)
		bool indoor = false;      ///< last probe said "under cover" -- not drawn
		///< True only after the drop passed through a portal opening. Room voxels
		///< alone must not grant this: gappy fire LODs can flood room labels
		///< outside the shell, and an outdoor drop trusting them would skip every
		///< kill and fall straight through the roof.
		bool legalIndoor = false;
	};
	struct Splash
	{
		Vector3 position{VZero};
		float age = 0.0f;
	};

	std::vector<Drop> _drops;
	std::vector<Splash> _splashes;
	Foundation::Time _lastUpdate;
	float _spawnAccumulator = 0.0f;
    bool _snowVolumeReady = false;
    int _snowTarget = 0;
    Vector3 _snowVolumeCentre{VZero};
	Foundation::Time _nextDiagTime; ///< once-per-second stats gate for POSEIDON_TEST_RAIN
	bool _forceWeatherRain = false; ///< POSEIDON_TEST_RAIN: drive Landscape rain density to full
	unsigned _rand = 0x9E3779B9u; ///< private stream; do not touch GRandGen
	float RandUnit();
	float RandSymmetric();

	// Rooms-mode broad phase, rebuilt from the Landscape object grid once per
	// frame. A rain disc can overlap several buildings even when the camera is
	// outdoors; caching only the building containing the camera missed all of
	// those roofs. Raw pointers are safe for this one-frame list.
	struct Occluder
	{
		class Object const* object = nullptr;
		class BuildingInterior const* interior = nullptr;
		Matrix4 inv = MIdentity;
		Vector3 centre{VZero};
		float radius = 0.0f;
	};
	std::vector<Occluder> _occluders;
};

/// The live rain authority, next to GWind.
extern RainSystem GRain;
extern RainSystem GSnowFlakes;

} // namespace Poseidon
