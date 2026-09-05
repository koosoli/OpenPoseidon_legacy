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
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <stb_image_resize2.h>

#include <algorithm>
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
bool TerrainWgpu::BakeEnfusionClutter(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection)
{
    const int paletteSize = land.GetEnfusionSurfaceCount();
    if (paletteSize <= 0)
    {
        return false; // every non-native world takes this exit before touching anything
    }
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
    struct SurfaceRun
    {
        float weight = 0.0f; // summed occurrence-weighted density, unnormalised
        uint8_t layerFirst = 0;
        uint8_t layerCount = 0;
    };
    std::vector<SurfaceRun> runs(static_cast<size_t>(paletteSize));
    constexpr size_t LayerBytes = static_cast<size_t>(GrassGeo::AtlasSize) * GrassGeo::AtlasSize * 4u;
    std::vector<uint8_t> atlas(LayerBytes * GrassGeo::AtlasLayers, 0u);
    uint32_t layerCount = 1; // layer 0 stays the loose primary card, filled by UploadGrassTuft

    // One decode per MODEL, not per surface: `c_Arrhenatherum_elatius_01` is named
    // by five of Everon's collections and is 2048 square before the crop.
    std::unordered_map<std::string, uint32_t> layerOfModel;
    std::string report;
    int surfacesWithClutter = 0, surfacesWithSet = 0, plantsMissingTexture = 0, plantsDropped = 0;
    float maxWeight = 0.0f;

    for (int i = 0; i < paletteSize; ++i)
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
        std::stable_sort(plants.begin(), plants.end(),
                         [](const NativePlant& a, const NativePlant& b)
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

    LOG_INFO(Graphics,
             "Wgpu grass Enfusion clutter: {} of {} palette surfaces name a ClutterConfig, {} grew a run over {} of {} "
             "atlas layers ({} plants had no colour+coverage pair, {} dropped at the {}-per-surface / {}-layer caps) "
             "[{}]",
             surfacesWithSet, paletteSize, surfacesWithClutter, layerCount, GrassGeo::AtlasLayers,
             plantsMissingTexture, plantsDropped, MaxPlantsPerSurface, GrassGeo::AtlasLayers,
             report.empty() ? "none" : report);

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

    // ---- Phase 2: the per-cell answer ---------------------------------------
    //
    // No mask decode: the surface index IS per land cell on a native world
    // (LandLoadEnfusion point-samples the 1 m TMAT into it), so the cell's answer
    // is a lookup. That is the whole reason this bake is cheap where the Arma one
    // costs 700 ms of image decoding.
    const float invMax = 1.0f / maxWeight;
    size_t grownCells = 0;
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
                word |= GrassGeo::TextureCell;
                ++grownCells;
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
             "Wgpu grass Enfusion clutter: {} of {} land cells grow clutter ({:.1f}%), peak surface weight {:.2f}, "
             "{} atlas layers, {:.0f} ms",
             grownCells, static_cast<size_t>(n) * static_cast<size_t>(n),
             100.0 * static_cast<double>(grownCells) / static_cast<double>(static_cast<size_t>(n) * n), maxWeight,
             layerCount, ms);
    return grownCells > 0;
}

void TerrainWgpu::SetEnfusionClutterEnabled(bool enabled)
{
    if (_enfusionClutterEnabled == enabled)
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
