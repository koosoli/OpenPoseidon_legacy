#pragma once
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
// Unset keeps normal adaptation. Any supplied nonpositive/nonfinite token
// chooses the highest-detail bound; a finite positive token pins density.
inline float ParseBenchmarkLodFix(const char* value)
{
    if (!value)
        return 0.0f;
    const float parsed = std::strtof(value, nullptr);
    return std::isfinite(parsed) && parsed > 0 ? parsed : -1.0f;
}
}
