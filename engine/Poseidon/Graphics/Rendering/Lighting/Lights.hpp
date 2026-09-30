#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Core/Visual.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/MuzzleFlashPulse.hpp>


namespace Poseidon
{
struct LightContext
{
	Vector3 position;
};

// LGT-025 -- the owner's "der headlight und die strassenlampe muessen wirklich glowen so dass
// man etwas verblendet wird". Two separate things make a lit bulb read as lit, and only one of
// them was on under wgpu.
//
// THE CORE is the HalfLight billboard Scene::DrawVolumeLight puts at the bulb. Measured at the
// cafe at 23:00, 40 m from a street lamp: its peak pixel is 199/255, while the wall the same
// lamp lights peaks at 222 and the brightest pixel in the whole frame is 241. The bulb is
// DIMMER than the surface it illuminates, which is the report in one number. It is dim because
// LightReflector/LightPointVisible normalise the colour to unit LENGTH before packing it -- a
// warm (0.9, 0.8, 0.6) lamp leaves that as (0.66, 0.59, 0.44), so the marker is asked for 66%
// of white and gets it. `peakNormalised` asks for peak = 1 instead, which is free: the light it
// CASTS is a different object entirely (the LightPoint) and is not touched.
//
// THE HALO is the flare sprite Scene::DrawFlares draws for every active light the camera can
// see, with the reflector's own cone test (LightReflector::FlareIntensity returns 0 unless the
// camera is INSIDE the beam) already deciding when an oncoming headlight should dazzle. It is
// alive on wgpu but throttled to nothing: alpha capped at 0.10 and size 0.030 against GL33's
// 0.2 / 0.22, from an era when the sprite was a large opaque core that read as a solar disc.
// `strength` and `size` scale those two caps.
//
// Deliberately NOT a bigger HalfLight billboard: LGT-023 tried an angular size floor on it and
// it drew a grey SAUCER beside the lamp, because HalfLight is a flat world-space disc. The
// billboard keeps its authored size; the soft falloff comes from the flare texture, which is
// what a halo is actually made of.
//
// What this canNOT do, measured rather than assumed -- see the report on LGT-025. The wgpu HDR
// bloom knee is at 1.0 scene-referred and NOTHING in a night frame reaches it: with
// WGR_BLOOM_INTENSITY=2.0 (fifty times the shipped 0.04) the lamp head region moves 14.6 -> 14.8
// and the wall 77.2 -> 77.9, i.e. the pyramid is inert. The bulb cannot exceed 1.0 because every
// stage of the path it takes is 8-bit or clamped -- PackedColor at the DrawVolumeLight API,
// TLVertex::color in DoLightLighting, WgrVertex2D::color (u32) in the wgpu 2D vertex, and
// gfx2d/shader.wgsl's `textureSample * in.color`; and EngineWgpu's BlendForSpec has no additive
// case at all, so the IsLight sprite is SrcAlpha/OneMinusSrcAlpha and cannot accumulate past
// its own source either. A real HDR glare needs one of those widened; none of them is in this
// change's reach.
struct BulbGlowSettings
{
	bool enabled = true;      //!< POSEIDON_BULB_GLOW=0 restores the pre-LGT-025 look exactly.
	bool peakNormalised = true; //!< POSEIDON_BULB_GLOW_PEAK=0 keeps the unit-length colour.
	float strength = 4.0f;    //!< POSEIDON_BULB_GLOW_STRENGTH: scales the wgpu flare alpha cap.
	float size = 6.0f;        //!< POSEIDON_BULB_GLOW_SIZE: scales the wgpu flare sprite size.
	// A reflector (headlight, searchlight) flares only inside its +/-12 degree cone. That is
	// correct for the beam and wrong for the bulb -- a lens you can see is a lens that glows.
	// This is the halo a reflector keeps OUTSIDE its cone, as a fraction of the in-cone value,
	// faded by cos^2 of the off-axis angle so it reaches zero side-on and never flares from
	// behind. POSEIDON_BULB_GLOW_SIDE=0 is the 2001 hard cut.
	float sideGlow = 0.30f;   //!< POSEIDON_BULB_GLOW_SIDE
	// LGT-027. The MARKER -- the billboard drawn at the bulb itself, which is a different
	// mechanism from the flare above and, unlike it, is not occlusion-gated. It is why a
	// vehicle's lamps read as lit at all (LGT-014). It was drawn at `_size * 0.5` and then
	// crushed again by 0.035 inside DrawVolumeLight, i.e. about 1.75 CENTIMETRES, which is a
	// dot. A real headlight lens is 15 to 20 cm across, so this is the authored size in
	// metres rather than a multiplier on a number nobody can read.
	//
	// It is a FIXED world size on purpose. LGT-023 withdrew an angular floor here because
	// scaling a flat world-space billboard up with distance produced a grey saucer beside the
	// lamp; a headlight-sized marker at a headlight has neither problem.
	// LGT-027: back to roughly the authored size. Enlarging this was the wrong lever -- with
	// additive blending a headlight-sized marker GLOWS, and without it no size looked like
	// anything but fog. Measured at 0.45 m it swallowed the whole vehicle in grey.
	float markerSize = 0.05f; //!< POSEIDON_BULB_GLOW_MARKER, metres across
};
BulbGlowSettings& GBulbGlow();

struct TLMaterial;

enum LightKind
{
	LTDirectional,
	LTPoint,
	LTSpotLight
};
struct LightDescription // D3D-like light description
{
	Vector3 pos,dir;
	Color diffuse,ambient;
	LightKind type;
	float startAtten; // distance with intensity 1, falloff starts here
	// for spotlight only: inner and outer cone angles
	float theta,phi; // see corresponding D3DLIGHT8 members
	// LGT-014: how far past startAtten the light still reaches, as a multiple of it. The
	// shader has always cut a local light at 10x its start attenuation, so START RADIUS AND
	// REACH WERE THE SAME NUMBER -- which is why shrinking a street lamp's core to get a
	// pool instead of a floodlight also shrank its reach from ~500 m to ~90 m, and the owner
	// saw lamps vanish as he backed away. They are separate properties and this separates
	// them. 10 is the historical value, so a light that says nothing behaves as before.
	float endAttenScale = 10.0f;
};

class Light: public RefCountWithLinks, virtual public Poseidon::Frame
{
protected:
	bool _on;
	bool _daylightVisible;

