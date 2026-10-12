#pragma once

#define LOG_FLAG_CHANGES	0

#include <Poseidon/AI/Path/ArcadeWaypoint.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>

namespace Poseidon { class CStaticMapMain; }
using Poseidon::CStaticMapMain;

namespace Poseidon { class SoundObject; class DynSoundObject; }
namespace Poseidon
{
using Poseidon::SoundObject;
using Poseidon::DynSoundObject;
#define NON_AI_DETECTOR 1
#if NON_AI_DETECTOR
	typedef Vehicle DetectorBase;
#else
	typedef EntityAI DetectorBase;
#endif

class Detector : public DetectorBase
{
	friend class ::CStaticMapMain; // drawing
protected:
	typedef DetectorBase base;

	float _a;
	float _b;
	float _e;
	float _sinAngle;
	float _cosAngle;
	bool _rectangular;

	ArcadeSensorActivation _activationBy;
	ArcadeSensorActivationType _activationType;
	OLink<AIGroup> _assignedGroup;
	int _assignedStatic;
	int _assignedVehicle;
	bool _repeating;
	bool _interruptable;
	float _timeoutMin;
	float _timeoutMid;
	float _timeoutMax;

	ArcadeSensorType _action;
	RString _text;

	RString _expCond;
	RString _expActiv;
	RString _expDesactiv;

	AutoArray<int> _synchronizations;

	ArcadeEffects _effects;

	Poseidon::Foundation::Time _nextCheck;
	Poseidon::Foundation::Time _countdown;
	bool _active;
	bool _activeCountdown;

	SRef<GameValue> _vehicles;

	Ref<DynSoundObject> _dynSound;
	Ref<SoundObject> _sound;
	Ref<SoundObject> _voice;

#if NON_AI_DETECTOR
	Ref<const VehicleNonAIType> _type;
	const VehicleNonAIType *GetType() const {return _type;}
#endif

public:
	Detector(EntityType *name, int id);
	~Detector() override;

	void FromTemplate(const ArcadeSensorInfo &info);
	void AssignGroup(AIGroup *group);
	void AssignStatic(int id);
	void AssignVehicle(int id);
	
	void SetArea(float a, float b, float angle, bool rectangular);
	void SetActivation(ArcadeSensorActivation by, ArcadeSensorActivationType type, bool repeating);
	void SetTriggerType(ArcadeSensorType type);
	void SetTimeout(float min, float mid, float max, bool interruptable);
	void AttachVehicle(EntityAI *vehicle);
	void SetStatements(RString cond, RString activ, RString desactiv);
	void SetSynchronizations(AutoArray<int> synchronizations) {_synchronizations = synchronizations;}
	ArcadeEffects &GetEffects() {return _effects;}

	void Simulate( float deltaT, SimulationImportance prec ) override;
	bool QIsManual()const {return false;}

	TargetSide GetTargetSide() const {return _targetSide;}

	int GetActivationBy() const {return _activationBy;}
	int GetActivationType() const {return _activationType;}
	int GetAction() const {return _action;}
	int NSynchronizations() const {return _synchronizations.Size();}
	int GetSynchronization(int i) const {return _synchronizations[i];}
	float GetCountdown();
	RString GetText() const {return _text;}
	void SetText(RString text) {_text = text;}

	bool IsActive() const {return _active;}
	float GetAreaA() const {return _a;}
	float GetAreaB() const {return _b;}
	float GetAreaAngleDeg() const {return atan2(_sinAngle, _cosAngle) * (180.0f / 3.14159265f);}
	bool IsAreaRectangular() const {return _rectangular;}
	bool IsCountdown() const {return _activeCountdown;}
	bool IsRepeating() const {return _repeating;}

	Object *GetActiveVehicle();
	int NVehicles() const;
	const EntityAI *GetVehicle(int i) const;
	EntityAI *GetVehicle(int i);

	const GameValue &GetGameValue() const;

	void DoActivate();

	LSError Serialize(ParamArchive &ar) override;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static Detector *CreateObject(NetworkMessageContext &ctx);
	void DestroyObject() override;
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx) override;

protected:
	bool IsInside(Vector3Par pos, Vector3Par f1, Vector3Par f2);
	bool TestSide(EntityAI *vehicle);
	bool TestSide(const AITargetInfo &target);
	AICenter *GetCenter() const;
	bool TestVehicle(Vehicle *veh, Vector3Par f1, Vector3Par f2);
	bool TestVehicle(AICenter *center, Vehicle *veh, Vector3Par f1, Vector3Par f2);
	void Scan();
	void OnActivate(Object *obj);
	void OnDesactivate();

	USE_CASTING(base)
};

class FlagCarrier: public VehicleSupply
{
	typedef VehicleSupply base;

protected:
	Ref<Texture> _flagTexture;
	TargetSide _flagSide;
	OLink<Person> _flagOwner;
	OLink<Person> _flagOwnerWanted;

	bool _down;
	bool _up;
	Poseidon::Foundation::Time _animStart;
	Ref<AnimationRT> _animation;
	WeightInfo _weights;
	Ref<Skeleton> _skeleton;

	float _phase;
	float _animSpeed;
	float _invAnimSpeed;

public:
	FlagCarrier(VehicleType *type);

	void AnimateMatrix(Matrix4 &mat,int level, int selection) const override;
	bool IsAnimated(int level) const override {return true;}

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;

	Texture *GetFlagTexture() override;
	Texture *GetFlagTextureInternal() override;
	void SetFlagTexture(RString name) override;
	Person *GetFlagOwner() override {return _flagOwner;}
	void SetFlagOwner(Person *veh) override;
	TargetSide GetFlagSide() const override;
	void SetFlagSide(TargetSide side) override;
	
	bool QIsManual() const override {return false;}

	void Simulate( float deltaT, SimulationImportance prec ) override;

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
};
} // namespace Poseidon
