#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>

#include <vector>

namespace Poseidon
{

// ---------------------------------------------------------------------------
// Particle rain (roadmap WTR-245, "precipitation: occluded rain").
//
// The legacy rain is a full-screen scrolling TextureRain overlay (Scene::DrawRain):
// cheap, but it rains at the same strength indoors, under bridges and in the
// driver's seat. This is the world-space replacement: a camera-following pool of
// drop streaks that fall through the wind, die on the ground with an optional
// splash, and -- the point of the feature -- are killed by the SAME room query
// the smoke containment built (ISmokeWorldQuery::IndoorRoomAt). One query
// boundary, two consumers, exactly as the rooms-and-portals handover asked.
//
// Purely visual and client-local: nothing here is simulated, replicated or
// serialized. The dev panel's Weather tab owns the only toggles; the default
// mode is Legacy, so a world that never opens the panel rains exactly as before.
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
	// -- field ------------------------------------------------------------
	// Defaults are what a mid-range GPU holds at frame time WITH occlusion on.
	// Density is the first thing to raise and the last thing you miss; the
	// previous defaults spent thousands of world queries a second for rain
	// nobody was counting.
	int targetDrops = 700;       ///< steady-state drop count the spawner aims for
	int maxDrops = 1500;         ///< hard cap of the pool
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
	float opacity = 0.5f;
	float red = 0.75f;
	float green = 0.80f;
	float blue = 0.90f;

	// -- ground interaction -----------------------------------------------
	bool splashes = true;        ///< brief expanding sprite where a drop lands
	float splashSize = 0.35f;    ///< splash radius at end of life, m
	float splashLifetime = 0.30f;///< s

	// -- occlusion --------------------------------------------------------
	RainOcclusion occlusion = RainOcclusionRooms;
	/// Probe cadence for the Sheltered mode and for the sky probe that
	/// accompanies Rooms mode (jittered per drop, so probes do not fire in
	/// lockstep bursts).
	float occlusionCadence = 0.40f;
};

struct RainStats
{
	int liveDrops = 0;        ///< drops currently falling
	int liveSplashes = 0;
	int indoorKills = 0;      ///< drops that entered a room since ResetStats
	int roofKills = 0;        ///< drops that hit the shell (roofline/wall) since ResetStats
	int probeKills = 0;       ///< drops killed by the Rooms-mode sky probe since ResetStats
	int groundHits = 0;       ///< drops that landed on terrain since ResetStats
	int spawned = 0;          ///< drops spawned since ResetStats
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
/// How many overlapping sprites render one drop's streak: 1 vertical, more as
/// the lean from wind steepens. Pure so the look has a focused test.
int RainTrailBeadCount(float horizontalSpeed, float totalSpeed);

class RainSystem
{
public:
	RainSystem();

	RainMode Mode() const { return _mode; }
	void SetMode(RainMode mode) { _mode = mode; }

	const RainParams &Params() const { return _params; }
	void SetParams(const RainParams &params) { _params = params; }

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

	// Preserve the pre-feature shipping behaviour until particle rain is made a
	// real graphics option. RainOff used to disable all rain on every fresh run.
	RainMode _mode = RainLegacy;
	RainParams _params{};
	RainStats _stats{};
	float _legacyDensityOverride = -1.0f;

	struct Drop
	{
		Vector3 position{VZero};
		float speed = 9.0f;       ///< this drop's terminal velocity, m/s
		float velocityX = 0.0f;   ///< retained horizontal velocity, responds to gust changes
		float velocityZ = 0.0f;
		float occlusionTimer = 0.0f; ///< s until its next occlusion probe (Sheltered mode)
		bool indoor = false;      ///< last probe said "under cover" -- not drawn
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
	Foundation::Time _nextDiagTime; ///< once-per-second stats gate for POSEIDON_TEST_RAIN
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

} // namespace Poseidon
