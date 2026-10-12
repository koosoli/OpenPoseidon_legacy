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
#include <cstdlib>
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

//! RFG-114: the prefab's `placement` mode (`slopelandcontact`, `none`, ...), lowercase, from the
//! first file down the inheritance chain that declares one at the ENTITY's top level (one
//! leading space -- nested component blocks are indented further). Empty when none does.
//! 205 of Reforger's wall/fence prefabs declare `slopelandcontact`, 8 `none`; a fence run in
//! Eden.ent stores pitch 0 / roll 0 and stepped heights, so the tilt is the runtime's job.
inline std::string ResolvePrefabPlacement(const EnfusionMount& mount, const std::string& etPath, int depthLimit = 8)
{
    std::string current = etPath;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        std::vector<uint8_t> blob;
        if (!mount.Read(current, blob))
            return {};
        const char* data = reinterpret_cast<const char*>(blob.data());
        const size_t size = blob.size();
        size_t at = 0;
        while (at < size)
        {
            size_t lineEnd = at;
            while (lineEnd < size && data[lineEnd] != 0x0A)
                ++lineEnd;
            static constexpr char key[] = " placement ";
            const size_t keyLen = sizeof(key) - 1;
            if (lineEnd - at > keyLen && std::memcmp(data + at, key, keyLen) == 0)
            {
                size_t b = at + keyLen, e = b;
                while (e < lineEnd && data[e] != ' ' && data[e] != 0x0D && data[e] != 0x09)
                    ++e;
                return PrefabDetail::LowerCopy(std::string(data + b, e - b));
            }
            at = lineEnd + 1;
        }
        const auto parsed = Poseidon::Asset::Material::ParseEmat(std::string_view(data, size));
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
    //! RFG-121: `MeshObject { Materials { MaterialAssignClass { SourceMaterial "<slot>"
    //! AssignedMaterial "<.emat>" } } }` -- the prefab's own colour variant of a shared model
    //! (CarpetFrame_01_green over a model whose slot `CarpetFrame_01` defaults to Red; door,
    //! window-frame and roof variants the same way). Slot name -> `.emat`, child first.
    std::vector<std::pair<std::string, std::string>> materialRemap;

    //! The mesh key the loader lists: the `.xob` itself, or `enfm|slot=emat;...|<xob>` when the
    //! prefab remaps materials, so each colour variant is its own model and the key still ends
    //! in `.xob` (the object stream recognises native models by that suffix).
    std::string MeshKey() const;
};

//! Splits a mesh key from PrefabResolution::MeshKey back into the `.xob` and its remap.
inline std::string SplitRemappedMeshKey(const std::string& key,
                                        std::vector<std::pair<std::string, std::string>>* remap = nullptr)
{
    if (key.compare(0, 5, "enfm|") != 0)
        return key;
    const size_t bar = key.find('|', 5);
    if (bar == std::string::npos)
        return key;
    if (remap != nullptr)
    {
        const std::string spec = key.substr(5, bar - 5);
        size_t at = 0;
        while (at < spec.size())
        {
            size_t semi = spec.find(';', at);
            if (semi == std::string::npos)
                semi = spec.size();
            const std::string entry = spec.substr(at, semi - at);
            const size_t eq = entry.find('=');
            if (eq != std::string::npos && eq > 0)
                remap->emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            at = semi + 1;
        }
    }
    return key.substr(bar + 1);
}

inline std::string PrefabResolution::MeshKey() const
{
    if (materialRemap.empty() || xobPath.empty())
        return xobPath;
    std::string key = "enfm|";
    for (size_t i = 0; i < materialRemap.size(); ++i)
        key += (i ? ";" : "") + materialRemap[i].first + "=" + materialRemap[i].second;
    return key + "|" + xobPath;
}

