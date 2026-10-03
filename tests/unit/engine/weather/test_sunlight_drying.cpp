#include "../../../../engine/Poseidon/World/Weather/SunlightDrying.hpp"
#include "../../../../engine/Poseidon/World/Weather/RainWaterField.hpp"
#include "../../../../engine/WgpuRenderer/TerrainPuddles.hpp"
#include <cassert>
#include <limits>

using Poseidon::SunlightDryingExposure;

static Poseidon::RainWaterField FlatWetField()
{
    Poseidon::RainWaterField field;
    assert(field.Configure(3, 3, 1, std::vector<float>(9, 10)));
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x)
            assert(field.AddWater(x, z, .01f));
    return field;
}

int main()
{
    const float clearDay = SunlightDryingExposure(-.8f, 0, 0);
    assert(std::abs(clearDay - .8f) < 1e-6f);
    assert(SunlightDryingExposure(.8f, 0, 1) == 0); // actual night sun travel
    assert(SunlightDryingExposure(-.8f, 0, 1) == 0); // moon travel must not dry
    assert(SunlightDryingExposure(.8f, 0, 0) == 0); // below-horizon sun
    assert(SunlightDryingExposure(0, 0, 0) == 0);
    assert(SunlightDryingExposure(-.8f, 1, 0) == 0); // actual opaque overcast
    assert(std::abs(SunlightDryingExposure(-.8f, .5f, .4f) - .12f) < 1e-6f);
    assert(SunlightDryingExposure(-2, -1, -1) == 1); // bounded external inputs
    assert(SunlightDryingExposure(-.8f, 2, 0) == 0);
    assert(SunlightDryingExposure(-.8f, 0, 2) == 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float invalid : {nan, inf, -inf})
    {
        assert(SunlightDryingExposure(invalid, 0, 0) == 0);
        assert(SunlightDryingExposure(-.8f, invalid, 0) == 0);
        assert(SunlightDryingExposure(-.8f, 0, invalid) == 0);
    }

    // Use the actual production histories and unchanged coefficients. Source
    // seeding is a CPU fixture, not an installed-game acceptance shortcut.
    Poseidon::TerrainPuddles::Wetness day, night, windy, cloudy;
    day.Update(0, 0); day.value = .8f;
    night = windy = cloudy = day;
    const float dayWet = day.Update(120, 0, clearDay, 0);
    const float nightWet = night.Update(120, 0, SunlightDryingExposure(.8f, 0, 1), 0);
    const float cloudWet = cloudy.Update(120, 0, SunlightDryingExposure(-.8f, 1, 0), 0);
    const float windWet = windy.Update(120, 0, clearDay, 1);
    assert(dayWet < nightWet && windWet < dayWet);
    assert(cloudWet == nightWet);
    assert(std::abs(dayWet - .8f * std::exp(-2.6f * 120 / 180)) < 1e-6f);
    assert(std::abs(nightWet - .8f * std::exp(-120.0f / 180)) < 1e-6f);
    assert(day.Update(120, 0, clearDay, 0) == dayWet); // paused duplicate draw

    auto sunny = FlatWetField(), shaded = sunny, windyWater = sunny;
    const double initial = sunny.Volume();
    for (int i = 0; i < 2400; ++i)
    {
        sunny.Advance(.25f, 0, clearDay, 0);
        shaded.Advance(.25f, 0, 0, 0);
        windyWater.Advance(.25f, 0, clearDay, 1);
    }
    assert(sunny.Volume() < shaded.Volume() && windyWater.Volume() < sunny.Volume());
    assert(std::abs(sunny.At(1, 1).depth - (.01f - .0000108f * 600)) < 3e-6f);
    assert(std::abs(shaded.At(1, 1).depth - (.01f - .000006f * 600)) < 3e-6f);
    for (const auto* field : {&sunny, &shaded, &windyWater})
    {
        const auto b = field->WaterBudget();
        assert(b.rain == 0 && b.outlet == 0);
        assert(std::abs(field->Volume() - (initial - b.infiltration - b.evaporation)) < 1e-5);
    }
    const double remaining = sunny.Volume();
    sunny.Advance(0, 0, clearDay, 1);
    assert(sunny.Volume() == remaining); // actual physics pause remains inert

    Poseidon::RainWaterFine fineDay;
    assert(fineDay.Configure(3, 3, 1, std::vector<float>(9, 10)));
    for (int z = 0; z < 3; ++z)
        for (int x = 0; x < 3; ++x)
            assert(fineDay.AddWater(x + .5, z + .5, .01));
    auto fineShade = fineDay, fineWind = fineDay;
    const double fineInitial = fineDay.Volume();
    for (int i = 0; i < 2400; ++i)
    {
        fineDay.Advance(.25, 0, clearDay, 0);
        fineShade.Advance(.25, 0, 0, 0);
        fineWind.Advance(.25, 0, clearDay, 1);
    }
    assert(fineDay.Volume() < fineShade.Volume() && fineWind.Volume() < fineDay.Volume());
    for (const auto* field : {&fineDay, &fineShade, &fineWind})
    {
        const auto b = field->WaterBudget();
        assert(b.rain == 0 && b.outlet == 0);
        assert(std::abs(field->Volume() - (fineInitial - b.infiltration - b.evaporation)) < 1e-10);
    }
}
