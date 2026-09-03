// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace Poseidon::Asset::Formats::Enfusion
{
namespace
{
std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

EnfusionMount& EnfusionMount::Instance()
{
    static EnfusionMount mount;
    return mount;
}

void EnfusionMount::Close()
{
    _archives.clear();
    _index.clear();
    _root.clear();
}

bool EnfusionMount::Open(const std::string& addonsDir)
{
    Close();

    std::vector<std::string> paks;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(addonsDir, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        if (Lower(it->path().extension().string()) == ".pak")
            paks.push_back(it->path().string());
    }
    if (paks.empty())
    {
        LOG_WARN(World, "Enfusion mount: no .pak archives under '{}'", addonsDir);
        return false;
    }

    // Sorted so the index is built in a stable order: when two archives declare the
    // same virtual path, which one wins must not depend on directory iteration
    // order, or the same install resolves differently on two machines.
    std::sort(paks.begin(), paks.end());

    _archives.resize(paks.size());
    size_t opened = 0;
    for (size_t i = 0; i < paks.size(); ++i)
    {
        if (!_archives[i].Open(paks[i]))
        {
            LOG_WARN(World, "Enfusion mount: cannot open '{}': {}", paks[i], _archives[i].Error());
            continue;
        }
        ++opened;
        const std::vector<PakEntry>& entries = _archives[i].Entries();
        for (size_t e = 0; e < entries.size(); ++e)
        {
            // First writer wins, matching the sort above: a later archive does not
            // silently shadow an earlier one.
            _index.emplace(Lower(entries[e].path),
                           std::make_pair(static_cast<uint32_t>(i), static_cast<uint32_t>(e)));
        }
    }
    if (opened == 0)
    {
        Close();
        return false;
    }
    _root = addonsDir;
    LOG_INFO(World, "Enfusion mount: {} of {} archives, {} entries, from '{}'", opened, paks.size(), _index.size(),
             addonsDir);
    return true;
}

bool EnfusionMount::Has(const std::string& virtualPath) const
{
    return _index.find(Lower(virtualPath)) != _index.end();
}

bool EnfusionMount::Read(const std::string& virtualPath, std::vector<uint8_t>& out) const
{
    const auto hit = _index.find(Lower(virtualPath));
    if (hit == _index.end())
        return false;
    const PakArchive& archive = _archives[hit->second.first];
    return archive.Read(archive.Entries()[hit->second.second], out);
}

std::vector<std::string> EnfusionMount::FindAll(const std::string& needle) const
{
    const std::string want = Lower(needle);
    std::vector<std::string> out;
    for (const auto& [path, where] : _index)
        if (path.find(want) != std::string::npos)
            out.push_back(path);
    // The map's order is a hash order, which would make "the first tile" mean
    // something different on every run.
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
