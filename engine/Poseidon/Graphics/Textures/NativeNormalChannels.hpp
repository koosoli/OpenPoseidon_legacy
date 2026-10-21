#pragma once

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>

#include <string>

namespace Poseidon
{

// The upload half shared by both answers below: the original four-channel data survives
// only in BC7 or a decoded DDS, and only an RG-packed normal has B/A data lanes at all.
inline bool NativeAuxChannelsSurvive(PacFormat uploadedFormat, bool decodedDds, bool normalRgPacked)
{
    return normalRgPacked && (uploadedFormat == PacBC7 || (uploadedFormat == PacARGB8888 && decodedDds));
}

// The name half of NativeNtcCavityChannelAvailable. Split out so a caller asking per frame
// can memoise it (TextureWgpu::NameIs); the name of a texture never changes.
inline bool IsNativeNtcName(const char* name)
{
    if (name == nullptr)
        return false;
    std::string lower(name);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const size_t dot = lower.find_last_of('.');
    const size_t end = dot == std::string::npos ? lower.size() : dot;
    return end >= 4 && lower.compare(end - 4, 4, "_ntc") == 0;
}

// NTC alpha is authored cavity data. BC7 keeps it in the compressed upload, and
// the decoded DDS path keeps it in ARGB8888. Other decoded texture sources may
// repack alpha for legacy normals, so they must not acquire this flag by name.
inline bool NativeNtcCavityChannelAvailable(PacFormat uploadedFormat, bool decodedDds, bool normalRgPacked,
                                            const char* name)
{
    return NativeAuxChannelsSurvive(uploadedFormat, decodedDds, normalRgPacked) && IsNativeNtcName(name);
}

// NMO's B/A lanes are metalness/ambient occlusion only when the original four-
// channel DDS survived upload. BC5 supplies synthetic B=0/A=1, and a converted
// PAA can reuse the suffix without preserving Enfusion's auxiliary channels.
inline bool IsNativeNmoName(const char* name)
{
    if (name == nullptr)
        return false;
    std::string lower(name);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    constexpr char suffix[] = "_nmo.edds";
    return lower.size() >= sizeof(suffix) - 1 &&
           lower.compare(lower.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0;
}

inline bool NativeNmoMaterialChannelsAvailable(PacFormat uploadedFormat, bool decodedDds, bool normalRgPacked,
                                               const char* name)
{
    return NativeAuxChannelsSurvive(uploadedFormat, decodedDds, normalRgPacked) && IsNativeNmoName(name);
}

} // namespace Poseidon
