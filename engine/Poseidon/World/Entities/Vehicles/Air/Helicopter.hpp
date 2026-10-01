#pragma once

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Indicator.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>

#include <Poseidon/World/Entities/Infantry/Wounds.hpp>


namespace Poseidon
{
// Read-only developer evidence. No flight command or update is performed.
struct HelicopterHoverDiagnostics
{
    Vector3 wantedSpeed;
    Vector3 mouseDirection;
    bool hovering, speedHelper, heightHelper, directionHelper;
    float wantedHeight, wantedHeading, wantedDive;
};

class HelicopterType: public TransportType
{
	typedef TransportType base;
	friend class Helicopter;
	friend class HelicopterAuto;

	protected:
	Vector3 _pilotPos; // neutral neck and head positions

	Vector3 _missileLPos,_missileRPos;
	Vector3 _rocketLPos,_rocketRPos;

	Vector3 _gunPos,_gunDir;

	AnimationAnimatedTexture _animFire;
	TurretType _turret;

	AnimationRotation _rotorV; // animate geometry
	AnimationRotation _rotorH;

	AnimationSection _hRotorStill,_hRotorMove;
	AnimationSection _vRotorStill,_vRotorMove;

	Vector3 _mainRotorDiveAxis;
	Vector3 _backRotorDiveAxis;

	Indicator _altRadarIndicator, _altRadarIndicator2;
	Indicator _altBaroIndicator, _altBaroIndicator2;
	Indicator _speedIndicator, _speedIndicator2;
	Indicator _vertSpeedIndicator, _vertSpeedIndicator2;
	Indicator _rpmIndicator, _rpmIndicator2;
	Indicator _compass, _compass2;
	IndicatorWatch _watch, _watch2;
	AnimationWithCenter _vario, _vario2;
	Vector3 _varioDirection[MAX_LOD_LEVELS], _vario2Direction[MAX_LOD_LEVELS];
	Vector3 _hudPosition, _hudRight, _hudDown; 

	bool _enableSweep;

	HitPoint _hullHit;
	HitPoint _engineHit;
	HitPoint _rotorHHit,_rotorVHit;
	HitPoint _avionicsHit;
	HitPoint _missiles;
	HitPoint _glassRHit;
	HitPoint _glassLHit;

	WoundTextureSelections _glassDammageHalf;
	WoundTextureSelections _glassDammageFull;
 
	float _mainRotorSpeed;
	float _backRotorSpeed;

	// note: dive is used for Chinook.
	// Back rotor is actualy second main rotot in this case
	float _minMainRotorDive;
	float _minBackRotorDive;
	float _maxMainRotorDive;
	float _maxBackRotorDive;
	float _neutralMainRotorDive;
	float _neutralBackRotorDive;

	public:
	HelicopterType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class Helicopter: public Transport
{
	typedef Transport base;

	protected:
	Ref<Shape> _cargoCockpit;

	bool _missileLRToggle:1;
	bool _rocketLRToggle:1;
		
	float _rndFrequency; // volume control - low down hero sound
	float _rotorSpeed,_rotorSpeedWanted; // turning motor on/off
	float _snowRotorTime = 0;
	float _landRotorTime = 0;
	unsigned _landRotorPhase = 0;
	float _rotorPosition;

	// pilot can directly coordinate these parameters
	float _backRotor,_backRotorWanted;
	float _mainRotor,_mainRotorWanted; // main rotor thrust
	float _cyclicForward,_cyclicForwardWanted;
	float _cyclicAside,_cyclicAsideWanted; // main rotor thrust direction

	float _rotorDive,_rotorDiveWanted;

	Turret _turret;

	Vector3 _lastAngVelocity; // helper for prediction
	Vector3 _turbulence;
	Foundation::Time _lastTurbulenceTime;

	WeaponLightSource _mGunFire;
	int _mGunFireFrames;
	Foundation::UITime _mGunFireTime;
	int _mGunFirePhase;
	WeaponCloudsSource _mGunClouds;

	public:
	Helicopter( VehicleType *name, Person *pilot );
	~Helicopter() override;

	virtual bool ReadHoverDiagnostics(HelicopterHoverDiagnostics& out) const { (void)out; return false; }
	
	const HelicopterType *Type() const
	{
		return static_cast<const HelicopterType *>(GetType());
	}

	Matrix4 GunTransform() const;

	protected:
	float GetGlassBroken() const;
	void EmitLandRotorRing(float deltaT, SimulationImportance prec);

	void DammageAnimation( int level );
	void DammageDeanimation( int level );

	public:
	bool IsAnimated( int level ) const override;
	bool IsAnimatedShadow( int level ) const override;
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	Vector3 AnimatePoint( int level, int index ) const override;
	void AnimateMatrix(Matrix4 &mat, int level, int selection) const override;

	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;

	bool IsAbleToMove() const override;
	bool IsPossibleToGetIn() const override;

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void SimulateOptimized(float deltaT, SimulationImportance prec) override;
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	float GetEngineVol( float &freq ) const override;
	float GetEnvironVol( float &freq ) const override;

	Texture *GetCursorTexture(Person *person) override;
	Texture *GetCursorAimTexture(Person *person) override;

	float Rigid() const override {return 0.3;} // how much energy is transfered in collision

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

	void DoTransform
	(
		TLVertexTable &dst,
		const Shape &src, const Matrix4 &posView,
		int from, int to
	) const override;

	SimulationImportance WorstImportance() const override {return SimulateVisibleFar;}
	SimulationImportance BestImportance() const override {return SimulateCamera;}

	bool Airborne() const override;
	// Normalized physical rotor RPM. This remains non-zero while the blades
	// coast down after engine shutdown, unlike EngineIsOn().
	float RotorSpeed() const { return _rotorSpeed; }

	bool HasHUD() const override {return true;}

	LSError Serialize(ParamArchive &ar) override;
};

class HelicopterAuto: public Helicopter
{
	typedef Helicopter base;

