#pragma once

#include <unordered_map>

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/AI/Path/AITypes.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/World/Entities/Weapons/Recoil.hpp>

#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>

class UIActions;
class UIAction;

namespace Poseidon
{
#if _DEBUG
	template <class basic>
	class derived
	{
		int _val;
		public:
		operator basic() const {return _val;}
		operator basic&() {return _val;}
		explicit derived( basic val ){_val=val;}
	};
	class SensorRowID: public derived<int>
	{
		public:
		explicit SensorRowID( int val ):derived<int>(val){}
	};
	class SensorColID: public derived<int>
	{
		public:
		explicit SensorColID( int val ):derived<int>(val){}
	};
	inline int format_as(SensorRowID id) { return (int)id; }
	inline int format_as(SensorColID id) { return (int)id; }
#else
	typedef int SensorRowID;
	typedef int SensorColID;
#endif



DECL_ENUM(MoveFinishF)
// note: value zero is reserved as no action

enum VehicleKind {VSoft,VArmor,VAir,NVehicleKind};

class Threat
{
	float _data[NVehicleKind];

	public:
	Threat(){_data[0]=_data[1]=_data[2]=0;}
	Threat( float soft, float armor, float air )
	{
		_data[VSoft]=soft,_data[VArmor]=armor,_data[VAir]=air;
	}

	float operator [] ( VehicleKind kind ) const {return _data[kind];}
	float &operator [] ( VehicleKind kind ) {return _data[kind];}

	void operator += ( const Threat &a )
	{
		_data[0]+=a._data[0];
		_data[1]+=a._data[1];
		_data[2]+=a._data[2];
	}
	Threat operator * ( float c ) const
	{
		return Threat(_data[VSoft]*c,_data[VArmor]*c,_data[VAir]*c);
	}
};

enum UnitInfoType
{
	UnitInfoSoldier,
	UnitInfoTank,
	UnitInfoCar,
	UnitInfoShip,
	UnitInfoAirplane,
	UnitInfoHelicopter,
	NUnitInfoType
};

enum LightType
{
	LightTypeMarker,
	LightTypeMarkerBlink,
};

struct LightInfo
{
	LightType type;
	Vector3 position;
	Vector3 direction;
	Color color;
	Color ambient;
	float brightness;
};

class HitPoint
{
	int _selection; // selection index (in Hitpoints LOD)
	float _armor; // hit point armor
	float _invArmor; // hit point armor
	float _passThrough; // how much of the hit will this part pass
	// to further processing
	int _index; // index in EntityAIType
	int _material; // material index
	FindArray<int> _indexCC; // index of corresponding convex component

	public:
	HitPoint();
	HitPoint
	(
		Shape *shape, const char *name, const char *altName, float armor,
		int material
	);

	HitPoint( Shape *shape, const ParamEntry &par, float armor );

	__forceinline int GetSelection() const {return _selection;}
	__forceinline float GetArmor() const {return _armor;}
	__forceinline float GetInvArmor() const {return _invArmor;}
	__forceinline float GetPassThrough() const {return _passThrough;}

	__forceinline void SetIndex(int index){_index=index;}
	__forceinline void AddIndexCC(int indexCC){_indexCC.AddUnique(indexCC);}
	__forceinline void SetIndexCC(const FindArray<int> &array){_indexCC = array;}

