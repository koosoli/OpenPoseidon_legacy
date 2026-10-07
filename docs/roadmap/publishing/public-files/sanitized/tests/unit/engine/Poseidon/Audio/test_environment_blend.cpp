// test_environment_blend.cpp -- the world-state -> reverb path, roadmap 1.4 item 2.
//
// Both halves are pure logic and are tested against the PRODUCTION code, not a copy of
// it: Audio::ClassifyEnvironment is the function World::PerformSound calls, and
// Audio::EnvironmentBlender is the object SoundSystemOAL::TickEnvironmentBlend drives.
// That matters here -- the pre-existing test_eax_efx.cpp pins the SE*->preset mapping
// against a local re-implementation, which cannot notice the engine drifting away from
// it.
//
// See design notes

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/Audio/EnvironmentBlend.hpp>
#include <Poseidon/Audio/EnvironmentClassify.hpp>

#include <cmath>
#include <vector>

using namespace Poseidon;
using namespace Poseidon::Audio;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Classifier
// ---------------------------------------------------------------------------

TEST_CASE("EnvClassify: bare terrain with nothing on it is Plain", "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings s;
    const SoundEnvironment env = ClassifyEnvironment(s);
    CHECK(env.type == SEPlain);
    CHECK(env.size == Approx(75.0f));
    CHECK(env.density == Approx(0.5f));
}

TEST_CASE("EnvClassify: Plain tightens as soft objects accumulate", "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings s;
    s.objects = 2;
    const SoundEnvironment env = ClassifyEnvironment(s);
    CHECK(env.type == SEPlain);
    CHECK(env.size == Approx(45.0f)); // 75 - 2 * 15
}

TEST_CASE("EnvClassify: forest geography wins over altitude", "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings s;
    s.forest = true;
    s.surfaceYAboveWater = 900.0f; // would be Mountains on its own
    const SoundEnvironment env = ClassifyEnvironment(s);
    CHECK(env.type == SEForest);
    CHECK(env.size == Approx(38.0f));
}

TEST_CASE("EnvClassify: hard objects give City, denser and tighter with more of them",
          "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings one;
    one.hardObjects = 1;
    EnvironmentSurroundings three;
    three.hardObjects = 3;

    const SoundEnvironment a = ClassifyEnvironment(one);
    const SoundEnvironment b = ClassifyEnvironment(three);
    CHECK(a.type == SECity);
    CHECK(b.type == SECity);
    // Both directions matter: more buildings = smaller space, more reflections.
    CHECK(b.size < a.size);
    CHECK(b.density > a.density);
    CHECK(a.size == Approx(45.0f));
    CHECK(b.size == Approx(15.0f));
    CHECK(b.density == Approx(1.0f));
}

TEST_CASE("EnvClassify: Mountains is an altitude threshold, and it is only that",
          "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings low;
    low.surfaceYAboveWater = kMountainsSurfaceY; // exactly at the threshold is NOT above it
    CHECK(ClassifyEnvironment(low).type == SEPlain);

    EnvironmentSurroundings high;
    high.surfaceYAboveWater = kMountainsSurfaceY + 1.0f;
    CHECK(ClassifyEnvironment(high).type == SEMountains);

    // The size band saturates at both ends -- a 300 m hill and a 3000 m peak sound the
    // same, which is a limitation of the model and is pinned here so it stays visible.
    EnvironmentSurroundings peak;
    peak.surfaceYAboveWater = 3000.0f;
    EnvironmentSurroundings hill;
    hill.surfaceYAboveWater = 300.0f;
    CHECK(ClassifyEnvironment(peak).size == Approx(100.0f));
    CHECK(ClassifyEnvironment(hill).size == Approx(100.0f));
}

TEST_CASE("EnvClassify: being indoors beats every outdoor cue", "[Audio][EAX][EnvClassify]")
{
    EnvironmentSurroundings s;
    s.insideRoom = true;
    s.roomSize = 5.5f;
    s.forest = true;
    s.hardObjects = 3;
    s.surfaceYAboveWater = 900.0f;

    const SoundEnvironment env = ClassifyEnvironment(s);
    CHECK(env.type == SERoom);
    CHECK(env.size == Approx(5.5f)); // the caller's room scale is passed through verbatim
}

// ---------------------------------------------------------------------------
// Blender
// ---------------------------------------------------------------------------

