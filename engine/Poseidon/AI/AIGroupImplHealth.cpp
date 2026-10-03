#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/AI/AIRadio.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>

#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Random/randomGen.hpp>
#include <stdio.h>

#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>

#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <float.h>
#include <stdio.h>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

#include <Poseidon/Game/UiActions.hpp>
#include <Poseidon/World/Entities/Infantry/MoveActions.hpp>
#include <Poseidon/AI/Path/AIDefs.hpp>
#pragma warning(disable : 4355)

using namespace Poseidon;
namespace Poseidon
{
using namespace Foundation;
using Foundation::EnumName;

// Parameters

#define COMMAND_TIMEOUT 480.0 // 8 min

#define FUEL_NEAR 100.0f
#define FUEL_FAR 500.0f

bool AIGroup::FindRefuelPosition(Command& cmd) const
{
    const AITargetInfo* target = FindRefuelPosition(AIUnit::RSCritical);
    if (!target)
    {
        return false;
    }

    cmd._destination = target->_realPos;
    cmd._target = target->_idExact;
    cmd._time = Glob.time + COMMAND_TIMEOUT;
    return true;
}

bool AIGroup::CheckFuel()
{
    AI_ERROR(Leader());
    if (IsAnyPlayerGroup())
    {
        return true;
    }

    bool ok = true;
    AIUnit::ResourceState stateMax = AIUnit::RSNormal;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        AIUnit::ResourceState state = GetFuelStateReported(unit);
        if (state == AIUnit::RSNormal)
        {
            continue;
        }

        ok = false;
        if (unit->GetCombatMode() == CMCareless)
        {
            continue;
        }
        // check both group and center radio channel
        if (CommandSent(unit, Command::Refuel, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Refuel, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Support))
        {
            continue;
        }
        if (state > stateMax)
        {
            stateMax = state;
        }
    }

    const AITargetInfo* target = FindRefuelPosition(stateMax);

    if (target)
    {
        bool channelCenter = false;
        EntityAI* veh = target->_idExact;
        if (veh)
        {
            AIUnit* unit = veh->CommanderUnit();
            if (unit && unit->GetLifeState() == AIUnit::LSAlive)
            {
                channelCenter = unit->GetGroup() != this;
            }
        }

        Command cmd;
        cmd._message = Command::Refuel;
        cmd._destination = target->_realPos;
        cmd._target = target->_idExact;
        cmd._time = Glob.time + COMMAND_TIMEOUT;

        for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
        {
            AIUnit* unit = _units[i];
            if (!unit)
            {
                continue;
            }
            if (unit->GetCombatMode() == CMCareless)
            {
                continue;
            }
            // check both group and center radio channel
            if (CommandSent(unit, Command::Refuel, false))
            {
                continue;
            }
            if (CommandSent(unit, Command::Refuel, true))
            {
                continue;
            }
            if (CommandSent(unit, Command::Support))
            {
                continue;
            }

            if (unit->GetFuelState() >= stateMax)
            {
                SendAutoCommandToUnit(cmd, unit, true, channelCenter);
            }
        }
    }
    else
    {
        if (stateMax == AIUnit::RSCritical && !IsAnyPlayerGroup())
        {
            AI_ERROR(_center);
            // FIX
            if (_center->GetRadio().Done() && _center->CanSupport(ATRefuel) &&
                !_center->WaitingForSupport(this, ATRefuel))
            {
                _center->GetRadio().Transmit(new RadioMessageSupportAsk(this, ATRefuel), _center->GetLanguage());
            }
        }
    }

    return ok;
}

#define REPAIR_NEAR 100.0f
#define REPAIR_FAR 500.0f

const AITargetInfo* AIGroup::FindRepairPosition(AIUnit::ResourceState state) const
{
    if (state == AIUnit::RSNormal)
    {
        return nullptr;
    }
    AI_ERROR(Leader());

    // find repair in close range
    const AITargetInfo* depot = nullptr;
    float dist2Depot = FLT_MAX;
    const AITargetInfo* truck = nullptr;
    float dist2Truck = FLT_MAX;

    for (int i = 0; i < GetCenter()->NTargets(); i++)
    {
        const AITargetInfo& info = GetCenter()->GetTarget(i);
        if (!info._type)
        {
            continue;
        }
        if (!(info._side == TCivilian) && !(GetCenter()->IsFriendly(info._side)))
        {
            continue;
        }
        if (info._type->GetMaxRepairCargo() <= 0)
        {
            continue;
        }
        VehicleSupply* veh = dyn_cast<VehicleSupply, Object>(info._idExact);
        if (!veh)
        {
            continue;
        }
        if (veh->GetRepairCargo() <= 0)
        {
            continue;
        }
        if (GetCenter()->GetExposurePessimistic(info._realPos) > 0)
        {
            continue;
        }
        float dist2 = (Leader()->Position() - info._realPos).SquareSizeXZ();

        if (info._type->IsKindOf(GLOB_WORLD->Preloaded(VTypeStatic)))
        {
            if (dist2 < dist2Depot)
            {
                dist2Depot = dist2;
                depot = &info;
            }
        }
        else
        {
            if (dist2 < dist2Truck)
            {
                dist2Truck = dist2;
                truck = &info;
            }
        }
    }

    if (depot && dist2Depot < Square(REPAIR_NEAR))
    {
        // repair in near depot
        return depot;
    }
    if (truck && dist2Truck < Square(REPAIR_NEAR))
    {
        // repair in near truck
        return truck;
    }

    if (state == AIUnit::RSLow)
    {
        return nullptr;
    }

    // find repair in wider range
    if (depot && dist2Depot < Square(REPAIR_FAR))
    {
        // repair in far depot
        return depot;
    }
    if (truck && dist2Truck < Square(REPAIR_FAR))
    {
        // repair in far truck
        return truck;
    }

    // repair not found
    return nullptr;
}