	__forceinline int GetIndex() const {return _index;}
	__forceinline int GetMaterial() const {return _material;}
	__forceinline bool IsConnectedCC(int i) const {return _indexCC.Find(i)>=0;}
};

typedef AutoArray< InitPtr<HitPoint> > HitPointList;

#define DEF_HIT(shape,var,name,altName,armor) \
	var=HitPoint(shape->HitpointsLevel(),name,altName,armor,-1), \
	var.SetIndex(_hitPoints.Add(&var))

#define DEF_HIT_CFG(shape,var,par,armor) \
	var=HitPoint(shape->HitpointsLevel(),par,armor), \
	var.SetIndex(_hitPoints.Add(&var))

struct ExtSoundInfo
{
	RStringB name;
	AutoArray<SoundPars> pars;
};

struct ReflectorInfo
{
	Color color;
	Color colorAmbient;
	int positionIndex;
	int directionIndex;
	Vector3 position;
	Vector3 direction;
	float size;
	float brightness;
	//! LGT-REFL-001: optional `opConeAngle` (full beam angle, degrees). Stored as the half-angle
	//! in radians; 0 when the class does not say, and then the beam keeps the engine's
	//! historical 36 degrees -- `size` is NOT read as an angle here, old configs never meant it.
	float coneHalfAngle = 0;
	//! LGT-REFL-001: optional `opFlareRange` (metres): the lamp's bulb stays visible this far
	//! even when the light it casts is short. 0 = not given, flare follows the light as before.
	float flareRange = 0;
	AnimationSection selection; // used to Hide/Unhide
	HitPoint hitPoint;

	void Load(EntityAIType &type, const ParamEntry &cls, float armor);
};

template <class Type>
struct EncryptedTraits
{
  typedef Type EncryptedType;
  enum {Key=0xa5a5a5a5};
  static EncryptedType Encrypt(Type val)
  {
    return val^Key;
  }
  static Type Decrypt(EncryptedType val)
  {
    return val^Key;
  }
};

template <class Type, class Traits=EncryptedTraits<Type> >
class Encrypted
{
  typedef typename Traits::EncryptedType EncryptedType;

  EncryptedType _value;

  static EncryptedType Encrypt(Type value) {return Traits::Encrypt(value);}
  static Type Decrypt(EncryptedType value) {return Traits::Decrypt(value);}

  public:
  operator Type() const {return Decrypt(_value);}
  Encrypted(){}
  Encrypted(Type val){_value=Encrypt(val);}

  Type operator += (Type val)
  {
    Type res = Decrypt(_value)+val;
    _value = Encrypt(res);
    return res;
  }
  Type operator -= (Type val)
  {
    Type res = Decrypt(_value)-val;
    _value = Encrypt(res);
    return res;
  }

};

class Magazine : public RefCount
{
public:
	const Ref<MagazineType> _type;
	Encrypted<int> _ammo;
	int _burstLeft; // how many shots are there in the burst (auto fired)
	float _reload;
	float _reloadMagazine;

	int _creator;
	int _id;

private:
	Magazine(const Magazine &src); // no copy - _id would be the same
	void operator =(const Magazine &src); // no copy - _id would be the same

public:
	Magazine(const MagazineType *type); // each magazine must have a type
	~Magazine() override;

	LSError Serialize(ParamArchive &ar);
	static Magazine *CreateObject(ParamArchive &ar);

	static Magazine *CreateObject(NetworkMessageContext &ctx);
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	TMError TransferMsg(NetworkMessageContext &ctx);

	USE_FAST_ALLOCATOR
};

class MagazineSlot
{
public:
	// magazine - can be nullptr
	Ref<Magazine> _magazine;
	int _mode;

	// weapon
	Ref<WeaponType> _weapon;
	Ref<MuzzleType> _muzzle;

public:
	MagazineSlot();
};

// Help to cache muzzle name of MagazineSlots
// Size, operator[], Add, Delete, Clear, Resize are used by EntityAI::_magazineSlots

// Besides. There're no guarantee in OFP that an object can't have multiple muzzle using same name
// (this bug can be simply imitated by editing the config TBH)
// However existed codes ignore this case (they check magazineslot one by one and once the muzzle name
// is matched the loop ends) so current design decide to ignore it as well
class MuzzleNameCachedMagazineSlots
{
public:
	int Size() const { return m_array.Size(); }
	const MagazineSlot &operator [] ( int i ) const { return m_array[i]; }

	MagazineSlot &operator [] ( int i ) { invalidateCache(); return m_array[i]; }

