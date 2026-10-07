#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/AI/Path/AIDefs.hpp>

#include <Poseidon/AI/AIRadio.hpp>

#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Config/CombatAssistSettings.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/UI/Locale/Languages.hpp>
#include <Poseidon/UI/Locale/Sentences.hpp>
#include <Poseidon/Game/UiActions.hpp>

#include <ctype.h>

#include <Poseidon/Game/Chat.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <float.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/IO/ParamFile/ParamFile.hpp>

using namespace Poseidon;
void AddEnemyInfo(TargetType* id, const VehicleType* type, int x, int z);

namespace Poseidon
{
using Foundation::EnumName;
void PositionToAA11(Vector3Val pos, char* buffer);
void SetTeam(int i, Team team);
} // namespace Poseidon

namespace Poseidon
{
PackedBoolArray GetUnitsList(AIGroup* grp, OLinkArray<AIUnit>& units);
PackedBoolArray PrepareList(AIGroup* grp, PackedBoolArray list, bool wholeCrew);
void ShowGroupDir(AIGroup* to);
void ShowGroupDir(OLinkArray<AIUnit>& to);
bool UnitAlive(AIUnit* unit);

const float coefAa11 = 100.0 / (256 * 50);
const float invCoefAa11 = 1.0 / coefAa11;

void RadioMessageReturnToFormation::Transmitted()
{
    AIUnit* sender = GetSender();
    if (!sender || !_to)
    {
        return;
    }

    if (_to == GWorld->FocusOn() && GWorld->UI())
    {
        GWorld->UI()->ShowFormPosition();
    }
}

const char* RadioMessageReturnToFormation::PrepareSentence(SentenceParams& params)
{
    AIUnit* sender = GetSender();
    if (!sender || !_to)
    {
        return nullptr;
    }

    SetPlayerMsg(_to == GWorld->FocusOn());

    params.Add(_to);
    params.Add(sender);

    return "SentReturnToFormation";
}

RadioMessageReply::RadioMessageReply(AIUnit* from, AIGroup* to)
{
    _from = from;
    _to = to;
}

LSError RadioMessageReply::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(RadioMessage::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRef("To", _to, 1))
    return LSOK;
}

RadioMessageReportTarget::RadioMessageReportTarget(AIUnit* from, AIGroup* to, ReportSubject subject, Target& target)
    : base(from, to)
{
    _subject = subject;

    _x = toIntFloor(coefAa11 * target.position.X());
    _z = toIntFloor(coefAa11 * target.position.Z());
    _targets.Resize(1);
    _targets[0].tgt = &target;
}

LSError RadioMessageReportTarget::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRef("To", _to, 1))
    PARAM_CHECK(ar.SerializeEnum("subject", _subject, 1, ReportNew))
    PARAM_CHECK(ar.Serialize("x", _x, 1))
    PARAM_CHECK(ar.Serialize("z", _z, 1))
    PARAM_CHECK(ar.Serialize("Targets", _targets, 1))
    return LSOK;
}

const char* RadioMessageReportTarget::GetPriorityClass()
{
    return "Detected";
}

const char* RadioMessageReportTarget::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to || !_to->Leader())
    {
        return nullptr;
    }
    AICenter* center = _to->GetCenter();
    if (!center)
    {
        return nullptr;
    }
    if (!GWorld->CameraOn())
    {
        return nullptr;
    }

    AI_ERROR(_subject == ReportNew);
    AI_ERROR(_targets.Size() == 1);

    ReportTargetInfo& target = _targets[0];
    Target* tgt = target.tgt;
    if (!tgt)
    {
        return nullptr;
    }
    if (tgt->side == center->GetSide())
    {
        return nullptr;
    }

    SetPlayerMsg(_from == GWorld->FocusOn());

    const float tgtLeaderDist2 = _to->Leader()->Position().Distance2(tgt->position);
    const float untLeaderDist2 = _to->Leader()->Position().Distance2(_from->Position());
    const bool farReport = untLeaderDist2 > Square(100) && untLeaderDist2 > tgtLeaderDist2 * 0.75f;

    // Adapted from the GPL-3.0 donated Physical Inventory fork. Only presentation
    // changes: retain the reported target position/knowledge and distant grid reports.
    // A freefly camera is not the listener; bearings and range use the player's unit.
    if (!farReport && GWorld->GetMode() != GModeNetware && BearingCalloutsEnabled())
    {
        AIUnit* listener = GWorld->FocusOn();
        if (!listener)
        {
            listener = _to->Leader();
        }
        const Vector3 relative = tgt->position - listener->Position();
        const float distance = relative.Size();
        const float degrees = std::atan2(relative.X(), relative.Z()) * (180.0f / H_PI);
        if (std::isfinite(degrees) && std::isfinite(distance))
        {
            const int bearing = static_cast<int>(std::floor(degrees + 360.5f)) % 360;
            if (tgt->side == TSideUnknown)
            {
                params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
            }
            else if (center->IsEnemy(tgt->side))
            {
                params.AddWord("enemy", LocalizeString(IDS_WORD_ENEMY));
            }
            else if (center->IsFriendly(tgt->side))
            {
                params.AddWord("friendly", LocalizeString(IDS_WORD_FRIENDLY));
            }
            else
            {
                params.AddWord("neutral", LocalizeString(IDS_WORD_NEUTRAL));
            }
            params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());
            // No custom voice asset: AIRadio speaks stock "at" + three stock digits.
            params.AddWord("bearing", "Bearing");
            params.Add(bearing);
            params.AddDistance(distance);
            return "$BEARING$";
        }
    }

    const char* sentence = nullptr;

    params.Add(_from);

    params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());

    if (farReport)
    {
        // square in map
        char buffer[6];
        PositionToAA11(tgt->position, buffer);
        params.Add(RString(buffer));

        if (tgt->side == TCivilian)
        {
            sentence = "SentEnemyDetectedSimpleFar";
        }
        else
        {
            sentence = "SentEnemyDetectedFar"; // add side
        }
    }
    else // near
    {
        // direction
        params.AddAzimut(tgt->position);
        ::ShowGroupDir(_to);

        if (tgt->side == TCivilian)
        {
            sentence = "SentEnemyDetectedSimple";
        }
        else
        {
            sentence = "SentEnemyDetected"; // add side
        }
    }

    if (tgt->side == TSideUnknown)
    {
        params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
    }
    else if (_to->GetCenter()->IsEnemy(tgt->side))
    {
        params.AddWord("enemy", LocalizeString(IDS_WORD_ENEMY));
    }
    else if (_to->GetCenter()->IsFriendly(tgt->side))
    {
        params.AddWord("friendly", LocalizeString(IDS_WORD_FRIENDLY));
    }
    else
    {
        params.AddWord("neutral", LocalizeString(IDS_WORD_NEUTRAL));
    }

    float distance = _from->Position().Distance(tgt->position);
    params.AddDistance(distance);
    return sentence;
}

