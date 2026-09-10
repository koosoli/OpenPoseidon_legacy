#pragma once

#include <cctype>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Poseidon
{
} // namespace Poseidon


namespace MissionPathLoader
{

// The legacy save/network header has fixed arrays. Keep their layout, terminate
// within the array, and let callers diagnose unsupported identifiers explicitly.
template<size_t N>
bool CopyLegacyIdentifier(char (&destination)[N], std::string_view source)
{
    static_assert(N > 0);
    const size_t count = std::min(source.size(), N - 1);
    if (count != 0)
        std::memmove(destination, source.data(), count);
    std::memset(destination + count, 0, N - count);
    return source.size() < N;
}

struct Selection
{
    std::string missionFilePath;
    std::string missionParentDirectory;
    std::string missionName;
    std::string worldName;
};

class Loader
{
public:
    static std::optional<std::string> ResolveMissionFile(const std::filesystem::path& inputPath)
    {
        std::error_code ec;
        std::filesystem::path candidate = inputPath;
        if (std::filesystem::is_directory(candidate, ec))
        {
            if (ec)
            {
                return std::nullopt;
            }
            candidate /= "mission.sqm";
        }

        candidate = candidate.lexically_normal();
        const std::string filename = NormalizeSeparators(candidate.filename().string());
        if (!EqualsIgnoreCase(filename, "mission.sqm"))
        {
            return std::nullopt;
        }

        if (!std::filesystem::exists(candidate, ec) || ec)
        {
            return std::nullopt;
        }

        return NormalizeSeparators(candidate.generic_string());
    }

    static std::optional<Selection> DescribeMissionFile(std::string_view missionFilePath)
    {
        std::string normalized = NormalizeSeparators(missionFilePath);
        const size_t missionFileSlash = normalized.rfind('/');
        if (missionFileSlash == std::string::npos)
        {
            return std::nullopt;
        }

        if (!EqualsIgnoreCase(normalized.substr(missionFileSlash + 1), "mission.sqm"))
        {
            return std::nullopt;
        }

        std::string missionDirectory = normalized.substr(0, missionFileSlash);
        const size_t missionDirectorySlash = missionDirectory.rfind('/');
        const std::string folder =
            missionDirectorySlash == std::string::npos ? missionDirectory : missionDirectory.substr(missionDirectorySlash + 1);

        const size_t dot = folder.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= folder.size())
        {
            return std::nullopt;
        }

        Selection selection;
        selection.missionFilePath = std::move(normalized);
        selection.missionParentDirectory =
            missionDirectorySlash == std::string::npos ? "" : missionDirectory.substr(0, missionDirectorySlash + 1);
        selection.missionName = folder.substr(0, dot);
        selection.worldName = folder.substr(dot + 1);
        if (selection.missionName.size() >= 80 || selection.worldName.size() >= 80)
            return std::nullopt; // Do not load a different, truncated identifier.
        return selection;
    }

private:
    static std::string NormalizeSeparators(std::string_view input)
    {
        std::string normalized(input);
        for (char& ch : normalized)
        {
            if (ch == '\\')
            {
                ch = '/';
            }
        }
        return normalized;
    }

    static bool EqualsIgnoreCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (size_t i = 0; i < left.size(); ++i)
        {
            const unsigned char lhs = static_cast<unsigned char>(left[i]);
            const unsigned char rhs = static_cast<unsigned char>(right[i]);
            if (std::tolower(lhs) != std::tolower(rhs))
            {
                return false;
            }
        }
        return true;
    }
};

} // namespace MissionPathLoader