	int Add() { invalidateCache(); return m_array.Add(); }
	void Delete( int index, int count=1 ) { invalidateCache(); return m_array.Delete(index, count); }
	void Clear() { invalidateCache(); return m_array.Clear(); }
	void Resize( int n ) { invalidateCache(); return m_array.Resize(n); }

	int IdxOfMuzzle(const RString& muzzle) const;
private:
	void invalidateCache() { m_muzzleName2Idx.clear(); }
private:
    AutoArray<MagazineSlot> m_array;
	struct StrHashCI
	{
		unsigned int operator()(const RString& str) const
		{
			return CalculateStringHashValueCI(str.Data());
		}
	};
	struct StrEqualCI
	{
		bool operator()(const RString& lhs, const RString& rhs) const
		{
			return 0 == stricmp(lhs.Data(), rhs.Data());
		}
	};
	// use RStringB as key. This will save memory but require once extra hash search in StringBank
    std::unordered_map<RStringB, int, StrHashCI, StrEqualCI> m_muzzleName2Idx;
};

struct WeaponCargoItem
{
	WeaponType *weapon;
	int count;
};

struct MagazineCargoItem
{
	MagazineType *magazine;
	int count;
};

typedef AutoArray<WeaponCargoItem> WeaponCargo;
typedef AutoArray<MagazineCargoItem> MagazineCargo;

// different units have access to different sensors

enum CanSee
{
	CanSeeRadar=1,
	CanSeeEye=2,
	CanSeeOptics=4,
	CanSeeEar=8,
	CanSeeCompass=16,
	CanSeeAll=~0
};

// camera parameters
struct ViewPars
{
	float _initAngleY,_minAngleY,_maxAngleY;
	float _initAngleX,_minAngleX,_maxAngleX;
	float _initFov,_minFov,_maxFov;

	void Load( const ParamEntry &cfg );
	void InitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const;
	void LimitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const;
};

class PlateInfo
{
public:
	Vector3 _center;
	Vector3 _normal;
	float _size;
	Offset _face;

public:
	void Init(Shape *shape, Offset face);
};


class PlateInfos
{
protected:
	AutoArray<PlateInfo> _plates[MAX_LOD_LEVELS];
	Ref<Font> _font;
	PackedColor _color;

public:
	void Init(LODShape *shape, RString name, FontID font, PackedColor color);
	void Draw(int level, ClipFlags clipFlags, const FrameBase &pos, const char *text) const;
};

struct ReloadAnimationType
{
	Ref<WeaponType> weapon;
	Ref<AnimationType> animation;
	float multiplier;
};

struct UserTypeAction
{
	RString displayName;
	Vector3 modelPosition;
	float radius;
	RString condition;
	RString statement;
};


//! entity event enum defined using enum factory
#define ENTITY_EVENT_ENUM(type,prefix,XX) \
	XX(type, prefix, Killed) /*object:killer*/ \
	XX(type, prefix, Hit) /*object:causedBy,scalar:howmuch*/ \
	XX(type, prefix, Engine) /*bool:engineState*/ \
	\
	XX(type, prefix, GetIn) /*string:position,object:unit*/ \
	XX(type, prefix, GetOut) /*string:position,object:unit*/ \
	\
	XX(type, prefix, Fired) /*string:weapon,string:muzzle,string:mode,string:ammo*/ \
	XX(type, prefix, IncomingMissile) /*string:ammo,object:whoFired*/ \
	XX(type, prefix, Dammaged) /*string:name,scalar:howmuch*/ \
	XX(type, prefix, Gear) /*bool:gearState*/ \
	XX(type, prefix, Fuel) /*bool:fuelState*/ \
	\
	XX(type, prefix, AnimChanged) /*string:animation*/ \
	\
	XX(type, prefix, Init) /**/ 		XX(type, prefix, AnimDone) /*string:animation -- a move has played to its end (Arma 3)*/

DECLARE_ENUM(EntityEvent,EE,ENTITY_EVENT_ENUM)

//! Information shared between EntityAI objects of the same type

class EntityAIType: public EntityType
{
	typedef EntityType base;

