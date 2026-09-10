// SPDX-License-Identifier: GPL-3.0-or-later
//
// RFG-062 -- ground clutter for a NATIVELY loaded Enfusion world.
//
// The Arma-generation path (TerrainWgpu.cpp, BakeAuthoredGrassMask) reads
// CfgSurfaces -> CfgSurfaceCharacters -> a clutter `.p3d` -> a per-surface atlas,
// and decides WHERE the clutter stands from the material's authored LCA mask. A
// natively loaded Reforger world has none of that: `_a3TerrainMaterials` is left
// zeroed on purpose (LandLoadEnfusion.cpp:1364, "surfaceCount == 0 is what selects
// the legacy single-texture branch"), so that bake returns false before touching a
// byte, and the legacy per-layer name/class classification then answers `0 of 22
// layers grass-capable` because a native surface's Texture is a CreateDynamic
// handle with no name to match. Everon therefore grew nothing at all.
//
// What the native world DOES have is the thing the Arma path spends a mask decode
// to recover: one authored surface index per land cell (`Landscape::GetTexture`),
// and the surface's `.emat` path per palette entry (`GetEnfusionSurface`, added
// with this change). So the bake here is the same answer arrived at without the
// mask -- and Reforger's own data carries the rest of the chain in plain text:
//
//   Terrains/Common/Surfaces/Grass_03.emat
//     ClutterConfig -> Configs/Clutter/Sets/c_set_Grass_03.conf
//       ClutterSet { "Configs/Clutter/Collections/c_collection_grass_03_*.conf" }
//         ClutterCollection { ClutterDefinition { ClutterConfig ...; Occurrence } }
//           ClutterConfig { Model "Assets/Clutter/Vegetation/.../c_*.xob"; Density }
//             the xob's material .emat -> BCRMap + OpacityMap (two .edds files)
//
// Every hop is an Enfusion class record with the same grammar as an `.emat`, and
// every file is in the mounted `.pak`s. Nothing here is a table of guesses: if a
// hop is missing the surface simply grows nothing and the log says which hop.
//
// The colour and the coverage arrive as two separate `.edds` -- Enfusion keeps a
// cutout's alpha in its own file (RFG-019/RFG-023) -- and are merged into one RGBA
// card here, which is the same merge the `enfa|` composite performs for models.

#include "TerrainWgpu.hpp"

