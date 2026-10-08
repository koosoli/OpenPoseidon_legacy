#pragma once

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Indicator.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>


namespace Poseidon
{
class AirplaneType: public TransportType
{
	typedef TransportType base;

	friend class Airplane;
	friend class AirplaneAuto;

	protected:
	Vector3 _pilotPos; // neutral neck and head positions
	Vector3 _gunnerPos; // current head position

	Vector3 _rocketLPos,_rocketRPos;

	Vector3 _gunPos,_gunDir; // gun position and direction

	Vector3 _ejectSpeed;

	AnimationAnimatedTexture _animFire;

	AnimationRotation _rFlap,_lFlap;
	AnimationRotation _rAileronT,_lAileronT;
	AnimationRotation _rAileronB,_lAileronB;
	AnimationRotation _rRudder,_lRudder;
	AnimationRotation _rElevator,_lElevator;

	enum {MaxRotors=8};
	AnimationRotation _rotors[MaxRotors];
	AnimationSection _rotorStill, _rotorMove;

	AnimationWithCenter _fWheel,_rWheel,_lWheel;

	Indicator _altRadarIndicator;
	Indicator _altRadarIndicator2;
	Indicator _altBaroIndicator;
	Indicator _speedIndicator;
	Indicator _vertSpeedIndicator;
	Indicator _vertSpeedIndicator2;
	Indicator _rpmIndicator;
	Indicator _compass, _compass2;
	IndicatorWatch _watch, _watch2;
	AnimationWithCenter _vario;
	Vector3 _varioDirection[MAX_LOD_LEVELS];

	Vector3 _lDust,_rDust;

	float _minGunTurn,_maxGunTurn; // gun movement
	float _minGunElev,_maxGunElev;
	bool _gearRetracting;
	float _landingSpeed;
	float _takeOffSpeed;
	float _stallSpeed;
	float _aileronSensitivity;
	float _elevatorSensitivity;
	float _wheelSteeringSensitivity;
	float _noseDownCoef;
	float _landingAoa;
	float _flapsFrictionCoef;
	
