#include <Poseidon/World/SimVehicleCost.hpp>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace Poseidon::SimVehCost;
int main(int argc, char** argv)
{
    assert(argc == 2);
    const bool enabled = argv[1][0] == '1';
    assert(Enabled() == enabled);
    static_assert(static_cast<unsigned>(Stage::ManBase) == 10);
    static_assert(static_cast<unsigned>(Stage::CloudletSoftSurfaces) == 11);
    static_assert(kStageCount == 16);
    Accum sample{};
    assert(!RecordCloudletStage(sample, Stage::Cloudlets, 5000000));
    assert(!RecordCloudletStage(sample, static_cast<Stage>(kStageCount), 5000000));
    const auto i = static_cast<std::size_t>(Stage::CloudletRunoffAdvance);
    assert(RecordCloudletStage(sample, Stage::CloudletRunoffAdvance, 500000));
    assert(RecordCloudletStage(sample, Stage::CloudletRunoffAdvance, 3000000));
    assert(RecordCloudletStage(sample, Stage::CloudletRunoffAdvance, 0));
    assert(sample.substageCalls[i] == 3 && std::abs(sample.stageMs[i]-3.5) < 1e-12);
    assert(sample.substageMaxMs[i] == 3 && sample.stageMs[1] == 0);
    Reset();
    {
        ScopedCloudletStage timer(Stage::CloudletRunoffAdvance);
        if (enabled) {
            const auto start = NowNs();
            while (NowNs() - start < 1000) {} // ensure one real measurable clock interval
        }
        timer.Stop();
        timer.Stop(); // destructor also stops; a single call must remain a single call
    }
    assert(Costs().substageCalls[i] == (enabled ? 1U : 0U));
    if (enabled) assert(Costs().stageMs[i] > 0 && Costs().substageMaxMs[i] == Costs().stageMs[i]);
    else assert(Costs().stageMs[i] == 0 && Costs().substageMaxMs[i] == 0);
    Reset();
    assert(Costs().ticks == 0);
    for (std::size_t n=0; n<kStageCount; ++n)
        assert(Costs().substageCalls[n] == 0 && Costs().substageMaxMs[n] == 0 && Costs().stageMs[n] == 0);
    std::cout << "PASS cloudlet cost native clock/gate/count/max/reset (enabled=" << enabled << ")\n";
}
