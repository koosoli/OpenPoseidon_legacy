#pragma once

namespace Poseidon
{
// World::Draw explicitly forces the inside LOD only for its actual inside
// camera carrier (including an inside camera effect). A fallback InsideLOD=0
// is also a normal visual LOD and cannot identify that queue by equality alone.
constexpr bool ExplicitInsideViewDraw(int drawLod, int forcedLod, int insideLod)
{
    return insideLod >= 0 && drawLod == insideLod && forcedLod == insideLod;
}
} // namespace Poseidon
