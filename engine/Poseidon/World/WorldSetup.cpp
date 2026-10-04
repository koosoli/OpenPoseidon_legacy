#include <Poseidon/World/Simulation/StableExtract.hpp>
#include <cstdlib>
#include <unordered_map>
#include <SDL3/SDL_scancode.h>
#include "SimVehicleCost.hpp" // PERF-023

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/CheatCode.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/Frame/WorldFrameObserver.hpp>
#include <Poseidon/Audio/Speaker.hpp>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>

using namespace Poseidon;
extern void SDLGamepad_SetEngine(float mag);
extern void SDLGamepad_PlayRamp(float beg, float end, float dur);
#include <Poseidon/Graphics/Rendering/Primitives/ClipVert.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/Visibility.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/Game/TitEffects.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>
#include <chrono>

#include <Poseidon/Core/Progress.hpp>

#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>

#include <Poseidon/Game/Commands/GameStateExt.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>

#include <Poseidon/Game/Chat.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Graphics/Cursor/ICursorOverlay.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/UI/Map/UIMapCommon.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/World/WorldShared.hpp>
namespace Poseidon
{
#undef GetObject
#undef DrawText
Script* World::GetCameraScript() const
{
    return _cameraScript;
}
} // namespace Poseidon
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Graphics/Cursor/ICursorOverlay.hpp>
using namespace Poseidon::Dev;
using Poseidon::Foundation::IsOutOfMemory;

constexpr double kTimeAccMin = 1.0;
constexpr double kTimeAccMax = 4.0;

bool DisableTextures = false;

#define LOG_ADD_REMOVE_VEHICLE 0

#define PERF_SIM 0

static bool timeJumpPreview = false;

Person* World::PlayerOn() const
{
    return _playerOn;
}
void World::SwitchPlayerTo(Person* veh)
{
    _playerOn = veh;
    Log("SwitchPlayerTo %s", (const char*)veh->GetDebugName());
    SetActiveChannels();
}
Person* World::GetRealPlayer() const
{
    return _realPlayer;
}
void World::SetRealPlayer(Person* veh)
{
    _realPlayer = veh;
}

RadioChannel& World::GetRadio()
{
    return *_radio;
}
const RadioChannel& World::GetRadio() const
{
    return *_radio;
}

void World::SetSpeaker(RString speaker, float pitch)
{
    const ParamEntry& cfg = Pars >> "CfgVoice";
    const ParamEntry& entry = cfg >> speaker;
    Ref<BasicSpeaker> basic = new BasicSpeaker(entry);
    _speaker = new Speaker(basic, pitch);
}

AIUnit* World::FocusOn() const
{
    EntityAI* vehicle = dyn_cast<EntityAI>(CameraOn());
    if (!vehicle)
    {
        return nullptr;
    }
    Person* player = PlayerOn();
    AIUnit* unit = (player ? player->CommanderUnit() : nullptr);
    if (!unit || unit->GetVehicle() != vehicle)
    {
        unit = vehicle->CommanderUnit();
    }
    if (!unit)
    {
        return nullptr;
    }
    if (!unit->GetPerson())
    {
        return nullptr;
    }
    return unit;
}
namespace Poseidon
{
World* GWorld;
}

#define BACKGROUND_AI 0

AICenter* World::GetCenter(TargetSide side)
{
    switch (side)
    {
        case TWest:
            return _westCenter;
        case TEast:
            return _eastCenter;
        case TGuerrila:
            return _guerrilaCenter;
        case TCivilian:
            return _civilianCenter;
        case TLogic:
            return _logicCenter;
    }
    return nullptr;
}

AICenter* World::CreateCenter(TargetSide side)
{
    AICenterMode mode = TranslateMode(_mode);
    Ref<AICenter> center = new AICenter(side, mode);

    switch (side)
    {
        case TWest:
            _westCenter = center;
            break;
        case TEast:
            _eastCenter = center;
            break;
        case TGuerrila:
            _guerrilaCenter = center;
            break;
        case TCivilian:
            _civilianCenter = center;
            break;
        case TLogic:
            _logicCenter = center;
            break;
        default:
            Fail("invalid center");
            break;
    }
    return center;
}

void World::DeleteCenter(TargetSide side)
{
    switch (side)
    {
        case TWest:
            _westCenter = nullptr;
            break;
        case TEast:
            _eastCenter = nullptr;
            break;
        case TGuerrila:
            _guerrilaCenter = nullptr;
            break;
        case TCivilian:
            _civilianCenter = nullptr;
            break;
        case TLogic:
            _logicCenter = nullptr;
            break;
        default:
            Fail("invalid center");
            break;
    }
}

void World::AddCenter(AICenter* center)
{
    switch (center->GetSide())
    {
        case TWest:
            _westCenter = center;
            break;
        case TEast:
            _eastCenter = center;
            break;
        case TGuerrila:
            _guerrilaCenter = center;
            break;
        case TCivilian:
            _civilianCenter = center;
            break;
        case TLogic:
            _logicCenter = center;
            break;
        default:
            Fail("invalid center");
            break;
    }
}

void World::RemoveCenter(AICenter* center)
{
    switch (center->GetSide())
    {
        case TWest:
            _westCenter = nullptr;
            break;
        case TEast:
            _eastCenter = nullptr;
            break;
        case TGuerrila:
            _guerrilaCenter = nullptr;
            break;
        case TCivilian:
            _civilianCenter = nullptr;
            break;
        case TLogic:
            _logicCenter = nullptr;
            break;
        default:
            Fail("invalid center");
            break;
    }
}

void World::ScanPlayers(StaticArrayAuto<OLink<Person>>& players)
{
    players.Resize(0);
    for (int i = 0; i < NVehicles(); i++)
    {
        Entity* veh = GetVehicle(i);
        Person* pers = dyn_cast<Person>(veh);
        if (!pers)
        {
            continue;
        }
        if (pers->GetRemotePlayer() != AI_PLAYER)
        {
            players.Add(pers);
        }
    }
    for (int i = 0; i < NOutVehicles(); i++)
    {
        Entity* veh = GetOutVehicle(i);
        Person* pers = dyn_cast<Person>(veh);
        if (pers->GetRemotePlayer() != AI_PLAYER)
        {
            players.Add(pers);
        }
    }
}

