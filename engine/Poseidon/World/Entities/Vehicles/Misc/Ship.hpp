#pragma once

#include <Poseidon/World/Entities/Vehicles/Tracks.hpp>
#include <Poseidon/World/Entities/Vehicles/GearBox.hpp>

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>

#include <Poseidon/IO/ParamFile/ParamFile.hpp>

namespace Poseidon
{
class ShipType: public TransportType
{
	typedef TransportType base;
	friend class Ship;
	friend class ShipWithAI;

	protected:
	Point3 _pilotPos; // camera position	
	Point3 _gunnerPilotPos; // camera position - gunner view
	Point3 _commanderPilotPos;

	Point3 _gunPos,_gunDir;

	AnimationAnimatedTexture _animFire;
	TurretType _turret;

	Matrix4 _toWheelAxis; // transformation
	AnimationWithCenter _drivingWheel;

	Matrix4 _toIndicatorSpeedAxis; // transformation
	AnimationWithCenter _indicatorSpeed;

	AnimationRotation _radar;

	HitPoint _turretHit,_gunHit;

	public:
	ShipType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class Ship: public Transport
{
	typedef Transport base;
	protected:
	Vector3 _lastAngVelocity;
	float _randFrequency;

	bool _pilotBrake:1;
	bool _targetOutOfAim:1;

	Turret _turret;

	WeaponLightSource _mGunFire;
	int _mGunFireFrames;
	Foundation::UITime _mGunFireTime;
	int _mGunFirePhase;
	WeaponCloudsSource _mGunClouds;

	WaterSource _leftEngine,_rightEngine;
	WaterSource _leftWater,_rightWater;
		
	float _thrustLWanted,_thrustRWanted; // accelerate (left), accelerate (right)
	float _thrustL,_thrustR; // accelerate (left), accelerate (right)

	float _sink; // current sink status

	public:
	Ship( VehicleType *name, Person *driver );

	const ShipType *Type() const
	{
		return static_cast<const ShipType *>(GetType());
	}
	
	Vector3 DragFriction( Vector3Par speed );
	Vector3 DragForce( Vector3Par speed );

	void MoveWeapons(float deltaT);
	float Rigid() const override {return 0.1;}

	void Simulate( float deltaT, SimulationImportance prec ) override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	Matrix4 InsideCamera( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	bool IsVirtual( CameraType camType ) const override;
	void InitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const override;
	void LimitVirtual
	(
		CameraType camType, float &heading, float &dive, float &fov
	) const override;

	float OutsideCameraDistance( CameraType camType ) const override
	{
		return floatMax(20,GetShape()->GeometrySphere()*2);
	}
	Matrix4 TurretTransform() const;
	Matrix4 GunTurretTransform() const;

	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;

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

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	float TrackingSpeed() const override {return 80;}

	// all tank weapons are linked
	// special case is guided missile, which is linked, but have weapon lock
	bool AimWeapon( int weapon, Vector3Par direction ) override;
	bool AimWeapon( int weapon, Target *target ) override;
	float GetAimed( int weapon, Target *target ) const override;
	Vector3 GetWeaponDirection( int weapon ) const override;
	Vector3 GetWeaponCenter( int weapon ) const override;
	
	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	void Eject(AIUnit *unit) override;

	Vector3 GetDriverGetInPos(Person *person, Vector3Par pos) const override;
	Vector3 GetCommanderGetInPos(Person *person, Vector3Par pos) const override;
	Vector3 GetGunnerGetInPos(Person *person, Vector3Par pos) const override;
	Vector3 GetCargoGetInPos(Person *person, Vector3Par pos) const override;
	Vector3 GetCargoGetOutPos(Person *person) const override;
	// boats in deep water: board / leave right beside the hull (swimmers cannot drown)
	Vector3 PlaceGetInPos(Person *person, Vector3Par memPos, Vector3Par upos) const;

	void SuspendedPilot(AIUnit *unit, float deltaT ) override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void JoystickPilot( float deltaT );
	void FakePilot( float deltaT ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override{}

	bool HasHUD() const override {return true;}

	void ResetStatus() override;

	LSError Serialize(ParamArchive &ar) override;

	// TW-WATER W6: the engines' thrust (0..1) for the renderer's wake (PresentationSnapshot)
	float WakeThrust() const {return 0.5f*(fabs(_thrustL)+fabs(_thrustR));}
};

class ShipWithAI: public Ship
{
	typedef Ship base;

public:
	enum StopState
	{
		SSNone,
		SSFindPath,
		SSMove,
		SSStop
	};

protected:
	Vector3 _stopPosition;
	StopState _stopState;

public:
	ShipWithAI( VehicleType *name, Person *driver );
	~ShipWithAI() override;

	float GetFieldCost( const GeographyInfo &info ) const override;
	float GetCost( const GeographyInfo &info ) const override;
	float GetCostTurn( int difDir ) const override;
	float FireInRange( int weapon, float &timeToAim, const Target &target ) const override;

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void AIGunner(AIUnit *unit, float deltaT ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override;

	Vector3 GetStopPosition() const override {return _stopPosition;}

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;

protected:
	void FindStopPosition() override;
	void Autopilot(AIUnit *unit, float &speedWanted, float &headChange, float &turnPredict);
};

}  // namespace Poseidon
