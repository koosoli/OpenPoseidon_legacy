// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-010 — following an Enfusion prefab to the mesh it actually draws.
//
// Moved verbatim out of `apps/tools/Tools/commands/XobCommand.cpp`, where it was
// the asset CLI's private business, because the native world loader needs the same
// walk and a second copy is how two answers to one question start to differ. The
// tool now calls this; it does not keep a fork.
//
// A placement names a PREFAB, not a mesh, and the prefab may declare no mesh at all
// -- Everon's most-placed prefab, 71,319 instances, overrides two materials and
// inherits its `Object` from a parent. Reading only the placed `.et` therefore
// finds nothing for the commonest object on the island.
//
// No GUID map is involved here: a parent reference carries `{GUID}path` with the
// path inline, so the map is needed exactly once, to turn a placement's prefab GUID
// into the first `.et` path.

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{

namespace PrefabDetail
{
inline std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
} // namespace PrefabDetail

//! The `.xob` a prefab body names, or empty.
//!
//! Keyed on the exact property name `Object`. `m_sPhaseModel` inside a
//! `SCR_BaseDestructionPhase` block names a `.xob` too -- the stump a tree becomes
//! when it is destroyed -- so a scan for "any .xob on any line" picks that one up on
//! every destructible prefab in the corpus.
//!
//! A line scanner rather than `ParseEmat` on purpose: the mesh sits two blocks deep
//! (`components { MeshObject "{..}" { Object "{..}path.xob" } }`) and the property
//! parser folds sub-level tokens into the enclosing property's value list, so
//! `Object` stops being a key.
inline std::string FindObjectReference(const std::vector<uint8_t>& blob)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    size_t at = 0;
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != '\n')
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;
        if (cursor + 7 <= lineEnd && std::memcmp(data + cursor, "Object", 6) == 0 &&
            static_cast<unsigned char>(data[cursor + 6]) <= ' ')
        {
            const size_t open = std::string_view(data + cursor, lineEnd - cursor).find('"');
            if (open != std::string_view::npos)
            {
                const size_t begin = cursor + open + 1;
                size_t end = begin;
                while (end < lineEnd && data[end] != '"')
                    ++end;
                std::string guid, path;
                Poseidon::Asset::Material::Detail::SplitGuid(std::string(data + begin, end - begin), guid, path);
                if (PrefabDetail::EndsWith(PrefabDetail::LowerCopy(path), ".xob"))
                    return path;
            }
        }
        at = lineEnd + 1;
    }
    return {};
}

struct PrefabResolution
{
    std::string xobPath;
    std::string via;    //!< the `.et` the mesh was finally found in
    int depth = 0;      //!< 0 when the placed prefab carried its own mesh
    std::string reason; //!< empty on success; names what failed
};

//! Follows a prefab's inheritance chain until a `MeshObject`'s `Object` appears.
//!
//! `depthLimit` is 8 for the same reason `ResolveEmatInheritance` uses 8: it bounds
//! a cycle in authored data rather than expressing an expected depth. Everon's top
//! 25 prefabs resolve at depth 0 or 1.
inline PrefabResolution ResolvePrefabModel(const EnfusionMount& mount, const std::string& etPath, int depthLimit = 8)
{
    PrefabResolution out;
    std::string current = etPath;
    std::vector<std::string> seen;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        for (const std::string& before : seen)
        {
            if (PrefabDetail::LowerCopy(before) == PrefabDetail::LowerCopy(current))
            {
                out.reason = "prefab inheritance cycle at " + current;
                return out;
            }
        }
        seen.push_back(current);

        std::vector<uint8_t> blob;
        if (!mount.Read(current, blob))
        {
            out.reason = "prefab not in any .pak: " + current;
            return out;
        }
        const std::string object = FindObjectReference(blob);
        if (!object.empty())
        {
            out.xobPath = object;
            out.via = current;
            out.depth = depth;
            return out;
        }
        const auto parsed = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (parsed.parentPath.empty())
        {
            out.reason = parsed.valid() ? "prefab declares no MeshObject and no parent: " + current
                                        : "prefab header unreadable (" + parsed.error + "): " + current;
            return out;
        }
        current = parsed.parentPath;
    }
    out.reason = "prefab inheritance deeper than " + std::to_string(depthLimit) + " from " + etPath;
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
