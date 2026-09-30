#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Evaluator/express.hpp>



namespace Poseidon
{
class Smoke: public Vehicle
{
	typedef Vehicle base;

	protected:
	float _animation;
	float _animationSpeed;

	float _timeToLive;

	bool _firstLoop;
	bool _invisible;
	float _alpha;
	float _fadeValue;
	float _fadeIn,_fadeInTime,_fadeInInv;
	float _fadeOut,_fadeOutTime,_fadeOutInv;

	public:
	Smoke( LODShapeWithShadow *shape, VehicleNonAIType *type, float duration, float loopedDuration=0 );
	~Smoke() override;

	void SetFadeIn( float fadeIn )
	{
		_fadeInInv=( fadeIn>0 ? 1/fadeIn :0 );
		_fadeIn=_fadeInTime=fadeIn;
	}
	void SetFadeOut( float fadeOut )
	{
		_fadeOutInv=( fadeOut>0 ? 1/fadeOut :0 );
		_fadeOut=_fadeOutTime=fadeOut;
	}
	void SetFades( float fadeIn, float fadeOut )
	{
		SetFadeIn(fadeIn);
		SetFadeOut(fadeOut);
	}
	void SetAlpha( float alpha ) {_alpha=alpha;}
	void SetSpeed( Vector3Par speed ){_speed=speed;}
	void SetTimeToLive( float time ){_timeToLive=time;}
	void Simulate( float deltaT, SimulationImportance prec ) override;

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override; // alpha animation
	void Deanimate( int level ) override;

	float Animation() const {return _animation;}
	void Sound( bool inside, float deltaT ) override{}
	void UnloadSound() override{}
	bool Invisible() const override {return _invisible;}

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR;
};

struct CloudletTItem
{
	float maxT;
	Color color;

	LSError Serialize(ParamArchive &ar);
};

class CloudletTTable : public RefCount, public AutoArray<CloudletTItem>
{
public:
	typedef AutoArray<CloudletTItem> base;

	int Add(float maxT, Color color);
	Color GetColor(float t) const;
};

class Cloudlet: public Smoke
{
	typedef Smoke base;

	float _size; // size of cloudlet
	float _growSize; // current size (0..1)
	float _growUp,_growUpTime,_growUpInv;

	float _accY; // gravity acceleration
	float _minYSpeed,_maxYSpeed; // max. descent rate

	float _xSpeed0,_zSpeed0;
	float _xFriction,_zFriction; // deceleration until speed0

	float _t, _dt;
	Ref<CloudletTTable> _cloudletTTable;
	Color _cloudletColor;

	// SMK-030 containment state. `_roomId` is -1 for outdoors AND for "not asked yet";
	// `_roomChecked` separates the two, so the one spawn query happens exactly once.
	int _roomId = -1;
	bool _roomChecked = false;
	float _nextOutsideCheck = 0.0f;    //!< SMK-031: next time this outdoor cloudlet may ask
	float _nextClearanceCheck = 0.0f;  //!< SMK-032: next time the drawn size may be re-measured
	float _clearance = -1.0f;          //!< SMK-032: last measured room clearance, -1 = none yet

	// SMK-035 shading state, both snapshotted at spawn from the source that dropped this
	// particle. `_shadeOrigin` is the plume's ORIGIN, not the particle's own spawn point --
	// the offset between them is what gives the pseudo-normal its lateral direction.
	// `_shadeAge` / `_shadeLife` are the optical-depth yardstick, and they are AGE, not
	// metres: a plume's root is where the smoke is thickest and its fringe is where it has
	// spread out and thinned, so how far along a particle is in its life says how much smoke
	// is around it. Metres cannot say that -- a grenade's plume is 6 m across and an
	// artillery column is 60, and the class does not know which it belongs to.
	Vector3 _shadeOrigin = VZero;
	float _shadeAge = 0.0f;
	float _shadeLife = 1.0f;
	// SMK-036 ground-shadow state. The ground is sampled ONCE, at the same first step the
	// room query happens on: a cloudlet lives seconds and travels metres, and the terrain
	// under it does not move.
	float _drawScale = 1.0f;
	float _groundY = 0.0f;
	//! SMK-039: the alpha the source asked for, before the dispersal thinning scales it.
	float _alpha0 = -1.0f;
	bool _groundChecked = false;
	bool _smokeShadow = true;

	public:
	Cloudlet( LODShapeWithShadow *shape, float duration, float loopedDuration=0 );

	void SetGrowUp( float growUp, float size )
	{
		_size=size;
		_growUpInv=( growUp>0 ? 1/growUp :0 );
		_growUp=_growUpTime=growUp;
	}

