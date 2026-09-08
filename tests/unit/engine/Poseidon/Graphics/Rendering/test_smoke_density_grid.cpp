#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Effects/SmokeDensityGrid.hpp>
#include <Poseidon/World/Effects/SmokeVolume.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <vector>

namespace
{
class SmokeRoomQuery final : public Poseidon::ISmokeWorldQuery
{
  public:
    float clearance = 0.4f;
    mutable int sweeps = 0;
    Poseidon::SmokeSurfaceHit SweepSphere(Vector3Par, Vector3Par, float) const override
    {
        ++sweeps;
        return {};
    }
    float GroundHeight(float, float) const override { return 0; }
    float FloorHeight(float, float) const override { return 0; }
    float FloorHeightBelow(Vector3Par) const override { return 0; }
    bool IsSheltered(Vector3Par, float) const override { return true; }
    float IndoorClearanceAt(Vector3Par, int, float limit) const override { return std::min(clearance, limit); }
};
}

TEST_CASE("Smoke rendering respects walls and roofs without more collision sweeps", "[smoke][render-bounds]")
{
    Poseidon::SmokeParams params;
    params.startRadius = 1;
    params.endRadius = 5;
    params.contain = true;
    params.collide = true;
    Poseidon::SmokeParticle particle;
    particle.age = particle.lifetime;
    SmokeRoomQuery query;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == 5);
    particle.roomId = 1;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == Catch::Approx(0.375f));
    query.clearance = 0;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == 0);
    particle.roomId = -1;
    particle.planeValid = true;
    particle.planeNormal = Vector3(0, -1, 0);
    particle.position = Vector3(0, 2, 0);
    particle.planeOffset = -2;
    particle.planePadding = 0.25f;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == Catch::Approx(0.225f));
    particle.age = 0;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == Catch::Approx(0.225f));
    particle.planeNormal = Vector3(-1, 0, 0);
    particle.position = Vector3(2, 0, 0);
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == Catch::Approx(0.225f));
    params.contain = false;
    CHECK(Poseidon::SmokeParticleRenderRadius(particle, params, query) == 1);
    CHECK(query.sweeps == 0);
}

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