#include "EngineWgpu.hpp"

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp> // RFG-090 blade-layer dump
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon
{
namespace
{
using Poseidon::Asset::Formats::Enfusion::EnfusionMount;

// A surface whose clutter run gets more than this many layers would starve the
// rest of the palette; the run is also what the 3-bit count field can address.
constexpr size_t MaxPlantsPerSurface = GrassGeo::AtlasMaxRun;
// Guards a cyclic or absurdly deep `ClutterSet : base` chain.
constexpr int MaxConfDepth = 8;

// ---------------------------------------------------------------------------
// The Enfusion class-record grammar, enough of it.
//
// Deliberately NOT ParseEmat: that reads a FLAT key/value body, and every file on
// this chain nests -- `Clutter { ClutterDefinition "{guid}" { ... } }` -- and one
// of them uses a bare list of quoted strings as a block body. A tolerant reader is
// twenty lines longer than bending the flat one and cannot silently drop a level.
// ---------------------------------------------------------------------------
struct ConfNode
{
    std::string name;                                    //!< "ClutterSet", "ClutterDefinition", ...
    std::string label;                                   //!< the quoted token between the name and `{`
    std::string base;                                    //!< after `:` -- the record this one inherits
    std::vector<std::pair<std::string, std::string>> keys;
    std::vector<std::string> refs;                       //!< bare quoted strings in the body
    std::vector<ConfNode> children;

    const std::string* Key(const char* key) const
    {
        for (const auto& entry : keys)
            if (entry.first == key)
                return &entry.second;
        return nullptr;
    }
    float Number(const char* key, float fallback) const
    {
        const std::string* value = Key(key);
        if (value == nullptr || value->empty())
            return fallback;
        return static_cast<float>(std::atof(value->c_str()));
    }
};

//! Strips Enfusion's `{HEXGUID}` resource prefix. A path without one is returned
//! unchanged; a GUID with no path (`"{625AC78A...}"`, which ClutterDefinition uses
//! as its identity) yields the empty string, which is what makes it a label rather
//! than a reference at the call site.
std::string StripGuid(const std::string& text)
{
    if (text.size() >= 2 && text[0] == '{')
    {
        const size_t close = text.find('}');
        if (close != std::string::npos)
            return text.substr(close + 1);
    }
    return text;
}

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

struct ConfLexer
{
    const std::string& text;
    size_t pos = 0;

    void SkipSpace()
    {
        while (pos < text.size())
        {
            const char ch = text[pos];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            {
                ++pos;
                continue;
            }
            // `//` line comments appear in hand-edited configs.
            if (ch == '/' && pos + 1 < text.size() && text[pos + 1] == '/')
            {
                while (pos < text.size() && text[pos] != '\n')
                    ++pos;
                continue;
            }
            break;
        }
    }

    //! One token. Quoted strings come back WITHOUT their quotes and are flagged.
    bool Next(std::string& out, bool& quoted)
    {
        SkipSpace();
        quoted = false;
        if (pos >= text.size())
            return false;
        const char ch = text[pos];
        if (ch == '"')
        {
            ++pos;
            const size_t start = pos;
            while (pos < text.size() && text[pos] != '"')
                ++pos;
            out = text.substr(start, pos - start);
            if (pos < text.size())
                ++pos;
            quoted = true;
            return true;
        }
        if (ch == '{' || ch == '}' || ch == ':')
        {
            out = std::string(1, ch);
            ++pos;
            return true;
        }
        const size_t start = pos;
        while (pos < text.size())
        {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}' || c == '"')
                break;
            ++pos;
        }
        out = text.substr(start, pos - start);
        return !out.empty();
    }

    size_t Mark() const { return pos; }
    void Reset(size_t mark) { pos = mark; }
};

bool ParseConfBody(ConfLexer& lexer, ConfNode& node);

//! Reads `Name [":" "base"] "{" body "}"`. `name` has already been consumed.
bool ParseConfBlock(ConfLexer& lexer, const std::string& name, ConfNode& out)
{
    out = ConfNode{};
    out.name = name;
    std::string token;
    bool quoted = false;
    size_t mark = lexer.Mark();
    if (!lexer.Next(token, quoted))
        return false;
    if (!quoted && token == ":")
    {
        if (!lexer.Next(token, quoted) || !quoted)
            return false;
        out.base = StripGuid(token);
        mark = lexer.Mark();
        if (!lexer.Next(token, quoted))
            return false;
    }
    if (quoted)
    {
        out.label = token;
        mark = lexer.Mark();
        if (!lexer.Next(token, quoted))
            return false;
    }
    if (quoted || token != "{")
    {
        lexer.Reset(mark);
        return false;
    }
    return ParseConfBody(lexer, out);
}

bool ParseConfBody(ConfLexer& lexer, ConfNode& node)
{
    for (;;)
    {
        std::string token;
        bool quoted = false;
        const size_t mark = lexer.Mark();
        if (!lexer.Next(token, quoted))
            return false; // unterminated
        if (!quoted && token == "}")
            return true;
        if (quoted)
        {
            // A bare quoted string in a body: `ClutterSet { "a.conf" "b.conf" }`.
            node.refs.push_back(StripGuid(token));
            continue;
        }
        if (token == "{" || token == ":")
            continue; // malformed; skip rather than abandon the whole file

        // `Ident ...` -- either a nested block or a key with one or more values.
        const size_t afterName = lexer.Mark();
        ConfNode child;
        if (ParseConfBlock(lexer, token, child))
        {
            node.children.push_back(std::move(child));
            continue;
        }
        lexer.Reset(afterName);
        // Key: take values until the next line-leading identifier, `{` or `}`.
        std::string value;
        for (;;)
        {
            const size_t valueMark = lexer.Mark();
            std::string valueToken;
            bool valueQuoted = false;
            if (!lexer.Next(valueToken, valueQuoted))
                break;
            if (!valueQuoted && (valueToken == "}" || valueToken == "{"))
            {
                lexer.Reset(valueMark);
                break;
            }
            if (valueQuoted)
            {
                value = StripGuid(valueToken);
                break; // a quoted value is always the whole value
            }
            // A number continues the value; anything else starts the next key.
            const char lead = valueToken[0];
            const bool numeric = (lead >= '0' && lead <= '9') || lead == '-' || lead == '+' || lead == '.';
            if (!numeric)
            {
                lexer.Reset(valueMark);
                break;
            }
            if (!value.empty())
                value += ' ';
            value += valueToken;
        }
        node.keys.emplace_back(token, value);
        (void)mark;
    }
}

//! Parses the FIRST top-level record of a `.conf` / `.emat` text.
bool ParseConfText(const std::string& text, ConfNode& out)
{
    ConfLexer lexer{text};
    std::string token;
    bool quoted = false;
    if (!lexer.Next(token, quoted) || quoted)
        return false;
    return ParseConfBlock(lexer, token, out);
}

//! Reads one record out of the mount, its `: base` chain resolved into it. The base
//! contributes what the derived record does not restate, which is exactly how the
//! `_aut` (autumn) variants are authored: they inherit a collection and replace
//! only the plant of each definition.
bool ReadConf(const EnfusionMount& mount, const std::string& path, ConfNode& out, int depth = 0)
{
    if (path.empty() || depth > MaxConfDepth)
        return false;
    std::vector<uint8_t> bytes;
    if (!mount.Read(path, bytes) || bytes.empty())
        return false;
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!ParseConfText(text, out))
        return false;
    if (out.base.empty())
        return true;
    ConfNode parent;
    if (!ReadConf(mount, out.base, parent, depth + 1))
        return true; // the derived record stands on its own
    for (const auto& key : parent.keys)
        if (out.Key(key.first.c_str()) == nullptr)
            out.keys.push_back(key);
    for (const std::string& ref : parent.refs)
        if (std::find(out.refs.begin(), out.refs.end(), ref) == out.refs.end())
            out.refs.push_back(ref);
    // Children merge by (name, label): a derived ClutterDefinition with the same
    // identity replaces the inherited one, everything else is added.
    for (const ConfNode& parentChild : parent.children)
    {
        const bool overridden =
            std::any_of(out.children.begin(), out.children.end(),
                        [&](const ConfNode& mine)
                        { return mine.name == parentChild.name && mine.label == parentChild.label; });
        if (!overridden)
            out.children.push_back(parentChild);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The chain
// ---------------------------------------------------------------------------

//! One plant a surface grows, with the weight the map gave it.
struct NativePlant
{
    std::string conf;    //!< Configs/Clutter/c_<plant>.conf
    std::string model;   //!< the .xob it names
    float occurrence = 1.0f;
    float density = 1.0f;
};

//! Walks a ClutterSet (and the sets it inherits) down to the collections, and the
//! collections down to the individual plants. Duplicates are folded by conf path so
//! a plant named by two collections does not take two atlas layers.
void CollectPlants(const EnfusionMount& mount, const std::string& setPath, std::vector<NativePlant>& out,
                   std::unordered_set<std::string>& seenSets, int depth = 0)
{
    if (setPath.empty() || depth > MaxConfDepth)
        return;
    if (!seenSets.insert(LowerCopy(setPath)).second)
        return;
    ConfNode set;
    if (!ReadConf(mount, setPath, set))
        return;
    // A set's body holds a nested block of the SAME name whose body is the bare
    // list of collection paths, plus (in the mountain sets) further set references.
    std::vector<std::string> refs = set.refs;
    for (const ConfNode& child : set.children)
        refs.insert(refs.end(), child.refs.begin(), child.refs.end());
    for (const std::string& ref : refs)
    {
        const std::string lower = LowerCopy(ref);
        if (lower.find("/sets/") != std::string::npos)
        {
            CollectPlants(mount, ref, out, seenSets, depth + 1);
            continue;
        }
        ConfNode collection;
        if (!ReadConf(mount, ref, collection))
            continue;
        // ClutterCollection { Clutter { ClutterDefinition ... } }
        std::vector<const ConfNode*> definitions;
        for (const ConfNode& child : collection.children)
        {
            if (LowerCopy(child.name) == "clutterdefinition")
                definitions.push_back(&child);
            for (const ConfNode& grandChild : child.children)
                if (LowerCopy(grandChild.name) == "clutterdefinition")
                    definitions.push_back(&grandChild);
        }
        for (const ConfNode* definition : definitions)
        {
            const std::string* confRef = definition->Key("ClutterConfig");
            if (confRef == nullptr || confRef->empty())
                continue;
            ConfNode plantConf;
            if (!ReadConf(mount, *confRef, plantConf))
                continue;
            const std::string* model = plantConf.Key("Model");
            if (model == nullptr || model->empty())
                continue;
            NativePlant plant;
            plant.conf = *confRef;
            plant.model = *model;
            plant.occurrence = definition->Number("Occurrence", 100.0f);
            plant.density = plantConf.Number("Density", 1.0f);
            const auto existing =
                std::find_if(out.begin(), out.end(),
                             [&](const NativePlant& have) { return LowerCopy(have.conf) == LowerCopy(plant.conf); });
            if (existing != out.end())
                existing->occurrence += plant.occurrence;
            else
                out.push_back(std::move(plant));
        }
    }
}

//! The colour and coverage maps of a plant's `.xob`, through its material `.emat`.
bool PlantTextures(const EnfusionMount& mount, const std::string& modelPath, std::string& outColour,
                   std::string& outOpacity)
{
    namespace Xob = Poseidon::Asset::Formats::Enfusion;
    namespace Mat = Poseidon::Asset::Material;
    std::vector<uint8_t> bytes;
    if (!mount.Read(modelPath, bytes) || bytes.empty())
        return false;
    const Xob::XobHeader header = Xob::ReadXobHeader(bytes.data(), bytes.size());
    for (const Xob::XobMaterial& material : header.materials)
    {
        if (material.path.empty())
            continue;
        std::vector<uint8_t> ematBytes;
        if (!mount.Read(material.path, ematBytes))
            continue;
        Mat::EmatMaterial parsed = Mat::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(ematBytes.data()), ematBytes.size()));
        if (!parsed.valid())
            continue;
        Mat::ResolveEmatInheritance(parsed,
                                    [&mount](const std::string& parentPath, std::string& text)
                                    {
                                        std::vector<uint8_t> parentBytes;
                                        if (!mount.Read(parentPath, parentBytes))
                                            return false;
                                        text.assign(reinterpret_cast<const char*>(parentBytes.data()),
                                                    parentBytes.size());
                                        return true;
                                    });
        std::string colour = parsed.TextureOf("BCRMap");
        if (colour.empty())
            colour = parsed.TextureOf("AlbedoMap");
        const std::string opacity = parsed.TextureOf("OpacityMap");
        // Both halves or nothing: a clutter card with no coverage is an opaque
        // square, which is worse on screen than no card at all.
        if (!colour.empty() && !opacity.empty())
        {
            outColour = colour;
            outOpacity = opacity;
            return true;
        }
    }
    return false;
}

