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
#include <utility>
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

//! The `.emat` a prefab body names under `Material`, or empty.
//!
//! RFG-027: a `RoadEntity`'s surface is usually not on the entity. 2,200 of Everon's
//! 2,370 roads carry a prefab reference and only 183 carry a `Material` GUID of their
//! own, so the tarmac is a property of `Prefabs/Roads/Road_Asphalt_E_01.et` and its
//! kin. The reference is written in full -- `Material "{GUID}Assets/....emat"` -- so
//! the path comes straight out of the quotes and no GUID lookup is needed.
//!
//! Same line-scanner shape as FindObjectReference and for the same reason: the
//! property parser folds sub-level tokens into the enclosing property's value list.
inline std::string FindMaterialReference(const std::vector<uint8_t>& blob)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    size_t at = 0;
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != 0x0A)
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;
        if (cursor + 9 <= lineEnd && std::memcmp(data + cursor, "Material", 8) == 0 &&
            static_cast<unsigned char>(data[cursor + 8]) <= ' ')
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
                if (PrefabDetail::EndsWith(PrefabDetail::LowerCopy(path), ".emat"))
                    return path;
            }
        }
        at = lineEnd + 1;
    }
    return {};
}

//! Follows a prefab's inheritance chain until a `Material` appears.
inline std::string ResolvePrefabMaterial(const EnfusionMount& mount, const std::string& etPath, int depthLimit = 8)
{
    std::string current = etPath;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        std::vector<uint8_t> blob;
        if (!mount.Read(current, blob))
            return {};
        const std::string material = FindMaterialReference(blob);
        if (!material.empty())
            return material;
        const auto parsed = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (parsed.parentPath.empty())
            return {};
        current = parsed.parentPath;
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

// ---------------------------------------------------------------------------
// RFG-064 -- the parts a prefab hangs off its mesh's bones.
//
// A Reforger house is not one model. The `.xob` carries named socket bones and the
// prefab attaches a SEPARATE prefab to each of them, so a reader that returns only
// the first `Object` draws the shell and nothing else: Everon's houses came out
// with holes where their doors, gates and windows belong.
//
// Two declarations do this, and both are needed -- measured over the 19,448 `.et`
// entries of the local Reforger corpus:
//
//   * a CHILD ENTITY carrying `Hierarchy { PivotID "<bone>" }`, naming one exact
//     bone -- 3,099 files, 25,368 entries;
//   * `SlotBoneMappings { SlotBoneMappingObject { BonePrefix "..." Prefab "..." } }`,
//     a rule over a bone-name PREFIX -- 450 files, 1,650 entries.
//
// They are not redundant. 2,902 files carry pivots and no slot block; 253 carry a
// slot block and no pivots. Where both appear the pivots are the more specific
// answer -- on `FarmHouse_E_1L01_Base.et` the slot rule says every
// `socket_door_ext_left*` takes `DoorSet_Village_E_01_L_EXT_COV_B_brown`, while the
// pivots give socket 01 `..._EXT_ST_M1` and socket 02 `..._EXT_ST_B`. So the exact
// bone wins and the prefix rule only fills bones nothing claimed. 61 of the 450
// slot files declare EMPTY `SlotBoneMappingObject` blocks and carry their real
// attachments as pivots only, which is the case a prefix-only reader loses whole.
//
// Inheritance is a union, child first: of the 450 slot-declaring files 98 have an
// ancestor that also declares slots, and across those the child repeats 77 entries
// verbatim, OVERRIDES 5, and ADDS 37. Overriding is real but rare, so "first seen
// down the chain wins, keep walking for the rest" is the rule -- and the pivots
// need it just as badly, because a placed `.et` is usually a stub whose whole
// content is inherited (`FarmHouse_E_1L01.et` is 362 bytes and declares no
// attachment at all; all 25 of its sockets come from its parent).

struct PrefabSlot
{
    std::string bone;   //!< lower-cased exact bone name; empty when this is a prefix rule
    std::string prefix; //!< lower-cased bone-name prefix; empty when `bone` is set
    std::string prefab; //!< the `.et` of the part to attach
};

struct PrefabSlots
{
    std::vector<PrefabSlot> exact;    //!< PivotID children, one exact bone each
    std::vector<PrefabSlot> prefixes; //!< SlotBoneMappings rules
    int depth = 0;                    //!< how far up the chain the last entry came from
    std::string reason;               //!< empty on success

    bool empty() const { return exact.empty() && prefixes.empty(); }
};

namespace PrefabDetail
{
//! The quoted string after the first `"` on a line, GUID stripped.
inline bool QuotedReference(const char* data, size_t begin, size_t end, std::string& path)
{
    const std::string_view line(data + begin, end - begin);
    const size_t open = line.find('"');
    if (open == std::string_view::npos)
        return false;
    const size_t at = begin + open + 1;
    size_t close = at;
    while (close < end && data[close] != '"')
        ++close;
    std::string guid;
    Poseidon::Asset::Material::Detail::SplitGuid(std::string(data + at, close - at), guid, path);
    return true;
}

//! A line beginning `Ident :` or `$grp Ident :` followed by a quoted `.et`.
inline bool ChildHeader(const char* data, size_t begin, size_t end, std::string& path)
{
    size_t at = begin;
    if (at + 4 < end && std::memcmp(data + at, "$grp", 4) == 0)
    {
        at += 4;
        while (at < end && static_cast<unsigned char>(data[at]) <= ' ')
            ++at;
    }
    const size_t wordStart = at;
    while (at < end && (std::isalnum(static_cast<unsigned char>(data[at])) != 0 || data[at] == '_'))
        ++at;
    if (at == wordStart)
        return false;
    while (at < end && static_cast<unsigned char>(data[at]) <= ' ')
        ++at;
    if (at >= end || data[at] != ':')
        return false;
    if (!QuotedReference(data, at, end, path))
        return false;
    return EndsWith(LowerCopy(path), ".et");
}

//! Braces outside quotes, which is what the depth walk must count. The only braces
//! inside a quote are a reference's `{GUID}` and those balance -- but counting them
//! would still be counting the wrong thing.
inline int NetBraces(const char* data, size_t begin, size_t end)
{
    int net = 0;
    bool quoted = false;
    for (size_t at = begin; at < end; ++at)
    {
        if (data[at] == '"')
            quoted = !quoted;
        else if (!quoted && data[at] == '{')
            ++net;
        else if (!quoted && data[at] == '}')
            --net;
    }
    return net;
}

inline bool Keyword(const char* data, size_t begin, size_t end, const char* key)
{
    const size_t n = std::strlen(key);
    return begin + n < end && std::memcmp(data + begin, key, n) == 0 &&
           static_cast<unsigned char>(data[begin + n]) <= ' ';
}
} // namespace PrefabDetail

//! One `.et` body's own slot declarations, in file order.
inline void FindSlotDeclarations(const std::vector<uint8_t>& blob, std::vector<PrefabSlot>& exact,
                                 std::vector<PrefabSlot>& prefixes)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();

    // (body depth, prefab path) for every enclosing child-entity block. The FILE's
    // own header sits at depth 0 and names its PARENT, not a child, so it is never
    // pushed: attributing a pivot to it would hang the house's own doors off the
    // house's base prefab.
    std::vector<std::pair<int, std::string>> stack;
    std::string pendingPrefix;
    int depth = 0;
    size_t at = 0;
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != '\n')
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;

        std::string quoted;
        if (depth >= 1 && PrefabDetail::ChildHeader(data, cursor, lineEnd, quoted))
        {
            stack.emplace_back(depth, quoted);
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "PivotID"))
        {
            if (!stack.empty() && PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted) && !quoted.empty())
                exact.push_back({PrefabDetail::LowerCopy(quoted), std::string(), stack.back().second});
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "BonePrefix"))
        {
            if (PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted))
                pendingPrefix = PrefabDetail::LowerCopy(quoted);
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "Prefab") && !pendingPrefix.empty())
        {
            if (PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted) && !quoted.empty())
                prefixes.push_back({std::string(), pendingPrefix, quoted});
            pendingPrefix.clear();
        }

        depth += PrefabDetail::NetBraces(data, cursor, lineEnd);
        while (!stack.empty() && depth <= stack.back().first)
            stack.pop_back();
        at = lineEnd + 1;
    }
}