void RadioMessageReportTarget::Transmitted()
{
    if (_to && UnitAlive(_from))
    {
        if (_to->IsPlayerGroup())
        {
            for (int i = 0; i < _targets.Size(); i++)
            {
                Target* tgt = _targets[i].tgt;
                if (tgt && _to->GetCenter()->IsEnemy(tgt->side))
                {
                    AddEnemyInfo(tgt->idExact, tgt->type, _x, _z);
                }
            }
        }
    }
}

bool RadioMessageReportTarget::IsTarget(TargetType* id) const
{
    for (int i = 0; i < _targets.Size(); i++)
    {
        const ReportTargetInfo& target = _targets[i];
        if (target.tgt && target.tgt->idExact == id)
        {
            return true;
        }
    }
    return false;
}

bool RadioMessageReportTarget::HasType(const VehicleType* type) const
{
    for (int i = 0; i < _targets.Size(); i++)
    {
        const ReportTargetInfo& target = _targets[i];
        if (target.tgt && target.tgt->type == type)
        {
            return true;
        }
    }
    return false;
}

bool RadioMessageReportTarget::IsInside(Vector3Val pos) const
{
    int x = toIntFloor(coefAa11 * pos.X());
    if (x != _x)
    {
        return false;
    }
    int z = toIntFloor(coefAa11 * pos.Z());
    return z == _z;
}

void RadioMessageReportTarget::DeleteTarget(TargetType* id)
{
    int i = 0;
    while (i < _targets.Size())
    {
        ReportTargetInfo& target = _targets[i];
        if (target.tgt && target.tgt->idExact == id)
        {
            _targets.Delete(i);
            continue;
        }
        i++;
    }
}

void RadioMessageReportTarget::AddTarget(Target& target)
{
    int index = _targets.Add();
    _targets[index].tgt = &target;
}

RadioMessageNotifyCommand::RadioMessageNotifyCommand(AIUnit* from, AIGroup* to, Command& command) : base(from, to)
{
    _command = command;
}

LSError RadioMessageNotifyCommand::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("Command", _command, 1))
    return LSOK;
}

const char* RadioMessageNotifyCommand::GetPriorityClass()
{
    return "Notify";
}

const char* RadioMessageNotifyCommand::PrepareSentence(SentenceParams& params)
{
    if (!UnitAlive(_from) || !_to)
    {
        return nullptr;
    }

    SetPlayerMsg(_from == GWorld->FocusOn());

    const char* sentence = nullptr;
    params.Add(_from);

    switch (_command._message)
    {
        case Command::Attack:
#if ENABLE_HOLDFIRE_FIX
        case Command::AttackAndFire:
#endif
        {
            sentence = "SentNotifyAttack";

            Target* tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->GetGroup()->FindTarget(_command._target);
            }
            if (tgt)
            {
                params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());
            }
            else
            {
                params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
            }
        }
        break;
        case Command::Support:
        {
            VehicleWithAI* target = _command._target;
            if (!target)
            {
                return nullptr;
            }

            sentence = "SentNotifySupport";
            params.AddWord(target->GetType()->GetNameSound(), target->GetType()->GetDisplayName());
        }
        break;
    }

    return sentence;
}

void RadioMessageNotifyCommand::Transmitted() {}

RadioMessageObjectDestroyed::RadioMessageObjectDestroyed(AIUnit* from, AIGroup* to, const VehicleType* type)
    : base(from, to)
{
    _vehicleType = type;
}

LSError RadioMessageObjectDestroyed::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(Poseidon::Serialize(ar, "vehicleType", _vehicleType, 1))
    return LSOK;
}

const char* RadioMessageObjectDestroyed::GetPriorityClass()
{
    return "Completition";
}

void RadioMessageObjectDestroyed::Transmitted() {}

const char* RadioMessageObjectDestroyed::PrepareSentence(SentenceParams& params)
{
    if (!_to || !UnitAlive(_from))
    {
        return nullptr;
    }

    SetPlayerMsg(_from == GWorld->FocusOn());

    params.Add(_from);

    if (_vehicleType)
    {
        params.AddWord(_vehicleType->GetNameSound(), _vehicleType->GetDisplayName());
        return "SentObjectDestroyed";
    }
    else
    {
        return "SentObjectDestroyedUnknown";
    }
}

RadioMessageContact::RadioMessageContact(AIUnit* from, AIGroup* to) : base(from, to) {}

LSError RadioMessageContact::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageContact::GetPriorityClass()
{
    return "UrgentCommand";
}

void RadioMessageContact::Transmitted()
{
    if (!_to)
    {
        return;
    }
}

const char* RadioMessageContact::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to)
    {
        return nullptr;
    }

    params.Add(_from);

    return "SentContact";
}

RadioMessageUnderFire::RadioMessageUnderFire(AIUnit* from, AIGroup* to) : base(from, to) {}

LSError RadioMessageUnderFire::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageUnderFire::GetPriorityClass()
{
    return "UrgentCommand";
}

void RadioMessageUnderFire::Transmitted()
{
    if (!_to)
    {
        return;
    }
}

const char* RadioMessageUnderFire::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to)
    {
        return nullptr;
    }

    SetPlayerMsg(_from == GWorld->FocusOn());

    params.Add(_from);

    return "SentUnderFire";
}

RadioMessageClear::RadioMessageClear(AIUnit* from, AIGroup* to) : base(from, to) {}

LSError RadioMessageClear::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageClear::GetPriorityClass()
{
    return "NormalCommand";
}

void RadioMessageClear::Transmitted() {}

const char* RadioMessageClear::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to)
    {
        return nullptr;
    }

    params.Add(_from);

    return "SentClear";
}

RadioMessageRepeatCommand::RadioMessageRepeatCommand(AIUnit* from, AIGroup* to) : base(from, to) {}

LSError RadioMessageRepeatCommand::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageRepeatCommand::GetPriorityClass()
{
    return "NormalCommand";
}

void RadioMessageRepeatCommand::Transmitted()
{
    if (!_from || !_to || !_to->Leader())
    {
        return;
    }
    AISubgroup* subgrp = _from->GetSubgroup();
    if (!subgrp)
    {
        return;
    }
    Command* cmd = subgrp->GetCommand();
    if (!cmd)
    {
        return;
    }
    _to->GetRadio().Transmit(new RadioMessageCommand(_to, subgrp->GetUnitsList(), *cmd, false, false),
                             _to->GetCenter()->GetLanguage());
}

const char* RadioMessageRepeatCommand::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to)
    {
        return nullptr;
    }

    params.Add(_from);

    return "SentRepeatCommand";
}

RadioMessageWhereAreYou::RadioMessageWhereAreYou(AIUnit* from, AIGroup* to) : base(from, to) {}

LSError RadioMessageWhereAreYou::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageWhereAreYou::GetPriorityClass()
{
    return "NormalCommand";
}

