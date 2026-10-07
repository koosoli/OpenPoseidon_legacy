#include "SimStageEvidence.hpp"

// Every chain below was walked by hand in the source on 2026-08-31 and then written down
// so it never has to be walked by hand again. The audit that produced them is
// design notes
//
// Read a chain top to bottom. Hop 0's `symbol` is the stage's entry point as
// `World::StepSimulation` calls it; the last hop's `contains` is the statement that
// actually touches the resource. `indirect` marks the one kind of hop a text check cannot
// follow, and there are exactly two such calls in the engine's tick: the SQF command table
// (`gstate->Execute`) and the entity dispatch (`(vehicle->*simul)`). Nine chains cross one
// of them, and the test bounds that count in both directions so a table that quietly became
// all-indirect fails rather than passing by skipping.

namespace Poseidon::Sim
{

namespace
{

// Roots, spelled once so a renamed entry point breaks in one place.
constexpr std::string_view kWorldSetup = "World/WorldSetup.cpp";
constexpr std::string_view kWorldImpl = "World/WorldImpl.cpp";
constexpr std::string_view kWorldCpp = "World/World.cpp";
constexpr std::string_view kScripts = "Game/Scripting/Scripts.cpp";
constexpr std::string_view kAIUnitImpl = "AI/AIUnitImpl.cpp";
constexpr std::string_view kTarget = "World/Detection/Target.cpp";
constexpr std::string_view kSensors = "World/Terrain/Visibility.cpp";

constexpr std::string_view kSimLandscape = "void World::SimulateLandscape(";
constexpr std::string_view kSimScripts = "void World::SimulateScripts()";
constexpr std::string_view kPerformAI = "void World::PerformAI(";
constexpr std::string_view kSimAllVehicles = "void World::SimulateAllVehicles(";
constexpr std::string_view kSmartUpdateAll = "int SensorList::SmartUpdateAll()";
constexpr std::string_view kStepSimulation = "void World::StepSimulation(";

// The prefix every AI chain shares: PerformAI -> centre -> group. Nine of the twelve hops
// in the AI stage's longest chain are this walk, which is precisely why nobody re-derives
// it when they edit a declaration.
constexpr SimEvidenceHop kAIWalkToGroup[] = {
    {kWorldImpl, kPerformAI, "_eastCenter->Think()"},
    {"AI/AICenterImplPreview.cpp", "void AICenter::Think()", "UpdateGroup();"},
    {"AI/AICenterImplPreview.cpp", "void AICenter::UpdateGroup()", "maxGrp->Think()"},
};

SimEvidence Make(SimStage stage, SimResource resource, bool write, std::string_view note,
                 std::initializer_list<SimEvidenceHop> hops)
{
    SimEvidence e;
    e.stage = stage;
    e.resource = resource;
    e.write = write;
    e.note = note;
    for (const SimEvidenceHop& hop : hops)
    {
        if (e.hopCount < kMaxEvidenceHops)
        {
            e.hops[e.hopCount++] = hop;
        }
    }
    return e;
}

std::vector<SimEvidence> BuildEvidence()
{
    std::vector<SimEvidence> t;

    // ---------------------------------------------------------------- Clock ----------
    t.push_back(Make(SimStage::Clock, SimResource::Clock, true, "The tick's one clock advance.",
                     {{kWorldSetup, kSimLandscape, "Glob.clock.AdvanceTime("}}));

    // The finding SIM-811 did not have. The clock stage declared `reads = 0`.
    t.push_back(Make(SimStage::Clock, SimResource::VehicleState, false,
                     "Rotor wash: the wind tick ranks helicopters by distance and reads their "
                     "positions, so the FIRST stage of the tick reads entity state the LAST "
                     "vehicle stage wrote -- a previous-tick edge nobody had recorded.",
                     {{kWorldSetup, kSimLandscape, "GAirflow.Update("},
                      {"World/Weather/AirflowField.cpp", "void AirflowField::Update(", "helicopter->Position()"}}));

    // -------------------------------------------------------------- Scripts ----------
    t.push_back(
        Make(SimStage::Scripts, SimResource::Clock, false,
             "The reason scripts sit after the clock: a script observing `time` sees "
             "this tick's value.",
             {{kWorldSetup, kSimScripts, "->OnSimulate()"}, {kScripts, "bool Script::OnSimulate()", "Glob.time"}}));

    t.push_back(Make(
        SimStage::Scripts, SimResource::ScriptState, false, "Script locals, read back after the body ran.",
        {{kWorldSetup, kSimScripts, "->OnSimulate()"}, {kScripts, "bool Script::OnSimulate()", "gstate->VarGet("}}));

    t.push_back(Make(
        SimStage::Scripts, SimResource::ScriptState, true, "Script locals.",
        {{kWorldSetup, kSimScripts, "->OnSimulate()"}, {kScripts, "bool Script::OnSimulate()", "gstate->VarSet("}}));

    t.push_back(Make(SimStage::Scripts, SimResource::VehicleState, true,
                     "What a script is for. `eject` is one command of several hundred; the "
                     "chain crosses the command table, which no text check can follow.",
                     {{kWorldSetup, kSimScripts, "->OnSimulate()"},
                      {kScripts, "bool Script::OnSimulate()", "SimulateBody()"},
                      {kScripts, "bool Script::SimulateBody()", "gstate->Execute(", /*indirect*/ true},
                      {"Game/Commands/GameStateExtWorld.cpp", "static void Eject(", "unit->DoGetOut("},
                      {kAIUnitImpl, "void AIUnit::DoGetOut(", "getOutTo->SetSpeed("}}));

    t.push_back(Make(SimStage::Scripts, SimResource::AIState, true,
                     "Same chain, different statement: getting out clears the unit's plan.",
                     {{kWorldSetup, kSimScripts, "->OnSimulate()"},
                      {kScripts, "bool Script::OnSimulate()", "SimulateBody()"},
                      {kScripts, "bool Script::SimulateBody()", "gstate->Execute(", /*indirect*/ true},
                      {"Game/Commands/GameStateExtWorld.cpp", "static void Eject(", "unit->DoGetOut("},
                      {kAIUnitImpl, "void AIUnit::DoGetOut(", "ClearOperativePlan()"}}));

    t.push_back(Make(SimStage::Scripts, SimResource::VehicleState, false,
                     "The same command reads where the vehicle is before it moves anybody.",
                     {{kWorldSetup, kSimScripts, "->OnSimulate()"},
                      {kScripts, "bool Script::OnSimulate()", "SimulateBody()"},
                      {kScripts, "bool Script::SimulateBody()", "gstate->Execute(", /*indirect*/ true},
                      {"Game/Commands/GameStateExtWorld.cpp", "static void Eject(", "unit->DoGetOut("},
                      {kAIUnitImpl, "void AIUnit::DoGetOut(", "veh->Position()"}}));

    // ------------------------------------------------------------------- AI ----------
    t.push_back(Make(SimStage::AI, SimResource::Clock, false, "Target memory ages against the tick's clock.",
                     {{kWorldImpl, kPerformAI, "_eastCenter->Think()"},
                      {"AI/AICenterImplPreview.cpp", "void AICenter::Think()", "Glob.time"}}));

    t.push_back(Make(SimStage::AI, SimResource::AIState, true, "The centre's target database.",
                     {{kWorldImpl, kPerformAI, "_eastCenter->Think()"},
                      {"AI/AICenterImplPreview.cpp", "void AICenter::Think()", "_targets.Delete("}}));

    // The edge SIM-810 flagged and SIM-811 named. Nine hops. This is the entry that
    // justifies calling the declarations hand-audited rather than obvious.
    t.push_back(Make(SimStage::AI, SimResource::Visibility, false,
                     "THE previous-tick edge: the AI decides on sensor results the Visibility "
                     "stage will not recompute until later in this same tick.",
                     {kAIWalkToGroup[0],
                      kAIWalkToGroup[1],
                      kAIWalkToGroup[2],
                      {"AI/AIGroup.cpp", "bool AIGroup::Think()", "CreateTargetList(false, true,"},
                      {"AI/AIGroupImpl.cpp", "bool AIGroup::CreateTargetList(", "WhatIsVisible("},
                      {kTarget, "void EntityAI::WhatIsVisible(", "TrackTargets("},
                      {kTarget, "void EntityAI::TrackTargets(TargetList& res, AIUnit* unit", "CalcVisibility("},
                      {kTarget, "float EntityAI::CalcVisibility(", "GWorld->Visibility(brain, ai)"},
                      {kWorldImpl, "float World::Visibility(", "_sensorList->GetVisibility("}}));

    t.push_back(Make(SimStage::AI, SimResource::VehicleState, false, "Ranging to targets.",
                     {kAIWalkToGroup[0],
                      kAIWalkToGroup[1],
                      kAIWalkToGroup[2],
                      {"AI/AIGroup.cpp", "bool AIGroup::Think()", "CreateTargetList(false, true,"},
                      {"AI/AIGroupImpl.cpp", "bool AIGroup::CreateTargetList(", "WhatIsVisible("},
                      {kTarget, "void EntityAI::WhatIsVisible(", "TrackTargets("},
                      {kTarget, "void EntityAI::TrackTargets(TargetList& res, AIUnit* unit", "ai->Position()"}}));

    // The second finding. SIM-811 declared the AI stage writes AIState and nothing else.
    t.push_back(Make(SimStage::AI, SimResource::VehicleState, true,
                     "An AI decision to dismount teleports a person and gives them the "
                     "vehicle's velocity -- eleven hops below `PerformAI`, and an undeclared "
                     "write to the resource the NEXT stage integrates.",
                     {kAIWalkToGroup[0],
                      kAIWalkToGroup[1],
                      kAIWalkToGroup[2],
                      {"AI/AIGroup.cpp", "bool AIGroup::Think()", "subgrp->Think(prec)"},
                      {"AI/AISubgroup.cpp", "bool AISubgroup::Think(", "unit->Think(prec)"},
                      {kAIUnitImpl, "bool AIUnit::Think(", "CheckGetOut();"},
                      {kAIUnitImpl, "void AIUnit::CheckGetOut()", "ProcessGetOut(false)"},
                      {kAIUnitImpl, "bool AIUnit::ProcessGetOut(", "DoGetOut(veh, parachute)"},
                      {kAIUnitImpl, "void AIUnit::DoGetOut(", "getOutTo->SetSpeed("}}));

    // ------------------------------------------------------------- Vehicles ----------
    t.push_back(Make(SimStage::Vehicles, SimResource::Clock, false, "Importance redistribution is clock-driven.",
                     {{kWorldImpl, kSimAllVehicles, "Glob.time >"}}));

    t.push_back(Make(SimStage::Vehicles, SimResource::VehicleState, true, "Entity integration, the point of the stage.",
                     {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
                      {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
                      {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
                      {"World/Entities/Vehicles/Ground/Car.cpp", "void Car::Simulate(", "SimulateUnits(deltaT)"},
                      {"World/Entities/Vehicles/TransportCore.cpp", "bool Transport::SimulateUnits(",
                       "_fire.SetTarget(gunnerUnit,"}}));

    t.push_back(Make(
        SimStage::Vehicles, SimResource::AIState, false, "A gunner consults what its unit knows.",
        {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
         {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
         {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
         {"World/Entities/Vehicles/Ground/Car.cpp", "void Car::Simulate(", "SimulateUnits(deltaT)"},
         {"World/Entities/Vehicles/TransportCore.cpp", "bool Transport::SimulateUnits(", "IsKnownBy(gunnerUnit)"}}));

    // The third finding. Vehicles was declared to read AIState and VehicleState only.
    t.push_back(Make(SimStage::Vehicles, SimResource::Visibility, false,
                     "Automatic fire asks whether the target can be seen -- five layers under "
                     "`SimulateAllVehicles`, and reading the same previous-tick sensor results "
                     "the AI stage does.",
                     {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
                      {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
                      {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
                      {"World/Entities/Vehicles/Ground/Car.cpp", "void Car::Simulate(", "SimulateUnits(deltaT)"},
                      {"World/Entities/Vehicles/TransportCore.cpp", "bool Transport::SimulateUnits(",
                       "WhatFireResult(result, *_fire._fireTarget, _currentWeapon, timeToLive)"},
                      {"World/Detection/TargetFire.cpp",
                       "bool EntityAI::WhatFireResult(FireResult& result, const Target& target, int weapon",
                       "GLOB_WORLD->Visibility(unit, target.idExact)"},
                      {kWorldImpl, "float World::Visibility(", "_sensorList->GetVisibility("}}));

    // The fourth finding, and the one with an ordering consequence: the read happens
    // BEFORE the physics stage that writes it.
    t.push_back(Make(SimStage::Vehicles, SimResource::PhysicsState, false,
                     "A loose object takes its pose from the solver, in the stage BEFORE the "
                     "solver steps -- so a barrel is always one tick behind.",
                     {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
                      {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
                      {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
                      {"World/Scene/Thing.cpp", "void Thing::Simulate(", "LooseObjects::Step(*this, _physicsBody)"},
                      {"World/Physics/LooseObjects.cpp", "bool Step(Entity& thing, Physics::BodyId& body)",
                       "world->GetBodyMotion(body, motion)"}}));

    t.push_back(Make(SimStage::Vehicles, SimResource::PhysicsState, true,
                     "The same hand-over spawns and removes solver bodies from inside the "
                     "vehicle stage.",
                     {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
                      {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
                      {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
                      {"World/Scene/Thing.cpp", "void Thing::Simulate(", "LooseObjects::Step(*this, _physicsBody)"},
                      {"World/Physics/LooseObjects.cpp", "bool Step(Entity& thing, Physics::BodyId& body)",
                       "world->SpawnModelProbe("}}));

    // The fifth finding, and the one that dents the model: a stage re-enters another stage.
    t.push_back(
        Make(SimStage::Vehicles, SimResource::ScriptState, true,
             "A flare runs `onFlare.sqs` INSIDE the vehicle stage, so the Scripts "
             "stage is not the only place scripts run in a tick.",
             {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
              {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
              {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
              {"World/Entities/Weapons/Shots.cpp", "void IlluminatingShell::Simulate(", "GWorld->AddScript(script)"}}));

    t.push_back(
        Make(SimStage::Vehicles, SimResource::ScriptState, false, "…and runs them, there and then.",
             {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
              {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
              {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
              {"World/Entities/Weapons/Shots.cpp", "void IlluminatingShell::Simulate(", "GWorld->SimulateScripts()"},
              {kWorldSetup, kSimScripts, "->OnSimulate()"}}));

    t.push_back(Make(SimStage::Vehicles, SimResource::VehicleState, false, "Integration reads before it writes.",
                     {{kWorldImpl, kSimAllVehicles, "SimulateVehicles("},
                      {kWorldImpl, "void World::SimulateVehicles(", "SimulateOnly(_vehicles,"},
                      {kWorldSetup, "void World::SimulateOnly(VehicleList& vehicles", "SimulateTimed(vehicle,"},
         {kWorldSetup, "static void SimulateTimed(", "(vehicle->*simul)(", true},
                      {"World/Scene/Thing.cpp", "void Thing::Simulate(", "LooseObjects::Step(*this, _physicsBody)"},
                      {"World/Physics/LooseObjects.cpp", "bool Step(Entity& thing, Physics::BodyId& body)",
                       "thing.SetPosition(motion.position)"}}));

    // ----------------------------------------------------------- Visibility ----------
    t.push_back(Make(SimStage::Visibility, SimResource::Visibility, true, "Who can see whom, recomputed.",
                     {{kSensors, kSmartUpdateAll, "UpdateRow("},
                      {kSensors, "int SensorList::UpdateRow(", "UpdateCell(id, SensorColID(i))"},
                      {kSensors, "int SensorList::UpdateCell(", "info.SetVisibility("}}));

    t.push_back(Make(SimStage::Visibility, SimResource::VehicleState, false, "Where everything ended up.",
                     {{kSensors, kSmartUpdateAll, "UpdateRow("},
                      {kSensors, "int SensorList::UpdateRow(", "UpdateCell(id, SensorColID(i))"},
                      {kSensors, "int SensorList::UpdateCell(", "from->Position()"}}));

    t.push_back(Make(SimStage::Visibility, SimResource::Clock, false,
                     "Sensor results are stamped with the tick's time; undeclared before "
                     "SIM-814.",
                     {{kSensors, kSmartUpdateAll, "UpdateRow("},
                      {kSensors, "int SensorList::UpdateRow(", "UpdateCell(id, SensorColID(i))"},
                      {kSensors, "int SensorList::UpdateCell(", "info.SetLastVisible(Glob.time)"}}));

    t.push_back(Make(SimStage::Visibility, SimResource::AIState, false,
                     "A dead sensor is retired only if nobody commands it; undeclared before "
                     "SIM-814.",
                     {{kSensors, kSmartUpdateAll, "CheckPos();"},
                      {kSensors, "void SensorList::CheckPos()", "veh->CommanderUnit()"}}));

    // -------------------------------------------------------------- Physics ----------
    // This stage has no function of its own; its body is inline in the dispatch, so the
    // root and the leaf are the same symbol.
    t.push_back(Make(SimStage::Physics, SimResource::VehicleState, false, "Where to put the player proxy.",
                     {{kWorldCpp, kStepSimulation, "Dev::UpdatePhysicsPlayerProxy("}}));

    t.push_back(Make(SimStage::Physics, SimResource::PhysicsState, true, "The one solver step per tick.",
                     {{kWorldCpp, kStepSimulation, "Physics::GetPhysicsWorld()->Step(step.deltaT)"}}));

    t.push_back(Make(SimStage::Physics, SimResource::PhysicsState, false,
                     "Carry-over: the solver integrates the state it holds. Not an edge, but "
                     "declared so the resource is not silently absent.",
                     {{kWorldCpp, kStepSimulation, "Physics::GetPhysicsWorld()->Step(step.deltaT)"}}));

    return t;
}

std::vector<SimDeclWidening> BuildWidenings()
{
    return {
        // Kept from SIM-811 rather than deleted. A grep over `Game/Commands` for
        // `GLOB_WORLD->Visibility`, `GetSensorList` and `GetVisibility` finds nothing, so no
        // path was written down -- but the command table is several hundred entries and a
        // MISSING read is the dangerous direction, exactly as SIM-811 argued. Unwiden by
        // finding the command, or by proving the absence over the whole table.
        {SimStage::Scripts, SimResource::Visibility, false,
         "No witness found by grep over Game/Commands; retained because the command table is "
         "large and an under-declared read is the direction that hurts."},
        // The AI stage reads more than target ranging; the declaration keeps AIState as a
        // read even though the evidenced chains only show writes and a self-read, because a
        // centre reading its own database across a tick is carry-over and a stage reading
        // another centre's is not distinguished at this granularity.
        {SimStage::AI, SimResource::AIState, false,
         "Self-read carry-over at this granularity; kept so the resource is present in the "
         "declaration rather than inferred from the write."},
        {SimStage::Scripts, SimResource::AIState, false,
         "Commands read group and unit state constantly (`knowsAbout`, `leader`, ...); one "
         "chain would be arbitrary among hundreds."},
    };
}

// Shorthands for the two site kinds, so the tables below read as data.
SimCensusSite InTick(std::string_view file, std::string_view line, SimStage stage, bool write, std::string_view reason)
{
    SimCensusSite s;
    s.file = file;
    s.line = line;
    s.inTick = true;
    s.stage = stage;
    s.write = write;
    s.reason = reason;
    return s;
}

SimCensusSite OutOfTick(std::string_view file, std::string_view line, std::string_view reason)
{
    SimCensusSite s;
    s.file = file;
    s.line = line;
    s.inTick = false;
    s.reason = reason;
    return s;
}

std::vector<SimResourceCensusEntry> BuildCensus()
{
    std::vector<SimResourceCensusEntry> c;

    {
        SimResourceCensusEntry e;
        e.resource = SimResource::PhysicsState;
        e.witness = "Physics::GetPhysicsWorld()";
        e.sites = {
            InTick("World/Entities/Weapons/Shots.cpp", "Physics::PhysicsWorld* physics = Physics::GetPhysicsWorld();",
                   SimStage::Vehicles, false,
                   "shell penetration casts against the collider set; shots simulate "
                   "inside the vehicle stage"),
            InTick("World/Physics/LooseObjects.cpp", "Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();",
                   SimStage::Vehicles, true, "the loose-object hand-over, called from Thing::Simulate"),
            InTick("World/World.cpp", "if (Physics::GetPhysicsWorld() == nullptr)", SimStage::Physics, false,
                   "the stage gate, evaluated at the top of the tick"),
            InTick("World/World.cpp", "Physics::GetPhysicsWorld()->Step(step.deltaT);", SimStage::Physics, true,
                   "the one solver step per tick"),
            OutOfTick("World/Physics/LooseObjects.cpp",
                      "if (Physics::PhysicsWorld* world = Physics::GetPhysicsWorld())",
                      "Release(), teardown from a destructor and from the dev panel"),
            OutOfTick("World/Physics/PhysicsWorld.cpp", "", "the accessor's own definition"),
            OutOfTick("World/Physics/PhysicsWorld.hpp", "", "its declaration"),
            OutOfTick("Dev/Diag/PhysicsCorpus.cpp", "", "dev panel"),
            OutOfTick("Dev/Diag/PhysicsProbe.cpp", "", "dev panel"),
            OutOfTick("Dev/Diag/PhysicsRayAudit.cpp", "", "dev panel"),
            OutOfTick("Dev/Diag/PhysicsTab.cpp", "", "dev panel"),
        };
        c.push_back(std::move(e));
    }

    {
        SimResourceCensusEntry e;
        e.resource = SimResource::Visibility;
        e.witness = "GetSensorList()";
        e.sites = {
            InTick("World/World.cpp", "world.GetSensorList()->SmartUpdateAll();", SimStage::Visibility, true,
                   "the Visibility stage itself"),
            OutOfTick("World/WorldInit.cpp", "GetSensorList()->UpdateAll();", "mission init, before any tick"),
            OutOfTick("World/Viewer.cpp", "GetSensorList()->UpdateAll();", "the map/viewer path"),
            OutOfTick("World/World.hpp", "", "the accessor itself"),
            OutOfTick("UI/InGame/InGameUIDrawCursor.cpp", "", "the HUD, on the render side"),
        };
        c.push_back(std::move(e));
    }

    {
        SimResourceCensusEntry e;
        e.resource = SimResource::ScriptState;
        e.witness = "SimulateScripts()";
        e.sites = {
            InTick("World/World.cpp", "world.SimulateScripts();", SimStage::Scripts, true, "the Scripts stage itself"),
            // The re-entrancy. Listing it here is what makes the census refuse to be
            // satisfied unless the Vehicles stage declares ScriptState.
            InTick("World/Entities/Weapons/Shots.cpp", "GWorld->SimulateScripts();", SimStage::Vehicles, true,
                   "a flare runs onFlare.sqs from inside IlluminatingShell::Simulate"),
            OutOfTick("World/WorldSetup.cpp", "", "the definition, and camera-script start-up"),
            OutOfTick("World/WorldInit.cpp", "", "mission init"),
            OutOfTick("World/World.hpp", "", "the declaration"),
            // Bare, unqualified: World::Simulate's per-RENDERED-FRAME call, the one that
            // runs when POSEIDON_FIXEDSTEP_SCRIPTS is off -- which is the default. The
            // qualified `world.SimulateScripts();` above is the in-tick one, and the
            // longest-match rule keeps the two apart.
            OutOfTick("World/World.cpp", "SimulateScripts();", "the per-rendered-frame call, outside StepSimulation"),
            OutOfTick("UI/DisplayUIMenus.cpp", "", "UI event handlers"),
            OutOfTick("UI/DisplayUI.cpp", "", "UI event handlers"),
            OutOfTick("UI/Controls/UIControlsSlider.cpp", "", "UI event handlers"),
            OutOfTick("UI/Controls/UIControls3D.cpp", "", "UI event handlers"),
            OutOfTick("UI/Controls/UIControls.cpp", "", "UI event handlers"),
        };
        c.push_back(std::move(e));
    }

    return c;
}

} // namespace

const std::vector<SimEvidence>& SimStageEvidence()
{
    static const std::vector<SimEvidence> table = BuildEvidence();
    return table;
}

const std::vector<SimDeclWidening>& SimStageWidenings()
{
    static const std::vector<SimDeclWidening> table = BuildWidenings();
    return table;
}

const std::vector<SimResourceCensusEntry>& SimResourceCensus()
{
    static const std::vector<SimResourceCensusEntry> table = BuildCensus();
    return table;
}

std::uint32_t CensusedResources()
{
    std::uint32_t mask = 0;
    for (const SimResourceCensusEntry& e : SimResourceCensus())
    {
        mask |= SimResourceMask(e.resource);
    }
    return mask;
}

} // namespace Poseidon::Sim