//! Every slot a prefab declares or inherits. Child first; ancestors only add.
inline PrefabSlots ResolvePrefabSlots(const EnfusionMount& mount, const std::string& etPath, int depthLimit = 8)
{
    PrefabSlots out;
    std::string current = etPath;
    std::vector<std::string> seen;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        for (const std::string& before : seen)
            if (PrefabDetail::LowerCopy(before) == PrefabDetail::LowerCopy(current))
                return out;
        seen.push_back(current);

        std::vector<uint8_t> blob;
        if (!mount.Read(current, blob))
        {
            if (out.empty())
                out.reason = "prefab not in any .pak: " + current;
            return out;
        }
        std::vector<PrefabSlot> exact, prefixes;
        FindSlotDeclarations(blob, exact, prefixes);
        for (const PrefabSlot& slot : exact)
        {
            bool known = false;
            for (const PrefabSlot& have : out.exact)
                known = known || have.bone == slot.bone;
            if (!known)
            {
                out.exact.push_back(slot);
                out.depth = depth;
            }
        }
        for (const PrefabSlot& slot : prefixes)
        {
            bool known = false;
            for (const PrefabSlot& have : out.prefixes)
                known = known || have.prefix == slot.prefix;
            if (!known)
            {
                out.prefixes.push_back(slot);
                out.depth = depth;
            }
        }

        const auto parsed = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (parsed.parentPath.empty())
            return out;
        current = parsed.parentPath;
    }
    return out;
}

//! The part a bone takes, or empty. Exact pivot first, then the LONGEST matching
//! prefix -- `socket_win_130x142` and `socket_win_130x72` share nine characters, and
//! a shortest-match rule would hang the wrong window on half a village.
inline std::string SlotPrefabForBone(const PrefabSlots& slots, const std::string& boneName)
{
    const std::string lower = PrefabDetail::LowerCopy(boneName);
    for (const PrefabSlot& slot : slots.exact)
        if (slot.bone == lower)
            return slot.prefab;
    const PrefabSlot* best = nullptr;
    for (const PrefabSlot& slot : slots.prefixes)
    {
        if (slot.prefix.empty() || lower.size() < slot.prefix.size() ||
            lower.compare(0, slot.prefix.size(), slot.prefix) != 0)
            continue;
        if (best == nullptr || slot.prefix.size() > best->prefix.size())
            best = &slot;
    }
    return best != nullptr ? best->prefab : std::string();
}

} // namespace Poseidon::Asset::Formats::Enfusion