//! RFG-121: the root entity's own MaterialAssignClass pairs in one `.et` body (defined below,
//! after the line helpers it uses).
inline void FindMaterialAssignments(const std::vector<uint8_t>& blob,
                                    std::vector<std::pair<std::string, std::string>>& out);

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
        // RFG-121: the variant's material assignments, child first (the first file that names
        // a slot decides it). POSEIDON_PREFAB_MATERIALS=0 ignores them (every variant draws the
        // model's default materials, the pre-RFG-121 look).
        {
            static const bool remapOn = []
            {
                const char* env = std::getenv("POSEIDON_PREFAB_MATERIALS");
                return !(env && env[0] == '0');
            }();
            if (remapOn)
            {
                std::vector<std::pair<std::string, std::string>> assigned;
                FindMaterialAssignments(blob, assigned);
                for (auto& pair : assigned)
                {
                    bool known = false;
                    for (const auto& have : out.materialRemap)
                        known = known || PrefabDetail::LowerCopy(have.first) == PrefabDetail::LowerCopy(pair.first);
                    if (!known)
                        out.materialRemap.push_back(std::move(pair));
                }
            }
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
    //! RFG-128: MaterialAssignClass pairs written inside this child's own block -- the parent's
    //! override of the part (a door's glass child: window_glass -> window_glass_uneven_INT).
    //! They outrank the part prefab's own assignments.
    std::vector<std::pair<std::string, std::string>> materialRemap;
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

//! RFG-121: the root entity's own MaterialAssignClass pairs in one `.et` body (child entities'
//! assignments belong to them and are skipped).
inline void FindMaterialAssignments(const std::vector<uint8_t>& blob,
                                    std::vector<std::pair<std::string, std::string>>& out)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    int depth = 0;
    int childDepth = -1; // >= 0 while inside a child entity declared at that depth
    std::string pendingSource;
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
        if (childDepth < 0 && depth >= 1)
        {
            // A child entity header: `Class : "x.et" {` (MaterialAssignClass has no colon).
            const size_t colon = std::string_view(data + cursor, lineEnd - cursor).find(" : ");
            if (colon != std::string_view::npos && PrefabDetail::ChildHeader(data, cursor, lineEnd, quoted))
                childDepth = depth;
            else if (PrefabDetail::Keyword(data, cursor, lineEnd, "SourceMaterial"))
            {
                std::string source;
                if (PrefabDetail::QuotedReference(data, cursor, lineEnd, source))
                    pendingSource = source;
            }
            else if (PrefabDetail::Keyword(data, cursor, lineEnd, "AssignedMaterial") && !pendingSource.empty())
            {
                std::string assigned;
                if (PrefabDetail::QuotedReference(data, cursor, lineEnd, assigned) && !assigned.empty())
                    out.emplace_back(pendingSource, assigned);
                pendingSource.clear();
            }
        }
        depth += PrefabDetail::NetBraces(data, cursor, lineEnd);
        if (childDepth >= 0 && depth <= childDepth)
            childDepth = -1;
        at = lineEnd + 1;
    }
}

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
    struct ChildFrame
    {
        int depth = 0;
        std::string prefab;
        std::vector<std::pair<std::string, std::string>> remap; //!< RFG-128: its own assignments
        std::string pendingSource;
        int slot = -1; //!< index into `exact` once its PivotID is read
    };
    std::vector<ChildFrame> stack;
    std::string pendingPrefix;
    int depth = 0;
    size_t at = 0;
    const auto popFrame = [&]()
    {
        ChildFrame& frame = stack.back();
        if (frame.slot >= 0 && static_cast<size_t>(frame.slot) < exact.size())
            exact[static_cast<size_t>(frame.slot)].materialRemap = std::move(frame.remap);
        stack.pop_back();
    };
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
            ChildFrame frame;
            frame.depth = depth;
            frame.prefab = quoted;
            stack.push_back(std::move(frame));
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "PivotID"))
        {
            if (!stack.empty() && PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted) && !quoted.empty())
            {
                exact.push_back({PrefabDetail::LowerCopy(quoted), std::string(), stack.back().prefab, {}});
                stack.back().slot = static_cast<int>(exact.size()) - 1;
            }
        }
        else if (!stack.empty() && PrefabDetail::Keyword(data, cursor, lineEnd, "SourceMaterial"))
        {
            PrefabDetail::QuotedReference(data, cursor, lineEnd, stack.back().pendingSource);
        }
        else if (!stack.empty() && PrefabDetail::Keyword(data, cursor, lineEnd, "AssignedMaterial"))
        {
            ChildFrame& frame = stack.back();
            if (!frame.pendingSource.empty() && PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted) &&
                !quoted.empty())
                frame.remap.emplace_back(frame.pendingSource, quoted);
            frame.pendingSource.clear();
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "BonePrefix"))
        {
            if (PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted))
                pendingPrefix = PrefabDetail::LowerCopy(quoted);
        }
        else if (PrefabDetail::Keyword(data, cursor, lineEnd, "Prefab") && !pendingPrefix.empty())
        {
            if (PrefabDetail::QuotedReference(data, cursor, lineEnd, quoted) && !quoted.empty())
                prefixes.push_back({std::string(), pendingPrefix, quoted, {}});
            pendingPrefix.clear();
        }

        depth += PrefabDetail::NetBraces(data, cursor, lineEnd);
        while (!stack.empty() && depth <= stack.back().depth)
            popFrame();
        at = lineEnd + 1;
    }
    while (!stack.empty())
        popFrame();
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
inline const PrefabSlot* SlotForBone(const PrefabSlots& slots, const std::string& boneName)
{
    const std::string lower = PrefabDetail::LowerCopy(boneName);
    for (const PrefabSlot& slot : slots.exact)
        if (slot.bone == lower)
            return &slot;
    const PrefabSlot* best = nullptr;
    for (const PrefabSlot& slot : slots.prefixes)
    {
        if (slot.prefix.empty() || lower.size() < slot.prefix.size() ||
            lower.compare(0, slot.prefix.size(), slot.prefix) != 0)
            continue;
        if (best == nullptr || slot.prefix.size() > best->prefix.size())
            best = &slot;
    }
    return best;
}

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