//! One `.edds` out of the archives, decoded to RGBA at or below `maxEdge`.
bool DecodeEdds(const EnfusionMount& mount, const std::string& path, int maxEdge, int& width, int& height,
                std::vector<uint8_t>& rgba)
{
    std::vector<uint8_t> bytes;
    if (!mount.Read(path, bytes))
        return false;
    if (!Poseidon::IsEddsBuffer(bytes.data(), bytes.size()))
        return false;
    const Poseidon::EddsImage image = Poseidon::ReadEddsBuffer(bytes.data(), bytes.size());
    if (!image.valid() || image.mipmaps.empty())
        return false;
    size_t level = 0;
    for (size_t i = 0; i < image.mipmaps.size(); ++i)
    {
        level = i;
        if (image.mipmaps[i].width <= maxEdge && image.mipmaps[i].height <= maxEdge)
            break;
    }
    const auto& mip = image.mipmaps[level];
    if (mip.width <= 0 || mip.height <= 0 || mip.data.empty())
        return false;
    rgba.assign(static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height) * 4, 0);
    if (!Poseidon::ConvertPixels(mip.data.data(), rgba.data(), mip.width, mip.height, image.format,
                                 Poseidon::PixelFormat::RGBA8888))
        return false;
    if (image.format == Poseidon::PixelFormat::ARGB8888)
    {
        // The reader hands ARGB8888 back as B,G,R,A; the swap is the caller's, the
        // same way Image::FromFile does it. Getting this wrong is a blue island.
        for (size_t i = 0; i + 2 < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
    }
    width = mip.width;
    height = mip.height;
    return true;
}

//! Colour + coverage -> one 512-square RGBA atlas layer.
bool ComposeCard(const EnfusionMount& mount, const std::string& colourPath, const std::string& opacityPath,
                 uint8_t* layer)
{
    int cw = 0, ch = 0, aw = 0, ah = 0;
    std::vector<uint8_t> colour, opacity;
    if (!DecodeEdds(mount, colourPath, 1024, cw, ch, colour))
        return false;
    if (!DecodeEdds(mount, opacityPath, 1024, aw, ah, opacity))
        return false;
    std::vector<uint8_t> resizedColour(static_cast<size_t>(GrassGeo::AtlasSize) * GrassGeo::AtlasSize * 4u);
    stbir_resize_uint8_linear(colour.data(), cw, ch, cw * 4, resizedColour.data(), GrassGeo::AtlasSize,
                              GrassGeo::AtlasSize, GrassGeo::AtlasSize * 4, STBIR_RGBA);
    std::vector<uint8_t> resizedOpacity(static_cast<size_t>(GrassGeo::AtlasSize) * GrassGeo::AtlasSize * 4u);
    stbir_resize_uint8_linear(opacity.data(), aw, ah, aw * 4, resizedOpacity.data(), GrassGeo::AtlasSize,
                              GrassGeo::AtlasSize, GrassGeo::AtlasSize * 4, STBIR_RGBA);
    // The BRIGHTEST channel, not channel 0 -- the same rule the `enfa|` composite
    // arrived at by measurement (DdsImport.cpp, RFG-023). A coverage map is
    // single-channel, and DecodeEdds's ARGB8888 red/blue swap moves that one channel
    // out of index 0 for part of the corpus; taking index 0 then reads the swapped-in
    // zero, which is a fully transparent card that classifies as opaque and draws as
    // a solid quad. max() does not care which slot the value ended up in. Alpha is
    // not it either: the container's alpha is 255 throughout.
    size_t opaque = 0;
    for (size_t i = 0; i + 3 < resizedColour.size(); i += 4)
    {
        const uint8_t coverage =
            std::max({resizedOpacity[i + 0], resizedOpacity[i + 1], resizedOpacity[i + 2]});
        layer[i + 0] = resizedColour[i + 0];
        layer[i + 1] = resizedColour[i + 1];
        layer[i + 2] = resizedColour[i + 2];
        layer[i + 3] = coverage;
        opaque += coverage > 128 ? 1 : 0;
    }
    // A card that is empty after the merge is a wrong answer that looks like an
    // absent one; refuse it so the surface falls back to layer 0 instead.
    return opaque > 64;
}
} // namespace

