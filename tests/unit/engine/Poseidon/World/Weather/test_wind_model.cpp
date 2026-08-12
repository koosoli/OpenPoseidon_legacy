#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Weather/WindModel.hpp>

#include <cmath>

using namespace Poseidon;

namespace
{
constexpr int64_t kMinute = 60 * 1000;

float MeanSpeedAt(float overcast, int64_t ms)
{
    return EvaluateWind(WindConditions{}, overcast, ms).meanSpeed;
}
} // namespace

TEST_CASE("Wind is a closed form: the same inputs always give the same vector", "[World][Weather][Wind]")
{
    const WindConditions conditions{};
    const WindSample a = EvaluateWind(conditions, 0.42f, 1234567);
    const WindSample b = EvaluateWind(conditions, 0.42f, 1234567);

    // Bit equality, not approximate: this is the multiplayer determinism claim.
    // Two machines evaluating the same clock and overcast must not merely agree
    // closely, they must agree exactly, or smoke drifts differently per client.
    REQUIRE(a.velocityX == b.velocityX);
    REQUIRE(a.velocityZ == b.velocityZ);
    REQUIRE(a.meanSpeed == b.meanSpeed);
    REQUIRE(a.gustFraction == b.gustFraction);

    // Evaluating out of order must not matter either — there is no integrator.
    const WindSample late = EvaluateWind(conditions, 0.42f, 90 * kMinute);
    (void)EvaluateWind(conditions, 0.9f, 5 * kMinute);
    REQUIRE(EvaluateWind(conditions, 0.42f, 90 * kMinute).velocityX == late.velocityX);
}

TEST_CASE("A different seed gives a different wind, deterministically", "[World][Weather][Wind]")
{
    WindConditions a{};
    WindConditions b{};
    b.seed = 20260812u;

    const WindSample sa = EvaluateWind(a, 0.6f, 7 * kMinute);
    const WindSample sb = EvaluateWind(b, 0.6f, 7 * kMinute);
    REQUIRE(sa.directionRad != sb.directionRad);
    REQUIRE(EvaluateWind(b, 0.6f, 7 * kMinute).directionRad == sb.directionRad);
}

TEST_CASE("Wind speed stays inside the envelope the legacy model occupied", "[World][Weather][Wind]")
{
    // The legacy Weather random walk produced roughly 0..1.4 m/s at overcast 0 and
    // peaked around 13 m/s at overcast 1 (walk + overcast bias + gust). Anything
    // outside that envelope would change how classic missions look and feel.
    for (int step = 0; step < 4000; ++step)
    {
        const int64_t ms = static_cast<int64_t>(step) * 700;
        const WindSample calm = EvaluateWind(WindConditions{}, 0.0f, ms);
        const WindSample storm = EvaluateWind(WindConditions{}, 1.0f, ms);

        REQUIRE(std::isfinite(calm.speed));
        REQUIRE(calm.speed >= 0.0f);
        REQUIRE(calm.speed < 3.0f);

        REQUIRE(std::isfinite(storm.speed));
        REQUIRE(storm.speed >= 0.0f);
        REQUIRE(storm.speed < 16.0f);

        // The vector and the reported scalar must agree, or consumers that use one
        // (grass reads speed/direction) and consumers that use the other (smoke
        // reads the vector) would disagree about the same wind.
        const float magnitude = std::sqrt(calm.velocityX * calm.velocityX + calm.velocityZ * calm.velocityZ);
        REQUIRE(magnitude == Catch::Approx(calm.speed).margin(1e-4f));
    }
}

TEST_CASE("Wind is smooth: no step between adjacent simulation frames", "[World][Weather][Wind]")
{
    // This is the whole point of replacing the 5-second random walk, which changed
    // the vector discontinuously and made smoke visibly jerk. Over one 100 ms frame
    // at the roughest sea state the vector must move by centimetres per second, not
    // metres per second.
    float worst = 0.0f;
    for (int step = 0; step < 6000; ++step)
    {
        const int64_t ms = static_cast<int64_t>(step) * 100;
        const WindSample a = EvaluateWind(WindConditions{}, 1.0f, ms);
        const WindSample b = EvaluateWind(WindConditions{}, 1.0f, ms + 100);
        const float dx = b.velocityX - a.velocityX;
        const float dz = b.velocityZ - a.velocityZ;
        worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
    }
    REQUIRE(worst < 0.25f);
}