	public:
	virtual float FlareIntensity( Vector3Par camPos, Vector3Par camDir ) const {return 0;}
	virtual void Prepare( const Matrix4 &worldToModel ) = 0;
	virtual void SetMaterial( const TLMaterial &mat ) = 0;
	virtual Color Apply( Vector3Par pos, Vector3Par normal ) = 0;
	virtual float Brightness() const = 0;
	virtual float SortBrightness() const;
	virtual Color GetObjectColor() const = 0;
	virtual void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false )= 0;
	virtual bool Visible( const Object *obj ) const;

	virtual void GetDescription(LightDescription &desc) const = 0;

	virtual Object *AttachedOn(){return nullptr;}
	virtual bool HasVisibleBulb() const {return false;}

	Light();
	~Light() override;

	virtual bool IsOn() const {return _on;}
	void Switch(bool on = true) {_on = on;}

	// By default a local light is scaled by the sun's NightEffect and dropped
	// entirely in daylight -- right for a street lamp, which should not glow at
	// noon. A fire, a muzzle flash or headlights are bright enough to read
	// against daylight and must not be gated on it. Off by default so every
	// existing lamp keeps its behaviour exactly.
	bool IsDaylightVisible() const {return _daylightVisible;}
	void SetDaylightVisible(bool visible) {_daylightVisible = visible;}

	float SquareDistance( Vector3Par from ) const
	{
		Vector3 temp = Position()-from;
		return temp.SquareSize();
	}

	int Compare( const Light &with, const LightContext &context ) const;
	int Compare( const Light &with ) const;
	bool operator < ( const Light &with ) const {return Compare(with)<0;}
	bool operator > ( const Light &with ) const {return Compare(with)>0;}
};

// basic directional+ambient light

class LightSun
{
	private:

