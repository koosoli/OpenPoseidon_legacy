#pragma once
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Poseidon::PatnikArchiveReason
{
// Two fixed source identities. The startup selector never accepts an arbitrary path.
inline constexpr std::string_view Patnik = R"(dz\structures\residential\misc\data\patnik_smdi.paa)";
inline constexpr std::string_view Tree = R"(dz\plants\tree\data\t_piceaabies_trunk_no.paa)";
inline std::string_view Select(const char* value) noexcept
{
    if (!value || !*value || std::strcmp(value, "patnik") == 0) return Patnik;
    if (std::strcmp(value, "tree") == 0) return Tree;
    return {}; // Invalid selector refuses both probe and targeted trace.
}
inline std::string_view TargetKey() noexcept
{
    static const auto key = Select(std::getenv("WGR_WARM_STAGE_TARGET"));
    return key;
}
inline bool Same(char a, char b) noexcept
{
    if (a == '/') a = '\\';
    if (b == '/') b = '\\';
    return std::tolower(static_cast<unsigned char>(a)) ==
        std::tolower(static_cast<unsigned char>(b));
}
inline size_t BoundedLength(const char* text) noexcept
{
    if (!text) return 8192;
    size_t n = 0;
    while (n < 8192 && text[n]) ++n;
    return n;
}
inline bool Matches(std::string_view key, const char* path) noexcept
{
    if (key.empty()) return false;
    const size_t n = BoundedLength(path);
    if (n != key.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if (!Same(path[i], key[i])) return false;
    return true;
}
inline bool Matches(const char* path) noexcept { return Matches(TargetKey(), path); }
inline bool MatchesJoined(std::string_view key, const char* prefix, const char* member) noexcept
{
    if (key.empty()) return false;
    const size_t p = BoundedLength(prefix), m = BoundedLength(member);
    if (p == 8192 || m == 8192 || p + m != key.size()) return false;
    for (size_t i = 0; i < p; ++i)
        if (!Same(prefix[i], key[i])) return false;
    for (size_t i = 0; i < m; ++i)
        if (!Same(member[i], key[p + i])) return false;
    return true;
}
inline bool MatchesJoined(const char* prefix, const char* member) noexcept
{ return MatchesJoined(TargetKey(), prefix, member); }
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