bool AIGroup::FindRepairPosition(Command& cmd) const
{
    const AITargetInfo* target = FindRepairPosition(AIUnit::RSCritical);
    if (!target)
    {
        return false;
    }

    cmd._destination = target->_realPos;
    cmd._target = target->_idExact;
    cmd._time = Glob.time + COMMAND_TIMEOUT;
    return true;
}

bool AIGroup::CheckArmor()
{
    AI_ERROR(Leader());
    if (IsAnyPlayerGroup())
    {
        return true;
    }

    bool ok = true;
    AIUnit::ResourceState stateMax = AIUnit::RSNormal;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        AIUnit::ResourceState state = GetDammageStateReported(unit);
        if (state == AIUnit::RSNormal)
        {
            continue;
        }

        ok = false;
        if (unit->GetCombatMode() == CMCareless)
        {
            continue;
        }
        // check both group and center radio channel
        if (CommandSent(unit, Command::Repair, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Repair, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Support))
        {
            continue;
        }

        if (state > stateMax)
        {
            stateMax = state;
        }
    }

    const AITargetInfo* target = FindRepairPosition(stateMax);

    if (target)
    {
        bool channelCenter = false;
        EntityAI* veh = target->_idExact;
        if (veh)
        {
            AIUnit* unit = veh->CommanderUnit();
            if (unit && unit->GetLifeState() == AIUnit::LSAlive)
            {
                channelCenter = unit->GetGroup() != this;
            }
        }

        Command cmd;
        cmd._message = Command::Repair;
        cmd._destination = target->_realPos;
        cmd._target = target->_idExact;
        cmd._time = Glob.time + COMMAND_TIMEOUT;

        for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
        {
            AIUnit* unit = _units[i];
            if (!unit)
            {
                continue;
            }
            if (unit->GetCombatMode() == CMCareless)
            {
                continue;
            }
            // check both group and center radio channel
            if (CommandSent(unit, Command::Repair, false))
            {
                continue;
            }
            if (CommandSent(unit, Command::Repair, true))
            {
                continue;
            }
            if (CommandSent(unit, Command::Support))
            {
                continue;
            }

            if (unit->GetArmorState() >= stateMax)
            {
                SendAutoCommandToUnit(cmd, unit, true, channelCenter);
            }
        }
    }
    else
    {
        if (stateMax == AIUnit::RSCritical && !IsAnyPlayerGroup())
        {
            AI_ERROR(_center);
            if (_center->GetRadio().Done() && _center->CanSupport(ATRepair) &&
                !_center->WaitingForSupport(this, ATRepair))
            {
                _center->GetRadio().Transmit(new RadioMessageSupportAsk(this, ATRepair), _center->GetLanguage());
            }
        }
    }

    return ok;
}

#define HOSPITAL_NEAR 100.0f
#define HOSPITAL_FAR 500.0f

const AITargetInfo* AIGroup::FindHealPosition(AIUnit::ResourceState state, AIUnit* unit) const
{
    if (state == AIUnit::RSNormal)
    {
        return nullptr;
    }
    AI_ERROR(Leader());

    // find hospital / ambulance in close range
    const AITargetInfo* depot = nullptr;
    float dist2Depot = FLT_MAX;
    const AITargetInfo* truck = nullptr;
    float dist2Truck = FLT_MAX;

    Vector3 pos = unit ? unit->Position() : Leader()->Position();

    for (int i = 0; i < GetCenter()->NTargets(); i++)
    {
        const AITargetInfo& info = GetCenter()->GetTarget(i);
        if (info._destroyed)
        {
            continue;
        }
        const VehicleType* type = info._idExact ? info._idExact->GetType() : info._type;
        if (!type)
        {
            continue;
        }
        if (!type->IsAttendant())
        {
            continue;
        }
        if (!(info._side == TCivilian) && !(GetCenter()->IsFriendly(info._side)))
        {
            continue;
        }
        if (unit && info._idExact == unit->GetPerson())
        {
            continue;
        }
        if (unit && info._idExact == unit->GetVehicleIn())
        {
            continue;
        }
        if (GetCenter()->GetExposureOptimistic(info._realPos) > 50.0f)
        {
            continue;
        }
        float dist2 = (pos - info._realPos).SquareSizeXZ();

        if (type->IsKindOf(GLOB_WORLD->Preloaded(VTypeStatic)))
        {
            if (dist2 < dist2Depot)
            {
                dist2Depot = dist2;
                depot = &info;
            }
        }
        else
        {
            if (dist2 < dist2Truck)
            {
                dist2Truck = dist2;
                truck = &info;
            }
        }
    }

    if (depot && dist2Depot < Square(HOSPITAL_NEAR))
    {
        // heal in near hospital
        return depot;
    }
    if (truck && dist2Truck < Square(HOSPITAL_NEAR))
    {
        // heal in near ambulance
        return truck;
    }

    if (state == AIUnit::RSLow)
    {
        return nullptr;
    }

    // find hospital / ambulance in wider range
    if (depot && dist2Depot < Square(HOSPITAL_FAR))
    {
        // heal in far hospital
        return depot;
    }
    if (truck && dist2Truck < Square(HOSPITAL_FAR))
    {
        // heal in far ambulance
        return truck;
    }

    // hospital / ambulance not found
    return nullptr;
}

