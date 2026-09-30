#pragma once

#include <algorithm>

namespace Poseidon::render::grass
{
// The shaders advect their gust field at (30 + 15 * strength) * scroll.
// In automatic mode that must resolve to the measured wind speed, rather
// than another multiple of it. The small floor keeps noise moving in calm
// air; zero strength still produces no displacement.
inline float GustScrollForWind(float speed, float strength)
{
    const float shaderSpeed = 30.0f + 15.0f * std::clamp(strength, 0.0f, 3.0f);
    return std::clamp(std::max(speed, 0.0f) / shaderSpeed, 0.02f, 1.0f);
}
}