	void SetClimbRate( float accY, float minY, float maxY )
	{
		_accY=accY;
		_minYSpeed=minY;
		_maxYSpeed=maxY;
	}
	void SetSideSpeed( float xS0, float zS0, float xF, float zF )
	{
		_xSpeed0=xS0,_zSpeed0=zS0;
		_xFriction=xF,_zFriction=zF; // deceleration until speed0
	}
	void SetColor(ColorVal color) {_cloudletColor = color;}
	void SetTemperature(float t, float dt, CloudletTTable *table);
	//! SMK-036: this particle as a ground-shadow blob; false if it is too faint to bother.
	bool SampleShadow(struct CloudletShadowSample &out) const;
	void SetSmokeShadow(bool enabled) {_smokeShadow = enabled;}
	//! SMK-035: where the plume this particle belongs to starts, and how long it lives.
	void SetShadeOrigin(Vector3Par origin, float life) {_shadeOrigin = origin; _shadeLife = life; _shadeAge = 0;}

	float CloudletClippingCoef() const override;
	SimulationImportance WorstImportance() const override;
	SimulationImportance BestImportance() const override;
	
	void Simulate( float deltaT, SimulationImportance prec ) override;
	void Draw( int level, ClipFlags clipFlags, const FrameBase &frame  ) override;
	#if ALPHA_SPLIT
	void DrawAlpha( int level, ClipFlags clipFlags );
	#endif

	// cloudlet does not occlude neither view nor fire
	bool OcclusionFire() const override {return false;}
	bool OcclusionView() const override {return false;}

	bool MustBeSaved() const override {return false;}

	LSError Serialize(ParamArchive &ar) override {return LSOK;}