bool AIGroup::FindHealPosition(Command& cmd) const
{
    const AITargetInfo* target = FindHealPosition(AIUnit::RSCritical);
    if (!target)
    {
        return false;
    }

    cmd._destination = target->_realPos;
    cmd._target = target->_idExact;
    cmd._time = Glob.time + COMMAND_TIMEOUT;
    return true;
}

void AIGroup::SendIsDown(Transport* vehicle, AIUnit* down)
{
    // any unit from the same vehicle should report if possible
    // check which units are alive
    // consider: better check here, with Report Status verification
    AIUnit* reportTransport = nullptr;
    AIUnit* report = nullptr;
    AIUnit* reportPlayer = nullptr;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        // check when was the unit last seen (or heard)
        // ignore units that are alive
        if (unit->GetLifeState() == AIUnit::LSAlive)
        {
            if (!reportTransport && unit->GetVehicle() == vehicle)
            {
                reportTransport = unit;
            }
            if (report)
            {
                continue;
            }
            if (unit->IsPlayer() || unit->GetPerson()->IsRemotePlayer())
            {
                if (!reportPlayer)
                {
                    reportPlayer = unit;
                }
                continue;
            }
            report = unit;
            continue;
        }
    }
    // lowest ID AI soldier should report casualties
    if (reportTransport)
    {
        report = reportTransport;
    }
    if (!report)
    {
        report = reportPlayer;
    }
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        if (unit->GetLifeState() != AIUnit::LSAlive)
        {
            if (report)
            {
                if (report->IsLocal())
                {
                    SendUnitDown(report, unit);
                }
            }
            else
            {
                if (unit->GetLifeState() == AIUnit::LSDead)
                {
                    AISubgroup* subgrp = unit->GetSubgroup();
                    if (subgrp)
                    {
                        subgrp->ReceiveAnswer(unit, AI::UnitDestroyed);
                    }
                }
            }
        }
    }
}

void AIGroup::SendIsDown(AIUnit* down)
{
    // lowest ID AI soldier should report casualties
    // autoselect sender
    // check which units are alive
    // consider: better check here, with Report Status verification
    AIUnit* report = nullptr;
    AIUnit* reportPlayer = nullptr;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        // check when was the unit last seen (or heard)
        // ignore units that are alive
        if (unit->GetLifeState() == AIUnit::LSAlive)
        {
            if (report)
            {
                continue;
            }
            if (unit->IsPlayer() || unit->GetPerson()->IsRemotePlayer())
            {
                if (!reportPlayer)
                {
                    reportPlayer = unit;
                }
                continue;
            }
            report = unit;
            continue;
        }
    }
    // lowest ID AI soldier should report casualties
    if (!report)
    {
        report = reportPlayer;
    }
    // for sure: verify unit is down
    if (down->GetLifeState() != AIUnit::LSAlive)
    {
        if (report)
        {
            if (report->IsLocal())
            {
                SendUnitDown(report, down);
            }
        }
        else
        {
            if (down->GetLifeState() == AIUnit::LSDead)
            {
                AISubgroup* subgrp = down->GetSubgroup();
                if (subgrp)
                {
                    subgrp->ReceiveAnswer(down, AI::UnitDestroyed);
                }
            }
        }
    }
}

