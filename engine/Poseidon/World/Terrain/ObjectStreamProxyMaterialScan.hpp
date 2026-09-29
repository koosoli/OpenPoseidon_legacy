#pragma once

#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Poseidon::Streaming
{

// A deliberately speculative source-byte scan, not a P3D parser or an IR
// dependency census. It allocates at most sixteen short names and never asks a
// compressed ODOL array to allocate from an untrusted wire count. Ordinary
// owner resolution still decides whether any candidate is actually consumed.
struct ProxyMaterialNameScan
{
    std::vector<std::string> names;
    bool overflow = false;
    bool supportedSignature = false;
    static constexpr size_t MaxSourceBytes = 4 * 1024 * 1024;
    static constexpr size_t MaxNames = 16;
    static constexpr size_t MaxNameBytes = 240;
};

inline ProxyMaterialNameScan ScanProxyMaterialNames(std::span<const char> bytes)
{
    ProxyMaterialNameScan result;
    if (bytes.size() < 8 || bytes.size() > ProxyMaterialNameScan::MaxSourceBytes)
        return result;
    const std::string_view signature(bytes.data(), 4);
    result.supportedSignature = signature == "ODOL" || signature == "MLOD";
    if (!result.supportedSignature)
        return result;

    // BIS material paths are ASCII, NUL-terminated source strings. Scan only
    // short path-character runs; arbitrary binary, huge strings and compressed
    // payloads cannot expand the scratch allocation. False positives are safe
    // because this list only drives bounded speculative preparation.
    auto pathByte = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
            c == '\\' || c == '/';
    };
    for (size_t i = 8; i < bytes.size();)
    {
        if (!pathByte(static_cast<unsigned char>(bytes[i]))) { ++i; continue; }
        const size_t start = i;
        do { ++i; } while (i < bytes.size() && pathByte(static_cast<unsigned char>(bytes[i])));
        const size_t length = i - start;
        if (length < 7 || length > ProxyMaterialNameScan::MaxNameBytes ||
            i == bytes.size() || bytes[i] != '\0') continue;
        const std::string_view token(bytes.data() + start, length);
        constexpr std::string_view suffix = ".rvmat";
        bool material = true;
        for (size_t j = 0; j < suffix.size(); ++j)
        {
            unsigned char c = static_cast<unsigned char>(token[length - suffix.size() + j]);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c != static_cast<unsigned char>(suffix[j])) { material = false; break; }
        }
        if (!material) continue;
        const auto path = Asset::VirtualPath::Parse(std::string(token));
        const auto& key = path.canonical();
        if (key.empty() || key.size() > ProxyMaterialNameScan::MaxNameBytes ||
            path.extension() != ".rvmat" || path.looksHostAbsolute()) continue;
        if (std::find(result.names.begin(), result.names.end(), key) != result.names.end()) continue;
        if (result.names.size() == ProxyMaterialNameScan::MaxNames)
        { result.overflow = true; break; }
        result.names.push_back(key);
    }
    return result;
}

} // namespace Poseidon::Streaming