void RadioMessageWhereAreYou::Transmitted()
{
    if (!_to)
    {
        return;
    }
    AIUnit* leader = _to->Leader();
    if (!leader || leader->IsAnyPlayer())
    {
        return;
    }
    if (leader->GetLifeState() != AIUnit::LSAlive)
    {
        _to->SetReportBeforeTime(leader, Glob.time + 10);
    }
    else
    {
        leader->SendAnswer(AI::ReportPosition);
    }
}

const char* RadioMessageWhereAreYou::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to)
    {
        return nullptr;
    }

    params.Add(_from);

    return "SentWhereAreYou";
}

RadioMessageCommandConfirm::RadioMessageCommandConfirm(AIUnit* from, AIGroup* to, Command& command) : base(from, to)
{
    _command = command;
}

LSError RadioMessageCommandConfirm::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("Command", _command, 1))
    return LSOK;
}

const char* RadioMessageCommandConfirm::GetPriorityClass()
{
    return "Confirmation";
}

const char* RadioMessageCommandConfirm::PrepareSentence(SentenceParams& params)
{
    if (!_from || _from->GetLifeState() != AIUnit::LSAlive)
    {
        return nullptr;
    }

    SetPlayerMsg(_from == GWorld->FocusOn());

    params.Add(_from);
    if (_from->GetPerson()->GetRank() == RankPrivate)
    {
        return "SentConfirmPrivate";
    }
    else if (_command._message == Command::Attack || _command._message == Command::AttackAndFire)
    {
        return "SentConfirmAttack";
    }
    else if (_command._message == Command::Move)
    {
        return "SentConfirmMove";
    }
    else
    {
        return "SentConfirmOther";
    }
}

void RadioMessageCommandConfirm::Transmitted()
{
    if (!_from)
    {
        return;
    }
    if (UnitAlive(_from))
    {
        return;
    }
    // unit should report, but does not
    AIGroup* grp = _from->GetGroup();
    if (grp)
    {
        grp->SetReportBeforeTime(_from, Glob.time + 5);
    }
}

LSError RadioMessageFormation::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(RadioMessage::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRef("To", _to, 1))
    PARAM_CHECK(ar.SerializeEnum("formation", _formation, 1, AI::FormLine))
    return LSOK;
}

const char* RadioMessageFormation::GetPriorityClass()
{
    return "NormalCommand";
}

const char* RadioMessageFormation::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }

    const char* sentence = nullptr;

    params.Add(_from->Leader());
    switch (_formation)
    {
        case AI::FormColumn:
            sentence = "SentFormColumn";
            break;
        case AI::FormStaggeredColumn:
            sentence = "SentFormStaggeredColumn";
            break;
        case AI::FormWedge:
            sentence = "SentFormWedge";
            break;
        case AI::FormEcholonLeft:
            sentence = "SentFormEcholonLeft";
            break;
        case AI::FormEcholonRight:
            sentence = "SentFormEcholonRight";
            break;
        case AI::FormVee:
            sentence = "SentFormVee";
            break;
        default:
        case AI::FormLine:
            sentence = "SentFormLine";
            break;
    }

    return sentence;
}

void RadioMessageFormation::Transmitted()
{
    if (_to)
    {
        _to->SetFormation(_formation);
    }
}

RadioMessageState::RadioMessageState(AIGroup* from, PackedBoolArray list)
{
    _from = from;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        if (list.Get(i))
        {
            AIUnit* unit = from->UnitWithID(i + 1);
            if (unit)
            {
                _to.Add(unit);
            }
        }
    }
}

RadioMessageState::RadioMessageState() = default;

bool RadioMessageState::IsTo(AIUnit* unit) const
{
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (u == unit)
        {
            return true;
        }
    }
    return false;
}

bool RadioMessageState::IsOnlyTo(AIUnit* unit) const
{
    bool ret = false;
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (!u)
        {
            continue;
        }
        if (u != unit)
        {
            return false;
        }
        else
        {
            ret = true;
        }
    }
    return ret;
}

void RadioMessageState::DeleteTo(AIUnit* unit)
{
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (u == unit)
        {
            _to.Delete(i);
            return;
        }
    }
}

void RadioMessageState::ClearTo()
{
    _to.Clear();
}

void RadioMessageState::AddTo(AIUnit* unit)
{
    AI_ERROR(!IsTo(unit));
    _to.Add(unit);
}

bool RadioMessageState::IsToSomeone() const
{
    for (int i = 0; i < _to.Size(); i++)
    {
        if (_to[i])
        {
            return true;
        }
    }
    return false;
}

const char* RadioMessageState::GetPriorityClass()
{
    return "NormalCommand";
}

LSError RadioMessageState::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRefs("To", _to, 1))
    return LSOK;
}

RadioMessageSemaphore::RadioMessageSemaphore(AIGroup* from, PackedBoolArray list, AI::Semaphore semaphore)
    : base(from, list)
{
    _semaphore = semaphore;
}

LSError RadioMessageSemaphore::Serialize(ParamArchive& ar)
{
    return LSOK;
}

const char* RadioMessageSemaphore::GetPriorityClass()
{
    return "Default";
}

const char* RadioMessageSemaphore::PrepareSentence(SentenceParams& params)
{
    return nullptr;
}

void RadioMessageSemaphore::Transmitted() {}

RadioMessageBehaviour::RadioMessageBehaviour(AIGroup* from, PackedBoolArray list, CombatMode behaviour)
    : base(from, list)
{
    _behaviour = behaviour;
}

LSError RadioMessageBehaviour::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeEnum("behaviour", _behaviour, 1, CMAware))
    return LSOK;
}

const char* RadioMessageBehaviour::GetPriorityClass()
{
    return "NormalCommand";
}

const char* RadioMessageBehaviour::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    params.Add(_to, true);
    params.Add(_from->Leader());
    switch (_behaviour)
    {
        case CMCareless:
            return "SentBehaviourCareless";
        case CMSafe:
            return "SentBehaviourSafe";
        case CMAware:
            return "SentBehaviourAware";
        case CMCombat:
            return "SentBehaviourCombat";
        case CMStealth:
            return "SentBehaviourStealth";
        default:
            Fail("Behaviour");
            return nullptr;
    }
}

void RadioMessageBehaviour::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (unit)
        {
            unit->SetCombatModeMajor(_behaviour);
        }
    }
}

template <>
const EnumName* Foundation::GetEnumNames(OpenFireState dummy)
{
    static const EnumName OpenFireNames[] = {EnumName(OFSNeverFire, "NEVER FIRE"), EnumName(OFSHoldFire, "HOLD FIRE"),
                                             EnumName(OFSOpenFire, "OPEN FIRE"),   EnumName(OFSHoldFire, "0"),
                                             EnumName(OFSOpenFire, "1"),           EnumName()};
    return OpenFireNames;
}

RadioMessageOpenFire::RadioMessageOpenFire(AIGroup* from, PackedBoolArray list, OpenFireState open) : base(from, list)
{
    _open = open;
}