void AIGroup::CheckAlive()
{
    // check which units are alive
    // consider: better check here, with Report Status verification
    AIUnit* leader = Leader();
    bool leaderAlive = leader && leader->GetLifeState() == AIUnit::LSAlive;
    if (leader && !leaderAlive)
    {
        // set max. time when to known leader is dead
        SetReportBeforeTime(leader, Glob.time + 30);
    }
    if ((!IsAnyPlayerGroup() || USER_CONFIG.IsEnabled(DTAutoSpot)) && leaderAlive)
    {
        // only AI groups or autospot players will ask for status
        for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
        {
            AIUnit* unit = _units[i];
            if (!unit)
            {
                continue;
            }
            // check when was the unit last seen (or heard)
            // ignore units that are alive
            if (unit->GetLifeState() == AIUnit::LSAlive)
            {
                continue;
            }
            // find corresponding target
            Target* tgt = FindTarget(unit->GetVehicle());
            if (!tgt || tgt->lastSeen < Glob.time - 50)
            {
                // ask unit to report status
                // check if report was not asked yet?
                if (GetReportBeforeTime(unit) > Glob.time + 30)
                {
                    // nobody asked about it - it is probably dead?
                    PackedBoolArray list;
                    list.Set(i, true);
                    SendReportStatus(list);
                }
            }
        }
    }
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        // only local unit can report
        if (GetReportBeforeTime(unit) < Glob.time)
        {
            SetReportBeforeTime(unit, TIME_MAX);
            SendIsDown(unit);
        }
    }
}

void AIGroup::CheckSupport()
{
    bool playerGroup = IsPlayerGroup();

    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit || !unit->IsUnit())
        {
            continue;
        }
        if (unit->IsPlayer() && playerGroup)
        {
            continue;
        }
        if (unit->GetCombatMode() == CMCareless)
        {
            continue;
        }
        if (playerGroup && unit->IsKeepingFormation())
        {
            continue;
        }

        if (CommandSent(unit, Command::Support))
        {
            continue;
        }
        if (CommandSent(unit, Command::Heal, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Heal, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Rearm, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Rearm, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Repair, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Repair, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Refuel, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Refuel, true))
        {
            continue;
        }

        VehicleSupply* veh = dyn_cast<VehicleSupply>(unit->GetVehicle());
        if (!veh)
        {
            continue;
        }

        EntityAI* target = veh->GetAllocSupply();
        if (!target)
        {
            float minDist2 = FLT_MAX;
            const OLinkArray<AIUnit>& supplyUnits = veh->GetSupplyUnits();
            for (int j = 0; j < supplyUnits.Size(); j++)
            {
                if (!supplyUnits[j])
                {
                    continue;
                }
                EntityAI* t = supplyUnits[j]->GetVehicle();
                float dist2 = t->Position().Distance2(veh->Position());
                if (dist2 < minDist2)
                {
                    target = t;
                    minDist2 = dist2;
                }
            }
        }
        if (!target)
        {
            continue;
        }

        if (target->Position().Distance2(veh->Position()) > Square(veh->GetPrecision() * 4))
        {
            Command cmd;
            cmd._message = Command::Support;
            cmd._destination = target->Position();
            cmd._target = target;
            cmd._time = Glob.time + COMMAND_TIMEOUT;
            if (unit->IsPlayer() || unit->IsKeepingFormation())
            {
                SendAutoCommandToUnit(cmd, unit, true);
            }
            else
            {
                NotifyAutoCommand(cmd, unit);
            }
        }
    }
}

bool AIGroup::CheckHealth()
{
    bool ok = true;

    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        if (unit->GetCombatMode() == CMCareless)
        {
            continue;
        }

        EntityAI* vehicle = unit->GetVehicle();
        if (vehicle->GetType()->IsAttendant())
        {
            // heal himself
            if (unit->GetHealthState() != AIUnit::RSNormal && !vehicle->CheckActionProcessing(ATHeal, unit))
            {
                UIAction action;
                action.type = ATHeal;
                action.target = vehicle;
                action.param = 0;
                action.param2 = 0;
                action.priority = 0;
                action.showWindow = false;
                action.hideOnUse = false;
                vehicle->StartActionProcessing(action, unit);
            }
            continue;
        }

        AIUnit::ResourceState state = GetHealthStateReported(unit);
        if (state == AIUnit::RSNormal)
        {
            continue;
        }

        ok = false;

        // check both group and center radio channel
        if (CommandSent(unit, Command::Heal, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Heal, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Support))
        {
            continue;
        }

        const AITargetInfo* target = FindHealPosition(state, unit);
        if (!target)
        {
            if (state == AIUnit::RSCritical && !IsAnyPlayerGroup())
            {
                AI_ERROR(_center);
                if (_center->GetRadio().Done() && _center->CanSupport(ATHeal) &&
                    !_center->WaitingForSupport(this, ATHeal))
                {
                    _center->GetRadio().Transmit(new RadioMessageSupportAsk(this, ATHeal), _center->GetLanguage());
                }
            }
            continue;
        }

        bool channelCenter = false;
        EntityAI* veh = target->_idExact;
        if (veh)
        {
            AIUnit* unit = veh->CommanderUnit();
            if (unit && unit->GetLifeState() == AIUnit::LSAlive)
            {
                channelCenter = unit->GetGroup() != this;
            }
        }

        Command cmd;
        cmd._message = Command::Heal;
        cmd._destination = target->_realPos;
        cmd._target = veh;
        cmd._time = Glob.time + COMMAND_TIMEOUT;
        SendAutoCommandToUnit(cmd, unit, true, channelCenter);
    }

    return ok;
}

#define AMMO_NEAR 100.0f
#define AMMO_FAR 500.0f