	// information common to all vehicles of the same type
	friend class EntityAI;
	friend class VehicleTypeBank;
	friend class Transport;

	protected:

	HitPointList _hitPoints;

	Ref<Texture> _picture; // UI picture
	Ref<Texture> _icon;		 // UI Map icon

	// side markings
	AnimationSection _unitNumber,_groupSign,_sectorSign,_sideSign;
	AnimationSection _clan;
	AnimationSection _dashboard;

	AnimationSection _backLights;

	RefArray<WeaponType> _weapons;					// weapons (types)
	RefArray<MagazineType> _magazines;			// magazines (types)

	PlateInfos _squadTitles;

	AutoArray<ReloadAnimationType> _reloadAnimations;
	RString _eventHandlers[NEntityEvent];

	public: // tired of writing access functions
	// all attributes made public
	bool _shapeReversed;

	bool _forceSupply;
	bool _showWeaponCargo;

	float _camouflage; // some targets are very difficult to see
	float _audible; // audible/visible ratio (0..1)

	float _spotableNightLightsOff; // night spotability coeficients
	float _spotableNightLightsOn;

	float _visibleNightLightsOff; // night target recognition
	float _visibleNightLightsOn;

	int _commanderCanSee;
	int _driverCanSee;
	int _gunnerCanSee;

	LocalizedString _displayName; // resolves $STR_ against the *current* language (map/editor object names)
	RString _nameSound;
	TargetSide _typicalSide; // which side uses it normally
	DestructType _destrType;
	// typical destruction type
	// DestructDefault means autodetect

	int _weaponSlots;

	Vector3 _supplyPoint;
	float _supplyRadius;
	Vector3 _extCameraPosition;
	Vector3 _extCameraUp;

	float _armor;
	float _invArmor;
	float _logArmor;
	float _cost;
	float _fuelCapacity;

	float _structuralDammageCoef;

	float _secondaryExplosion;

	float _sensitivity; // sensitivity compared to man
	float _sensitivityEar; // sensitivity compared to man

	float _brakeDistance;
	float _precision;
	float _formationX,_formationZ;
	float _formationTime;
	float _invFormationTime;

	float _steerAheadSimul;
	float _steerAheadPlan;

	float _predictTurnSimul;
	float _predictTurnPlan;

	float _minFireTime;

	float _irScanRangeMin; //! min. IR scanner range
	float _irScanRangeMax; //! max. IR scanner range
	float _irScanToEyeFactor; //! ratio of IR scanner range to eye distance

	bool _laserScanner; // equipped with lasser scanner?

	bool _irTarget; // is IR lock possible?
	bool _laserTarget; // is laser lock possible (used only for virtual laser target)

	bool _irScanGround; // IR capable of tracking ground targets

	bool _attendant;
	bool _nightVision;

	bool _preferRoads;
	bool _hideUnitInfo;

	ViewPars _viewPilot;

	// other properties
	UnitInfoType _unitInfoType;

	float _maxFuelCargo;
	float _maxRepairCargo;
	float _maxAmmoCargo;
	int _maxWeaponsCargo;
	int _maxMagazinesCargo;
	WeaponCargo _weaponCargo;
	MagazineCargo _magazineCargo;
	VehicleKind _kind;
	Threat _threat;

	float _minCost;
	float _maxSpeed; // max speed - level road

	SoundPars _mainSound;
	SoundPars _envSound;
	SoundPars _dmgSound;
	SoundPars _crashSound;
	SoundPars _getInSound,_getOutSound;
	SoundPars _servoSound;
	SoundPars _landCrashSound;
	SoundPars _waterCrashSound;
	AutoArray<ExtSoundInfo> _extEnvSounds;

