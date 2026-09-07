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

} // namespace Poseidon
