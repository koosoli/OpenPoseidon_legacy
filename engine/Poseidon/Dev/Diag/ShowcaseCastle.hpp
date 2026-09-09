#pragma once
#include <vector>

namespace Poseidon::Dev
{
struct CastleBlock
{
    float x, z, height;
    bool side;
    float halfWidth = 0.99f;
};

inline std::vector<CastleBlock> ShowcaseCastleLayout()
{
    std::vector<CastleBlock> blocks;
    blocks.reserve(512);
    for (int layer = 0; layer < 14; ++layer)
    {
        const float y = 0.4f + layer * 0.805f;
        for (int col = 0; col < 16; ++col)
        {
            if (layer >= 10 && col != 0 && col != 15) continue;
            const float x = -15.0f + col * 2.0f;
            // One spanning lintel rests on the jambs. Separate floating bricks
            // cannot bridge a gate without mortar or structural constraints.
            if (layer == 4 && col == 6)
                blocks.push_back({0.0f, -9.0f, y, false, 3.99f});
            if (!(col >= 7 && col <= 8 && layer < 4) && !(layer == 4 && col >= 6 && col <= 9))
                blocks.push_back({x, -9.0f, y, false});
            blocks.push_back({x, 9.0f, y, false});
        }
        if (layer < 10)
            for (int col = 0; col < 8; ++col)
            {
                const float z = -7.0f + col * 2.0f;
                blocks.push_back({-15.6f, z, y, true});
                blocks.push_back({15.6f, z, y, true});
            }
    }
    return blocks;
}
} // namespace Poseidon::Dev
