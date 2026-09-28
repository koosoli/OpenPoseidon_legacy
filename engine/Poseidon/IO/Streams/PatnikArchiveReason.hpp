#pragma once
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Poseidon::PatnikArchiveReason
{
inline bool Matches(const char* path)
{
    if (!path) return false;
    const char* leaf = path;
    size_t n = 0;
    for (; n < 8192 && path[n]; ++n)
        if (path[n] == '/' || path[n] == '\\') leaf = path + n + 1;
    if (n == 8192) return false;
    constexpr char target[] = "patnik_smdi.paa";
    if (std::strlen(leaf) != sizeof(target) - 1) return false;
    for (size_t i = 0; i < sizeof(target) - 1; ++i)
        if (std::tolower(static_cast<unsigned char>(leaf[i])) != target[i]) return false;
    return true;
}
inline bool Enabled()
{
    static const bool enabled = [] {
        const char* flag = std::getenv("WGR_PATNIK_ARCHIVE_REASON");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    return enabled;
}
inline bool ReserveRow()
{
    static std::atomic<uint32_t> rows{0};
    uint32_t current = rows.load(std::memory_order_relaxed);
    while (current < 32)
        if (rows.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return true;
    return false;
}
}
