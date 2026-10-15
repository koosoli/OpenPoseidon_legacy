#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>

#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Detection/Detector.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

using Poseidon::Foundation::GetEnumValue;

using namespace Poseidon;
namespace Poseidon
{
} // namespace Poseidon

GameValue GuardedPointCreate(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (!CheckSize(state, array, 4))
        return NOTHING;
    if (!CheckType(state, array[0], GameSide))
        return NOTHING;
    if (!CheckType(state, array[2], GameScalar))
        return NOTHING;
    if (!CheckType(state, array[3], GameObject))
        return NOTHING;

    Vector3 pos;
    if (!GetPos(state, pos, array[1]))
        return NOTHING;
    pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);

    AICenter* center = GWorld->GetCenter(GetSide(array[0]));
    if (!center)
        return NOTHING;

    int idStatic = toInt((float)array[2]);
    if (idStatic >= 0)
    {
        Object* obj = GLandscape->FindObject(idStatic);
        if (obj)
            pos = obj->Position();
    }
    else
    {
        EntityAI* veh = dyn_cast<EntityAI>(GetObject(array[3]));
        if (veh)
            pos = veh->Position();
    }

    center->AddGuardedPoint(pos);
    return NOTHING;
}

GameValue TriggerCreate(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    // [type, position] -- Arma 3 adds a third element (make the trigger global), which a
    // single game has no use for.
    if (array.Size() != 3 && !CheckSize(state, array, 2))
    {
        return OBJECT_NULL;
    }
    if (!CheckType(state, array[0], GameString))
    {
        return OBJECT_NULL;
    }

    GameStringType type = array[0];
    Vector3 pos;
    if (!GetPos(state, pos, array[1]))
    {
        return OBJECT_NULL;
    }
    pos[1] = GLOB_LAND->RoadSurfaceYAboveWater(pos[0], pos[2]);

    Ref<Vehicle> vehicle = NewNonAIVehicle(type);
    if (!vehicle)
    {
        return OBJECT_NULL;
    }

    // position is on sea level now
    if (vehicle->GetShape())
    {
        pos += vehicle->GetShape()->BoundingCenter();
    }

    Vector3 normal = VUp;
    Matrix4 transform;
    transform.SetUpAndDirection(normal, Vector3(0, 0, 1));
    transform.SetPosition(pos);
    vehicle->SetTransform(transform);

    GWorld->AddBuilding(vehicle);
    if (GWorld->GetMode() == GModeNetware)
    {
        GetNetworkManager().CreateVehicle(vehicle, VLTBuilding, "", -1);
    }

    sensorsMap.Add(vehicle.GetRef());

    return GameValueExt(vehicle);
}

inline Detector* GetDetector(GameValuePar oper)
{
    Object* obj = GetObject(oper);
    return dyn_cast<Detector>(obj);
}

GameValue TriggerSetArea(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 4))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[0], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[1], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[2], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[3], GameBool))
    {
        return NOTHING;
    }

    Detector* det = GetDetector(oper1);
    if (det)
    {
        det->SetArea(array[0], array[1], array[2], array[3]);
    }

    return NOTHING;
}

GameValue TriggerSetActivation(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 3))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[0], GameString))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[1], GameString))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[2], GameBool))
    {
        return NOTHING;
    }

    Detector* det = GetDetector(oper1);
    if (det)
    {
        RString by = array[0];
        RString type = array[1];
        det->SetActivation(GetEnumValue<ArcadeSensorActivation>((const char*)by),
                           GetEnumValue<ArcadeSensorActivationType>((const char*)type), array[2]);
    }

    return NOTHING;
}

GameValue TriggerSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Detector* det = GetDetector(oper1);
    if (det)
    {
        RString type = oper2;
        det->SetTriggerType(GetEnumValue<ArcadeSensorType>((const char*)type));
    }

    return NOTHING;
}