	// basic element from which smoke trails are built
	USE_FAST_ALLOCATOR;
};

// SMK-020: the legacy cloudlet smoke (every grenade, shell, crater and exhaust in the
// game) is authored in CfgCloudlets for 2001's particle budget: a grenade's puff lives
// 0.2-6 s and rises 0-6 m/s. The owner asked for smoke that lasts longer and travels
// further. These scale what CfgCloudlets says at the moment a cloudlet class is loaded:
// lifetime multiplies the particle life and the source's emission time, rise multiplies
// the vertical speed (wind drift is baked in at spawn, so a longer life already drifts
// further). Seeded from POSEIDON_SMOKE_LIFETIME_SCALE / POSEIDON_SMOKE_RISE_SCALE; the
// Smoke tab edits them live, and a class loaded after that picks the new values up --
// which for shots is the next mission start.
struct LegacySmokeScale
{
    float lifetime = 2.0f;
    float rise = 1.3f;
    // SMK-034: the LOOK. CfgCloudlets is authored for 2001's particle budget, which is why a
    // plume reads as a handful of separate blobs sailing past each other. The cure is the
    // standard one: more particles, each smaller and thinner, fading in over a longer grow-up
    // so none of them pops into existence. All four are multipliers on the authored numbers,
    // applied when a cloudlet class is loaded, so the shape of every effect is preserved.
    // POSEIDON_SMOKE_LOOK=0 restores the authored values exactly.
    bool  look = true;
    float density = 1.7f;  //!< divides `interval`: this many times as many particles
    // Was 0.85 when SMK-034 landed. The owner looked at it and said smaller puffs do NOT
    // look better, so the size is back to what CfgCloudlets authors and the density and the
    // thinner alpha do the work of making the plume read as one body.
    float size = 1.0f;     //!< multiplies `cloudletSize`
    float alpha = 0.62f;   //!< multiplies `cloudletAlpha` -- thinner, so overlapping reads as one body
    float grow = 1.6f;     //!< multiplies `cloudletGrowUp` and the fades: no pop-in, no pop-out
};
LegacySmokeScale& GLegacySmokeScale();

// SMK-030: containment for the LEGACY cloudlets -- which is every smoke the game
// actually produces (grenades, exhaust, wheel dust, burning wrecks, explosions). The
// modern SmokeVolume system carries the room/portal containment work, and nothing in
// gameplay routes through it (SmokeSystem.hpp: "Both spawn paths are dev-driven"), so a
// smoke grenade thrown in a house has always poured straight through the walls.
//
// The cost rule is the one that makes this affordable: EVERY cloudlet asks
// `IndoorRoomAt` ONCE, at its first step. Outdoors -- exhaust, dust, artillery, every
// plume in the open -- that is the only query it will ever make. Only a cloudlet that
// starts inside a room pays the per-step `IndoorContainStep`, which is a voxel walk over
// the cached per-building interior, not a geometry sweep.
struct LegacySmokeContainment
{
    bool enabled = true;      //!< POSEIDON_SMOKE_CONTAIN_LEGACY=0 turns it off
    float slide = 0.15f;      //!< velocity kept along the blocked direction (0 = stop dead)
    // SMK-031: the other direction -- an OUTDOOR plume drifting into a house. OFF by
    // default, and the reason is measured, not assumed: the check needs IndoorRoomAt at the
    // step's destination, IndoorRoomAt needs BuildingContaining, and BuildingContaining
    // caches only POSITIVE results -- so every outdoor query runs two full
    // Landscape::IsInside passes. At ~0.4 ms each that is 21-24 ms of cloudlet simulation in
    // a village, against 0.28 ms for everything else here. The gates below (the land cell
    // must hold a solid object, then once every `outsideInterval` seconds per particle) are
    // not enough, because in a village every cell holds one. Turning this on is a deliberate
    // trade; making it cheap means a negative cache in BuildingContaining, which is a change
    // to a shared query and wants its own measurement.
    bool blockOutsideIn = false; //!< POSEIDON_SMOKE_CONTAIN_OUTSIDE_IN=1 turns it on
    float outsideInterval = 0.25f;
    // SMK-032: the DRAWN footprint. A billboard beside a wall paints through it whatever
    // the particle centre does; indoors the puff is clamped to the room clearance, which is
    // what SmokeVolume::Draw already does for the modern path.
    bool clampDraw = true;      //!< POSEIDON_SMOKE_CONTAIN_DRAW=0 turns it off
    // The clearance query walks the interior voxels and is FAR from free: measured at 28 ms
    // of cloudlet simulation when every indoor particle asked every frame. Each particle
    // re-measures at this interval instead (staggered at spawn), and a puff moves centimetres
    // in that time.
    float drawInterval = 0.25f;
    // SMK-033: legacy smoke bakes the wind in at spawn. Indoors there is no wind to bake.
    bool windOutdoorsOnly = true; //!< POSEIDON_SMOKE_WIND_INDOORS=1 restores the old behaviour
    long long windQueries = 0;
    long long windBlocked = 0;
    long long spawnQueries = 0;
    long long stepQueries = 0;
    long long blocked = 0;
    long long indoor = 0;
    long long outsideChecks = 0;
    long long blockedIn = 0;
    long long drawQueries = 0;
    long long drawClamped = 0;
};
LegacySmokeContainment& GLegacySmokeContainment();

// SMK-035: cloudlets are not lit by anything. `_constantColor` is the authored
// `cloudletColor` times a temperature ramp and nothing else, so every particle in a plume
// carries the SAME colour whatever the time of day, wherever the sun is and however deep
// in the column it sits. That is the whole reason smoke reads as a flat grey decal: a
// plume has no light side, no dark side and no interior.
//
// This shades each particle on the CPU, once per simulation step, from the scene's own
// sun (so dusk smoke is warm and night smoke is dark, like everything else in the world):
//
//   n     -- a pseudo-normal. A billboard has none, so use the particle's offset from the
//            plume origin, with the vertical flattened: on the shell of a rising column
//            that points outward, which is the surface the eye actually sees.
//   wrap  -- wrap-around diffuse, (n.L + w)/(1 + w). Smoke is translucent; a hard N.L
//            would make the far side black, and it is not, it is lit through.
//   depth -- how much smoke lies between this particle and the outside. Low in the column
//            and near its axis is deep; the top and the fringe are thin. This is the
//            self-shadow term, and it is what makes a dense plume nearly black in the
//            middle and bright at the edges.
//
// A HOT particle is emissive, not lit: a fireball, a muzzle flash and a tracer make their
// own light, and shading one would be wrong. Above `emissiveT` the shading fades out, so
// every explosion and gun effect in CfgCloudlets is left exactly as authored.
//
// Cost is a dozen float operations per particle per step; there is no query and nothing is
// cached. POSEIDON_SMOKE_SHADE=0 restores the flat 2001 colour exactly.
struct LegacySmokeShading
{
    bool enabled = true;        //!< POSEIDON_SMOKE_SHADE=0 turns it off
    float ambient = 1.0f;       //!< scales the scene's ambient term
    float diffuse = 1.0f;       //!< scales the scene's directional term
    float wrap = 0.55f;         //!< translucency: 0 = hard N.L, 1 = almost flat
    float selfShadow = 0.55f;   //!< how dark the inside of a plume gets, 0..1
    float emissiveT = 700.0f;   //!< K: above this the particle glows and is not shaded
    float emissiveBand = 400.0f;//!< K: the fade width below emissiveT
    // SMK-036: the shadow the plume throws on the GROUND. The renderer already has this
    // pass -- SmokeVolume feeds it (SmokeSystem::CollectShadowBlobs) and it is why the dev
    // smoke darkens the grass under it. The game's own cloudlets were never in it, so the
    // smoke that gameplay actually produces has always floated over a fully lit world.
    //
    // MEASURED LIMIT, so nobody re-discovers it: the pass writes into the CLOUD shadow map,
    // which is 512 texels over a span sized from the draw distance -- about 7.8 m per texel
    // at a 4 km span (sky/mod.rs CLOUD_SHADOW_DIM). A smoke grenade's plume is ~6 m across,
    // i.e. under ONE texel, and the shader widens every blob to 0.6 of a texel so it lands at
    // all. So this shows up for a BIG plume (a burning wreck, an artillery column -- the sizes
    // the dev SmokeVolume was tuned at) and is invisible for a hand grenade. Turning the
    // strength up does not sharpen it, it dims the whole landscape: measured at strength 20,
    // 1406 blobs, the terrain, houses and trees all darkened together. A crisp grenade shadow
    // needs a finer, plume-local shadow region, not a bigger number here.
    bool groundShadow = true;   //!< POSEIDON_SMOKE_GROUND_SHADOW_LEGACY=0 turns it off
    float groundWeight = 1.3f;  //!< POSEIDON_SMOKE_GROUND_SHADOW_LEGACY_WEIGHT
    long long shaded = 0;       //!< particles shaded since start
    long long emissive = 0;     //!< particles left alone because they were hot
    long long blobs = 0;        //!< cloudlets handed to the ground-shadow pass last frame
};
LegacySmokeShading& GLegacySmokeShading();

// SMK-039: how a plume DIES. CfgCloudlets grows a puff to full size over `cloudletGrowUp`
// and then holds it there for the rest of its life, fading it out on a timer. Real smoke
// does the opposite: it keeps entraining air, so the puffs keep growing AND keep drifting
// apart, and the same smoke spread through more air is thinner. That is why the legacy
// plume ends as a clump of still-solid blobs that wink out, instead of dissolving.
//
// Three coupled terms, all driven by the particle's own age:
//   expand -- the puff keeps growing after grow-up, to `expand` times its size at death.
//   thin   -- the density that expansion implies. A puff `k` times wider holds the same
//             smoke over k^2 more area, so its opacity falls as k^-2*thin. This is what
//             makes the end of a plume fade out instead of shrinking away.
//   spread -- an outward drift from the plume's axis that RAMPS with age, so the puffs
//             separate as they rise rather than staying in the column they were born in.
//
// The point of all three is that a particle should be invisible BEFORE it is deleted.
// POSEIDON_SMOKE_DISPERSE=0 restores the authored behaviour exactly.
struct LegacySmokeDisperse
{
    bool enabled = true;
    float expand = 1.9f;   //!< POSEIDON_SMOKE_DISPERSE_EXPAND: size multiplier at end of life
    float thin = 1.0f;     //!< POSEIDON_SMOKE_DISPERSE_THIN: 0 = expand without thinning
    float spread = 0.45f;  //!< POSEIDON_SMOKE_DISPERSE_SPREAD: outward drift, m/s^2 at end of life
    float floorAlpha = 0.05f; //!< never thin below this fraction, or a fresh plume goes see-through
};
LegacySmokeDisperse& GLegacySmokeDisperse();

//! SMK-036: one cloudlet's contribution to the renderer's ground-shadow pass.
struct CloudletShadowSample
{
    Vector3 pos;
    float radius = 0.0f;
    float density = 0.0f;
    float heightAboveGround = 0.0f;
};
//! Returns false for anything that is not a legacy cloudlet, or is too faint to matter.
bool SampleCloudletShadow(const Entity* entity, CloudletShadowSample& out);

class CloudletSource: public SerializeClass
{
	protected:
	// source of smoke trail
	// how often generate
	float _interval,_nextTime;

