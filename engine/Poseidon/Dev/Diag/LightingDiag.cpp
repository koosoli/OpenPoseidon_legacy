#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace Poseidon::Dev
{

std::array<float, 3> LampTemperatureColor(float kelvin)
{
    // Approximate display-RGB temperature fit, matching the existing RGB control.
    // Formula: tannerhelland.com/2012/09/18/convert-temperature-rgb-algorithm-code.html
    const float t = std::clamp(std::isfinite(kelvin) ? kelvin : 2700.0f, 1000.0f, 12000.0f) / 100.0f;
    const float r = t <= 66.0f ? 255.0f : 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
    const float g = t <= 66.0f ? 99.4708025861f * std::log(t) - 161.1195681661f
                              : 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
    const float b = t >= 66.0f ? 255.0f : t <= 19.0f ? 0.0f
                                    : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
    return {std::clamp(r / 255.0f, 0.0f, 1.0f), std::clamp(g / 255.0f, 0.0f, 1.0f),
            std::clamp(b / 255.0f, 0.0f, 1.0f)};
}

LightCounters& GLightCounters()
{
    static LightCounters counters;
    return counters;
}

LightVolumeConeSettings& GLightVolumeCone()
{
    static LightVolumeConeSettings s = []
    {
        LightVolumeConeSettings v;
        if (const char* e = std::getenv("POSEIDON_LIGHT_VOLUME_CONE"))
            v.enabled = (std::atoi(e) != 0);
        return v;
    }();
    return s;
}

LocalShadowSettings& GLocalShadowSettings()
{
    static LocalShadowSettings s = []
    {
        LocalShadowSettings v;
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOWS"))
            v.enabled = (std::atoi(e) != 0);
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_LIGHTS"))
            v.maxLights = std::clamp(std::atoi(e), 0, 24);
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_NEAR"))
        {
            const double d = std::atof(e);
            if (d > 0.0)
                v.nearD = static_cast<float>(d);
        }
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_RANGE"))
        {
            const double d = std::atof(e);
            if (d > 0.0)
                v.rangeScale = static_cast<float>(d);
        }
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_RANGE_MAX"))
        {
            const double d = std::atof(e);
            if (d > 0.0)
                v.rangeMax = static_cast<float>(d);
        }
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_SELF"))
            v.selfRadius = std::max(0.0f, static_cast<float>(std::atof(e)));
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_DARKNESS"))
            v.darkness = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.0f);
        // LGT-026. The cache is the A/B arm every measurement of this feature is stated
        // against, so it has to be reachable without a rebuild.
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_CACHE"))
            v.cacheStatic = (std::atoi(e) != 0);
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_FORCE_REFRESH"))
            v.forceRefresh = (std::atoi(e) != 0);
        return v;
    }();
    return s;
}

SpotBeamSettings& GSpotBeam()
{
    static SpotBeamSettings s = []
    {
        SpotBeamSettings v;
        if (const char* e = std::getenv("POSEIDON_SPOT_BEAM_SHAPE"))
        {
            const double d = std::atof(e);
            if (d > 0.0)
                v.shape = static_cast<float>(d);
        }
        return v;
    }();
    return s;
}

LampLightSettings& GLampLightSettings()
{
    // Seeded once from the environment so a capture harness can sweep the lamp look
    // without a UI, the same way WGR_MAX_ACTIVE_LIGHTS / WGR_MOON_LIGHT_GAIN work.
    // Deliberately NOT in the ImGui panel yet: DebugOverlay.cpp currently carries
    // another session's uncommitted work in this worktree.
    //   POSEIDON_LAMP_RADIUS_SCALE     multiplies startAtten (pool size). 1 = shipped.
    //   POSEIDON_LAMP_BRIGHTNESS_SCALE multiplies the lamp colour. 1 = shipped.
    //   POSEIDON_LAMP_LIGHTS=0         no street-lamp light at all (the A/B).
    //   POSEIDON_LAMP_CONE=0           back to an omnidirectional point light (LAMP-004).
    //   POSEIDON_LAMP_COLOR=r,g,b      lamp colour; =0 keeps the authored config colour.
    //   POSEIDON_LAMP_AMBIENT_SCALE    0..1 the direction-independent term. 1 = authored.
    //   POSEIDON_LAMP_SPILL            0..1 weak omni light beside the cone. 0 = pure spot.
    //   POSEIDON_LAMP_SPILL_RADIUS     that light's radius, relative to the pool's.
    static LampLightSettings settings = []
    {
        LampLightSettings s;
        if (const char* v = std::getenv("POSEIDON_LAMP_RADIUS_SCALE"))
        {
            const double d = std::atof(v);
            if (d > 0.0)
                s.radiusScale = static_cast<float>(d);
        }
        if (const char* v = std::getenv("POSEIDON_LAMP_BRIGHTNESS_SCALE"))
        {
            const double d = std::atof(v);
            if (d > 0.0)
                s.brightnessScale = static_cast<float>(d);
        }
        if (const char* v = std::getenv("POSEIDON_LAMP_LIGHTS"))
            s.enabled = (std::atoi(v) != 0);
        if (const char* v = std::getenv("POSEIDON_LAMP_AMBIENT_SCALE"))
        {
            const double d = std::atof(v);
            if (d >= 0.0)
                s.ambientScale = static_cast<float>(d);
        }
        if (const char* v = std::getenv("POSEIDON_LAMP_CONE"))
            s.cone = (std::atoi(v) != 0);
        if (const char* v = std::getenv("POSEIDON_LAMP_SPILL"))
            s.spill = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("POSEIDON_LAMP_SPILL_AMBIENT"))
            s.spillAmbient = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        if (const char* v = std::getenv("POSEIDON_LAMP_SPILL_RADIUS"))
        {
            const double d = std::atof(v);
            if (d > 0.0)
                s.spillRadius = static_cast<float>(d);
        }
        if (const char* v = std::getenv("POSEIDON_LAMP_COLOR"))
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (std::sscanf(v, "%f,%f,%f", &r, &g, &b) == 3)
            {
                s.color[0] = r, s.color[1] = g, s.color[2] = b;
                s.colorOverride = true;
            }
            else if (std::atoi(v) == 0)
            {
                s.colorOverride = false; // POSEIDON_LAMP_COLOR=0 -> the authored config colour
            }
        }
        return s;
    }();
    return settings;
}

void LampLightSettingsChanged()
{
    ++GLampLightSettings().generation;
}

void ResetLightingSettings()
{
    auto& lamp = GLampLightSettings();
    const auto generation = lamp.generation;
    lamp = LampLightSettings{};
    lamp.generation = generation;
    LampLightSettingsChanged(); // Existing lamps must rebuild even on repeated resets.
    GLocalShadowSettings() = LocalShadowSettings{};
    GSpotBeam() = SpotBeamSettings{};
    GLightVolumeCone() = LightVolumeConeSettings{};
    GBulbGlow() = BulbGlowSettings{};
}

} // namespace Poseidon::Dev
