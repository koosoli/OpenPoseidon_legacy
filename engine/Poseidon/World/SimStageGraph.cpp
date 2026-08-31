#include <chrono>
#include "SimStageGraph.hpp"

// The declarations below were read off the call sites in World.cpp (SIM-810's table),
// one stage at a time. Where a stage's reach was uncertain the declaration is WIDER
// rather than narrower: an over-declared read produces a spurious edge, which somebody
// investigates and removes, whereas an under-declared read produces a missing edge,
// which nobody ever notices.
//
// SIM-814: every bit below is now backed by a recorded call path in `SimStageEvidence.cpp`
// or by a named widening in the same file, and a test walks both. Do not add a bit here
// without adding its evidence -- the test fails, by design, and names the stage and the
// resource. Six bits were ADDED by that audit, all of them reached through two or more
// layers of call, and each is marked (SIM-814) below.

namespace Poseidon::Sim
{

namespace
{

constexpr std::uint32_t Res(SimResource r)
{
    return SimResourceMask(r);
}

// Indexed by SimStage. Order must match the enum.
constexpr std::array<SimStageDecl, kSimStageCount> kDecls = {{
    // Clock -- `Glob.time += deltaT; SimulateLandscape(deltaT)`. Advances the clock and
    // steps weather and wind.
    //
    // (SIM-814) It also READS VehicleState, which SIM-811 recorded as `reads = 0`. The
    // wind tick ends with `GAirflow.Update(...)`, which ranks helicopters by distance
    // from the camera and reads their positions and rotor speeds. The first stage of the
    // tick therefore consumes what the fourth stage wrote -- last tick's.
    {SimStage::Clock, Res(SimResource::VehicleState), Res(SimResource::Clock)},

    // Scripts -- `SimulateScripts()`. Reads `time` (that is WHY it sits after the clock),
    // and reads world state broadly through game commands. Writes script state, and
    // writes AI and vehicle state because that is what a script is for: issuing orders
    // and moving things. Those writes are why its position ahead of the AI matters.
    {SimStage::Scripts, Res(SimResource::Clock) | Res(SimResource::ScriptState) | Res(SimResource::AIState) |
                            Res(SimResource::VehicleState) | Res(SimResource::Visibility),
     Res(SimResource::ScriptState) | Res(SimResource::AIState) | Res(SimResource::VehicleState)},

    // AI -- `PerformAI`. Reads the clock, its own carried state, where everything is,
    // and WHO CAN SEE WHOM. That last read is the one that matters: Visibility is
    // written at stage 5, after this, so this read resolves to the previous tick.
    //
    // (SIM-814) It also WRITES VehicleState. `AIUnit::Think` -> `CheckGetOut` ->
    // `ProcessGetOut` -> `DoGetOut` sets a person's transform and hands them the
    // vehicle's velocity, eleven hops below `PerformAI`. The stage that integrates entity
    // motion runs immediately after, so this is a same-tick edge and not a race today --
    // but it was undeclared, and under Phase 8's concurrency it would have been one.
    {SimStage::AI,
     Res(SimResource::Clock) | Res(SimResource::AIState) | Res(SimResource::VehicleState) |
         Res(SimResource::Visibility),
     Res(SimResource::AIState) | Res(SimResource::VehicleState)},

    // Vehicles -- `SimulateAllVehicles`. Consumes the orders the AI just wrote and
    // integrates entity motion.
    //
    // (SIM-814) Three additions, all reached five layers down through the
    // `(vehicle->*simul)` dispatch:
    //   * Visibility, read -- automatic fire asks `WhatFireResult`, which asks
    //     `GLOB_WORLD->Visibility`. The same previous-tick sensor results the AI reads.
    //   * PhysicsState, read and write -- `Thing::Simulate` hands loose objects to the
    //     solver and reads their pose back, in the stage BEFORE the solver steps.
    //   * ScriptState, read and write -- `IlluminatingShell::Simulate` adds and RUNS a
    //     script (`onFlare.sqs`) from inside this stage, so "scripts are a stage" is not
    //     the whole truth.
    {SimStage::Vehicles,
     Res(SimResource::Clock) | Res(SimResource::ScriptState) | Res(SimResource::AIState) |
         Res(SimResource::VehicleState) | Res(SimResource::Visibility) | Res(SimResource::PhysicsState),
     Res(SimResource::ScriptState) | Res(SimResource::VehicleState) | Res(SimResource::PhysicsState)},

    // Visibility -- `GetSensorList()->SmartUpdateAll()`. Reads where everything ended up
    // and recomputes who can see whom.
    //
    // (SIM-814) Also reads the clock -- every cell it writes is stamped with `Glob.time`
    // -- and AIState, because `CheckPos` retires a dead sensor only when nothing
    // commands it.
    {SimStage::Visibility, Res(SimResource::Clock) | Res(SimResource::AIState) | Res(SimResource::VehicleState),
     Res(SimResource::Visibility)},

    // Physics -- the player-proxy move plus one `PhysicsWorld::Step`. Reads the player's
    // (or camera vehicle's) position to place the proxy, then steps its own world.
    {SimStage::Physics, Res(SimResource::VehicleState) | Res(SimResource::PhysicsState),
     Res(SimResource::PhysicsState)},
}};

} // namespace

std::string_view SimStageName(SimStage stage)
{
    switch (stage)
    {
    case SimStage::Clock:
        return "clock";
    case SimStage::Scripts:
        return "scripts";
    case SimStage::AI:
        return "ai";
    case SimStage::Vehicles:
        return "vehicles";
    case SimStage::Visibility:
        return "visibility";
    case SimStage::Physics:
        return "physics";
    }
    return "?";
}

std::string_view SimResourceName(SimResource resource)
{
    switch (resource)
    {
    case SimResource::Clock:
        return "clock";
    case SimResource::ScriptState:
        return "scriptState";
    case SimResource::AIState:
        return "aiState";
    case SimResource::VehicleState:
        return "vehicleState";
    case SimResource::Visibility:
        return "visibility";
    case SimResource::PhysicsState:
        return "physicsState";
    }
    return "?";
}

const std::array<SimStageDecl, kSimStageCount>& SimStageDecls()
{
    return kDecls;
}

const SimStageDecl& SimStageDeclFor(SimStage stage)
{
    return kDecls[static_cast<std::size_t>(stage)];
}

std::vector<SimStageEdge> AnalyseSerialOrder(SimStageSpan order)
{
    std::vector<SimStageEdge> edges;

    for (std::size_t consumerPos = 0; consumerPos < order.size(); ++consumerPos)
    {
        const SimStage      consumer = order[consumerPos];
        const SimStageDecl& consumerDecl = SimStageDeclFor(consumer);

        for (std::size_t r = 0; r < kSimResourceCount; ++r)
        {
            const auto resource = static_cast<SimResource>(r);
            if ((consumerDecl.reads & SimResourceMask(resource)) == 0)
            {
                continue;
            }

            // Nearest earlier writer: walk backwards so the LAST write before the read
            // wins, which is the one the consumer actually observes.
            bool     found = false;
            SimStage producer{};
            bool     sameTick = false;

            for (std::size_t back = consumerPos; back-- > 0;)
            {
                if ((SimStageDeclFor(order[back]).writes & SimResourceMask(resource)) != 0)
                {
                    producer = order[back];
                    sameTick = true;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                // Nobody earlier wrote it. Walk backwards from the END of the tick: the
                // last stage to write it before the order wraps is what this read sees.
                for (std::size_t back = order.size(); back-- > consumerPos + 1;)
                {
                    if ((SimStageDeclFor(order[back]).writes & SimResourceMask(resource)) != 0)
                    {
                        producer = order[back];
                        sameTick = false;
                        found = true;
                        break;
                    }
                }
            }

            // Reading only what you yourself write is carry-over state, not an edge.
            if (found)
            {
                edges.push_back({producer, consumer, resource, sameTick});
            }
        }
    }

    return edges;
}

std::vector<SimStageEdge> PreviousTickEdges(SimStageSpan order)
{
    std::vector<SimStageEdge> lagged;
    for (const SimStageEdge& edge : AnalyseSerialOrder(order))
    {
        if (!edge.sameTick)
        {
            lagged.push_back(edge);
        }
    }
    return lagged;
}

SimStageTrace RunSimStagesSerial(SimStageSpan order, std::uint32_t enabledMask, void* context,
                                 SimStageDispatch dispatch)
{
    SimStageTrace trace;
    SimStageCosts().ticks++;
    for (const SimStage stage : order)
    {
        if ((enabledMask & SimStageMask(stage)) == 0)
        {
            continue;
        }
        const auto stageStart = std::chrono::steady_clock::now();
        dispatch(context, stage);
        const float stageMs =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - stageStart).count();
        SimStageCosts().ms[static_cast<std::size_t>(stage)] += stageMs;
        // The trace is fixed-size because a tick runs each stage at most once. A caller
        // that passes a longer order still gets every stage RUN; only the record of it
        // stops growing.
        if (trace.count < kSimStageCount)
        {
            trace.ms[trace.count] = stageMs;
            trace.visited[trace.count] = stage;
            ++trace.count;
        }
    }
    return trace;
}

SimStageTrace RunSimStagesSerial(std::uint32_t enabledMask, void* context, SimStageDispatch dispatch)
{
    return RunSimStagesSerial(SimStageSpan{kSimStageOrder}, enabledMask, context, dispatch);
}

SimStageCostAccum& SimStageCosts()
{
    static SimStageCostAccum accum;
    return accum;
}

void ResetSimStageCosts()
{
    SimStageCosts() = SimStageCostAccum{};
}

} // namespace Poseidon::Sim