	public:
	AirplaneType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class Airplane: public Transport
{
	typedef Transport base;

	protected:
	Ref<IWave> _servoSound;

	bool _pilotGear;

	bool _rocketLRToggle;
	bool _gearDammage;

	int _pilotFlaps;

	float _pilotBrake;

	float _rndFrequency; // volume control - low down hero sound
	float _servoVol;

	float _gunYRot,_gunYRotWanted;
	float _gunXRot,_gunXRotWanted;
	float _gunXSpeed,_gunYSpeed;

	float _rpm; // on/off
	float _thrust,_thrustWanted; // turning motor on/off
	float _rotorSpeed;
	float _rotorPosition;
	float _elevator,_elevatorWanted;
	float _rudder,_rudderWanted;
	float _aileron,_aileronWanted;

	float _flaps; // actual flap position
	float _gearsUp; // actual gear position
	float _brake;

	DustSource _leftDust,_rightDust;

	Vector3 _lastAngVelocity; // helper for prediction

	enum SweepState {SweepDisengage,SweepTurn,SweepEngage,SweepFire};
	SweepState _sweepState;
	Foundation::Time _sweepDelay;
	LinkTarget _sweepTarget;
	Vector3 _sweepDir;

	WeaponLightSource _mGunFire;
	int _mGunFireFrames;
	Foundation::UITime _mGunFireTime;
	int _mGunFirePhase;
	WeaponCloudsSource _mGunClouds;

	public:
	Airplane( VehicleType *name, Person *pilot );
	~Airplane() override;
	
	const AirplaneType *Type() const
	{
		return static_cast<const AirplaneType *>(GetType());
	}

	bool IsAnimated( int level ) const override;
	bool IsAnimatedShadow( int level ) const override;
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	bool CastProxyShadow(int level, int index) const override;
	int GetProxyComplexity
	(
		int level, const FrameBase &pos, float dist2
	) const override;
	void DrawProxies
	(
		int level, ClipFlags clipFlags,
		const Matrix4 &transform, const Matrix4 &invTransform,
		float dist2, float z2, const LightList &lights
	) override;
	Object *GetProxy
	(
		LODShapeWithShadow *&shape,
		int level,
		Matrix4 &transform, Matrix4 &invTransform,
		const FrameBase &parentPos, int i
	) const override;

	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;

	float DetectStall() const;
	void PerformFF( FFEffects &effects ) override;
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	Matrix4 InsideCamera( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	void InitVirtual( CameraType camType, float &heading, float &dive, float &fov ) const override;
	void LimitVirtual( CameraType camType, float &heading, float &dive, float &fov ) const override;
	bool IsVirtual( CameraType camType ) const override {return true;}
	bool IsContinuous(CameraType camType ) const override;
	float TrackingSpeed() const override {return 200;}

	bool Airborne() const override;

	SimulationImportance WorstImportance() const override {return SimulateVisibleFar;}
	SimulationImportance BestImportance() const override {return SimulateCamera;}

  Vector3 GetEyeDirection() const override;

	bool HasHUD() const override {return true;}

	LSError Serialize(ParamArchive &ar) override;

	void RepairGear() { _gearDammage = false; }
};

enum AirplaneState
{
	TaxiIn,
	Takeoff,Flight,Marshall,Approach,Final,Landing,Touchdown,WaveOff,
	TaxiOff,
};

class AirplaneAuto: public Airplane
{
	typedef Airplane base;

	protected:
	float _pilotSpeed;
	float _pilotHeading;
	float _pilotHeight;
	float _defPilotHeight;

	Foundation::Time _pilotAvoidHigh;
	float _pilotAvoidHighHeight;
	Foundation::Time _pilotAvoidLow;
	float _pilotAvoidLowHeight;

	float _pilotDive;
	float _pilotBank;

	float _dirCompensate;  // how much we compensate for estimated change

	bool _pilotHelperBankDive;
	bool _pilotHelperHeight;
	bool _pilotHelperDir;
	bool _pilotHelperThrust;

	bool _pilotHeadingSet;
	bool _pilotDiveSet;

	bool _pressedForward,_pressedBack;
	bool _targetOutOfAim;

	float _forceDive; // dive necessary for aiming

	AutopilotState _state;
	AirplaneState _planeState;
	// Which of the world's airports this plane uses (World::NearestAirport): chosen afresh on the ground and in
	// free flight, kept from the landing pattern to the touchdown. -1 = not chosen yet.
	int _airport = -1;
	int Airport();

	public:
	AirplaneAuto( VehicleType *name, Person *pilot );

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void AvoidGround( float minHeight );

	void DrawDiags() override;

	bool IsStopped() const override;
	void DammageCrew(EntityAI *killer, float howMuch, RString ammo) override;
	void Eject(AIUnit *unit) override;
	void Land() override;
	void CancelLand() override;
	void SetFlyingHeight(float val) override;

	bool IsAway( float factor=1 ) override;

	void EngineOn() override;
	void EngineOff() override;

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;
	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;

	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	void KeyboardAny(AIUnit *unit, float deltaT) override;

	void DetectControlMode() const override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void JoystickDirPilot( float deltaT );
	void JoystickThrustPilot( float deltaT );
	void FakePilot( float deltaT ) override;

	enum PathResult
	{
		PathFinished,
		PathAborted, // obstacle
		PathGoing,
	};

	PathResult PathAutopilot( const Vector3 *path, int nPath );

	bool TaxiOffAutopilot();
	bool TaxiInAutopilot();

	void Autopilot
	(
		Vector3Par target, Vector3Par tgtSpeed, // target
		Vector3Par direction, Vector3Par speed // wanted values
	);
	void ResetAutopilot();

	float MakeAirborne() override;

	RString DiagText() const override;

	// AI interface

	bool CalculateAimWeapon( int weapon, Vector3 &dir, Target *target ) override;
	bool AimWeapon( int weapon, Vector3Par direction ) override;
	bool AimWeapon( int weapon, Target *target ) override;
	Matrix4 GunTransform() const;
	Vector3 GetWeaponDirection( int weapon ) const override;
	void LimitCursor( CameraType camType, Vector3 &dir ) const override;
	float GetAimed( int weapon, Target *target ) const override;

	float GetFieldCost( const GeographyInfo &info ) const override;
	float GetCost( const GeographyInfo &info ) const override;
	float GetCostTurn( int difDir ) const override;

	float FireInRange( int weapon, float &timeToAim, const Target &target ) const override;

	void MoveWeapons( float deltaT );

	void AvoidCollision();
	void AIGunner(AIUnit *unit, float deltaT) override;
	void AIPilot(AIUnit *unit, float deltaT ) override;

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
