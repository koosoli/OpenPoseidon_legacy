#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Weapons/ProxyWeapon.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/Dev/Diag/DiagModes.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/UI/Locale/Sentences.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/Game/UiActions.hpp>
#include <Poseidon/World/Simulation/FrameInv.hpp>
#include <Poseidon/World/Entities/Infantry/MoveActions.hpp>
#include <Poseidon/World/Entities/Infantry/ManActs.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <float.h>
#include <cmath>
#include <utility>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>

namespace Poseidon
{
using namespace Foundation;

static const float CmdPlanDist = 30.0f;
static const float CmdMinDist = 10.0f;
const float DriverReactionTime = 0.3f;

float Transport::NeedsLoadFuel() const
{
    if (IsDammageDestroyed())
    {
        return 0;
    }
    float max = GetType()->GetMaxFuelCargo();
    if (max <= 0)
    {
        return 0;
    }
    return 1 - GetFuelCargo() / max;
}

float Transport::NeedsLoadAmmo() const
{
    if (IsDammageDestroyed())
    {
        return 0;
    }
    float max = GetType()->GetMaxAmmoCargo();
    if (max <= 0)
    {
        return 0;
    }
    return 1 - GetAmmoCargo() / max;
}

float Transport::NeedsLoadRepair() const
{
    if (IsDammageDestroyed())
    {
        return 0;
    }
    float max = GetType()->GetMaxRepairCargo();
    if (max <= 0)
    {
        return 0;
    }
    return 1 - GetRepairCargo() / max;
}

bool Transport::IsPossibleToGetIn() const
{
    if (_isDead)
    {
        return false;
    }
    if (_isUpsideDown)
    {
        return false;
    }
    if (IsDammageDestroyed())
    {
        return false;
    }
    return true;
}

bool Transport::IsWorking() const
{
    if (_isDead)
    {
        return false;
    }
    if (_isUpsideDown)
    {
        return false;
    }
    if (IsDammageDestroyed())
    {
        return false;
    }
    return true;
}

bool Transport::IsAbleToMove() const
{
    Person* driver = Driver();
    if (driver)
    {
        AIUnit* unit = driver->Brain();
        if (!unit && !_driverAssigned)
        {
            return false;
        }
    }
    return IsWorking();
}
bool Transport::IsAbleToFire() const
{
    if (GetType()->NMagazines() == 0)
    {
        return false;
    }
    return IsWorking();
}

float Transport::CalculateTotalCost() const
{
    float cost = GetType()->GetCost();
    if (_driver)
    {
        cost += _driver->GetType()->GetCost();
    }
    if (_commander)
    {
        cost += _commander->GetType()->GetCost();
    }
    if (_gunner)
    {
        cost += _gunner->GetType()->GetCost();
    }
    for (int i = 0; i < _manCargo.Size(); i++)
    {
        if (_manCargo[i])
        {
            cost += _manCargo[i]->GetType()->GetCost();
        }
    }
    return cost;
}

bool Transport::QCanIBeIn(Person* who) const
{
    if (!IsWorking())
    {
        return false;
    }
    return true;
}

RString Transport::GetActionName(const UIAction& action)
{
    switch (action.type)
    {
        case ATMoveToDriver:
            if (GetType()->IsKindOf(GWorld->Preloaded(VTypeAir)))
            {
                return LocalizeString(IDS_ACTION_TO_PILOT);
            }
            else
            {
                return LocalizeString(IDS_ACTION_TO_DRIVER);
            }
        case ATMoveToGunner:
            return LocalizeString(IDS_ACTION_TO_GUNNER);
        case ATMoveToCommander:
            return LocalizeString(IDS_ACTION_TO_COMMANDER);
        case ATMoveToCargo:
            return LocalizeString(IDS_ACTION_TO_CARGO);
        case ATManualFire:
            if (_manualFire)
            {
                return LocalizeString(IDS_ACTION_MANUAL_FIRE_CANCEL);
            }
            else
            {
                return LocalizeString(IDS_ACTION_MANUAL_FIRE);
            }
    }
    return base::GetActionName(action);
}

typedef bool (AIUnit::*AssignF)(Transport* veh);

static void SwapPositions(Transport* vehicle, Ref<Person>& p1, ManVehAction a1, bool cargo1, AssignF assign1,
                          Ref<Person>& p2, ManVehAction a2, bool cargo2, AssignF assign2)
{
    // switch animation state
    swap(p1, p2);
    if (p1)
    {
        if (Man* man = dyn_cast<Man>(p1.GetRef())) man->ResetCargoWeaponMode();
        p1->SwitchVehicleAction(a1);
    }
    if (p2)
    {
        if (Man* man = dyn_cast<Man>(p2.GetRef())) man->ResetCargoWeaponMode();
        p2->SwitchVehicleAction(a2);
    }
    AIUnit* unit1 = p1 ? p1->Brain() : nullptr;
    AIUnit* unit2 = p2 ? p2->Brain() : nullptr;
    if (unit1)
    {
        if (cargo1)
        {
            unit1->SetState(AIUnit::InCargo);
        }
        else if (unit1->GetState() == AIUnit::InCargo)
        {
            unit1->SetState(AIUnit::Wait);
        }
        (unit1->*assign1)(vehicle);
    }
    if (unit2)
    {
        if (cargo2)
        {
            unit2->SetState(AIUnit::InCargo);
        }
        else if (unit2->GetState() == AIUnit::InCargo)
        {
            unit2->SetState(AIUnit::Wait);
        }
        (unit2->*assign2)(vehicle);
    }
}

void Transport::ChangePosition(UIActionType type, Person* soldier)
{
    ManVehAction unitAct = ManVehActNone;
    bool unitCargo = false;
    AssignF unitAssign = &AIUnit::AssignAsCargo;

    Ref<Person>* unitPos = nullptr;
    if (soldier == _driver)
    {
        unitPos = &_driver;
        unitAct = DriverAction();
        unitAssign = &AIUnit::AssignAsDriver;
        if (_driver->Brain() == CommanderUnit())
        {
            _manualFire = false;
        }
    }
    else if (soldier == _gunner)
    {
        unitPos = &_gunner;
        unitAct = GunnerAction();
        unitAssign = &AIUnit::AssignAsGunner;
    }
    else if (soldier == _commander)
    {
        unitPos = &_commander;
        unitAct = CommanderAction();
        unitAssign = &AIUnit::AssignAsCommander;
        if (_commander->Brain() == CommanderUnit())
        {
            _manualFire = false;
        }
    }
    else
    {
        for (int i = 0; i < _manCargo.Size(); i++)
        {
            if (soldier == _manCargo[i])
            {
                unitPos = &_manCargo.Set(i).man, unitAct = CargoAction(i);
                unitCargo = true;
            }
        }
    }
    if (!unitPos)
    {
        return;
    }
    switch (type)
    {
        case ATMoveToDriver:
            SwapPositions(this, _driver, DriverAction(), false, &AIUnit::AssignAsDriver, *unitPos, unitAct, unitCargo,
                          unitAssign);
            break;
        case ATMoveToGunner:
            SwapPositions(this, _gunner, GunnerAction(), false, &AIUnit::AssignAsGunner, *unitPos, unitAct, unitCargo,
                          unitAssign);
            break;
        case ATMoveToCommander:
            SwapPositions(this, _commander, CommanderAction(), false, &AIUnit::AssignAsCommander, *unitPos, unitAct,
                          unitCargo, unitAssign);
            break;
        case ATMoveToCargo:
            if (_manCargo.Size() > 0)
            {
                SwapPositions(this, _manCargo.Set(0).man, CargoAction(0), true, &AIUnit::AssignAsCargo, *unitPos,
                              unitAct, unitCargo, unitAssign);
            }
            break;
    }
}

void Transport::PerformAction(const UIAction& action, AIUnit* unit)
{
    switch (action.type)
    {
        case ATMoveToDriver:
        case ATMoveToGunner:
        case ATMoveToCommander:
        case ATMoveToCargo:
            if (IsLocal())
            {
                ChangePosition(action.type, unit->GetPerson());
            }
            else
            {
                GetNetworkManager().AskForChangePosition(unit->GetPerson(), this, action.type);
            }
            return;
        case ATManualFire:
            if (GunnerUnit() && GunnerUnit()->IsAnyPlayer())
            {
                _manualFire = false;
            }
            else
            {
                _manualFire = !_manualFire;
            }
            return;
        case ATGetOut:
            unit->ProcessGetOut(false);
            return;
        case ATEngineOn:
            EngineOnAction();
            return;
        case ATEngineOff:
            EngineOffAction();
            return;
        case ATEject:
            Eject(unit);
            return;
        case ATLand:
            Land();
            return;
        case ATCancelLand:
            CancelLand();
            return;
        case ATTurnIn:
            HidePerson(unit->GetPerson(), 1.0);
            return;
        case ATTurnOut:
            HidePerson(unit->GetPerson(), 0.0);
            return;
    }
    base::PerformAction(action, unit);
}

void Transport::GetActions(UIActions& actions, AIUnit* unit, bool now)
{
    base::GetActions(actions, unit, now);
    if (!unit)
    {
        return;
    }

    if (unit->IsFreeSoldier())
    {
        if (unit->IsPlayer() && _lock == LSLocked)
        {
            return;
        }
        bool leader = _lock == LSUnlocked || unit->IsGroupLeader() || GWorld->GetMode() == GModeNetware;
        if (!leader && unit->VehicleAssigned() != this)
        {
            return;
        }
        AIGroup* grp = unit->GetGroup();
        PoseidonAssert(grp);
        AICenter* center = grp->GetCenter();
        PoseidonAssert(center);
        Person* person = unit->GetPerson();

        {
            Person* flagOwner = nullptr;
            if (Commander() && Commander()->IsDammageDestroyed() && Commander()->GetFlagTexture())
            {
                flagOwner = Commander();
                goto FlagFound;
            }
            if (Driver() && Driver()->IsDammageDestroyed() && Driver()->GetFlagTexture())
            {
                flagOwner = Driver();
                goto FlagFound;
            }
            if (Gunner() && Gunner()->IsDammageDestroyed() && Gunner()->GetFlagTexture())
            {
                flagOwner = Gunner();
                goto FlagFound;
            }
            for (int i = 0; i < GetManCargo().Size(); i++)
            {
                Person* man = GetManCargo()[i];
                if (man && man->IsDammageDestroyed() && man->GetFlagTexture())
                {
                    flagOwner = man;
                    goto FlagFound;
                }
            }
        FlagFound:
            if (flagOwner)
            {
                EntityAI* flagCarrier = flagOwner->GetFlagCarrier();

                TargetSide side = flagCarrier->GetFlagSide();
                AIGroup* grp = unit->GetGroup();
                AICenter* center = grp ? grp->GetCenter() : nullptr;
                if (center)
                {
                    if (center->IsEnemy(side))
                    {
                        actions.Add(ATTakeFlag, flagOwner, 0.99, 0, true, true);
                    }
                    else if (center->IsFriendly(side))
                    {
                        actions.Add(ATReturnFlag, flagOwner, 0.99, 0, true, true);
                    }
                }
            }
        }

        if (Commander() && !Commander()->IsDammageDestroyed() && center->IsEnemy(Commander()->GetTargetSide()))
        {
            return;
        }
        if (Driver() && !Driver()->IsDammageDestroyed() && center->IsEnemy(Driver()->GetTargetSide()))
        {
            return;
        }
        if (Gunner() && !Gunner()->IsDammageDestroyed() && center->IsEnemy(Gunner()->GetTargetSide()))
        {
            return;
        }
        for (int i = 0; i < GetManCargo().Size(); i++)
        {
            Person* man = GetManCargo()[i];
            if (!man || man->IsDammageDestroyed())
            {
                continue; // CWR fork: dead crew/cargo do not block getting in
            }
            if (center->IsEnemy(man->GetTargetSide()))
            {
                return;
            }
        }

        // a swimmer hangs about 1.4 m under the surface (the swim animations
        // are placed from the water line): measure his reach from the surface
        // so a boat can be boarded from the water
        Vector3 personPos = person->Position();
        if (person->IsSwimming() && GLandscape)
        {
            personPos[1] = SwimWaterSurfaceY(personPos.X(), personPos.Z());
        }

        float maxDist2 = 0;
        float invMaxDist2 = 0;
        if (now)
        {
            if (Speed().SquareSize() > Square(1.5))
            {
                return;
            }
            // require soldier to be in front of the vehicle
            Vector3Val relPos = person->PositionWorldToModel(Position());
            if (relPos.Z() <= 0)
            {
                return;
            }

            maxDist2 = Square(GetGetInRadius());
            invMaxDist2 = 1.0 / maxDist2;
        }

        if (leader)
        {
            if (QCanIGetIn(person))
            {
                if (now)
                {
                    Vector3 pos = GetDriverGetInPos(person, personPos);
                    float dist2 = pos.Distance2(personPos);
                    if (dist2 <= maxDist2)
                    {
                        float priority = 0.900 - 0.3 * dist2 * invMaxDist2;
                        actions.Add(ATGetInDriver, this, priority, 0, true);
                    }
                }
                else
                {
                    actions.Add(ATGetInDriver, this, 0.9, 0, true);
                }
            }
            if (QCanIGetInCommander(person))
            {
                if (now)
                {
                    Vector3 pos = GetCommanderGetInPos(person, personPos);
                    float dist2 = pos.Distance2(personPos);
                    if (dist2 <= maxDist2)
                    {
                        float priority = 0.899 - 0.3 * dist2 * invMaxDist2;
                        actions.Add(ATGetInCommander, this, priority, 0, true);
                    }
                }
                else
                {
                    actions.Add(ATGetInCommander, this, 0.8, 0, true);
                }
            }
            if (QCanIGetInGunner(person))
            {
                if (now)
                {
                    Vector3 pos = GetGunnerGetInPos(person, personPos);
                    float dist2 = pos.Distance2(personPos);
                    if (dist2 <= maxDist2)
                    {
                        float priority = 0.898 - 0.3 * dist2 * invMaxDist2;
                        actions.Add(ATGetInGunner, this, priority, 0, true);
                    }
                }
                else
                {
                    actions.Add(ATGetInGunner, this, 0.7, 0, true);
                }
            }
            if (QCanIGetInCargo(person))
            {
                if (now)
                {
                    Vector3 pos = GetCargoGetInPos(person, personPos);
                    float dist2 = pos.Distance2(personPos);
                    if (dist2 <= maxDist2)
                    {
                        float priority = 0.897 - 0.3 * dist2 * invMaxDist2;
                        actions.Add(ATGetInCargo, this, priority, 0, true);
                    }
                    else
                    {
                        Vector3 pos = GetCoDriverGetInPos(person, personPos);
                        float dist2 = pos.Distance2(personPos);
                        if (dist2 <= maxDist2)
                        {
                            float priority = 0.897 - 0.3 * dist2 * invMaxDist2;
                            actions.Add(ATGetInCargo, this, priority, 0, true);
                        }
                    }
                }
                else
                {
                    actions.Add(ATGetInCargo, this, 0.6, 0, true);
                }
            }
        }
        else
        {
            if (now)
            {
                Vector3 pos = GetUnitGetInPos(unit);
                float dist2 = pos.Distance2(personPos);
                if (dist2 > maxDist2)
                {
                    return;
                }
            }
            if (GetDriverAssigned() == unit)
            {
                if (QCanIGetIn(person))
                {
                    actions.Add(ATGetInDriver, this, 0.9, 0, true);
                }
            }
            else if (GetCommanderAssigned() == unit)
            {
                if (QCanIGetInCommander(person))
                {
                    actions.Add(ATGetInCommander, this, 0.8, 0, true);
                }
            }
            else if (GetGunnerAssigned() == unit)
            {
                if (QCanIGetInGunner(person))
                {
                    actions.Add(ATGetInGunner, this, 0.7, 0, true);
                }
            }
            else // assigned in cargo
            {
                if (QCanIGetInCargo(person))
                {
                    actions.Add(ATGetInCargo, this, 0.6, 0, true);
                }
            }
        }
    }
    else if (unit->GetVehicleIn() == this)
    {
        if (unit->IsPlayer())
        {
            Man* man = dyn_cast<Man>(unit->GetPerson());
            if (man && man->GetAvailableCargoWeaponSeat() && !man->IsActionInProgress(MFReload))
                actions.Add(man->GetConfiguredCargoWeaponSeat() ? ATCargoWeaponStow : ATCargoWeaponReady,
                            this, 0.89, 0, true);
        }
        if (!unit->IsPlayer() || GetLock() != LSLocked)
        {
            if (IsStopped())
            {
                actions.Add(ATGetOut, this, 0.9, 0, true);

                // check if I can change position
                if (_effCommander && unit == _effCommander->Brain() &&
                    (IsPersonHidden(unit->GetPerson()) || unit->IsInCargo()))
                {
                    bool leader = unit->IsGroupLeader() || GWorld->GetMode() == GModeNetware;
                    if (unit != PilotUnit() && Type()->_hasDriver && (leader || _driverAssigned == unit))
                    {
                        actions.Add(ATMoveToDriver, this, 0.04);
                    }
                    if (unit != ObserverUnit() && Type()->_hasCommander && (leader || _commanderAssigned == unit))
                    {
                        actions.Add(ATMoveToCommander, this, 0.03);
                    }
                    if (unit != GunnerUnit() && Type()->_hasGunner && (leader || _gunnerAssigned == unit))
                    {
                        actions.Add(ATMoveToGunner, this, 0.02);
                    }
                    bool enabled = leader;
                    if (!enabled)
                    {
                        for (int i = 0; i < _cargoAssigned.Size(); i++)
                        {
                            if (_cargoAssigned[i] == unit)
                            {
                                enabled = true;
                                break;
                            }
                        }
                    }
                    if (_manCargo.Size() > 0 && !unit->IsInCargo() && enabled)
                    {
                        actions.Add(ATMoveToCargo, this, 0.01);
                    }
                }
            }
            else
            {
                actions.Add(ATEject, this, 0.05);
            }
        }
        if (PilotUnit() == unit && GetFuel() > 0)
        {
            if (EngineIsOn())
            {
                actions.Add(ATEngineOff, this, 0.1);
            }
            else
            {
                actions.Add(ATEngineOn, this, 0.1);
            }
        }
        if (unit->IsPlayer())
        {
            if (Type()->HasGunner() && CommanderUnit() == unit &&
                (unit->GetPerson() == _commander || unit->GetPerson() == _driver) &&
                //				GWorld->GetMode() != GModeNetware
                (!GunnerUnit() || !GunnerUnit()->IsAnyPlayer()))
            {
                actions.Add(ATManualFire, this, 0.59);
            }

            if (Type()->_hideProxyInCombat)
            {
                Person* person = unit->GetPerson();
                bool turn = false;
                bool out = false;
                if (person == Commander())
                {
                    turn = !Type()->_forceHideCommander;
                    out = IsCommanderHidden();
                }
                else if (person == Driver())
                {
                    turn = !Type()->_forceHideDriver;
                    out = IsDriverHidden();
                }
                else if (person == Gunner())
                {
                    turn = !Type()->_forceHideGunner;
                    out = IsGunnerHidden();
                }
                if (turn)
                {
                    if (out)
                    {
                        actions.Add(ATTurnOut, this, 0.6);
                    }
                    else
                    {
                        actions.Add(ATTurnIn, this, 0.95);
                    }
                }
            }
        }
    }
}

bool Transport::QCanIGetIn(Person* who) const
{
    if (!QCanIBeIn(who))
    {
        return false;
    }
    if (!Type()->HasDriver())
    {
        return false;
    }
    return _driver == nullptr || _driver->IsDammageDestroyed();
}

bool Transport::QCanIGetInGunner(Person* who) const
{
    if (!QCanIBeIn(who))
    {
        return false;
    }
    if (!Type()->HasGunner())
    {
        return false;
    }
    return _gunner == nullptr || _gunner->IsDammageDestroyed();
}

bool Transport::QCanIGetInCommander(Person* who) const
{
    if (!QCanIBeIn(who))
    {
        return false;
    }
    if (!Type()->HasCommander())
    {
        return false;
    }
    return _commander == nullptr || _commander->IsDammageDestroyed();
}

bool Transport::QCanIGetInCargo(Person* who) const
{
    if (!QCanIBeIn(who))
    {
        return false;
    }
    if (GetFreeManCargo() > 0)
    {
        return true;
    }

    // try to find some dead body
    for (int i = 0; i < _manCargo.Size(); i++)
    {
        if (_manCargo[i] && _manCargo[i]->IsDammageDestroyed())
        {
            return true;
        }
    }
    return false;
}

bool Transport::QCanIGetInAny(Person* who) const
{
    return QCanIGetIn(who) || QCanIGetInGunner(who) || QCanIGetInCommander(who) || QCanIGetInCargo(who);
}

void Transport::GetInStarted(AIUnit* unit)
{
    if (_getinUnits.Find(unit) < 0)
    {
        _getinUnits.Add(unit);
    }
}
void Transport::GetOutStarted(AIUnit* unit)
{
    PoseidonAssert(_getoutUnits.Find(unit) < 0);
    if (_getoutUnits.Find(unit) < 0)
    {
        _getoutUnits.Add(unit);
    }
}

void Transport::GetInFinished(AIUnit* unit)
{
    for (int i = 0; i < _getinUnits.Size(); i++)
    {
        if (unit == _getinUnits[i])
        {
            _getinUnits.Delete(i);
            return;
        }
    }
}
void Transport::GetOutFinished(AIUnit* unit)
{
    for (int i = 0; i < _getoutUnits.Size(); i++)
    {
        if (unit == _getoutUnits[i])
        {
            _getoutUnits.Delete(i);
            return;
        }
    }
}

void Transport::WaitForGetIn(AIUnit* unit)
{
    if (unit->GetVehicle() == this)
    {
        return;
    }

    AIUnit* brain = PilotUnit();
    if (brain)
    {
        if (brain->GetState() == AIUnit::Stopped)
        {
            int x = toIntFloor(unit->Position().X() * InvLandGrid);
            int z = toIntFloor(unit->Position().Z() * InvLandGrid);
            GeographyInfo geogr = GLOB_LAND->GetGeography(x, z);
            float dist = (Position() - unit->Position()).SizeXZ();
            Time time = Glob.time + 2.0 * unit->GetVehicle()->GetCost(geogr) * dist + 30.0;
            if (time > GetGetInTimeout())
            {
                SetGetInTimeout(time);
            }
        }
        else
        {
            SetGetInTimeout(TIME_MAX);
            if (brain->GetState() != AIUnit::Stopping)
                Verify(brain->SetState(AIUnit::Stopping));
        }
    }

    GetInStarted(unit);
}

void Transport::WaitForGetOut(AIUnit* unit)
{
    AIUnit* brain = PilotUnit();

    GetOutStarted(unit);
    if (brain && brain->GetState() != AIUnit::Stopping && brain->GetState() != AIUnit::Stopped)
    {
        Verify(brain->SetState(AIUnit::Stopping));
    }
}

void Transport::LandStarted(LandingMode landing)
{
    _landing = landing;
    AIUnit* brain = PilotUnit();
    if (!brain)
    {
        return;
    }
    if (brain->GetState() != AIUnit::Stopping && brain->GetState() != AIUnit::Stopped)
    {
        Verify(brain->SetState(AIUnit::Stopping));
    }
}

bool Transport::IsStopped() const
{
    AIUnit* unitInto = PilotUnit();
    if (!unitInto)
    {
        return true;
    }
    if (!unitInto->IsPlayer())
    {
        // Stopped may timeout - test for Stopping
        if (unitInto->GetState() == AIUnit::Stopping)
        {
            return false;
        }
        if (unitInto->GetState() == AIUnit::Stopped)
        {
            return true;
        }
    }
    return Speed().SquareSize() < Square(0.5);
}

bool Transport::CanCancelStop() const
{
    if (_landing != LMNone)
    {
        return false;
    }
    if (!base::CanCancelStop())
    {
        return false;
    }
    if (WhoIsGettingIn().Count() == 0 && WhoIsGettingOut().Count() == 0)
    {
        _getinUnits.Clear();
        _getoutUnits.Clear();
        return true;
    }
    return false;
}

void VehicleSupply::UpdateStop()
{
    if (CanCancelStop())
    {
        AIUnit* brain = PilotUnit();
        if (brain)
        {
            if (brain && (brain->GetState() == AIUnit::Stopping || brain->GetState() == AIUnit::Stopped))
            {
                brain->SetState(AIUnit::Wait); // valid pass Stopped -> Wait
            }
        }
    }
}

Vector3 Transport::GetStopPosition() const
{
    float estT = 2; // seconds of travel at current velocity
    return Position() + Speed() * estT + 0.5 * estT * estT * Acceleration();
}

void Transport::GetOutAny(const Matrix4& outPos, Person* soldier, bool sound, const char* name)
{
    PoseidonAssert(soldier);

    DoAssert(GWorld->ValidateOutVehicle(soldier));
    DoAssert(ValidateCrew(soldier));

    if (soldier->IsInLandscape())
    {
        RptF("GetOutAny soldier %s already in landscape", (const char*)soldier->GetDebugName());
    }
    if (soldier->IsMoveOutInProgress())
    {
        RptF("%s: Getting out while IsMoveOutInProgress", (const char*)soldier->GetDebugName());
        soldier->CancelMoveOutInProgress();
        soldier->Move(outPos);
        soldier->Init(outPos);
    }
    else
    {
        soldier->SetTransform(outPos);
        soldier->Init(outPos);
        soldier->ResetMoveOut(); // mark it is in landscape
        GLOB_WORLD->AddVehicle(soldier);
        GLOB_WORLD->RemoveOutVehicle(soldier);
    }

    // lock position of empty vehicles
    LockPosition();

    if (soldier->Brain())
    {
        soldier->Brain()->SetVehicleIn(nullptr);
    }
    soldier->ResetMovement(2.5, Type()->_getOutAction);
    soldier->SetSpeed(_speed);
    if (!_doorSound && sound)
    {
        const SoundPars& pars = GetType()->GetGetOutSound();
        _doorSound = GSoundScene->OpenAndPlayOnce(pars.name, Position(), Speed(), pars.vol);
        if (_doorSound)
        {
            GSoundScene->SimulateSpeedOfSound(_doorSound);
            GSoundScene->AddSound(_doorSound);
        }
    }
    if (sound)
    {
        OnEvent(EEGetOut, name, soldier);
    }
}

void Transport::GetInAny(Person* soldier, bool sound, const char* name)
{
    AIUnit* unit = soldier->Brain();
    if (unit)
    {
        Transport* in = unit->GetVehicleIn();
        PoseidonAssert(in != this);
        if (in)
        {
            unit->DoGetOut(in, false);
        }
    }

    soldier->UnlockPosition();
    if (soldier->IsInLandscape())
    {
        soldier->SetMoveOut(this); // remove reference from the world
    }
    else
    {
        PoseidonAssert(!soldier->IsMoveOutInProgress());
        PoseidonAssert(soldier->GetHierachyParent() == this);
    }
    soldier->SetPilotLight(false);
    soldier->SwitchLight(false);
    if (unit)
    {
        unit->SetVehicleIn(this);
    }

    if (!_doorSound && sound)
    {
        const SoundPars& pars = GetType()->GetGetInSound();
        _doorSound = GSoundScene->OpenAndPlayOnce(pars.name, Position(), Speed(), pars.vol);
        if (_doorSound)
        {
            GSoundScene->SimulateSpeedOfSound(_doorSound);
            GSoundScene->AddSound(_doorSound);
        }
    }
    if (sound)
    {
        OnEvent(EEGetIn, name, soldier);
    }
}

void Transport::GetInDriver(Person* driver, bool sound)
{
    if (_driver)
    {
        DoAssert(_driver->IsDammageDestroyed());

        Vector3 pos = Type()->GetDriverGetInPos(0);
        Matrix4 transform;
        PositionModelToWorld(pos, pos);
        transform.SetPosition(pos);
        transform.SetUpAndDirection(VUp, pos - Position());

        GetOutDriver(transform, false);
    }

    _driver = driver;
    if (!_commander)
    {
        SetTargetSide(_driver->GetTargetSide());
    }
    if (driver == GLOB_WORLD->CameraOn())
    {
        GLOB_WORLD->SwitchCameraTo(this, GLOB_WORLD->GetCameraType());
    }
    GetInAny(driver, sound, "driver");
    _driverHiddenWanted = _driverHidden = Type()->_hideProxyInCombat;
    driver->SwitchVehicleAction(DriverAction());

    if (!_commander && (Type()->_driverIsCommander || !_gunner))
    {
        _effCommander = driver;
        SetTargetSide(driver->GetTargetSide());
    }
}

int Transport::GetManCargoSize() const
{
    int n = 0;
    for (int i = 0; i < _manCargo.Size(); i++)
    {
        if (_manCargo[i])
        {
            n++;
        }
    }
    return n;
}

int Transport::GetFreeManCargo() const
{
    return Type()->_maxManCargo - GetManCargoSize();
}

void Transport::GetOutDriver(const Matrix4& outPos, bool sound)
{
    if (!_driver)
    {
        return;
    }
    GetOutAny(outPos, _driver, sound, "driver");
    EngineOff();
    GWorld->VehicleSwitched(this, _driver);
    if (_driver->Brain() == CommanderUnit())
    {
        _manualFire = false;
    }
    _driver = nullptr;
}

void Transport::GetInCargo(Person* soldier, bool sound, int index)
{
    // if position is not specified, search for first free one
    if (index < 0)
    {
        for (int i = 0; i < _manCargo.Size(); i++)
        {
            if (_manCargo[i] == nullptr || _manCargo[i]->IsDammageDestroyed())
            {
                index = i;
                break;
            }
        }
        if (index < 0)
        {
            Fail("No free position in cargo");
            return;
        }
    }

    if (_manCargo[index])
    {
        bool coDriver = false;
        if (index >= Type()->_cargoCoDriver.Size())
        {
            coDriver = Type()->_cargoCoDriver[Type()->_cargoCoDriver.Size() - 1];
        }
        else
        {
            coDriver = Type()->_cargoCoDriver[index];
        }
        // check if cargo or co-driver
        Vector3 pos = coDriver ? Type()->GetCoDriverGetInPos(0) : Type()->GetCargoGetInPos(0);
        Matrix4 transform;
        PositionModelToWorld(pos, pos);
        transform.SetPosition(pos);
        transform.SetUpAndDirection(VUp, pos - Position());

        GetOutCargo(_manCargo[index], transform, false);
    }

    _manCargo.Set(index).man = soldier;
    GetInAny(soldier, sound, "cargo");
    if (Man* man = dyn_cast<Man>(soldier))
        man->ResetCargoWeaponMode();
    soldier->SwitchVehicleAction(CargoAction(index));
    GWorld->VehicleSwitched(soldier, this);
    if (!_driver && !_gunner && !_commander)
    {
        _effCommander = soldier;
        SetTargetSide(soldier->GetTargetSide());
    }
}

void Transport::GetOutCargo(Person* cargo, const Matrix4& outPos, bool sound)
{
    GetOutAny(outPos, cargo, sound, "cargo");
    for (int i = 0; i < _manCargo.Size(); i++)
    {
        if (_manCargo[i] == cargo)
        {
            _manCargo.Set(i).man = nullptr;
            return;
        }
    }
    Fail("Unit is not in cargo");
}

void Transport::GetInGunner(Person* soldier, bool sound)
{
    if (_gunner)
    {
        DoAssert(_gunner->IsDammageDestroyed());

        Vector3 pos = Type()->GetGunnerGetInPos(0);
        Matrix4 transform;
        PositionModelToWorld(pos, pos);
        transform.SetPosition(pos);
        transform.SetUpAndDirection(VUp, pos - Position());

        GetOutGunner(transform, false);
    }

    _gunner = soldier;
    _gunnerHiddenWanted = _gunnerHidden = Type()->_hideProxyInCombat;
    GetInAny(soldier, sound, "gunner");
    soldier->SwitchVehicleAction(GunnerAction());
    GWorld->VehicleSwitched(soldier, this);

    if (!_commander && (!Type()->_driverIsCommander || !_driver))
    {
        _effCommander = soldier;
        SetTargetSide(soldier->GetTargetSide());
    }

    if (GunnerUnit() && GunnerUnit()->IsAnyPlayer())
    {
        _manualFire = false;
    }
}

void Transport::GetOutGunner(const Matrix4& outPos, bool sound)
{
    if (!_gunner)
    {
        return;
    }
    GetOutAny(outPos, _gunner, sound, "gunner");
    _gunner = nullptr;
}

void Transport::GetInCommander(Person* soldier, bool sound)
{
    if (_commander)
    {
        DoAssert(_commander->IsDammageDestroyed());

        Vector3 pos = Type()->GetCommanderGetInPos(0);
        Matrix4 transform;
        PositionModelToWorld(pos, pos);
        transform.SetPosition(pos);
        transform.SetUpAndDirection(VUp, pos - Position());

        GetOutCommander(transform, false);
    }

    _commander = soldier;
    SetTargetSide(_commander->GetTargetSide());
    _commanderHiddenWanted = _commanderHidden = Type()->_hideProxyInCombat;
    GetInAny(soldier, sound, "commander");
    soldier->SwitchVehicleAction(CommanderAction());
    GWorld->VehicleSwitched(soldier, this);

    _effCommander = soldier;
    SetTargetSide(soldier->GetTargetSide());
}

void Transport::GetOutCommander(const Matrix4& outPos, bool sound)
{
    if (!_commander)
    {
        return;
    }
    GetOutAny(outPos, _commander, sound, "commander");
    if (_commander->Brain() == CommanderUnit())
    {
        _manualFire = false;
    }
    _commander = nullptr;
}

void Transport::Eject(AIUnit* unit)
{
    unit->ProcessGetOut(false);
}

void Transport::Land() {}

void Transport::CancelLand() {}

Vector3 Transport::GetUnitGetInPos(AIUnit* unit)
{
    PoseidonAssert(unit);
    Person* person = unit->GetPerson();
    Vector3Val unitPos = person->Position();
    if (GetDriverAssigned() == unit)
    {
        return GetDriverGetInPos(person, unitPos);
    }
    else if (GetCommanderAssigned() == unit)
    {
        return GetCommanderGetInPos(person, unitPos);
    }
    else if (GetGunnerAssigned() == unit)
    {
        return GetGunnerGetInPos(person, unitPos);
    }
    else
    {
        return GetCargoGetInPos(person, unitPos);
    }
}

Vector3 Transport::GetUnitGetOutPos(AIUnit* unit)
{
    PoseidonAssert(unit);
    Person* person = unit->GetPerson();
    if (Driver() == person)
    {
        return GetDriverGetOutPos(person);
    }
    else if (Commander() == person)
    {
        return GetCommanderGetOutPos(person);
    }
    else if (Gunner() == person)
    {
        return GetGunnerGetOutPos(person);
    }
    else
    {
        return GetCargoGetOutPos(person);
    }
}

Vector3 Transport::GetDriverGetInPos(Person* person, Vector3Par pos) const
{
    Vector3Val unitPos = pos;
    Vector3 bestPos = VZero;
    float minDist2 = FLT_MAX;
    int n = Type()->NDriverGetInPos();
    for (int i = 0; i < n; i++)
    {
        Vector3 pos = Type()->GetDriverGetInPos(i);
        PositionModelToWorld(pos, pos);
        float dist2 = pos.Distance2(unitPos);
        if (dist2 < minDist2)
        {
            minDist2 = dist2;
            bestPos = pos;
        }
    }
    return bestPos;
}

Vector3 Transport::GetCommanderGetInPos(Person* person, Vector3Par pos) const
{
    Vector3Val unitPos = pos;
    Vector3 bestPos = VZero;
    float minDist2 = FLT_MAX;
    int n = Type()->NCommanderGetInPos();
    for (int i = 0; i < n; i++)
    {
        Vector3 pos = Type()->GetCommanderGetInPos(i);
        PositionModelToWorld(pos, pos);
        float dist2 = pos.Distance2(unitPos);
        if (dist2 < minDist2)
        {
            minDist2 = dist2;
            bestPos = pos;
        }
    }
    return bestPos;
}

Vector3 Transport::GetGunnerGetInPos(Person* person, Vector3Par pos) const
{
    Vector3Val unitPos = pos;
    Vector3 bestPos = VZero;
    float minDist2 = FLT_MAX;
    int n = Type()->NGunnerGetInPos();
    for (int i = 0; i < n; i++)
    {
        Vector3 pos = Type()->GetGunnerGetInPos(i);
        PositionModelToWorld(pos, pos);
        float dist2 = pos.Distance2(unitPos);
        if (dist2 < minDist2)
        {
            minDist2 = dist2;
            bestPos = pos;
        }
    }
    return bestPos;
}

Vector3 Transport::GetCargoGetInPos(Person* person, Vector3Par pos) const
{
    Vector3Val unitPos = pos;
    Vector3 bestPos = VZero;
    float minDist2 = FLT_MAX;
    int n = Type()->NCargoGetInPos();
    for (int i = 0; i < n; i++)
    {
        Vector3 pos = Type()->GetCargoGetInPos(i);
        PositionModelToWorld(pos, pos);
        float dist2 = pos.Distance2(unitPos);
        if (dist2 < minDist2)
        {
            minDist2 = dist2;
            bestPos = pos;
        }
    }
    return bestPos;
}

Vector3 Transport::GetCoDriverGetInPos(Person* person, Vector3Par pos) const
{
    Vector3Val unitPos = pos;
    Vector3 bestPos = VZero;
    float minDist2 = FLT_MAX;
    int n = Type()->NCoDriverGetInPos();
    for (int i = 0; i < n; i++)
    {
        Vector3 pos = Type()->GetCoDriverGetInPos(i);
        PositionModelToWorld(pos, pos);
        float dist2 = pos.Distance2(unitPos);
        if (dist2 < minDist2)
        {
            minDist2 = dist2;
            bestPos = pos;
        }
    }
    return bestPos;
}

Vector3 Transport::GetDriverGetOutPos(Person* person) const
{
    Vector3Val pos = person->WorldPosition();
    return GetDriverGetInPos(person, pos);
}
Vector3 Transport::GetCommanderGetOutPos(Person* person) const
{
    Vector3Val pos = person->WorldPosition();
    return GetCommanderGetInPos(person, pos);
}
Vector3 Transport::GetGunnerGetOutPos(Person* person) const
{
    Vector3Val pos = person->WorldPosition();
    return GetGunnerGetInPos(person, pos);
}

Vector3 Transport::GetCargoGetOutPos(Person* person) const
{
    Vector3Val pos = person->WorldPosition();

    int index = -1;
    for (int i = 0; i < _manCargo.Size(); i++)
    {
        if (person == _manCargo[i])
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        // no slot: fall back
        return GetCargoGetInPos(person, pos);
    }
    bool coDriver = false;
    if (index >= Type()->_cargoCoDriver.Size())
    {
        coDriver = Type()->_cargoCoDriver[Type()->_cargoCoDriver.Size() - 1];
    }
    else
    {
        coDriver = Type()->_cargoCoDriver[index];
    }
    if (coDriver)
    {
        return GetCoDriverGetInPos(person, pos);
    }
    else
    {
        return GetCargoGetInPos(person, pos);
    }
}

void Transport::PerformFF(FFEffects& effects)
{
    base::PerformFF(effects);

    float vol, freq;
    vol = GetEngineVol(freq);

    effects.engineMag = vol * 0.09f;
    effects.engineFreq = freq * 50;
}

void Transport::Sound(bool inside, float deltaT)
{
    float vol, freq;
    vol = GetEngineVol(freq);
    if (freq > 0.01 && vol > 0.01)
    {
        const SoundPars& sound = GetType()->GetMainSound();
        if (!_engineSound && sound.name.GetLength() > 0)
        {
            _engineSound = GSoundScene->OpenAndPlay(sound.name, PositionModelToWorld(GetEnginePos()), Speed());
        }
        if (_engineSound)
        {
            freq *= sound.freq;
            vol *= sound.vol;
            if (inside)
            {
                vol *= Type()->_insideSoundCoef;
            }
            _engineSound->SetVolume(vol, freq);
            _engineSound->SetPosition(PositionModelToWorld(GetEnvironPos()), Speed());
            _engineSound->Set3D(!inside);
        }
    }
    else
    {
        _engineSound.Free();
    }

    vol = GetEnvironVol(freq);
    if (freq > 0.01 && vol > 0.01)
    {
        const SoundPars& sound = GetType()->GetEnvSound();
        if (!_envSound && sound.name.GetLength() > 0)
        {
            _envSound = GSoundScene->OpenAndPlay(sound.name, Position(), Speed());
        }
        if (_envSound)
        {
            freq *= sound.freq;
            vol *= sound.vol;
            if (inside)
            {
                vol *= Type()->_insideSoundCoef;
            }
            _envSound->SetVolume(vol, freq);
            _envSound->SetPosition(Position(), Speed());
            _envSound->Set3D(!inside);
        }
    }
    else
    {
        _envSound.Free();
    }

    if (_showDmg)
    {
        const SoundPars& sound = GetType()->GetDmgSound();
        if (!_dmgSound && sound.name.GetLength() > 0)
        {
            _dmgSound = GSoundScene->OpenAndPlay(sound.name, PositionModelToWorld(GetType()->_showDmgPoint), Speed());
        }
        if (_dmgSound)
        {
            freq = sound.freq;
            vol = sound.vol;
            _dmgSound->Set3D(!inside);
            _dmgSound->SetVolume(vol, freq);
            _dmgSound->SetPosition(PositionModelToWorld(GetType()->_showDmgPoint), Speed());
        }
    }
    else
    {
        _dmgSound.Free();
    }

    if (_doorSound)
    {
        const SoundPars& pars = GetType()->GetGetOutSound();
        float vol = pars.vol;
        _doorSound->SetVolume(vol);
        _doorSound->Set3D(!inside);
        _doorSound->SetPosition(Position(), Speed());
    }

    if (_doCrash != CrashNone && Glob.time > _timeCrash + 1.0)
    {
        _timeCrash = Glob.time;
        const SoundPars* pars = nullptr;
        switch (_doCrash)
        {
            case CrashObject:
                pars = &GetType()->GetCrashSound();
                break;
            case CrashLand:
                pars = &GetType()->GetLandCrashSound();
                break;
            case CrashWater:
                pars = &GetType()->GetWaterCrashSound();
                break;
        }
        if (pars)
        {
            float volume = pars->vol * _crashVolume;
            float freq = pars->freq;
            IWave* sound = GSoundScene->OpenAndPlayOnce(pars->name, Position(), Speed(), volume, freq);
            if (sound)
            {
                GSoundScene->SimulateSpeedOfSound(sound);
                GSoundScene->AddSound(sound);
            }
        }
    }
    _doCrash = CrashNone;
    base::Sound(inside, deltaT);
}

void Transport::UnloadSound()
{
    _engineSound.Free();
    _envSound.Free();
    _dmgSound.Free();
    base::UnloadSound();
}

bool Transport::IsFireEnabled()
{
    if (Type()->HasGunner())
    {
        if (!EffectiveGunnerUnit())
        {
            return false;
        }
    }
    else
    {
        if (!PilotUnit())
        {
            return false;
        }
    }

    return true;
}

bool Transport::StopAtStrategicPos() const
{
    switch (_moveMode)
    {
        case VMMBackward:
        case VMMSlowForward:
        case VMMForward:
        case VMMFastForward:
            return false;
        default:
            return true;
    }
}

float Transport::CalculateDriverPos(float& headChange, float minDist)
{
    const float reactionTime = DriverReactionTime;
    float speedCoef = 1.0f;
    switch (_moveMode)
    {
        case VMMFormation:
            Fail("No command");
            return speedCoef;
        case VMMMove:
            break;
        case VMMStop:
            if (_turnMode == VTMAbs)
            {
                Vector3 dir = DirectionWorldToModel(_dirWanted);
                headChange = atan2(dir.X(), dir.Z());
                if (fabs(headChange) < 0.01 * H_PI)
                {
                    _turnMode = VTMNone;
                }
            }
            return speedCoef;
        case VMMBackward:
        {
            speedCoef = -1.0;
            if (_turnMode != VTMNone)
            {
                _turnMode = VTMNone;
            }
            else
            {
                float dist2 = (_driverPos - Position()).SquareSizeXZ();
                if (dist2 >= Square(minDist))
                {
                    break;
                }
            }
            Vector3 from = Position() + Speed() * reactionTime;
            _driverPos = from - CmdPlanDist * _dirWanted;
            break;
        }
        case VMMSlowForward:
            speedCoef = 0.5;
            goto Forward;
        case VMMForward:
            speedCoef = 1.0;
            goto Forward;
        case VMMFastForward:
            speedCoef = 2.0;
            goto Forward;
        Forward:
        {
            if (_turnMode != VTMNone)
            {
                _turnMode = VTMNone;
            }
            else
            {
                float dist2 = (_driverPos - Position()).SquareSizeXZ();
                if (dist2 >= Square(minDist))
                {
                    break;
                }
            }
            Vector3 from = Position() + Speed() * reactionTime;
            _driverPos = from + CmdPlanDist * _dirWanted;
            break;
        }
    }
    return speedCoef;
}

void Transport::CommandPilot(float& speedWanted, float& headChange, float& turnPredict)
{
    speedWanted = 0;
    headChange = 0;
    turnPredict = 0;

    if (_turnMode == VTMLeft)
    {
        headChange = -0.25 * H_PI;
        return;
    }
    else if (_turnMode == VTMRight)
    {
        headChange = +0.25 * H_PI;
        return;
    }

    float speedCoef = CalculateDriverPos(headChange, CmdMinDist);

    if (_moveMode == VMMFormation || _moveMode == VMMStop)
    {
        return;
    }

    AIUnit* unit = PilotUnit();
    PoseidonAssert(unit);

    if (unit->GetPlanningMode() != AIUnit::VehiclePlanned ||
        unit->GetWantedPosition().Distance2(_driverPos) > Square(1))
    {
        unit->SetWantedPosition(_driverPos, AIUnit::VehiclePlanned, false);
    }

    LeaderPathPilot(unit, speedWanted, headChange, turnPredict, speedCoef);
}

void Transport::AIGunner(AIUnit* unit, float deltaT)
{
    if (_isDead || _isUpsideDown)
    {
        return;
    }
    PoseidonAssert(unit);

    if (!GetFireTarget())
    {
        // aim weapon based on formation / watch
        AIUnit* commander = CommanderUnit();
        if (!commander)
        {
            commander = unit;
        }

        if (Glob.time < _turretFrontUntil)
        {
            AimWeapon(_currentWeapon, Direction());
            return;
        }
        _turretFrontUntil = TIME_MIN;

        Vector3Val watchDir = commander->GetWatchDirection();
        float watchDirSize2 = watchDir.SquareSize();

        if (watchDirSize2 > 0.1f)
        {
            AimWeapon(_currentWeapon, watchDir);
        }
        return;
    }

    AimWeapon(_currentWeapon, GetFireTarget());
    if (!IsLocal())
    {
        AskForAimWeapon(_currentWeapon, GetWeaponDirectionWanted(_currentWeapon));
    }

    if (_currentWeapon < 0)
    {
        return;
    }
    if (_fire._firePrepareOnly)
    {
        return;
    }

    // check if weapon is aimed
    if (GetWeaponLoaded(_currentWeapon) && GetAimed(_currentWeapon, GetFireTarget()) >= 0.75 &&
        GetWeaponReady(_currentWeapon, GetFireTarget()) && !IsManualFire())
    {
        if (!GetAIFireEnabled(GetFireTarget()))
        {
            ReportFireReady();
        }
        else
        {
            FireWeapon(_currentWeapon, GetFireTarget()->idExact);
        }
    }
}

void Transport::FormationPilot(float& speedWanted, float& headChange, float& turnPredict)
{
    if (_userStopped || !PilotUnit())
    {
        speedWanted = 0;
        headChange = 0;
        turnPredict = 0;
        return;
    }

    if (_moveMode == VMMFormation)
    {
        base::FormationPilot(speedWanted, headChange, turnPredict);
    }
    else
    {
        CommandPilot(speedWanted, headChange, turnPredict);
    }
}

void Transport::LeaderPilot(float& speedWanted, float& headChange, float& turnPredict)
{
    if (_moveMode == VMMFormation)
    {
        base::LeaderPilot(speedWanted, headChange, turnPredict);
    }
    else
    {
        CommandPilot(speedWanted, headChange, turnPredict);
    }
}

void Transport::ReceivedMove(Vector3Par tgt)
{
    _moveMode = VMMMove;
    _driverPos = tgt;
}

void Transport::ReceivedJoin()
{
    _moveMode = VMMFormation;
    if (CommanderUnit())
    {
        AISubgroup* subgrp = CommanderUnit()->GetSubgroup();
        if (subgrp)
        {
            AIGroup* grp = subgrp->GetGroup();
            PoseidonAssert(grp);
            if (subgrp != grp->MainSubgroup())
            {
                subgrp->JoinToSubgroup(grp->MainSubgroup());
            }
        }
    }

    if (PilotUnit())
    {
        PilotUnit()->SetWantedPosition(_driverPos, AIUnit::DoNotPlan, true);
    }
}

void Transport::ReceivedCeaseFire()
{
    _fire._firePrepareOnly = true;
}

void Transport::ReceivedFire(Target* tgt)
{
    AIUnit* unit = GunnerUnit();
    _fire.SetTarget(unit, tgt);
    _fire._fireMode = _currentWeapon;
    _fire._firePrepareOnly = false;
}

void Transport::ReceivedTarget(Target* tgt)
{
    AIUnit* unit = GunnerUnit();
    _fire.SetTarget(unit, tgt);
    _fire._fireMode = _currentWeapon;
    _fire._firePrepareOnly = true;
}

void Transport::ReceivedSimpleCommand(SimpleCommand cmd)
{
    const float reactionTime = DriverReactionTime;
    switch (cmd)
    {
        case SCForward:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
            }
            _moveMode = VMMForward;
            _driverPos = Position() + Speed() * reactionTime;
            break;
        case SCStop:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
            }
            _moveMode = VMMStop;
            _driverPos = Position() + Speed() * reactionTime;
            break;
        case SCBackward:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
            }
            _moveMode = VMMBackward;
            _driverPos = Position() + Speed() * reactionTime;
            break;
        case SCFaster:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
            }
            _moveMode = VMMFastForward;
            _driverPos = Position() + Speed() * reactionTime;
            break;
        case SCSlower:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
            }
            _moveMode = VMMSlowForward;
            _driverPos = Position() + Speed() * reactionTime;
            break;
        case SCLeft:
        case SCRight:
            if (!_driver)
            {
                return;
            }
            if (_moveMode == VMMFormation || _moveMode == VMMMove)
            {
                _dirWanted = Direction();
                _azimutWanted = atan2(_dirWanted.X(), _dirWanted.Z());
                if (_moveMode == VMMFormation && CommanderUnit() && CommanderUnit()->IsGroupLeader())
                {
                    _moveMode = VMMStop;
                }
                else
                {
                    _moveMode = VMMForward;
                }
            }
            if (cmd == SCRight)
            {
                _turnMode = VTMRight;
            }
            else
            {
                _turnMode = VTMLeft;
            }
            break;
        case SCFire:
            // if (!_gunner || GunnerBrain()->IsAnyPlayer()) return;
            if (GunnerBrain())
            {
                if (_currentWeapon >= 0)
                {
                    if (GunnerBrain()->IsAnyPlayer())
                    {
                        _fire._firePrepareOnly = false;
                    }
                    else
                    {
                        const WeaponModeType* mode = GetWeaponMode(_currentWeapon);
                        if (mode && mode->_autoFire)
                        {
                            _fire._firePrepareOnly = false;
                        }
                        else if (GetWeaponLoaded(_currentWeapon))
                        {
                            FireWeapon(_currentWeapon, _fire._fireTarget ? _fire._fireTarget->idExact : nullptr);
                        }
                        else
                        {
                            SendFireFailed();
                        }
                    }
                }
            }
            break;
        case SCCeaseFire:
            if (!_gunner)
            {
                return;
            }
            _fire._firePrepareOnly = true;
            break;
        default:
            RptF("Simple command %d", cmd);
            break;
    }
}

