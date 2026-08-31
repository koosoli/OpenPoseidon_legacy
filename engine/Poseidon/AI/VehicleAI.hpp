#pragma once

#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/AI/EntityAI.hpp>


namespace Poseidon
{
class IndicesResourceSupply
{
public:
	int fuelCargo;
	int repairCargo;
	int ammoCargo;
	int supplying;
	int alloc;
	int action;
	int actionParam;
	int actionParam2;
	int actionParam3;

	IndicesResourceSupply();
	void Scan(NetworkMessageFormatBase *format);
};

class ResourceSupply: public RefCount
{
	private:
	OLink<EntityAI> _parent;

	float _fuelCargo,_repairCargo,_ammoCargo;
	RefArray<WeaponType> _weaponCargo;
	RefArray<Magazine> _magazineCargo;
	OLink<EntityAI> _supplying;
	OLink<EntityAI> _alloc;
	UIActionType _action;
	int _actionParam;
	int _actionParam2;
	RString _actionParam3;

	public:
	ResourceSupply( EntityAI *vehicle );

	bool Check(EntityAI *vehicle, SupportCheckF check, float limit, bool now) const;

	void Simulate( float deltaT, SimulationImportance prec );

	void SetAlloc(EntityAI *vehicle) {_alloc=vehicle;}
	EntityAI *GetAlloc() const {return _alloc;}

	bool Supply(EntityAI *vehicle, UIActionType action, int param, int param2, RString param3);
	EntityAI *GetSupplying() const {return _supplying;}

	float GetFuelCargo() const {return _fuelCargo;}
	float GetRepairCargo() const {return _repairCargo;}
	float GetAmmoCargo() const {return _ammoCargo;}
	int GetWeaponCargoSize() const {return _weaponCargo.Size();}
	const WeaponType *GetWeaponCargo(int weapon) const {return _weaponCargo[weapon];}
	int GetMagazineCargoSize() const {return _magazineCargo.Size();}
	const Magazine *GetMagazineCargo(int magazine) const {return _magazineCargo[magazine];}

	int GetFreeWeaponCargo() const {return _parent->GetType()->_maxWeaponsCargo - _weaponCargo.Size();}
	int GetFreeMagazineCargo() const {return _parent->GetType()->_maxMagazinesCargo - _magazineCargo.Size();}

	void LoadFuelCargo( float cargo ) {_fuelCargo+=cargo;}
	void LoadRepairCargo( float cargo ) {_repairCargo+=cargo;}
	void LoadAmmoCargo( float cargo ) {_ammoCargo+=cargo;}
	void ClearWeaponCargo();
	int AddWeaponCargo(WeaponType *weapon, int count, bool deleteWhenFull=false);
	bool RemoveWeaponCargo(WeaponType *weapon);
	void ClearMagazineCargo();
	int AddMagazineCargo(Magazine *magazine, bool deleteWhenFull=false);
	int AddMagazineCargo(MagazineType *type, int count, bool deleteWhenFull=false);
	bool RemoveMagazineCargo(Magazine *magazine);

	void GetActions(UIActions &actions, AIUnit *unit, bool now);

	bool FindWeapon(const WeaponType *weapon) const;
	bool FindMagazine(const Magazine *magazine) const;

	const Magazine *FindMagazine(int creator, int id) const;
	const Magazine *FindMagazine(RString name) const;

	LSError Serialize(ParamArchive &ar);
	static ResourceSupply *CreateObject(ParamArchive &ar) {return new ResourceSupply();}

	static void CreateFormat(NetworkMessageFormat &format);
	TMError TransferMsg(NetworkMessageContext &ctx, const IndicesResourceSupply *indices);
	float CalculateError(NetworkMessageContext &ctx, const IndicesResourceSupply *indices);

	void SetParent(EntityAI *vehicle) {_parent = vehicle;}
private:
	ResourceSupply(); // used for serialization only

};

class IndicesUpdateVehicleSupply : public IndicesUpdateVehicleAI
{
	typedef IndicesUpdateVehicleAI base;

public:
	IndicesResourceSupply supply;

	IndicesUpdateVehicleSupply();
	NetworkMessageIndices *Clone() const override {return new IndicesUpdateVehicleSupply;}
	void Scan(NetworkMessageFormatBase *format) override;
};

class VehicleSupply: public EntityAI
{
	typedef EntityAI base;

	protected:
	Ref<ResourceSupply> _supply;

	mutable OLinkArray<AIUnit> _supplyUnits;