// ---------------------------------------------------------------------------
// The bake
// ---------------------------------------------------------------------------
bool TerrainWgpu::BakeEnfusionClutter(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection,
                                    bool reuseDefinitions)
{
    const int paletteSize = land.GetEnfusionSurfaceCount();
    if (paletteSize <= 0)
    {
        return false; // every non-native world takes this exit before touching anything
    }
    // Recorded before any of the early exits below: SetEnfusionClutterEnabled compares
    // against it every frame, and a stale value would re-upload the geography each frame.
    _enfusionRoadClearBaked = Landscape::EnfusionRoadClutterClear();
    if (!_enfusionClutterEnabled)
    {
        LOG_INFO(Graphics, "Wgpu grass: native Enfusion clutter is OFF (dev panel: Grass > Enfusion map clutter)");
        return false;
    }
    const EnfusionMount& mount = EnfusionMount::Instance();
    if (!mount.IsOpen())
    {
        LOG_WARN(Graphics, "Wgpu grass: {} native surfaces but the Enfusion archives are not mounted", paletteSize);
        return false;
    }
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return false;
    }

    const auto started = std::chrono::steady_clock::now();

    // ---- Phase 1: surface -> plants, per palette entry -----------------------
    using SurfaceRun = EnfusionClutterRun;
    auto& runs = _enfusionClutterRuns;
    float maxWeight = _enfusionClutterMaxWeight;
    uint32_t layerCount = _clutterAtlasLayers;
    const char* cacheEnv = std::getenv("WGR_NATIVE_CLUTTER_REUSE");
    const bool reused = reuseDefinitions && !(cacheEnv && cacheEnv[0] == '0') &&
                        runs.size() == static_cast<size_t>(paletteSize) &&
                        !_clutterAtlas.empty() && layerCount > 1 && maxWeight > 0.0f;
    const char* verifyEnv = std::getenv("WGR_NATIVE_CLUTTER_VERIFY");
    const bool verify = reused && verifyEnv && verifyEnv[0] == '1';
    std::vector<uint32_t> verifyInput;
    if (verify) verifyInput = geography;
    if (!reused)
    {
        runs.assign(static_cast<size_t>(paletteSize), {});
        maxWeight = 0.0f;
        constexpr size_t LayerBytes = static_cast<size_t>(GrassGeo::AtlasSize) * GrassGeo::AtlasSize * 4u;
        std::vector<uint8_t> atlas(LayerBytes * GrassGeo::AtlasLayers, 0u);
        layerCount = 1; // layer 0 stays the loose primary card, filled by UploadGrassTuft

        // One decode per MODEL, not per surface: `c_Arrhenatherum_elatius_01` is named
        // by five of Everon's collections and is 2048 square before the crop.
        std::unordered_map<std::string, uint32_t> layerOfModel;
        std::string report;
        int surfacesWithClutter = 0, surfacesWithSet = 0, plantsMissingTexture = 0, plantsDropped = 0;

        // RFG-064: hand out atlas layers by how much GROUND a surface covers, not by its
        // index in the palette.
        //
        // There are 32 layers and Everon's palette is 22 surfaces deep. Walked in palette
        // order, the four forest floors (indices 6-9) take all 32 between them and the
        // island's two commonest grass surfaces reach the loop with nothing left -- so the
        // measured result was "6 surfaces grew a run" and a player standing in a meadow saw
        // bare ground. The forests were not more deserving; they were earlier.
        //
        // Coverage is counted from the same `GetTexture` lookup phase 2 uses, so the order
        // is derived from the world actually being drawn rather than from a guess about
        // which surfaces matter.
        std::vector<size_t> cellsPerSurface(static_cast<size_t>(paletteSize), 0);
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x)
            {
                const int material = land.GetTexture(z, x);
                if (material >= 0 && material < paletteSize)
                    ++cellsPerSurface[static_cast<size_t>(material)];
            }
        std::vector<int> surfaceOrder(static_cast<size_t>(paletteSize));
        for (int i = 0; i < paletteSize; ++i)
            surfaceOrder[static_cast<size_t>(i)] = i;
        std::stable_sort(surfaceOrder.begin(), surfaceOrder.end(), [&cellsPerSurface](int a, int b)
                         { return cellsPerSurface[static_cast<size_t>(a)] > cellsPerSurface[static_cast<size_t>(b)]; });

        for (const int i : surfaceOrder)
        {
            const std::string surface = land.GetEnfusionSurface(i);
            if (surface.empty())
                continue;
            ConfNode material;
            if (!ReadConf(mount, surface, material))
                continue;
            const std::string* setPath = material.Key("ClutterConfig");
            if (setPath == nullptr || setPath->empty())
                continue; // a surface the map says grows nothing -- rock, seabed, asphalt
            ++surfacesWithSet;

            std::vector<NativePlant> plants;
            std::unordered_set<std::string> seenSets;
            CollectPlants(mount, *setPath, plants, seenSets);
            if (plants.empty())
                continue;
            // Commonest first, so a run truncated by the cap keeps the plants that
            // actually cover the ground rather than whichever collection parsed first.
            std::stable_sort(plants.begin(), plants.end(), [](const NativePlant& a, const NativePlant& b)
                             { return a.occurrence * a.density > b.occurrence * b.density; });

            const uint32_t first = layerCount;
            uint32_t count = 0;
            float weight = 0.0f;
            for (const NativePlant& plant : plants)
            {
                if (count >= MaxPlantsPerSurface || layerCount >= GrassGeo::AtlasLayers)
                {
                    ++plantsDropped;
                    continue;
                }
                const std::string modelKey = LowerCopy(plant.model);
                const auto cached = layerOfModel.find(modelKey);
                if (cached != layerOfModel.end())
                {
                    // Already resident, but a run must be CONTIGUOUS, so the card is
                    // copied into this surface's run rather than pointed at.
                    std::memcpy(atlas.data() + static_cast<size_t>(layerCount) * LayerBytes,
                                atlas.data() + static_cast<size_t>(cached->second) * LayerBytes, LayerBytes);
                }
                else
                {
                    std::string colourPath, opacityPath;
                    if (!PlantTextures(mount, plant.model, colourPath, opacityPath))
                    {
                        ++plantsMissingTexture;
                        continue;
                    }
                    if (!ComposeCard(mount, colourPath, opacityPath,
                                     atlas.data() + static_cast<size_t>(layerCount) * LayerBytes))
                    {
                        ++plantsMissingTexture;
                        continue;
                    }
                    layerOfModel.emplace(modelKey, layerCount);
                }
                weight += plant.occurrence * 0.01f * std::max(plant.density, 0.01f);
                ++layerCount;
                ++count;
            }
            if (count == 0)
                continue;
            runs[static_cast<size_t>(i)].layerFirst = static_cast<uint8_t>(first);
            runs[static_cast<size_t>(i)].layerCount = static_cast<uint8_t>(count);
            runs[static_cast<size_t>(i)].weight = weight;
            maxWeight = std::max(maxWeight, weight);
            ++surfacesWithClutter;
            if (report.size() < 700)
            {
                char line[224];
                std::snprintf(line, sizeof(line), "%s%d=%s %u plant(s) @%u w%.2f", report.empty() ? "" : ", ", i,
                              surface.c_str(), count, first, weight);
                report += line;
            }
        }

        LOG_INFO(
            Graphics,
            "Wgpu grass Enfusion clutter: {} of {} palette surfaces name a ClutterConfig, {} grew a run over {} of {} "
            "atlas layers ({} plants had no colour+coverage pair, {} dropped at the {}-per-surface / {}-layer caps) "
            "[{}]",
            surfacesWithSet, paletteSize, surfacesWithClutter, layerCount, GrassGeo::AtlasLayers, plantsMissingTexture,
            plantsDropped, MaxPlantsPerSurface, GrassGeo::AtlasLayers, report.empty() ? "none" : report);

        if (surfacesWithClutter == 0 || layerCount <= 1 || maxWeight <= 0.0f)
        {
            // Nothing usable. Drop the buffer so UploadGrassTuft keeps the loose photo
            // cards rather than uploading 32 blank layers.
            _clutterAtlas.clear();
            _clutterAtlasLayers = 0;
            return false;
        }

        // Unused slots are left at zero here; UploadGrassTuft repeats layer 0 into them
        // once it has filled layer 0, so nothing samples uninitialised memory.
        _clutterAtlas = std::move(atlas);
        _clutterAtlasLayers = layerCount;
        _enfusionClutterMaxWeight = maxWeight;
    }

    // ---- Phase 2: the per-cell answer ---------------------------------------
    //
    // No mask decode: the surface index IS per land cell on a native world
    // (LandLoadEnfusion point-samples the 1 m TMAT into it), so the cell's answer
    // is a lookup. That is the whole reason this bake is cheap where the Arma one
    // costs 700 ms of image decoding.
    // RFG-065: normalise against the TYPICAL surface, not the densest one.
    //
    // Dividing by the maximum lets a single outlier decide what every other surface
    // looks like. Everon's conifer floor scores 14.15 -- seven times a meadow -- so
    // Grass_03 at 2.02 came out at 18 of 127 coverage, about 14%, and with the card
    // coverage factor on top that is roughly three percent of the ground. Measured and
    // visible: the cells were flagged, the atlas was resident, the cards were drawn,
    // and a capture at eye height on a Grass_03 meadow showed bare earth. Nothing was
    // broken; the grass was simply scaled into invisibility by a forest.
    //
    // The median makes the ordinary surface fully covered and lets the outliers
    // saturate, which is the right way round: a meadow should look like a meadow, and
    // a conifer floor is allowed to be the densest thing on the island without
    // defining the scale for everything else.
    float reference = maxWeight;
    {
        std::vector<float> weights;
        weights.reserve(runs.size());
        for (const SurfaceRun& run : runs)
            if (run.layerCount != 0 && run.weight > 0.0f)
                weights.push_back(run.weight);
        if (!weights.empty())
        {
            const size_t middle = weights.size() / 2;
            std::nth_element(weights.begin(), weights.begin() + static_cast<ptrdiff_t>(middle), weights.end());
            reference = weights[middle];
        }
        if (!(reference > 0.0f))
            reference = maxWeight;
    }
    const float invMax = 1.0f / reference;
    size_t grownCells = 0;
    // RFG-027 x RFG-063: roads are draped ribbons, not surfaces, so the cell under a
    // road still names its grass surface and would grow clutter THROUGH the tarmac.
    // The native loader marks the cells each ribbon crosses (Landscape::_enfusionRoadCells,
    // half width + 1 m verge); a marked cell keeps its authored bits for the dev panel
    // but gets no TextureCell. Whole 12.5 m cells go, so a strip of meadow beside a
    // 6-8 m road goes with it -- the honest first cut at this grid. The finer answers
    // are a sub-cell mask (a bit per 12.5/4 m quadrant in the geography word) or a
    // distance-to-road test in the clutter placement shader; neither is built here.
    const bool clearRoads = _enfusionRoadClearBaked && land.HasEnfusionRoadCells();
    size_t roadCleared = 0;
    for (int z = 0; z < n; ++z)
    {
        for (int x = 0; x < n; ++x)
        {
            const size_t cell = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const int material = land.GetTexture(z, x);
            uint32_t word = geography[cell] & ~GrassGeo::BakedMask;
            if (material < 0 || material >= paletteSize)
            {
                geography[cell] = word;
                continue;
            }
            const SurfaceRun& run = runs[static_cast<size_t>(material)];
            const bool selected = material < static_cast<int>(_grassSurfaceEnabled.size()) &&
                                  _grassSurfaceEnabled[static_cast<size_t>(material)];
            word |= GrassGeo::AuthoredCell;
            if (run.layerCount == 0 || run.weight <= 0.0f)
            {
                geography[cell] = word;
                continue;
            }
            const uint32_t quantised =
                static_cast<uint32_t>(std::lround(std::min(run.weight * invMax, 1.0f) * 127.0f));
            word |= (quantised << GrassGeo::CoverageShift) & GrassGeo::CoverageMask;
            word |= (static_cast<uint32_t>(run.layerFirst) << GrassGeo::SurfaceShift) & GrassGeo::SurfaceMask;
            word |= (static_cast<uint32_t>(run.layerCount) << GrassGeo::RunCountShift) & GrassGeo::RunCountMask;
            if (quantised > 0 && selected)
            {
                if (clearRoads && land.GetEnfusionRoadCell(z, x))
                {
                    ++roadCleared;
                }
                else
                {
                    word |= GrassGeo::TextureCell;
                    ++grownCells;
                }
            }
            geography[cell] = word;
        }
    }

    // The dev panel's per-surface list: only surfaces that actually grow something
    // are selectable, the same rule the Arma path uses. Re-derived on a world change
    // only, so a live toggle is not undone by the next re-upload.
    if (refreshSelection && static_cast<int>(_grassSurfaceEnabled.size()) == paletteSize)
    {
        for (int i = 0; i < paletteSize; ++i)
            _grassSurfaceEnabled[static_cast<size_t>(i)] = runs[static_cast<size_t>(i)].layerCount != 0;
    }

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    LOG_INFO(Graphics,
             "Wgpu grass Enfusion clutter: {} of {} land cells grow clutter ({:.1f}%), {} cells cleared under roads "
             "({}), peak surface weight {:.2f}, {} atlas layers, {:.0f} ms, definitions={}",
             grownCells, static_cast<size_t>(n) * static_cast<size_t>(n),
             100.0 * static_cast<double>(grownCells) / static_cast<double>(static_cast<size_t>(n) * n), roadCleared,
             clearRoads ? "road clearing ON" : (land.HasEnfusionRoadCells() ? "road clearing OFF" : "no road mask"),
             maxWeight, layerCount, ms, reused ? "reused" : "rebuilt");
    if (verify)
    {
        auto expectedGeography = geography;
        auto expectedAtlas = _clutterAtlas;
        geography = std::move(verifyInput);
        const bool full = BakeEnfusionClutter(land, geography, refreshSelection, false);
        // Layer 0 and unused slots are filled later by UploadGrassTuft, not by this bake.
        constexpr size_t bytesPerLayer = static_cast<size_t>(GrassGeo::AtlasSize) * GrassGeo::AtlasSize * 4u;
        const bool equal = full == (grownCells > 0) && geography == expectedGeography &&
            _clutterAtlasLayers == layerCount && _clutterAtlas.size() == expectedAtlas.size() &&
            std::equal(expectedAtlas.begin() + bytesPerLayer, expectedAtlas.begin() + layerCount * bytesPerLayer,
                       _clutterAtlas.begin() + bytesPerLayer);
        if (equal) LOG_INFO(Graphics, "Native clutter reuse verification: identical geography and authored atlas");
        else LOG_ERROR(Graphics, "Native clutter reuse verification: MISMATCH");
        return full;
    }
    return grownCells > 0;
}

