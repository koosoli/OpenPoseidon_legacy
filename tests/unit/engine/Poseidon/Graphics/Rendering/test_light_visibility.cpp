#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/LegacyGlass.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <limits>
#include <Poseidon/Core/Global.hpp>
#include <catch2/catch_approx.hpp>

using namespace Poseidon;

TEST_CASE("Muzzle pulse is bounded and independent of simulation partition", "[Graphics][muzzle-light]")
{
    MuzzleFlashPulse pulse;
    REQUIRE(pulse.Energy(1000) == 0);
    pulse.Trigger(1000);
    REQUIRE(pulse.Energy(999) == 0); // a pre-birth world clock fails closed
    REQUIRE(pulse.Energy(1000) == 1);
    REQUIRE(pulse.Energy(1020) == 1);
    float previous = 1;
    for (int age = 0; age <= 300; ++age)
    {
        const float energy = pulse.Energy(1000 + age);
        REQUIRE(energy >= 0);
        REQUIRE(energy <= previous);
        previous = energy;
    }
    REQUIRE(pulse.Energy(1110) == 0);
    pulse.Trigger(1040, 100); // burst retrigger cannot accumulate energy
    REQUIRE(pulse.Energy(1040) == 1);
    REQUIRE(pulse.Energy(1150) == 0);
    pulse.Trigger(1040, std::numeric_limits<float>::quiet_NaN());
    REQUIRE(pulse.Energy(1040) == 0);
    pulse.Trigger(1040, -1);
    REQUIRE(pulse.Energy(1040) == 0);
    pulse.Trigger(std::numeric_limits<std::int64_t>::min());
    REQUIRE(pulse.Energy(std::numeric_limits<std::int64_t>::max()) == 0);
    REQUIRE(MuzzleFlashPulse::CoreMetres * MuzzleFlashPulse::EndScale == Catch::Approx(8.4));
}

TEST_CASE("Actual muzzle point light expires through base dispatch without an AI tick", "[Graphics][muzzle-light]")
{
    struct RestoreClock
    {
        Foundation::Time saved = Glob.time;
        ~RestoreClock() { Glob.time = saved; }
    } restore;
    Glob.time = Foundation::Time(1000);
    MuzzleFlashLight muzzle;
    Light* light = &muzzle;
    REQUIRE_FALSE(light->IsOn());
    muzzle.Trigger(Vector3(5, 2, 7));
    REQUIRE(light->IsOn());
    REQUIRE(light->IsDaylightVisible());
    LightDescription peak{}, faded{}, expired{};
    light->GetDescription(peak);
    REQUIRE(peak.type == LTPoint);
    REQUIRE(peak.pos.X() == 5);
    REQUIRE(peak.pos.Y() == 2);
    REQUIRE(peak.pos.Z() == 7);
    REQUIRE(peak.startAtten == Catch::Approx(0.7));
    REQUIRE(peak.endAttenScale == 12);
    REQUIRE(peak.diffuse.R() > peak.diffuse.G());
    REQUIRE(peak.diffuse.G() > peak.diffuse.B());
    Glob.time = Foundation::Time(1060);
    REQUIRE(light->IsOn());
    light->GetDescription(faded);
    REQUIRE(faded.diffuse.R() < peak.diffuse.R());
    REQUIRE(faded.diffuse.R() > 0);
    LightDescription paused{};
    light->GetDescription(paused);
    REQUIRE(paused.diffuse.R() == faded.diffuse.R()); // rendering does not advance history
    Glob.time = Foundation::Time(1110); // no WeaponLightSource::Simulate call
    REQUIRE_FALSE(light->IsOn());
    light->GetDescription(expired);
    REQUIRE(expired.diffuse.R() == 0);
    REQUIRE(expired.ambient.R() == 0);
    muzzle.Trigger(Vector3(-1, 3, 8));
    REQUIRE(light->IsOn());
    light->GetDescription(peak);
    REQUIRE(peak.pos.X() == -1);
    REQUIRE(peak.diffuse.R() == 2); // no burst accumulation or new light instance
    light->Switch(false);
    REQUIRE_FALSE(light->IsOn());
    light->GetDescription(expired);
    REQUIRE(expired.diffuse.R() == 0);
    muzzle.Trigger(Vector3(std::numeric_limits<float>::quiet_NaN(), 0, 0));
    REQUIRE_FALSE(light->IsOn());
    light->GetDescription(expired);
    REQUIRE(expired.diffuse.R() == 0);
    LightPoint ordinary(HWhite, HBlack);
    Light* unchanged = &ordinary;
    REQUIRE(unchanged->IsOn());
    REQUIRE_FALSE(unchanged->IsDaylightVisible());
}

TEST_CASE("Explicit reflector cone reaches renderer without changing legacy constructors", "[Graphics][LightVisibility]")
{
    LightReflector light(nullptr, HBlack, HBlack, 0.4f);
    LightDescription original{}, narrow{}, wide{};
    light.GetDescription(original);
    CHECK(original.phi > 0.6f);
    light.SetAngle(0.12f);
    light.GetDescription(narrow);
    CHECK(narrow.phi == 0.12f);
    CHECK(narrow.theta == 0.12f * 0.6f);
    light.SetAngle(1.1f);
    light.GetDescription(wide);
    CHECK(wide.phi == 1.1f);
    CHECK(wide.phi > narrow.phi);
    light.SetAngle(std::numeric_limits<float>::quiet_NaN());
    light.SetAngle(-1);
    light.GetDescription(wide);
    CHECK(wide.phi == 1.1f);
}

