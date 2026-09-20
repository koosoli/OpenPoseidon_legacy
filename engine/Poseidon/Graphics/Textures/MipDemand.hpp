#pragma once
#include <algorithm>
#include <cstdint>

namespace Poseidon::render
{
// A quality/thrash policy, not a GPU lifetime mechanism. Unknown demand keeps full detail.
struct MipDemand
{
    int candidate = -1;
    uint64_t since = 0;
    uint64_t replaced = 0;
    bool awaitingObservation = false;

    int Choose(int current, unsigned observed, unsigned age, unsigned groups,
               uint64_t frame, bool viewChanged, int maxBias)
    {
        if (viewChanged) { candidate = -1; return 0; }
        const bool fresh = groups != 0 && observed != 255 && age <= groups * 2 + 8;
        // A replacement changes the shader's mip coordinate system. Allow a complete
        // sampling rotation before interpreting the cleared observation as missing demand.
        if (awaitingObservation && !fresh && frame - replaced < groups * 2 + 8) return current;
        awaitingObservation = false;
        const int wanted = !fresh || observed == 0 ? 0 :
            std::clamp(current + static_cast<int>(observed) - 1, 0, maxBias);
        if (wanted <= current) { candidate = -1; return wanted; }
        if (candidate != wanted) { candidate = wanted; since = frame; }
        return frame - since >= std::max<uint64_t>(120, groups * 3) ? wanted : current;
    }
    void Replaced(uint64_t frame) { candidate = -1; replaced = frame; awaitingObservation = true; }
};
}
