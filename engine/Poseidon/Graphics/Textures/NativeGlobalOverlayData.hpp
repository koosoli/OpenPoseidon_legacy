#pragma once

#include <string>

namespace Poseidon
{
// A preparation policy, captured alongside the other DDS options. An empty
// suffix preserves the existing coverage-alpha interpretation.
inline std::string NativeGlobalOverlayDataSuffix(bool pbr, bool ordered, bool srgb,
                                                  bool overlay, const char* material)
{
    if (!pbr || !ordered || !srgb || !overlay) return {};
    std::string name(material && *material ? material : "Pub_Village_E_01_L_Ext.emat");
    if (name.size() > 1024 || name.find_first_of("|\r\n") != std::string::npos) return {};
    for (char& c : name)
    {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '\\') c = '/';
    }
    constexpr char extension[] = ".emat";
    if (name.size() <= 5 || name.compare(name.size() - 5, 5, extension) != 0) return {};
    name.resize(name.size() - 5);
    return name + "_mcr.edds";
}
inline bool NativeGlobalOverlayDataName(const char* texture, const std::string& suffix)
{
    if (!texture || suffix.empty()) return false;
    std::string name(texture);
    if (name.size() > 4096 || name.find('|') != std::string::npos) return false;
    for (char& c : name)
    {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '\\') c = '/';
    }
    if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    return name.size() == suffix.size() || name[name.size() - suffix.size() - 1] == '/';
}
} // namespace Poseidon