	// parameters of cloudlets
	Ref<LODShapeWithShadow> _cloudletShape;
	float _cloudletDuration;
	float _cloudletAnimPeriod;
	float _cloudletSize;
	float _cloudletAlpha;
	bool _smokeShadow = true;
	float _cloudletGrowUp;
	float _cloudletFadeIn;
	float _cloudletFadeOut;
	float _cloudletAccY;
	float _cloudletMinYSpeed;
	float _cloudletMaxYSpeed;
	float _cloudletInitT;
	float _cloudletDeltaT;
	Color _cloudletColor;
	Ref<CloudletTTable> _cloudletTTable;
	Vector3 _cloudletSpeed;

	Vector3 _lastPosition;
	bool _lastPositionValid;

	float _generalize; // LOD level 1.0 = best, 1e20 = invisible

	public:
	CloudletSource( LODShapeWithShadow *shape = nullptr, float interval = 0.3);
	float GetInterval() const {return _interval;}
	void SetInterval( float interval ){_interval=interval;}
	void SetShape( LODShapeWithShadow *shape ){_cloudletShape=shape;}
	void SetAlpha( float alpha ) {_cloudletAlpha=alpha;}
	void SetSmokeShadow(bool enabled) {_smokeShadow = enabled;}
	void SetSize( float size ) {_cloudletSize=size;}
	void SetTimes( float duration, float animPeriod )
	{
		_cloudletDuration=duration;
		_cloudletAnimPeriod=animPeriod;
	}
	void SetFades( float growUp, float fadeIn, float fadeOut )
	{
		_cloudletGrowUp=growUp;
		_cloudletFadeIn=fadeIn;
		_cloudletFadeOut=fadeOut;
	}
	void SetSpeed( Vector3Par speed ) {_cloudletSpeed=speed;}
	void SetClimbRate( float accY, float minY, float maxY )
	{
		_cloudletAccY=accY;
		_cloudletMinYSpeed=minY;
		_cloudletMaxYSpeed=maxY;
	}
	Color GetColor() const {return _cloudletColor;}
	void SetColor(ColorVal color) {_cloudletColor = color;}

