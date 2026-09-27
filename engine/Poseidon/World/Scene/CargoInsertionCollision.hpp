#pragma once
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Scene/Object.hpp>

namespace Poseidon
{
// Narrow AI cargo insertion enhancement. Unsupported/incomplete coverage keeps
// the original trace, never translates a residency status into permission to
// fire. The caller retains its carrier/self hit and ahead/ground semantics.
// OFF/off-owner invokes neither queue nor modern trace. One Fire descriptor is
// used only for pivot->muzzle; a second ray must not overwrite that interest.
template<class Queue, class ModernTrace, class LegacyTrace>
void TraceCargoInsertion(bool modernOwner, CollisionBuffer& hits,
    Queue&& queue, ModernTrace&& modernTrace, LegacyTrace&& legacyTrace)
{
    using Status = Streaming::SimulationResidencyStatus;
    if (modernOwner)
    {
        const auto queued = queue();
        if ((queued == Status::Ready || queued == Status::Pending) && modernTrace(hits) == Status::Ready)
            return;
    }
    legacyTrace(hits);
}

// Invalid/noncargo/player transitions withdraw only this owner's Fire channel.
// Valid blocked attempts retain interest for a subsequent scheduled attempt;
// callers that stop entirely are covered by the bounded service heartbeat.
template<class Release>
void ReleaseCargoInsertionInterest(bool modernOwner, bool validAiInsertion, Release&& release)
{
    if (modernOwner && !validAiInsertion) release();
}
}
