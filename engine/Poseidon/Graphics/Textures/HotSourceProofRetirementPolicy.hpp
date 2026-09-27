#pragma once
#include <cstdint>
namespace Poseidon::render
{
// Retires a single optional source-proof owner, never a payload/lease alias or GPU resource.
struct HotSourceProofRetirementPolicy
{
    static bool Reserve(bool enabled, bool owner, uint64_t gpuHandle, int alphaClass,
        bool hasSource, bool dynamic, bool delegated, bool& attempted)
    {
        if (!enabled || !owner || !gpuHandle || alphaClass < 0 || !hasSource || dynamic || delegated || attempted)
            return false;
        attempted = true; // Unknown/non-PAC sources need no repeated virtual retry until actual Init.
        return true;
    }
};
}