TEST_CASE("Overcast drives the mean wind", "[World][Weather][Wind]")
{
    // Coupling to the engine's existing authoritative weather scalar is what makes
    // one front raise the sea, lean the grass and run the clouds together.
    float calmTotal = 0.0f;
    float stormTotal = 0.0f;
    constexpr int kSamples = 600;
    for (int step = 0; step < kSamples; ++step)
    {
        const int64_t ms = static_cast<int64_t>(step) * 3000;
        calmTotal += MeanSpeedAt(0.0f, ms);
        stormTotal += MeanSpeedAt(1.0f, ms);
    }
    REQUIRE(stormTotal > calmTotal * 4.0f);
}

TEST_CASE("Gusts ride on a mean that stays still", "[World][Weather][Wind]")
{
    // The ocean spectrum is parameterised by meanSpeed and is rebuilt when it moves
    // buckets. If the mean moved on the gust clock the FFT would thrash, so assert
    // the separation the water latch depends on: over one minute the mean must
    // barely move while the instantaneous speed swings.
    const WindConditions conditions{};
    float meanMin = 1e9f;
    float meanMax = -1e9f;
    float instMin = 1e9f;
    float instMax = -1e9f;
    for (int step = 0; step <= 600; ++step)
    {
        const WindSample s = EvaluateWind(conditions, 1.0f, static_cast<int64_t>(step) * 100);
        meanMin = std::min(meanMin, s.meanSpeed);
        meanMax = std::max(meanMax, s.meanSpeed);
        instMin = std::min(instMin, s.speed);
        instMax = std::max(instMax, s.speed);
    }
    const float meanSwing = meanMax - meanMin;
    const float instSwing = instMax - instMin;
    REQUIRE(meanSwing < 0.5f);
    REQUIRE(instSwing > meanSwing * 4.0f);
}

TEST_CASE("The mean wind crosses sea-state buckets rarely", "[World][Weather][Wind]")
{
    // Direct evidence for the FFT rebuild budget: WaterWgpu latches the mean wind
    // into 1 m/s buckets and only re-derives the spectrum when it leaves one. Count
    // the crossings over a simulated hour of steadily worsening weather; a handful
    // is fine, one per frame is the failure this guards.
    int crossings = 0;
    float latched = MeanSpeedAt(0.0f, 0);
    constexpr int kSteps = 3600; // one sample per simulated second
    for (int step = 0; step <= kSteps; ++step)
    {
        const float overcast = static_cast<float>(step) / static_cast<float>(kSteps);
        const float mean = MeanSpeedAt(overcast, static_cast<int64_t>(step) * 1000);
        if (std::abs(mean - latched) > 0.5f + 0.35f)
        {
            latched = std::round(mean);
            ++crossings;
        }
    }
    // Overcast sweeps its entire 0..1 range here, which is far faster than the
    // engine's forecast ever moves; even so the spectrum re-derives ~10 times an
    // hour, not 60 times a second.
    REQUIRE(crossings > 0);
    REQUIRE(crossings < 30);
}

TEST_CASE("Ballistics wind is opt-in", "[World][Weather][Wind]")
{
    // Classic OFP missions must keep behaving as they did. The flag is what makes
    // ShotShell::Simulate's expression byte-identical to the pre-wind code.
    const bool previous = WindModel::BallisticsEnabled();
    WindModel::SetBallisticsEnabled(false);
    REQUIRE_FALSE(WindModel::BallisticsEnabled());
    WindModel::SetBallisticsEnabled(true);
    REQUIRE(WindModel::BallisticsEnabled());
    WindModel::SetBallisticsEnabled(previous);
}

TEST_CASE("The model instance caches one evaluation per tick", "[World][Weather][Wind]")
{
    WindModel model;
    REQUIRE_FALSE(model.IsActive());

    model.Init();
    REQUIRE(model.IsActive());

    model.Update(42 * kMinute, 0.75f);
    const WindSample cached = model.Sample();
    const WindSample direct = EvaluateWind(model.Conditions(), 0.75f, 42 * kMinute);
    REQUIRE(cached.velocityX == direct.velocityX);
    REQUIRE(cached.velocityZ == direct.velocityZ);
    REQUIRE(model.Overcast() == Catch::Approx(0.75f));

    // Idempotent: re-ticking the same frame cannot move the world.
    model.Update(42 * kMinute, 0.75f);
    REQUIRE(model.Sample().velocityX == cached.velocityX);

    model.Deactivate();
    REQUIRE_FALSE(model.IsActive());
}