	//! `centre` is the plume ORIGIN when `pos` is a jittered spawn point around it (SMK-035);
	//! passing nothing means the two are the same.
	Cloudlet *Drop( Vector3Par pos, Vector3Par speed, const Vector3 *centre = nullptr );
	void Simulate
	(
		Vector3Par pos, Vector3Par speed, float deltaT
	);

	void Load(const ParamEntry &cls);

	LSError Serialize(ParamArchive &ar) override;
};

class DustSource: public CloudletSource
{
	typedef CloudletSource base;

	float _size,_sourceSize;

	protected:
	float _generalizeFactor;
	float _maxGeneralize;
	float _windCoef;

	private:
	void Init();

	public:
	DustSource( LODShapeWithShadow *shape, float interval );
	DustSource( float interval=0.05 ); // with basic shape
	void Simulate
	(
		Vector3Par pos, Vector3Par speed, float density, float deltaT
	);
	float GetSize() const {return _size;}
	float GetSourceSize() const {return _sourceSize;}

	void SetSize( float size ) {_size=_sourceSize=size;}
	void SetSize( float size, float sSize ) {_size=size,_sourceSize=sSize;}

	void SetWindCoef(float coef) {_windCoef = coef;}

	LSError Serialize(ParamArchive &ar) override;

	void Load(const ParamEntry &cls);
};

class WaterSource: public DustSource
{
	public:
	WaterSource( LODShapeWithShadow *shape, float interval );
	WaterSource( float interval=0.05 ); // water shape
};

class ExhaustSource: public DustSource
{
	typedef DustSource base;

	void Init();

	public:
	ExhaustSource( LODShapeWithShadow *shape, float interval );
	ExhaustSource( float interval=0.05 ); // water shape

	void SetSize( float size );
};

class WeaponCloudsSource: public DustSource
{
	typedef DustSource base;

	protected:
	float _timeToLive;

	private:
	void Init();

	public:
	WeaponCloudsSource( float interval=0.05 ); // basic shape
	WeaponCloudsSource( LODShapeWithShadow *shape, float interval );
	void Simulate
	(
		Vector3Par pos, Vector3Par speed, float density, float deltaT
	);
	void Start( float time );
	bool Active() const {return _timeToLive>=0;}

	LSError Serialize(ParamArchive &ar) override;
};

class SmokeSource: public CloudletSource
{
	typedef CloudletSource base;

	protected:
	Color _color;
	Vector3 _speed;

	float _density;
	float _size, _sourceSize;
	float _inOutDensity;

	float _timeToLive;
	float _in,_inTime,_inInv;
	float _out,_outTime,_outInv;

	public:
	SmokeSource(LODShapeWithShadow *shape = nullptr, float density = 1.0, float size = 1.0);

	bool Simulate
	(
		Vector3Par pos, Vector3Par speed, float deltaT, SimulationImportance prec
	);

	void SetSourceSize(float sourceSize)
	{
		_sourceSize = sourceSize;
	}
	void SetSize(float size, float sourceSize)
	{
		_size = size;
		_sourceSize = sourceSize;
	}

	void SetIn( float in )
	{
		_inInv=( in>0 ? 1/in :0 );
		_in=_inTime=in;
	}
	void SetOut( float out )
	{
		_outInv=( out>0 ? 1/out :0 );
		_out=_outTime=out;
	}
	void SetSourceTimes( float in, float live, float out )
	{
		_timeToLive=live;
		SetIn(in);
		SetOut(out);
	}

