#pragma once

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/Audio/DynSound.hpp>


namespace Poseidon
{

class BuildingInterior;

struct HousePathArrayItem
{
	Vector3 pos;
	int index;
};

typedef StaticArrayAuto<int> HousePathArrayIndexed;
typedef StaticArrayAuto<HousePathArrayItem> HousePathArray;

#define HOUSE_PATH_ARRAY(name,size) AUTO_STATIC_ARRAY(HousePathArrayItem,name,size)
#define HOUSE_PATH_ARRAY_INDEXED(name,size) AUTO_STATIC_ARRAY(int,name,size)

struct Ladder
{
	int _top,_bottom;
};

struct WeaponProxy
{
	LLink<Object> obj;
	int selection; // copied over from proxyObject

	WeaponProxy() {selection = -1;}
	bool IsPresent() const {return selection >= 0;}
};

class BuildingType: public TransportType
{
	friend class Building;
	friend class IPaths; // give access to path information
	friend class BuildingInterior; // reads _exits/_positions/_connections/_shape to derive rooms

	typedef TransportType base;

	AutoArray<int> _exits;
	AutoArray<int> _positions;
	AutoArray< FindArray<int> > _connections;
	AutoArray<Ladder> _ladders;

	float _coefInside;
	float _coefInsideHeur;
	bool _removeOnDestruction = false;

	WeaponProxy _proxies[MAX_LOD_LEVELS];

	public:
	BuildingType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override; // after shape is loaded

	const V3 &GetPosition(int index) const;
	float GetCoefInside() const {return _coefInside;}
	float GetCoefInsideHeur() const {return _coefInsideHeur;}
	int FindNearestExit(Vector3Par pos, Vector3 &ret) const;
	int FindNearestPosition(Vector3Par pos, Vector3 &ret) const;
	bool SearchPath(int from, int to, HousePathArrayIndexed &path) const;

	const Ladder &GetLadder(int i) const {return _ladders[i];}
	int GetLadderCount() const {return _ladders.Size();}
};

class FountainType: public BuildingType
{
	friend class Fountain;

	typedef BuildingType base;

	Animation _water; // east/west flag
	RString _sound;
	float _animSpeed;

	public:
	FountainType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override; // after shape is loaded
};

class ChurchType : public BuildingType
{
	friend class Church;
	typedef BuildingType base;

	AnimationRotation _hour1;
	AnimationRotation _minute1;
	AnimationRotation _hour2;
	AnimationRotation _minute2;
	AnimationRotation _hour3;
	AnimationRotation _minute3;
	AnimationRotation _hour4;
	AnimationRotation _minute4;

	public:
	ChurchType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override; // after shape is loaded
};

class IPaths
{
protected:
	AutoArray<int> _locks; 

public:
	virtual const BuildingType *GetBType() const = 0;
	virtual const Object *GetObject() const = 0;
	
	Vector3 GetPosition(int index) const;
	bool SearchPath( int from, int to, HousePathArray &path ) const;
	int FindNearestExit(Vector3Par pos, Vector3 &ret) const;
	int FindNearestPosition(Vector3Par pos, Vector3 &ret) const;
	int FindNearestPoint(Vector3Par pos, Vector3 &ret,float maxDist2=FLT_MAX) const;

	int NExits() const {return GetBType()->_exits.Size();}
	int NPos() const {return GetBType()->_positions.Size();}
	int NPoints() const {return GetBType()->_connections.Size();}
	int GetExit(int index) const {return GetBType()->_exits[index];}
	int GetPos(int index) const {return GetBType()->_positions[index];}

	bool IsLocked(int index) const
	{
		if (index >= _locks.Size()) return false;
		return _locks[index] > 0;
	}
	void Lock(int index)
	{
		int n = _locks.Size();
		if (index >= n)
		{
			_locks.Resize(index + 1);
			for (int i=n; i<=index; i++) _locks[i] = 0;
		}
		_locks[index]++;
	}
	void Unlock(int index)
	{
		DoAssert(index < _locks.Size());
		_locks[index]--;
	}
};

class Building: public VehicleSupply, public IPaths
{
	typedef VehicleSupply base;

public:
	void Destroy(EntityAI* owner, float overkill, float minExp, float maxExp) override;
	Building( VehicleType *name, int id, LODShapeWithShadow *shape );

	const IPaths *GetIPaths() const override {return this;}
	const Object *GetObject() const override {return this;}
	
	const BuildingType *Type() const
	{
		return static_cast<const BuildingType *>(GetType());
	}

	const BuildingType *GetBType() const override
	{
		return static_cast<const BuildingType *>(GetType());
	}

	void Simulate( float deltaT, SimulationImportance prec ) override;

	void Sound( bool inside, float deltaT ) override {}
	void UnloadSound() override {}

	bool CastShadow() const override;

	void DrawDiags() override;
	bool QIsManual() const override {return false;}
	bool IsMoveTarget() const override;

	Matrix4 InsideCamera( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	
	// no get-in to buildings
	bool QCanIGetIn( Person *who = nullptr ) const {return false;}

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	Vector3 GetLadderPos( int ladder, bool up );

	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;

	void DrawProxies
	(
		int level, ClipFlags clipFlags,
		const Matrix4 &transform, const Matrix4 &invTransform,
		float dist2, float z2, const LightList &lights
	) override;
	int GetProxyComplexity
	(
		int level, const FrameBase &pos, float dist2
	) const override;

	USE_CASTING(base)
};

class CameraBuilding: public Transport
{
	typedef Transport base;

	public:
	CameraBuilding( VehicleType *name, int id, LODShapeWithShadow *shape );
	
	const BuildingType *Type() const
	{
		return static_cast<const BuildingType *>(GetType());
	}

	void Simulate( float deltaT, SimulationImportance prec ) override{}

	void Sound( bool inside, float deltaT ) override {}
	void UnloadSound() override {}

	void DrawDiags() override{}
	bool QIsManual() const override {return false;}

	Matrix4 InsideCamera( CameraType camType ) const override {return MIdentity;}
	int InsideLOD( CameraType camType ) const override {return 0;}
	
	bool QCanIGetIn( Person *who = nullptr ) const override {return false;}

	bool IsAnimated( int level ) const override {return false;}
	bool IsAnimatedShadow( int level ) const override {return false;}
	void Animate( int level ) override {}
	void Deanimate( int level ) override {}
};

class Church: public Building
{
	typedef Building base;

	int _ringSmall,_ringBig;
	float _nextRing;
	float _badTime; // no clock is exact

	public:
	Church( VehicleType *name, int id, LODShapeWithShadow *shape );
	
	void Simulate( float deltaT, SimulationImportance prec ) override;

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	void ResetStatus() override;

	const ChurchType *Type() const
	{
		return static_cast<const ChurchType *>(GetType());
	}
};

class Fountain: public Building
{
	float _anim;
	Foundation::Time _lastAnimation;
	Ref<SoundObject> _sound;

	typedef Building base;

	public:
	Fountain( VehicleType *name, int id, LODShapeWithShadow *shape );

	const FountainType *Type() const
	{
		return static_cast<const FountainType *>(GetType());
	}
	
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;	
};

}  // namespace Poseidon