// RFG-120: the children a prefab places by TRANSFORM rather than on a socket bone.
//
// A Reforger house's furniture is not in the world file and not on a bone: the building
// prefab carries `GenericEntity : ".../Villa_E_2I01_furniture_01.et" { coords 0 0 0 }`, and
// that composition is in turn 38 entries of `Class : "<prop>.et" { ID coords angles scale }`
// and `$grp Class : "<prop>.et" { { ID coords ... } { ... } }`, every transform LOCAL to its
// parent. The slot reader above only follows PivotID / BonePrefix, so every room came out
// bare. Children that carry a PivotID stay the slot reader's (they hang on a bone).
struct PrefabChild
{
    std::string prefab; //!< the child's `.et`
    std::string id;     //!< its `ID`, the key a derived prefab overrides it by
    float position[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f}; //!< Enfusion order: pitch, yaw, roll (degrees)
    float scale = 1.0f;
};

namespace PrefabDetail
{
inline bool Floats(const char* data, size_t begin, size_t end, float* out, int count)
{
    const std::string text(data + begin, end - begin);
    const char* p = text.c_str();
    for (int i = 0; i < count; ++i)
    {
        char* next = nullptr;
        const float v = std::strtof(p, &next);
        if (next == p)
            return false;
        out[i] = v;
        p = next;
    }
    return true;
}
} // namespace PrefabDetail