// RFG-089: Reforger's blade layer, from the file that defines it.
//
// Everon's short ground cover is not a model and not our procedural grass: each grass
// surface `.emat` names a `PlantMat`, a procedural `Grass` material (`Plants 8`,
// `BladesCount 3`, `Height 0.1`) whose BCR + opacity pair is a blade ATLAS, and whose
// `DistributionAtlasUVs` block lists the sub-rectangles (u0 u1 v0 v1) the engine picks
// tufts from, weighted by `Distribution`. The near ring here already draws eight photo
// blade layers (UploadGrassBladeAtlas); this cuts those eight from the world's own atlas
// instead, most-weighted rectangles first, each placed bottom-aligned at the blade's
// width so the tuft's proportions survive (blade_uv v = 1 at the base). Nothing about
// blade geometry, density or wind changes -- only the image on the blade.
//
// Read from the commonest palette surface that names a PlantMat, because the atlas is
// one texture for the whole world and that surface is what most of the ground shows.
namespace
{
struct AtlasRect
{
    float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    float weight = 1.0f;
};

// `DistributionAtlasUVs { u0 u1 v0 v1 ... }` and `Distribution "w w w ..."` out of the raw
// material text: the block is rows of bare numbers, which the key/value parser folds into
// nothing useful, so it is read by hand.
void ReadAtlasRects(const std::string& text, std::vector<AtlasRect>& out)
{
    std::vector<float> weights;
    if (const size_t d = text.find("Distribution \""); d != std::string::npos)
    {
        const size_t open = text.find('"', d);
        const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
        if (open != std::string::npos && close != std::string::npos)
        {
            const std::string list = text.substr(open + 1, close - open - 1);
            size_t at = 0;
            while (at < list.size())
            {
                char* end = nullptr;
                const float v = std::strtof(list.c_str() + at, &end);
                if (end == list.c_str() + at)
                    break;
                weights.push_back(v);
                at = static_cast<size_t>(end - list.c_str());
                while (at < list.size() && list[at] == ' ')
                    ++at;
            }
        }
    }
    const size_t b = text.find("DistributionAtlasUVs");
    if (b == std::string::npos)
        return;
    const size_t open = text.find('{', b);
    const size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
    if (open == std::string::npos || close == std::string::npos)
        return;
    const std::string body = text.substr(open + 1, close - open - 1);
    std::vector<float> numbers;
    size_t at = 0;
    while (at < body.size())
    {
        while (at < body.size() && (body[at] == ' ' || body[at] == '\n' || body[at] == '\r' || body[at] == '\t'))
            ++at;
        if (at >= body.size())
            break;
        char* end = nullptr;
        const float v = std::strtof(body.c_str() + at, &end);
        if (end == body.c_str() + at)
            break;
        numbers.push_back(v);
        at = static_cast<size_t>(end - body.c_str());
    }
    for (size_t i = 0; i + 3 < numbers.size(); i += 4)
    {
        AtlasRect r;
        r.u0 = numbers[i];
        r.u1 = numbers[i + 1];
        r.v0 = numbers[i + 2];
        r.v1 = numbers[i + 3];
        const size_t index = i / 4;
        r.weight = index < weights.size() ? weights[index] : 1.0f;
        if (r.u1 > r.u0 && r.v1 > r.v0)
            out.push_back(r);
    }
}
} // namespace

