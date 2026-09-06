#pragma once

#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Physics/PhysicsTypes.hpp>

namespace Poseidon
{
class ThingType: public TransportType
{
	protected:
	friend class Thing;
	friend class ThingEffectLight;

	typedef VehicleTransportType base;
	float _submerged; // some thing submerge slowly under ground
	// _submerged<=0 should be interpreted as 0, it just make submerging
	// to start later
	float _submergeSpeed; // how fast do we submerge
	float _timeToLive;
	bool _disappearAtContact;

	public:
	ThingType( const ParamEntry *param );
	~ThingType() override;

	void Load(const ParamEntry &par) override;

};

class Thing: public VehicleSupply
{
	protected:
	enum CrashType {CrashNone,CrashLand,CrashWater,CrashObject};
	CrashType _doCrash;
	float _crashVolume;
	// time of last crash sound (will not repeat for some time)
	Foundation::Time _timeCrash;
	float _submerged; // some thing submerge - see ThingType
	bool _isCloudlet;

	/// This object's body in the physics world. Invalid unless loose-object
	/// physics is switched on AND this object is ours to move; see LooseObjects,
	/// which owns every rule about when that is true.
	Physics::BodyId _physicsBody;
	// RFG-095: a Thing gets a physics body only once something DISTURBS it -- a hit, a
	// collision, a push. Giving all 528 pieces of Everon's furniture a body at world
	// load made every chair settle against its table and every table against its
	// chairs, and the café had rearranged itself before anyone fired a shot.
	bool _physicsWake = false;

	typedef VehicleSupply base;

	public:
	Thing( VehicleType *name );
	~Thing() override;
	
	const ThingType *Type() const
	{
		return static_cast<const ThingType *>(GetType());
	}
	Vector3 Friction( Vector3Par speed );

	void Simulate( float deltaT, SimulationImportance prec ) override;

	float Rigid() const override {return 1;} // how much energy is transfered in collision

	void CrashDammage( float ammount, const Vector3 &pos=VZero );
	void HitBy( EntityAI *owner, float howMuch, RString ammo ) override; // RFG-095: wakes the body

	// building are usually empty
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	void DrawDiags() override;
	bool QIsManual() const override {return false;}

	void SetCloudlet(bool val) {_isCloudlet = val;}

	Matrix4 InsideCamera( CameraType camType ) const override;
	int InsideLOD( CameraType camType ) const override;
	
	// no get-in to buildings
	bool QCanIGetIn( Person *who = nullptr ) const {return false;}

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	USE_CASTING(base)
};

class ThingEffect: public Thing
{
	typedef Thing base;

	public:
	ThingEffect( VehicleType *name );

	USE_CASTING(base)
};

class ThingEffectLight: public Vehicle
{
	protected:
	enum CrashType {CrashNone,CrashLand,CrashWater,CrashObject};
	CrashType _doCrash;
	float _crashVolume;
	// time of last crash sound (will not repeat for some time)
	Foundation::Time _timeCrash;
	float _submerged; // some thing submerge - see ThingType
	float _timeToLive;

	bool _isCloudlet;

	typedef Vehicle base;

	public:
	ThingEffectLight( ThingType *name );

	const ThingType *Type() const
	{
		return static_cast<const ThingType *>(GetNonAIType());
	}
	Vector3 Friction( Vector3Par speed );

	void Simulate( float deltaT, SimulationImportance prec ) override;

	float Rigid() const override {return 1;} // how much energy is transfered in collision

	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	void SetCloudlet(bool val) {_isCloudlet = val;}

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

enum ThingEffectKind
{
	TEGround,
	TEArmor,
	TEHouse,
	TECartridge,
	NThingEffectKind
};

// create thing for given type - exact class name is randomized
// also adds it to World and Landscape
Entity *CreateThingEffect
(
	ThingEffectKind kind, // kind
	Matrix4Val pos, Vector3Val vel // position and velocity
);

// create thing with a known type
// also adds it to World and Landscape

Entity *CreateThing
(
	VehicleType *type, // kind
	Matrix4Val pos, Vector3Val vel // position and velocity
);

// we also might want to set angular velocity
// but this can be accomplised via AddImpulse()

}  // namespace Poseidon