	LSError Serialize(ParamArchive &ar) override;

	void Load(const ParamEntry &cls);
};

class SmokeSourceVehicle: public Entity,public SmokeSource
{
	typedef Vehicle base;

	protected:
	Ref<LightPoint> _light;
	float _lightTime;
	OLink<EntityAI> _owner;
	WeaponCloudsSource _fire,_darkFire;
	float _minExplosionFactor,_maxExplosionFactor;
	Poseidon::Foundation::Time _explosionTime;
	bool _exploded;
	
	public:
	SmokeSourceVehicle
	(
		LODShapeWithShadow *shape=nullptr,
		float density=1, float size=1,
		EntityAI *owner=nullptr
	);
	~SmokeSourceVehicle() override;
	void SetExplosion( float minExp, float maxExp )
	{
		_minExplosionFactor=minExp;
		_maxExplosionFactor=maxExp;
	}

	void SimulateExplosion();
	void Simulate( float deltaT, SimulationImportance prec ) override;
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;
	void Explode(Poseidon::Foundation::Time time=TIME_MIN);
	bool ExplosionFinished() const;

	virtual Object *GetObject() const {return nullptr;}

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR
	USE_CASTING(base)
};

class SmokeSourceOnVehicle: public SmokeSourceVehicle,public AttachedOnVehicle
{
	public:
	SmokeSourceOnVehicle
	(
		LODShapeWithShadow *shape,
		float density=1, float size=1, EntityAI *owner=nullptr,
		Object *vehicle=nullptr, Vector3Par position=VZero
	);
	~SmokeSourceOnVehicle() override{}

	void UpdatePosition() override;

	Object *GetObject() const override {return _vehicle;}

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR;
};

class Explosion: public Vehicle
{
	typedef Vehicle base;

	float _exSize;
	WeaponCloudsSource _fire;
	bool _soundDone;
	Ref<LightPoint> _light;
	float _minLightTime;
	OLink<Vehicle> _owner; // who owned the shot

	public:
	Explosion
	(
		LODShapeWithShadow *shape,
		Vehicle *owner=nullptr, float duration=1
	);
	~Explosion() override;

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void Sound( bool inside, float deltaT ) override;

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static Explosion *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;

	USE_FAST_ALLOCATOR;
};

class Crater: public Smoke
{
	typedef Smoke base;

	SmokeSource _smoke1;
	SmokeSource _smoke2;
	SmokeSource _smoke3;
	DustSource _dust;
	float _dustTimeToLive;
	float _size;
	bool _isSmoke;
	bool _isBlood;
	bool _isWater;

public:
	Crater
	(
		LODShapeWithShadow *shape, VehicleNonAIType *type, float timeToLive=20, float size=1,
		bool smoke = true, bool blood = false, bool water = false
	);
	void Simulate( float deltaT, SimulationImportance prec ) override;

	bool IsAnimated( int level ) const override {return false;} // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override {return false;} // shadow changed with Animate

	void Animate( int level ) override{} // no texture animation
	void Deanimate( int level ) override{}

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static Crater *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;

	USE_FAST_ALLOCATOR;
	USE_CASTING(base)
protected:
	void Init(float timeToLive, float size, bool smoke, bool blood, bool water);
};

class CraterOnVehicle: public Crater, public AttachedOnVehicle
{
	typedef Crater base;

	public:
	CraterOnVehicle
	(
		LODShapeWithShadow *shape, VehicleNonAIType *type, float timeToLive, float size,
		Object *vehicle, Vector3Par position, Vector3Par direction
	);

	CraterOnVehicle(LODShapeWithShadow *shape, VehicleNonAIType *type); // used for serialization only

	void UpdatePosition() override;

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static CraterOnVehicle *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;

	USE_FAST_ALLOCATOR;
	USE_CASTING(base)
};

class Slop : public Smoke
{
protected:
	typedef Smoke base;

	float _size;
	float _growSize; // current size (0..1)
	float _growUpTime, _growUpInv;

	Matrix4 _origTransform;

public:
	Slop(LODShapeWithShadow *shape, VehicleNonAIType *type, const Matrix4 &trans, float timeToLive = 180, float size = 1);

	void SetGrowUp( float growUp, float size )
	{
		_size = size;
		_growUpTime = growUp;
		_growUpInv = growUp > 0 ? 1 / growUp : 0;
	}

	void Simulate(float deltaT, SimulationImportance prec) override;

	bool IsAnimated( int level ) const override {return false;} // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override {return false;} // shadow changed with Animate

	void Animate( int level ) override{} // no texture animation
	void Deanimate( int level ) override{}
	
private:
	void Init(float timeToLive, float size);