bool TerrainWgpu::UploadEnfusionBladeAtlas(const Landscape& land)
{
    const int paletteSize = land.GetEnfusionSurfaceCount();
    if (paletteSize <= 0 || !_renderer)
        return false;
    const EnfusionMount& mount = EnfusionMount::Instance();
    if (!mount.IsOpen())
        return false;
    const int n = land.GetLandRange();
    if (n <= 0)
        return false;

    // The commonest surface that names a PlantMat.
    std::vector<size_t> cellsPerSurface(static_cast<size_t>(paletteSize), 0);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const int material = land.GetTexture(z, x);
            if (material >= 0 && material < paletteSize)
                ++cellsPerSurface[static_cast<size_t>(material)];
        }
    std::vector<int> order(static_cast<size_t>(paletteSize));
    for (int i = 0; i < paletteSize; ++i)
        order[static_cast<size_t>(i)] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&cellsPerSurface](int a, int b)
                     { return cellsPerSurface[static_cast<size_t>(a)] > cellsPerSurface[static_cast<size_t>(b)]; });

    std::string plantMat, surfaceName;
    ConfNode    surfaceMat;
    for (const int i : order)
    {
        const std::string surface = land.GetEnfusionSurface(i);
        if (surface.empty())
            continue;
        ConfNode material;
        if (!ReadConf(mount, surface, material))
            continue;
        const std::string* value = material.Key("PlantMat");
        if (value == nullptr || value->empty())
            continue;
        plantMat = StripGuid(*value);
        surfaceName = surface;
        surfaceMat = material;
        break;
    }
    if (plantMat.empty())
    {
        LOG_INFO(Graphics, "Wgpu grass: no palette surface names a PlantMat; near blades keep the stock atlas");
        return false;
    }
    const std::string worldKey = "enfusion:" + plantMat;
    if (_grassBladeAtlasWorld == worldKey)
        return true;

    std::vector<uint8_t> raw;
    if (!mount.Read(plantMat, raw))
    {
        LOG_WARN(Graphics, "Wgpu grass: PlantMat '{}' unreadable; near blades keep the stock atlas", plantMat);
        return false;
    }
    const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    ConfNode mat;
    if (!ParseConfText(text, mat))
        return false;
    const std::string* colourKey = mat.Key("BCRMap");
    const std::string* opacityKey = mat.Key("OpacityMap");
    if (colourKey == nullptr || opacityKey == nullptr)
    {
        LOG_WARN(Graphics, "Wgpu grass: PlantMat '{}' has no BCRMap/OpacityMap pair", plantMat);
        return false;
    }
    std::vector<AtlasRect> rects;
    ReadAtlasRects(text, rects);
    if (rects.empty())
        rects.push_back(AtlasRect{}); // the whole atlas as one tuft, rather than nothing
    std::stable_sort(rects.begin(), rects.end(), [](const AtlasRect& a, const AtlasRect& b) { return a.weight > b.weight; });
    // Distinct rectangles only: the list repeats a rectangle to weight it, and eight copies
    // of one tuft would be a poorer field than eight different ones.
    std::vector<AtlasRect> distinct;
    for (const AtlasRect& r : rects)
    {
        bool seen = false;
        for (const AtlasRect& d : distinct)
            seen = seen || (std::fabs(d.u0 - r.u0) < 1e-4f && std::fabs(d.u1 - r.u1) < 1e-4f &&
                            std::fabs(d.v0 - r.v0) < 1e-4f && std::fabs(d.v1 - r.v1) < 1e-4f);
        if (!seen)
            distinct.push_back(r);
    }

    int cw = 0, ch = 0, aw = 0, ah = 0;
    std::vector<uint8_t> colour, opacity;
    if (!DecodeEdds(mount, StripGuid(*colourKey), 2048, cw, ch, colour) ||
        !DecodeEdds(mount, StripGuid(*opacityKey), 2048, aw, ah, opacity))
    {
        LOG_WARN(Graphics, "Wgpu grass: PlantMat '{}' atlas undecodable; near blades keep the stock atlas", plantMat);
        return false;
    }
    if (aw != cw || ah != ch)
    {
        std::vector<uint8_t> resized(static_cast<size_t>(cw) * ch * 4u);
        stbir_resize_uint8_linear(opacity.data(), aw, ah, aw * 4, resized.data(), cw, ch, cw * 4, STBIR_RGBA);
        opacity.swap(resized);
        aw = cw;
        ah = ch;
    }
    // Opacity sits in the coverage map; the BCR alpha is roughness (measured, RFG-049).
    for (size_t i = 0; i + 3 < colour.size(); i += 4)
        colour[i + 3] = std::max({opacity[i + 0], opacity[i + 1], opacity[i + 2]});

    constexpr int LayerW = 64, LayerH = 256, Layers = 8;
    std::vector<uint8_t> layers(static_cast<size_t>(LayerW) * LayerH * 4u * Layers, 0u);
    std::string report;
    for (int layer = 0; layer < Layers; ++layer)
    {
        const AtlasRect& r = distinct[static_cast<size_t>(layer) % distinct.size()];
        const int x0 = std::clamp(static_cast<int>(r.u0 * cw), 0, cw - 1);
        const int x1 = std::clamp(static_cast<int>(r.u1 * cw), x0 + 1, cw);
        const int y0 = std::clamp(static_cast<int>(r.v0 * ch), 0, ch - 1);
        const int y1 = std::clamp(static_cast<int>(r.v1 * ch), y0 + 1, ch);
        const int rw = x1 - x0, rh = y1 - y0;
        // Crop, then STRETCH to the whole layer. Bottom-aligning at the tuft's own aspect
        // left the plant in the lower third of the blade quad, exactly where the shader's
        // root shade and root occlusion are strongest, and the field came out black. The
        // blade quad is a thin strip; a tuft stretched onto it reads as blade streaks,
        // which at 10 cm is what the eye gets from Reforger's cover too. (The faithful
        // port is Reforger's own geometry -- `Plants 8` x `BladesCount 3` crossed CARDS
        // of the whole tuft -- which is the card path at blade spacing; parked.)
        std::vector<uint8_t> crop(static_cast<size_t>(rw) * rh * 4u);
        for (int y = 0; y < rh; ++y)
            std::memcpy(crop.data() + static_cast<size_t>(y) * rw * 4,
                        colour.data() + (static_cast<size_t>(y0 + y) * cw + x0) * 4, static_cast<size_t>(rw) * 4);
        const int fitH = LayerH;
        std::vector<uint8_t> fitted(static_cast<size_t>(LayerW) * fitH * 4u);
        stbir_resize_uint8_linear(crop.data(), rw, rh, rw * 4, fitted.data(), LayerW, fitH, LayerW * 4, STBIR_RGBA);
        uint8_t* dst = layers.data() + static_cast<size_t>(layer) * LayerW * LayerH * 4u;
        for (int y = 0; y < fitH; ++y)
            std::memcpy(dst + static_cast<size_t>(LayerH - fitH + y) * LayerW * 4,
                        fitted.data() + static_cast<size_t>(y) * LayerW * 4, static_cast<size_t>(LayerW) * 4);
        char line[64];
        std::snprintf(line, sizeof(line), "%s%d:[%.2f-%.2f,%.2f-%.2f]w%.0f", layer ? " " : "", layer, r.u0, r.u1,
                      r.v0, r.v1, r.weight);
        report += line;
    }

    EngineWgpu::AcquireProducerWindow("grass blade atlas");
    wgr_grass_set_blade_atlas(_renderer, LayerW, LayerH, Layers, layers.data());
    _enfusionBladeHeight = std::clamp(mat.Number("Height", 0.0f), 0.0f, 2.0f);
    // RFG-091: the ground colour behind the atlas. Reforger lerps the blade towards the
    // satellite map by `SatMapLerp`; with no satellite pages here, the mean linear albedo
    // of the surface's own BCR (its detail tile) is the stand-in.
    _enfusionBladeTintLerp = 0.0f;
    {
        const std::string* bcrKey = surfaceMat.Key("BCRMap");
        if (bcrKey == nullptr || bcrKey->empty())
            bcrKey = surfaceMat.Key("AlbedoMap");
        int tw = 0, th = 0;
        std::vector<uint8_t> tile;
        if (bcrKey != nullptr && !bcrKey->empty() && DecodeEdds(mount, StripGuid(*bcrKey), 64, tw, th, tile) &&
            tw > 0 && th > 0)
        {
            double sum[3] = {0.0, 0.0, 0.0};
            const size_t texels = static_cast<size_t>(tw) * th;
            for (size_t t = 0; t < texels; ++t)
                for (int c = 0; c < 3; ++c)
                    sum[c] += std::pow(tile[t * 4 + static_cast<size_t>(c)] / 255.0, 2.2); // sRGB -> linear
            for (int c = 0; c < 3; ++c)
                _enfusionBladeTint[c] = static_cast<float>(sum[c] / static_cast<double>(texels));
            _enfusionBladeTintLerp = std::clamp(mat.Number("SatMapLerp", 0.8f), 0.0f, 1.0f);
            LOG_INFO(Graphics,
                     "Wgpu grass: native blades lean {:.0f}% towards the ground colour ({:.3f}, {:.3f}, {:.3f} linear) "
                     "of '{}'",
                     _enfusionBladeTintLerp * 100.0f, _enfusionBladeTint[0], _enfusionBladeTint[1],
                     _enfusionBladeTint[2], StripGuid(*bcrKey));
        }
    }
    // POSEIDON_ENFUSION_BLADES_DUMP=<dir>: the eight layers as PNGs, so "the blades look
    // wrong" can be split into "the crop is wrong" and "the shading is wrong".
    if (const char* dumpDir = std::getenv("POSEIDON_ENFUSION_BLADES_DUMP"); dumpDir != nullptr && *dumpDir != '\0')
    {
        for (int layer = 0; layer < Layers; ++layer)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/enfusion_blade_%d.png", dumpDir, layer);
            Poseidon::PNGWriter::WritePNG(path, LayerW, LayerH, 4, layers.data() + static_cast<size_t>(layer) * LayerW * LayerH * 4u);
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/enfusion_blade_atlas.png", dumpDir);
        Poseidon::PNGWriter::WritePNG(path, cw, ch, 4, colour.data());
        LOG_INFO(Graphics, "Wgpu grass: blade layers dumped to {}", dumpDir);
    }
    _grassBladeAtlasWorld = worldKey;
    LOG_INFO(Graphics,
             "Wgpu grass: near blades from Reforger PlantMat '{}' (surface '{}', atlas {}x{}, {} distinct of {} "
             "rects, Plants {} BladesCount {} Height {:.2f} m) [{}]",
             plantMat, surfaceName, cw, ch, distinct.size(), rects.size(), static_cast<int>(mat.Number("Plants", 0)),
             static_cast<int>(mat.Number("BladesCount", 0)), mat.Number("Height", 0.0f), report);
    return true;
}

