#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <cstdlib>
#include <cstdio>

namespace Poseidon::Dev
{

LightCounters& GLightCounters()
{
    static LightCounters counters;
    return counters;
}

LocalShadowSettings& GLocalShadowSettings()
{
    static LocalShadowSettings s = []
    {
        LocalShadowSettings v;
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOWS"))
            v.enabled = (std::atoi(e) != 0);
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_LIGHTS"))
            v.maxLights = std::clamp(std::atoi(e), 0, 4);
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
        if (const char* e = std::getenv("POSEIDON_LOCAL_SHADOW_DARKNESS"))
            v.darkness = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.0f);
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

} // namespace Poseidon::Dev