	public:
	VehicleSupply(EntityAIType *name, bool fullCreate=true);

	void SupplyStarted( AIUnit *unit );
	void SupplyFinished( AIUnit *unit );
	void WaitForSupply(AIUnit *unit);

	const OLinkArray<AIUnit> &GetSupplyUnits() const {return _supplyUnits;}

	void UpdateStop(); // something has been changed
	virtual bool CanCancelStop() const;

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

	void SetAllocSupply( EntityAI *vehicle );
	EntityAI *GetAllocSupply() const;

	virtual bool Supply(EntityAI *vehicle, UIActionType action, int param, int param2, RString param3);
	EntityAI *GetSupplying() const;

	float GetFuelCargo() const {return _supply ? _supply->GetFuelCargo() : 0;}
	float GetRepairCargo() const {return _supply ? _supply->GetRepairCargo() : 0;}
	float GetAmmoCargo() const {return _supply ? _supply->GetAmmoCargo() : 0;}

	int GetFreeMagazineCargo() const {return _supply ? _supply->GetFreeMagazineCargo() : 0;}
	int GetFreeWeaponCargo() const {return _supply ? _supply->GetFreeWeaponCargo() : 0;}

	float GetExplosives() const override; // how much explosives is in

	int GetWeaponCargoSize() const {return _supply ? _supply->GetWeaponCargoSize() : 0;}
	const WeaponType *GetWeaponCargo(int weapon) const {return _supply ? _supply->GetWeaponCargo(weapon) : nullptr;}
	int GetMagazineCargoSize() const {return _supply ? _supply->GetMagazineCargoSize() : 0;}
	const Magazine *GetMagazineCargo(int magazine) const {return _supply ? _supply->GetMagazineCargo(magazine) : nullptr;}

	void LoadFuelCargo( float cargo ) {if( _supply ) _supply->LoadFuelCargo(cargo);}
	void LoadRepairCargo( float cargo ) {if( _supply ) _supply->LoadRepairCargo(cargo);}
	void LoadAmmoCargo( float cargo ) {if( _supply ) _supply->LoadAmmoCargo(cargo);}

	void ClearWeaponCargo() {if (_supply) _supply->ClearWeaponCargo();}
	int AddWeaponCargo(WeaponType *weapon, int count = 1, bool deleteWhenFull=false)
	{
		return _supply ? _supply->AddWeaponCargo(weapon, count,deleteWhenFull) : -1;
	}
	bool RemoveWeaponCargo(WeaponType *weapon) {return _supply ? _supply->RemoveWeaponCargo(weapon) : false;}
	void ClearMagazineCargo() {if (_supply) _supply->ClearMagazineCargo();}
	int AddMagazineCargo(Magazine *magazine, bool deleteWhenFull=false)
	{
		return _supply ? _supply->AddMagazineCargo(magazine,deleteWhenFull) : -1;
	}
	int AddMagazineCargo(MagazineType *type, int count, bool deleteWhenFull=false)
	{
		return _supply ? _supply->AddMagazineCargo(type, count,deleteWhenFull) : -1;
	}
	bool RemoveMagazineCargo(Magazine *magazine) {return _supply ? _supply->RemoveMagazineCargo(magazine) : false;}

	RString GetActionName(const UIAction &action) override;
	void PerformAction(const UIAction &action, AIUnit *unit) override;

	void GetActions(UIActions &actions, AIUnit *unit, bool now) override;

	bool FindWeapon(const WeaponType *weapon) const override;
	bool FindMagazine(const Magazine *magazine) const override;

	const Magazine *FindMagazine(int creator, int id) const override;
	const Magazine *FindMagazine(RString name) const override;

	void ResetStatus() override;

	USE_CASTING(base)
};

typedef VehicleSupply EntitySupply;

DECL_ENUM(UnitPosition)

enum Rank
{
	RankUndefined = -1,
	RankPrivate,
	RankCorporal,
	RankSergeant,
	RankLieutnant,
	RankCaptain,
	RankMajor,
	RankColonel,
	NRanks
};

inline int ClampRankIndex(int rank)
{
	if (rank < RankPrivate)
		return RankPrivate;
	if (rank >= NRanks)
		return NRanks - 1;
	return rank;
}

struct AIUnitInfo : public SerializeClass
{
	RString _identityContext;
	RString _name;
	RString _face;
	RString _glasses;
	RString _speaker;
	float _pitch;
	float _experience;
	float _initExperience;
	Rank _rank;
	Ref<Texture> _squadPicture;
	RString _squadTitle;

