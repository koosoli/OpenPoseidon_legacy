#pragma once

namespace Poseidon::GroundMeshSurface
{
// Source permission alone cannot license a whole model: every fragment also
// proves its upward authored side, bare-terrain proximity and physical rain cover.
inline bool OwnerAllowed(bool isStatic, bool intact, bool primary,
                         bool buildingOrThing, bool person, bool vegetation,
                         bool configuredAnimations) noexcept
{
    return isStatic && intact && (primary || buildingOrThing) &&
        !person && !vegetation && !configuredAnimations;
}
inline bool SectionAllowed(bool depthWriting, bool glass, bool leafCards) noexcept
{
    return depthWriting && !glass && !leafCards;
}
} // namespace Poseidon::GroundMeshSurface