World::~World()
{
    if (!IsOutOfMemory())
    {
        ProgressReset();
        ProgressStart(LocalizeString(IDS_SHUTDOWN));
    }
    DebugLog("World destruct");

    VehicleTypes.UnlockAllTypes();

    CleanUpDeinit();
    if (!IsOutOfMemory())
    {
        if (_scene.GetLandscape())
        {
            _scene.GetLandscape()->Init();
        }
    }
}

void World::DistributeImportances(VehiclesDistributed& target, VehicleList& list, SimulationImportance prec,
                                  const Vector3* viewerPos, int nViewers)
{
    SimulationImportance maxPrecG = SimulateInvisibleFar;

    // Removing each item separately shifted the remainder on every transfer:
    // crossing a detail boundary with 25k footprint marks became quadratic.
    StableExtract(list, [&](Entity* vehicle) {
        SimulationImportance iPrec = vehicle->CalculateImportance(viewerPos, nViewers);
        if (iPrec > maxPrecG)
        {
            iPrec = maxPrecG;
        }
        if (iPrec < SimulateVisibleNear)
        {
            iPrec = SimulateVisibleNear;
        }
        if (iPrec == prec)
        {
            return false;
        }
        VehicleList* tgt = nullptr;
        switch (iPrec)
        {
            case SimulateVisibleNear:
                tgt = &target._visibleNear;
                break;
            case SimulateVisibleFar:
                tgt = &target._visibleFar;
                break;
            case SimulateInvisibleNear:
                tgt = &target._invisibleNear;
                break;
            case SimulateInvisibleFar:
                tgt = &target._invisibleFar;
                break;
        }
        PoseidonAssert(tgt);
        tgt->Insert(vehicle);
        return true;
    });
}

void World::GetViewerList(StaticArrayAuto<Vector3>& viewers)
{
    if (_cameraEffect)
    {
        viewers.Add(_cameraEffect->GetTransform().Position());
        return;
    }
    if (GWorld->GetMode() != GModeNetware)
    {
        if (_cameraOn != nullptr)
        {
            viewers.Add(_cameraOn->Position());
        }
    }
    else
    {
        AUTO_STATIC_ARRAY(OLink<Person>, players, 128);
        GWorld->ScanPlayers(players);
        RString playerNames;
        for (int i = 0; i < players.Size(); i++)
        {
            Person* player = players[i];
            viewers.Add(player->Position());
            playerNames = playerNames + player->GetDebugName() + RString(" ");
        }
        // Note: better solution - add position of all respawned players (seagull problem)
        Vector3 cameraPos;
        if (_cameraOn != nullptr)
        {
            cameraPos = _cameraOn->Position();
        }
        else
        {
            cameraPos = _scene.GetCamera()->Position();
        }
        bool found = false;
        for (int i = 0; i < viewers.Size(); i++)
        {
            if (viewers[i].Distance2(cameraPos) < Square(10))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            viewers.Add(cameraPos);
        }
    }
    if (viewers.Size() <= 0)
    {
        viewers.Add(_scene.GetCamera()->Position());
    }
}

void World::DistributeNearImportances(VehiclesDistributed& list)
{
    _nearImportanceDistributionTime = Glob.time;
    AUTO_STATIC_ARRAY(Vector3, viewers, 128)
    GetViewerList(viewers);
    DistributeImportances(list, list._visibleNear, SimulateVisibleNear, viewers.Data(), viewers.Size());
    DistributeImportances(list, list._visibleFar, SimulateVisibleFar, viewers.Data(), viewers.Size());
}

void World::DistributeFarImportances(VehiclesDistributed& list)
{
    _farImportanceDistributionTime = Glob.time;
    AUTO_STATIC_ARRAY(Vector3, viewers, 128)
    GetViewerList(viewers);
    DistributeImportances(list, list._invisibleNear, SimulateInvisibleNear, viewers.Data(), viewers.Size());
    DistributeImportances(list, list._invisibleFar, SimulateInvisibleFar, viewers.Data(), viewers.Size());
}

void World::DistributeNearImportances()
{
    DistributeNearImportances(_vehicles);
    DistributeNearImportances(_animals);
    DistributeNearImportances(_buildings);
}
void World::DistributeFarImportances()
{
    DistributeFarImportances(_vehicles);
    DistributeFarImportances(_animals);
    DistributeFarImportances(_buildings);
}

void World::AddSensor(Person* vehicle)
{
    _sensorList->AddRow(vehicle);
}

void World::RemoveTarget(Entity* vehicle)
{
    EntityAI* ai = dyn_cast<EntityAI>(vehicle);
    if (ai)
    {
        _sensorList->DeleteCol(ai);
    }
}

void World::RemoveSensor(Entity* vehicle)
{
    Person* driver = dyn_cast<Person>(vehicle);
    if (driver)
    {
        _sensorList->DeleteRow(driver);
    }
}