namespace
{
// Two stand-in parameter sets. Values are arbitrary but distinct in every component,
// which is what makes "did every parameter move" meaningful.
const std::vector<float> kDry{0.10f, 0.20f, 1.00f, 5000.0f, -0.50f};
const std::vector<float> kWet{0.90f, 0.75f, 0.25f, 250.0f, 0.50f};
const std::vector<float> kThird{0.50f, 0.40f, 0.60f, 1000.0f, 0.10f};

void Set(EnvironmentBlender& b, const std::vector<float>& v)
{
    b.SetTarget(v.data(), static_cast<int>(v.size()));
}
void Snap(EnvironmentBlender& b, const std::vector<float>& v)
{
    b.Snap(v.data(), static_cast<int>(v.size()));
}
} // namespace

TEST_CASE("EnvBlend: the first application snaps -- there is nothing to blend from",
          "[Audio][EAX][EnvBlend]")
{
    // SetTarget before anything is valid must snap, not fade in from a zeroed set --
    // fading up from silence would itself be a wrong sound at mission start.
    EnvironmentBlender b;
    CHECK_FALSE(b.Valid());
    const bool blended = b.SetTarget(kDry.data(), static_cast<int>(kDry.size()));
    CHECK_FALSE(blended);
    CHECK(b.Valid());
    CHECK(b.Settled());
    for (size_t i = 0; i < kDry.size(); ++i)
    {
        CHECK(b.Current()[i] == Approx(kDry[i]));
    }
}

TEST_CASE("EnvBlend: a settled blender does no work and needs no device write",
          "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    CHECK(b.Settled());
    CHECK_FALSE(b.Tick(1.0f / 60.0f));
}

TEST_CASE("EnvBlend: every parameter approaches its target monotonically", "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);
    REQUIRE_FALSE(b.Settled());

    std::vector<float> prev(b.Current(), b.Current() + b.Count());
    int ticks = 0;
    while (!b.Settled() && ticks < 10000)
    {
        b.Tick(1.0f / 60.0f);
        ++ticks;
        for (int i = 0; i < b.Count(); ++i)
        {
            const float cur = b.Current()[i];
            const float tgt = kWet[static_cast<size_t>(i)];
            // Monotonic: the gap to the target never grows...
            CHECK(std::fabs(tgt - cur) <= std::fabs(tgt - prev[static_cast<size_t>(i)]) + 1e-6f);
            // ...and the ease never overshoots past it.
            if (kDry[static_cast<size_t>(i)] < tgt)
            {
                CHECK(cur <= tgt + 1e-6f);
                CHECK(cur >= prev[static_cast<size_t>(i)] - 1e-6f);
            }
            else
            {
                CHECK(cur >= tgt - 1e-6f);
                CHECK(cur <= prev[static_cast<size_t>(i)] + 1e-6f);
            }
            prev[static_cast<size_t>(i)] = cur;
        }
    }
    CHECK(b.Settled());
    for (size_t i = 0; i < kWet.size(); ++i)
    {
        CHECK(b.Current()[i] == Approx(kWet[i]));
    }
}

TEST_CASE("EnvBlend: it settles in about three time constants", "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);

    const float step = 1.0f / 60.0f;
    float elapsed = 0.0f;
    while (!b.Settled() && elapsed < 30.0f)
    {
        b.Tick(step);
        elapsed += step;
    }
    REQUIRE(b.Settled());
    // Not a hard cut (the thing being removed) and not a drone. The upper bound is
    // generous because the settle epsilon is relative and the slowest parameter is the
    // one with the smallest magnitude.
    CHECK(elapsed > kEnvironmentBlendTauSeconds);
    CHECK(elapsed < 12.0f * kEnvironmentBlendTauSeconds);
}