	enum StopMode
	{
		SMNone,
		SMLand,
		SMGetIn,
		SMGetOut
	};

	protected:
	Vector3 _pilotSpeed;
	float _pilotHeading;
	float _pilotDive;

	float _forceDive; // dive necessary for aiming
	float _forceBank;
	float _pilotHeight;
	float _defPilotHeight; // set by scripting
	float _dirCompensate;  // how much we compensate for estimated change
	bool _avoidBankJitter;  // this should be off when autopilot is flying precisely, esp. when landing

	bool _pilotHeightHelper;
	bool _pilotSpeedHelper;
	bool _pilotDirHelper;

	bool _hoveringAutopilot;

	bool _pressedForward,_pressedBack;
	bool _pressedUp,_pressedDown;
	bool _pilotHeadingSet;
	bool _pilotDiveSet;

	Vector3 _stopPosition;
	StopMode _stopMode;

	float _bankWanted,_diveWanted;

	AutopilotState _state;

	enum SweepState {SweepDisengage,SweepTurn,SweepEngage,SweepFire};
	SweepState _sweepState;
	Foundation::Time _sweepDelay;
	LinkTarget _sweepTarget;
	Vector3 _sweepDir;
	
	public:
	HelicopterAuto( VehicleType *name, Person *pilot );

	bool ReadHoverDiagnostics(HelicopterHoverDiagnostics& out) const override
	{
		out = {_pilotSpeed, _mouseDirWanted, _hoveringAutopilot, _pilotSpeedHelper,
			_pilotHeightHelper, _pilotDirHelper, _pilotHeight, _pilotHeading, _pilotDive};
		return true;
	}

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void AvoidGround( float minHeight );

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;
	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;

	bool IsStopped() const override;

	void FindStopPosition() override;

	void DammageCrew( EntityAI *killer, float howMuch,RString ammo ) override;
	void Eject(AIUnit *unit) override;

	float MakeAirborne() override;
	void SetFlyingHeight(float val) override;

	void EngineOn() override;
	void EngineOff() override;
	void EngineOffAction() override;
	bool EngineIsOn() const override;

	void StopRotor();

	bool FireWeapon( int weapon, TargetType *target ) override;
	void FireWeaponEffects(int weapon, const Magazine *magazine,EntityAI *target) override;

	float FireValidTime() const override {return 45;}

	void SuspendedPilot(AIUnit *unit, float deltaT ) override;
	void KeyboardPilot(AIUnit *unit, float deltaT ) override;
	void DetectControlMode() const override;

	void JoystickDirPilot( float deltaT );
	void JoystickHeightPilot( float deltaT );
	void FakePilot( float deltaT ) override;

	Vector3 GetStopPosition() const override {return _stopPosition;}

	void Autopilot
	(
		float deltaT,
		Vector3Par target, Vector3Par tgtSpeed, // target
		Vector3Par direction, Vector3Par speed // wanted values
	);
	void ResetAutopilot();
	void BrakingManeuver();

	// AI interface
	bool AimWeapon(int weapon, Vector3Par direction ) override;
	bool AimWeapon(int weapon, Target *target ) override;
	bool AimObserver(Vector3Par direction) override;

	Vector3 GetWeaponPoint( int weapon ) const override;
	Vector3 GetWeaponCenter( int weapon ) const override;
	Vector3 GetWeaponDirection( int weapon ) const override;
	Vector3 GetWeaponDirectionWanted( int weapon ) const override;
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
	static HelicopterAuto *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;
protected:
	void UpdateStopMode(AIUnit *unit);
};

}  // namespace Poseidon