void World::MoveOutAndDelete(VehicleList& vehicles, float deltaT, bool applyMove)
{
    // Deleting each entry in-place shifts the entire RefArray tail. Footprint
    // marks expire in large cohorts, turning this maintenance pass quadratic
    // in dense infantry scenes. Compact survivors once while preserving their
    // order, so simulation and deterministic gameplay ordering stay unchanged.
    const int outCountBefore = _outVehicles.Size();
    const int count = vehicles.Size();
    int write = 0;
    for (int read = 0; read < count; ++read)
    {
        // Most entries survive. Avoid two reference-count writes per mark on
        // every tick; retain the guard around lifecycle callbacks below.
        Entity* current = vehicles[read];
        if (!current->ToDelete() && !current->ToMoveOut() && !current->ToConvertToObject())
        {
            if (write != read)
                vehicles.Set(write) = vehicles[read];
            ++write;
            continue;
        }
        Ref<Entity> keepAlive = vehicles[read];
        Entity* vehicle = keepAlive;
        if (vehicle->ToDelete())
        {
            vehicle->ResetDelete();
            vehicle->UnloadSound();
            _scene.GetLandscape()->RemoveObject(vehicle);
            RemoveTarget(vehicle);
            RemoveSensor(vehicle);
        }
        else if (vehicle->ToMoveOut())
        {
            LOG_DEBUG(World, "Moving {} from landscape", (const char*)vehicle->GetDebugName());
            vehicle->UnloadSound();
            DoAssert(vehicle->IsMoveOutInProgress());
            vehicle->SetMoveOutFlag();
            _outVehicles.Add(vehicle);
            _scene.GetLandscape()->RemoveObject(vehicle);
            RemoveTarget(vehicle);
            RemoveSensor(vehicle);
        }
        else if (vehicle->ToConvertToObject())
        {
            vehicle->UnloadSound();
            vehicle->ResetConvertToObject();
            // PERF-031: take it OUT of the landscape index before its type changes and
            // put it back after. The cell keeps a per-type count now, and this is the
            // only place in the engine that would mutate an indexed object's type --
            // the two branches above already remove before they change anything, and
            // this one did not. (`SetConvertToObject` has no callers today, so this is
            // pre-emptive: the assert in ChangeTypeVehicleCount would have caught it,
            // but only in a debug build, and only once somebody added that caller.)
            _scene.GetLandscape()->RemoveObject(vehicle);
            vehicle->SetType(Primary);
            _scene.GetLandscape()->AddObject(vehicle);
            RemoveSensor(vehicle);
        }
        else
        {
            if (write != read)
            {
                vehicles.Set(write) = vehicles[read];
            }
            ++write;
        }
    }
    vehicles.Resize(write);

    // Stable compaction retains removed entries in the untrimmed tail until
    // Resize. Validate only now, when a newly boarded soldier is absent from
    // the normal list, as it was after the old per-entry Delete operation.
    for (int i = outCountBefore; i < _outVehicles.Size(); ++i)
    {
        DoAssert(ValidateOutVehicle(_outVehicles[i]));
    }
}

void World::MoveOutAndDelete(VehiclesDistributed& list, float deltaT, bool applyMove)
{
    MoveOutAndDelete(list._visibleNear, deltaT, applyMove);
    MoveOutAndDelete(list._visibleFar, deltaT, applyMove);
    MoveOutAndDelete(list._invisibleNear, deltaT, applyMove);
    MoveOutAndDelete(list._invisibleFar, deltaT, applyMove);
}

// PERF-023: one Simulate call, clocked into its entity kind's bucket. The kind test is a
// short IsKindOf chain walk per call, well under the Simulate it brackets.
static void SimulateTimed(Entity* vehicle, VehicleSimulation simul, float deltaT, SimulationImportance prec)
{
    // PERF-027: the accounting is OFF unless asked for. What follows is not a
    // couple of clock reads -- it is a type-hierarchy classification per entity
    // per tick, and `_animals` holds one entity per footfall.
    if (!SimVehCost::Enabled())
    {
        (vehicle->*simul)(deltaT, prec);
        return;
    }
    const std::uint64_t t0 = SimVehCost::NowNs();
    (vehicle->*simul)(deltaT, prec);
    const std::uint64_t ns = SimVehCost::NowNs() - t0;
    // Memoised on the TYPE, not recomputed per entity: the chain below is six
    // `IsKindOf` walks and six `Preloaded` lookups, and a scene has a handful of
    // types against thousands of entities.
    static std::unordered_map<const EntityType*, SimVehCost::Kind> kindCache;
    SimVehCost::Kind kind = SimVehCost::Kind::Other;
    if (const EntityType* type = vehicle->GetNonAIType())
    {
        if (const auto hit = kindCache.find(type); hit != kindCache.end())
        {
            SimVehCost::AddKind(hit->second, ns);
            return;
        }
        const World* w = GLOB_WORLD;
        if (type->IsKindOf(w->Preloaded(VTypeMan)))
            kind = SimVehCost::Kind::Man;
        else if (type->IsKindOf(w->Preloaded(VTypeTank)) || type->IsKindOf(w->Preloaded(VTypeAPC)))
            kind = SimVehCost::Kind::Tank;
        else if (type->IsKindOf(w->Preloaded(VTypeCar)))
            kind = SimVehCost::Kind::Car;
        else if (type->IsKindOf(w->Preloaded(VTypeAir)))
            kind = SimVehCost::Kind::Air;
        else if (type->IsKindOf(w->Preloaded(VTypeShip)))
            kind = SimVehCost::Kind::Ship;
        else if (type->IsKindOf(w->Preloaded(VTypeStatic)))
            kind = SimVehCost::Kind::Static;
    }
    if (const EntityType* type = vehicle->GetNonAIType())
    {
        kindCache.emplace(type, kind);
    }
    SimVehCost::AddKind(kind, ns);
}

void World::SimulateOnly(VehicleList& vehicles, float deltaT, VehicleSimulation simul, Entity* insideVehicle,
                         SimulationImportance prec)
{
    int i;
    int oldVehicles = vehicles.Size();
    if (prec == SimulateVisibleNear)
    {
        if (GetMode() != GModeNetware)
        {
            for (i = 0; i < oldVehicles; i++)
            {
                Entity* vehicle = vehicles[i];
                if (vehicle != insideVehicle)
                {
                    SimulateTimed(vehicle, simul, deltaT, prec);
                }
                else
                {
                    vehicle->SetLastImportance(SimulateCamera);
                    SimulateTimed(vehicle, simul, deltaT, SimulateCamera);
                }
            }
        }
        else
        {
            for (i = 0; i < oldVehicles; i++)
            {
                Entity* vehicle = vehicles[i];
                if (vehicle->IsLocal())
                {
                    if (vehicle != insideVehicle)
                    {
                        SimulateTimed(vehicle, simul, deltaT, prec);
                    }
                    else
                    {
                        vehicle->SetLastImportance(SimulateCamera);
                        SimulateTimed(vehicle, simul, deltaT, SimulateCamera);
                    }
                }
                else
                {
                    SimulationImportance p = vehicle->GetLastImportance();
                    if (p == SimulateDefault)
                    {
                        p = prec;
                    }
                    SimulateTimed(vehicle, simul, deltaT, p);
                }
            }
        }
    }
    else
    {
        for (i = 0; i < oldVehicles; i++)
        {
            PoseidonAssert(i < vehicles.Size());
            Entity* vehicle = vehicles[i];
            SimulateTimed(vehicle, simul, deltaT, prec);
        }
    }
    for (i = oldVehicles; i < vehicles.Size(); i++)
    {
        Entity* vehicle = vehicles[i];
        SimulateTimed(vehicle, simul, deltaT, prec);
    }
#if PERF_SIM
#endif
}