	Vector3 _direction;
	Vector3 _shadowDirection;

	Vector3 _sunDirection;
	Vector3 _moonDirection,_moonDirectionUp;
	Matrix3 _starsOrientation;

	Color _colorFull; // without considering clouds
	Color _diffuse;
	Color _ambient;
	Color _sunColor;
	Color _sunObjectColor,_sunHaloObjectColor;
	Color _moonObjectColor,_moonHaloObjectColor;
	Color _skyColor,_sunSkyColor;

	Color _ambientPrecalc;
	Color _diffusePrecalc;

	private:
	float _moonPhase; // 0.5 - full moon
	float _nightEffect;
	float _starsVisible;

	// --- Real lunar/solar geometry (Graphics/Rendering/Lighting/Ephemeris.hpp) -------
	// The 2001 model had a moon DIRECTION and a scalar "phase" it used only to pick a
	// frame of the moon.p3d texture animation. A phase-correct renderer needs more:
	// how much of the disc is lit, how big the disc is, and which way the terminator
	// faces. These are filled by Recalculate from the ephemeris (or held at neutral
	// values when the legacy model is selected).

	// Unit direction TO the sun as seen from the moon (i.e. the moon's light vector).
	// The sun is 390x further away than the moon, so this is the solar direction to
	// well under a degree — but it is kept SEPARATE from _sunDirection on purpose: the
	// renderer may be showing a legacy or temporally-smoothed sun, and the terminator
	// must come from the astronomy, not from whatever the sun disc happens to be doing.
	Vector3 _moonSunDirection;
	float _moonIllumination;   // lit fraction of the visible disc, 0 (new) .. 1 (full)
	float _moonBrightness;     // total disc brightness relative to a full moon (0..1)
	float _moonAngularRadius;  // apparent RADIUS of the disc, radians (~0.0045)
	float _moonLightAmount;    // 0..1 — how much of the shading light is moonlight now


	public:

	LightSun();

	float NightEffect() const {return _nightEffect;}
	float StarsVisibility() const {return _starsVisible;}

	void SetMaterial( const TLMaterial &mat );
	void GetDescription(LightDescription &desc) const;

	void Recalculate(World *world=nullptr);

	Vector3Val ShadowDirection() const {return _shadowDirection;}
	Vector3Val SunDirection() const {return _sunDirection;}
	Vector3Val MoonDirection() const {return _moonDirection;}
	const Matrix3 &StarsOrientation() const {return _starsOrientation;}
	Vector3Val MoonDirectionUp() const {return _moonDirectionUp;}

	ColorVal SunColor() const {return _sunColor;}
	ColorVal SunSkyColor() const {return _sunSkyColor;}

	ColorVal SunObjectColor() const {return _sunObjectColor;}
	ColorVal SunHaloObjectColor() const {return _sunHaloObjectColor;}

	ColorVal MoonObjectColor() const {return _moonObjectColor;}
	ColorVal MoonHaloObjectColor() const {return _moonHaloObjectColor;}
	// Synodic age, 0 = new .. 0.5 = full. Legacy units (drives moon.p3d's texture
	// animation on the GL33 skydome); a phase-correct shader wants MoonIllumination().
	float MoonPhase() const {return _moonPhase;}

	// Unit direction TO the sun as seen from the moon — the vector a shader shades the
	// lunar sphere with, and the thing that fixes the terminator's ORIENTATION.
	Vector3Val MoonSunDirection() const {return _moonSunDirection;}
	// Illuminated fraction of the disc, 0 (new) .. 1 (full).
	float MoonIllumination() const {return _moonIllumination;}
	// Total brightness relative to a full moon, from the empirical lunar phase function
	// (astro::MoonPhaseBrightness). Strongly non-linear: quarter moon is ~0.09, not 0.5.
	float MoonBrightness() const {return _moonBrightness;}
	// Apparent angular RADIUS of the disc in radians. Varies ~+/-6% over the month.
	float MoonAngularRadius() const {return _moonAngularRadius;}
	// 0..1 blend of moonlight into the scene's directional light. >0 only once the sun
	// is well below the horizon AND the moon is above it; scaled by the phase function.
	// While this is >0, Direction()/ShadowDirection() point along the MOONLIGHT, not the
	// sun (SunDirection() always stays astronomical, for the sky).
	float MoonLightAmount() const {return _moonLightAmount;}

