// PERF-023 -- see SimVehicleCost.hpp.
#include "SimVehicleCost.hpp"

#include <chrono>

namespace Poseidon::SimVehCost
{
std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::Importance: return "importance";
    case Stage::Cloudlets: return "cloudlets";
    case Stage::Slow: return "slow";
    case Stage::Fast: return "fast";
    case Stage::Buildings: return "buildings";
    case Stage::Attached: return "attached";
    }
    return "?";
}

const char* KindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Man: return "man";
    case Kind::Tank: return "tank";
    case Kind::Car: return "car";
    case Kind::Air: return "air";
    case Kind::Ship: return "ship";
    case Kind::Static: return "static";
    case Kind::Other: return "other";
    }
    return "?";
}

static Accum g_accum;

Accum& Costs()
{
    return g_accum;
}

void Reset()
{
    g_accum = Accum{};
}

ScopedStage::ScopedStage(Stage stage) : _stage(stage), _startNs(NowNs()) {}

ScopedStage::~ScopedStage()
{
    g_accum.stageMs[static_cast<std::size_t>(_stage)] += static_cast<double>(NowNs() - _startNs) * 1.0e-6;
}

void AddKind(Kind kind, std::uint64_t ns)
{
    const std::size_t i = static_cast<std::size_t>(kind);
    g_accum.kindMs[i] += static_cast<double>(ns) * 1.0e-6;
    g_accum.kindCalls[i]++;
}
} // namespace Poseidon::SimVehCost
