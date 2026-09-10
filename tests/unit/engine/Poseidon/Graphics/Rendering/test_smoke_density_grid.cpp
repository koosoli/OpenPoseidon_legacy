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
    mutable int probes = 0;
    bool roofPresent = true;
    bool ProbeSegment(Vector3Par from, Vector3Par to, Vector3& hit) const override
    {
        ++probes;
        if (!roofPresent || from.Y() >= 3.0f || to.Y() <= 3.0f || std::abs(from.X()) > 2.0f)
            return false;
        hit = Vector3(from.X(), 3.0f, from.Z());
        return true;
    }
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

TEST_CASE("Smoke keeps a real roof contact after a tangential sweep misses", "[smoke][render-bounds]")
{
    SmokeRoomQuery query;
    Poseidon::SmokeParticle particle;
    particle.position = Vector3(0, 2.4f, 0);
    particle.planeNormal = Vector3(0, -1, 0);
    particle.planePadding = 0.2f;
    particle.planeOffset = -2.4f;
    CHECK_FALSE(Poseidon::SmokeContactPlaneStillPresent(particle, 0.4f, query));
    CHECK(query.probes == 0);
    particle.planeValid = true;
    CHECK(Poseidon::SmokeContactPlaneStillPresent(particle, 0.4f, query));
    CHECK(query.probes == 1);
    particle.position[0] = 3;
    CHECK_FALSE(Poseidon::SmokeContactPlaneStillPresent(particle, 0.4f, query));
    particle.position[0] = 0;
    query.roofPresent = false;
    CHECK_FALSE(Poseidon::SmokeContactPlaneStillPresent(particle, 0.4f, query));
    query.roofPresent = true;
    particle.position[1] = 1;
    const int before = query.probes;
    CHECK_FALSE(Poseidon::SmokeContactPlaneStillPresent(particle, 0.4f, query));
    CHECK(query.probes == before);
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

TEST_CASE("Smoke optical density dilutes through expansion independently of shadow controls", "[smoke][density]")
{
    Poseidon::SmokeParams params;
    params.fadeIn = 0;
    params.fadeOut = 0.2f;
    params.startRadius = 1;
    params.endRadius = 5;
    params.opacity = 1;
    params.densityFalloff = 2;
    Poseidon::SmokeParticle p;
    p.lifetime = 10;
    p.age = 0;
    p.sizeJitter = p.alphaJitter = 1;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) == 1);
    p.age = 5;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) == Catch::Approx(1.0f/9.0f));
    params.groundShadow = 0;
    p.roomId = 1;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) == Catch::Approx(1.0f/9.0f));
    p.age = 9;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) < 0.03f);
    p.age = 10;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) == 0);
    params.balloons = true;
    p.age = 5;
    CHECK(Poseidon::SmokeParticleOpacity(p, params) == params.balloonOpacity);
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

TEST_CASE("Cached smoke sort depths preserve the per-camera draw order", "[smoke][draw-order]")
{
    std::vector<Poseidon::SmokeParticle> particles(4096);
    for (int i = 0; i < static_cast<int>(particles.size()); ++i)
        particles[i].position = Vector3((i * 31) % 101, (i * 7) % 13, (i * 43) % 97);
    std::vector<float> depths;
    std::vector<int> order;
    for (float heading : {0.0f, 0.7f, 2.4f})
    {
        Matrix4 view;
        view.SetRotationY(heading);
        view.SetPosition(Vector3(7764, 23, 4408));
        std::vector<int> reference(particles.size());
        for (int i = 0; i < static_cast<int>(reference.size()); ++i)
            reference[i] = i;
        std::sort(reference.begin(), reference.end(), [&](int a, int b)
                  { return (view * particles[a].position).Z() > (view * particles[b].position).Z(); });
        Poseidon::BuildSmokeDrawOrder(particles, view, depths, order);
        REQUIRE(order == reference);
        REQUIRE(depths.size() == particles.size());
    }
    particles.clear();
    Poseidon::BuildSmokeDrawOrder(particles, M4Identity, depths, order);
    REQUIRE(order.empty());
    REQUIRE(depths.empty());
}