LSError RadioMessageOpenFire::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeEnum("open", _open, 1, OFSOpenFire))
    return LSOK;
}

const char* RadioMessageOpenFire::GetPriorityClass()
{
    switch (_open)
    {
        case OFSNeverFire:
        case OFSHoldFire:
            return "UrgentCommand";
        default:
            Fail("Open fire status");
        case OFSOpenFire:
            return "NormalCommand";
    }
}

const char* RadioMessageOpenFire::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    params.Add(_to, true);
    params.Add(_from->Leader());
    switch (_open)
    {
        case OFSNeverFire:
        case OFSHoldFire:
            if (_from->GetCombatModeMinor() >= CMCombat)
            {
                return "SentHoldFireInCombat";
            }
            else
            {
                return "SentHoldFire";
            }
        default:
            Fail("Open Fire State");
        case OFSOpenFire:
            if (_from->GetCombatModeMinor() >= CMCombat)
            {
                return "SentOpenFireInCombat";
            }
            else
            {
                return "SentOpenFire";
            }
    }
}

AI::Semaphore ApplyOpenFire(AI::Semaphore s, OpenFireState open)
{
    switch (s)
    {
        case AI::SemaphoreBlue:
            switch (open)
            {
                case OFSHoldFire:
                    s = AI::SemaphoreGreen;
                    break;
                case OFSOpenFire:
                    s = AI::SemaphoreYellow;
                    break;
            }
            break;
        case AI::SemaphoreGreen:
            switch (open)
            {
                case OFSNeverFire:
                    s = AI::SemaphoreBlue;
                    break;
                case OFSOpenFire:
                    s = AI::SemaphoreYellow;
                    break;
            }
            break;
        case AI::SemaphoreWhite:
            switch (open)
            {
                case OFSNeverFire:
                    s = AI::SemaphoreBlue;
                    break;
                case OFSOpenFire:
                    s = AI::SemaphoreRed;
                    break;
            }
            break;
        case AI::SemaphoreYellow:
            switch (open)
            {
                case OFSNeverFire:
                    s = AI::SemaphoreBlue;
                    break;
                case OFSHoldFire:
                    s = AI::SemaphoreGreen;
                    break;
            }
            break;
        case AI::SemaphoreRed:
            switch (open)
            {
                case OFSNeverFire:
                    s = AI::SemaphoreBlue;
                    break;
                case OFSHoldFire:
                    s = AI::SemaphoreWhite;
                    break;
            }
            break;
    }
    return s;
}

void RadioMessageOpenFire::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (unit)
        {
            AI::Semaphore s = unit->GetSemaphore();
            s = ApplyOpenFire(s, _open);
            unit->SetSemaphore(s);
        }
    }
}

RadioMessageCeaseFire::RadioMessageCeaseFire(AIUnit* from, AIUnit* to, bool insideGroup)
{
    _from = from;
    _to = to;
    _insideGroup = insideGroup;
}

LSError RadioMessageCeaseFire::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("from", _from, 1))
    PARAM_CHECK(ar.SerializeRef("to", _to, 1))
    PARAM_CHECK(ar.Serialize("insideGroup", _insideGroup, 1, true))
    return LSOK;
}

const char* RadioMessageCeaseFire::GetPriorityClass()
{
    return "UrgentCommand";
}

const char* RadioMessageCeaseFire::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_to || !_to->GetGroup())
    {
        return nullptr;
    }
    if (_from->GetLifeState() != AIUnit::LSAlive)
    {
        return nullptr;
    }
    SetPlayerMsg(_to == GWorld->FocusOn());

    if (_insideGroup)
    {
        params.Add(_to);
        return "SentCeaseFireInsideGroup";
    }
    else
    {
        params.AddWord("", _to->GetGroup()->GetName());
        params.Add(_to);
        return "SentCeaseFire";
    }
}

void RadioMessageCeaseFire::Transmitted() {}

RadioMessageLooseFormation::RadioMessageLooseFormation(AIGroup* from, PackedBoolArray list, bool loose)
    : base(from, list)
{
    _loose = loose;
}

LSError RadioMessageLooseFormation::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("loose", _loose, 1, false))
    return LSOK;
}

const char* RadioMessageLooseFormation::GetPriorityClass()
{
    return "NormalCommand";
}

const char* RadioMessageLooseFormation::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    params.Add(_to, true);
    params.Add(_from->Leader());
    if (_loose)
    {
        return "SentLooseFormation";
    }
    else
    {
        return "SentKeepFormation";
    }
}

AI::Semaphore ApplyLooseFormation(AI::Semaphore s, bool loose)
{
    switch (s)
    {
        case AI::SemaphoreBlue:
            if (loose)
            {
                s = AI::SemaphoreWhite;
            }
            break;
        case AI::SemaphoreGreen:
            if (loose)
            {
                s = AI::SemaphoreWhite;
            }
            break;
        case AI::SemaphoreWhite:
            if (!loose)
            {
                s = AI::SemaphoreGreen;
            }
            break;
        case AI::SemaphoreYellow:
            if (loose)
            {
                s = AI::SemaphoreRed;
            }
            break;
        case AI::SemaphoreRed:
            if (!loose)
            {
                s = AI::SemaphoreYellow;
            }
            break;
    }
    return s;
}

void RadioMessageLooseFormation::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (unit)
        {
            AI::Semaphore s = unit->GetSemaphore();
            s = ApplyLooseFormation(s, _loose);
            unit->SetSemaphore(s);
        }
    }
}

template <>
const EnumName* Foundation::GetEnumNames(AI::FormationPos dummy)
{
    static const EnumName FormationPosNames[] = {EnumName(UPUp, "Up"), EnumName(UPDown, "Down"),
                                                 EnumName(UPAuto, "Auto"), EnumName()};
    return FormationPosNames;
}

RadioMessageFormationPos::RadioMessageFormationPos(AIGroup* from, PackedBoolArray list, AI::FormationPos state)
    : base(from, list)
{
    _state = state;
}

LSError RadioMessageFormationPos::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeEnum("state", _state, 1, AI::PosInFormation))
    return LSOK;
}

const char* RadioMessageFormationPos::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence;

    params.Add(_to, true);
    params.Add(_from->Leader());
    switch (_state)
    {
        case AI::PosAdvance:
            sentence = "SentFormPosAdvance";
            break;
        case AI::PosStayBack:
            sentence = "SentFormPosStayBack";
            break;
        case AI::PosFlankLeft:
            sentence = "SentFormPosFlankLeft";
            break;
        case AI::PosFlankRight:
            sentence = "SentFormPosFlankRight";
            break;
        default:
            Fail("FormationPos");
            return nullptr;
    }

    return sentence;
}

void RadioMessageFormationPos::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (unit)
        {
            unit->AddFormationPos(_state);
        }
    }
}

template <>
const EnumName* Foundation::GetEnumNames(UnitPosition dummy)
{
    static const EnumName UnitPosNames[] = {EnumName(UPUp, "Up"), EnumName(UPDown, "Down"), EnumName(UPAuto, "Auto"),
                                            EnumName()};
    return UnitPosNames;
}