GameValue TriggerSetTimeout(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 4))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[0], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[1], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[2], GameScalar))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[3], GameBool))
    {
        return NOTHING;
    }

    Detector* det = GetDetector(oper1);
    if (det)
    {
        det->SetTimeout(array[0], array[1], array[2], array[3]);
    }

    return NOTHING;
}

GameValue TriggerSetText(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Detector* det = GetDetector(oper1);
    if (det)
    {
        det->SetText(oper2);
    }

    return NOTHING;
}

GameValue TriggerSetStatements(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 3))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[0], GameString))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[1], GameString))
    {
        return NOTHING;
    }
    if (!CheckType(state, array[2], GameString))
    {
        return NOTHING;
    }

    Detector* det = GetDetector(oper1);
    if (det)
    {
        det->SetStatements(array[0], array[1], array[2]);
    }

    return NOTHING;
}

GameValue TriggerAttachObject(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    Detector* det = GetDetector(oper1);
    if (det)
        det->AssignStatic(toInt((float)oper2));

    return NOTHING;
}

GameValue TriggerAttachVehicle(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (array.Size() > 1)
    {
        state->SetError(EvalDim, array.Size(), 1);
        return NOTHING;
    }
    EntityAI* vehicle = nullptr;
    if (array.Size() == 1)
    {
        if (!CheckType(state, array[0], GameObject))
            return NOTHING;
        Object* obj = GetObject(array[0]);
        vehicle = dyn_cast<EntityAI>(obj);
    }

    Detector* det = GetDetector(oper1);
    if (det)
        det->AttachVehicle(vehicle);

    return NOTHING;
}

static ArcadeWaypointInfo* GetWaypoint(const GameState* state, GameValuePar oper, AIGroup** group = nullptr,
                                       int* index = nullptr)
{
    const GameArrayType& array = oper;
    if (!CheckSize(state, array, 2))
        return nullptr;
    if (!CheckType(state, array[0], GameGroup))
        return nullptr;
    if (!CheckType(state, array[1], GameScalar))
        return nullptr;

    AIGroup* grp = GetGroup(array[0]);
    if (!grp)
        return nullptr;

    int i = toInt((float)array[1]);
    if (i < 0)
        return nullptr;
    if (i >= grp->NWaypoints())
        return nullptr;

    if (group)
        *group = grp;
    if (index)
        *index = i;

    return &grp->GetWaypoint(i);
}

GameValue TriggerSynchronize(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    for (int i = 0; i < array.Size(); i++)
    {
        if (!CheckType(state, array[i], GameArray))
            return NOTHING;

        const GameArrayType& subarray = array[i];
        if (!CheckSize(state, subarray, 2))
            return NOTHING;
        if (!CheckType(state, subarray[0], GameGroup))
            return NOTHING;
        if (!CheckType(state, subarray[1], GameScalar))
            return NOTHING;
    }

    Detector* det = GetDetector(oper1);
    if (!det)
        return NOTHING;

    // delete old synchronizations
    for (int i = 0; i < synchronized.Size(); i++)
    {
        SynchronizedItem& sync = synchronized[i];
        DoAssert(sync.sensors.Size() <= 1);
        if (sync.sensors.Size() == 1 && sync.sensors[0].sensor == det)
        {
            sync.sensors.Clear();
            DoAssert(sync.groups.Size() == 1);
            AIGroup* grp = sync.groups[0].group;
            if (grp)
            {
                // remove synchronization from waypoint
                for (int j = 0; j < grp->NWaypoints(); j++)
                {
                    ArcadeWaypointInfo& wp = grp->GetWaypoint(j);
                    bool found = false;
                    for (int s = 0; s < wp.synchronizations.Size(); s++)
                        if (wp.synchronizations[s] == i)
                        {
                            wp.synchronizations.Delete(s);
                            found = true;
                            break;
                        }
                    if (found)
                        break;
                }
            }
            sync.groups.Clear();
            sync.sensors.Clear();
        }
    }

    // create new synchronizations
    AutoArray<int> sync;

    for (int i = 0; i < array.Size(); i++)
    {
        const GameArrayType& subarray = array[i];

        AIGroup* grp = GetGroup(subarray[0]);
        if (!grp)
            continue;

        int index = toInt((float)subarray[1]);
        if (index < 0)
            continue;
        if (index >= grp->NWaypoints())
            continue;

        int s = synchronized.Add();
        synchronized[s].Add(det);
        synchronized[s].Add(grp);

        sync.Add(s);

        ArcadeWaypointInfo& wp = grp->GetWaypoint(index);
        wp.synchronizations.Add(s);
    }

    sync.Compact();
    det->SetSynchronizations(sync);

    return NOTHING;
}

