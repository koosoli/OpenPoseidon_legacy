#include "Poseidon/World/Terrain/EarthStreamingMode.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

using namespace Poseidon::EarthStreaming;

GroundPatch Patch(uint64_t revision = 1, uint64_t generation = 1)
{
    // Deliberately non-planar: bilinear interpolation would give 115 at
    // (2.5,2.5), but the rendered lower triangle is exactly 107.5 there.
    return {0, 0, 10, 2, revision, generation, {100, 110, 120, 250}};
}

int main()
{
    LocationState state;
    assert(!state.QueryGround(2, 2));
    assert(state.PublishGround(Patch()));
    const auto lower = state.QueryGround(2.5f, 2.5f);
    assert(lower && lower->height == 107.5f && lower->dX == 1 && lower->dZ == 2);
    const auto upper = state.QueryGround(7.5f, 7.5f);
    assert(upper && upper->height == 182.5f && upper->dX == 13 && upper->dZ == 14);
    assert(state.QueryGround(5, 5)->height == 115);
    assert(state.QueryGround(0, 0)->height == 100);
    assert(state.QueryGround(10, 10)->height == 250);
    assert(state.QueryGround(10, 0)->height == 110);
    assert(state.QueryGround(0, 10)->height == 120);
    for (const auto [x, z] : {std::pair{-0.01f, 5.0f}, {10.01f, 5.0f}, {5.0f, -0.01f},
                             {5.0f, 10.01f}, {NAN, 0.0f}, {0.0f, INFINITY}})
        assert(!state.QueryGround(x, z));
    const auto snapshot = state.Ground();
    assert(snapshot->Contains(2, 8, 2));
    assert(!snapshot->Contains(1.99f, 8, 2));
    assert(!snapshot->Contains(5, 5, 6));
    assert(!snapshot->Contains(5, 5, -1));
    assert(!state.PublishGround(Patch())); // duplicate generation cannot replace support
    assert(state.PublishGround(Patch(1, 3)));
    assert(!state.PublishGround(Patch(1, 2)));
    auto mutablePatch = Patch(1, 4);
    assert(state.PublishGround(mutablePatch));
    mutablePatch.heights[0] = 999;
    assert(state.QueryGround(0, 0)->height == 100); // publish owns immutable data
    assert(snapshot->Sample(2.5f, 2.5f)->generation == 1);

    for (int problem = 0; problem < 9; ++problem)
    {
        auto invalid = Patch(1, 5);
        switch (problem)
        {
            case 0: invalid.heights.pop_back(); break;
            case 1: invalid.gridMetres = 0; break;
            case 2: invalid.gridMetres = INFINITY; break;
            case 3: invalid.heights[0] = NAN; break;
            case 4: invalid.samples = 258; break;
            case 5: invalid.x = INFINITY; break;
            case 6: invalid.x = 550001; break;
            case 7: invalid.gridMetres = 25601; break;
            case 8: invalid.heights[0] = 100001; break;
        }
        assert(!state.PublishGround(std::move(invalid)));
        assert(state.QueryGround(0, 0)->generation == 4);
    }

    assert(state.Set(49.6, 6.1, 4500));
    assert(!state.QueryGround(2, 2)); // geographic revision invalidates immediately
    assert(!state.PublishGround(Patch(1, 99))); // old in-flight worker result
    auto negative = Patch(2, 1);
    negative.x = -20;
    negative.z = -30;
    negative.heights = {-200, -190, -180, -170};
    assert(state.PublishGround(negative));
    assert(state.QueryGround(-15, -25)->height == -185); // no legacy sea clamp
    assert(!state.QueryGround(0, 0));
    state.ClearGround();
    assert(!state.QueryGround(-15, -25));

    // Concurrent relocation and publication use the same lock. Readers receive
    // an internally consistent immutable revision, never mixed terrain samples.
    std::atomic<bool> done = false;
    std::atomic<unsigned> reads = 0;
    std::thread reader([&] {
        do
        {
            if (const auto sample = state.QueryGround(2.5f, 2.5f))
            {
                assert(sample->height == 107.5f && sample->dX == 1 && sample->dZ == 2);
                assert(sample->revision >= 3 && sample->generation == 1);
            }
            ++reads;
        } while (!done.load());
    });
    while (!reads.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 500; ++i)
    {
        const auto oldRevision = state.Get().revision;
        assert(state.Set(49.6, 6.1, 4500));
        assert(!state.PublishGround(Patch(oldRevision, 100)));
        assert(state.PublishGround(Patch(oldRevision + 1, 1)));
    }
    done = true;
    reader.join();
    assert(reads > 0);
    // Regression coordinates from the reported buried teleport. SQF altitude
    // 0.60 is an AGL offset; the renderer/physics both use absolute vertex Y.
    const GroundPatch resident{6000, 6800, 25, 17, 1, 1, std::vector<float>(17 * 17, 300)};
    assert(resident.Valid());
    const auto support = resident.Sample(6181.81f, 6985.71f);
    assert(support && support->height == 300);
    const float teleportedY = support->height + 0.60f;
    assert(std::abs(teleportedY - 300.60f) < .001f);
    assert(support->Penetration(0.60f, 0.5f) > 299);
    assert(support->Penetration(teleportedY, 0.5f) < 0);
    for (const float bodyFootY : {300.0f, 299.95f, 299.5f})
        assert(std::abs(support->Penetration(bodyFootY, 0.5f) - (300.5f - bodyFootY)) < .001f);
    std::cout << "Earth ground: rendered triangles/slopes, bounded coverage, immutable publication, "
                 "generation ordering, geographic invalidation and concurrent queries PASS\n";
}