	AutoArray<ReflectorInfo> _reflectors;

	// LGT-AUTO-001: light CAST by the tail / brake lamps of models that only have the glowing
	// "zadni svetlo" / "brzdove svetlo" faces (every OFP / CWA vehicle). Worked out once per
	// type from LOD 0 (VehicleAutoLights.cpp); the faces themselves are untouched.
	mutable bool _autoLightsReady = false;
	mutable bool _autoTailCasts = false; //!< false when the model already has a rear Reflector
	mutable AutoArray<Vector3> _autoTailPos;
	mutable AutoArray<Vector3> _autoBrakePos;
	void PrepareAutoLights() const;

	AutoArray<LightInfo> _lights;
	AnimationSection _showDmg;
	Vector3 _showDmgPoint;

	AutoArray<AnimationSection> _hiddenSelections;
	AutoArray<RString> _hiddenSelectionsTextureNames; // Arma: default texture per hidden selection
	AutoArray<RString> _hiddenSelectionsMaterialNames; // Arma: default material (.rvmat) per hidden selection

	AutoArray<UserTypeAction> _userTypeActions;

	public:
	EntityAIType( const ParamEntry *param );
	~EntityAIType() override;

	void Load(const ParamEntry &par) override;

	float GetCamouflage() const {return _camouflage;}
	float GetAudible() const {return _audible;}

	float GetSpotableNightLightsOff() const {return _spotableNightLightsOff;}
	float GetSpotableNightLightsOn() const {return _spotableNightLightsOn;}

	float GetVisibleNightLightsOff() const {return _visibleNightLightsOff;}
	float GetVisibleNightLightsOn() const {return _visibleNightLightsOn;}

	TargetSide GetTypicalSide() const {return _typicalSide;}
	DestructType GetDestructType() const {return _destrType;}

	void InitShape() override; // after shape is loaded
	void DeinitShape() override; // before shape is unloaded

	__forceinline float GetCost() const {return _cost;}
	__forceinline float GetArmor() const {return _armor;}
	__forceinline float GetInvArmor() const {return _invArmor;}
	__forceinline float GetLogArmor() const {return _logArmor;}
	__forceinline float GetFuelCapacity() const {return _fuelCapacity;}

	__forceinline bool GetIRTarget() const {return _irTarget;} // is IR lock possible?
	__forceinline bool GetLaserTarget() const {return _laserTarget;} // is laser lock possible?
	float GetIRScanRange() const;
	__forceinline bool GetLaserScanner() const {return _laserScanner;}
	__forceinline bool GetIRScanGround() const {return _irScanGround;} // IR tracks ground targets

	__forceinline bool GetNightVision() const {return _nightVision;}

	__forceinline bool PreferRoads() const {return _preferRoads;}
	__forceinline UnitInfoType GetUnitInfoType() const {return _unitInfoType;}

	__forceinline float GetMaxSpeed() const {return _maxSpeed;}
	float GetTypSpeed() const {return _maxSpeed*0.66f;}

	float GetMaxSpeedMs() const {return _maxSpeed*(1.0f/3.6f);}
	float GetTypSpeedMs() const {return _maxSpeed*(1.0f/3.6f)*0.66f;}

	__forceinline float GetMinCost() const {return _minCost;}

	__forceinline float GetSteerAheadSimul() const {return _steerAheadSimul;}
	__forceinline float GetSteerAheadPlan() const {return _steerAheadPlan;}

	__forceinline float GetPredictTurnSimul() const {return _predictTurnSimul;}
	__forceinline float GetPredictTurnPlan() const {return _predictTurnPlan;}

	__forceinline float GetMinFireTime() const {return _minFireTime;}

	virtual bool HasDriver() const {return true;}
	virtual bool HasGunner() const {return false;}
	virtual bool HasCommander() const {return false;}
	virtual bool HasCargo() const {return false;}