inline SimulationImportance MinPrec(SimulationImportance p1, SimulationImportance p2)
{
    if (p1 > p2)
    {
        return p1;
    }
    return p2;
}

void World::SimulateOnly(VehiclesDistributed& vehicles, float deltaT, VehicleSimulation simul, Entity* insideVehicle,
                         SimulationImportance prec)
{
    SimulateOnly(vehicles._visibleNear, deltaT, simul, insideVehicle, SimulateVisibleNear);

    // A fixed 60 Hz world tick made the old remainder pass fully simulate every
    // off-screen unit on every tick. Use the entity's existing deterministic
    // precision accumulator off-screen; visible-near entities and the camera
    // vehicle retain the exact full-rate path. Visible-far goes through the
    // accumulator too, but Entity::SimulateOptimized caps its step
    // (PERF-033, default 1/15 s) so distant-visible soldiers stay smooth
    // instead of freezing at their 1/5 s declared precision. Full-rate
    // visible-far was measured at 26-50 ms/tick and re-entered the catch-up
    // spiral, so it is not the fallback. The escape hatch is an
    // A/B control and restores the former behaviour without changing saves.
    static const bool fullRateFar = []
    {
        const char* value = std::getenv("POSEIDON_FAR_SIM_FULL_RATE");
        return value != nullptr && value[0] == '1';
    }();
    const VehicleSimulation farSimul =
        !fullRateFar && simul == &Entity::SimulateRest ? &Entity::SimulateOptimized : simul;
    SimulateOnly(vehicles._visibleFar, deltaT, farSimul, insideVehicle, SimulateVisibleFar);
    SimulateOnly(vehicles._invisibleNear, deltaT, farSimul, insideVehicle, SimulateInvisibleNear);
    SimulateOnly(vehicles._invisibleFar, deltaT, farSimul, insideVehicle, SimulateInvisibleFar);
}

void World::AddAttachment(AttachedOnVehicle* attach)
{
    _attached.Add(attach);
}

void World::RemoveAttachment(AttachedOnVehicle* attach)
{
    int index = _attached.Find(attach);
    PoseidonAssert(index >= 0);
    if (index >= 0)
    {
        _attached.Delete(index);
    }
}

void VehicleList::Add(Entity* object)
{
#if 1
    Ref<Entity> temp = object;
    int index = Find(object);
    if (index >= 0)
    {
        LOG_DEBUG(World, "Entity listed twice {}", (const char*)object->GetDebugName());
        Fail("Entity listed twice.");
        return;
    }
#endif

    object->StartFrame();
    Insert(object);
    GLOB_LAND->AddObject(object);
}

LSError VehicleList::Serialize(ParamArchive& ar)
{
    if (ar.IsSaving())
    {
        RefArray<Entity> mustBeSaved;
        for (int i = 0; i < Size(); i++)
        {
            Entity* veh = Get(i);
            if (veh->Object::GetType() == Primary)
            {
                continue;
            }
            if (!veh->MustBeSaved())
            {
                continue;
            }
            mustBeSaved.Add(veh);
        }
        PARAM_CHECK(ar.Serialize("Vehicles", mustBeSaved, 1))
    }
    else
    {
        PARAM_CHECK(ar.Serialize("Vehicles", *(RefArray<Entity>*)this, 1))
        if (ar.GetPass() == ParamArchive::PassFirst)
        {
            for (int i = 0; i < Size(); i++)
            {
                Entity* object = Set(i);
                object->StartFrame();
                GLOB_LAND->AddObject(object);
            }
        }
    }
    return LSOK;
}

void VehiclesDistributed::Clear()
{
    for (int i = 0; i < _visibleNear.Size(); i++)
    {
        _visibleNear[i]->SetList(nullptr);
    }
    for (int i = 0; i < _visibleFar.Size(); i++)
    {
        _visibleFar[i]->SetList(nullptr);
    }
    for (int i = 0; i < _invisibleNear.Size(); i++)
    {
        _invisibleNear[i]->SetList(nullptr);
    }
    for (int i = 0; i < _invisibleFar.Size(); i++)
    {
        _invisibleFar[i]->SetList(nullptr);
    }
    _visibleNear.Clear();
    _visibleFar.Clear();
    _invisibleNear.Clear();
    _invisibleFar.Clear();
}

void VehiclesDistributed::Add(Entity* vehicle)
{
    vehicle->SetList(this);
    _visibleNear.Add(vehicle);
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "Vehicle {} added to list {:x}", (const char*)vehicle->GetDebugName(), (uintptr_t)this);
#endif
}

void VehiclesDistributed::Insert(Entity* vehicle)
{
    Fail("Obsolete");
    vehicle->SetList(this);
    _visibleNear.Add(vehicle);
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "Vehicle {} inserted to list {:x}", (const char*)vehicle->GetDebugName(), (uintptr_t)this);
#endif
}

int VehiclesDistributed::Find(Entity* vehicle) const
{
    int offset = 0;
    int index = 0;
#define ONE_LIST(list)          \
    index = list.Find(vehicle); \
    if (index >= 0)             \
        return index + offset;  \
    offset += list.Size();
    ONE_LIST(_visibleNear)
    ONE_LIST(_visibleFar)
    ONE_LIST(_invisibleNear)
    ONE_LIST(_invisibleFar)
    return -1;
}

void VehiclesDistributed::Delete(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "Vehicle {} deleted from list {:x}", (const char*)vehicle->GetDebugName(), (uintptr_t)this);
#endif
    DoAssert(this == vehicle->GetList());
    int deleted = 0;
    if (_visibleNear.Delete(vehicle))
    {
        deleted++;
    }
    if (_visibleFar.Delete(vehicle))
    {
        deleted++;
    }
    if (_invisibleNear.Delete(vehicle))
    {
        deleted++;
    }
    if (_invisibleFar.Delete(vehicle))
    {
        deleted++;
    }
    vehicle->SetList(nullptr);
    DoAssert(deleted == 1);
    GLOB_LAND->RemoveObject(vehicle);
}

