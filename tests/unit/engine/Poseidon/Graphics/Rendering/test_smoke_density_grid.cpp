#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Effects/SmokeDensityGrid.hpp>
#include <vector>

TEST_CASE("Smoke cell totals match the neighbour walk including self exclusion", "[smoke][density]")
{
    Poseidon::SmokeDensityGrid grid;
    std::vector<std::pair<uint64_t, float>> particles;
    for (int i = 0; i < 2000; ++i)
        particles.emplace_back(i % 7, (i % 13) * 0.01f);
    grid.Clear(particles.size());
    for (const auto& [key, density] : particles)
        grid.Add(key, density);
    for (std::size_t self : {0u, 21u, 1999u})
        for (uint64_t key = 0; key < 9; ++key)
        {
            float reference = 0;
            for (std::size_t i = 0; i < particles.size(); ++i)
                if (i != self && particles[i].first == key)
                    reference += particles[i].second;
            CHECK(grid.SampleExcluding(key, particles[self].first, particles[self].second) ==
                  Catch::Approx(reference).margin(0.0001f));
        }
    grid.Clear(1);
    grid.Add(3, 0.7f);
    CHECK(grid.SampleExcluding(3, 3, 0.7f) == 0.0f);
    CHECK(grid.SampleExcluding(0, 3, 0.7f) == 0.0f);
}
