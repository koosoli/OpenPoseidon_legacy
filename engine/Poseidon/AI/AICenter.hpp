#pragma once
#include <Poseidon/AI/AICore.hpp>

#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Audio/Speaker.hpp>
#include <Poseidon/AI/Path/PathPlanner.hpp>
#include <Poseidon/AI/AIGroup.hpp>

namespace Poseidon
{
class AITargetInfo
{
public:
	TargetId _idExact;
	TargetSide _side;
	const VehicleType *_type;

	Point3 _realPos;
	Point3 _pos;
	Vector3 _dir;

	int _x, _z;

	float _accuracySide;
	Foundation::Time _timeSide;
	float _accuracyType;
	Foundation::Time _timeType;
	float _precisionPos;
	Foundation::Time _time;

	bool _exposure;

	bool _vanished; // disappered - e.g. GetIn
	bool _destroyed; // dead - do not attack

	AITargetInfo();

	LSError Serialize(ParamArchive &ar);

	float FadingSideAccuracy() const;
	float FadingTypeAccuracy() const;
	float FadingPositionAccuracy() const;
};

// SIM-803 (Phase 8.3, stable iteration and reductions).
//
// Ranking key for AICenter::UpdateGuarding's guarded-vehicle list, split out from
// the AITargetInfo pointers so it can be exercised without a live world: known
// sides ahead of unknown, then descending type cost, then a DELIBERATE 0.
//
// Returning 0 on a tie is the point. The old comparator broke equal-cost ties with
// `info1->_idExact - info2->_idExact`, the difference of two heap addresses, which
// made a gameplay decision (which group guards which vehicle) depend on the
// allocator -- and, being a ptrdiff_t narrowed to int, could return the wrong sign
// for objects more than 2GiB apart. Ties are now resolved by std::stable_sort at
// the call site, i.e. by the info's index in AICenter's ordered target array.
//
// Returns <0 if 1 ranks before 2, >0 if after, 0 if the two are indistinguishable.
int CompareGuardedVehicleRank(bool sideUnknown1, float cost1, bool sideUnknown2, float cost2);

// Strict-weak ordering over AITargetInfo built on CompareGuardedVehicleRank.
// Contains no pointer term; must only ever be used with a STABLE sort.
bool GuardedVehicleBefore(const AITargetInfo *i1, const AITargetInfo *i2);

class AICheckPointInfo
{
public:
	Point3 _pos;
	int _x;
	int _z;
	Foundation::Time _time;
	int _missions;

	AICheckPointInfo();
	AICheckPointInfo(Vector3Val pos, Foundation::Time time);
};

struct AIEnemyInfo
{
	Point3 _pos;
	AutoArray<int> _targets; // indexes into AITargetList
};

typedef AutoArray<AITargetInfo> AITargetList;
typedef AutoArray<AICheckPointInfo> AICheckPointList;
typedef AutoArray<AIEnemyInfo> AIEnemyList;

typedef bool (*SideFunction)(const AICenter *center, TargetSide side);

union AIThreat
{
	unsigned int packed;
	struct
	{
		BYTE soft;
		BYTE armor;
		BYTE air;
		BYTE reserved;
	} u;
};

enum AIGuardTargetType
{
	GTTNothing,
	GTTVehicle,
	GTTPoint
};

struct AIGuardTarget
{
	AIGuardTargetType type;
	TargetId vehicle;
	Vector3 position;
};

struct AIGurdedVehicle
{
	TargetId vehicle;
	OLink<AIGroup> by;

	LSError Serialize(ParamArchive &ar);
};

struct AIGurdedPoint
{
	Vector3 position;
	OLink<AIGroup> by;

	LSError Serialize(ParamArchive &ar);
};

struct AIGuardingGroup
{
	OLink<AIGroup> group;
	bool guarding;

	LSError Serialize(ParamArchive &ar);
};

struct AISupportTarget
{
	OLink<AIGroup> group;
	Vector3 pos;
	bool heal;
	bool repair;
	bool rearm;
	bool refuel;

	LSError Serialize(ParamArchive &ar);
};

// group ready for support (on waypoint SUPPORT)
struct AISupportGroup
{
	OLink<AIGroup> group;
	bool heal;
	bool repair;
	bool rearm;
	bool refuel;
	OLink<AIGroup> assigned;

