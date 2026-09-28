#pragma once

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>

#include <string>

namespace Poseidon
{

// NTC alpha is authored cavity data. BC7 keeps it in the compressed upload, and
// the decoded DDS path keeps it in ARGB8888. Other decoded texture sources may
// repack alpha for legacy normals, so they must not acquire this flag by name.
inline bool NativeNtcCavityChannelAvailable(PacFormat uploadedFormat, bool decodedDds,
                                             bool normalRgPacked, const char* name)
{
    if (!normalRgPacked ||
        (uploadedFormat != PacBC7 && !(uploadedFormat == PacARGB8888 && decodedDds)) ||
        name == nullptr)
        return false;

    std::string lower(name);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const size_t dot = lower.find_last_of('.');
    const size_t end = dot == std::string::npos ? lower.size() : dot;
    return end >= 4 && lower.compare(end - 4, 4, "_ntc") == 0;
}

// NMO's B/A lanes are metalness/ambient occlusion only when the original four-
// channel DDS survived upload. BC5 supplies synthetic B=0/A=1, and a converted
// PAA can reuse the suffix without preserving Enfusion's auxiliary channels.
inline bool NativeNmoMaterialChannelsAvailable(PacFormat uploadedFormat, bool decodedDds,
                                               bool normalRgPacked, const char* name)
{
    if (!normalRgPacked ||
        (uploadedFormat != PacBC7 && !(uploadedFormat == PacARGB8888 && decodedDds)) ||
        name == nullptr)
        return false;

    std::string lower(name);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    constexpr char suffix[] = "_nmo.edds";
    return lower.size() >= sizeof(suffix) - 1 &&
           lower.compare(lower.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0;
}

} // namespace Poseidon
