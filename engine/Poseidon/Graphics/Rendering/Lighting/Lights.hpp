#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Core/Visual.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>


namespace Poseidon
{
struct LightContext
{
	Vector3 position;
};

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
};

class Light: public RefCountWithLinks, virtual public Poseidon::Frame
{
protected:
	bool _on;

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

	Object *AttachedOn(){return nullptr;}

	Light();
	~Light() override;

	bool IsOn() const {return _on;}
	void Switch(bool on = true) {_on = on;}

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
	float MoonPhase() const {return _moonPhase;}

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

	public:
	LightPoint();
	LightPoint( ColorVal diffuse, ColorVal ambient );
	void SetBrightness( float coef ){_startAtten=50*coef;}
	float Brightness() const override;
	float SortBrightness() const override;
	Color GetObjectColor() const override;

	float FlareIntensity( Vector3Par camPos, Vector3Par camDir ) const override;
	Color Apply( Vector3Par point, Vector3Par normal ) override;
	void GetDescription(LightDescription &desc) const override;
	void ToDraw( ClipFlags clipFlags=ClipAll, bool dimmed=false ) override;

	void Load(const ParamEntry &cls);
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
	Ref<LODShapeWithShadow> _shape;
	float _startAtten;
	float _size;

	public:
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

	void SetAngle( float angle ){_angle=angle;}
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
	Object *AttachedOn(){return AttachedOnVehicle::AttachedOn();}
};
class LightPseudoReflectorOnVehicle: public LightPseudoReflector,public AttachedOnVehicle
{
	public:
	LightPseudoReflectorOnVehicle
	(
		LODShapeWithShadow *shape,
		Object *vehicle, Vector3Par position, Vector3Par direction
	);
	Object *AttachedOn(){return AttachedOnVehicle::AttachedOn();}
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
	Object *AttachedOn(){return AttachedOnVehicle::AttachedOn();}

	void Load(const ParamEntry &cls);
};
#pragma warning(default:4250)
} // namespace Poseidon