	LSError Serialize(ParamArchive &ar);
};

DEFINE_ENUM_BEG(EndMode)
	EMContinue, EMKilled, EMLoser,
	EMEnd1, EMEnd2, EMEnd3, EMEnd4, EMEnd5, EMEnd6
DEFINE_ENUM_END(EndMode)

struct VehicleInitCmd : public NetworkSimpleObject
{
	EntityAI *vehicle;
	RString init;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	TMError TransferMsg(NetworkMessageContext &ctx) override;
};

DEFINE_ENUM_BEG(AICenterMode)
    AICMDisabled,AICMArcade,AICMIntro,AICMNetwork
DEFINE_ENUM_END(AICenterMode)

class AICenter : public AI
{
	public:
	typedef AICenterMode Mode;

	private:
	// Structure
	RefArray<AIGroup> _groups;

	RadioChannel _radio;

	Mode _mode;

	Foundation::Time _lastUpdateTime;		// for groups

	// State
	TargetSide _side;
	float _friends[TSideUnknown];
	float _resources;	// actual money

	// Strategic database
	AITargetList _targets;

	// Strategic map
	SRef<AIMap> _map;
	int _row;
	int _column;

	int _nSoldier;
	int _nWoman;

	// guarded targets
	AutoArray<AIGuardingGroup> _guardingGroups;
	AutoArray<AIGurdedVehicle> _guardedVehicles;
	AutoArray<AIGurdedPoint> _guardedPoints;
	Foundation::Time _guardingValid;

	// supported targets
	AutoArray<AISupportTarget> _supportTargets;
	AutoArray<AISupportGroup> _supportGroups;
	Foundation::Time _supportValid;

public:
	AICenter(TargetSide side, Mode mode=AICMArcade);

	LSError Serialize(ParamArchive &ar);
	static AICenter *CreateObject(ParamArchive &ar)
		{return new AICenter(TSideUnknown, AICMDisabled);}
	static AICenter *LoadRef(ParamArchive &ar);
	LSError SaveRef(ParamArchive &ar) const;

	NetworkMessageType GetNMType(NetworkMessageClass cls) const override;
	static NetworkMessageFormat &CreateFormat
	(
		NetworkMessageClass cls,
		NetworkMessageFormat &format
	);
	static AICenter *CreateObject(NetworkMessageContext &ctx);
	void DestroyObject() override;
	TMError TransferMsg(NetworkMessageContext &ctx) override;
	float CalculateError(NetworkMessageContext &ctx	) override;
	static float CalculateErrorCoef(Vector3Par position, Vector3Par cameraPosition) {return 1.0;}
	Vector3 GetCurrentPosition() const override {return VZero;}

	RadioChannel &GetRadio() {return _radio;}
	const RadioChannel &GetRadio() const {return _radio;}
	float Resources() const {return _resources;}
	void SetResources(float res)
	{
		AI_ERROR(res >= 0);
		_resources = res;
	}
	void SpendResources(float toSpend)
	{
	}
	int NGroups() const {return _groups.Size();}
	AIGroup *GetGroup(int i) const {return _groups[i];}

	int NTargets() const {return _targets.Size();}
	const AITargetInfo &GetTarget(int i) const {return _targets[i];}
	const AITargetInfo *FindTargetInfo(TargetType *id) const;
	AITargetInfo *FindTargetInfo(TargetType *id);
	int FindTargetIndex(TargetType *id) const;

	Mode GetMode() const {return _mode;}
	void SetMode(Mode mode) {_mode = mode;}

	TargetSide GetSide() const {return _side;}
	int GetLanguage() const;
	float GetFriendship(TargetSide side) const {return _friends[side];}
	void SetFriendship(TargetSide side, float friendship)
	{
		if (side < TSideUnknown) _friends[side] = friendship;
	}

	bool IsFriendly( TargetSide side) const; // more complex test
	bool IsNeutral( TargetSide side) const;
	bool IsEnemy( TargetSide side) const;

	bool IsUnknown( TargetSide side) const; // used when selector funcion is needed
	bool IsCivilian( TargetSide side) const;
	bool IsWest( TargetSide side) const;
	bool IsEast( TargetSide side) const;
	bool IsResistance( TargetSide side) const;

	void UpdateGuarding();
	AIGuardTarget GetGuardTarget(AIGroup *grp);
	int AddGuardedPoint(Vector3Par pos);

	// supported targets
	void UpdateSupport();
	void ReadyForSupport(AIGroup *grp);
	void AskSupport(AIGroup *grp, UIActionType type);
	void SupportDone(AIGroup *grp);
	bool IsSupported(AIGroup *grp, UIActionType type);
	bool WaitingForSupport(AIGroup *grp, UIActionType type);
	bool CanSupport(UIActionType type);

	void InitPreview(const ArcadeUnitInfo &info);
	void Init(ArcadeTemplate &t, AutoArray<VehicleInitCmd, Foundation::MemAllocSA> &inits);

