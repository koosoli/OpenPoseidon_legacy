#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/LegacyGlass.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <limits>

using namespace Poseidon;

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