	ColorVal SkyColor() const {return _skyColor;}
	ColorVal GetDiffuse() const {return _diffuse;}
	void SetDiffuse( ColorVal diffuse ) {_diffuse=diffuse;}
	ColorVal GetColorFull() const {return _colorFull;}

	ColorVal Diffuse() const {return _diffuse;}
	ColorVal Ambient() const {return _ambient;}
	Vector3Val Direction() const {return _direction;}

	Color AmbientResult() const;
	Color FullResult( float diffuse=1.0 ) const; // full or partial diffuse + ambient

	ColorVal DiffusePrecalc() const {return _diffusePrecalc;}
	ColorVal AmbientPrecalc() const {return _ambientPrecalc;}
};

class LightPositioned: public Light
{
	typedef Light base;

	protected:
	Vector3 _modelPos,_modelDir;
	
	public:
	LightPositioned();
	// prepare light to be applied in model space
	void Prepare( const Matrix4 &worldToModel ) override;
};

class LightPositionedColored: public LightPositioned
{
	typedef LightPositioned base;

	protected:
	Color _ambientPrecalc;
	Color _diffusePrecalc;

	Color _ambient;
	Color _diffuse;

	public:
	LightPositionedColored();
	LightPositionedColored( ColorVal diffuse, ColorVal ambient );
	void SetMaterial( const TLMaterial &mat ) override;

	void SetDiffuse( ColorVal diffuse ){_diffuse=diffuse;}
	ColorVal GetDiffuse() const {return _diffuse;}
	
	void SetAmbient( ColorVal ambient ){_ambient=ambient;}
	ColorVal Ambient() const {return _ambient;}

};

class LightPoint: public LightPositionedColored
{
	// omnidirectional point light
	typedef LightPositionedColored base;

	protected:
	float _startAtten;
	float _endAttenScale = 10.0f; //!< LGT-014, see LightDescription::endAttenScale

	public:
	LightPoint();
	LightPoint( ColorVal diffuse, ColorVal ambient );
	void SetBrightness( float coef ){_startAtten=50*coef;}
	//! Reach, as a multiple of startAtten. Raise it when the core is deliberately small.
	void SetEndAttenScale( float scale ){_endAttenScale=scale;}
	float GetEndAttenScale() const {return _endAttenScale;}
	float Brightness() const override;
	float SortBrightness() const override;
	Color GetObjectColor() const override;

	float FlareIntensity( Vector3Par camPos, Vector3Par camDir ) const override;
	Color Apply( Vector3Par point, Vector3Par normal ) override;
	void GetDescription(LightDescription &desc) const override;
	void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false ) override;

	void Load(const ParamEntry &cls);
};

// The same actual point-light description/local-light renderer as ordinary
// lights, with simulation-clock expiry even between low-frequency AI ticks.
class MuzzleFlashLight final: public LightPoint
{
    MuzzleFlashPulse _pulse;
public:
    MuzzleFlashLight();
    void Trigger(Vector3Par position, float strength = 1.0f);
    bool IsOn() const override;
    void GetDescription(LightDescription& desc) const override;
    Color Apply(Vector3Par point, Vector3Par normal) override;
    float FlareIntensity(Vector3Par camPos, Vector3Par camDir) const override;
    Color GetObjectColor() const override;
};

class LightPointVisible: public LightPoint
{
	Ref<LODShapeWithShadow> _shape;
	float _size;

	public:
	LightPointVisible();
	LightPointVisible
	(
		LODShapeWithShadow *shape,
		ColorVal diffuse, ColorVal ambient, float size=1
	);
	void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false ) override;
	void SetSize( float size ){_size=size;}

	void Load(const ParamEntry &cls);
};

class LightReflector: public LightPositionedColored
{
	typedef LightPositionedColored base;