	void InitSensors(bool initialize = false);
	void AddGroup(AIGroup *grp, int id = 0);
	void GroupRemoved(AIGroup *grp);
	void SelectLeader(AIGroup *grp);

	// mind
	void Think();
	// communication with group
	void SendMission(AIGroup *to, Mission &mis);
	void ReceiveAnswer(AIGroup *from, Answer answer);
	void ReceiveReport(AIGroup *from, ReportSubject subject, Target &target);

	void InitTarget(EntityAI *veh, float age);
	void DeleteTarget(TargetType *id);
	void UpdateTarget(Target &target);

	float GetExposurePessimistic(int x, int z) const;
	float GetExposureOptimistic(int x, int z) const;
	float GetExposureUnknown(int x, int z) const;

	float GetExposurePessimistic(Vector3Par pos) const;
	float GetExposureOptimistic(Vector3Par pos) const;
	float GetExposureUnknown(Vector3Par pos) const;

	RString GetDebugName() const override;
	bool AssertValid() const;
	void Dump(int indent = 0) const;

	const ParamEntry &NextSoldierIdentity(bool woman);

// Implementation
private:
	float RecalculateExposure(int x, int z, AIThreat &result, SideFunction func);
	void UpdateField(int x, int z);
	void UpdateMap();
	void UpdateGroup();

	void BeginArcade(ArcadeTemplate &t, AutoArray<VehicleInitCmd, Foundation::MemAllocSA> &inits);
	void BeginPreviewUnit(const ArcadeUnitInfo &info);

	AIUnit *CreateSoldier(Transport *transport, int rank, const ParamEntry &cfgSide, bool multiplayer);
	AIUnit *CreateUnit
	(
		const ArcadeUnitInfo &info, ArcadeTemplate &t, bool multiplayer, AIGroup *grp,
		bool disableC = false, bool disableD = false, bool disableG = false
	);

	friend EntityAI *CreateVehicle(const ArcadeUnitInfo &info, ArcadeTemplate &t, TargetSide side, bool multiplayer);
	friend Vehicle *CreateSensor(const ArcadeSensorInfo &info, AIGroup *grp, bool multiplayer);
	friend void CreateMarker(const ArcadeMarkerInfo &info, bool multiplayer);

	void RemoveOldExposures();
	void AddNewExposures();
};

AICenter *CreateCenter(ArcadeTemplate &t, TargetSide side, AICenter::Mode mode);
void InitNoCenters(ArcadeTemplate &t, AutoArray<VehicleInitCmd, Foundation::MemAllocSA> &inits, bool multiplayer);

inline AISubgroup* AIUnit::GetSubgroup() const {return _subgroup;}
inline AIGroup* AISubgroup::GetGroup() const {return _group;}
inline AICenter* AIGroup::GetCenter() const {return _center;}
inline bool AIUnit::IsSubgroupLeader() const {return GetSubgroup() && GetSubgroup()->Leader() == this;}
inline bool AIUnit::IsSubgroupLeaderVehicle() const {return GetSubgroup() && GetSubgroup()->Leader()->GetVehicle() == GetVehicle();}
inline bool AIUnit::IsGroupLeader() const {return GetGroup() && GetGroup()->Leader() == this;}

enum AIStatsEventType
{
	SETUnitLost,
	SETKillsEnemyInfantry,
	SETKillsEnemySoft,
	SETKillsEnemyArmor,
	SETKillsEnemyAir,
	SETKillsFriendlyInfantry,
	SETKillsFriendlySoft,
	SETKillsFriendlyArmor,
	SETKillsFriendlyAir,
	SETKillsCivilInfantry,
	SETKillsCivilSoft,
	SETKillsCivilArmor,
	SETKillsCivilAir,
	SETUser,
};

enum AIStatsKills
{
	SKEnemyInfantry,
	SKEnemySoft,
	SKEnemyArmor,
	SKEnemyAir,
	SKFriendlyInfantry,
	SKFriendlySoft,
	SKFriendlyArmor,
	SKFriendlyAir,
	SKCivilianInfantry,
	SKCivilianSoft,
	SKCivilianArmor,
	SKCivilianAir,
	SKN
};

struct AIStatsMPRow
{
	int order;
	RString player;
	TargetSide side;
	int killsInfantry;
	int killsSoft;
	int killsArmor;
	int killsAir;
	int killsPlayers;
	int customScore;
	int killsTotal;
	int killed;

	AIStatsMPRow();
	void RecalculateTotal();
};

struct AIStatsEvent
{
	AIStatsEventType type;
	float time;	// from Clock
	Vector3 position;
	RString message;

