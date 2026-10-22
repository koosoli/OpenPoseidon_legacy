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
	// Tidewater's helm wheel ("tw_wheel" about the memory axis "osa volantu"): the 2001 "volant" takes its axis from
	// points in the pilot-view LOD, which a generated model cannot keep (faceless points), and drew the wheel away
	AnimationRotation _twWheel;
	Animation _twFlag; // (OP) Tidewater's ensign, flown in the apparent wind (Ship::Animate)

	HitPoint _turretHit,_gunHit;

	public:
	// A hull described in the config (class TidewaterHull, written by generate_tidewater_boat.py for
	// Tidewater's own boat): its waterplane patches, inertia, propeller, rudder and keel, in model space.
	// Without it ShipTidewater.cpp scales Tidewater's reference hull to the model's box.
	struct TidewaterHull
	{
		static constexpr int MaxSamples = 32;
		bool valid = false;
		int samples = 0;
		Vector3 sample[MaxSamples];
		float area[MaxSamples] = {};
		float depth[MaxSamples] = {}; // design depth under the waterline (Tidewater's -p.y)
		Vector3 origin;               // Tidewater's boat-frame origin (the design waterline) in the model
		Vector3 inertia;              // about x (pitch), y (yaw), z (roll)
		float maxThrust = 0, pitchSpeed = 0, reverseFactor = 0.45f;
		Vector3 propeller, rudder;
		int stations = 0;
		float stationZ[8] = {}, stationArea[8] = {};
		float lateralY = 0;
		int contacts = 0;
		Vector3 contact[8];
		// the hull's outline (waterMask[]: half beam at the sheer, stern and bow z, sheer height aft
		// and at the bow, Tidewater's frame): the water renderer draws no sea inside it
		bool waterMask = false;
		float maskHalfBeam = 0, maskStern = 0, maskBow = 0, maskSheerAft = 0, maskSheerBow = 0;
	};
	const TidewaterHull &GetTidewaterHull() const {return _twHull;}

	protected:
	TidewaterHull _twHull;
	void LoadTidewaterHull(const ParamEntry &par);

	public:
	ShipType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

// The draft a hull of this length floats at in the Tidewater boat model (ShipTidewater.cpp); the wake
// simulation uses the same.
float ShipDesignDraft(float length);

class Ship: public Transport
{
	typedef Transport base;

	public:
	const Ship *AsShip() const override {return this;}
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

	// Tidewater boat model (ShipTidewater.cpp): its own 120 Hz integrator and the water under each hull
	// sample from the previous frame (height, the water's vertical velocity, where it was asked, slope).
	static constexpr int TwSamples = 21; // the generic hull's 7 x 3 grid
	static constexpr int TwMaxSamples = ShipType::TidewaterHull::MaxSamples;
	struct TidewaterState
	{
		float acc = 0;
		bool hasWater = false;
		float rudder = 0, throttle = 0, rpm = 0;
		float waterH[TwMaxSamples] = {}, waterV[TwMaxSamples] = {}, qx[TwMaxSamples] = {}, qz[TwMaxSamples] = {},
			gx[TwMaxSamples] = {}, gz[TwMaxSamples] = {};
		float heaveSpeed = 0; // last hull vertical speed relative to the water (ripples)
		// (OP) the drawn sea from the renderer's queries (DrawnWaterQuery): the result set the samples'
		// anchors came from, when it arrived, and each sample's height in it
		bool fromQuery = false;
		uint32_t qVersion = 0;
		double qSeconds = 0;
		float anchorH[TwMaxSamples] = {};
		float switchOff[TwMaxSamples] = {}; // a source switch's height jump, fading out
		bool settled = false;             // past its first step (placed at its draft if it began afloat)
	} _tw;
	bool SimulateTidewater(float deltaT, Frame& moveTrans);

	// Men standing on this hull's roadway (decks): carried with every move the hull makes, so the deck
	// does not slide out from under them (Man::UpdatePlatform registers them).
	OLinkArray<Vehicle> _riders;
	void CarryRiders(const Matrix4& before);

	// (OP) Tidewater's boat sounds (SeaSoundscape.hpp): water lapping the hull at rest, rushing past it under way,
	// chop slapping it
	Ref<IWave> _twLap, _twRush, _twEngine;
	float _twSlapT = 0;
	float _twEngineOn = 0; // Tidewater's engine spool (0 off .. 1 running)
	// (OP) Tidewater's flag: the apparent wind's direction in model space and its strength 0..1 (BoatModel.update)
	Vector3 _twFlagDir = Vector3(0, 0, -1);
	float _twFlagWind = 0.5f;
	void FlyFlag(int level, bool restore);

	bool _hasDeck = false; // its model has a roadway LOD (counted in DeckShips)

	public:
	void AddRider(Vehicle *rider);
	void RemoveRider(Vehicle *rider);
	// boats with a deck in the world: while there are none, no man looks for one under his feet
	static int DeckShips();
	protected:

	public:
	Ship( VehicleType *name, Person *driver );
	~Ship() override;

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
	// a boat with a deck keeps her lights as her helmsman left them when he steps out onto it or moves to a seat
	bool LightsStayUnmanned() const override {return _hasDeck;}
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

	// A boat with a deck: a swimmer within reach of its side climbs aboard (ATClimbAboard)
	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;
	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;
	// the point of the deck's lowest walkable level beside `worldPos`, if a swimmer there can reach it
	bool DeckPointNear(Vector3Par worldPos, Vector3 &deck) const;

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
