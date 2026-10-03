#include <Poseidon/World/Weather/RainParticlePolicy.hpp>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using namespace Poseidon;

struct Population
{
    std::vector<std::array<float, 3>> drops;
    unsigned groundHits = 0;
    unsigned volumeCulls = 0;
    uint64_t simulatedDrops = 0;
    float spawnDebt = 0;
    void Step(float cameraHeight, bool bounded)
    {
        constexpr float dt = 1.0f / 60.0f;
        if (bounded)
        {
            volumeCulls += static_cast<unsigned>(std::erase_if(drops, [&](const auto& p) {
                return !RainParticleInViewVolume(p, {0, cameraHeight, 0}, 20, 18);
            }));
            const int limit = RainParticlePopulationLimit(1800, 4000, 1);
            if (drops.size() > static_cast<size_t>(limit)) drops.resize(limit);
            spawnDebt = std::min(spawnDebt + 900 * dt, float(limit - int(drops.size())));
        }
        else spawnDebt += 900 * dt;
        while (spawnDebt >= 1)
        {
            --spawnDebt;
            if (drops.size() >= 4000) {spawnDebt = 0; break;}
            // Fixed ordinary birth height separates altitude from jitter/RNG.
            drops.push_back({0, cameraHeight + 12.15f, 0});
        }
        simulatedDrops += drops.size();
        for (auto& p : drops) p[1] -= 9 * dt;
        groundHits += static_cast<unsigned>(std::erase_if(drops, [](const auto& p) {return p[1] <= 0;}));
    }
};

int main()
{
    const std::array<float, 3> camera{5018.51f, 117.58f, 4087.66f};
    assert(RainParticleInViewVolume(camera, camera, 20, 18));
    assert(RainParticleInViewVolume({camera[0]+20, camera[1], camera[2]}, camera, 20, 18));
    assert(!RainParticleInViewVolume({camera[0]+20.25f, camera[1], camera[2]}, camera, 20, 18));
    assert(RainParticleInViewVolume({camera[0], camera[1]-18, camera[2]}, camera, 20, 18));
    assert(!RainParticleInViewVolume({camera[0], camera[1]-18.25f, camera[2]}, camera, 20, 18));
    assert(!RainParticleInViewVolume({camera[0], camera[1]+18.25f, camera[2]}, camera, 20, 18));
    assert(!RainParticleInViewVolume(camera, {camera[0]+45, camera[1], camera[2]}, 20, 18));
    assert(!RainParticleInViewVolume(camera, camera, 0, 18));
    assert(!RainParticleInViewVolume(camera, camera, 20, -1));
    for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        assert(!RainParticleInViewVolume({invalid, 0, 0}, camera, 20, 18));
        assert(!RainParticleInViewVolume(camera, {0, invalid, 0}, 20, 18));
        assert(!RainParticleInViewVolume(camera, camera, invalid, 18));
        assert(RainParticlePopulationLimit(1800, 4000, invalid) == 0);
    }
    assert(RainParticlePopulationLimit(1800, 4000, 1) == 1800);
    assert(RainParticlePopulationLimit(1800, 4000, .25f) == 450);
    assert(RainParticlePopulationLimit(1800, 4000, 0) == 0);
    assert(RainParticlePopulationLimit(1800, 4000, -1) == 0);
    assert(RainParticlePopulationLimit(8000, 4000, 1) == 4000);
    assert(RainParticlePopulationLimit(1800, 0, 1) == 0);
    assert(RainParticlePopulationLimit(1800, 4000, 5) == 1800);
    int previous = 0;
    for (int i = 0; i <= 10000; ++i)
    {
        const int n = RainParticlePopulationLimit(1800, 4000, float(i)/10000);
        assert(n >= previous && n <= 1800);
        previous = n;
    }

    Population groundBefore, groundAfter, airBefore, airAfter;
    for (int frame = 0; frame < 60 * 30; ++frame)
    {
        groundBefore.Step(1.7f, false); groundAfter.Step(1.7f, true);
        airBefore.Step(117.58f, false); airAfter.Step(117.58f, true);
        assert(airAfter.drops.size() <= 1800);
    }
    // At eye height ordinary impacts occur inside the camera volume unchanged.
    assert(groundAfter.groundHits == groundBefore.groundHits && groundAfter.volumeCulls == 0);
    assert(groundAfter.simulatedDrops == groundBefore.simulatedDrops);
    assert(airBefore.drops.size() > 3000 && airAfter.drops.size() <= 1800);
    assert(airAfter.volumeCulls > 0 && airAfter.groundHits == 0);
    assert(airAfter.simulatedDrops * 100 < airBefore.simulatedDrops * 55);
    Population higher;
    for (int frame = 0; frame < 60 * 30; ++frame) higher.Step(1000, true);
    assert(higher.drops.size() == airAfter.drops.size());
    assert(higher.groundHits == 0);
    std::cout << "PASS camera-local rain bounds, altitude-independent cap, dry/invalid controls; "
              << "30s synthetic drop-update work " << airBefore.simulatedDrops << " -> "
              << airAfter.simulatedDrops << " (not a runtime benchmark)\n";
}
