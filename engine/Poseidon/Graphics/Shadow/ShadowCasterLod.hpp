#pragma once

#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

namespace Poseidon::shadow
{
inline bool NeedsExteriorCasterLod(const LODShape& shape, int level)
{
    return level >= 0 && level < shape.NLevels() &&
           (shape.IsSpecLevel(level, VIEW_PILOT) || shape.IsSpecLevel(level, VIEW_GUNNER) ||
            shape.IsSpecLevel(level, VIEW_CARGO));
}
}