	LSError Serialize(ParamArchive &ar) override;
};

class IndicesUpdateVehicleBrain : public IndicesUpdateVehicleSupply
{
	typedef IndicesUpdateVehicleSupply base;

public:
	int remotePlayer;

	IndicesUpdateVehicleBrain();
	NetworkMessageIndices *Clone() const override {return new IndicesUpdateVehicleBrain;}
	void Scan(NetworkMessageFormatBase *format) override;
};

//Target status (position, spotability etc.)
struct Target: public RemoveLLinks
{
	Vector3 position;
	Vector3 posError; // audible results are very inaccurate
	Vector3 speed;
	Vector3 posReported; // which position was reported

	TargetSide side; // side info merged from side and type
	bool sideChecked; // side info obtained from observation
	const EntityAIType *type;

	float spotability; // fading
	Foundation::Time spotabilityTime;
	float accuracy; // fading
	Foundation::Time accuracyTime;
	float sideAccuracy; // fading
	Foundation::Time sideAccuracyTime;
	Foundation::Time lastSeen;

	Foundation::Time delay; // delay for all
	Foundation::Time delaySensor; // delay for sensor (if bigger that delay -> invalid)

	Foundation::Time timeReported;

	bool isKnown; // do we remmember it (potential)
	bool vanished; // target vanished - probably GetIn
	bool destroyed; // target is dead

	TargetId idExact;
	OLink<Person> idSensor; // who sees the target best
	OLink<EntityAI> idKiller; // who killed this target
	OLink<AIGroup> group;

	float dammagePerMinute;
	float subjectiveCost;

	// functions

	float FadingSideAccuracy() const;
	float FadingAccuracy() const;
	float FadingSpotability() const;

	bool IsKnownBy( AIUnit *unit ) const;
	bool IsKnownByAll() const; // known by all group member
	bool IsKnownBySome() const; // known by all group member

	bool IsKnown() const;

	void LimitError( float error );

	LSError Serialize(ParamArchive &ar);
	static Target *CreateObject(ParamArchive &ar) {return new Target(nullptr);}
	static Target *LoadRef(ParamArchive &ar);
	LSError SaveRef(ParamArchive &ar);

	void Init();
	Target( AIGroup *group );

	float VisibleSize() const;
	Vector3 AimingPosition() const;
	Vector3 ExactAimingPosition() const;
	Vector3 LandAimingPosition() const;

	TargetState State(AIUnit *sensor) const;

	USE_FAST_ALLOCATOR
};

inline TargetType *LinkTarget::IdExact() const
{
	Target *link=GetLink();
	if( link ) return link->idExact;
	return nullptr;
}

class TargetList: public RefArray<Target>
{
	public:
	void Manage( AIGroup *group );
	void Manage(); // player visible list managenemt
	int Find( TargetType *target ) const;

	// SIM-803 (Phase 8.3, stable iteration and reductions).
	//
	// Ranks the list by descending subjectiveCost. The ordering this produces is
	// gameplay-observable: AIGroup::AssignTargets and SelectInterestingTarget walk
	// this array in order, so whichever of two equally-costed targets lands first
	// is the one a unit engages.
	//
	// subjectiveCost ties are NOT rare. AIGroup::GetSubjectiveCost collapses to the
	// type's flat GetCost() for any target that cannot currently fire back, so two
	// identical unit types tie exactly, bit for bit, every think cycle.
	//
	// This is deliberately a STABLE sort. The previous implementation used QSort
	// (Foundation/Algorithms/Qsort.hpp) with a comparator that returns 0 on a tie;
	// QSort is an unstable quicksort (and its <=8-element ShortSort path is a
	// selection sort, also unstable), so equal-cost targets came out in whatever
	// order the partitioning happened to leave them. That is deterministic only for
	// as long as the array arrives in a deterministic order -- which is exactly the
	// assumption Phase 8 breaks when detection starts producing targets from worker
	// jobs that complete in scheduler order.
	//
	// Tie-break is therefore the target's existing index in this array, which is a
	// stable identity that already exists here and does not involve a heap address.
	// (Object::ID() is not usable as the identity: WorldImpl.cpp:1772+ assigns -1 to
	// every dynamically created vehicle, so it is not unique for AI entities.)
	void SortBySubjectiveCost();
};


}  // namespace Poseidon

// Person.hpp depends on VehicleSupply (defined above); include last.
#include <Poseidon/World/Entities/Infantry/Person.hpp>
