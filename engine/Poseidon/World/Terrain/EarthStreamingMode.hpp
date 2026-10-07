#pragma once

#include <cstdlib>
#include <cstring>

namespace Poseidon::EarthStreaming
{
// Shared presentation/camera gate; this opt-in does not replace simulation land.
inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_WHOLE_WORLD");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
constexpr float CoordinateLimit = 500000.0f;
}