GameValue WaypointSynchronize(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array1 = oper1;
    if (!CheckSize(state, array1, 2))
        return NOTHING;
    if (!CheckType(state, array1[0], GameGroup))
        return NOTHING;
    if (!CheckType(state, array1[1], GameScalar))
        return NOTHING;

    const GameArrayType& array2 = oper2;
    for (int i = 0; i < array2.Size(); i++)
    {
        if (!CheckType(state, array2[i], GameArray))
            return NOTHING;

        const GameArrayType& subarray = array2[i];
        if (!CheckSize(state, subarray, 2))
            return NOTHING;
        if (!CheckType(state, subarray[0], GameGroup))
            return NOTHING;
        if (!CheckType(state, subarray[1], GameScalar))
            return NOTHING;
    }

    AIGroup* grp1 = GetGroup(array1[0]);
    if (!grp1)
        return NOTHING;
    int index1 = toInt((float)array1[1]);
    if (index1 < 0)
        return NOTHING;
    if (index1 >= grp1->NWaypoints())
        return NOTHING;
    ArcadeWaypointInfo& wp1 = grp1->GetWaypoint(index1);

    // delete old synchronizations
    for (int i = 0; i < wp1.synchronizations.Size();)
    {
        int sIndex = wp1.synchronizations[i];
        SynchronizedItem& sync = synchronized[sIndex];
        DoAssert(sync.sensors.Size() <= 1);
        if (sync.sensors.Size() == 1)
        {
            // synchronization waypoint - trigger
            DoAssert(sync.groups.Size() == 1);
            DoAssert(sync.groups[0].group == grp1) i++;
            continue;
        }

        // synchronization waypoint - waypoint
        DoAssert(sync.groups.Size() == 2);
        if (sync.groups[1].group != grp1)
        {
            // maintained by second waypoint
            DoAssert(sync.groups[0].group == grp1);
            i++;
            continue;
        }

        // remove from first waypoint
        wp1.synchronizations.Delete(i);

        // remove from second waypoint
        AIGroup* grp2 = sync.groups[0].group;
        if (grp2)
        {
            // remove synchronization from waypoint
            for (int j = 0; j < grp2->NWaypoints(); j++)
            {
                ArcadeWaypointInfo& wp2 = grp2->GetWaypoint(j);
                bool found = false;
                for (int s = 0; s < wp2.synchronizations.Size(); s++)
                    if (wp2.synchronizations[s] == sIndex)
                    {
                        wp2.synchronizations.Delete(s);
                        found = true;
                        break;
                    }
                if (found)
                    break;
            }
        }

        // remove from list of synchronizations
        sync.groups.Clear();
    }

    // create new synchronizations
    for (int i = 0; i < array2.Size(); i++)
    {
        const GameArrayType& subarray = array2[i];

        AIGroup* grp2 = GetGroup(subarray[0]);
        if (!grp2)
            continue;

        int index2 = toInt((float)subarray[1]);
        if (index2 < 0)
            continue;
        if (index2 >= grp2->NWaypoints())
            continue;

        ArcadeWaypointInfo& wp2 = grp2->GetWaypoint(index2);

        int s = synchronized.Add();
        synchronized[s].Add(grp2);
        synchronized[s].Add(grp1); // must be in group[1]

        wp1.synchronizations.Add(s);
        wp2.synchronizations.Add(s);
    }

    return NOTHING;
}