const AITargetInfo* AIGroup::FindRearmPosition(AIUnit::ResourceState state) const
{
    if (state == AIUnit::RSNormal)
    {
        return nullptr;
    }
    AI_ERROR(Leader());

    // find ammo in close range
    const AITargetInfo* depot = nullptr;
    float dist2Depot = FLT_MAX;
    const AITargetInfo* truck = nullptr;
    float dist2Truck = FLT_MAX;

    for (int i = 0; i < GetCenter()->NTargets(); i++)
    {
        const AITargetInfo& info = GetCenter()->GetTarget(i);
        if (info._destroyed)
        {
            continue;
        }
        if (!(info._side == TCivilian) && !(GetCenter()->IsFriendly(info._side)))
        {
            continue;
        }
        if (info._type->GetMaxAmmoCargo() <= 0)
        {
            continue;
        }
        VehicleSupply* veh = dyn_cast<VehicleSupply, Object>(info._idExact);
        if (!veh)
        {
            continue;
        }
        if (veh->GetAmmoCargo() <= 0)
        {
            continue;
        }
        if (GetCenter()->GetExposurePessimistic(info._realPos) > 0)
        {
            continue;
        }
        float dist2 = (Leader()->Position() - info._realPos).SquareSizeXZ();

        if (info._type->IsKindOf(GLOB_WORLD->Preloaded(VTypeStatic)))
        {
            if (dist2 < dist2Depot)
            {
                dist2Depot = dist2;
                depot = &info;
            }
        }
        else
        {
            if (dist2 < dist2Truck)
            {
                dist2Truck = dist2;
                truck = &info;
            }
        }
    }
    if (depot && dist2Depot < Square(AMMO_NEAR))
    {
        // rearm in near depot
        return depot;
    }
    if (truck && dist2Truck < Square(AMMO_NEAR))
    {
        // rearm in near truck
        return truck;
    }

    if (state == AIUnit::RSLow)
    {
        return nullptr;
    }

    // find ammo in wider range
    if (depot && dist2Depot < Square(AMMO_FAR))
    {
        // rearm in far depot
        return depot;
    }
    if (truck && dist2Truck < Square(AMMO_FAR))
    {
        // rearm in far truck
        return truck;
    }

    // ammo not found
    return nullptr;
}

bool AIGroup::FindRearmPosition(Command& cmd) const
{
    const AITargetInfo* target = FindRearmPosition(AIUnit::RSCritical);
    if (!target)
    {
        return false;
    }

    cmd._destination = target->_realPos;
    cmd._target = target->_idExact;
    cmd._time = Glob.time + COMMAND_TIMEOUT;
    return true;
}

bool AIGroup::CheckAmmo()
{
    AI_ERROR(Leader());
    if (IsAnyPlayerGroup())
    {
        return true;
    }

    bool ok = true;
    AIUnit::ResourceState stateMax = AIUnit::RSNormal;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        AIUnit::ResourceState state = GetAmmoStateReported(unit);
        if (state == AIUnit::RSNormal)
        {
            continue;
        }
        ok = false;

        if (unit->GetCombatMode() == CMCareless)
        {
            continue;
        }
        // check both group and center radio channel
        if (CommandSent(unit, Command::Rearm, false))
        {
            continue;
        }
        if (CommandSent(unit, Command::Rearm, true))
        {
            continue;
        }
        if (CommandSent(unit, Command::Support))
        {
            continue;
        }
        unit->CheckAmmo();
        if (state > stateMax)
        {
            stateMax = state;
        }
    }

    const AITargetInfo* target = FindRearmPosition(stateMax);
    if (target)
    {
        bool channelCenter = false;
        EntityAI* veh = target->_idExact;
        if (veh)
        {
            AIUnit* unit = veh->CommanderUnit();
            if (unit && unit->GetLifeState() == AIUnit::LSAlive)
            {
                channelCenter = unit->GetGroup() != this;
            }
        }

        Command cmd;
        cmd._message = Command::Rearm;
        cmd._destination = target->_realPos;
        cmd._target = veh;
        cmd._time = Glob.time + COMMAND_TIMEOUT;

        for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
        {
            AIUnit* unit = _units[i];
            if (!unit)
            {
                continue;
            }
            if (unit->GetCombatMode() == CMCareless)
            {
                continue;
            }
            // check both group and center radio channel
            if (CommandSent(unit, Command::Rearm, false))
            {
                continue;
            }
            if (CommandSent(unit, Command::Rearm, true))
            {
                continue;
            }
            if (CommandSent(unit, Command::Support))
            {
                continue;
            }

            if (GetAmmoStateReported(unit) >= stateMax)
            {
                SendAutoCommandToUnit(cmd, unit, true, channelCenter);
            }
        }
    }
    else
    {
        if (stateMax == AIUnit::RSCritical && !IsAnyPlayerGroup())
        {
            AI_ERROR(_center);
            if (_center->GetRadio().Done() && _center->CanSupport(ATRearm) &&
                !_center->WaitingForSupport(this, ATRearm))
            {
                _center->GetRadio().Transmit(new RadioMessageSupportAsk(this, ATRearm), _center->GetLanguage());
            }
        }
    }

    return ok;
}

