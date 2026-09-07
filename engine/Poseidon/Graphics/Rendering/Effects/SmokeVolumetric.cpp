#include <Poseidon/Graphics/Rendering/Effects/SmokeVolumetric.hpp>

#include <algorithm>
#include <cstdlib>

namespace Poseidon
{

// Seeded once from the environment, exactly like GLegacySmokeShading: the dev panel holds
// the mouse, so an automated capture cannot tick a box, and without an env seed nothing
// here can be A/B'd from a command line.
SmokeSoftParticles& GSmokeSoftParticles()
{
    static SmokeSoftParticles soft = []
    {
        SmokeSoftParticles s;
        if (const char* v = std::getenv("POSEIDON_SMOKE_SOFT"))
            s.enabled = (*v == '1');
        if (const char* v = std::getenv("POSEIDON_SMOKE_SOFT_FADE"))
            s.fade = std::clamp(static_cast<float>(std::atof(v)), 0.05f, 20.0f);
        return s;
    }();
    return soft;
}

SmokeVolumetricParams& GSmokeVolumetric()
{
    static SmokeVolumetricParams vol = []
    {
        SmokeVolumetricParams v;
        if (const char* e = std::getenv("POSEIDON_SMOKE_SYSTEM"))
        {
            const int m = std::atoi(e);
            v.mode = (m == 1) ? SmokeSystemMode::Volumetric
                              : (m == 2 ? SmokeSystemMode::Both : SmokeSystemMode::Legacy);
        }
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_CELL"))
            v.cellSize = std::clamp(static_cast<float>(std::atof(e)), 0.1f, 8.0f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_STEPS"))
            v.marchSteps = std::clamp(std::atoi(e), 4, 256);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_SUN_STEPS"))
            v.sunSteps = std::clamp(std::atoi(e), 0, 16);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_DENSITY"))
            v.density = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 20.0f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_EXTINCTION"))
            v.extinction = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 20.0f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_ALBEDO"))
            v.albedo = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.0f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_G"))
            v.anisotropy = std::clamp(static_cast<float>(std::atof(e)), -0.9f, 0.9f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_SELFSHADOW"))
            v.selfShadow = std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.0f);
        if (const char* e = std::getenv("POSEIDON_SMOKE_VOL_SCALE"))
            v.scale = std::clamp(std::atoi(e), 1, 4);
        return v;
    }();
    return vol;
}

} // namespace Poseidon