RadioMessageUnitPos::RadioMessageUnitPos(AIGroup* from, PackedBoolArray list, UnitPosition state) : base(from, list)
{
    _state = state;
}

LSError RadioMessageUnitPos::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeEnum("state", _state, 1, (UnitPosition)UPAuto))
    return LSOK;
}

const char* RadioMessageUnitPos::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence;

    params.Add(_to, true);
    params.Add(_from->Leader());
    switch (_state)
    {
        case UPDown:
            sentence = "SentUnitPosDown";
            break;
        case UPUp:
            sentence = "SentUnitPosUp";
            break;
        case UPAuto:
            sentence = "SentUnitPosAuto";
            break;
        default:
            Fail("UnitPos");
            return nullptr;
    }

    return sentence;
}

void RadioMessageUnitPos::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (unit)
        {
            unit->SetUnitPosition(_state);
        }
    }
}

RadioMessageWatchAround::RadioMessageWatchAround(AIGroup* from, PackedBoolArray list) : base(from, list) {}

LSError RadioMessageWatchAround::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

void RadioMessageWatchAround::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        unit->SetWatchAround();
    }
}

const char* RadioMessageWatchAround::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence = "SentWatchAround";
    params.Add(_to, false);
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageWatchAuto::RadioMessageWatchAuto(AIGroup* from, PackedBoolArray list) : base(from, list) {}

LSError RadioMessageWatchAuto::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

void RadioMessageWatchAuto::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        unit->SetNoWatch();
    }
}

const char* RadioMessageWatchAuto::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence = "SentNoTarget";
    params.Add(_to, false);
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageWatchDir::RadioMessageWatchDir(AIGroup* from, PackedBoolArray list, Vector3Val dir) : base(from, list)
{
    _dir = dir;
}

LSError RadioMessageWatchDir::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("dir", _dir, 1))
    return LSOK;
}

void RadioMessageWatchDir::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        unit->SetWatchDirection(_dir);
    }
}

const char* RadioMessageWatchDir::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence = "SentWatchDir";
    params.Add(_to, false);
    params.AddAzimutDir(_dir);
    ::ShowGroupDir(_to);
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageWatchPos::RadioMessageWatchPos(AIGroup* from, PackedBoolArray list, Vector3Val pos) : base(from, list)
{
    _pos = pos;
}

LSError RadioMessageWatchPos::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("pos", _pos, 1))
    return LSOK;
}

void RadioMessageWatchPos::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        unit->SetWatchPosition(_pos);
    }
}

const char* RadioMessageWatchPos::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence = "SentWatchPos";
    params.Add(_to, false);
    params.AddAzimut(_pos);
    ::ShowGroupDir(_to);
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageWatchTgt::RadioMessageWatchTgt(AIGroup* from, PackedBoolArray list, Target* tgt) : base(from, list)
{
    _tgt = tgt;
}

LSError RadioMessageWatchTgt::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("tgt", _tgt, 1))
    return LSOK;
}

void RadioMessageWatchTgt::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        unit->SetWatchTarget(_tgt);
    }
}

const char* RadioMessageWatchTgt::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence = "SentWatchTgt";
    params.Add(_to, false);

    Target* tgt = _tgt;
    if (tgt)
    {
        params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());
    }
    else
    {
        params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
    }
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageReportStatus::RadioMessageReportStatus(AIGroup* from, PackedBoolArray list) : base(from, list) {}

LSError RadioMessageReportStatus::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

const char* RadioMessageReportStatus::GetPriorityClass()
{
    return "NormalCommand";
}

const char* RadioMessageReportStatus::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    const char* sentence;

    sentence = "SentReportStatus";
    params.Add(_to, true);
    params.Add(_from->Leader());

    return sentence;
}

void RadioMessageReportStatus::Transmitted()
{
    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* unit = _to[i];
        if (!unit)
        {
            continue;
        }
        if (unit->GetLifeState() == AIUnit::LSAlive)
        {
            unit->ReportStatus();
            AIGroup* grp = unit->GetGroup();
            if (grp)
            {
                // reset status indication here
                // if it should be set, unit will report it soon
                grp->SetHealthStateReported(unit, AIUnit::RSNormal);
                grp->SetDammageStateReported(unit, AIUnit::RSNormal);
                grp->SetFuelStateReported(unit, AIUnit::RSNormal);
                grp->SetAmmoStateReported(unit, AIUnit::RSNormal);
            }
        }
        else
        {
            AIGroup* grp = unit->GetGroup();
            if (grp)
            {
                grp->SetReportBeforeTime(unit, Glob.time + 5);
            }
        }
    }
}

RadioMessageTarget::RadioMessageTarget()
{
    _engage = false;
    _fire = false;
}

RadioMessageTarget::RadioMessageTarget(AIGroup* from, PackedBoolArray list, Target* target, bool engage, bool fire)
    : base(from, list)
{
    _target = target;
    _engage = engage;
    _fire = fire;
}

LSError RadioMessageTarget::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("target", _target, 1))
    PARAM_CHECK(ar.Serialize("engage", _engage, 1, false))
    PARAM_CHECK(ar.Serialize("fire", _fire, 1, false))
    return LSOK;
}

void RadioMessageTarget::Transmitted()
{
    if (_target && (_target->destroyed || _target->vanished))
    {
        return;
    }
    if (_target)
    {
        if (IsTo(GWorld->FocusOn()) && GWorld->UI())
        {
            GWorld->UI()->ShowTarget();
        }
    }
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* to = _to[i];
        if (!to)
        {
            continue;
        }
        if (_target)
        {
            VehicleWithAI* tgtAI = _target->idExact;
            if (to->GetPerson()->IsRemotePlayer())
            {
                GetNetworkManager().ShowTarget(to->GetPerson(), tgtAI);
            }
            if (tgtAI == to->GetVehicleIn() || tgtAI == to->GetPerson())
            {
                // unit cannot target itself
                continue;
            }
        }
        if (_engage)
        {
            Target* tgt = _target ? (Target*)_target : to->GetTargetAssigned();
            to->EngageTarget(tgt);
        }
        if (_fire)
        {
            Target* tgt = _target ? (Target*)_target : to->GetTargetAssigned();
            to->EnableFireTarget(tgt);
        }
        if (_target)
        {
            to->AssignTarget(_target);
            VehicleWithAI* veh = to->GetVehicle();
            veh->BegAttack(_target);
        }
    }
}

void RadioMessageTarget::Canceled()
{
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* to = _to[i];
        if (!to)
        {
            continue;
        }
        to->GetGroup()->UnitAssignCanceled(to);
    }
}

const char* RadioMessageTarget::GetPriorityClass()
{
    return "UrgentCommand";
}