float AIGroup::GetCost() const
{
    // note: this function is obsolete and needs some polishing
    // if it should be used again
    float cost = 0;
    for (int u = 0; u < MAX_UNITS_PER_GROUP; u++)
    {
        AIUnit* unit = UnitWithID(u + 1);
        if (!unit)
        {
            continue;
        }
        cost += unit->GetPerson()->GetType()->GetCost();
        if (!unit->IsSoldier())
        { // driver
            cost += unit->GetVehicle()->GetType()->GetCost();
        }
    }
    for (int v = 0; v < _vehicles.Size(); v++)
    {
        Transport* veh = _vehicles[v];
        if (!veh || veh->IsDammageDestroyed())
        {
            continue;
        }
        cost += veh->GetType()->GetCost();
    }
    return cost;
}

float AIGroup::EstimateTime(Vector3Val pos) const
{
    EntityAI* vehicle = nullptr;
    for (int v = 0; v < _vehicles.Size(); v++)
    {
        Transport* veh = _vehicles[v];
        if (!veh || veh->IsDammageDestroyed())
        {
            continue;
        }
        vehicle = veh;
        break;
    }
    if (!vehicle)
    {
        for (int u = 0; u < MAX_UNITS_PER_GROUP; u++)
        {
            AIUnit* unit = UnitWithID(u + 1);
            if (!unit || !unit->IsUnit())
            {
                continue;
            }
            vehicle = unit->GetVehicle();
            break;
        }
    }
    AI_ERROR(vehicle);
    AI_ERROR(Leader());
    float dist = (pos - Leader()->Position()).SizeXZ();
    float time = dist / (0.5 * vehicle->GetType()->GetTypSpeed() * (1000.0 / 60.0)); // speed km/h -> m/min

    return time;
}

RString AIGroup::GetDebugName() const
{
    char buffer[256];
    if (GetCenter())
    {
        snprintf(buffer, sizeof(buffer), "%s %s", (const char*)GetCenter()->GetDebugName(), GetName());
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%s %s", "new", GetName());
    }
    return buffer;
}

bool AIGroup::AssertValid() const
{
    bool result = true;
    if (NUnits() <= 0)
    {
        return true; // its valid now
    }
    else if (!Leader())
    {
        Fail("no leader");
        result = false;
    }
    else if (Leader()->GetGroup() == nullptr)
    {
        Fail("leader has no group");
        result = false;
    }
    else if (Leader()->GetGroup() != this)
    {
        Fail("leader from other group");
        result = false;
    }
    if (!MainSubgroup())
    {
        Fail("no main subgroup");
        result = false;
    }
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        if (unit->ID() != i + 1)
        {
            RptF("Group %s, id %d, should be %d", (const char*)GetDebugName(), unit->ID(), i + 1);
            Fail("unit with bad ID");
            result = false;
            // error recovery
            AIGroup* grp = const_cast<AIGroup*>(this);
            grp->_units[i] = nullptr;
            continue;
        }
        if (unit->GetGroup() == nullptr)
        {
            Fail("unit with no group");
            result = false;
        }
        else if (unit->GetGroup() != this)
        {
            Fail("unit with other group");
            result = false;
        }
    }
    for (int i = 0; i < _subgroups.Size(); i++)
    {
        AISubgroup* subgrp = _subgroups[i];
        if (!subgrp)
        {
            continue;
        }
        if (subgrp->GetGroup() == nullptr)
        {
            Fail("subgroup with no group");
            result = false;
        }
        else if (subgrp->GetGroup() != this)
        {
            Fail("subgroup with other group");
            result = false;
        }
        // check subgroup
        if (!subgrp->AssertValid())
        {
            result = false;
        }
    }
    return result;
}

void AIGroup::Dump(int indent) const {}

template <>
const EnumName* Foundation::GetEnumNames(Mission::Action dummy)
{
    static const EnumName MissionActionNames[] = {EnumName(Mission::NoMission, "NO MISSION"),
                                                  EnumName(Mission::Arcade, "ARCADE"),
                                                  EnumName(Mission::Arcade, "TRAINING"), EnumName()};
    return MissionActionNames;
}

LSError Mission::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.SerializeEnum("action", _action, 1, Mission::Arcade))
    PARAM_CHECK(ar.SerializeRef("Target", _target, 1))
    PARAM_CHECK(ar.Serialize("destination", _destination, 1))
    return LSOK;
}

namespace
{
RString GetLocalizedGroupDisplayName(RString letterName, RString colorName)
{
    if (letterName.GetLength() == 0)
        return RString();

    const ParamEntry* clsName = (Pars >> "CfgWorlds" >> "GroupNames").FindEntry(letterName);
    if (!clsName)
        return RString();

    RString displayName = (*clsName) >> "name";
    if (displayName.GetLength() == 0)
        displayName = letterName;

    if (colorName.GetLength() == 0)
        return displayName;

    const ParamEntry* clsColor = (Pars >> "CfgWorlds" >> "GroupColors").FindEntry(colorName);
    if (!clsColor)
        return displayName;

    RString displayColor = (*clsColor) >> "name";
    if (displayColor.GetLength() == 0)
        return displayName;

    return displayName + RString(" ") + displayColor;
}
} // namespace

