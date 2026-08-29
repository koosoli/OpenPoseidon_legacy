#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>

#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>


namespace Poseidon
{
class Shot: public Vehicle
{
	typedef Vehicle base;

	protected:
	OLink<EntityAI> _parent;
	Ref<IWave> _sound;
	float _timeToLive;

	public:
	Shot( EntityAI *parent, const AmmoType *type );
	void SetParent( EntityAI *parent );

	const AmmoType *Type() const
	{
		return static_cast<const AmmoType *>(GetNonAIType());
	}

	bool OcclusionFire() const override {return false;}
	bool OcclusionView() const override {return false;}

	#ifdef NDEBUG
		bool Invisible() const override;
	#endif
	float VisibleSize() const override {return 0;}
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	float Rigid() const override {return 0;}

	EntityAI *GetOwner() const;
	
	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static Shot *CreateObject(NetworkMessageContext &ctx);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;
	float GetMaxPredictionTime(NetworkMessageContext &ctx) const override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class TimeBomb : public Shot
{
	typedef Shot base;

public:
	TimeBomb( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class PipeBomb : public Shot
{
	typedef Shot base;

protected:
	bool _explosion;

public:
	PipeBomb( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void Explode() {_explosion = true;}
	void SetTimer(float ttl = 20) {_timeToLive = ttl;}
	float GetTimer() const {return _timeToLive;}

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class Mine : public Shot
{
	typedef Shot base;

protected:
	bool _active;

public:
	Mine( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	bool IsActive() const {return _active;}
	void SetActive(bool active) {_active = active;}

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;
	
	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class ShotShell: public Shot
{
	typedef Shot base;

protected:
	float _airFriction;
	float _initDelay;
	
	float _coefGravity;
	// Drag law for this round. Constant unless the ammo config declares a drag function; see
	// Ballistics::DragRetardation, which both this and the aim predictor call.
	Poseidon::Ballistics::DragModel _dragModel = Poseidon::Ballistics::DragModel::Constant;
	float _ballisticCoefficient = 0.0f;
	bool _waterImpactDone;

	/// Seconds left on the fuse. Only meaningful while `_fuseArmed`.
	float _fuse = 0.0f;
	/// True when the ammo config declared `explosionTime > 0`. Everything the fuse
	/// changes hangs off this one flag, so a round without it takes byte-identical
	/// paths to before.
	bool _fuseArmed = false;
	/// The grenade has stopped moving and is only counting down. Integration is
	/// skipped while this holds -- otherwise gravity keeps accumulating into
	/// `_speed` and a resting grenade slowly sinks through the ground it lies on.
	bool _atRest = false;

public:
	ShotShell( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;
	/// Penetration follow-up to PHY-030: does this round come out the far side of
	/// what it just hit? Needs the physics collider set -- the thickness comes from
	/// a ray, so with physics absent it returns false, which is today's behaviour
	/// and the safe direction.
	bool TryPenetrate(const CollisionInfo &info, float deltaT);
	/// Reflects the shell off a surface and bleeds energy into it. Returns false
	/// when what is left is too slow to be worth simulating, in which case the
	/// shell is left at rest to cook off where it lies.
	bool BounceOff(Vector3Par surfaceNormal, Vector3Par contactPoint);
	/// Explodes here and now: the visual, the damage, and nothing else. Shared by
	/// the fuse and by the contact detonation so the two cannot drift apart.
	void Detonate(Vector3Par pos, Object *hitObject, Vector3Par hitNormal);
	bool Invisible() const override;
	void Sound( bool inside, float deltaT ) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class ShotBullet: public ShotShell
{
	typedef ShotShell base;

	Vector3 _beg,_end; // world space line

	public:
	ShotBullet( EntityAI *parent, const AmmoType *type );

	float GetMass() const override {return 0.05;}
	float GetInvMass() const override {return 1.0/0.05;}

	void Simulate( float deltaT, SimulationImportance prec ) override;
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override{}

	void StartFrame() override;
	bool Invisible() const override {return false;}

	int PassNum( int lod ) override;

	void DoDraw( int level, ClipFlags clipFlags, const FrameBase &pos );
	void Draw( int level, ClipFlags clipFlags, const FrameBase &pos ) override;
	#if ALPHA_SPLIT
	void DrawAlpha( int level, ClipFlags clipFlags, const FrameBase &pos );
	#endif

	void SetBeg( Vector3Val beg );
	void SetEnd( Vector3Val end );

	bool IsAnimated( int level ) const override;
	bool IsAnimatedShadow( int level ) const override;
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	void AnimatedMinMax( int level, Vector3 *minMax ) override;
	void AnimatedBSphere( int level, Vector3 &bCenter, float &bRadius, bool isAnimated ) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class Missile: public Shot
{
	typedef Shot base;

public:
	enum EngineState {Init,Thrust,Fly};
	enum LockState {Locked,Lost};

private:
	Ref<IWave> _soundEngine;

	CloudletSource _cloudlets;

	float _initTime,_thrustTime;

	Color _lightColor;
	Ref<LightPointOnVehicle> _light;

	float _thrust;
	EngineState _engine;
	LockState _lock;
	OLink<Object> _target;

	Vector3 _controlDirection;
	bool _controlDirectionSet;
	
	public:
	Missile
	(
		EntityAI *parent, const AmmoType *type, Object *target
	);
	~Missile() override;
	void Sound( bool inside, float deltaT ) override;
	void UnloadSound() override;

	void SetLight( ColorVal color );

	void Animate( int level ) override;
	void Deanimate( int level ) override;
	void Simulate( float deltaT, SimulationImportance prec ) override;
	void SetControlDirection( Vector3 dir );

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	float CalculateError(NetworkMessageContext &ctx) override;
	float GetMaxPredictionTime(NetworkMessageContext &ctx) const override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class IlluminatingShell: public ShotShell
{
	typedef ShotShell base;

protected:
	Color _lightColor;
	Ref<LightPointOnVehicle> _light;

public:
	IlluminatingShell( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void SetLight( ColorVal color );

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;

	LSError Serialize(ParamArchive &ar) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

class SmokeShell: public ShotShell
{
	typedef ShotShell base;

protected:
	SmokeSource _smoke;

public:
	SmokeShell( EntityAI *parent, const AmmoType *type );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	LSError Serialize(ParamArchive &ar) override;

	USE_CASTING(base)
	USE_FAST_ALLOCATOR
};

Shot *NewShot( EntityAI *parent, const AmmoType *type, Object *target );

}  // namespace Poseidon
