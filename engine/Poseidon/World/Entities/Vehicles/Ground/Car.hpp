#pragma once

#include <Poseidon/World/Entities/Vehicles/TankOrCar.hpp>

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>

#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>

#include <Poseidon/World/Entities/Infantry/Wounds.hpp>
#include <Poseidon/World/Weather/MudWheelTrail.hpp>

namespace Poseidon
{
enum CarWheel
{
	FrontWheels,
	FLWheel=FrontWheels,FRWheel,
	FL2Wheel,FR2Wheel,
	BackWheels,
	MLWheel=BackWheels,MRWheel,
	BLWheel,BRWheel,
	MaxWheels,
	NoWheel=MaxWheels
};


struct ScudProxy
{
	LLink<Object> obj;
	int selection; // copied over from proxyObject

	ScudProxy() {selection = -1;}
	bool IsPresent() const {return selection >= 0;}
};

class CarType: public TankOrCarType
{
	typedef TankOrCarType base;
	friend class Car;

	protected:
	
	Matrix4 _toWheelAxis; // transformation
	AnimationWithCenter _drivingWheel;

	Vector3 _steeringPoint;
	float _wheelCircumference;
	float _turnCoef;
	float _terrainCoef;

	float _damperSize;
	float _damperForce;

	AnimationAnimatedTexture _animFire;
	TurretType _turret;
	bool _hasTurret;

	AnimationWithCenter _frontLeftWheel,_frontRightWheel;
	AnimationWithCenter _front2LeftWheel,_front2RightWheel;
	AnimationWithCenter _midLeftWheel,_midRightWheel;
	AnimationWithCenter _backLeftWheel,_backRightWheel;
	Buffer<CarWheel> _whichWheelContact; // conversion from landcontact
	Buffer<CarWheel> _whichWheelGeometry; // conversion from landcontact

	WoundTextureSelections _glassDammageHalf;
	WoundTextureSelections _glassDammageFull;

	AnimationWithCenter *_wheels[MaxWheels];

	PlateInfos _plateInfos;

	HitPoint _glassRHit;
	HitPoint _glassLHit;
	HitPoint _bodyHit;
	HitPoint _fuelHit;

	HitPoint _wheelLFHit;
	HitPoint _wheelRFHit;

	HitPoint _wheelLF2Hit;
	HitPoint _wheelRF2Hit;

	HitPoint _wheelLMHit;
	HitPoint _wheelRMHit;

	HitPoint _wheelLBHit;
	HitPoint _wheelRBHit;

	Ref<WeightInfo> _weights; // rt animation weighting info	
	Ref<Skeleton> _skeleton;

	ScudProxy _proxies[MAX_LOD_LEVELS];

	Ref<AnimationRT> _scudLaunch;
	Ref<AnimationRT> _scudStart;

	SoundPars _scudSound;
	SoundPars _scudSoundElevate;

	Ref<LODShapeWithShadow> _scudModel;
	Ref<LODShapeWithShadow> _scudModelFire;

	void ScanWheels(Buffer<CarWheel> &buffer, int contactLevel);

	public:
	CarType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class Car: public TankOrCar
{
	typedef TankOrCar base;

	protected:
	Link<IWave> _hornSound;
	Ref<IWave> _scudSound;

	WeaponCloudsSource _scudSmoke;
	Vector3 _scudPos;
	Vector3 _scudSpeed;

	Turret _turret;

	WeaponLightSource _mGunFire;
	int _mGunFireFrames;
	Foundation::UITime _mGunFireTime;
	int _mGunFirePhase;
	WeaponCloudsSource _mGunClouds;

	RString _plateNumber;
	
	float _wheelPhase;
	float _dampers[MaxWheels];
	MudWheelTrail _mudWheels[MaxWheels];
	
	float _reverseTimeLeft; // force reverse state
	float _forwardTimeLeft; // force forward state

	TrackOptimizedFour _track;
	
	float _thrustWanted,_thrust;
	float _turnWanted,_turn;
	float _turnIncreaseSpeed,_turnDecreaseSpeed;
	

	float _scudState;
	// 0..1 = init
	// 1..2 = launching
	// 2..3 = ready
	// 3..4 = starting 

	public:
	Car( VehicleType *name, Person *driver );

	const CarType *Type() const
	{
		return static_cast<const CarType *>(GetType());
	}

	Vector3 Friction( Vector3Par speed );

	float Rigid() const override {return 0.3;} // how much energy is transfered in collision
	bool HasTurret() const {return Type()->_hasTurret;}
	bool IsBlocked() const;

	float GetScudState() const {return _scudState;}

	void MoveWeapons(float deltaT);
	void Simulate( float deltaT, SimulationImportance prec ) override;
	Matrix4 InsideCamera( CameraType camType ) const override;
	Vector3 ExternalCameraPosition( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	bool IsGunner( CameraType camType ) const override;
	bool IsContinuous( CameraType camType ) const override;
	bool HasFlares( CameraType camType ) const override;
	void SimulateHUD(CameraType camType,float deltaT) override;
	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;
	void DrawProxies
	(
		int level, ClipFlags clipFlags,
		const Matrix4 &transform, const Matrix4 &invTransform,
		float dist2, float z2, const LightList &lights
	) override;
	void AnimateMatrix(Matrix4 &mat, int level, int selection) const override;

	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;
	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;

	Matrix4 TurretTransform() const;
	Matrix4 GunTurretTransform() const;

	float OutsideCameraDistance( CameraType camType ) const override {return 20;}

	float GetGlassBroken() const;

	void DammageAnimation( int level );
	void DammageDeanimation( int level );

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	Vector3 AnimatePoint( int level, int index ) const override;

	RString GetPlateNumber() const override {return _plateNumber;}
	void SetPlateNumber( RString plate ) override {_plateNumber=plate;}

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	bool IsAbleToMove() const override;
	bool IsPossibleToGetIn() const override;
	bool IsCautious() const override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	float Thrust() const override {return fabs(_thrust);}
	float ThrustWanted() const override {return fabs(_thrustWanted);}

	float TrackingSpeed() const override {return 80;}

	void PerformFF(FFEffects &eff) override;
	void Eject(AIUnit *unit) override;
	void JoystickPilot( float deltaT );
	void SuspendedPilot(AIUnit *unit, float deltaT ) override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void FakePilot( float deltaT ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override;

	RString DiagText() const override;

	float GetPathCost( const GeographyInfo &info, float dist ) const override;
	void FillPathCost( Path &path ) const override;

	float GetFieldCost( const GeographyInfo &info ) const override;
	float GetCost( const GeographyInfo &info ) const override;
	float GetCostTurn( int difDir ) const override;
	// TW-WATER W6g: an amphibious car may plan through water fields (as TankWithAI does)
	float GetTypeCost(OperItemType type) const override;

	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	Vector3 GetCameraDirection( CameraType camType ) const override;
	void LimitCursor( CameraType camType, Vector3 &dir ) const override;

	bool AimWeapon( int weapon, Vector3Par direction ) override;
	bool AimWeapon( int weapon, Target *target ) override;
	Vector3 GetWeaponDirection( int weapon ) const override;
	Vector3 GetWeaponCenter( int weapon ) const override;
	Vector3 GetWeaponDirectionWanted( int weapon ) const override;

	LSError Serialize(ParamArchive &ar) override;
	
	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;
};

}  // namespace Poseidon