const char* RadioMessageTarget::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    Target* target = _target;
    const char* sentence = "SentNoTarget";
    if (_engage && _fire)
    {
        sentence = "SentAttackNoTarget";
    }
    else if (_engage)
    {
        sentence = "SentEngageNoTarget";
    }
    else if (_fire)
    {
        sentence = "SentFireNoTarget";
    }
    params.Add(_to, false);

    if (target)
    {
        sentence = "SentTarget";
        if (_engage && _fire)
        {
            sentence = "SentCmdAttack";
        }
        else if (_engage)
        {
            sentence = "SentEngage";
        }
        else if (_fire)
        {
            sentence = "SentFire";
        }
    }

    if (target)
    {
        if (target->destroyed || target->vanished)
        {
            return nullptr;
        }
        params.AddWord(target->type->GetNameSound(), target->type->GetDisplayName());
    }
    else
    {
        params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
    }
    params.Add(_from->Leader());
    return sentence;
}

RadioMessageTeam::RadioMessageTeam(AIGroup* from, PackedBoolArray list, Team team)
{
    _from = from;
    int i;
    for (i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        if (list.Get(i))
        {
            AIUnit* unit = from->UnitWithID(i + 1);
            if (unit)
            {
                _to.Add(unit);
            }
        }
    }
    _team = team;
}

LSError RadioMessageTeam::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(RadioMessage::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRefs("To", _to, 1))
    PARAM_CHECK(ar.SerializeEnum("team", _team, 1, TeamMain))
    return LSOK;
}

const char* RadioMessageTeam::GetPriorityClass()
{
    return "NormalCommand";
}

void RadioMessageTeam::Transmitted()
{
    for (int i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (!u)
        {
            continue;
        }
        if (u->GetGroup() != _from)
        {
            continue;
        }
        SetTeam(u->ID() - 1, _team);
    }
}

bool RadioMessageTeam::IsTo(AIUnit* unit) const
{
    AI_ERROR(unit);

    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (u == unit)
        {
            return true;
        }
    }
    return false;
}

const char* RadioMessageTeam::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader())
    {
        return nullptr;
    }
    _to.Compact();
    if (_to.Size() <= 0)
    {
        return nullptr;
    }

    SetPlayerMsg(IsTo(GWorld->FocusOn()));

    params.Add(_to, false);
    switch (_team)
    {
        case TeamMain:
            params.AddWord("whiteTeam", LocalizeString(IDS_WORD_TEAM_MAIN));
            break;
        case TeamRed:
            params.AddWord("redTeam", LocalizeString(IDS_WORD_TEAM_RED));
            break;
        case TeamGreen:
            params.AddWord("greenTeam", LocalizeString(IDS_WORD_TEAM_GREEN));
            break;
        case TeamBlue:
            params.AddWord("blueTeam", LocalizeString(IDS_WORD_TEAM_BLUE));
            break;
        case TeamYellow:
            params.AddWord("yellowTeam", LocalizeString(IDS_WORD_TEAM_YELLOW));
            break;
    }
    params.Add(_from->Leader());
    return "SentTeam";
}

RadioMessageCommand::RadioMessageCommand(AIGroup* from, PackedBoolArray list, Command& command, bool toMainSubgroup,
                                         bool transmit)
{
    _from = from;
    int i;
    for (i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        if (list.Get(i))
        {
            AIUnit* unit = from->UnitWithID(i + 1);
            if (unit)
            {
                _to.Add(unit);
            }
        }
    }
    _toMainSubgroup = toMainSubgroup;
    _transmit = transmit;
    _command = command;
}

LSError RadioMessageCommand::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(RadioMessage::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("From", _from, 1))
    PARAM_CHECK(ar.SerializeRefs("To", _to, 1))
    PARAM_CHECK(ar.Serialize("Command", _command, 1))
    PARAM_CHECK(ar.Serialize("toMainSubgrp", _toMainSubgroup, 1, false))
    PARAM_CHECK(ar.Serialize("transmit", _transmit, 1, true))
    PARAM_CHECK(ar.Serialize("executed", _executed, 1, false))
    PARAM_CHECK(ar.SerializeRef("confirmUnit", _confirmUnit, 1))
    return LSOK;
}

const char* RadioMessageCommand::GetPriorityClass()
{
    if (_command._context == Command::CtxUI || _command._context == Command::CtxUIWithJoin)
    {
        return "UICommand";
    }
    else
    {
        switch (_command._message)
        {
            case Command::Move:
            case Command::Heal:
            case Command::Repair:
            case Command::Refuel:
            case Command::Rearm:
            case Command::Support:
            case Command::GetIn:
            case Command::GetOut:
            case Command::Join:
                return "NormalCommand";
            case Command::Attack:
#if ENABLE_HOLDFIRE_FIX
            case Command::AttackAndFire:
#endif
            case Command::Fire:
                return "UrgentCommand";
            default:
                return "Default";
        }
    }
}

bool RadioMessageCommand::IsTo(AIUnit* unit)
{
    // unit may be nullptr in MP

    if (_toMainSubgroup)
    {
        return _from && unit->GetSubgroup() == _from->MainSubgroup();
    }

    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (u == unit)
        {
            return true;
        }
    }
    return false;
}

void RadioMessageCommand::DeleteTo(AIUnit* unit)
{
    AI_ERROR(unit);

    if (_toMainSubgroup)
    {
        return;
    }

    int i;
    for (i = 0; i < _to.Size(); i++)
    {
        AIUnit* u = _to[i];
        if (u == unit)
        {
            _to.Delete(i);
            return;
        }
    }
}

Object* FindNearestMoveObject(Vector3Par pos)
{
    if (!GWorld->CameraOn())
    {
        return nullptr;
    }
    float dist2 = pos.Distance2(GWorld->CameraOn()->Position());
    if (dist2 > Square(400))
    {
        return nullptr;
    }
    float diff = 0.25 * sqrt(dist2);
    saturate(diff, 5.0, 100.0);
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, diff);

    Object* best = nullptr;
    const float minDist2 = diff * diff;
    float minFunc = FLT_MAX;
    for (int x = xMin; x <= xMax; x++)
    {
        for (int z = zMin; z <= zMax; z++)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            int n = list.Size();
            for (int i = 0; i < n; i++)
            {
                Object* obj = list[i];
                if (obj->GetType() == Network)
                {
                    continue;
                }
                if (obj->GetType() == Temporary)
                {
                    continue;
                }
                if (obj->GetType() == TypeTempVehicle)
                {
                    continue;
                }
                if (!obj->IsMoveTarget())
                {
                    continue;
                }
                float dist2 = pos.Distance2(obj->Position());
                if (dist2 >= minDist2)
                {
                    continue;
                }
                float func = dist2 * obj->GetInvMass();
                if (func >= minFunc)
                {
                    continue;
                }
                minFunc = func;
                best = obj;
            }
        } // (for all near objects)
    }
    return best;
}