void TerrainWgpu::SetEnfusionBladesEnabled(bool enabled)
{
    if (_enfusionBladesEnabled == enabled)
        return;
    _enfusionBladesEnabled = enabled;
    // Drop the key so the next geography upload re-decides which atlas the near ring gets.
    _grassBladeAtlasWorld.clear();
    if (_uploaded != nullptr)
        UploadGeography(*_uploaded);
}

void TerrainWgpu::SetEnfusionClutterEnabled(bool enabled)
{
    // Called every frame from EngineWgpu with the panel's value, so this is also where
    // the road-clearing switch (Landscape::EnfusionRoadClutterClear, set by the dev
    // panel) gets noticed: a move re-runs the same invalidation the clutter flag does.
    // Inert on a world without a road mask -- nothing the bake reads would change.
    const bool roadClearMoved = _uploaded != nullptr && _uploaded->HasEnfusionRoadCells() &&
                                Landscape::EnfusionRoadClutterClear() != _enfusionRoadClearBaked;
    if (_enfusionClutterEnabled == enabled && !roadClearMoved)
    {
        return;
    }
    _enfusionClutterEnabled = enabled;
    // The atlas is what GrassHasMapClutterAtlas() answers on, and UploadGrassTuft
    // skips a world it has already uploaded -- so the key has to be dropped or the
    // toggle would move the geography bits and leave the old atlas on the GPU.
    _grassTuftWorld.clear();
    if (_uploaded != nullptr)
    {
        UploadGeography(*_uploaded);
    }
}

} // namespace Poseidon