void AIGroup::RefreshDisplayName()
{
    const RString localized = GetLocalizedGroupDisplayName(_letterName, _colorName);
    if (localized.GetLength() > 0)
        _name = localized;
    if (_nameCycle > 0 && _name.GetLength() > 0)
    {
        // more groups than letter x colour names: "Alpha Black 2", "Alpha Black 3", ...
        char suffix[16];
        snprintf(suffix, sizeof(suffix), " %d", _nameCycle + 1);
        _name = _name + RString(suffix);
    }
}

// Letter/colour names are handed out in order and wrap once all combinations are
// used; _nameCycle counts the wraps. Returns false when the name config is missing.
static bool AutoGroupName(int id, RString& letter, RString& color, int& cycle)
{
    const ParamEntry& letters = Pars >> "CfgWorlds" >> "GroupNameList" >> "letters";
    const ParamEntry& colors = Pars >> "CfgWorlds" >> "GroupColorList" >> "colors";
    const int nLetters = letters.GetSize();
    const int pool = nLetters * colors.GetSize();
    if (id <= 0 || pool <= 0)
        return false;
    const int i = id - 1;
    cycle = i / pool;
    const int j = i % pool;
    letter = letters[j % nLetters];
    color = colors[j / nLetters];
    return true;
}

const char* AIGroup::GetName() const
{
    const_cast<AIGroup*>(this)->RefreshDisplayName();
    return _name;
}

void AIGroup::Init()
{
    AI_ERROR(_id > 0);
    // ids are never reused within a mission, so they can pass MaxGroups even
    // when fewer groups exist; names simply wrap with a number suffix
    RString letter, color;
    int cycle = 0;
    if (!AutoGroupName(_id, letter, color, cycle))
    {
        return;
    }
    _letterName = letter;
    _colorName = color;
    _nameCycle = cycle;
    RefreshDisplayName();
}

void AIGroup::SetIdentity(RString name, RString color, RString picture)
{
    const ParamEntry* clsName = (Pars >> "CfgWorlds" >> "GroupNames").FindEntry(name);
    if (!clsName)
    {
        return;
    }
    const ParamEntry* clsColor = (Pars >> "CfgWorlds" >> "GroupColors").FindEntry(color);
    if (!clsColor)
    {
        return;
    }

    _letterName = name;
    _colorName = color;
    {
        // keep the "2", "3"... suffix when these are this group's own automatic
        // names (a loaded save); an explicit setGroupId name has no suffix
        RString autoLetter, autoColor;
        int cycle = 0;
        _nameCycle = (AutoGroupName(_id, autoLetter, autoColor, cycle) && autoLetter == _letterName &&
                      autoColor == _colorName)
                         ? cycle
                         : 0;
    }

    const RString oldName = _name;
    RefreshDisplayName();
    const RString fullname = _name;
    AICenter* center = GetCenter();
    if (center)
    {
        for (int i = 0; i < center->NGroups(); i++)
        {
            AIGroup* grp = center->GetGroup(i);
            if (!grp)
            {
                continue;
            }
            grp->RefreshDisplayName();
            if (grp->_name == fullname)
            {
                grp->_name = oldName;
                break;
            }
        }
    }
    _name = fullname;

    _pictureName = picture;
    _picture = nullptr;
    if (_pictureName.GetLength() > 0)
    {
        _picture = GlobLoadTexture(picture);
    }
}

// structure
void AIGroup::RemoveFromCenter()
{
    if (!IsLocal())
    {
        return;
    }

    AICenter* center = _center;
    if (center)
    {
        _center = nullptr;
        center->GroupRemoved(this);
    }
}

void AIGroup::ForceRemoveFromCenter()
{
    AICenter* center = _center;
    if (center)
    {
        _center = nullptr;
        center->GroupRemoved(this);
    }
}

// FSM virtuals
void AIGroup::DoUpdate()
{
    AIGroupContext context(this);
    Update(&context);
}

void AIGroup::DoRefresh()
{
    AIGroupContext context(this);
    UpdateAndRefresh(&context);
}

// communication with center
void AIGroup::ReceiveMission(Mission& mis)
{
#if LOG_COMM
    Log("Receive mission: Group %s: Mission %d at %.0f,%.0f", (const char*)GetDebugName(), mis._action,
        mis._destination.X(), mis._destination.Z());
#endif
    // repeat mission for members of my group

    AIGroupContext context(this);
    base::Clear();
    PushTask(mis, &context);
}