static ArcadeEffects* GetEffects(const GameState* state, GameValuePar oper)
{
    if (oper.GetType() == GameObject)
    {
        Detector* det = GetDetector(oper);
        if (det)
            return &det->GetEffects();
        return nullptr;
    }
    else if (oper.GetType() == GameArray)
    {
        ArcadeWaypointInfo* wp = GetWaypoint(state, oper);
        if (wp)
            return &wp->effects;
        return nullptr;
    }

    state->TypeError(GameObject | GameArray, oper.GetType());
    return nullptr;
}

GameValue EffectSetCondition(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    ArcadeEffects* effects = GetEffects(state, oper1);
    if (effects)
        effects->condition = oper2;
    return NOTHING;
}

GameValue EffectSetCamera(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 2))
        return NOTHING;
    if (!CheckType(state, array[0], GameString))
        return NOTHING;
    if (!CheckType(state, array[1], GameString))
        return NOTHING;

    ArcadeEffects* effects = GetEffects(state, oper1);
    if (effects)
    {
        effects->cameraEffect = array[0];
        RString pos = array[1];
        effects->cameraPosition = GetEnumValue<CamEffectPosition>((const char*)pos);
    }
    return NOTHING;
}

GameValue EffectSetSound(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 4))
        return NOTHING;
    if (!CheckType(state, array[0], GameString))
        return NOTHING;
    if (!CheckType(state, array[1], GameString))
        return NOTHING;
    if (!CheckType(state, array[2], GameString))
        return NOTHING;
    if (!CheckType(state, array[3], GameString))
        return NOTHING;

    ArcadeEffects* effects = GetEffects(state, oper1);
    if (effects)
    {
        effects->sound = array[0];
        effects->voice = array[1];
        effects->soundEnv = array[2];
        effects->soundDet = array[3];
    }
    return NOTHING;
}

GameValue EffectSetMusic(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    ArcadeEffects* effects = GetEffects(state, oper1);
    if (effects)
        effects->track = oper2;
    return NOTHING;
}

GameValue EffectSetTitle(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 3))
        return NOTHING;
    if (!CheckType(state, array[0], GameString))
        return NOTHING;
    if (!CheckType(state, array[1], GameString))
        return NOTHING;
    if (!CheckType(state, array[2], GameString))
        return NOTHING;

    ArcadeEffects* effects = GetEffects(state, oper1);
    if (effects)
    {
        RString type = array[0];
        effects->titleType = GetEnumValue<enum TitleType>((const char*)type);
        RString name = array[1];
        effects->titleEffect = GetEnumValue<TitEffectName>((const char*)name);
        effects->title = array[2];
    }
    return NOTHING;
}

GameValue WaypointAdd(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue retValue = state->CreateGameValue(GameArray);
    GameArrayType& retArray = retValue;
    retArray.Realloc(2);
    retArray.Resize(2);
    retArray[0] = GROUP_NULL;
    retArray[1] = -1.0f;

    AIGroup* group = GetGroup(oper1);
    if (!group)
        return retValue;

    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 2))
        return retValue;

    Vector3 pos;
    if (!GetPos(state, pos, array[0]))
        return retValue;

    if (!CheckType(state, array[1], GameScalar))
        return retValue;
    float placement = array[1];

    if (placement > 0)
    {
        Vector3 FindWaypointPosition(Vector3Par center, float radius);
        // set random position
        pos = FindWaypointPosition(pos, placement);
    }

    int index = group->AddWaypoint();
    ArcadeWaypointInfo& wp = group->GetWaypoint(index);
    wp.position = pos;
    wp.placement = placement;

    retArray[0] = GameValueExt(group);
    retArray[1] = (float)index;

    GetNetworkManager().UpdateObject(group);

    return retArray;
}

