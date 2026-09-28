#pragma once

#include <cstddef>
#include <string_view>

namespace Poseidon::Streaming
{
// Diagnostic-only, exact virtual names. No mounted-bank lookup for any other model.
inline int TreeFixtureModelOrdinal(std::string_view path)
{
    if (path == R"(dz\plants\tree\t_piceaabies_2d.p3d)") return 0;
    if (path == R"(dz\plants\tree\d_piceaabies_stumpb.p3d)") return 1;
    return -1;
}

inline bool SelectedTreeFixtureModel(std::string_view path)
{
    return TreeFixtureModelOrdinal(path) >= 0;
}

inline bool EligibleTreeFixtureBytes(size_t bytes)
{
    return bytes > 0 && bytes <= 8 * 1024 * 1024;
}
} // namespace Poseidon::Streaming