float AIGroup::ActualStrength() const
{
    float strength = 0;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        Person* person = unit->GetPerson();
        if (!person)
        {
            continue;
        }
        float armor = person->GetType()->GetArmor();
        float dammage = person->GetTotalDammage();
        armor *= (1.0 - dammage);
        strength += armor * InvSqrt(armor);
        Transport* veh = unit->GetVehicleIn();
        if (!veh)
        {
            continue;
        }
        if (veh->GetGroupAssigned() != this)
        { // transporting vehicle
            continue;
        }
        if (unit->IsUnit())
        {
            armor = veh->GetType()->GetArmor();
            dammage = veh->GetTotalDammage();
            armor *= (1.0 - dammage);
            strength += armor * InvSqrt(armor);
        }
    }
    return strength;
}

void AIGroup::CalculateMaximalStrength()
{
    _maxStrength = 0;
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* unit = _units[i];
        if (!unit)
        {
            continue;
        }
        float armor = unit->GetPerson()->GetType()->GetArmor();
        _maxStrength += armor * InvSqrt(armor);
        Transport* veh = unit->GetVehicleIn();
        if (!veh)
        {
            continue;
        }
        if (veh->GetGroupAssigned() != this)
        { // transporting vehicle
            continue;
        }
        if (unit->IsUnit())
        {
            armor = veh->GetType()->GetArmor();
            _maxStrength += armor * InvSqrt(armor);
        }
    }
}

void AIGroup::CalculateCourage()
{
    if (_forceCourage >= 0)
    {
        _courage = _forceCourage;
        return;
    }
    if (!Leader())
    {
        return;
    }
    _courage = Leader()->GetAbility();
}

extern void Flee(AIGroupContext* context);
extern void Unflee(AIGroupContext* context);

Vector3 FindFleePoint(AIGroup* group, bool& forced);

void AIGroup::Flee()
{
    bool forced;
    Vector3 pos = FindFleePoint(this, forced);
    // CWR fork: squared distance was compared with 50 (about 7 m); 50 m was meant
    if (!forced && pos.Distance2(Leader()->Position()) < Square(50))
    {
        // if we've got nowhere to flee to, do not flee
        return;
    }
    _flee = true;
    if (!GetCurrent())
    {
        return;
    }
    AIGroupContext context(this);
    context._task = GetCurrent()->_task;
    context._fsm = GetCurrent()->_fsm;
    ::Flee(&context);
}

void AIGroup::Unflee()
{
    _flee = false;
    if (!GetCurrent())
    {
        return;
    }
    AIGroupContext context(this);
    context._task = GetCurrent()->_task;
    context._fsm = GetCurrent()->_fsm;
    ::Unflee(&context);
}

AIUnit::ResourceState AIGroup::GetHealthStateReported(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _healthState[unit->ID() - 1];
}

AIUnit::ResourceState AIGroup::GetAmmoStateReported(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _ammoState[unit->ID() - 1];
}

AIUnit::ResourceState AIGroup::GetFuelStateReported(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _fuelState[unit->ID() - 1];
}
AIUnit::ResourceState AIGroup::GetDammageStateReported(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _dammageState[unit->ID() - 1];
}
AIUnit::ResourceState AIGroup::GetWorstStateReported(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    int i = unit->ID() - 1;
    AIUnit::ResourceState state = _ammoState[i];
    if (state < _healthState[i])
    {
        state = _healthState[i];
    }
    if (state < _ammoState[i])
    {
        state = _ammoState[i];
    }
    if (state < _fuelState[i])
    {
        state = _fuelState[i];
    }
    if (state < _dammageState[i])
    {
        state = _dammageState[i];
    }
    return state;
}

void AIGroup::SetHealthStateReported(AIUnit* unit, AIUnit::ResourceState state)
{
    AI_ERROR(unit->GetGroup() == this);
    _healthState[unit->ID() - 1] = state;
}
void AIGroup::SetAmmoStateReported(AIUnit* unit, AIUnit::ResourceState state)
{
    AI_ERROR(unit->GetGroup() == this);
    _ammoState[unit->ID() - 1] = state;
}
void AIGroup::SetFuelStateReported(AIUnit* unit, AIUnit::ResourceState state)
{
    AI_ERROR(unit->GetGroup() == this);
    _fuelState[unit->ID() - 1] = state;
}
void AIGroup::SetDammageStateReported(AIUnit* unit, AIUnit::ResourceState state)
{
    AI_ERROR(unit->GetGroup() == this);
    _dammageState[unit->ID() - 1] = state;
}

bool AIGroup::GetReportedDown(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _reportedDown[unit->ID() - 1];
}

void AIGroup::SetReportedDown(AIUnit* unit, bool state)
{
    AI_ERROR(unit->GetGroup() == this);
    _reportedDown[unit->ID() - 1] = state;
}

Time AIGroup::GetReportBeforeTime(AIUnit* unit)
{
    AI_ERROR(unit->GetGroup() == this);
    return _reportBeforeTime[unit->ID() - 1];
}

void AIGroup::SetReportBeforeTime(AIUnit* unit, Time time)
{
    AI_ERROR(unit->GetGroup() == this);
    if (_reportedDown[unit->ID() - 1])
    {
        return;
    }
    Time& tgt = _reportBeforeTime[unit->ID() - 1];
    if (tgt > time)
    {
        tgt = time;
    }
}

} // namespace Poseidon
