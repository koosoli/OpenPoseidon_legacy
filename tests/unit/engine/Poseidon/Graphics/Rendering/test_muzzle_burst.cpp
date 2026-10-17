#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Effects/MuzzleBurst.hpp>
#include <cmath>
#include <vector>

using namespace Poseidon::render;

namespace
{
MuzzleBurstShot TestShot(std::uint32_t seed, MuzzleBurstVec forward = {0, 0, 1})
{
    MuzzleBurstShot shot;
    shot.muzzle = {100, 20, 300};
    shot.forward = Normalized(forward);
    shot.birthMs = 1000;
    shot.seed = seed;
    return shot;
}
std::vector<MuzzleParticle> At(const MuzzleBurstShot& shot, std::int64_t nowMs)
{
    std::vector<MuzzleParticle> out;
    const int n = MuzzleBurstParticles(shot, nowMs, 9.81f, [&](const MuzzleParticle& p) { out.push_back(p); });
    REQUIRE(n == int(out.size()));
    return out;
}
float Length(MuzzleBurstVec v) { return std::sqrt(Dot(v, v)); }
MuzzleBurstVec Minus(MuzzleBurstVec a, MuzzleBurstVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
}

TEST_CASE("Muzzle burst is a pure, bounded function of shot and age", "[muzzle-burst]")
{
    const auto shot = TestShot(7);
    REQUIRE(At(shot, 999).empty());                                 // before the shot
    REQUIRE(At(shot, 1000 + MuzzleBurstLifetimeMs).empty());       // after the longest spark
    std::size_t total = 0;
    for (std::int64_t t = 1000; t < 1000 + MuzzleBurstLifetimeMs; t += 3)
    {
        const auto a = At(shot, t);
        const auto b = At(shot, t);
        REQUIRE(a.size() == b.size());
        REQUIRE(int(a.size()) <= MuzzleBurstMaxParticles);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            REQUIRE(a[i].head.x == b[i].head.x);
            REQUIRE(std::isfinite(a[i].radiance));
            REQUIRE(a[i].radiance >= 0.0f);
            REQUIRE(a[i].radius > 0.0f);
            REQUIRE(a[i].r >= a[i].g);
            REQUIRE(a[i].g >= a[i].b); // white-yellow -> orange -> red only
        }
        total += a.size();
    }
    REQUIRE(total > 0);
}

TEST_CASE("Muzzle burst gas stays near the muzzle; plume goes forward, jets go sideways", "[muzzle-burst]")
{
    for (std::uint32_t seed = 1; seed < 40; ++seed)
    {
        const auto shot = TestShot(seed, {0.3f, 0.1f, 1.0f});
        const auto fresh = At(shot, 1001);
        int jets = 0, plume = 0;
        for (const auto& p : fresh)
        {
            if (p.kind == MuzzleParticleKind::Spark) continue;
            REQUIRE(Length(Minus(p.head, shot.muzzle)) < 0.05f);
            jets += p.kind == MuzzleParticleKind::Jet;
            plume += p.kind == MuzzleParticleKind::Plume;
        }
        REQUIRE(jets >= 8);
        REQUIRE(plume >= 4);
        for (const auto& p : At(shot, 1010))
        {
            const auto offset = Minus(p.head, shot.muzzle);
            const float along = Dot(offset, shot.forward);
            const float across = Length(Minus(offset, shot.forward * along));
            if (p.kind == MuzzleParticleKind::Jet)
            {
                REQUIRE(across > along * 0.8f); // out of the side slots
                REQUIRE(Length(offset) < 0.35f); // stops within a hand's width or so
            }
            if (p.kind == MuzzleParticleKind::Plume)
            {
                REQUIRE(along > 0.0f);
                REQUIRE(across < along); // a forward cone
                REQUIRE(along < 0.8f);
            }
        }
    }
}

TEST_CASE("Muzzle burst sparks fly fast, fall, cool, and vary per shot", "[muzzle-burst]")
{
    int shotsWithSparks = 0;
    for (std::uint32_t seed = 1; seed < 200; ++seed)
    {
        const auto shot = TestShot(seed);
        bool any = false;
        for (const auto& p : At(shot, 1060))
        {
            if (p.kind != MuzzleParticleKind::Spark) continue;
            any = true;
            const auto offset = Minus(p.head, shot.muzzle);
            REQUIRE(Dot(offset, shot.forward) > 0.5f);     // well downrange after 60 ms
            REQUIRE(Length(Minus(p.head, p.tail)) > 0.1f); // motion-blurred into a streak
            REQUIRE(p.r == 1.0f);
            REQUIRE(p.b < 0.4f);
        }
        shotsWithSparks += any;
    }
    REQUIRE(shotsWithSparks > 60);  // most shots throw some
    REQUIRE(shotsWithSparks < 199); // not every shot
    const auto a = At(TestShot(3), 1010);
    const auto b = At(TestShot(4), 1010);
    bool differs = a.size() != b.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i) differs = a[i].head.x != b[i].head.x;
    REQUIRE(differs);
}

TEST_CASE("Muzzle burst ring keeps the newest shots and only live ones", "[muzzle-burst]")
{
    MuzzleBurstRing ring;
    for (int i = 0; i < MuzzleBurstRing::Capacity + 10; ++i)
    {
        auto shot = TestShot(std::uint32_t(i));
        shot.birthMs = 1000 + i;
        ring.Record(shot);
    }
    int live = 0;
    std::int64_t oldest = 1 << 30;
    ring.ForEachLive(1000 + MuzzleBurstRing::Capacity + 9, [&](const MuzzleBurstShot& s) {
        ++live;
        oldest = std::min(oldest, s.birthMs);
    });
    REQUIRE(live == MuzzleBurstRing::Capacity);
    REQUIRE(oldest == 1010); // the ten oldest were dropped
    int expired = 0;
    ring.ForEachLive(1000 + MuzzleBurstRing::Capacity + 10 + MuzzleBurstLifetimeMs, [&](const MuzzleBurstShot&) { ++expired; });
    REQUIRE(expired == 0);
}
