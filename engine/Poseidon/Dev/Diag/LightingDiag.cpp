#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <cstdlib>

namespace Poseidon::Dev
{

LightCounters& GLightCounters()
{
    static LightCounters counters;
    return counters;
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
        return s;
    }();
    return settings;
}

void LampLightSettingsChanged()
{
    ++GLampLightSettings().generation;
}

} // namespace Poseidon::Dev