	const VehicleType *killedType;
	const VehicleType *killerType;
	RString killedName;
	RString killerName;
	bool killedPlayer;
	bool killerPlayer;

	LSError Serialize(ParamArchive &ar);
};

enum MPReportEventType
{
	MPRETKill,
	MPRETInjury,
	MPRETUser,
};

struct MPReportUnitInfo
{
	RString name;
	RString type;
	Vector3 position;
	TargetSide side;
	bool player;
};

struct MPReportEvent
{
	Foundation::Time t;
	MPReportEventType type;
	MPReportUnitInfo unit1;
	MPReportUnitInfo unit2;
	float value;
	RString text;
};

class AIStatsMission : public SerializeClass
{
protected:
	float _size;
	PackedColor _color;
	PackedColor _colorBg;
	PackedColor _colorSelected;
	PackedColor _colorWest;
	PackedColor _colorEast;
	PackedColor _colorCiv;
	PackedColor _colorRes;

public:
	AutoArray<AIStatsEvent> _events;
	AutoArray<AIStatsMPRow> _tableMP;
	int _lives;
	Foundation::Time _start;
	time_t _time;
	bool _reportWritten;

	AutoArray<RString> _reportHeader;
	AutoArray<MPReportEvent> _reportEvents;
	AutoArray<RString> _reportFooter;

public:
	AIStatsMission();
	void Init();

	void Clear();
	void AddEvent
	(
		AIStatsEventType type, Vector3Par position, RString message,
		const VehicleType *killedType, RString killedName, bool killedPlayer,
		const VehicleType *killerType, RString killerName, bool killerPlayer
	);
	void AddMPKill(RString playerName, TargetSide playerSide, const VehicleType *killedType, bool killedPlayer, bool isEnemy);
	void AddMPScore(RString playerName, TargetSide playerSide, int score);
	void RecordReportKill(EntityAI *killed, EntityAI *killer);
	void RecordReportInjury(EntityAI *injured, EntityAI *killer, float damage, RString ammo);
	void AddReportHeader(RString text);
	void AddReportEvent(RString text);
	void AddReportFooter(RString text);
	void WriteMPReport();
	void OnMissionStart();

	void DrawMPTable(float alpha);

	LSError Serialize(ParamArchive &ar) override;
};

struct AIUnitHeader : public SerializeClass
{
	int index;
	float experience;
	Rank rank;

	OLink<AIUnit> unit;
	bool used;

	LSError Serialize(ParamArchive &ar) override;
};

class AIStatsCampaign : public SerializeClass
{
public:
	AIUnitHeader _playerInfo;
	AutoArray<RString> _awards;
	AutoArray<RString> _penalties;

	float _casualties;
	float _inCombat;
	int _kills[SKN];

	AutoArray<GameVariable> _variables;

	float _score, _lastScore;

	time_t _date;

public:
	AIStatsCampaign();
	void Clear();
	void Update(AIStatsMission &mission);
	void AddVariable(GameVariable &var);

	LSError CampaignSerialize(ParamArchive &ar);
	LSError Serialize(ParamArchive &ar) override;
};

class AIStats : public SerializeClass
{
public:
	AIStatsMission _mission;
	AIStatsCampaign _campaign;

public:
	AIStats() {}
	void Init() {_mission.Init();}

	void ClearAll() {_campaign.Clear(); _mission.Clear();}
	void ClearMission() {_mission.Clear();}

	void OnMissionStart();
	void OnMPMissionEnd();

	void Update() {_campaign.Update(_mission);}

	void OnVehicleDestroyed(EntityAI *killed, EntityAI *killer);
	void OnVehicleDamaged(EntityAI *injured, EntityAI *killer, float damage, RString ammo);
	void AddReportHeader(RString text);
	void AddReportEvent(RString text);
	void AddReportFooter(RString text);
	void AddUserEvent(RString event, Vector3Par pos);

	void DrawMPTable(float alpha) {_mission.DrawMPTable(alpha);}

	LSError Serialize(ParamArchive &ar) override;
};

AIStats& GetGStats();
#define GStats GetGStats()
inline AIUnitHeader &PlayerInfo() {return GStats._campaign._playerInfo;}

ArcadeTemplate& GetCurrentTemplate();
#define CurrentTemplate GetCurrentTemplate()

extern RString CurrentCampaign;
extern RString CurrentBattle;
extern RString CurrentMission;

inline bool IsCampaign() {return CurrentBattle && CurrentBattle.GetLength() > 0;}

LSError AIGlobalSerialize(ParamArchive &ar);

}  // namespace Poseidon