GameValue WaypointDelete(const GameState* state, GameValuePar oper1)
{
    const GameArrayType& array = oper1;
    if (!CheckSize(state, array, 2))
        return NOTHING;
    if (!CheckType(state, array[0], GameGroup))
        return NOTHING;
    if (!CheckType(state, array[1], GameScalar))
        return NOTHING;

    AIGroup* grp = GetGroup(array[0]);
    if (!grp)
        return NOTHING;

    int index = toInt((float)array[1]);
    if (index < 0)
        return NOTHING;
    if (index >= grp->NWaypoints())
        return NOTHING;

    grp->DeleteWaypoint(index);

    GetNetworkManager().UpdateObject(grp);

    return NOTHING;
}

GameValue WaypointSetPosition(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 2))
        return NOTHING;

    Vector3 pos;
    if (!GetPos(state, pos, array[0]))
        return NOTHING;

    if (!CheckType(state, array[1], GameScalar))
        return NOTHING;
    float placement = array[1];

    if (placement > 0)
    {
        Vector3 FindWaypointPosition(Vector3Par center, float radius);
        // set random position
        pos = FindWaypointPosition(pos, placement);
    }

    wp->position = pos;
    wp->placement = placement;

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetType(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString type = oper2;
    wp->type = GetEnumValue<ArcadeWaypointType>((const char*)type);

    void OnWaypointsUpdated(AIGroupContext * context);
    AIGroupContext context(group);
    if (group->GetCurrent())
    {
        context._task = group->GetCurrent()->_task;
        context._fsm = group->GetCurrent()->_fsm;

        OnWaypointsUpdated(&context);
    }

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointAttachVehicle(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    const GameArrayType& array = oper2;
    if (array.Size() > 1)
    {
        state->SetError(EvalDim, array.Size(), 1);
        return NOTHING;
    }
    int id = -1;
    if (array.Size() == 1)
    {
        if (!CheckType(state, array[0], GameObject))
            return NOTHING;
        Object* obj = GetObject(array[0]);

        if (obj)
            for (int i = 0; i < vehiclesMap.Size(); i++)
                if (obj == vehiclesMap[i])
                {
                    id = i;
                    break;
                }
    }

    wp->id = id;

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointAttachObject(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    wp->idStatic = toInt((float)oper2);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetHousePos(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    wp->housePos = toInt((float)oper2);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetCombatMode(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString str = oper2;
    wp->combatMode = GetEnumValue<AI::Semaphore>((const char*)str);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetFormation(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString str = oper2;
    wp->formation = GetEnumValue<AI::Formation>((const char*)str);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetSpeed(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString str = oper2;
    wp->speed = GetEnumValue<SpeedMode>((const char*)str);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetBehaviour(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString str = oper2;
    wp->combat = GetEnumValue<CombatMode>((const char*)str);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetDescription(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    wp->description = oper2;

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetStatements(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 2))
        return NOTHING;
    if (!CheckType(state, array[0], GameString))
        return NOTHING;
    if (!CheckType(state, array[1], GameString))
        return NOTHING;

    wp->expCond = array[0];
    wp->expActiv = array[1];

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetScript(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    wp->script = oper2;

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointSetTimeout(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    const GameArrayType& array = oper2;
    if (!CheckSize(state, array, 3))
        return NOTHING;
    if (!CheckType(state, array[0], GameScalar))
        return NOTHING;
    if (!CheckType(state, array[1], GameScalar))
        return NOTHING;
    if (!CheckType(state, array[2], GameScalar))
        return NOTHING;

    wp->timeoutMin = array[0];
    wp->timeoutMid = array[1];
    wp->timeoutMax = array[2];

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}

GameValue WaypointShow(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    AIGroup* group = nullptr;
    ArcadeWaypointInfo* wp = GetWaypoint(state, oper1, &group);
    if (!wp)
        return NOTHING;

    RString str = oper2;
    wp->showWP = GetEnumValue<AWPShow>((const char*)str);

    GetNetworkManager().UpdateObject(group);

    return NOTHING;
}
