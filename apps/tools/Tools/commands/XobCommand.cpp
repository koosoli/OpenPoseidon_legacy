#include "XobCommand.hpp"

#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ARF-003 -- Arma Reforger's `.xob` render geometry into this engine's model
// pipeline, and its worlds' object placements into a manifest a world exporter can
// consume.
//
// Nothing here invents a format. The read side is XobModel.hpp (15,809 / 15,809
// files close their byte accounting), PakArchive.hpp (16 / 16 archives close) and
// EbinWorld.hpp (103 / 103 `.ent` files close, 1,230,897 placements on Everon); the
// write side is MLODWriter.hpp. This command is the wiring between them plus the
// three things that wiring needs and none of them provide: which LOD to take, which
// winding to write it in, and which model a placement's prefab GUID refers to.
//
// Three measured facts decide those, and each one is a mistake that a parse-success
// check does not catch:
//
//  1. LOD ORDER. XobModel.hpp's header comment says the coarsest LOD sits first and
//     thresholds "run strictly descending, LOD0 always 0.5". The descending part is
//     right and the coarsest-first part is not: measured on
//     Assets/Vegetation/Tree/Picea_Abies/t_picea_abies_3f.xob the five LODs run
//     0.5 / 0.15 / 0.02 / 0.005 / 0.001 with 3,216 / 1,188 / 336 / 98 / 6 triangles,
//     so threshold 0.5 is the FINEST and it is index 0. Selecting the minimum
//     threshold -- which "coarsest first" invites -- yields the 6-triangle billboard
//     and every check short of looking at the triangle count still passes. This
//     selects by MAXIMUM threshold and reports the count it took.
//
//  2. WINDING. Measured over the whole of LOD0 on three corpus models (1,500 + 457 +
//     3,216 triangles): the cross product of the first two edges of an `.xob` index
//     triple points ALONG the mean stored vertex normal, unanimously (1.0000 in all
//     three). ODOLLoader.hpp:206-215 records the opposite convention as measured for
//     ODOL -- cross opposite the stored normal -- and the ODOL loader does not
//     reverse winding on load while MLODLoader.hpp:384 does. So an MLOD file whose
//     on-disk cross points ALONG the normal reaches the IR with the same handedness
//     an ODOL file does, and `.xob` triples are already in that order. They are
//     therefore written VERBATIM. `--flip-winding` exists because the ODOL premise
//     rests on a packed-normal sign no render has confirmed, not because anything
//     measured here is uncertain.
//
//  3. PREFAB INHERITANCE. A world placement names a prefab GUID, not a model. The
//     `.et` prefab it resolves to often carries no mesh of its own:
//     Prefabs/Rocks/Granite/Granite_BeachCluster_02_Fucus.et -- the single most
//     placed prefab on Everon at 71,319 instances -- only overrides two materials
//     and inherits its `Object` from Granite_BeachCluster_02.et. Stopping at the
//     first `.et` loses the corpus's largest object outright.
//
//  4. WHERE THE ALBEDO GOES. A `.xob` part names an `.emat`, and an `.emat` is a
//     shader description, not an image. Written into the MLOD MATERIAL field alone it
//     resolves to nothing: `ParseRvMaterialFile` cannot read an `.emat`, so
//     `ResolveMaterialBaseColour` caches the miss, the face texture is empty, and the
//     section reaches the GPU with `texture_id == 0` -- the "section draws UNTEXTURED
//     (fallback=none)" warning, and a white tree.
//
//     `--textures` closes that chain in the oldest and bluntest way the format has:
//     the material's `BCRMap` is decoded out of the `.pak`, folded with its
//     `OpacityMap`, written as a `.paa` and named in the MLOD TEXTURE field. That is
//     the field `ShapeAdapter` hands to `GlobLoadTexture`, so no engine change is
//     needed and no material file has to exist for the albedo to bind.
//
//     The alpha is not a detail. `ClassifyGpuSection` takes a section's cutout
//     decision from the FACE TEXTURE's alpha class and from nothing else, so a leaf
//     card whose albedo arrives through the material slot with no face texture is
//     drawn as an opaque rectangle -- visibly worse than the white it replaced. The
//     `.paa` is therefore DXT5 with its top mip thresholded to 0/255, which is what
//     `ClassifyAlpha` needs to answer Cutout (partial alpha under 2%) rather than
//     Blend. DXT1 is used when the material names no `OpacityMap`, because a DXT1
//     PAA reports `IsAlpha() == false` and short-circuits to Opaque.
//
// The whole-model bounding box in metres is reported for every conversion because a
// unit-scale mistake is the failure mode that survives every structural check: the
// geometry parses, round-trips and draws, at 100x the size.

using namespace Poseidon::Asset::Formats::Enfusion;
namespace Fmt = Poseidon::Asset::Formats;

namespace PoseidonTools
{
namespace
{

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool EndsWith(const std::string& text, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

bool IsHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

//! Enfusion writes asset ids as 16 uppercase hex digits. Everything that keys on one
//! -- the world's placements, an `.et`'s `ID` line, a `{GUID}path` reference -- has
//! to agree on the case or the maps silently miss.
std::string UpperGuid(const char* begin, size_t length)
{
    std::string out(begin, length);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
// Every `.pak` under a root, opened once, with a case-insensitive path index
// ---------------------------------------------------------------------------

class PakSet
{
  public:
    bool Open(const std::string& root)
    {
        std::vector<std::string> paths;
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec))
        {
            paths.push_back(root);
        }
        else
        {
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_regular_file(ec) && LowerCopy(it->path().extension().string()) == ".pak")
                    paths.push_back(it->path().string());
            }
        }
        std::sort(paths.begin(), paths.end());
        if (paths.empty())
        {
            _error = "no .pak files under " + root;
            return false;
        }

        // Sized up front: the index holds archive/entry indices rather than
        // pointers, so a later push_back could not invalidate it, but the archives
        // still have to exist before anything indexes into them.
        _archives.resize(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (!_archives[i].Open(paths[i]))
            {
                std::cerr << "  cannot open " << paths[i] << ": " << _archives[i].Error() << "\n";
                continue;
            }
            const auto& entries = _archives[i].Entries();
            for (size_t e = 0; e < entries.size(); ++e)
                _index.emplace(LowerCopy(entries[e].path), std::pair<size_t, size_t>{i, e});
        }
        if (_index.empty())
        {
            _error = "no readable entries under " + root;
            return false;
        }
        return true;
    }

    size_t ArchiveCount() const { return _archives.size(); }
    size_t EntryCount() const { return _index.size(); }
    const std::string& Error() const { return _error; }
    const std::vector<PakArchive>& Archives() const { return _archives; }

    bool Read(const std::string& virtualPath, std::vector<uint8_t>& out) const
    {
        const auto it = _index.find(LowerCopy(virtualPath));
        if (it == _index.end())
            return false;
        const PakArchive& archive = _archives[it->second.first];
        return archive.Read(archive.Entries()[it->second.second], out);
    }

    bool Has(const std::string& virtualPath) const { return _index.count(LowerCopy(virtualPath)) != 0; }

    //! The stored spelling of a path, which is what a virtual path should be derived
    //! from -- the index is lowercased and the archives are not.
    const std::string* StoredPath(const std::string& virtualPath) const
    {
        const auto it = _index.find(LowerCopy(virtualPath));
        if (it == _index.end())
            return nullptr;
        return &_archives[it->second.first].Entries()[it->second.second].path;
    }

  private:
    std::vector<PakArchive> _archives;
    std::unordered_map<std::string, std::pair<size_t, size_t>> _index;
    std::string _error;
};

// ---------------------------------------------------------------------------
// GUID -> resource path
// ---------------------------------------------------------------------------