void VehiclesDistributed::Remove(Entity* vehicle)
{
#if LOG_ADD_REMOVE_VEHICLE
    LOG_DEBUG(World, "Vehicle {} removed from list {:x}", (const char*)vehicle->GetDebugName(), (uintptr_t)this);
#endif
    DoAssert(this == vehicle->GetList());
    int deleted = 0;
    if (_visibleNear.Delete(vehicle))
    {
        deleted++;
    }
    if (_visibleFar.Delete(vehicle))
    {
        deleted++;
    }
    if (_invisibleNear.Delete(vehicle))
    {
        deleted++;
    }
    if (_invisibleFar.Delete(vehicle))
    {
        deleted++;
    }
    DoAssert(deleted == 1);
    vehicle->SetList(nullptr);
}

int VehiclesDistributed::Size() const
{
    return (_visibleNear.Size() + _visibleFar.Size() + _invisibleNear.Size() + _invisibleFar.Size());
}

Entity* VehiclesDistributed::Get(int index) const
{
    if (index < _visibleNear.Size())
    {
        return _visibleNear[index];
    }
    index -= _visibleNear.Size();
    if (index < _visibleFar.Size())
    {
        return _visibleFar[index];
    }
    index -= _visibleFar.Size();
    if (index < _invisibleNear.Size())
    {
        return _invisibleNear[index];
    }
    index -= _invisibleNear.Size();
    if (index < _invisibleFar.Size())
    {
        return _invisibleFar[index];
    }
    Fail("Entity in no importance list");
    return nullptr;
}

LSError VehiclesDistributed::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("VisibleNear", _visibleNear, 1))
    PARAM_CHECK(ar.Serialize("VisibleFar", _visibleFar, 1))
    PARAM_CHECK(ar.Serialize("InvisibleNear", _invisibleNear, 1))
    PARAM_CHECK(ar.Serialize("InvisibleFar", _invisibleFar, 1))
    if (ar.IsLoading() && ar.GetPass() == ParamArchive::PassFirst)
    {
        for (int i = 0; i < _visibleNear.Size(); i++)
        {
            if (_visibleNear[i])
            {
                _visibleNear[i]->SetList(this);
            }
        }
        for (int i = 0; i < _visibleFar.Size(); i++)
        {
            if (_visibleFar[i])
            {
                _visibleFar[i]->SetList(this);
            }
        }
        for (int i = 0; i < _invisibleNear.Size(); i++)
        {
            if (_invisibleNear[i])
            {
                _invisibleNear[i]->SetList(this);
            }
        }
        for (int i = 0; i < _invisibleFar.Size(); i++)
        {
            if (_invisibleFar[i])
            {
                _invisibleFar[i]->SetList(this);
            }
        }
    }
    return LSOK;
}

void World::SetActiveChannels()
{
    AIUnit* unit = FocusOn();
    AIGroup* grp = unit ? unit->GetGroup() : nullptr;
    AICenter* center = grp ? grp->GetCenter() : nullptr;

    bool enableRadio = _enableRadio;
    if (GWorld->GetMode() != GModeNetware)
    {
        AIUnit* playerUnit = _realPlayer ? _realPlayer->Brain() : nullptr;
        if (!playerUnit || playerUnit->GetLifeState() != AIUnit::LSAlive)
        {
            enableRadio = false;
        }
    }

    _radio->SetAudible(enableRadio);

    if (enableRadio && center)
    {
        if (_channelSide != &center->GetRadio())
        {
            if (_channelSide)
            {
                _channelSide->SetAudible(false);
            }
            _channelSide = &center->GetRadio();
        }
        if (_channelSide)
        {
            _channelSide->SetAudible(true);
        }
    }
    else if (_channelSide)
    {
        _channelSide->SetAudible(false);
        _channelSide = nullptr;
    }

    if (!enableRadio || !center || _mode == GModeIntro)
    {
        if (_channelVehicle)
        {
            _channelVehicle->SetAudible(false);
            _channelVehicle = nullptr;
        }

        if (_channelGroup)
        {
            _channelGroup->SetAudible(false);
            _channelGroup = nullptr;
        }

        return;
    }

    if (_channelGroup != &grp->GetRadio())
    {
        if (_channelGroup)
        {
            _channelGroup->SetAudible(false);
        }
        _channelGroup = &grp->GetRadio();
    }
    if (_channelGroup)
    {
        _channelGroup->SetAudible(true);
    }

    Transport* transport = unit ? unit->GetVehicleIn() : nullptr;
    RadioChannel* nChannel = nullptr;
    if (transport &&
        (transport->DriverBrain() == unit || transport->CommanderBrain() == unit || transport->GunnerBrain() == unit))
    {
        nChannel = &transport->GetRadio();
    }

    if (_channelVehicle != nChannel)
    {
        if (_channelVehicle)
        {
            _channelVehicle->SetAudible(false);
        }
        _channelVehicle = nChannel;
    }
    if (_channelVehicle)
    {
        _channelVehicle->SetAudible(true);
    }
}

bool World::LookAroundEnabled() const
{
    if (InputSubsystem::Instance().IsLookAroundEnabled())
    {
        return true;
    }
    AIUnit* unit = FocusOn();
    const Man* man = unit ? dyn_cast<Man>(unit->GetPerson()) : nullptr;
    if (man && man->GetConfiguredCargoWeaponSeat())
        return false;
    return !unit || unit->IsInCargo();
}

bool World::MouseControlEnabled() const
{
    return fabs(_camHeading[_camType]) < 0.1 && !InputSubsystem::Instance().IsLookAroundEnabled();
}

void World::BrowseCamera(Object* vehicle)
{
    if (vehicle == _cameraOn)
    {
        return;
    }
    _ui->ResetHUD();
    _cameraOn = vehicle;
    SetActiveChannels();

#if 1
    EntityAI* ai = dyn_cast<EntityAI>(vehicle);
    if (ai && _ui)
    {
        _ui->ResetVehicle(ai);
    }
#endif
    DistributeNearImportances();
    DistributeFarImportances();

    InitCameraPars();
}