	USE_FAST_ALLOCATOR;
	USE_CASTING(base)
};

class WeaponLightSource
{
protected:
	float _timeToLight;
	float _invTotalTimeToLight;
	float _lightIntensity;
	bool _lightMGun;
	bool _muzzlePulse = false;
	Ref<LightPoint> _light;

private:
	void Init();

public:
	WeaponLightSource();
	void Start(float time, float intensity, bool mGun);
	void StartMuzzle(Vector3Par actualMuzzlePosition);
	bool Active() const;
	void Simulate(Vector3Par pos, float deltaT);
	virtual LSError Serialize(ParamArchive &ar);
};

class WeaponFireSource: public WeaponCloudsSource, public WeaponLightSource
{
	typedef WeaponCloudsSource base;
	typedef WeaponLightSource base2;

private:
	void Init();

public:
	WeaponFireSource( float interval=0.05 ); // fire shape
	WeaponFireSource( LODShapeWithShadow *shape, float interval );
	void Start( float time, float intensity, bool mGun );
	bool Active() const;
	void Simulate
	(
		Vector3Par pos, Vector3Par speed, float density, float deltaT
	);

	LSError Serialize(ParamArchive &ar) override;
};

class ObjectDestructed: public Vehicle
{
	typedef Vehicle base;

	SoundPars _soundPars;
	OLink<Object> _destroy;
	float _anim,_speed;
	Link<IWave> _sound;
	SmokeSource _dust;

	public:
	ObjectDestructed( LODShapeWithShadow *shape=nullptr );
	ObjectDestructed
	(
		Object *destroy, const SoundPars &soundPars,
		float timeToLive=3, float size=1
	);
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static ObjectDestructed *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;

	USE_FAST_ALLOCATOR;
};


enum EParticleType {
  PT_Billboard,
  PT_SpaceObject
};

/*
  Using this class you can receive random values in time which will depend
  on rate of an event and maximal value to change in one event. Now it doesn't
  matter wheter you will ask for a random value ten times each second or
  one in ten seconds. The result will be in the same boundary.
  Moreover in case the rate is too small and we ask for values often than _Rate
  parameter, the result will be interpolated (this is not done yet).
  Note that to use this class you have to call Init(...) method first.
*/
class CRandomQuantity {
private:
  // Rate of the event (in NumberOfEvents/ms).
  float _Rate;
  // Maximum value to change in one event.
  float _MaxChangeInEvent;
public:
  CRandomQuantity() {
  }
  void Init(float Rate, float MaxChangeInEvent) {
    _Rate = Rate;
    _MaxChangeInEvent = MaxChangeInEvent;
  }
  /*
    According to time period dT this method will return random number within 
    specified boundary.
  */
  float Get(int dT) {
    // MaxChangeDuringPeriod 
    float MCDP = _Rate * dT * _MaxChangeInEvent;
    return (rand() * MCDP * 2) / RAND_MAX - MCDP;
  }
};

/*
  This class represents a random walk according to specified parameters. Using
  this class we can retrieve it's result in an arbitrary time. Random walk starts
  in point zero. At every event value can change by an arbitrary value in interval
  (- _MaxChangeInEvent, _MaxChangeInEvent). This class also provides a linear
  interpolation on returned values. Note that method Get(Time) must be called with
  forward time in sequence - forward walk.
*/
class CRandomWalk {
private:
  // Time period between events (ms).
  /*
    This suppose to be a reasonable value not too much bigger than rate of calling
    the Get(Time) method. (50-30 ms = 20-30 FPS). At least in a present implementation.
    That's because next values in time we receive using iteration from current
    state. It would may be better to get next point directly. (Using inv Gaus curve?).
    Value -1 means the random walk returns allways 0.
  */
  int _EventPeriod;
  // Maximum value to change in one event.
  float _MaxChangeInEvent;
  // Time of the previous event.
  Poseidon::Foundation::Time _PrevTime;
  // Value of the previous event.
  float _PrevValue;
  // Value of the next event.
  float _NextValue;
public:
  CRandomWalk() {
    _EventPeriod = -1;
  }
  void Init(int EventPeriod, float MaxChangeInEvent) {
    _EventPeriod = EventPeriod;
    _MaxChangeInEvent = MaxChangeInEvent;
    _PrevTime = Poseidon::Foundation::Time(); // Set to zero
    _PrevValue = 0;
    _NextValue = (rand() * _MaxChangeInEvent * 2) / RAND_MAX - _MaxChangeInEvent;
  }
  /*
    This method is intend for forward walk only.
  */
  float Get(Poseidon::Foundation::Time T) {
    PoseidonAssert(T > _PrevTime);
    // In case we don't use the random walk
    if (_EventPeriod == -1) return 0.0f;
    // Find the proper interval using iteration.
    Poseidon::Foundation::Time NextTime = _PrevTime.AddMs(_EventPeriod);
    while (NextTime < T) {
      _PrevValue = _NextValue;
      _NextValue += (rand() * _MaxChangeInEvent * 2) / RAND_MAX - _MaxChangeInEvent;
      _PrevTime = NextTime;
      NextTime = _PrevTime.AddMs(_EventPeriod);
    }
    // Use linear interpolation to get the value
    return (T.Diff(_PrevTime) * (_NextValue - _PrevValue)) / _EventPeriod + _PrevValue;
  }
};

/*
  This macro will return value of index (index*(aa->Size()-1)). The value will be lineary
  interpolated from 2 values around. That's why this macro needs items in the
  autoarray to be allowed to add by themselves and multiply by a float.
  Note that this macro requires at least one item to be in the AutoArray.
*/
#define AA_GETVALUELINEAR(aa,index,value) { \
  double IntegerPart; \
  float FractionPart = modf((float)(aa.Size() - 1)*(index), &IntegerPart); \
  int i = (int)IntegerPart; \
  if (i < (aa.Size() - 1)) { \
    value = aa[i] + (aa[i + 1] - aa[i]) * FractionPart; \
  } \
  else { \
    value = aa[i]; \
  } \
}