// Two independent sources, exactly as the Python reference census established them:
//
//   * every `{<16 hex>}<path>` reference in any text-ish resource names ANOTHER
//     resource, which is the only source that covers non-text assets such as `.xob`;
//   * every `.et`'s own `ID "<16 hex>"` line.
//
// They are kept in SEPARATE maps and references are consulted first, which the
// reference implementation could not do because it merged them into one dictionary
// with order-dependent precedence. The distinction is real: on
// Prefabs/Rocks/Granite/Granite_BeachCluster_02_Fucus.et the `ID` line reads
// 50D67C36F5DCBE17 while the world places it as {5A403FBC1467033F}, so the `ID` line
// is the entity id and NOT the resource GUID a placement carries. Folding the two
// together means an `ID` collision can shadow a correct reference.
struct ResourceMap
{
    std::unordered_map<std::string, std::string> byReference;
    std::unordered_map<std::string, std::string> byOwnId;
    size_t filesScanned = 0;
    size_t filesUnreadable = 0;

    const std::string* Find(const std::string& guid) const
    {
        const auto ref = byReference.find(guid);
        if (ref != byReference.end())
            return &ref->second;
        const auto own = byOwnId.find(guid);
        if (own != byOwnId.end())
            return &own->second;
        return nullptr;
    }

    size_t Size() const { return byReference.size() + byOwnId.size(); }
};

//! Extensions whose contents are Enfusion's brace syntax rather than a binary blob.
bool IsTextResource(const std::string& lowerPath)
{
    static const char* kTexty[] = {".et",      ".emat", ".conf",     ".layout", ".gamemat", ".ct", ".physmat",
                                   ".vhcsurf", ".terr", ".imageset", ".styles", ".ptc",     ".bt", ".c"};
    for (const char* ext : kTexty)
        if (EndsWith(lowerPath, ext))
            return true;
    return false;
}

//! Collects every `{<16 hex>}<path>` reference in a blob. First spelling wins, so a
//! GUID that appears in several files resolves to one path deterministically.
void ScanReferences(const std::vector<uint8_t>& blob, ResourceMap& map)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    for (size_t i = 0; i + 18 <= size; ++i)
    {
        if (data[i] != '{' || data[i + 17] != '}')
            continue;
        bool hex = true;
        for (size_t k = 1; k <= 16 && hex; ++k)
            hex = IsHexDigit(data[i + k]);
        if (!hex)
            continue;
        size_t end = i + 18;
        while (end < size)
        {
            const char c = data[end];
            if (c == '"' || c == '\'' || c == '{' || c == '}' || static_cast<unsigned char>(c) <= ' ')
                break;
            ++end;
        }
        if (end > i + 18)
            map.byReference.emplace(UpperGuid(data + i + 1, 16), std::string(data + i + 18, end - i - 18));
        i = end - 1;
    }
}

//! The `ID "<16 hex>"` line an `.et` carries. Line-anchored: the same 16-hex shape
//! occurs inside brace references on almost every other line of the same file.
void ScanOwnId(const std::vector<uint8_t>& blob, const std::string& path, ResourceMap& map)
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
        if (cursor + 21 <= lineEnd && data[cursor] == 'I' && data[cursor + 1] == 'D' &&
            static_cast<unsigned char>(data[cursor + 2]) <= ' ')
        {
            size_t quote = cursor + 3;
            while (quote < lineEnd && static_cast<unsigned char>(data[quote]) <= ' ')
                ++quote;
            if (quote + 17 < lineEnd && data[quote] == '"' && data[quote + 17] == '"')
            {
                bool hex = true;
                for (size_t k = 1; k <= 16 && hex; ++k)
                    hex = IsHexDigit(data[quote + k]);
                if (hex)
                {
                    map.byOwnId[UpperGuid(data + quote + 1, 16)] = path;
                    return; // the ID line is the second line of every `.et`; one per file
                }
            }
        }
        at = lineEnd + 1;
    }
}

ResourceMap BuildResourceMap(const PakSet& paks)
{
    ResourceMap map;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (!IsTextResource(lower))
                continue;
            std::vector<uint8_t> blob;
            if (!archive.Read(entry, blob))
            {
                map.filesUnreadable++;
                continue;
            }
            map.filesScanned++;
            if (EndsWith(lower, ".et"))
                ScanOwnId(blob, entry.path, map);
            ScanReferences(blob, map);
        }
    }
    return map;
}

bool LoadResourceMapCache(const std::string& path, ResourceMap& map)
{
    std::ifstream file(path);
    if (!file)
        return false;
    std::string line;
    while (std::getline(file, line))
    {
        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos)
            continue;
        const size_t tab2 = line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
            continue;
        const std::string guid = line.substr(tab1 + 1, tab2 - tab1 - 1);
        std::string value = line.substr(tab2 + 1);
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        if (line[0] == 'R')
            map.byReference.emplace(guid, std::move(value));
        else
            map.byOwnId.emplace(guid, std::move(value));
    }
    return map.Size() != 0;
}

void SaveResourceMapCache(const std::string& path, const ResourceMap& map)
{
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return;
    for (const auto& [guid, value] : map.byReference)
        file << "R\t" << guid << '\t' << value << '\n';
    for (const auto& [guid, value] : map.byOwnId)
        file << "S\t" << guid << '\t' << value << '\n';
}

// ---------------------------------------------------------------------------
// `.et` prefab -> `.xob`
// ---------------------------------------------------------------------------

