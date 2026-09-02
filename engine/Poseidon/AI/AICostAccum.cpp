// PERF-014. See AICostAccum.hpp for what this measures and why the buckets nest.

#include <Poseidon/AI/AICostAccum.hpp>

#include <chrono>

namespace Poseidon::AICost
{

namespace
{
// Nanoseconds, not a float of milliseconds: the leaf buckets (`AIUnit::Think`) are called
// hundreds of times a tick and each call can be a few microseconds. Accumulating those into
// a float would lose the small ones against a total that grows all window.
std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

const char* BucketName(Bucket bucket)
{
    switch (bucket)
    {
        case Bucket::Radio:
            return "radio";
        case Bucket::Exposure:
            return "exposure";
        case Bucket::Map:
            return "map";
        case Bucket::Guarding:
            return "guarding";
        case Bucket::Support:
            return "support";
        case Bucket::GroupScan:
            return "groupScan";
        case Bucket::GroupThink:
            return "groupThink";
        case Bucket::GroupExpensive:
            return "groupExpensive";
        case Bucket::SubThink:
            return "subThink";
        case Bucket::UnitThink:
            return "unitThink";
        case Bucket::EndMission:
            return "endMission";
        case Bucket::GroupThinkEarly:
            return "groupThinkEarly";
        case Bucket::GroupThinkFull:
            // Missing until PERF-019; the case fell through to "?".
            return "groupThinkFull";
        case Bucket::GroupFlee:
            return "groupFlee";
        case Bucket::GroupFsm:
            return "groupFsm";
        case Bucket::GroupJoin:
            return "groupJoin";
        case Bucket::GroupTrack:
            return "groupTrack";
        case Bucket::UnitThinkEarly:
            return "unitThinkEarly";
        case Bucket::UnitThinkFull:
            return "unitThinkFull";
        case Bucket::UnitAttack:
            return "unitAttack";
        case Bucket::UnitWatch:
            return "unitWatch";
        case Bucket::UnitExpensive:
            return "unitExpensive";
        case Bucket::UnitGetInOut:
            return "unitGetInOut";
        case Bucket::UnitStrat:
            return "unitStrat";
        case Bucket::UnitOperTarget:
            return "unitOperTarget";
        case Bucket::UnitOperPlan:
            return "unitOperPlan";
    }
    return "?";
}

CostAccum& AICosts()
{
    static CostAccum accum;
    return accum;
}

void ResetAICosts()
{
    AICosts() = CostAccum{};
}

ScopedCost::ScopedCost(Bucket bucket) : _bucket(bucket), _startNs(NowNs()) {}

ScopedCost::~ScopedCost()
{
    AICosts().ms[static_cast<std::size_t>(_bucket)] += static_cast<double>(NowNs() - _startNs) * 1e-6;
}

} // namespace Poseidon::AICost