const char* RadioMessageCommand::PrepareSentence(SentenceParams& params)
{
    if (!_from || !_from->Leader() || _from->Leader()->GetLifeState() != AIUnit::LSAlive ||
        _command._message == Command::NoCommand || _command._message == Command::Wait ||
        _command._context == Command::CtxUndefined || _command._context == Command::CtxJoin)
    {
        return nullptr;
    }

    if (!_command._target && !_command._targetE &&
        (_command._message == Command::Fire || _command._message == Command::Attack ||
         _command._message == Command::AttackAndFire))
    {
        return nullptr;
    }
    if (!_command._target && (_command._message == Command::GetIn || _command._message == Command::Heal ||
                              _command._message == Command::Repair || _command._message == Command::Refuel ||
                              _command._message == Command::Rearm || _command._message == Command::Support))
    {
        return nullptr;
    }

    const char* sentence;
    AIUnit* first = nullptr;
    if (_toMainSubgroup)
    {
        params.AddWord("allGroup", LocalizeString(IDS_WORD_ALLGROUP));
        first = _from->MainSubgroup()->Leader();
        if (!first)
        {
            first = _from->Leader();
        }
    }
    else
    {
        _to.Compact();
        if (_to.Size() <= 0)
        {
            return nullptr;
        }
        bool wholeCrew = _command._message != Command::GetIn && _command._message != Command::GetOut;
        params.Add(_to, wholeCrew);
        first = _to[0];

        SetPlayerMsg(IsTo(GWorld->FocusOn()));
    }
    AI_ERROR(first);

    // say command
    Target* tgt = nullptr;
    switch (_command._message)
    {
        case Command::Join:
        {
            AISubgroup* subgroup = _command._joinToSubgroup;
            if (!subgroup)
            {
                subgroup = _from->MainSubgroup();
            }
            AI_ERROR(subgroup);
            if (!subgroup->Leader())
            {
                sentence = "SentCmdFollow";
                params.AddWord("allGroup", LocalizeString(IDS_WORD_ALLGROUP));
            }
            else if (subgroup->Leader() == _from->Leader())
            {
                sentence = "SentCmdFollowMe";
            }
            else
            {
                sentence = "SentCmdFollow";
                params.Add(subgroup->Leader());
            }
        }
        break;
        case Command::GetOut:
            sentence = "SentCmdGetOut";
            break;
        case Command::Move:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdMoveTo";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdMoveFar";
                goto CmdPositionFar;
            }
            else
            {
                Object* obj = FindNearestMoveObject(_command._destination);
                if (obj)
                {
                    sentence = "SentCmdMoveNear";
                    params.AddWord(obj->GetNameSound(), obj->GetDisplayName());
                    params.AddAzimut(obj->Position());
                    ::ShowGroupDir(_to);
                    break;
                }
                else
                {
                    sentence = "SentCmdMove";
                    goto CmdPosition;
                }
            }
        case Command::Stop:
            sentence = "SentCmdStop";
            break;
        case Command::Expect:
            sentence = "SentCmdExpect";
            break;
        case Command::Hide:
            sentence = "SentCmdHide";
            break;
        case Command::Heal:
        Heal:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdHealAt";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdHealFar";
                goto CmdPositionFar;
            }
            else
            {
                sentence = "SentCmdHeal";
                goto CmdPosition;
            }
        case Command::Repair:
        Repair:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdRepairAt";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdRepairFar";
                goto CmdPositionFar;
            }
            else
            {
                sentence = "SentCmdRepair";
                goto CmdPosition;
            }
        case Command::Refuel:
        Refuel:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdRefuelAt";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdRefuelFar";
                goto CmdPositionFar;
            }
            else
            {
                sentence = "SentCmdRefuel";
                goto CmdPosition;
            }
        case Command::Rearm:
        Rearm:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdRearmAt";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdRearmFar";
                goto CmdPositionFar;
            }
            else
            {
                sentence = "SentCmdRearm";
                goto CmdPosition;
            }
        case Command::Support:
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }
            if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
            {
                sentence = "SentCmdSupportAt";
                goto CmdSupplyTarget;
            }
            else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
            {
                sentence = "SentCmdSupportFar";
                goto CmdPositionFar;
            }
            else
            {
                sentence = "SentCmdSupport";
                goto CmdPosition;
            }
        case Command::Attack:
#if ENABLE_HOLDFIRE_FIX
        case Command::AttackAndFire:
