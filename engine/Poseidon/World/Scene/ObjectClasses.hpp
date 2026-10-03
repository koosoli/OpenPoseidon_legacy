#pragma once

#include <Poseidon/IO/ParamFile/ParamFile.hpp>

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>

// type of entity with hitpoint support

namespace Poseidon
{
class EntityHitType: public EntityType
{
	typedef EntityType base;
	friend class EntityHit;

	protected:
	
	HitPointList _hitPoints;
	float _structuralDammageCoef;

	public:
	EntityHitType( const ParamEntry *param );
	~EntityHitType() override;

	void Load(const ParamEntry &par) override;
	void InitShape() override; // after shape is loaded
	void DeinitShape() override; // before shape is unloaded

	__forceinline float GetStructuralDammageCoef() const {return _structuralDammageCoef;}
	__forceinline const HitPointList &GetHitPoints() const {return _hitPoints;}
	__forceinline HitPointList &GetHitPoints() {return _hitPoints;}

};

// entity with hitpoint support
class EntityHit: public Entity
{
	typedef Entity base;
	protected:
	AutoArray<float> _hit; // Hitpoints (local dammage)

	public:
	EntityHit
	(
		LODShapeWithShadow *shape, const EntityHitType *type, int id
	);
	~EntityHit() override;
	// apply hit at given point to hitpoints
	float LocalHit( Vector3Par pos, float val, float valRange ) override;
	// Get dammage state of given hitpoint (0 or 1)
	float GetHit( const HitPoint &hitpoint ) const;
	// Get dammage state of given hitpoint (continuos) 
	float GetHitCont( const HitPoint &hitpoint ) const; // used for indication

	void ResetStatus() override;

	__forceinline const EntityHitType *GetType() const
	{
		return static_cast<const EntityHitType *>(_type.GetRef());
	}
};

class StreetLampType: public EntityHitType
{
	typedef EntityHitType base;
	friend class StreetLamp;

	protected:
	HitPoint _bulbHit;
	
	float _brightness;
	Color _colorDiffuse;
	Color _colorAmbient;

	public:
	StreetLampType( const ParamEntry *param );
	~StreetLampType() override;

	void Load(const ParamEntry &par) override;
	void InitShape() override; // after shape is loaded
	void DeinitShape() override; // before shape is unloaded

};

class StreetLamp: public EntityHit
{
public:
	enum LightState
	{
		LSOff,
		LSOn,
		LSAuto
	};
	typedef EntityHit base;
protected:
	LightState _lightState;
	bool _pilotLight; // switch the light on/off
	// LAMP-004: a point light OR a downward spot, depending on the dev lever, so the
	// declared type has to be the common base of both.
	Ref<LightPositionedColored> _light;
	// LAMP-004: the weak omni light beside the cone. The cone CUTS -- lights_contrib
	// skips a fragment outside it entirely -- so without this the surroundings are black.
	Ref<LightPointVisible> _spill;
	Vector3 _lightPos;
	// Poseidon::Dev::LampLightSettings::generation the live _light was built with. A
	// dev-panel change bumps that counter; CreateLight then rebuilds this light so a
	// slider is visible rather than only affecting lamps created afterwards.
	unsigned _lightGeneration = 0;
	
public:
	StreetLamp( LODShapeWithShadow *shape, StreetLampType *type, int id );

	__forceinline const StreetLampType *Type() const
	{
		return static_cast<const StreetLampType *>(_type.GetRef());
	}

	LightState GetLightState() const {return _lightState;}
	void SwitchLight(LightState state);

	void Init( Matrix4Par pos ) override;
	void SimulateSwitch();
	void ResetStatus() override;
	void OnTimeSkipped() override;
	void CreateLight(Matrix4Par pos);
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void HitBy( EntityAI *killer, float howMuch, RString ammo ) override;

	LSError Serialize(ParamArchive &ar) override;
};

class RoadType: public RefCount
{
	friend class Road;
	Ref<LODShapeWithShadow> _shape;

	public:
	const char *GetName() const {return _shape->Name();}

	RoadType();
	RoadType( const char *name );
	~RoadType() override;
};

class RoadTypeBank: public BankArray<RoadType>
{	
};

extern RoadTypeBank RoadTypes;

class Road: public Object
{
	typedef Object base;

	InitPtr<RoadType> _roadType;

	public:
	Road( LODShapeWithShadow *shape, int id );

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate

	float GetArmor() const override;
	float GetInvArmor() const override;
	float GetLogArmor() const override;

	void DrawDiags() override;

	USE_FAST_ALLOCATOR
	USE_CASTING(base)
};

#define FOREST_PROXY_ENABLE 0

class ForestPlain: public Object
{
	typedef Object base;
	bool _singleMatrixT1;
	bool _singleMatrixT2;

	float _skewX = 0, _skewZ = 0, _offsetY = 0;
	bool _skewApplied = false; // InitSkew appended its skew to the matrix (OnTerrainChanged undoes it)

	// Cached terrain-conform plane (static per level: forest and terrain don't move),
	// published around Draw so the wgpu backend conforms this forest on the GPU instead
	// of the CPU rewriting the shared vertex buffer per instance. GL33 ignores it.
	ConformPlane _conformPlane;
	bool _conformValid = false;
	void ComputeConformPlane();

	public:
	ForestPlain( LODShapeWithShadow *type, int id );

	void InitSkew( Landscape *land ) override; // call to prepare skew matrix
	void OnTerrainChanged( Landscape *land ) override;

	Matrix4 GetInvTransform() const override;

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	Vector3 AnimatePoint( int level, int index ) const override;
	void Deanimate( int level ) override;

	float ViewDensity() const override;

	void Draw( int forceLOD, ClipFlags clipFlags, const FrameBase &pos ) override;

	// wgpu GPU-driven retained scene: expose the (static, cached) terrain-conform plane so
	// the backend can pack it once per instance instead of publishing it per-frame in Draw.
	// Returns false for skewed squares (t1/t2), whose conform is baked into the object matrix
	// (Transform()) -> they register as plain rigid; otherwise fills `out` (mode 1).
	bool GpuConformPlane( ConformPlane &out );

	#if !FOREST_PROXY_ENABLE
	// disabled proxies
	void DrawProxies
	(
		int level, ClipFlags clipFlags,
		const Matrix4 &transform, const Matrix4 &invTransform,
		float dist2, float z2, const LightList &lights
	) override{}
	int GetProxyComplexity
	(
		int level, const FrameBase &pos, float dist2
	) const override {return 0;}

	// proxy access
	int GetProxyCount(int level) const override {return 0;}
	#endif

	USE_CASTING(base)

	USE_FAST_ALLOCATOR
};

#define FOREST_PATHS 0

class Forest: public ForestPlain
#if FOREST_PATHS
,public IPaths
#endif
{
	Ref<BuildingType> _type;
	typedef ForestPlain base;
	
	public:
	Forest( BuildingType *type, int id );
	~Forest() override;

	#if FOREST_PATHS // forest are working without paths much faster
		virtual const BuildingType *GetBType() const {return _type;}
		virtual const IPaths *GetIPaths() const {return this;}
		virtual const Object *GetObject() const {return this;}
	#endif

	USE_FAST_ALLOCATOR
};

}  // namespace Poseidon
