#pragma once

#include <Poseidon/World/Entities/Vehicles/TankOrCar.hpp>
#include <Poseidon/World/Weather/MudWheelTrail.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>

#include <Poseidon/IO/ParamFile/ParamFile.hpp>

namespace Poseidon
{
class TankType: public TankOrCarType
{
	typedef TankOrCarType base;
	friend class Tank;
	friend class TankWithAI;

	protected:
	Point3 _leftLightPos,_rightLightPos;
	Vector3 _leftLightDir,_rightLightDir;
	
	Point3 _gunnerPilotPos; // camera position - gunner view

	Point3 _missilePos,_missileDir; // missile position and direction
	//Point3 _shellPos,_shellDir; // shell position and direction
	Point3 _gunPos,_gunDir; // to do: gun position and direction

	Vector3 _cargoLightPos;	// light in cargo

	TurretType _mainTurret;
	TurretType _comTurret;

	Indicator _radarIndicator;
	Indicator _turretIndicator;
	IndicatorWatch _watch;
	

	// some tanks have observer turret mounted on the main turret
	// while some not
	bool _comTurretOnMainTurret;

	AnimationUV _leftOffset,_rightOffset;
	//Ref<Texture> _trackStill,_trackMove;
	//Animation _hatch, _hatch2;
	Hatch _hatchDriver, _hatchCommander, _hatchGunner;

	AnimationAnimatedTexture _animFire;

	// wheels movement animation
	// some wheels move up/down
	// all rotate around the X-axis
	AutoArray<AnimationWithCenter> _wheelsRotL;
	AutoArray<AnimationWithCenter> _wheelsRotR;
	AutoArray<AnimationWithCenter> _wheelsUpDownL;
	AutoArray<AnimationWithCenter> _wheelsUpDownR;
	AutoArray<AnimationWithCenter> _tracksUpDownL;
	AutoArray<AnimationWithCenter> _tracksUpDownR;

	HitPoint _hullHit;
	HitPoint _trackLHit,_trackRHit;
	HitPoint _turretHit,_gunHit;

	public:
	