TEST_CASE("Legacy glazing classification excludes terrain decals", "[Graphics][Material]")
{
    CHECK(LegacyGlassAlphaIsGlazing(false,0,99,29)); // UH60 glazing histogram.
    CHECK_FALSE(LegacyGlassAlphaIsGlazing(true,0,99,29)); // Same alpha cannot turn a road into glass.
    CHECK_FALSE(LegacyGlassAlphaIsGlazing(false,11.5,10.4,212));
    CHECK_FALSE(LegacyGlassAlphaIsGlazing(false,50,45,60));
    CHECK_FALSE(LegacyGlassAlphaIsGlazing(false,0,100,255));
}

TEST_CASE("Picture mode defaults to subtle blur and manual focus", "[Graphics][PictureMode]")
{
    const Engine::DepthOfFieldSettings settings;
    CHECK(settings.maxBlurPixels == 4.0f);
    CHECK_FALSE(settings.enabled);
    CHECK_FALSE(settings.followPlayer);
    CHECK_FALSE(settings.focusOnClick);
}

TEST_CASE("Lighting reset restores defaults and invalidates live lamps", "[Graphics][LightVisibility]")
{
    const auto savedLamp = Dev::GLampLightSettings();
    const auto savedShadow = Dev::GLocalShadowSettings();
    const auto savedBeam = Dev::GSpotBeam();
    const auto savedCone = Dev::GLightVolumeCone();
    const auto savedGlow = GBulbGlow();
    Dev::GLampLightSettings().generation = 72;
    Dev::GLampLightSettings().brightnessScale = 7;
    Dev::GLampLightSettings().colorOverride = true;
    Dev::GLocalShadowSettings().enabled = false;
    Dev::GLocalShadowSettings().forceRefresh = true;
    GBulbGlow().strength = 7;
    Dev::ResetLightingSettings();
    CHECK(Dev::GLampLightSettings().generation == 73);
    CHECK(Dev::GLampLightSettings().brightnessScale == Dev::LampLightSettings{}.brightnessScale);
    CHECK(Dev::GLampLightSettings().colorOverride == Dev::LampLightSettings{}.colorOverride);
    CHECK(Dev::GLocalShadowSettings().enabled == Dev::LocalShadowSettings{}.enabled);
    CHECK_FALSE(Dev::GLocalShadowSettings().forceRefresh);
    CHECK(GBulbGlow().strength == BulbGlowSettings{}.strength);
    Dev::ResetLightingSettings();
    CHECK(Dev::GLampLightSettings().generation == 74);
    Dev::GLampLightSettings() = savedLamp;
    Dev::GLocalShadowSettings() = savedShadow;
    Dev::GSpotBeam() = savedBeam;
    Dev::GLightVolumeCone() = savedCone;
    GBulbGlow() = savedGlow;
}

TEST_CASE("Lamp temperatures are bounded and preserve the custom default", "[Graphics][LightVisibility]")
{
    const auto warm = Dev::LampTemperatureColor(2700);
    const auto cool = Dev::LampTemperatureColor(12000);
    REQUIRE(warm[0] > warm[1]);
    REQUIRE(warm[1] > warm[2]);
    REQUIRE(cool[2] > cool[0]);
    REQUIRE(Dev::LampTemperatureColor(0) == Dev::LampTemperatureColor(1000));
    REQUIRE(Dev::LampTemperatureColor(50000) == cool);
    REQUIRE(Dev::LampTemperatureColor(std::numeric_limits<float>::quiet_NaN()) == warm);
    for (int k = 1000; k <= 12000; k += 100)
        for (const float channel : Dev::LampTemperatureColor(static_cast<float>(k)))
            REQUIRE((channel >= 0 && channel <= 1));
    REQUIRE_FALSE(Dev::LampLightSettings{}.useColorTemperature);
}

namespace
{
class AttachedReflectorProbe final : public LightReflector
{
    Object* _carrier;
public:
    explicit AttachedReflectorProbe(Object* carrier)
        : LightReflector(nullptr, Color(HWhite), Color(HBlack), 0.5f), _carrier(carrier) {}
    Object* AttachedOn() override { return _carrier; }
};
}

TEST_CASE("Light-base visibility dispatch preserves the carrier and bulb switch", "[Graphics][LightVisibility]")
{
    Ref<ObjectPlain> carrier = new ObjectPlain(nullptr, -1);
    AttachedReflectorProbe reflector(carrier);
    Light* light = &reflector;
    REQUIRE(light->AttachedOn() == carrier.GetRef());
    REQUIRE(light->HasVisibleBulb());
    reflector.SetDrawHalo(false);
    REQUIRE_FALSE(light->HasVisibleBulb());

    LightPoint point;
    light = &point;
    REQUIRE(light->AttachedOn() == nullptr);
    REQUIRE_FALSE(light->HasVisibleBulb());
}