void World::BrowseCamera(int dir)
{
#if 1
    int i;
    Entity* nearestDown = nullptr;
    Entity* nearestUp = nullptr;
    Entity* mostDown = nullptr;
    Entity* mostUp = nullptr;
    Object* camVehicle = _cameraOn;
    for (i = 0; i < NVehicles(); i++)
    {
        Entity* vehicle = GetVehicle(i);
        if (vehicle == _cameraOn)
        {
            continue;
        }
        if (vehicle->Static())
        {
            continue;
        }
        if (vehicle->GetType() == TypeTempVehicle)
        {
            continue;
        }
        if (vehicle->IsDammageDestroyed())
        {
            continue;
        }
        EntityAI* vehicleAI = dyn_cast<EntityAI>(vehicle);
        if (!vehicleAI)
        {
            continue;
        }
        if (!vehicleAI->CommanderUnit())
        {
            continue;
        }
        if (vehicle > camVehicle)
        {
            if (!nearestUp || vehicle < nearestUp)
            {
                nearestUp = vehicle;
            }
        }
        if (vehicle < camVehicle)
        {
            if (!nearestDown || vehicle > nearestDown)
            {
                nearestDown = vehicle;
            }
        }
        if (!mostUp || vehicle > mostUp)
        {
            mostUp = vehicle;
        }
        if (!mostDown || vehicle < mostDown)
        {
            mostDown = vehicle;
        }
    }
    if (dir > 0)
    {
        if (nearestUp)
        {
            BrowseCamera(nearestUp);
        }
        else if (mostDown)
        {
            BrowseCamera(mostDown);
        }
    }
    else if (dir < 0)
    {
        if (nearestDown)
        {
            BrowseCamera(nearestDown);
        }
        else if (mostUp)
        {
            BrowseCamera(mostUp);
        }
    }
#endif
}

void World::VehicleSwitched(Object* from, Object* to)
{
    if (_cameraEffect && _cameraEffect->GetObject() == from)
    {
        _cameraEffect->SetObject(to);
        _cameraEffect->Simulate(0);
    }
}

void World::SwitchCameraTo(Object* vehicle, CameraType camType)
{
    // Ordinary boarding (including reboarding) targets the carrier. Supported
    // personal-weapon cargo needs the soldier's camera and cursor constraints.
    if (Man* passenger = dyn_cast<Man>(PlayerOn()); passenger && passenger->GetConfiguredCargoWeaponSeat() &&
        passenger->Brain()->GetVehicleIn() == vehicle)
        vehicle = passenger;
    bool change = (vehicle != _cameraOn);
    float changeDist2 = 1e10;
    if (vehicle && _cameraOn)
    {
        changeDist2 = vehicle->Position().Distance2(_cameraOn->Position());
    }
    if (vehicle != _cameraOn || _camTypeMain != camType)
    {
        Object* oldCameraOn = _cameraOn;
        _cameraOn = vehicle;
        _camType = _camTypeMain = camType;
        InitCameraPars();
        SetActiveChannels();
        if (change)
        {
            EntityAI* ai = dyn_cast<EntityAI>(vehicle);
            if (_ui && ai)
            {
                Person* player = PlayerOn();
                if (player == ai || oldCameraOn == player)
                {
                    InputSubsystem::Instance().ResetLookAroundToggle();
                }
                ai->ResetFF();
                _ui->ResetVehicle(ai);
            }
        }
        LOG_DEBUG(World, "Camera switched to {}", (const char*)vehicle->GetDebugName());
    }
    _nearImportanceDistributionTime = Glob.time - 60;
    _farImportanceDistributionTime = Glob.time - 60;
    if (vehicle && change && changeDist2 > Square(400))
    {
        _scene.GetLandscape()->FillCache(*vehicle);
    }
}

static void KeepNZone(float& value, float cursor, float pos, float size, float fov)
{
    float minNZone = pos - size;
    float maxNZone = pos + size;
    if (cursor < minNZone)
    {
        value += (minNZone - cursor) * fov;
    }
    else if (cursor > maxNZone)
    {
        value -= (cursor - maxNZone) * fov;
    }
}

inline Matrix3 CameraChange(float heading, float dive)
{
    return Matrix3(MRotationY, heading) * Matrix3(MRotationX, dive);
}

#if 1

struct SecondaryContext
{
    float deltaT;
    float noAccDeltaT;
    Entity* cameraVehicle;
    bool insideVehicle;
    World* world;
};

static void DoBackgroundSimulate(void* context)
{
    SecondaryContext* sContext = static_cast<SecondaryContext*>(context);
    sContext->world->BackgroundSimulate(sContext->deltaT, sContext->noAccDeltaT, sContext->cameraVehicle,
                                        sContext->insideVehicle);
}
#endif

void World::PrimaryAllowSwitch(int ms)
{
#if BACKGROUND_AI
    if (_secThread)
        _secThread->RunSecondary(ms);
#endif
}

void World::SecondaryAllowSwitch()
{
#if BACKGROUND_AI
    if (_secThread)
        _secThread->AllowSwitch();
#endif
}

const float IntroRiseTimeOfDay = 6 * OneHour + 15 * OneMinute;

void World::BackgroundSimulate(float deltaT, float noAccDeltaT, Entity* cameraVehicle, bool insideVehicle)
{
    PerformAI(deltaT, noAccDeltaT);
}

void World::SkipTime(float time)
{
    _timeToSkip = time;
}

void World::SimulateLandscape(float deltaT)
{
    LightSun* sun = _scene.MainLight();
    float timeD = deltaT * OneSecond;
    auto& input = InputSubsystem::Instance();

    if (!IsSimulationEnabled())
    {
        timeD = 0;
    }
#if _ENABLE_CHEATS
    {
        timeD += deltaT * OneHour * input.GetCheat1(SDL_SCANCODE_T);
        timeD -= deltaT * OneHour * input.GetCheat1(SDL_SCANCODE_Y);
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_G))
        timeD += OneDay;
    if (input.GetCheat1ToDo(SDL_SCANCODE_H))
        timeD -= OneDay;
