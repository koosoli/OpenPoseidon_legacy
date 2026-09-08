#pragma once

#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>

namespace Poseidon::render
{
enum class DecalDepth { None, Test, Soft };

inline DecalDepth SelectDecalDepth(int spec, bool softParticles)
{
    if (Has(SplitLegacy(spec).backend, Backend::NoZBuf))
        return DecalDepth::None;
    return softParticles ? DecalDepth::Soft : DecalDepth::Test;
}
}
