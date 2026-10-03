#pragma once

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>

#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>


namespace Poseidon
{
class ParachuteType: public TransportType
{
	typedef TransportType base;
	friend class Parachute;
	friend class ParachuteAuto;

	protected:
	Vector3 _pilotPos; // neutral neck and head positions

	Ref<WeightInfo> _weights;
	Ref<Skeleton> _skeleton;

	Ref<AnimationRT> _open;
	Ref<AnimationRT> _drop;
	
	public:
	ParachuteType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
	void DeinitShape() override;
};

class Parachute: public Transport
{
	typedef Transport base;

	protected:

	float _backRotor,_backRotorWanted;

	Vector3 _turbulence;
	Foundation::Time _lastTurbulenceTime;

	float _openState;
	// 0..1 = closed (init)<1
	// 1..2  = opening
	// 2..3  = dropping

	public:
	Parachute( VehicleType *name, Person *pilot );
	~Parachute() override;
	
	const ParachuteType *Type() const
	{
		return static_cast<const ParachuteType *>(GetType());
	}

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;
	bool IsAway(float factor) override;

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	float Rigid() const override {return 0.1;} // how much energy is transfered in collision

	bool IsVirtual( CameraType camType ) const override {return true;}
	bool HasFlares( CameraType camType ) const override;

	Matrix4 InsideCamera( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	void InitVirtual( CameraType camType, float &heading, float &dive, float &fov ) const override;
	void LimitVirtual( CameraType camType, float &heading, float &dive, float &fov ) const override;
	float TrackingSpeed() const override {return 100;}

	float GetCombatHeight() const override {return 30;}
	float GetMinCombatHeight() const override {return 30;}
	float GetMaxCombatHeight() const override {return 100;}

	SimulationImportance WorstImportance() const override {return SimulateVisibleFar;}
	SimulationImportance BestImportance() const override {return SimulateCamera;}

	bool IsAbleToMove() const override;
	bool IsPossibleToGetIn() const override;
	bool Airborne() const override;

	// the pilot can be shot even if the model's fire geometry leaves him out
	void IntersectExtra(const FrameBase &pos, CollisionBuffer &result,
		Vector3Par beg, Vector3Par end, float radius,
		ObjIntersect type, int hierLevel) const override;
	// AI aims at the pilot, not the canopy
	Vector3 AimingPosition() const override;

	LSError Serialize(ParamArchive &ar) override;
};

class ParachuteAuto: public Parachute
{
	typedef Parachute base;

	protected:
	float _pilotHeading;
	float _dirCompensate;  // how much we compensate for estimated change
	// esp. when landing

	// helpers for keyboard pilot
	bool _pilotHelper; // keyboard helper activated
	bool _targetOutOfAim;

	// non-helper interface (joystrick)
	Vector3 _lastAngVelocity; // helper for prediction

	public:
	ParachuteAuto( VehicleType *name, Person *pilot );

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void AvoidGround( float minHeight );

	void Eject(AIUnit *unit) override;

	float MakeAirborne() override;

	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	float FireValidTime() const override {return 45;}

	void SuspendedPilot(AIUnit *unit, float deltaT ) override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void JoystickPilot( float deltaT );
	void FakePilot( float deltaT ) override;

	// AI interface
	Matrix4 GunTransform() const;

	bool AimWeapon(int weapon, Vector3Par direction ) override;
	bool AimWeapon(int weapon, Target *target ) override;
	Vector3 GetWeaponDirection( int weapon ) const override;
	float GetAimed( int weapon, Target *target ) const override;

	float GetFieldCost( const GeographyInfo &info ) const override;
	float GetCost( const GeographyInfo &info ) const override;
	float GetCostTurn( int difDir ) const override;

	float FireInRange( int weapon, float &timeToAim, const Target &target ) const override;
	float FireAngleInRange( int weapon, Vector3Par rel ) const override;

	void MoveWeapons( float deltaT );

	void AIGunner(AIUnit *unit, float deltaT ) override;
	void AIPilot(AIUnit *unit, float deltaT ) override;

	void DrawDiags() override;
	RString DiagText() const override;

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