#endif

    timeD += _timeToSkip;
    _timeToSkip = 0;
    //
    if (Glob.clock.AdvanceTime(timeD))
    {
        sun->Recalculate(this);
    }
    float scaledTime = timeD * (1 / OneSecond);
    GLOB_LAND->Simulate(scaledTime);
    _scene.MainLightChanged();

#if _ENABLE_CHEATS
    if (_mode != GModeNetware)
    {
        if (input.GetCheat1ToDo(SDL_SCANCODE_MINUS))
        {
            GLandscape->ResampleTerrain(+1);
            Foundation::GlobalShowMessage(1000, "Terrain subdivision %d", TerrainRangeLog - LandRangeLog);
        }
        if (input.GetCheat1ToDo(SDL_SCANCODE_EQUALS))
        {
            GLandscape->SubdivideTerrain(+1);
            Foundation::GlobalShowMessage(1000, "Terrain subdivision %d", TerrainRangeLog - LandRangeLog);
        }
    }
#endif

    if (scaledTime > 0 && !_freezeWeather)
    {
        saturate(_wantedOvercast, 0, 1);
        saturate(_wantedFog, 0, 1);
        float diffOvercast = _wantedOvercast - _actualOvercast;
        float diffFog = _wantedFog - _actualFog;
        saturate(diffOvercast, -_speedOvercast * scaledTime, +_speedOvercast * scaledTime);
        saturate(diffFog, -_speedFog * scaledTime, +_speedFog * scaledTime);
        _actualOvercast += diffOvercast;
        _actualFog += diffFog;

        _weatherTime += scaledTime;
        while (_weatherTime >= _nextWeatherChange)
        {
            _actualOvercast = _wantedOvercast;
            _actualFog = _wantedFog;
            float gRand = (GRandGen.RandomValue() + GRandGen.RandomValue()) * 0.5;
            float time = (gRand + 0.1) * 12 * 60 * 60 + 20 * 60; // in sec;
            _nextWeatherChange += time;
            const float maxChange = 0.5;
            const float maxFogChange = 0.2;
            _wantedOvercast += GRandGen.RandomValue() * (maxChange * 2) - maxChange;
            _wantedFog += GRandGen.RandomValue() * (maxFogChange * 2) - maxFogChange;
            _speedOvercast = fabs(_wantedOvercast - _actualOvercast) / time;
            _speedFog = fabs(_wantedFog - _actualFog) / time;
        }
    }
    GLOB_LAND->SetOvercast(_actualOvercast);
    GLOB_LAND->SetFog(_actualFog);

    // THE one wind tick. Everything downstream — grass, smoke, cloth, parachutes,
    // helicopter turbulence, the cloud deck, the FFT ocean spectrum, and (opt-in)
    // ballistics — reads the cached sample this produces; nobody re-evaluates and
    // nobody rolls its own motion.
    //
    // Placed here, at the tail of the weather step, for three reasons:
    //   * _actualOvercast is final for this frame, so the wind is coupled to the
    //     overcast the sky and fog are about to be drawn with, not last frame's;
    //   * SimulateLandscape runs before SimulateAllVehicles, so every entity that
    //     reads wind this frame reads the value for this frame;
    //   * it is on the simulation side, so the renderer can only ever sample it.
    //
    // NetTime, not raw Glob.time: it is the clock corrected by NetTimeOffset, so
    // two machines in the same session evaluate the same closed form at the same
    // argument and get the same vector without a single replicated byte.
    //
    // Ticked even while _freezeWeather is set: freezing the forecast holds
    // overcast still, and the wind then holds its mean while still breathing,
    // which is what the dev panel wants when scrubbing time of day.
    GWind.Update(Glob.NetTime().toInt(), _actualOvercast);

    // Local airflow rides on top of the prevailing wind and must be rebuilt
    // after it, from the same frame's world state. Ranked by distance from the
    // camera: a rotor wake nobody can see moves nothing anybody can see, and the
    // emitter list is deliberately small.
    GAirflow.Update(CameraOn() ? CameraOn()->Position() : VZero);
}

void World::SetWeather(float overcast, float fog, float time)
{
    if (overcast < 0)
    {
        overcast = _actualOvercast;
    }
    if (fog < 0)
    {
        fog = _actualFog;
    }
    saturate(overcast, 0, 1);
    saturate(fog, 0, 1);

    if (time <= 0)
    {
        _actualOvercast = _wantedOvercast = overcast;
        _actualFog = _wantedFog = fog;
        _nextWeatherChange = _weatherTime;
        _speedOvercast = 0;
        _speedFog = 0;

        GLandscape->SetOvercast(_actualOvercast);
        GLandscape->SetFog(_actualFog);
    }
    else
    {
        _wantedOvercast = overcast;
        _wantedFog = fog;
        _nextWeatherChange = _weatherTime + time;
        _speedOvercast = fabs(_wantedOvercast - _actualOvercast) / time;
        _speedFog = fabs(_wantedFog - _actualFog) / time;
    }
}

void World::SetDate(int year, int month, int day, int hour, int minute)
{
    day--;
    for (int m = 0; m < month - 1; m++)
    {
        day += Poseidon::GetDaysInMonth(year, m);
    }
    float time = day * OneDay + hour * OneHour + minute * OneMinute + 0.5 * OneSecond;

    Glob.clock.SetTimeInYear(time);
    Glob.clock.SetTimeOfDay(hour, minute);
    Glob.clock.SetYear(year);

    LightSun* sun = _scene.MainLight();
    sun->Recalculate(this);
    _scene.MainLightChanged();

    // LAMP-002: SetDate is a time JUMP, so every static object that latched a decision
    // against the old clock has to re-decide. Street lamps are the visible case: a
    // StreetLamp evaluates its LSAuto day/night gate in Init() and then only again when
    // it re-simulates (SetSimulationPrecision(12.1256), and static objects out of the
    // active set may not re-simulate at all), so after `setDate [...,23,0]` -- or after
    // --test-world-hour, which calls this from the app loop -- a night village stayed
    // dark for the whole session. World::Simulate already broadcasts this once on
    // _firstFrame for the mission-load path (mission Intel time); the jump path had no
    // equivalent. SkipDayTime does broadcast it, which is why `skipTime` lit lamps and
    // `setDate` did not.
    if (GLandscape)
    {
        GLandscape->OnTimeSkipped();
    }
}

