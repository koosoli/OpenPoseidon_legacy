#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace Poseidon::EarthStreaming
{
struct GroundSample
{
    float height, dX, dZ;
    uint64_t revision, generation;
    float Penetration(float pointY, float above) const { return height + above - pointY; }
};

// The bounded near mesh only. Heights must be the actual rendered vertex Y,
// including projection/curvature. Far horizon and globe meshes are not ground.
struct GroundPatch
{
    float x, z, gridMetres;
    int samples;
    uint64_t revision, generation;
    std::vector<float> heights;

    bool Valid() const
    {
        if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(gridMetres) ||
            gridMetres < 1 || samples < 2 || samples > 257 || !revision || !generation ||
            heights.size() != size_t(samples) * size_t(samples)) return false;
        const double span = double(samples - 1) * gridMetres;
        if (span > 25600 || std::abs(double(x)) > 550000 || std::abs(double(z)) > 550000 ||
            std::abs(double(x) + span) > 550000 || std::abs(double(z) + span) > 550000) return false;
        return std::all_of(heights.begin(), heights.end(), [](float y) {
            return std::isfinite(y) && std::abs(y) <= 100000;
        });
    }

    bool Contains(float px, float pz, float margin = 0) const
    {
        if (!std::isfinite(px) || !std::isfinite(pz) || !std::isfinite(margin) || margin < 0) return false;
        const double span = double(samples - 1) * gridMetres;
        return double(px) >= double(x) + margin && double(pz) >= double(z) + margin &&
            double(px) <= double(x) + span - margin && double(pz) <= double(z) + span - margin;
    }

    std::optional<GroundSample> Sample(float px, float pz) const
    {
        if (!Contains(px, pz)) return std::nullopt;
        const double gx = (double(px) - x) / gridMetres, gz = (double(pz) - z) / gridMetres;
        const int ix = std::min(int(gx), samples - 2), iz = std::min(int(gz), samples - 2);
        const float u = float(gx - ix), v = float(gz - iz);
        const size_t a = size_t(iz) * samples + ix;
        const float y00 = heights[a], y10 = heights[a + 1];
        const float y01 = heights[a + samples], y11 = heights[a + samples + 1];
        // Same diagonal and barycentric interpolation as {a,c,b,b,c,d}.
        if (u + v <= 1)
            return GroundSample{y00 + (y10 - y00) * u + (y01 - y00) * v,
                (y10 - y00) / gridMetres, (y01 - y00) / gridMetres, revision, generation};
        return GroundSample{y11 + (y01 - y11) * (1 - u) + (y10 - y11) * (1 - v),
            (y11 - y01) / gridMetres, (y11 - y10) / gridMetres, revision, generation};
    }
};
}