	protected:
	float _angle;
	bool _explicitAngle = false;
	Ref<LODShapeWithShadow> _shape;
	float _startAtten;
	float _size;
	//! LGT-017: a reflector CARRIED BY THE VIEWER draws no bulb. The glow billboard sits right
	//! in front of the eye there, so it reads as a grey blob in the middle of the screen
	//! rather than as a lamp seen from outside. Vehicles keep theirs -- that IS the "the
	//! vehicle's own lights should look lit" the owner asked for.
	bool _drawHalo = true;

	public:
	void SetDrawHalo( bool on ){_drawHalo=on;}
	bool HasVisibleBulb() const override {return _drawHalo;}
	//! LGT-020: the reach, in metres, set DIRECTLY. SetBrightness is not a brightness at all --
	//! it is 200*InvSqrt(diffuse/coef), i.e. a reach, so turning a torch "up" made it carry
	//! 350 m across a village instead of making it brighter. Intensity belongs in the colour.
	void SetRange( float metres ){_startAtten=metres>0.01f?metres:0.01f;}
	float GetRange() const {return _startAtten;}
	LightReflector();
	LightReflector
	(
		LODShapeWithShadow *shape,
		ColorVal diffuse, ColorVal ambient, float angle, float size=1
	);

	float Brightness() const override;
	void SetBrightness(float coef);
	Color GetObjectColor() const override;

	void SetDiffuse( ColorVal diffuse ){_diffuse=diffuse;}
	ColorVal GetDiffuse() const {return _diffuse;}
	
	void SetAmbient( ColorVal ambient ){_ambient=ambient;}
	ColorVal Ambient() const {return _ambient;}

	void SetSize( float size ){_size=size;}

	// Explicit dev-tool half-angle; legacy constructor-only vehicle beams retain their profile.
	void SetAngle(float angle)
	{
		if (!(angle > 0.0f && angle < 1.55f)) return;
		_angle = angle;
		_explicitAngle = true;
	}
	float Angle() const {return _angle;}

	bool Visible( const Object *obj ) const override;
	float FlareIntensity( Vector3Par camPos, Vector3Par camDir ) const override;
	
	Color Apply( Vector3Par point, Vector3Par normal ) override;
	void GetDescription(LightDescription &desc) const override;
	void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false ) override;
};

class LightPseudoReflector: public LightPositioned
{
	protected:
	Ref<LODShapeWithShadow> _shape;

	public:
	LightPseudoReflector();
	LightPseudoReflector( LODShapeWithShadow *shape	);
	
	Color Apply( Vector3Par point, Vector3Par normal ) override;
	void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false ) override;
};

#pragma warning(disable:4250)
class LightReflectorOnVehicle: public LightReflector,public AttachedOnVehicle
{
	public:
	LightReflectorOnVehicle
	(
		LODShapeWithShadow *shape,
		ColorVal diffuse, ColorVal ambient,
		Object *vehicle,
		Vector3Par position, Vector3Par direction, float angle, float size=1
	)
	:LightReflector(shape,diffuse,ambient,angle,size),
	AttachedOnVehicle(vehicle,position,direction)
	{
	}
	Object *AttachedOn() override {return AttachedOnVehicle::AttachedOn();}
};
class LightPseudoReflectorOnVehicle: public LightPseudoReflector,public AttachedOnVehicle
{
	public:
	LightPseudoReflectorOnVehicle
	(
		LODShapeWithShadow *shape,
		Object *vehicle, Vector3Par position, Vector3Par direction
	);
	Object *AttachedOn() override {return AttachedOnVehicle::AttachedOn();}
};
class LightPointOnVehicle: public LightPointVisible,public AttachedOnVehicle
{
	public:
	LightPointOnVehicle
	(
		LODShapeWithShadow *shape,
		ColorVal diffuse, ColorVal ambient,
		Object *vehicle,
		Vector3Par position, float size=1
	);
	LightPointOnVehicle
	(
		Object *vehicle, Vector3Par position
	);
	Object *AttachedOn() override {return AttachedOnVehicle::AttachedOn();}

	void Load(const ParamEntry &cls);
};
#pragma warning(default:4250)
} // namespace Poseidon
