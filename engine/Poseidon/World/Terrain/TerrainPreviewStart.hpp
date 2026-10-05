// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>

namespace Poseidon
{
struct TerrainPreviewStart
{
    float x, z, height;
};
// One bounded startup search on the already loaded heightfield. Prefer the centre
// on equally high terrain, but avoid island-centre sea or fixed coordinates.
template <class HeightAt>
TerrainPreviewStart ChooseTerrainPreviewStart(float extent, HeightAt heightAt)
{
    TerrainPreviewStart best{extent * 0.5f, extent * 0.5f, 0.0f};
    const float centreHeight = heightAt(best.x, best.z);
    if (std::isfinite(centreHeight)) best.height = centreHeight;
    for (int z = 1; z <= 9; ++z)
        for (int x = 1; x <= 9; ++x)
        {
            const float px = extent * (static_cast<float>(x) * 0.1f);
            const float pz = extent * (static_cast<float>(z) * 0.1f);
            const float height = heightAt(px, pz);
            if (std::isfinite(height) && height > best.height) best = {px, pz, height};
        }
    return best;
}
}