void Transport::ReceivedLoad(int weapon)
{
    // LOG_DEBUG(Physics, "Receiving load {}->{}", _currentWeapon, weapon);

    if (!GunnerUnit()->IsAnyPlayer())
    {
        // LOG_DEBUG(Physics, "Received load {}->{}", _currentWeapon, weapon);
        SelectWeapon(weapon);
        _fire._fireMode = weapon;
        _fire._firePrepareOnly = true;
    }
}

void Transport::ReceivedAzimut(float azimut)
{
    _azimutWanted = azimut;
    _dirWanted = Vector3(sin(_azimutWanted), 0, cos(_azimutWanted));
    if (_moveMode == VMMFormation || _moveMode == VMMMove)
    {
        _moveMode = VMMForward;
    }
    _turnMode = VTMAbs;
}

void Transport::ReceivedStopTurning(float azimut)
{
    _azimutWanted = azimut;
    _dirWanted = Vector3(sin(_azimutWanted), 0, cos(_azimutWanted));
    _turnMode = VTMAbs;
    AIUnit* unit = PilotUnit();
    if (unit)
    {
        // check if moving
        switch (_moveMode)
        {
            case VMMBackward:
            case VMMSlowForward:
            case VMMForward:
            case VMMFastForward:
            {
                // replan path immediatelly - to get short reaction time
                float headChange;
                CalculateDriverPos(headChange, 0);

                unit->SetWantedPosition(_driverPos, AIUnit::VehiclePlanned, true);
            }
            break;
        }
    }
}

RadioMessage* CreateVehicleMessage(int type)
{
    switch (type)
    {
        case RMTVehicleMove:
            return new RadioMessageVMove();
        case RMTVehicleFire:
            return new RadioMessageVFire();
        case RMTVehicleFormation:
            return new RadioMessageVFormation();
        case RMTVehicleCeaseFire:
            return new RadioMessageVCeaseFire();
        case RMTVehicleSimpleCommand:
            return new RadioMessageVSimpleCommand();
        case RMTVehicleTarget:
            return new RadioMessageVTarget();
        case RMTVehicleLoad:
            return new RadioMessageVLoad();
        case RMTVehicleAzimut:
            return new RadioMessageVAzimut();
        case RMTVehicleStopTurning:
            return new RadioMessageVStopTurning();
        case RMTVehicleFireFailed:
            return new RadioMessageVFireFailed();
    }
    Fail("Message Type");
    return nullptr;
}

} // namespace Poseidon