void World::ProcessNetwork()
{
    GetNetworkManager().OnSimulate();
}

void World::DisableUserInput(bool disable)
{
    auto& input = InputSubsystem::Instance();
    if (disable)
    {
        if (_userInputDisabled++ == 0)
        {
            input.ForgetKeys();
        }
    }
    else
    {
        _userInputDisabled--;
    }
}

void World::SetCameraEffect(CameraEffect* effect)
{
    _cameraEffect = effect;
    _nearImportanceDistributionTime = Glob.time - 60;
    _farImportanceDistributionTime = Glob.time - 60;
    _playerSuspended = (effect != nullptr);
}

void World::SetTitleEffect(TitleEffect* effect)
{
    _titleEffect = effect;
    Log("SetTitleEffect %p", (void*)effect);
}

void World::SetCutEffect(TitleEffect* effect)
{
    _cutEffect = effect;
    Log("SetCutEffect %p", (void*)effect);
}

void World::AddScript(Script* script)
{
    int i = _scripts.Add(script);
    (void)i;
}

void World::TerminateScript(Script* script)
{
    int index = _scripts.Find(script);
    if (index < 0)
    {
        return;
    }
    _scripts.Delete(index);
}

void World::SimulateScripts()
{
    for (int i = 0; i < _scripts.Size();)
    {
        if (_scripts[i]->OnSimulate())
        {
            _scripts.Delete(i);
        }
        else
        {
            i++;
        }
    }
}

void World::SetCameraScript(Script* script)
{
    _cameraScript = script;
}

void World::TerminateCameraScript()
{
    if (GetCameraScript())
    {
        TerminateScript(GWorld->GetCameraScript());
    }
}

void World::StartCameraScript(Script* script)
{
    TerminateCameraScript();
    AddScript(script);
    SetCameraScript(script);
    SimulateScripts();
}

const float ZoomSpeed = 8;

bool BreakIntro();
#if _ENABLE_CHEATS
void DebugOperMap();
void DebugOperMap(AIUnit* unit);
void DebugOperMapTrouble();
#endif

bool showCinemaBorder = true;

void ShowCinemaBorder(bool show)
{
    showCinemaBorder = show;
}

GameValue ShowCinemaBorder(const GameState* state, GameValuePar oper1)
{
    showCinemaBorder = oper1;
    return GameValue();
}

static void DrawConnectionQuality(ConnectionQuality quality)
{
    PackedColor color;
    switch (quality)
    {
        default:
            Fail("Connection quality");
        case CQGood:
            return;
        case CQPoor:
            color = PackedColor(Color(1, 1, 0, 0.5));
            break;
        case CQBad:
            color = PackedColor(Color(1, 0, 0, 0.5));
            break;
    }
    float w = GEngine->Width2D(), h = GEngine->Height2D();
    MipInfo mip = GEngine->TextBank()->UseMipmap(nullptr, 0, 0);
    GEngine->Draw2D(mip, color, Rect2DPixel(0.97 * w, 0.96 * h, 0.03 * w, 0.04 * h));
}

static void ClipCamera(Vector3& cam, Object* focus, Vector3Val focusPos, float& maxDistSmooth)
{
    CollisionBuffer col;
    const float sDist = 0.5;
    static const float coefs[][4] = {
        // up distance, aside distance, along distance, distance coef used
        {0, 0, 1, 1}, {sDist, 0, 1.2, 1.5}, {-sDist, 0, 1.2, 1.5}, {0, sDist, 1.2, 1.5}, {0, -sDist, 1.2, 1.5},
    };
    const int nCoefs = sizeof(coefs) / sizeof(*coefs);
    Vector3 dir = cam - focusPos;

    Vector3 up = VUp;
    Vector3 aside = dir.CrossProduct(up);
    aside.Normalize();
    up = dir.CrossProduct(aside);
    up.Normalize();
    float dirSize2 = dir.SquareSize();
    float invDirSize = InvSqrt(dirSize2);
    float dirSize = dirSize2 * invDirSize;

    float minMaxDist = dirSize;
    Vector3 dirNorm = dir * invDirSize;

    int nCol = 0;
    for (int c = 0; c < nCoefs; c++)
    {
        float distTest = dirSize * coefs[c][2];
        Vector3 camPosAdj = cam + up * coefs[c][0] + aside * coefs[c][1] + dirNorm * coefs[c][2];
        GLandscape->ObjectCollision(col, focus, nullptr, focusPos, camPosAdj, 0.2, ObjIntersectView);
        if (col.Size() <= 0)
        {
            continue;
        }
        bool someCol = false;
        float minT = 1;
        for (int i = 0; i < col.Size(); i++)
        {
            const CollisionInfo& info = col[i];
            if (info.object->ViewDensity() > -3.0f)
            {
                continue;
            }
            if (info.under < minT)
            {
                minT = info.under;
            }
            someCol = true;
        }

        if (someCol)
        {
            nCol++;
        }

        float maxDist = distTest * minT;
        maxDist -= 0.2f;
        saturateMax(maxDist, 0.5f);
        saturateMax(maxDist, 0.333f * dirSize);
        saturateMin(minMaxDist, maxDist * coefs[c][3]);
    }

    if (nCol > 3)
    {
        saturateMin(maxDistSmooth, minMaxDist);
    }
    cam = focusPos + dirNorm * floatMin(dirSize, maxDistSmooth);
}

bool IsPlayerDead()
{
    Person* player = GWorld->GetRealPlayer();
    if (!player)
    {
        return true;
    }
    if (!player->Brain())
    {
        return true;
    }
    return player->Brain()->GetLifeState() == AIUnit::LSDead;
}

ChatChannel ActualChatChannel();
void SetChatChannel(ChatChannel channel);
void PrevChatChannel();
void NextChatChannel();

#if _ENABLE_CHEATS
bool forceControlsPaused = false;
#endif