	__forceinline float GetBrakeDistance() const {return _brakeDistance;}
	__forceinline float GetPrecision() const {return _precision;}
	__forceinline float GetFormationX() const {return _formationX;}
	__forceinline float GetFormationZ() const {return _formationZ;}
	__forceinline float GetFormationTime() const {return _formationTime;}
	__forceinline float GetInvFormationTime() const {return _invFormationTime;}

	__forceinline Vector3Val GetSupplyPoint() const {return _supplyPoint;}
	__forceinline float GetSupplyRadius() const {return _supplyRadius;}

	__forceinline float GetMaxFuelCargo() const {return _maxFuelCargo;}
	__forceinline float GetMaxRepairCargo() const {return _maxRepairCargo;}
	__forceinline float GetMaxAmmoCargo() const {return _maxAmmoCargo;}
	__forceinline int GetMaxWeaponsCargo() const {return _maxWeaponsCargo;}
	__forceinline int GetMaxMagazinesCargo() const {return _maxMagazinesCargo;}

	__forceinline const WeaponCargo &GetWeaponCargo() const {return _weaponCargo;}
	__forceinline const MagazineCargo &GetMagazineCargo() const {return _magazineCargo;}
	__forceinline bool IsAttendant() const {return _attendant;}

	__forceinline VehicleKind GetKind() const {return _kind;}
	__forceinline Threat GetThreat() const {return _threat;}
	virtual Threat GetDammagePerMinute
	(
		float distance2, float visibility, EntityAI *vehicle=nullptr
	) const;
	virtual Threat GetStrategicThreat( float distance2, float visibility, float cosAngle ) const;

	__forceinline float GetStructuralDammageCoef() const {return _structuralDammageCoef;}
	__forceinline const HitPointList &GetHitPoints() const {return _hitPoints;}
	__forceinline HitPointList &GetHitPoints() {return _hitPoints;}

	__forceinline const SoundPars &GetMainSound() const {return _mainSound;}
	__forceinline const SoundPars &GetEnvSound() const {return _envSound;}
	__forceinline const SoundPars &GetDmgSound() const {return _dmgSound;}
	__forceinline const SoundPars &GetCrashSound() const {return _crashSound;}
	__forceinline const SoundPars &GetGetInSound() const {return _getInSound;}
	__forceinline const SoundPars &GetGetOutSound() const {return _getOutSound;}
	__forceinline const SoundPars &GetServoSound() const {return _servoSound;}
	__forceinline const SoundPars &GetLandCrashSound() const {return _landCrashSound;}
	__forceinline const SoundPars &GetWaterCrashSound() const {return _waterCrashSound;}
	const SoundPars &GetEnvSoundExt(RStringB name) const;
	const SoundPars &GetEnvSoundExtRandom(RStringB name) const;

	RStringB GetDisplayName() const {return _displayName;}
	__forceinline const RString &GetNameSound() const {return _nameSound;}

	__forceinline Texture *GetIcon() const {return _icon;}

	// weapons and magazines
	int NWeaponSystems() const {return _weapons.Size();}
	const WeaponType *GetWeaponSystem(int i) const {return _weapons[i];}
	int NMagazines() const {return _magazines.Size();}
	const MagazineType *GetMagazine(int i) const {return _magazines[i];}

	int AddWeapon(RStringB name);
	int AddMagazine(RStringB name);

	protected:
	void AddHitPoints(const ParamEntry &par);

};

typedef EntityAIType VehicleType;

LSError Serialize(ParamArchive &ar, RString name, const EntityType * &value, int minVersion);
LSError Serialize(ParamArchive &ar, RString name, const EntityType * &value, int minVersion, const EntityType *defValue);

LSError Serialize(ParamArchive &ar, RString name, const EntityAIType * &value, int minVersion);
LSError Serialize(ParamArchive &ar, RString name, const EntityAIType * &value, int minVersion, const EntityAIType *defValue);

const float MaxDammageWorking=0.75; // can move/attack

}  // namespace Poseidon