/*
  Provides a particle simulation.
*/
class CParticle : public Entity {
private:
  typedef Entity base;
  // Orientation of the particle after rotation - usually set by random.
  Matrix3 _RotOrientation0_m_Orientation;
  Matrix3 _InvRotOrientation0;
  // ----- Behaviour section -----
  // Type of the particle.
  EParticleType _Type;
  // Time period (int s) the OnTimer event will be executed with.
  float _TimerPeriod;
  // Last age of the particle OnTimer event was called.
  float _LastOnTimerScriptCalling;
	Ref<AnimationRT> _Animation;
	WeightInfo _Weights;
	Ref<Skeleton> _Skeleton;
  // ----- LifeTime & Age -----
  // Expected time for a particle to live (in s).
  float _Lifetime;
  // Actual age of a particle (in s).
  float _Age;
  // ----- State of the particle -----
  // Position of the particle in space.
  Vector3 _Position;
  // Orientation of the particle - rotation around random vector (angle).
  float _Rotation;
  // ----- Movement changes -----
  // Vector which determines direction of a movement and speed (m/s) by it's magnitude in the beginning.
  Vector3 _MoveVelocity;
  // Scalar which determines rotation speed of the particle at the beginning(rot/sec).
  float _RotationVelocity;
  // ----- Movement parameters -----
  // Weight of a particle (in kg).
  float _Weight;
  // Volume of a particle (in m^3).
  float _Volume;
  // Impact of the density of the environment to movement of the particle (0..1).
  float _Rubbing;
  // ----- Appearance -----
  // Size of the particle in time.
  /*
    Note that this variable is intend for rendering only. Not a physics computation.
  */
  AutoArray<float> _Size;
  // Color and tranpsarency of the particle in time.
  AutoArray<Color> _Color;
  // Phase of animation of the particle in time.
  AutoArray<float> _AnimationPhase;
  // ----- Random changes -----
  // Period of the random change of the direction of the particle (in s).
  float _RandomDirectionPeriod;
  // Intensity of the random change of the direction of the particle (in m/s).
  float _RandomDirectionIntensity;
  // ----- Scripts -----
  // Script which will be executed regulary in specified period.
  RString _OnTimerScript;
  // Script which will be executed before destroying the particle.
  RString _BeforeDestroyScript;
public:
  CParticle(
    LODShapeWithShadow *Shape, RString AnimationName, EParticleType PType, float TimerPeriod, float LifeTime,
    Vector3Par Position, Vector3Par MoveVelocity, float RotationVelocity,
    float Weight, float Volume, float Rubbing,
    AutoArray<float> Size, AutoArray<Color> PColor, AutoArray<float> AnimationPhase,
    float RandomDirectionPeriod, float RandomDirectionIntensity,
    RString OnTimerScript, RString BeforeDestroyScript);
  ~CParticle() override;
  void Simulate(float deltaT, SimulationImportance prec) override;
  bool IsAnimated(int level) const override;
  bool IsAnimatedShadow(int level) const override;
  void Animate(int level) override;
  void Deanimate(int level) override;
  void Draw(int level, ClipFlags clipFlags, const FrameBase &frame) override;

	USE_FAST_ALLOCATOR
};
} // namespace Poseidon
