#pragma once

#include <Poseidon/World/Entities/Vehicles/Tracks.hpp>
#include <Poseidon/World/Entities/Vehicles/GearBox.hpp>

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>

#include <Poseidon/World/Scene/Indicator.hpp>


namespace Poseidon
{
class TankOrCarType: public TransportType
{
	typedef TransportType base;
	friend class TankOrCar;
	friend class Car;
	friend class Tank;
	friend class TankWithAI;

	protected:
	Indicator _speedIndicator, _speedIndicator2, _rpmIndicator;

	Vector3 _pilotPos;
	Vector3 _outPilotPos;
	Vector3 _exhaustPos,_exhaustDir;

	SoundPars _gearSound;

	AnimationSection _brakeLights;

	HitPoint _engineHit;

	bool _outPilotOnTurret;
	bool _hasExhaust;
	bool _canFloat; // is able to move in water

	public:

	TankOrCarType( const ParamEntry *param );
	void Load(const ParamEntry &par) override;
	void InitShape() override;
};

class IndicesUpdateTankOrCar : public IndicesUpdateTransport
{
	typedef IndicesUpdateTransport base;

public:
	int pilotBrake;

	IndicesUpdateTankOrCar();
	NetworkMessageIndices *Clone() const override {return new IndicesUpdateTankOrCar;}
	void Scan(NetworkMessageFormatBase *format) override;
};

class TankOrCar: public Transport
{
	typedef Transport base;
	protected:

	float _rpm,_rpmWanted;
	GearBox _gearBox;
	DustSource _leftDust,_rightDust;
	Link<IWave> _gearSound;

	ExhaustSource _exhaust;

	bool _pilotBrake;
	Foundation::Time _lastPilotBrake; // avoid flashing lights

	// sometimes vehicles goes "flying" - e.g. after hit (impulse)
	Foundation::Time _freeFallUntil;

	// TW-WATER W6-pre: how far a wreck in the water has flooded, 0..1 (TrackedWaterResponse.hpp)
	float _wreckFlood = 0.0f;

	public:
	TankOrCar( VehicleType *name, Person *driver );

	const TankOrCarType *Type() const
	{
		return static_cast<const TankOrCarType *>(GetType());
	}

	void Simulate( float deltaT, SimulationImportance prec ) override;

	virtual void AnimateSpeedIndicator(Matrix4 &trans, int level);

	void Animate( int level ) override;
	void Deanimate( int level ) override;

	void SimulateExhaust( float deltaT, SimulationImportance prec );

	float GetSteerAheadSimul() const override;
	float GetSteerAheadPlan() const override;

	float GetPrecision() const override;

	virtual float Thrust() const = 0;
	virtual float ThrustWanted() const = 0;

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