TEST_CASE("EnvBlend: a change mid-blend retargets, it does not restart", "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);

    // Get part-way there.
    for (int i = 0; i < 12; ++i)
    {
        b.Tick(1.0f / 60.0f);
    }
    REQUIRE_FALSE(b.Settled());
    const std::vector<float> midway(b.Current(), b.Current() + b.Count());
    // Genuinely part-way: away from both ends.
    CHECK(midway[0] > kDry[0] + 1e-3f);
    CHECK(midway[0] < kWet[0] - 1e-3f);

    // Now change environment again. The applied set must be exactly where the ear
    // already is -- no jump back to kDry, no jump forward to kThird.
    const bool blended = b.SetTarget(kThird.data(), static_cast<int>(kThird.size()));
    CHECK(blended);
    CHECK_FALSE(b.Settled());
    for (int i = 0; i < b.Count(); ++i)
    {
        CHECK(b.Current()[i] == Approx(midway[static_cast<size_t>(i)]));
        CHECK(b.Target()[i] == Approx(kThird[static_cast<size_t>(i)]));
    }

    // And it converges on the NEW target, not the abandoned one.
    for (int i = 0; i < 6000 && !b.Settled(); ++i)
    {
        b.Tick(1.0f / 60.0f);
    }
    CHECK(b.Settled());
    for (size_t i = 0; i < kThird.size(); ++i)
    {
        CHECK(b.Current()[i] == Approx(kThird[i]));
    }
}

TEST_CASE("EnvBlend: retargeting back to where it came from still does not jump",
          "[Audio][EAX][EnvBlend]")
{
    // The doorway case: step in, step straight back out. The reverb must walk back from
    // wherever it got to, not snap to the environment it was already heading toward.
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);
    for (int i = 0; i < 10; ++i)
    {
        b.Tick(1.0f / 60.0f);
    }
    const float midway = b.Current()[0];
    Set(b, kDry);
    CHECK(b.Current()[0] == Approx(midway));
    b.Tick(1.0f / 60.0f);
    CHECK(b.Current()[0] < midway); // heading back down toward kDry
    CHECK(b.Current()[0] > kDry[0]);
}

TEST_CASE("EnvBlend: a stalled frame cannot teleport the blend", "[Audio][EAX][EnvBlend]")
{
    // Without the dt cap, one long frame after a loading hitch would land the reverb on
    // its target -- reintroducing the hard cut this whole path exists to remove.
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);
    b.Tick(30.0f);
    CHECK_FALSE(b.Settled());
    CHECK(b.Current()[0] < kWet[0] - 1e-3f);

    // The cap is exactly kEnvironmentBlendMaxStepSeconds: a longer frame and a
    // just-capped frame must move the blend by the same amount.
    EnvironmentBlender c;
    Snap(c, kDry);
    Set(c, kWet);
    c.Tick(kEnvironmentBlendMaxStepSeconds);
    CHECK(c.Current()[0] == Approx(b.Current()[0]));
}

TEST_CASE("EnvBlend: a zero-length frame moves nothing and does not divide",
          "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    Set(b, kWet);
    b.Tick(0.0f);
    CHECK(b.Current()[0] == Approx(kDry[0]));
    CHECK(EnvironmentBlendFactor(0.0f) == Approx(0.0f));
    CHECK(std::isfinite(EnvironmentBlendFactor(0.0f)));
}

TEST_CASE("EnvBlend: the blend factor is in [0,1), which is why it cannot overshoot",
          "[Audio][EAX][EnvBlend]")
{
    for (const float dt : {0.0f, 0.001f, 1.0f / 240.0f, 1.0f / 60.0f, 0.1f, 0.25f, 5.0f, 1000.0f})
    {
        const float k = EnvironmentBlendFactor(dt);
        CHECK(k >= 0.0f);
        CHECK(k < 1.0f);
    }
}

TEST_CASE("EnvBlend: invalidation forgets the applied set", "[Audio][EAX][EnvBlend]")
{
    // DeinitEFX does this. Blending FROM a parameter set that a destroyed effect object
    // last held would fade the new device in from a sound nobody heard on it.
    EnvironmentBlender b;
    Snap(b, kDry);
    b.Invalidate();
    CHECK_FALSE(b.Valid());
    CHECK(b.Settled());
    CHECK_FALSE(b.Tick(1.0f / 60.0f));
    // The next application therefore snaps, as the first one always must.
    CHECK_FALSE(b.SetTarget(kWet.data(), static_cast<int>(kWet.size())));
    CHECK(b.Settled());
}

TEST_CASE("EnvBlend: a differently shaped parameter vector snaps rather than blending",
          "[Audio][EAX][EnvBlend]")
{
    EnvironmentBlender b;
    Snap(b, kDry);
    const std::vector<float> shorter{0.5f, 0.5f};
    CHECK_FALSE(b.SetTarget(shorter.data(), 2));
    CHECK(b.Count() == 2);
    CHECK(b.Settled());
}