//! One `.et` body's own transform-placed children, in file order.
inline void FindChildPlacements(const std::vector<uint8_t>& blob, std::vector<PrefabChild>& out)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    struct Frame
    {
        int depth = 0;       //!< brace depth of the header line
        bool group = false;  //!< `$grp`: its anonymous blocks are the instances
        bool open = false;   //!< a group instance is being read
        bool pivot = false;  //!< the current entry hangs on a bone: not ours
        bool nested = false; //!< declared inside another child: its transform is relative to that one
        PrefabChild entry;
    };
    std::vector<Frame> stack;
    int depth = 0;
    size_t at = 0;
    const auto flush = [&](Frame& frame)
    {
        // Nested entries are tracked so their PivotID / coords cannot leak onto the outer
        // entry, but not emitted: the outer child's own `.et` is expanded recursively by the
        // caller, which is where its children belong.
        if (!frame.pivot && !frame.nested && !frame.entry.prefab.empty())
            out.push_back(frame.entry);
        frame.pivot = false;
        const std::string prefab = frame.entry.prefab;
        frame.entry = PrefabChild{};
        frame.entry.prefab = prefab;
    };
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != '\n')
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;
        size_t trimmedEnd = lineEnd;
        while (trimmedEnd > cursor && static_cast<unsigned char>(data[trimmedEnd - 1]) <= ' ')
            --trimmedEnd;

        std::string quoted;
        if (depth >= 1 && PrefabDetail::ChildHeader(data, cursor, lineEnd, quoted))
        {
            Frame frame;
            frame.depth = depth;
            frame.group = lineEnd - cursor > 4 && std::memcmp(data + cursor, "$grp", 4) == 0;
            frame.nested = !stack.empty();
            frame.entry.prefab = quoted;
            stack.push_back(frame);
        }
        else if (!stack.empty())
        {
            Frame& top = stack.back();
            const bool bareOpen = trimmedEnd == cursor + 1 && data[cursor] == '{';
            if (top.group && !top.open && bareOpen && depth == top.depth + 1)
                top.open = true;
            const int bodyDepth = top.group ? top.depth + 2 : top.depth + 1;
            const bool ours = (!top.group || top.open) && depth == bodyDepth;
            if (PrefabDetail::Keyword(data, cursor, lineEnd, "PivotID") && depth >= bodyDepth)
                top.pivot = true;
            else if (ours && PrefabDetail::Keyword(data, cursor, lineEnd, "coords"))
                PrefabDetail::Floats(data, cursor + 6, lineEnd, top.entry.position, 3);
            else if (ours && PrefabDetail::Keyword(data, cursor, lineEnd, "angles"))
                PrefabDetail::Floats(data, cursor + 6, lineEnd, top.entry.angles, 3);
            else if (ours && PrefabDetail::Keyword(data, cursor, lineEnd, "scale"))
                PrefabDetail::Floats(data, cursor + 5, lineEnd, &top.entry.scale, 1);
            else if (ours && PrefabDetail::Keyword(data, cursor, lineEnd, "ID"))
                PrefabDetail::QuotedReference(data, cursor, lineEnd, top.entry.id);
        }

        depth += PrefabDetail::NetBraces(data, cursor, lineEnd);
        while (!stack.empty())
        {
            Frame& top = stack.back();
            if (top.group && top.open && depth <= top.depth + 1)
            {
                flush(top);
                top.open = false;
            }
            if (depth > top.depth)
                break;
            if (!top.group)
                flush(top);
            stack.pop_back();
        }
        at = lineEnd + 1;
    }
}

//! Every transform-placed child a prefab declares or inherits. Child first; an ancestor's
//! entry with an ID the descendant already declared is the same child, and the descendant's
//! version wins.
inline std::vector<PrefabChild> ResolvePrefabChildren(const EnfusionMount& mount, const std::string& etPath,
                                                      int depthLimit = 8)
{
    std::vector<PrefabChild> out;
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
            return out;
        std::vector<PrefabChild> own;
        FindChildPlacements(blob, own);
        for (PrefabChild& child : own)
        {
            bool known = false;
            if (!child.id.empty())
                for (const PrefabChild& have : out)
                    known = known || have.id == child.id;
            if (!known)
                out.push_back(std::move(child));
        }
        const auto parsed = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (parsed.parentPath.empty())
            return out;
        current = parsed.parentPath;
    }
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