//! Why this is a line scanner and not ParseEmat: the mesh reference sits two blocks
//! deep (`components { MeshObject "{..}" { Object "{..}path.xob" } }`) and ParseEmat
//! folds every token below the top level into the enclosing property's value list,
//! so the key `Object` stops being a key. ParseEmat IS used for the header, which is
//! where the four measured header forms and the parent reference live.
std::string FindObjectReference(const std::vector<uint8_t>& blob)
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
        // Keyed on the exact property name. `m_sPhaseModel` in a
        // SCR_BaseDestructionPhase block names a `.xob` too -- the stump the tree
        // becomes when destroyed -- and a scan for "any .xob on any line" picks that
        // one up on every destructible prefab in the corpus.
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
                if (EndsWith(LowerCopy(path), ".xob"))
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
//! `depthLimit` is 8 for the same reason ResolveEmatInheritance uses 8: it bounds a
//! cycle in authored data rather than expressing an expected depth. Everon's top 25
//! prefabs resolve at depth 0 or 1.
PrefabResolution ResolvePrefabModel(const PakSet& paks, const std::string& etPath, int depthLimit = 8)
{
    PrefabResolution out;
    std::string current = etPath;
    std::vector<std::string> seen;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        for (const std::string& before : seen)
        {
            if (LowerCopy(before) == LowerCopy(current))
            {
                out.reason = "prefab inheritance cycle at " + current;
                return out;
            }
        }
        seen.push_back(current);

        std::vector<uint8_t> blob;
        if (!paks.Read(current, blob))
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
// `.emat` -> a `.paa` the engine can bind
// ---------------------------------------------------------------------------

//! Defined with the rest of the path helpers below; used here first.
std::filesystem::path OnDiskPathFor(const std::string& outputRoot, const std::string& virtualPath);

struct TextureOptions
{
    bool enabled = false;
    std::string prefix = "reforger\\everon\\textures";
    //! Longest edge written. Reforger authors 2048 and 4096 albedos; the whole point
    //! of the cap is that this writes one file per material and the corpus's top 50
    //! models name around sixty of them. 4096 also runs into the writer's truncated
    //! mip chain (measured on DXT3), so staying well under it is not only a budget.
    int maxSize = 1024;
};

//! Decode a `.edds` out of the paks into straight RGBA8.
bool LoadEdds(const PakSet& paks, const std::string& path, Poseidon::Image& out)
{
    std::vector<uint8_t> bytes;
    if (!paks.Read(path, bytes) || bytes.empty())
        return false;
    Poseidon::EddsImage edds = Poseidon::ReadEddsBuffer(bytes.data(), bytes.size());
    if (!edds.valid() || edds.mipmaps.empty())
        return false;
    Poseidon::Image image(edds.width, edds.height, edds.format, std::move(edds.mipmaps[0].data));
    out = image.ToRGBA();
    return out.valid() && out.data().size() >= static_cast<size_t>(out.width()) * static_cast<size_t>(out.height()) * 4;
}

//! One 2x2 box step. Exact on the power-of-two textures Reforger authors, and the
//! reason no resampler is linked in here: every one of them halves cleanly.
void HalveRGBA(std::vector<uint8_t>& pixels, int& width, int& height)
{
    const int nw = width / 2;
    const int nh = height / 2;
    std::vector<uint8_t> out(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
    for (int y = 0; y < nh; ++y)
    {
        for (int x = 0; x < nw; ++x)
        {
            const size_t a = (static_cast<size_t>(2 * y) * width + 2 * x) * 4;
            const size_t b = a + 4;
            const size_t c = a + static_cast<size_t>(width) * 4;
            const size_t d = c + 4;
            for (int ch = 0; ch < 4; ++ch)
            {
                const unsigned sum = pixels[a + ch] + pixels[b + ch] + pixels[c + ch] + pixels[d + ch];
                out[(static_cast<size_t>(y) * nw + x) * 4 + ch] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    pixels = std::move(out);
    width = nw;
    height = nh;
}

struct TextureStats
{
    size_t materialsSeen = 0;  //!< distinct `.emat` paths asked about
    size_t materialsBound = 0; //!< of those, the ones that produced a `.paa`
    size_t filesWritten = 0;   //!< distinct `.paa` files on disk
    size_t filesWithAlpha = 0; //!< of those, the ones carrying a cutout mask
    size_t baseLayerOnly = 0;  //!< MatPBRMulti materials approximated by their BCR_1
    size_t bytesWritten = 0;
    std::map<std::string, size_t> failures; //!< reason -> distinct materials
};

//! Turns the `.emat` a `.xob` part names into a `.paa` at a virtual path, once per
//! material, and reports why when it cannot.
//!
//! Two caches, and they are not the same cache: several `.emat` files share one
//! `BCRMap` (every Picea LOD's crown is the same atlas), so the material cache stops
//! the parse and the image cache stops the decode-and-encode. Keying the image cache
//! on the BCR ALONE would be wrong -- two materials may share an albedo and differ in
//! their opacity mask -- so the key is the pair.
class TextureBaker
{
  public:
    TextureBaker(const PakSet& paks, const std::string& outputRoot, const TextureOptions& options)
        : _paks(paks), _outputRoot(outputRoot), _options(options)
    {
    }

    const TextureStats& Stats() const { return _stats; }

    //! The virtual `.paa` path a section using `ematPath` should name, or "" when the
    //! material resolves no albedo this engine can bind.
    const std::string& TextureFor(const std::string& ematPath)
    {
        const std::string key = LowerCopy(ematPath);
        const auto found = _byMaterial.find(key);
        if (found != _byMaterial.end())
            return found->second;
        _stats.materialsSeen++;
        std::string result = Bake(ematPath);
        if (!result.empty())
            _stats.materialsBound++;
        return _byMaterial.emplace(key, std::move(result)).first->second;
    }

  private:
    void Unbound(const char* reason) { _stats.failures[reason]++; }

    std::string Bake(const std::string& ematPath)
    {
        std::vector<uint8_t> blob;
        if (!_paks.Read(ematPath, blob))
        {
            Unbound(".emat not in any .pak");
            return {};
        }
        Poseidon::Asset::Material::EmatMaterial emat = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (!emat.valid())
        {
            Unbound(".emat does not parse");
            return {};
        }
        // 18.3% of Reforger's materials get their maps only from a parent, and a child
        // read without one keeps its own overrides and silently loses the albedo.
        Poseidon::Asset::Material::ResolveEmatInheritance(emat,
                                                          [&](const std::string& path, std::string& text)
                                                          {
                                                              std::vector<uint8_t> parent;
                                                              if (!_paks.Read(path, parent))
                                                                  return false;
                                                              text.assign(parent.begin(), parent.end());
                                                              return true;
                                                          });

        // `MatPBRMulti` is the one common class with no single albedo: it blends up to
        // four layers through a `MaskMap`, and each layer names its own `BCR_N`. Two of
        // the corpus's top 50 models are that class -- GraniteCliff_01 (9,445
        // placements) and the Picea stump (8,275) -- so refusing them costs more than
        // approximating them. `BCR_1` is the base layer: it is the only one with no
        // `Enabled_N` switch, so it is the layer that is always painted. Binding it is
        // an approximation of the blend and is counted separately for that reason.
        std::string bcr = emat.TextureOf("BCRMap");
        if (bcr.empty())
        {
            for (const char* layer : {"BCR_1", "BCR_2", "BCR_3", "BCR_4"})
            {
                bcr = emat.TextureOf(layer);
                if (!bcr.empty())
                {
                    _stats.baseLayerOnly++;
                    break;
                }
            }
        }
        if (bcr.empty())
        {
            Unbound("material names no BCRMap and no BCR_N layer");
            return {};
        }
        const std::string opacity = emat.TextureOf("OpacityMap");

        const std::string imageKey = LowerCopy(bcr) + "|" + LowerCopy(opacity);
        const auto cached = _byImage.find(imageKey);
        if (cached != _byImage.end())
            return cached->second;

        std::string written = Write(bcr, opacity);
        _byImage.emplace(imageKey, written);
        return written;
    }

    //! `assets/vegetation/_polyplanes/polyplane_picea_abies_bcr.paa` under the prefix;
    //! `_ca` appended when an opacity mask was folded in, which is Real Virtuality's
    //! own notation for "colour plus alpha" and keeps the two variants of a shared
    //! albedo apart.
    std::string VirtualNameFor(const std::string& bcrPath, bool alpha) const
    {
        std::string stem = LowerCopy(bcrPath);
        const size_t dot = stem.rfind('.');
        if (dot != std::string::npos)
            stem = stem.substr(0, dot);
        for (char& c : stem)
            if (c == '/')
                c = '\\';
        if (alpha)
            stem += "_ca";
        std::string head = LowerCopy(_options.prefix);
        for (char& c : head)
            if (c == '/')
                c = '\\';
        while (!head.empty() && head.back() == '\\')
            head.pop_back();
        std::string candidate = (head.empty() ? stem : head + "\\" + stem) + ".paa";
        // Two distinct (albedo, opacity) pairs can still land on one name when the
        // same albedo is masked two different ways. Rare, but a collision here means
        // one material silently wears another's cutout.
        for (int suffix = 2; _taken.count(candidate) != 0; ++suffix)
            candidate = (head.empty() ? stem : head + "\\" + stem) + "_" + std::to_string(suffix) + ".paa";
        return candidate;
    }

    std::string Write(const std::string& bcrPath, const std::string& opacityPath)
    {
        Poseidon::Image albedo;
        if (!LoadEdds(_paks, bcrPath, albedo))
        {
            Unbound("BCRMap .edds does not decode");
            return {};
        }
        int width = albedo.width();
        int height = albedo.height();
        std::vector<uint8_t> pixels = albedo.data();
        pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

        // The opacity map is a separate single-channel image and may be authored at a
        // different resolution than the albedo. BC4 decodes into R with G and B left
        // at zero and BC7 greyscale into all three, so the maximum reads both without
        // having to know which the file was.
        bool hasAlpha = false;
        if (!opacityPath.empty())
        {
            Poseidon::Image mask;
            if (!LoadEdds(_paks, opacityPath, mask))
            {
                Unbound("OpacityMap .edds does not decode");
            }
            else
            {
                const std::vector<uint8_t>& mp = mask.data();
                for (int y = 0; y < height; ++y)
                {
                    const int sy = mask.height() == height ? y : y * mask.height() / height;
                    for (int x = 0; x < width; ++x)
                    {
                        const int sx = mask.width() == width ? x : x * mask.width() / width;
                        const size_t at = (static_cast<size_t>(sy) * mask.width() + sx) * 4;
                        const uint8_t value = std::max({mp[at], mp[at + 1], mp[at + 2]});
                        pixels[(static_cast<size_t>(y) * width + x) * 4 + 3] = value;
                    }
                }
                hasAlpha = true;
            }
        }
        if (!hasAlpha)
        {
            for (size_t at = 3; at < pixels.size(); at += 4)
                pixels[at] = 255;
        }

        while ((width > _options.maxSize || height > _options.maxSize) && width % 2 == 0 && height % 2 == 0 &&
               width > 4 && height > 4)
            HalveRGBA(pixels, width, height);

        // Binary, and only after the downscale: `ClassifyAlpha` calls anything with
        // more than 2% partial-alpha texels a Blend, and `ClassifyGpuSection` drops a
        // Blend section out of the retained pass altogether. A hard 0/255 top mip is
        // what makes the same texture answer Cutout, which is what alpha-tests the
        // leaf cards at the 0.5 reference the retained path forces on them.
        if (hasAlpha)
        {
            for (size_t at = 3; at < pixels.size(); at += 4)
                pixels[at] = pixels[at] >= 128 ? 255 : 0;
        }

        const std::string virtualPath = VirtualNameFor(bcrPath, hasAlpha);
        const std::filesystem::path target = OnDiskPathFor(_outputRoot, virtualPath);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        const Poseidon::PixelFormat format = hasAlpha ? Poseidon::PixelFormat::DXT5 : Poseidon::PixelFormat::DXT1;
        if (!Poseidon::PAAEncoder::WritePAA(target.string(),
                                            Poseidon::Image::FromRGBA(width, height, std::move(pixels)), format))
        {
            Unbound("WritePAA refused it");
            return {};
        }

        // WritePAA accepts formats the PAA READER cannot read, and a texture that
        // fails to load is silent all the way to a white draw. Read it back.
        Poseidon::PAAInfo info;
        const Poseidon::DecodedImage back = Poseidon::DecodePAAFile(target.string());
        if (!ReadPAAInfo(target.string(), info) || !back.valid() || back.width != width || back.height != height)
        {
            std::filesystem::remove(target, ec);
            Unbound(".paa does not read back");
            return {};
        }

        _taken.insert(virtualPath);
        _stats.filesWritten++;
        _stats.filesWithAlpha += hasAlpha ? 1 : 0;
        _stats.bytesWritten += static_cast<size_t>(std::filesystem::file_size(target, ec));
        return virtualPath;
    }

    const PakSet& _paks;
    std::string _outputRoot;
    TextureOptions _options;
    std::unordered_map<std::string, std::string> _byMaterial;
    std::unordered_map<std::string, std::string> _byImage;
    std::unordered_set<std::string> _taken;
    TextureStats _stats;
};

// ---------------------------------------------------------------------------
// `.xob` -> MLOD P3DM
// ---------------------------------------------------------------------------

struct ConvertOptions
{
    float resolution = 1.0f;  //!< the LOD's viewing distance; anything under 1e5 is Visual
    bool secondUv = false;    //!< emit both #UVSet# blocks when a part declares two sets
    bool flipWinding = false; //!< see the winding note at the top of this file
    bool materialAsRvmat = false;
    TextureOptions textures;
};

struct ConvertStats
{
    size_t lodIndex = 0;
    size_t lodCount = 0;
    float threshold = 0.0f;
    size_t parts = 0;
    size_t points = 0;
    size_t triangles = 0;
    size_t droppedUnreferenced = 0; //!< source vertices no triangle indexes
    size_t materials = 0;
    size_t partsTextured = 0;    //!< parts whose material resolved to a written `.paa`
    size_t crossAlongNormal = 0; //!< triangles whose edge cross agrees with the stored normals
    float bboxMin[3] = {0.0f, 0.0f, 0.0f};
    float bboxMax[3] = {0.0f, 0.0f, 0.0f};

    float Extent(int axis) const { return bboxMax[axis] - bboxMin[axis]; }
};

//! Selects the finest visual LOD: the MAXIMUM threshold. See fact 1 at the top --
//! taking the minimum is the mistake this exists to make impossible.
size_t FinestLodIndex(const XobHeader& header)
{
    size_t best = 0;
    for (size_t i = 1; i < header.lods.size(); ++i)
        if (header.lods[i].threshold > header.lods[best].threshold)
            best = i;
    return best;
}

bool ConvertXob(const std::vector<uint8_t>& bytes, const ConvertOptions& options, TextureBaker* baker,
                Fmt::MLOD::WriteModel& out, ConvertStats& stats, std::string& error)
{
    const XobHeader header = ReadXobHeader(bytes.data(), bytes.size());
    if (!header.valid())
    {
        error = "HEAD: " + header.error;
        return false;
    }
    if (header.lods.empty())
    {
        error = "no LODs";
        return false;
    }

    stats.lodCount = header.lods.size();
    stats.lodIndex = FinestLodIndex(header);
    stats.threshold = header.lods[stats.lodIndex].threshold;

    XobLod lod;
    if (!ReadXobLod(bytes.data(), bytes.size(), header, stats.lodIndex, lod, error))
        return false;

    Fmt::MLOD::WriteLod writeLod;
    writeLod.resolution = options.resolution;
    bool anySecondSet = false;
    std::unordered_set<std::string> materialPaths;

    float boxMin[3] = {1e30f, 1e30f, 1e30f};
    float boxMax[3] = {-1e30f, -1e30f, -1e30f};

    for (const XobPart& part : lod.parts)
    {
        if (part.uvSets.empty() || part.uvSets[0].size() != part.positions.size())
        {
            // The corpus's one nUV == 0 part (Billiard_01_ball LOD1) is refused by
            // the reader already; this catches the same shape reaching here another
            // way rather than writing a model with UVs silently at the origin.
            error = "part has no usable UV set 0";
            return false;
        }

        // Only referenced source vertices become points. Every point must be
        // referenced for the round-trip check below to be an identity: MLODLoader
        // creates vertices from face corners, so an unreferenced point produces no
        // vertex and shifts every index after it.
        std::vector<int32_t> remap(part.positions.size(), -1);
        for (uint16_t index : part.indices)
            if (index < remap.size())
                remap[index] = 0;

        for (size_t v = 0; v < part.positions.size(); ++v)
        {
            if (remap[v] != 0)
            {
                stats.droppedUnreferenced++;
                continue;
            }
            remap[v] = static_cast<int32_t>(writeLod.points.size());

            Fmt::MLOD::WritePoint point;
            point.position = {part.positions[v].x, part.positions[v].y, part.positions[v].z};
            // Deliberately 0: POINT_* bits are an OFP concept the source has no
            // equivalent for, and MLODLoader turns 0 into ClipAll, which is what an
            // ordinary visual point wants.
            point.flags = 0;
            writeLod.points.push_back(point);
            writeLod.normals.push_back({part.normals[v].x, part.normals[v].y, part.normals[v].z});

            for (int axis = 0; axis < 3; ++axis)
            {
                const float value = axis == 0   ? part.positions[v].x
                                    : axis == 1 ? part.positions[v].y
                                                : part.positions[v].z;
                boxMin[axis] = std::min(boxMin[axis], value);
                boxMax[axis] = std::max(boxMax[axis], value);
            }
        }

        std::string materialSource;
        if (part.materialIndex < header.materials.size())
            materialSource = header.materials[part.materialIndex].path;

        // The albedo, resolved from the SOURCE `.emat` -- the rename below is a
        // naming convention for the material field and must not reach the baker.
        std::string texturePath;
        if (baker && !materialSource.empty())
            texturePath = baker->TextureFor(materialSource);
        if (!texturePath.empty())
            stats.partsTextured++;

        std::string materialPath = materialSource;
        if (options.materialAsRvmat && EndsWith(LowerCopy(materialPath), ".emat"))
            materialPath = materialPath.substr(0, materialPath.size() - 5) + ".rvmat";
        if (!materialPath.empty())
            materialPaths.insert(materialPath);

        const bool hasSecond = part.uvSets.size() > 1 && part.uvSets[1].size() == part.positions.size();
        anySecondSet = anySecondSet || hasSecond;

        for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
        {
            uint16_t corner[3] = {part.indices[t], part.indices[t + 1], part.indices[t + 2]};
            if (options.flipWinding)
                std::swap(corner[0], corner[1]);

            // Winding evidence, accumulated over every triangle actually written --
            // a spot check on one face proves nothing about a mesh with mixed parts.
            {
                const XobVec3& p0 = part.positions[corner[0]];
                const XobVec3& p1 = part.positions[corner[1]];
                const XobVec3& p2 = part.positions[corner[2]];
                const float e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                const float e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                        e1[0] * e2[1] - e1[1] * e2[0]};
                float mean[3] = {0.0f, 0.0f, 0.0f};
                for (int c = 0; c < 3; ++c)
                {
                    mean[0] += part.normals[corner[c]].x;
                    mean[1] += part.normals[corner[c]].y;
                    mean[2] += part.normals[corner[c]].z;
                }
                if (cross[0] * mean[0] + cross[1] * mean[1] + cross[2] * mean[2] > 0.0f)
                    stats.crossAlongNormal++;
            }

            Fmt::MLOD::WriteFace face;
            face.vertexCount = 3;
            face.flags = 0;
            face.material = materialPath;
            // The `.emat` path itself never goes in the texture slot: it is a shader
            // description, not an image, and ShapeAdapter.cpp:300-307 would hand it
            // straight to GlobLoadTexture. What goes here is the `.paa` the baker
            // wrote out of the material's BCRMap -- see fact 4 at the top. Empty
            // without `--textures`, which leaves the old behaviour intact.
            face.texture = texturePath;
            for (int c = 0; c < 3; ++c)
            {
                const int32_t point = remap[corner[c]];
                face.vertices[c].point = point;
                face.vertices[c].normal = point;
                face.vertices[c].u = part.uvSets[0][corner[c]].u;
                face.vertices[c].v = part.uvSets[0][corner[c]].v;
                if (hasSecond)
                {
                    face.vertices[c].u1 = part.uvSets[1][corner[c]].u;
                    face.vertices[c].v1 = part.uvSets[1][corner[c]].v;
                }
            }
            writeLod.faces.push_back(std::move(face));
        }
    }

    if (writeLod.points.empty() || writeLod.faces.empty())
    {
        error = "LOD has no geometry";
        return false;
    }

    writeLod.secondUVChannel = options.secondUv && anySecondSet;
    stats.parts = lod.parts.size();
    stats.points = writeLod.points.size();
    stats.triangles = writeLod.faces.size();
    stats.materials = materialPaths.size();
    for (int axis = 0; axis < 3; ++axis)
    {
        stats.bboxMin[axis] = boxMin[axis];
        stats.bboxMax[axis] = boxMax[axis];
    }

    out.lods.clear();
    out.lods.push_back(std::move(writeLod));
    return true;
}

// ---------------------------------------------------------------------------
// Round-trip through the engine's own reader
// ---------------------------------------------------------------------------

// This is the check that makes a written model trustworthy, and it is an exact
// identity rather than a tolerance: with one point per source vertex and every point
// referenced, MLODLoader's per-point vertex merge produces exactly one vertex per
// point, and its sortVerticesByPointIndex then puts vertex k at point k (all points
// carry the same flags, so the sort is by point index alone). Anything that breaks
// that -- a stride, an index base, a UV taken from the wrong stream -- shows up as a
// mismatch count, not as a "loaded fine".
struct RoundTrip
{
    bool ok = false;
    std::string reason;
    size_t vertices = 0;
    size_t triangles = 0;
    size_t positionMismatches = 0;
    size_t uvMismatches = 0;
    size_t topologyMismatches = 0;
};

RoundTrip VerifyRoundTrip(const std::string& filePath, const Fmt::MLOD::WriteModel& written)
{
    RoundTrip result;
    Poseidon::Model::Model loaded;
    try
    {
        loaded = Fmt::MLODLoader::load(filePath);
    }
    catch (const std::exception& e)
    {
        result.reason = std::string("MLODLoader refused it: ") + e.what();
        return result;
    }

    if (loaded.lodLevels.size() != written.lods.size())
    {
        result.reason =
            "LOD count " + std::to_string(loaded.lodLevels.size()) + " != " + std::to_string(written.lods.size());
        return result;
    }
    const auto& source = written.lods[0];
    const auto& mesh = loaded.lodLevels[0].mesh;
    result.vertices = mesh.vertices.size();
    result.triangles = mesh.triangles.size();

    if (mesh.vertices.size() != source.points.size())
    {
        result.reason = "vertex count " + std::to_string(mesh.vertices.size()) + " != point count " +
                        std::to_string(source.points.size());
        return result;
    }
    if (mesh.triangles.size() != source.faces.size() || !mesh.quads.empty())
    {
        result.reason = "face count " + std::to_string(mesh.triangles.size()) + " (+" +
                        std::to_string(mesh.quads.size()) + " quads) != " + std::to_string(source.faces.size());
        return result;
    }

    for (size_t i = 0; i < source.points.size(); ++i)
    {
        const auto& want = source.points[i].position;
        const auto& got = mesh.vertices[i].position;
        if (got.x != want.x || got.y != want.y || got.z != want.z)
            result.positionMismatches++;
    }

    for (size_t f = 0; f < source.faces.size(); ++f)
    {
        const auto& face = source.faces[f];
        const auto& triangle = mesh.triangles[f];
        // MLODLoader.hpp:384 swaps corners 0 and 1 on load; undo it to compare.
        const uint32_t got[3] = {triangle.indices[1], triangle.indices[0], triangle.indices[2]};
        for (int c = 0; c < 3; ++c)
        {
            if (got[c] != static_cast<uint32_t>(face.vertices[c].point))
            {
                result.topologyMismatches++;
                break;
            }
        }
        for (int c = 0; c < 3; ++c)
        {
            const uint32_t vertex = got[c];
            if (vertex >= mesh.vertices.size())
                continue;
            if (mesh.vertices[vertex].uv.u != face.vertices[c].u || mesh.vertices[vertex].uv.v != face.vertices[c].v)
            {
                result.uvMismatches++;
                break;
            }
        }
    }

    result.ok = result.positionMismatches == 0 && result.uvMismatches == 0 && result.topologyMismatches == 0;
    if (!result.ok)
        result.reason = std::to_string(result.positionMismatches) + " positions, " +
                        std::to_string(result.uvMismatches) + " UVs, " + std::to_string(result.topologyMismatches) +
                        " triangles differ";
    return result;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

//! `Assets/Vegetation/.../t_picea_abies_0.xob` ->
//! `reforger\everon\models\assets\vegetation\...\t_picea_abies_0.p3d`.
//!
//! Lowercased and backslash-separated because that is what the engine's virtual file
//! system compares on (ShapeAdapter.cpp:300-305 lowercases before every lookup).
std::string VirtualPathFor(const std::string& prefix, const std::string& sourcePath)
{
    std::string out = LowerCopy(sourcePath);
    const size_t dot = out.rfind('.');
    if (dot != std::string::npos)
        out = out.substr(0, dot);
    out += ".p3d";
    for (char& c : out)
        if (c == '/')
            c = '\\';
    if (prefix.empty())
        return out;
    std::string head = LowerCopy(prefix);
    for (char& c : head)
        if (c == '/')
            c = '\\';
    if (head.back() != '\\')
        head += '\\';
    return head + out;
}

std::filesystem::path OnDiskPathFor(const std::string& outputRoot, const std::string& virtualPath)
{
    std::string relative = virtualPath;
    for (char& c : relative)
        if (c == '\\')
            c = '/';
    return std::filesystem::path(outputRoot) / relative;
}

// ---------------------------------------------------------------------------
// One model, end to end
// ---------------------------------------------------------------------------

struct ModelResult
{
    std::string virtualPath;
    ConvertStats stats;
    RoundTrip roundTrip;
    bool written = false;
    std::string error;
};

ModelResult ConvertOne(const PakSet& paks, const std::string& sourcePath, const std::string& outputRoot,
                       const std::string& prefix, const ConvertOptions& options, TextureBaker* baker)
{
    ModelResult result;
    const std::string* stored = paks.StoredPath(sourcePath);
    result.virtualPath = VirtualPathFor(prefix, stored ? *stored : sourcePath);

    std::vector<uint8_t> bytes;
    if (!paks.Read(sourcePath, bytes))
    {
        result.error = "not in any .pak";
        return result;
    }

    Fmt::MLOD::WriteModel model;
    if (!ConvertXob(bytes, options, baker, model, result.stats, result.error))
        return result;

    const std::filesystem::path target = OnDiskPathFor(outputRoot, result.virtualPath);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    try
    {
        Fmt::MLODWriter::write(model, target.string());
    }
    catch (const std::exception& e)
    {
        result.error = std::string("MLOD writer refused it: ") + e.what();
        return result;
    }
    result.written = true;
    result.roundTrip = VerifyRoundTrip(target.string(), model);
    return result;
}

void PrintModelLine(const ModelResult& result)
{
    std::cout << "  " << std::left << std::setw(94) << result.virtualPath << " ";
    if (!result.error.empty())
    {
        std::cout << "FAILED: " << result.error << "\n";
        return;
    }
    const ConvertStats& s = result.stats;
    std::cout << std::right << std::setw(7) << s.triangles << " tri " << std::setw(7) << s.points << " vtx  LOD"
              << s.lodIndex << "/" << s.lodCount << " thr " << std::fixed << std::setprecision(3) << s.threshold
              << "  bbox " << std::setprecision(2) << s.Extent(0) << " x " << s.Extent(1) << " x " << s.Extent(2)
              << " m  tex " << s.partsTextured << "/" << s.parts << "  "
              << (result.roundTrip.ok ? "round-trips" : "ROUND-TRIP: " + result.roundTrip.reason) << "\n";
}

//! The one number that says whether the models will draw textured: a part with no
//! `.paa` in its face slot is a section that reaches the renderer with texture_id 0
//! and logs "section draws UNTEXTURED".
void PrintTextureReport(const TextureStats& stats, size_t partsTextured, size_t partsTotal)
{
    std::cout << "\nTextures:   " << stats.materialsBound << " / " << stats.materialsSeen
              << " materials resolved an albedo; " << stats.filesWritten << " .paa written (" << stats.filesWithAlpha
              << " with a cutout mask), " << std::fixed << std::setprecision(1)
              << static_cast<double>(stats.bytesWritten) / 1e6 << " MB.\n";
    std::cout << "            " << partsTextured << " / " << partsTotal
              << " written parts name a texture; the rest will draw UNTEXTURED.\n";
    if (stats.baseLayerOnly)
        std::cout << "            " << stats.baseLayerOnly
                  << " MatPBRMulti materials bound their BCR_1 base layer only (the mask blend is not reproduced).\n";
    if (!stats.failures.empty())
    {
        std::cout << "Materials that bound nothing:\n";
        for (const auto& [reason, count] : stats.failures)
            std::cout << "  " << std::left << std::setw(44) << reason << count << "\n";
    }
}

// ---------------------------------------------------------------------------
// Subcommand: inspect
// ---------------------------------------------------------------------------

int Inspect(const std::string& root, const std::string& filter, int limit)
{
    PakSet paks;
    if (!paks.Open(root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    const std::string needle = LowerCopy(filter);

    int shown = 0;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (!EndsWith(lower, ".xob"))
                continue;
            if (!needle.empty() && lower.find(needle) == std::string::npos)
                continue;
            if (shown >= limit)
                return 0;
            std::vector<uint8_t> bytes;
            if (!archive.Read(entry, bytes))
                continue;
            const XobHeader header = ReadXobHeader(bytes.data(), bytes.size());
            std::cout << entry.path << "\n";
            if (!header.valid())
            {
                std::cout << "  HEAD failed: " << header.error << "\n";
                shown++;
                continue;
            }
            std::cout << "  head bbox " << std::fixed << std::setprecision(3) << header.bboxMin.x << " "
                      << header.bboxMin.y << " " << header.bboxMin.z << "  ..  " << header.bboxMax.x << " "
                      << header.bboxMax.y << " " << header.bboxMax.z << "   (" << (header.bboxMax.x - header.bboxMin.x)
                      << " x " << (header.bboxMax.y - header.bboxMin.y) << " x "
                      << (header.bboxMax.z - header.bboxMin.z) << " m)\n";
            for (size_t i = 0; i < header.lods.size(); ++i)
            {
                size_t tris = 0, verts = 0;
                for (const XobPartDesc& part : header.lods[i].parts)
                {
                    tris += part.triangleCount;
                    verts += part.vertexCount;
                }
                std::cout << "  lod" << i << " thr " << std::setprecision(4) << header.lods[i].threshold << "  parts "
                          << header.lods[i].partCount << "  tris " << tris << "  verts " << verts
                          << (i == FinestLodIndex(header) ? "   <- taken" : "") << "\n";
            }
            for (const XobMaterial& material : header.materials)
                std::cout << "  material " << material.path << "\n";
            shown++;
        }
    }
    if (shown == 0)
        std::cerr << "No .xob matched '" << filter << "'.\n";
    return shown == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Subcommand: convert
// ---------------------------------------------------------------------------

int Convert(const std::string& root, const std::string& filter, const std::string& outputRoot,
            const std::string& prefix, int limit, const ConvertOptions& options)
{
    PakSet paks;
    if (!paks.Open(root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    const std::string needle = LowerCopy(filter);

    std::vector<std::string> targets;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (EndsWith(lower, ".xob") && (needle.empty() || lower.find(needle) != std::string::npos))
                targets.push_back(entry.path);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (static_cast<int>(targets.size()) > limit)
        targets.resize(static_cast<size_t>(limit));

    TextureBaker baker(paks, outputRoot, options.textures);
    size_t taken = 0;
    size_t partsTextured = 0, partsTotal = 0;
    for (const std::string& path : targets)
    {
        const ModelResult result =
            ConvertOne(paks, path, outputRoot, prefix, options, options.textures.enabled ? &baker : nullptr);
        PrintModelLine(result);
        partsTextured += result.stats.partsTextured;
        partsTotal += result.stats.parts;
        if (result.written && result.roundTrip.ok)
            taken++;
    }
    std::cout << "\n" << taken << " / " << targets.size() << " models converted and round-tripped.\n";
    if (options.textures.enabled)
        PrintTextureReport(baker.Stats(), partsTextured, partsTotal);
    return taken == targets.size() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Subcommand: export
// ---------------------------------------------------------------------------

struct ExportOptions
{
    std::string root;
    std::string world = "worlds/Eden/Eden.ent";
    std::string outputRoot;
    std::string manifest;
    std::string prefix = "reforger\\everon\\models";
    std::string resmapCache;
    size_t maxModels = 50;
    size_t maxPlacements = 20000;
    ConvertOptions convert;
};

int Export(const ExportOptions& options)
{
    PakSet paks;
    if (!paks.Open(options.root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    std::cout << "Archives:   " << paks.ArchiveCount() << " .pak, " << paks.EntryCount() << " entries\n";

    // --- the world -------------------------------------------------------
    std::vector<uint8_t> worldBytes;
    std::string worldPath = options.world;
    if (!paks.Read(worldPath, worldBytes))
    {
        // Fall back to a substring search so `-w Eden` works as well as a full path.
        const std::string needle = LowerCopy(options.world);
        worldPath.clear();
        for (const PakArchive& archive : paks.Archives())
        {
            if (!archive.IsOpen())
                continue;
            for (const PakEntry& entry : archive.Entries())
            {
                const std::string lower = LowerCopy(entry.path);
                if (EndsWith(lower, ".ent") && lower.find(needle) != std::string::npos)
                {
                    if (worldPath.empty() || entry.path.size() < worldPath.size())
                        worldPath = entry.path;
                }
            }
        }
        if (worldPath.empty() || !paks.Read(worldPath, worldBytes))
        {
            std::cerr << "Error: no .ent world matching '" << options.world << "'\n";
            return 1;
        }
    }
    const EbinWorld world = ReadEbin(worldBytes.data(), worldBytes.size());
    if (!world.valid())
    {
        std::cerr << "Error: " << worldPath << " does not close: " << world.error << "\n";
        return 1;
    }
    std::cout << "World:      " << worldPath << "  " << world.placements.size() << " placements, " << world.consumed
              << " / " << worldBytes.size() << " bytes accounted for\n";

    // --- GUID -> resource path -------------------------------------------
    ResourceMap resources;
    bool fromCache = false;
    if (!options.resmapCache.empty() && LoadResourceMapCache(options.resmapCache, resources))
    {
        fromCache = true;
    }
    else
    {
        std::cout << "Scanning text resources for {GUID}path references...\n";
        resources = BuildResourceMap(paks);
        if (!options.resmapCache.empty())
            SaveResourceMapCache(options.resmapCache, resources);
    }
    std::cout << "Resources:  " << resources.byReference.size() << " GUIDs from references + "
              << resources.byOwnId.size() << " from .et ID lines = " << resources.Size()
              << (fromCache ? "  (cache)" : "") << "\n";

    // --- which prefabs matter --------------------------------------------
    std::unordered_map<std::string, size_t> byPrefab;
    size_t withoutPrefab = 0, withoutPosition = 0;
    for (const EbinPlacement& placement : world.placements)
    {
        if (!placement.hasPosition)
        {
            withoutPosition++;
            continue;
        }
        if (placement.prefabGuid.empty())
        {
            withoutPrefab++;
            continue;
        }
        byPrefab[placement.prefabGuid]++;
    }
    std::vector<std::pair<std::string, size_t>> ranked(byPrefab.begin(), byPrefab.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b)
              { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::cout << "Prefabs:    " << ranked.size() << " distinct GUIDs; " << withoutPosition
              << " placements have no transform, " << withoutPrefab << " no prefab\n\n";

    // --- resolve and convert the top N -----------------------------------
    std::cout << "Converting the " << options.maxModels << " most-placed prefabs that resolve to a model:\n";
    std::unordered_map<std::string, std::string> modelOfPrefab;  //!< prefab GUID -> written virtual path
    std::unordered_map<std::string, std::string> convertedByXob; //!< .xob path -> virtual path
    std::map<std::string, size_t> failures;                      //!< reason -> count
    std::vector<ModelResult> results;
    size_t attempted = 0, roundTripped = 0;
    size_t coveredPlacements = 0;
    TextureBaker baker(paks, options.outputRoot, options.convert.textures);

    for (const auto& [guid, count] : ranked)
    {
        if (convertedByXob.size() >= options.maxModels)
            break;
        const std::string* etPath = resources.Find(guid);
        if (!etPath)
        {
            failures["prefab GUID in no resource map"]++;
            continue;
        }
        const PrefabResolution resolved = ResolvePrefabModel(paks, *etPath);
        if (resolved.xobPath.empty())
        {
            failures[resolved.reason.substr(0, resolved.reason.find(':'))]++;
            continue;
        }
        const auto already = convertedByXob.find(LowerCopy(resolved.xobPath));
        if (already != convertedByXob.end())
        {
            modelOfPrefab[guid] = already->second;
            coveredPlacements += count;
            continue;
        }
        if (!paks.Has(resolved.xobPath))
        {
            failures[".xob not in any .pak"]++;
            continue;
        }

        attempted++;
        ModelResult result = ConvertOne(paks, resolved.xobPath, options.outputRoot, options.prefix, options.convert,
                                        options.convert.textures.enabled ? &baker : nullptr);
        std::cout << "  " << std::right << std::setw(6) << count << "x ";
        PrintModelLine(result);
        if (!result.written || !result.roundTrip.ok)
        {
            failures[result.error.empty() ? "round-trip mismatch" : "conversion failed"]++;
            continue;
        }
        roundTripped++;
        convertedByXob[LowerCopy(resolved.xobPath)] = result.virtualPath;
        modelOfPrefab[guid] = result.virtualPath;
        coveredPlacements += count;
        results.push_back(std::move(result));
    }

    std::cout << "\n"
              << roundTripped << " / " << attempted
              << " attempted models converted AND round-tripped through MLODLoader.\n";
    if (!failures.empty())
    {
        std::cout << "Prefabs skipped while filling the quota:\n";
        for (const auto& [reason, count] : failures)
            std::cout << "  " << std::left << std::setw(44) << reason << count << "\n";
    }

    // --- bounding boxes, in metres ---------------------------------------
    //
    // Reported for every model because a unit-scale error is the failure a parse
    // check cannot see. Reforger `.xob` positions are plain float32 metres
    // (XobModel.hpp: "Positions are plain float32 and are NOT quantised").
    std::cout << "\nBounding boxes (metres, Y up):\n";
    for (const ModelResult& result : results)
    {
        std::cout << "  " << std::left << std::setw(94) << result.virtualPath << std::right << std::fixed
                  << std::setprecision(2) << std::setw(8) << result.stats.Extent(0) << " x " << std::setw(8)
                  << result.stats.Extent(1) << " x " << std::setw(8) << result.stats.Extent(2) << "   Y "
                  << std::setw(8) << result.stats.bboxMin[1] << " .. " << std::setw(8) << result.stats.bboxMax[1]
                  << "\n";
    }

    size_t crossAlong = 0, trianglesTotal = 0;
    for (const ModelResult& result : results)
    {
        crossAlong += result.stats.crossAlongNormal;
        trianglesTotal += result.stats.triangles;
    }
    if (options.convert.textures.enabled)
    {
        size_t partsTextured = 0, partsTotal = 0;
        for (const ModelResult& result : results)
        {
            partsTextured += result.stats.partsTextured;
            partsTotal += result.stats.parts;
        }
        PrintTextureReport(baker.Stats(), partsTextured, partsTotal);
    }

    if (trianglesTotal)
    {
        std::cout << "\nWinding: " << crossAlong << " / " << trianglesTotal
                  << " written triangles have their edge cross product along the stored normal ("
                  << std::setprecision(4) << (static_cast<double>(crossAlong) / static_cast<double>(trianglesTotal))
                  << ").\n";
    }

    // --- the manifest -----------------------------------------------------
    std::error_code ec;
    const auto manifestParent = std::filesystem::path(options.manifest).parent_path();
    if (!manifestParent.empty())
        std::filesystem::create_directories(manifestParent, ec);
    std::ofstream manifest(options.manifest, std::ios::trunc);
    if (!manifest)
    {
        std::cerr << "Error: cannot write " << options.manifest << "\n";
        return 1;
    }

    // Counted first, then sampled at a stride. Enfusion groups a world's records by
    // prefab ($grp), so file order is prefab order and not a walk of the island:
    // taking the first 20,000 eligible placements yields 11,256 of one boulder and
    // 8,696 of one raspberry bush and nothing else. A stride spreads the subset over
    // every converted model and over the whole map.
    size_t eligible = 0;
    for (const EbinPlacement& placement : world.placements)
        if (placement.hasPosition && modelOfPrefab.count(placement.prefabGuid) != 0)
            eligible++;
    const size_t stride = (options.maxPlacements == 0 || eligible <= options.maxPlacements)
                              ? 1
                              : (eligible + options.maxPlacements - 1) / options.maxPlacements;

    size_t emitted = 0, unresolved = 0, noTransform = 0, seen = 0;
    std::map<std::string, size_t> perModel;
    for (const EbinPlacement& placement : world.placements)
    {
        if (!placement.hasPosition)
        {
            noTransform++;
            continue;
        }
        const auto model = modelOfPrefab.find(placement.prefabGuid);
        if (model == modelOfPrefab.end())
        {
            unresolved++;
            continue;
        }
        if (seen++ % stride != 0)
            continue;
        // position[1] is up and angles[1] is yaw, both stated by EbinWorld.hpp and
        // both left in source order here: renaming an axis in the manifest is how a
        // consumer ends up guessing which convention it is in.
        char line[512];
        std::snprintf(line, sizeof(line), "%s\t%.4f\t%.4f\t%.4f\t%.3f\t%.5f", model->second.c_str(),
                      placement.position[0], placement.position[1], placement.position[2], placement.angles[1],
                      placement.scale);
        manifest << line << '\n';
        perModel[model->second]++;
        emitted++;
    }
    manifest.close();

    std::cout << "\nManifest:   " << options.manifest << "\n";
    std::cout << "            " << coveredPlacements << " of "
              << (world.placements.size() - withoutPosition - withoutPrefab) << " world placements are of one of the "
              << convertedByXob.size() << " converted models (" << std::setprecision(1)
              << 100.0 * static_cast<double>(coveredPlacements) /
                     static_cast<double>(world.placements.size() - withoutPosition - withoutPrefab)
              << "%).\n";
    std::cout << "            " << modelOfPrefab.size() << " prefab GUIDs map onto those " << convertedByXob.size()
              << " models -- several prefabs share one mesh and differ only in their material overrides.\n";
    std::cout << "            " << emitted << " written, sampled 1 in " << stride << " of " << eligible
              << " eligible (cap " << options.maxPlacements << ").\n";
    std::cout << "            " << unresolved << " skipped: prefab resolves to no converted model; " << noTransform
              << " skipped: no transform.\n";
    std::cout << "Per model:\n";
    for (const auto& [path, count] : perModel)
        std::cout << "  " << std::left << std::setw(94) << path << " " << count << "\n";

    return roundTripped == attempted && emitted > 0 ? 0 : 1;
}

} // namespace

void XobCommand::Setup(CLI::App& app)
{
    auto* xob = app.add_subcommand("xob", "Arma Reforger .xob models -> MLOD .p3d, and world placement manifests");
    xob->require_subcommand(1);
    {
        static std::string root;
        static std::string filter;
        static int limit = 20;
        auto* cmd = xob->add_subcommand("inspect", "Report a .xob's LOD ladder, bounding box and materials");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring (case-insensitive)");
        cmd->add_option("-n,--limit", limit, "Stop after this many models (default 20)");
        cmd->callback([]() { std::exit(Inspect(root, filter, limit)); });
    }
    {
        static std::string root;
        static std::string filter;
        static std::string output;
        static std::string prefix = "reforger\\everon\\models";
        static int limit = 50;
        static ConvertOptions options;
        auto* cmd = xob->add_subcommand("convert", "Convert matching .xob files to MLOD .p3d and round-trip each one");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring (case-insensitive)");
        cmd->add_option("-o,--output", output, "Output root; files land at <root>/<virtual path>")->required();
        cmd->add_option("--virtual-prefix", prefix, "Virtual path prefix (default reforger\\everon\\models)");
        cmd->add_option("-n,--limit", limit, "Stop after this many models (default 50)");
        cmd->add_option("--resolution", options.resolution, "LOD resolution written into the file (default 1.0)");
        cmd->add_flag("--second-uv", options.secondUv, "Emit both #UVSet# blocks when a part declares two UV sets");
        cmd->add_flag("--flip-winding", options.flipWinding, "Reverse triangle winding (see the winding note)");
        cmd->add_flag("--material-rvmat", options.materialAsRvmat, "Name materials .rvmat instead of .emat");
        cmd->add_flag("--textures", options.textures.enabled,
                      "Bake each material's BCRMap (+OpacityMap) to .paa and name it in the face texture slot");
        cmd->add_option("--texture-prefix", options.textures.prefix,
                        "Virtual path prefix for baked textures (default reforger\\everon\\textures)");
        cmd->add_option("--max-texture", options.textures.maxSize, "Longest baked texture edge (default 1024)");
        cmd->callback([]() { std::exit(Convert(root, filter, output, prefix, limit, options)); });
    }
    {
        static ExportOptions options;
        auto* cmd =
            xob->add_subcommand("export", "Convert the most-placed prefabs of a world and write a placement manifest");
        cmd->add_option("input", options.root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-w,--world", options.world, "World .ent path or substring (default worlds/Eden/Eden.ent)");
        cmd->add_option("-o,--output", options.outputRoot, "Output root for the .p3d tree")->required();
        cmd->add_option("-m,--manifest", options.manifest, "Placement manifest path")->required();
        cmd->add_option("--virtual-prefix", options.prefix, "Virtual path prefix (default reforger\\everon\\models)");
        cmd->add_option("--max-models", options.maxModels, "How many distinct models to convert (default 50)");
        cmd->add_option("--max-placements", options.maxPlacements, "Cap on manifest lines (default 20000)");
        cmd->add_option("--resmap-cache", options.resmapCache,
                        "Read/write the GUID->path map here so a rerun skips the corpus scan");
        cmd->add_option("--resolution", options.convert.resolution, "LOD resolution written into each file");
        cmd->add_flag("--second-uv", options.convert.secondUv, "Emit both #UVSet# blocks");
        cmd->add_flag("--flip-winding", options.convert.flipWinding, "Reverse triangle winding");
        cmd->add_flag("--material-rvmat", options.convert.materialAsRvmat, "Name materials .rvmat instead of .emat");
        cmd->add_flag("--textures", options.convert.textures.enabled,
                      "Bake each material's BCRMap (+OpacityMap) to .paa and name it in the face texture slot");
        cmd->add_option("--texture-prefix", options.convert.textures.prefix,
                        "Virtual path prefix for baked textures (default reforger\\everon\\textures)");
        cmd->add_option("--max-texture", options.convert.textures.maxSize, "Longest baked texture edge (default 1024)");
        cmd->callback([]() { std::exit(Export(options)); });
    }
}

} // namespace PoseidonTools