#endif
            sentence = "SentCmdAttack";
            goto CmdTarget;
        case Command::Fire:
            sentence = "SentCmdFire";
            goto CmdTarget;
        case Command::GetIn:
            sentence = "SentCmdGetIn";
            if (!_toMainSubgroup)
            {
                if (_to.Size() == 1)
                {
                    Transport* veh = first->VehicleAssigned();
                    if (veh == _command._target)
                    {
                        if (first == veh->GetCommanderAssigned())
                        {
                            sentence = "SentCmdGetInCommander";
                        }
                        else if (first == veh->GetDriverAssigned())
                        {
                            if (veh->GetType()->IsKindOf(GWorld->Preloaded(VTypeAir)))
                            {
                                sentence = "SentCmdGetInPilot";
                            }
                            else
                            {
                                sentence = "SentCmdGetInDriver";
                            }
                        }
                        else if (first == veh->GetGunnerAssigned())
                        {
                            sentence = "SentCmdGetInGunner";
                        }
                        else
                        {
                            for (int i = 0; i < veh->NCargoAssigned(); i++)
                            {
                                if (first == veh->GetCargoAssigned(i))
                                {
                                    sentence = "SentCmdGetInCargo";
                                    break;
                                }
                            }
                        }
                    }
                }
                else
                {
                    // check if all units gets in cargo
                    bool ok = true;
                    Transport* veh = first->VehicleAssigned();
                    if (veh)
                    {
                        for (int j = 0; j < _to.Size(); j++)
                        {
                            AIUnit* unit = _to[j];
                            if (unit->VehicleAssigned() != veh)
                            {
                                ok = false;
                                break;
                            }
                            bool found = false;
                            for (int i = 0; i < veh->NCargoAssigned(); i++)
                            {
                                if (unit == veh->GetCargoAssigned(i))
                                {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found)
                            {
                                ok = false;
                                break;
                            }
                        }
                    }
                    else
                    {
                        ok = false;
                    }
                    if (ok)
                    {
                        sentence = "SentCmdGetInCargo";
                    }
                }
            }
            goto CmdTarget;
        case Command::Action:
        {
            switch (_command._action)
            {
                case ATHeal:
                    goto Heal;
                case ATRepair:
                    goto Repair;
                case ATRefuel:
                    goto Refuel;
                case ATRearm:
                    goto Rearm;
                case ATTakeWeapon:
                    tgt = _command._targetE;
                    if (!tgt)
                    {
                        tgt = _from->FindTarget(_command._target);
                    }
                    if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
                    {
                        sentence = "SentCmdTakeWeaponAt";
                        goto CmdSupplyTarget;
                    }
                    else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
                    {
                        sentence = "SentCmdTakeWeaponFar";
                        goto CmdPositionFar;
                    }
                    else
                    {
                        sentence = "SentCmdTakeWeapon";
                        goto CmdPosition;
                    }
                case ATTakeMagazine:
                    tgt = _command._targetE;
                    if (!tgt)
                    {
                        tgt = _from->FindTarget(_command._target);
                    }
                    if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
                    {
                        sentence = "SentCmdTakeMagazineAt";
                        goto CmdSupplyTarget;
                    }
                    else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
                    {
                        sentence = "SentCmdTakeMagazineFar";
                        goto CmdPositionFar;
                    }
                    else
                    {
                        sentence = "SentCmdTakeMagazine";
                        goto CmdPosition;
                    }
                default:
                {
                    UIAction action;
                    action.type = _command._action;
                    action.target = _command._target;
                    action.param = _command._param;
                    action.param2 = _command._param2;
                    action.param3 = _command._param3;
                    action.priority = 0;
                    action.showWindow = false;
                    action.hideOnUse = false;
                    RString name = action.GetDisplayName(nullptr);
                    params.AddWord("", name);

                    tgt = _command._targetE;
                    if (!tgt)
                    {
                        tgt = _from->FindTarget(_command._target);
                    }
                    if (tgt && tgt->type->GetDisplayName().GetLength() > 0)
                    {
                        sentence = "SentCmdActionAt";
                        goto CmdSupplyTarget;
                    }
                    else if (_command._destination.Distance2(_from->Leader()->Position()) > Square(400))
                    {
                        sentence = "SentCmdActionFar";
                        goto CmdPositionFar;
                    }
                    else
                    {
                        sentence = "SentCmdAction";
                        goto CmdPosition;
                    }
                }
            }
        }
        default:
            Fail("Unknown command.");
            return nullptr;
        CmdPosition:
        {
            Vector3 dir = _command._destination - first->Position();
            int azimut = toInt(0.1 * atan2(dir.X(), dir.Z()) * (180 / H_PI));
            if (azimut < 0)
            {
                azimut += 36;
            }
            int dist = toInt(0.1 * dir.SizeXZ());
            params.Add("%02d", azimut);
            params.Add(dist);
        }
        break;
        CmdPositionFar:
        {
            char buffer[6];
            PositionToAA11(_command._destination, buffer);
            params.Add(RString(buffer));
        }
        break;
        CmdSupplyTarget:
        {
            params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());
            params.AddAzimut(tgt->position);
            ::ShowGroupDir(_to);
        }
        break;
        CmdTarget:
        {
            tgt = _command._targetE;
            if (!tgt)
            {
                tgt = _from->FindTarget(_command._target);
            }

            // check if we should use center
            if (tgt)
            {
                params.AddWord(tgt->type->GetNameSound(), tgt->type->GetDisplayName());
            }
            else
            {
                // FIX tgt not found - use center database
                AICenter* center = _from->GetCenter();
                AITargetInfo* aiTgt = center->FindTargetInfo(_command._target);
                if (aiTgt)
                {
                    params.AddWord(aiTgt->_type->GetNameSound(), aiTgt->_type->GetDisplayName());
                }
                else
                {
                    params.AddWord("unknown", LocalizeString(IDS_WORD_UNKNOWN));
                }
            }
        }
        break;
    }
    params.Add(_from->Leader());

    return sentence;
}

AIUnit* RadioMessageCommand::Apply()
{
    if (!_transmit)
    {
        return nullptr;
    }
    // The serialized flag also guards later audio completion and reentrant confirmations.
    _transmit = false;

    if (!_from)
    {
        return nullptr;
    }
    if (!_from->Leader())
    {
        return nullptr;
    }
    if (_from->Leader()->GetLifeState() != AIUnit::LSAlive)
    {
        return nullptr;
    }

    if (!_command._target && !_command._targetE &&
        (_command._message == Command::Attack || _command._message == Command::AttackAndFire ||
         _command._message == Command::Fire))
    {
        return nullptr;
    }
    if (!_command._target && (_command._message == Command::GetIn || _command._message == Command::Heal ||
                              _command._message == Command::Repair || _command._message == Command::Refuel ||
                              _command._message == Command::Rearm || _command._message == Command::Support))
    {
        return nullptr;
    }

    bool confirm = _command._message != Command::NoCommand && _command._message != Command::Wait &&
                   _command._context != Command::CtxUndefined && _command._context != Command::CtxJoin;

    AIUnit* unit = nullptr;
    if (_toMainSubgroup)
    {
        _from->MainSubgroup()->ReceiveCommand(_command);
        if (confirm)
        {
            unit = _from->MainSubgroup()->Leader();
            if (!unit)
            {
                unit = _from->Leader();
            }
        }
    }
    else
    {
        if (_to.Count() > 0)
        {
            bool wholeCrew = _command._message != Command::GetIn && _command._message != Command::GetOut;
            PackedBoolArray list = PrepareList(_from, GetUnitsList(_from, _to), wholeCrew);
            if (!list.IsEmpty())
            {
                _from->IssueCommand(_command, list);
                if (confirm)
                {
                    unit = nullptr;
                    for (int i = 0; i < _to.Size(); i++)
                    {
                        unit = _to[i];
                        if (unit)
                        {
                            break;
                        }
                    }
                    AI_ERROR(unit);
                }
            }
        }
    }

    return unit;
}

void RadioMessageCommand::ExecuteNow()
{
    if (_executed)
    {
        return;
    }
    _executed = true;
    _confirmUnit = Apply();
}

void RadioMessageCommand::Transmitted()
{
    // Normally the order was already carried out when it was issued, so the
    // radio line only has to be followed by the unit's reply.
    if (_replied)
    {
        return;
    }
    _replied = true;
    AIUnit* confirmed = _confirmUnit;
    AIUnit* unit = _executed ? confirmed : Apply();
    if (!unit || !_from)
    {
        return;
    }
    if (!unit->IsAnyPlayer())
    {
        _from->GetRadio().Transmit(new RadioMessageCommandConfirm(unit, _from, _command),
                                   _from->GetCenter()->GetLanguage());
    }
}

LSError RadioMessageText::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(RadioMessage::Serialize(ar))
    PARAM_CHECK(ar.Serialize("wave", _wave, 1))
    PARAM_CHECK(ar.Serialize("ttl", _timeToLive, 1))
    PARAM_CHECK(ar.SerializeRef("sender", _sender, 1))
    PARAM_CHECK(ar.Serialize("senderName", _senderName, 1, RString()))

    return LSOK;
}

const char* RadioMessageText::GetPriorityClass()
{
    return "Design";
}

const char* RadioMessageText::PrepareSentence(SentenceParams& params)
{
    return nullptr;
}

void RadioMessageText::Transmitted() {}

RString RadioMessageText::GetWave()
{
    return _wave;
}

float RadioMessageText::GetDuration() const
{
    return _timeToLive;
}
} // namespace Poseidon
