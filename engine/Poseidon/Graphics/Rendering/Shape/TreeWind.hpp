#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>

// Call only for MapTree/MapSmallTree. Native XOB trees have no imported branch
// weights; their solid and cutout sections must share one affine deformation.
// Direct draws encode this as foliage kind 3; retained draws use instance bit 8.
inline bool IsNativeTreeWindModel(const char* modelName)
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_NATIVE_COHERENT_WIND");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !modelName) return false;
    const char* ext = std::strrchr(modelName, '.');
    if (!ext || std::strlen(ext) != 4) return false;
    return std::tolower(static_cast<unsigned char>(ext[1])) == 'x' &&
           std::tolower(static_cast<unsigned char>(ext[2])) == 'o' &&
           std::tolower(static_cast<unsigned char>(ext[3])) == 'b';
}
