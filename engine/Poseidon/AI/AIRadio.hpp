#pragma once

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/AI/AIGroup.hpp>
namespace Poseidon::Dev::DebugCommands { class Command; }
namespace Poseidon { class SentenceParams; }

namespace Poseidon
{
enum RadioMessageType
{
	RMTCommand,
	RMTFormation,
	RMTSemaphore,
	RMTBehaviour,
	RMTOpenFire,
	RMTCeaseFire,
	RMTLooseFormation,
	RMTReportStatus,
	RMTTarget,
	RMTGroupAnswer,
	RMTSubgroupAnswer,
	RMTReturnToFormation,
	RMTFireStatus,
	RMTUnitAnswer,
	RMTUnitKilled,
	RMTText,
	RMTReportTarget,
	RMTObjectDestroyed,
	RMTContact, RMTUnderFire, RMTClear,
	RMTRepeatCommand,
	RMTWhereAreYou,
	RMTNotifyCommand,
	RMTCommandConfirm,
	RMTWatchAround,RMTWatchDir,RMTWatchPos,RMTWatchTgt,RMTWatchAuto,
	RMTPosition,
	RMTFormationPos,
	RMTTeam,
	RMTAskSupply,
	RMTSupportAsk,
	RMTSupportConfirm,
	RMTSupportReady,
	RMTSupportDone,
	RMTJoin,
	RMTJoinDone,
};

class RadioMessageUnitKilled : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	OLink<AIUnit> _who;

public:
	RadioMessageUnitKilled(AIUnit *from, AIUnit *who)
	{_from = from; _who = who;}
	RadioMessageUnitKilled() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTUnitKilled;}
	AIUnit *GetSender() const override {return _from;}
	AIUnit *GetWhoKilled() const {return _who;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageAskSupply : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	Command::Message _message;

public:
	RadioMessageAskSupply(AIUnit *from, Command::Message message)
	{_from = from; _message = message;}
	RadioMessageAskSupply() {_message = Command::NoCommand;}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTAskSupply;}
	AIUnit *GetSender() const override {return _from;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// used when group asked for support
class RadioMessageSupportAsk : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	UIActionType _type;

public:
	RadioMessageSupportAsk(AIGroup *from, UIActionType type)
	{_from = from; _type = type;}
	RadioMessageSupportAsk() {_type = (UIActionType)0;}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTSupportAsk;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// used when support group is assigned
class RadioMessageSupportConfirm : public RadioMessage
{
protected:
	OLink<AIGroup> _from;

public:
	RadioMessageSupportConfirm(AIGroup *from) {_from = from;}
	RadioMessageSupportConfirm() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTSupportConfirm;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// used when support group reached rendezvous place
class RadioMessageSupportReady : public RadioMessage
{
protected:
	OLink<AIGroup> _from;

public:
	RadioMessageSupportReady(AIGroup *from)
	{_from = from;}
	RadioMessageSupportReady() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTSupportReady;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// used when supported group already doesn't need support
class RadioMessageSupportDone : public RadioMessage
{
protected:
	OLink<AIGroup> _from;

public:
	RadioMessageSupportDone(AIGroup *from)
	{_from = from;}
	RadioMessageSupportDone() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTSupportDone;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageJoin : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	OLinkArray<AIUnit> _to;

public:
	RadioMessageJoin(AIGroup *from, PackedBoolArray list);
	RadioMessageJoin() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override {}
	int GetType() const override {return RMTJoin;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	bool IsTo(AIUnit *unit) const;
	const char *PrepareSentence(SentenceParams &params) override;
};

// used when supported group already doesn't need support
class RadioMessageJoinDone : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	OLink<AIGroup> _to;

public:
	RadioMessageJoinDone(AIUnit *from, AIGroup *to)
	{_from = from; _to = to;}
	RadioMessageJoinDone() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override {}
	int GetType() const override {return RMTJoinDone;}
	AIUnit *GetSender() const override {return _from;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageUnitAnswer : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	OLink<AISubgroup> _to;
	AI::Answer _answer;

public:
	RadioMessageUnitAnswer(AIUnit *from, AISubgroup *to, AI::Answer answer)
	{_from = from; _to = to; _answer = answer;}
	RadioMessageUnitAnswer() {}
	AIUnit *GetFrom() const {return _from;}
	AI::Answer GetAnswer() const {return _answer;}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTUnitAnswer;}
	AIUnit *GetSender() const override {return _from;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageFireStatus : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	bool _answer;

public:
	RadioMessageFireStatus(AIUnit *from, bool answer)
	{_from = from; _answer = answer;}
	RadioMessageFireStatus() {}
	AIUnit *GetFrom() const {return _from;}
	bool GetAnswer() const {return _answer;}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTFireStatus;}
	AIUnit *GetSender() const override {return _from;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageSubgroupAnswer : public RadioMessage
{
protected:
	OLink<AISubgroup> _from;
	OLink<AIUnit> _leader;
	OLink<AIGroup> _to;
	AI::Answer _answer;
	Command _cmd;
	bool _display;
	bool _forceLeader;

public:
	RadioMessageSubgroupAnswer
	(
		AISubgroup *from, AIGroup *to, AI::Answer answer, Command *cmd, bool display = true,
		AIUnit *leader = nullptr
	);
	RadioMessageSubgroupAnswer();
	const char *GetPriorityClass() override;
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTSubgroupAnswer;}
	AISubgroup *GetFrom() const {return _from;}
	AIGroup *GetTo() const {return _to;}
	const Command &GetCommand() const {return _cmd;}
	AIUnit *GetSender() const override;

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageGroupAnswer : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	OLink<AICenter> _to;
	AI::Answer _answer;

public:
	RadioMessageGroupAnswer(AIGroup *from, AICenter *to, AI::Answer answer);
	RadioMessageGroupAnswer() {}
	const char *GetPriorityClass() override;
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTGroupAnswer;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageReturnToFormation : public RadioMessage
{
protected:
	OLink<AISubgroup> _from;
	OLink<AIUnit> _leader;
	OLink<AIUnit> _to;

public:
	RadioMessageReturnToFormation(AISubgroup *from, AIUnit *to)
	{
		_from = from; _leader = from->Leader(); _to = to;
	}
	RadioMessageReturnToFormation() {}
	const char *GetPriorityClass() override;
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTReturnToFormation;}
	AIUnit *GetSender() const override {return _from && _from->Leader() ? _from->Leader() : (AIUnit*)_leader;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// abstract class for all reply information
class RadioMessageReply : public RadioMessage
{
protected:
	OLink<AIUnit> _from;
	OLink<AIGroup> _to;

public:
	RadioMessageReply(AIUnit *from, AIGroup *to);
	RadioMessageReply() {}
	LSError Serialize(ParamArchive &ar) override;
	AIUnit *GetFrom() const {return _from;}
	AIGroup *GetTo() const {return _to;}
	AIUnit *GetSender() const override {return _from;}
};

struct ReportTargetInfo
{
	LLink<Target> tgt;
	LSError Serialize(ParamArchive &ar);
};

class RadioMessageReportTarget : public RadioMessageReply
{
	typedef RadioMessageReply base;

protected:
	ReportSubject _subject;
	int _x;
	int _z;
	AutoArray<ReportTargetInfo> _targets;

public:
	RadioMessageReportTarget(AIUnit *from, AIGroup *to, ReportSubject subject, Target &target);
	RadioMessageReportTarget() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTReportTarget;}
	ReportSubject GetSubject() {return _subject;}
	bool IsTarget(TargetType *id) const;
	bool HasType(const VehicleType *type) const;
	bool IsInside(Vector3Val pos) const;
	void DeleteTarget(TargetType *id);
	void AddTarget(Target &target);

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageNotifyCommand : public RadioMessageReply
{
	typedef RadioMessageReply base;

protected:
	Command _command;

public:
	RadioMessageNotifyCommand(AIUnit *from, AIGroup *to, Command &command);
	RadioMessageNotifyCommand() {}
	const char *GetPriorityClass() override;
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTNotifyCommand;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageObjectDestroyed : public RadioMessageReply
{
	typedef RadioMessageReply base;

protected:
	const VehicleType *_vehicleType;

public:
	RadioMessageObjectDestroyed(AIUnit *from, AIGroup *to, const VehicleType *type);
	RadioMessageObjectDestroyed() {_vehicleType = nullptr;}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTObjectDestroyed;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageContact : public RadioMessageReply
{
	typedef RadioMessageReply base;
public:
	RadioMessageContact(AIUnit *from, AIGroup *to);
	RadioMessageContact() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTContact;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageUnderFire : public RadioMessageReply
{
	typedef RadioMessageReply base;

public:
	RadioMessageUnderFire(AIUnit *from, AIGroup *to);
	RadioMessageUnderFire() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTUnderFire;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageClear : public RadioMessageReply
{
	typedef RadioMessageReply base;

public:
	RadioMessageClear(AIUnit *from, AIGroup *to);
	RadioMessageClear() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTClear;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageRepeatCommand : public RadioMessageReply
{
	typedef RadioMessageReply base;

public:
	RadioMessageRepeatCommand(AIUnit *from, AIGroup *to);
	RadioMessageRepeatCommand() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTRepeatCommand;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWhereAreYou : public RadioMessageReply
{
	typedef RadioMessageReply base;

public:
	RadioMessageWhereAreYou(AIUnit *from, AIGroup *to);
	RadioMessageWhereAreYou() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTWhereAreYou;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageCommandConfirm : public RadioMessageReply
{
	typedef RadioMessageReply base;

protected:
	Command _command;

public:
	RadioMessageCommandConfirm(AIUnit *from, AIGroup *to, Command &command);
	RadioMessageCommandConfirm() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTCommandConfirm;}
	const Command &GetCommand() const {return _command;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageFormation : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	OLink<AISubgroup> _to;
	AI::Formation _formation;

public:
	RadioMessageFormation(AIGroup *from, AISubgroup *to, AI::Formation formation)
	{_from = from; _to = to; _formation = formation;}
	RadioMessageFormation() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTFormation;}

	AIGroup *GetFrom() {return _from;}
	AISubgroup *GetTo() {return _to;}
	AI::Formation GetFormation() {return _formation;}
	void SetFormation(AI::Formation f) {_formation = f;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

// abstract class for all state information
class RadioMessageState: public RadioMessage
{
	typedef RadioMessage base;

protected:
	OLink<AIGroup> _from;
	OLinkArray<AIUnit> _to;

public:
	RadioMessageState(AIGroup *from, PackedBoolArray list);
	RadioMessageState();
	virtual RadioMessageState *Clone() const = 0; 
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	
	AIGroup *GetFrom() const {return _from;}
	bool IsTo(AIUnit *unit) const;
	bool IsOnlyTo(AIUnit *unit) const;
	void DeleteTo(AIUnit *unit);
	void AddTo(AIUnit *unit);
	void ClearTo();
	bool IsToSomeone() const;

	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}
};

class RadioMessageSemaphore : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	AI::Semaphore _semaphore;

public:
	RadioMessageSemaphore(AIGroup *from, PackedBoolArray list, AI::Semaphore semaphore);
	RadioMessageSemaphore() {}
	RadioMessageState *Clone() const override {return new RadioMessageSemaphore(*this);}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTSemaphore;}
	
	AI::Semaphore GetSemaphore() {return _semaphore;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageBehaviour : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	CombatMode _behaviour;

public:
	RadioMessageBehaviour(AIGroup *from, PackedBoolArray list, CombatMode behaviour);
	RadioMessageBehaviour() {}
	RadioMessageState *Clone() const override {return new RadioMessageBehaviour(*this);}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTBehaviour;}
	
	CombatMode GetBehaviour() {return _behaviour;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

enum OpenFireState;
AI::Semaphore ApplyOpenFire(AI::Semaphore s, OpenFireState open);

class RadioMessageOpenFire : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	OpenFireState _open;

public:
	RadioMessageOpenFire(AIGroup *from, PackedBoolArray list, OpenFireState open);
	RadioMessageOpenFire() {}
	RadioMessageState *Clone() const override {return new RadioMessageOpenFire(*this);}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTOpenFire;}
	
	OpenFireState GetOpenFireState() {return _open;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageCeaseFire : public RadioMessage
{
	typedef RadioMessage base;

protected:
	OLink<AIUnit> _from;
	OLink<AIUnit> _to;
	bool _insideGroup;

public:
	RadioMessageCeaseFire(AIUnit *from, AIUnit *to, bool insideGroup);
	RadioMessageCeaseFire() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTCeaseFire;}
	AIUnit *GetSender() const override {return _from;}
	AIUnit *GetTo() const {return _to;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

AI::Semaphore ApplyLooseFormation(AI::Semaphore s, bool loose);

class RadioMessageLooseFormation : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	bool _loose;

public:
	RadioMessageLooseFormation(AIGroup *from, PackedBoolArray list, bool loose);
	RadioMessageLooseFormation() {}
	RadioMessageState *Clone() const override {return new RadioMessageLooseFormation(*this);}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTLooseFormation;}
	
	bool IsLooseFormation() {return _loose;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageUnitPos : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	UnitPosition _state;

public:
	RadioMessageUnitPos(AIGroup *from, PackedBoolArray list, UnitPosition state);
	RadioMessageUnitPos() {}
	RadioMessageState *Clone() const override {return new RadioMessageUnitPos(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTPosition;}
	
	UnitPosition GetSemaphore() {return _state;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageFormationPos : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	AI::FormationPos _state;

public:
	RadioMessageFormationPos(AIGroup *from, PackedBoolArray list, AI::FormationPos state);
	RadioMessageFormationPos() {}
	RadioMessageState *Clone() const override {return new RadioMessageFormationPos(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTFormationPos;}
	
	AI::FormationPos GetFormationPos() {return _state;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWatchAround : public RadioMessageState
{
	typedef RadioMessageState base;

public:
	RadioMessageWatchAround(AIGroup *from, PackedBoolArray list);
	RadioMessageWatchAround() {}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTWatchAround;}
	RadioMessageState *Clone() const override {return new RadioMessageWatchAround(*this);}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWatchAuto : public RadioMessageState
{
	typedef RadioMessageState base;

public:
	RadioMessageWatchAuto(AIGroup *from, PackedBoolArray list);
	RadioMessageWatchAuto() {}
	RadioMessageState *Clone() const override {return new RadioMessageWatchAuto(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTWatchAuto;}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWatchDir : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	Vector3 _dir;

public:
	RadioMessageWatchDir(AIGroup *from, PackedBoolArray list, Vector3Val dir);
	RadioMessageWatchDir() {}
	RadioMessageState *Clone() const override {return new RadioMessageWatchDir(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTWatchDir;}
	Vector3Val GetWatchDirection() const {return _dir;}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWatchPos : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	Vector3 _pos;

public:
	RadioMessageWatchPos(AIGroup *from, PackedBoolArray list, Vector3Val pos);
	RadioMessageWatchPos() {}
	RadioMessageState *Clone() const override {return new RadioMessageWatchPos(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTWatchPos;}
	Vector3Val GetWatchPosition() const {return _pos;}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageWatchTgt : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	LinkTarget _tgt;

public:
	RadioMessageWatchTgt(AIGroup *from, PackedBoolArray list, Target *tgt);
	RadioMessageWatchTgt() {}
	RadioMessageState *Clone() const override {return new RadioMessageWatchTgt(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	int GetType() const override {return RMTWatchTgt;}
	Target *GetWatchTarget() const {return _tgt;}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageReportStatus : public RadioMessageState
{
	typedef RadioMessageState base;

public:
	RadioMessageReportStatus(AIGroup *from, PackedBoolArray list);
	RadioMessageReportStatus() {}
	RadioMessageState *Clone() const override {return new RadioMessageReportStatus(*this);}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTReportStatus;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageTarget : public RadioMessageState
{
	typedef RadioMessageState base;

protected:
	LinkTarget _target;
	bool _engage;
	bool _fire;

public:
	RadioMessageTarget
	(
		AIGroup *from, PackedBoolArray list,Target *target,
		bool engage, bool fire
	);
	RadioMessageTarget();
	RadioMessageState *Clone() const override {return new RadioMessageTarget(*this);}
	LSError Serialize(ParamArchive &ar) override;
	void Transmitted() override;
	void Canceled() override;
	int GetType() const override {return RMTTarget;}
	Target *GetTarget() const {return _target;}
	const char *GetPriorityClass() override;

	bool GetEngage() const {return _engage;}
	bool GetFire() const {return _fire;}

	void SetEngage(bool val) {_engage=val;}
	void SetFire(bool val) {_fire=val;}
	
protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageTeam : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	OLinkArray<AIUnit> _to;
	Team _team;

public:
	RadioMessageTeam(AIGroup *from, PackedBoolArray list, Team team);
	RadioMessageTeam() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTTeam;}
	AIGroup *GetFrom() const {return _from;}
	bool IsTo(AIUnit *unit) const;
	Team GetTeam() const {return _team;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageCommand : public RadioMessage
{
protected:
	OLink<AIGroup> _from;
	OLinkArray<AIUnit> _to;
	Command _command;
	bool _toMainSubgroup;
	bool _transmit;
	// Order already carried out when it was issued (see ExecuteNow);
	// the queued voice line is then cosmetic and only triggers the reply.
	bool _executed = false;
	OLink<AIUnit> _confirmUnit;
	// the reply was sent (Oli: the speech bypass for player orders and the radio line finishing
	// both end in Transmitted; only the first may reply)
	bool _replied = false;

	//! issue the command to its recipients, return the unit that should reply
	AIUnit *Apply();

public:
	RadioMessageCommand(AIGroup *from, PackedBoolArray list, Command &command, bool toMainSubgroup = false, bool transmit = true);
	//! carry out the order now instead of waiting for the radio queue
	void ExecuteNow();
	RadioMessageCommand() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	int GetType() const override {return RMTCommand;}
	AIGroup *GetFrom() {return _from;}
	bool IsTo(AIUnit *unit);
	void DeleteTo(AIUnit *unit);
	bool IsToMainSubgroup() const {return _toMainSubgroup;}
	// Windows.h aliases `GetMessage` to GetMessageA/W — renamed to GetCmdMessage
	// so the method survives Windows-side compilation.
	Command::Message GetCmdMessage() {return _command._message;}
	Command::Context GetContext() {return _command._context;}
	AIUnit *GetSender() const override {return _from ? _from->Leader() : nullptr;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

class RadioMessageText : public RadioMessage
{
protected:
	RString _wave;
	OLink<AIUnit> _sender;
	RString _senderName;
	float _timeToLive;

public:
	RadioMessageText(RString wave, RString senderName, float ttl=2.0) {_wave = wave; _senderName = senderName; _timeToLive=ttl;}
	RadioMessageText(RString wave, AIUnit *sender, float ttl=2.0) {_wave = wave; _sender = sender; _timeToLive=ttl;}
	RadioMessageText() {}
	LSError Serialize(ParamArchive &ar) override;
	const char *GetPriorityClass() override;
	void Transmitted() override;
	RString GetWave() override;
	int GetType() const override {return RMTText;}
	float GetDuration() const override;
	AIUnit *GetSender() const override {return _sender;}
	RString GetSenderName() override {return _senderName;}

protected:
	const char *PrepareSentence(SentenceParams &params) override;
};

}  // namespace Poseidon