	TankType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class Tank: public TankOrCar
{
	typedef TankOrCar base;
	// basic catterpillar vehicle (tank/APC) simulation
	protected:
	
	float _randFrequency;

	bool _forwardUsedAsBrake:1;
	bool _backwardUsedAsBrake:1;
	bool _parkingBrake:1;

	bool _doGearSound:1;

	WaterSource _leftWater,_rightWater;
	DustSource _fireDust;
	float _invFireDustTimeTotal;
	float _fireDustTimeLeft;
		
	WeaponFireSource _gunFire;
	WeaponCloudsSource _gunClouds;

	WeaponLightSource _mGunFire;
	int _mGunFireFrames;
	Foundation::UITime _mGunFireTime;
	int _mGunFirePhase;
	WeaponCloudsSource _mGunClouds;

	float _phaseL,_phaseR; // texture animation
	
	TrackOptimized _track;
	// (MUD-RUNNER) ruts behind each track, sampled by distance like a wheel's
	MudWheelTrail _mudTrackL, _mudTrackR;

	Turret _mainTurret;
	Turret _comTurret;

	Ref<LightPointOnVehicle> _cargoLight;

	// thrust is relative, in range (-1.0,1.0)
	float _thrustLWanted,_thrustRWanted; // accelerate (left), accelerate (right)
	float _thrustL,_thrustR; // accelerate (left), accelerate (right)

	public:
	Tank( VehicleType *name, Person *driver );

	const TankType *Type() const
	{
		return static_cast<const TankType *>(GetType());
	}

	void ObjectContact
	(
		Frame &moveTrans,
		Vector3Par wCenter,
		float deltaT,
		Vector3 &torque, Vector3 &friction, Vector3 &torqueFriction,
		Vector3 &totForce, float &crash

	);
	void LandFriction
	(
		Vector3 &friction, Vector3 &torqueFriction,
		Vector3 &torque,
		bool brakeFriction, Vector3Par fSpeed, Vector3Par speed,
		Vector3Par pCenter,
		float coefNPoints, Texture *texture
	);

	
	Vector3 Friction( Vector3Par speed );

	void MoveWeapons(float deltaT);

	bool IsAbleToMove() const override;
	bool IsAbleToFire() const override;
	bool IsPossibleToGetIn() const override;

	void StabilizeTurrets
	(
		Matrix3Val oldTrans, Matrix3Val newTrans,
		Matrix3Val oldTurretTrans
	);

	bool UseSimpleSimulation(SimulationImportance prec) const;

	void SimulateOptimized( float deltaT, SimulationImportance prec ) override;
	void PlaceOnSurface(Matrix4 &trans) override;
	void Simulate( float deltaT, SimulationImportance prec ) override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	void PerformFF( FFEffects &effects ) override;

	float GetHitForDisplay(int kind) const override;

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	bool IsTurret( CameraType camType ) const override;
	bool HasFlares( CameraType camType ) const override;

	void InitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const override;
	void LimitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const override;
	void LimitCursorHard(CameraType camType, Vector3 &dir) const override;

	float OutsideCameraDistance( CameraType camType ) const override {return 25;}
	Matrix4 TurretTransform() const;
	Matrix4 GunTurretTransform() const;
	Matrix4 ObsTransform() const;
	Matrix4 ObsGunTurretTransform() const;

	bool AnimateTexture
	(
		int level,
		float phaseL, float phaseR, float speedL, float speedR
	);
	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	Vector3 AnimatePoint( int level, int index ) const override;
	void AnimateMatrix(Matrix4 &mat, int level, int selection) const override;

	RString DiagText() const override;

	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;
	#if ALPHA_SPLIT
	void DrawAlpha( int level, ClipFlags clipFlags, const FrameBase &pos );
	#endif

	float Thrust() const override {return (fabs(_thrustL)+fabs(_thrustR))*0.5;}
	float ThrustWanted() const override {return (fabs(_thrustLWanted)+fabs(_thrustRWanted))*0.5;}

	float TrackingSpeed() const override {return 80;}

	// all tank weapons are linked
	// special case is guided missile, which is linked, but have weapon lock
	bool CalculateAimWeapon( int weapon, Vector3 &dir, Target *target ) override;
	bool AimWeapon(int weapon, Vector3Par direction) override;
	bool AimWeapon(int weapon, Target *target) override;
	bool CalculateAimObserver(Vector3 &dir, Target *target) override;
	bool AimObserver(Vector3Val dir) override;

	float GetAimed( int weapon, Target *target ) const override;
	Vector3 GetWeaponDirection( int weapon ) const override;
	Vector3 GetWeaponCenter( int weapon ) const override;

	Vector3 GetEyeDirection() const override;

	Vector3 GetWeaponDirectionWanted( int weapon ) const override;
	Vector3 GetEyeDirectionWanted() const override;

	Vector3P GetTurretAbsDirection() const;

	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	void Eject(AIUnit *unit) override;
	void JoystickPilot( float deltaT );
	void SuspendedPilot(AIUnit *unit, float deltaT ) override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void FakePilot( float deltaT ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override{}

	bool HasHUD() const override {return true;}
};

class TankWithAI: public Tank
{
	typedef Tank base;

	public:
	TankWithAI( VehicleType *name, Person *driver );
	~TankWithAI() override;

	float GetFieldCost( const GeographyInfo &info ) const override;
	float GetCost( const GeographyInfo &info ) const override;
	float GetCostTurn( int difDir ) const override;
	float GetTypeCost(OperItemType type) const override;

	float FireAngleInRange( int weapon, Vector3Par rel ) const override;
	float FireInRange( int weapon, float &timeToAim, const Target &target ) const override;

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override;
	void AIGunner(AIUnit *unit, float deltaT ) override;

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
