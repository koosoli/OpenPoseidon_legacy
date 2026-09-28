#pragma once

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Material
{

// DZ-002 -- routes a DayZ water surface's real material into the existing pipeline.
//
// A DayZ water surface names an `.rvmat` that is a 79-byte stub: two shader ids
// (`CalmWater`) and nothing else. `ShaderSchema` describes that accurately as a family
// with no stages, which is why such a surface reaches the renderer with no textures and
// no colour of its own and comes out a flat fallback. The material it was actually
// authored with is the `.emat` sitting beside it.
//
// This translates the parts of an `.emat` that have a home in `TranslatedMaterial`, and
// deliberately no more. An `.emat` for water carries around thirty keys, and most of them
// -- `SunPower`, `WaterExtinction`, `RefractScale`, `Caustic`, the `Stream*` trio --
// describe a water shading model this engine does not have. Folding them into `specular`
// or `emissive` because those fields are free would be a claim about their meaning, and
// the roadmap's rule for materials is that this layer interprets nothing it has not
// measured. They stay in the `EmatMaterial` where a water renderer can read them.
//
// What does map, and why only these:
//
//   NormalMap       -> MaterialSlot::NormalMap    a normal map is a normal map
//   EnvironmentMap  -> MaterialSlot::Environment  the slot already exists for this
//   AlbedoMap       -> MaterialSlot::BaseColour   present on Sakhal's ice, absent on rivers
//   Color (RGBA)    -> diffuse                    the surface tint and its opacity
//
// The result keeps `shaderFamily` as the rvmat's own claim rather than inventing one:
// the surface really is CalmWater, and downstream code that keys off the family must not
// start seeing a family no rvmat ever wrote.

// True when a translated material bound nothing -- the state a CalmWater rvmat produces,
// and the only state this adapter should be asked to repair.
//
// A procedural stage counts as bound. It has no `path` (it is generated from its own
// name, not loaded), so testing the path alone would call a material that draws an
// authored flat colour "empty" -- and, once this adapter starts emitting one, would make
// it reject its own output.
inline bool BindsNothing(const TranslatedMaterial& material)
{
    for (const auto& slot : material.slots)
    {
        if (!slot.present)
            continue;
        if (slot.texture.isProcedural || !slot.texture.path.empty())
            return false;
    }
    return true;
}

// The `.emat` that sits beside an `.rvmat` of the same name.
inline std::string EmatSiblingOf(const std::string& rvmatPath)
{
    return EmatPathForRvmat(rvmatPath);
}

// True when a material path names an `.emat` outright rather than an `.rvmat`.
//
// This is the whole of defect 2 in the Reforger report. Every material path on Everon
// ends in `.emat` -- the converter carried the Enfusion virtual path through unchanged
// -- and the sibling lookup below compares `EmatPathForRvmat(p)` against `p`, which for
// such a path is the SAME STRING. The old guard read that equality as "there is no
// sibling" and returned false, so the one branch that can read an `.emat` was never
// entered for the 1,189 materials that are nothing but `.emat`s.
inline bool IsEmatPath(std::string_view path)
{
    // Strictly greater: a name that is nothing but the extension has no stem, and
    // treating it as a material would send an empty path to the resolver.
    if (path.size() <= 5)
        return false;
    return EmatMaterial::NameMatches(path.substr(path.size() - 5), ".emat");
}

// Where a converted world's `.emat` files were DEPLOYED, for a material path that
// still names where they LIVED.
//
// A `.xob` part names its material by Enfusion virtual path --
// `Assets/Vegetation/Tree/Alnus_Glutinosa/Data/t_alnus_glutinosa_2f_polyplane.emat` --
// and every converted Everon section carried that string through unchanged. Nothing
// under the game root is called `Assets\`, so `AutoOpen` fails and the material is
// cached as a miss: measured on 2026-08-16 (`.tmp-shadow/reforger/owner-trees.log`),
// 737 "no albedo ... family=" lines, one per distinct `.emat`, family EMPTY because
// TranslateEmat never ran. What DOES exist under the game root is the converter's
// texture tree, `reforger\everon\textures\assets\...`, and the converter now writes
// each material it baked into that same tree, at the same relative path, lower-cased
// (`TextureBaker::WriteEmat` in XobCommand.cpp; the naming is `VirtualNameFor`).
//
// So when the raw path does not open, look for it under each deploy root. This is
// what lets the 1,132 models ALREADY on disk -- whose sections name the raw path --
// open a material without being re-exported; models the converter writes from now on
// name the deployed path directly and never reach this branch.
//
// `POSEIDON_EMAT_ROOTS` overrides the list (`;`-separated); `POSEIDON_EMAT_ROOTS=none`
// disables the fallback, which is the A/B: with it, `family=MatPBR...` on every
// Reforger material line; without it, `family=` (empty), as before.
inline std::vector<std::string> ParseEmatDeployRoots(std::string_view list)
{
    std::vector<std::string> out;
    if (EmatMaterial::NameMatches(list, "none"))
        return out;
    size_t begin = 0;
    while (begin <= list.size())
    {
        const size_t end = list.find(';', begin);
        std::string root(list.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
            root.pop_back();
        if (!root.empty())
            out.push_back(root);
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return out;
}

inline std::vector<std::string>& EmatDeployRootsStorage()
{
    static std::vector<std::string> roots = []
    {
        const char* env = std::getenv("POSEIDON_EMAT_ROOTS");
        return ParseEmatDeployRoots(env && *env ? env : "reforger\\everon\\textures");
    }();
    return roots;
}

inline const std::vector<std::string>& EmatDeployRoots()
{
    return EmatDeployRootsStorage();
}

// Replaces the root list for the rest of the process. Exists for the unit test that
// deploys a material under a temp directory; the game never calls it.
inline void SetEmatDeployRoots(std::vector<std::string> roots)
{
    EmatDeployRootsStorage() = std::move(roots);
}

// `<root>\<path lower-cased, / -> \>` -- the exact spelling `VirtualNameFor` writes,
// so the engine and the converter cannot disagree about where a deployed file is.
inline std::string DeployedEmatPath(const std::string& root, std::string_view rawPath)
{
    std::string tail(rawPath);
    for (char& c : tail)
    {
        if (c == '/')
            c = '\\';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!tail.empty() && tail.front() == '\\')
        tail.erase(tail.begin());
    return root + "\\" + tail;
}

// True when `path` already sits under one of the deploy roots (case-insensitive
// prefix), so the fallback does not stack a root on top of a root.
inline bool IsUnderEmatDeployRoot(std::string_view path)
{
    for (const std::string& root : EmatDeployRoots())
    {
        if (path.size() <= root.size())
            continue;
        if (!EmatMaterial::NameMatches(path.substr(0, root.size()), root))
            continue;
        const char next = path[root.size()];
        if (next == '\\' || next == '/')
            return true;
    }
    return false;
}

// WORKER-SAFE loose-file read: plain std::ifstream, no QIFStreamB, no banks. The
// object-stream preparer's workers may not touch the file server (FileCache is an
// unlocked MRU array; QFBank compiles MT_SAFE out), and the converter-deployed `.emat`s
// they want ARE loose files under the deploy roots, so the pure read covers exactly the
// population a worker can help with. A material that only resolves through a bank
// simply misses here and takes the main-thread path as before. Mirrors ReadEmatText's
// lookup order: the path as written first, then the deployed spelling under each root.
inline bool ReadEmatTextLooseExact(const std::string& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return !out.empty();
}

// Reads a file through the same resolver model loading uses, so an addon-bank path
// resolves the way an RVMAT reference does. Returns false when the file is absent --
// which is the ordinary case for every non-DayZ material and must not be an error.
inline bool ReadEmatTextExact(const std::string& path, std::string& out)
{
    // RFG-014: a natively loaded Enfusion world names its materials INSIDE a `.pak`,
    // which the file layer cannot open. Asked first because when the mount is open
    // the world being drawn came out of it; it is closed for every other world, so
    // this is inert everywhere else.
    {
        const auto& mount = Poseidon::Asset::Formats::Enfusion::EnfusionMount::Instance();
        std::vector<uint8_t> bytes;
        if (mount.IsOpen() && mount.Read(path, bytes))
        {
            out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        }
    }
    QIFStreamB file;
    file.AutoOpen(path.c_str());
    if (file.fail())
        return false;
    const int length = file.rest();
    if (length <= 0)
        return false;
    out.assign(static_cast<size_t>(length), '\0');
    file.read(out.data(), length);
    return !file.fail();
}

// The path exactly as named first; the deployed spelling under each root second. The
// order matters: a path that opens as written (DayZ's `dz\water\...emat` inside a
// bank, or a converter-written `reforger\everon\textures\...emat`) must never be
// shadowed by a root lookup.
inline bool ReadEmatText(const std::string& path, std::string& out)
{
    if (ReadEmatTextExact(path, out))
        return true;
    if (path.empty() || IsUnderEmatDeployRoot(path))
        return false;
    // Absolute paths and drive-letter paths are not virtual paths; a root would
    // produce `root\C:\...`, which is nothing.
    if (path[0] == '\\' || path[0] == '/' || (path.size() > 1 && path[1] == ':'))
        return false;
    for (const std::string& root : EmatDeployRoots())
        if (ReadEmatTextExact(DeployedEmatPath(root, path), out))
            return true;
    return false;
}

inline bool ReadEmatTextLoose(const std::string& path, std::string& out)
{
    if (ReadEmatTextLooseExact(path, out))
        return true;
    if (path.empty() || IsUnderEmatDeployRoot(path))
        return false;
    if (path[0] == '\\' || path[0] == '/' || (path.size() > 1 && path[1] == ':'))
        return false;
    for (const std::string& root : EmatDeployRoots())
        if (ReadEmatTextLooseExact(DeployedEmatPath(root, path), out))
            return true;
    return false;
}

// ReadEmatFile with the loose reader end to end -- inheritance resolution included,
// through the same pure route, so a worker never opens a bank for a parent either.
inline bool ReadEmatFileLoose(const std::string& path, EmatMaterial& out)
{
    std::string text;
    if (!ReadEmatTextLoose(path, text))
        return false;
    out = ParseEmat(text);
    if (!out.valid())
        return false;
    ResolveEmatInheritance(
        out, [](const std::string& parent, std::string& text) { return ReadEmatTextLoose(parent, text); });
    return true;
}

inline bool ReadEmatFile(const std::string& path, EmatMaterial& out)
{
    std::string text;
    if (!ReadEmatText(path, text))
        return false;
    out = ParseEmat(text);
    if (!out.valid())
        return false;
    // 1,891 of Reforger's 10,311 materials (18.3%) take their maps only from a parent.
    // Read without one, the child keeps its own overrides and silently loses everything
    // else -- so resolve here, at the one place that knows how to open a file, rather
    // than leaving each caller to remember. A parent that cannot be found leaves the
    // child as it was: still valid, just unresolved.
    ResolveEmatInheritance(out,
                           [](const std::string& parent, std::string& text) { return ReadEmatText(parent, text); });
    return true;
}

// Fills a slot from a named `.emat` texture key, if the key names one.
// An `.emat` UV transform, e.g. `UVTransform_2 MatUVTransform "{GUID}" TilingU 1.3 TilingV 1.3`.
//
// Enfusion spells the transform as named scalars after a class token, where Real Virtuality
// spells it as three basis rows. Only the diagonal and the offset survive the trip, which is
// all `ResolveMaterialLayers` reads (`aside[0]`, `up[1]`, `pos[0]`, `pos[1]`) and all any
// observed Multi material carries. Rotation is deliberately dropped rather than approximated:
// the engine's layer_uv lane is a scale/offset pair with nowhere to put it, and a rotation
// folded into a scale is worse than an unrotated tile.
//
// Without this every layer samples at the section's raw UV, which collapses a 1.3x brick and a
// 0.7x roof tile onto one frame -- the scale difference IS what the surface reads as.
inline bool EmatUvTransform(const EmatMaterial& emat, const std::string& key, RvUvTransform& out)
{
    const EmatProperty* property = emat.Find(key);
    if (!property)
        return false;
    bool any = false;
    for (size_t i = 0; i + 1 < property->values.size(); ++i)
    {
        const EmatValue& name = property->values[i];
        const EmatValue& value = property->values[i + 1];
        if (name.isNumber || !value.isNumber)
            continue;
        if (EmatMaterial::NameMatches(name.text, "TilingU"))
            out.aside[0] = static_cast<float>(value.number), any = true;
        else if (EmatMaterial::NameMatches(name.text, "TilingV"))
            out.up[1] = static_cast<float>(value.number), any = true;
        else if (EmatMaterial::NameMatches(name.text, "OffsetU"))
            out.pos[0] = static_cast<float>(value.number), any = true;
        else if (EmatMaterial::NameMatches(name.text, "OffsetV"))
            out.pos[1] = static_cast<float>(value.number), any = true;
    }
    out.present = out.present || any;
    return any;
}

// Attach a UV transform, and/or a uvSource, to a slot the material already bound.
inline void SetEmatSlotUv(TranslatedMaterial& material, MaterialSlot slot, const EmatMaterial& emat,
                          const std::string& transformKey, const char* uvSource)
{
    for (TranslatedSlot& entry : material.slots)
    {
        if (entry.slot != slot)
            continue;
        if (!transformKey.empty())
            EmatUvTransform(emat, transformKey, entry.uvTransform);
        if (uvSource && *uvSource)
            entry.uvSource = uvSource;
        return;
    }
}

// RFG-072: does the material's `key` (e.g. `UVSrcGlobalMaps`) name Enfusion's "UV set 2"?
// Enfusion numbers its sets from 1, so "UV set 2" is the `.xob`'s second stream, index 1.
inline bool EmatNamesUvSet2(const EmatMaterial& emat, const char* key)
{
    const EmatProperty* property = emat.Find(key);
    if (!property || property->values.empty() || property->values[0].isNumber)
        return false;
    const std::string& text = property->values[0].text;
    return !text.empty() && text.back() == '2';
}

// RFG-072: attach a layer's `Color_N` to a slot the material already bound. Absent or
// malformed leaves the identity tint in place.
inline void SetEmatSlotTint(TranslatedMaterial& material, MaterialSlot slot, const EmatMaterial& emat,
                            const char* colourKey)
{
    float colour[4] = {};
    if (!emat.Vec4Of(colourKey, colour))
        return;
    for (TranslatedSlot& entry : material.slots)
    {
        if (entry.slot != slot)
            continue;
        for (int c = 0; c < 4; ++c)
            entry.tint[c] = colour[c];
        entry.tintPresent = true;
        return;
    }
}

inline bool AddEmatSlot(TranslatedMaterial& material, const EmatMaterial& emat, const char* key, MaterialSlot slot)
{
    const std::string path = emat.TextureOf(key);
    if (path.empty())
        return false;
    if (material.Find(slot))
        return false; // first key wins; the caller lists them in preference order
    TranslatedSlot entry;
    entry.slot = slot;
    entry.present = true;
    // No stage index: an .emat has no stages, and reporting a fake one would make a
    // consumer that logs "Stage N" print a number no file contains.
    entry.sourceStage = -1;
    entry.texture = RvTextureRef::Parse(path);
    entry.channels = ChannelsForSlot(slot);
    material.slots.push_back(std::move(entry));
    return true;
}

// The first of `keys` the material names, bound into `slot`. Returns the key that won,
// or nullptr when the material names none of them.
inline const char* AddFirstEmatSlot(TranslatedMaterial& material, const EmatMaterial& emat,
                                    std::initializer_list<const char*> keys, MaterialSlot slot)
{
    for (const char* key : keys)
        if (AddEmatSlot(material, emat, key, slot))
            return key;
    return nullptr;
}

inline bool EmatIsOpaqueSolid(const EmatMaterial& emat);

inline EmatAuthoredScalar ReadEmatPbrScalar(const EmatMaterial& emat, const std::string& key)
{
    EmatAuthoredScalar source;
    source.present = emat.FloatOf(key, source.value);
    return source;
}

inline bool IsOriginalBcrTexture(std::string_view path)
{
    constexpr std::string_view suffix = "_BCR.edds";
    return path.size() >= suffix.size() &&
           EmatMaterial::NameMatches(path.substr(path.size() - suffix.size()), suffix);
}

inline bool EmatPbrProvenanceEnabled()
{
    // Read once at process startup. The normal route pays one predictable
    // branch per translated EMAT, with no extra key scans or allocations.
    static const bool enabled = []
    {
        const char* value = std::getenv("WGR_REFORGER_PBR_PROVENANCE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

// Translates an `.emat` into the engine's material form. `family` is carried through from
// the rvmat that pointed here.
inline TranslatedMaterial TranslateEmat(const EmatMaterial& emat, const std::string& origin, const std::string& family)
{
    TranslatedMaterial out;
    out.origin = origin;
    // An `.emat` reached through an `.rvmat` stub keeps the stub's claim; an `.emat`
    // loaded as itself has no rvmat to claim anything, and its class name is the only
    // family it has. Reporting the class beats reporting nothing: every Reforger
    // material logged `family=` (empty) before this, which reads as "unrecognised
    // shader" when the file in fact says `MatPBRBasic`.
    out.shaderFamily = family.empty() ? emat.className : family;
    // The material is now described by something real, so a consumer may trust the slots.
    out.schemaKnown = true;

    // Enfusion's texture keys, MEASURED over the 10,312 `.emat` files Arma Reforger
    // ships. 9,481 of them name at least one texture; only 223 (2.2%) name any of the
    // three DayZ keys this function used to read, which is why every Reforger material
    // bound nothing at all. Counts, most-used first:
    //
    //   BCRMap        4,773   the object's own base colour
    //   MaskMap       4,044   MatPBRMulti's layer blend control
    //   BCR_1         3,905   a per-layer TILING DETAIL colour -- see below
    //   NMO_1         3,768   a per-layer TILING DETAIL normal  -- see below
    //   NMOMap        2,402   the object's own normal
    //   GlobalMCRMap  2,041   see the note below -- deliberately NOT bound
    //   OpacityMap    1,709
    //   NTCMap          334   the vegetation families' normal, under a second name
    //   AlbedoMap / NormalMap / EnvironmentMap    DayZ's spelling, kept
    //
    // The trap is `MatPBRMulti`, 4,191 files and the commonest class in the corpus. It
    // looks like it names four albedos and four normals; it does not. Read one:
    //
    //   MaskMap        Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01_GLOBAL_MASK.edds
    //   GlobalNMOMap   Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01_NMO.edds
    //   BCR_1          Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR.edds
    //   UVTransform_1  TilingU 4  TilingV 4
    //
    // `BCR_1` and `NMO_1` are SHARED LIBRARY TILES -- a one-metre square of painted
    // metal, used by hundreds of unrelated objects, tiled four times over by their own
    // transform. Only `MaskMap` and `GlobalNMOMap` are in the object's own UV layout.
    // So `NMO_1` must NOT reach NormalMap: bound there it is a one-metre tile stretched
    // across a whole jerrycan in the wrong UV frame, which is not a worse normal map
    // than none, it is a normal map that lights every surface wrongly. The layer maps
    // land in the Layer* slots, where the scale that makes them meaningful is recorded
    // beside them and nothing samples them until a shader exists that can blend them.
    // Enfusion numbers its layers 1..4 and the engine has a BASE plus three further
    // layers, so the two line up only one way -- and it is NOT the obvious one.
    //
    // The mask's channels weight layers 2, 3 and 4, and layer 1 takes whatever they
    // leave. Measured on WaterTower_02's global mask: channel means R 0.127, G 0.299,
    // B 0.238, summing to 0.664 with the remainder belonging to layer 1. So layer 1 is
    // the BASE -- which is also why it alone has no `Enabled_1` -- and `BCR_2..4` are
    // the three the mask paints on top.
    //
    // Binding `BCR_1` to LayerColour1 (as this did) shifts every layer down by one: the
    // mask's red channel then weights the base tile against itself and layer 4 is never
    // bound at all. `ResolveMaterialLayers` maps LayerColour1..3 onto the shader's
    // layer_slot[0..2], which the shader weights by mask .r/.g/.b in that order, so the
    // whole chain is fixed by numbering it correctly here.
    // RFG-072: remembered, because `Color_1` belongs to the tile and not to an object's own
    // bake -- a `BCRMap` is already the colour it should be.
    bool baseIsLayerTile = false;
    if (!AddFirstEmatSlot(out, emat, {"BCRMap", "AlbedoMap"}, MaterialSlot::BaseColour))
        baseIsLayerTile = AddEmatSlot(out, emat, "BCR_1", MaterialSlot::BaseColour);
    AddFirstEmatSlot(out, emat, {"NMOMap", "GlobalNMOMap", "NTCMap", "NormalMap"}, MaterialSlot::NormalMap);
    if (EmatPbrProvenanceEnabled() && EmatMaterial::NameMatches(emat.className, "MatPBRMulti") &&
        baseIsLayerTile)
        AddEmatSlot(out, emat, "NMO_1", MaterialSlot::LayerNormal0);
    AddEmatSlot(out, emat, "MaskMap", MaterialSlot::Mask);
    // A layer the material switched off (`Enabled_N 0`) binds nothing: drawing
    // it anyway paints a layer the author removed. Absent means on (layer 1
    // has no switch at all, and dozens of materials name a BCR_N with none),
    // so this only ever removes explicitly disabled layers.
    const auto layerEnabled = [&](const char* key)
    {
        float v = 1.0f;
        return !emat.FloatOf(key, v) || v != 0.0f;
    };
    const bool en2 = layerEnabled("Enabled_2"), en3 = layerEnabled("Enabled_3"), en4 = layerEnabled("Enabled_4");
    if (en2)
    {
        // `MatPBR2Layers` spells its second layer `BCRMap2`/`NMOMap2`
        // (GraniteStone_01); first key wins per slot, and the two families
        // never name each other's keys, so both spellings can stand here.
        if (!AddEmatSlot(out, emat, "BCR_2", MaterialSlot::LayerColour1))
            AddEmatSlot(out, emat, "BCRMap2", MaterialSlot::LayerColour1);
        if (!AddEmatSlot(out, emat, "NMO_2", MaterialSlot::LayerNormal1))
            AddEmatSlot(out, emat, "NMOMap2", MaterialSlot::LayerNormal1);
    }
    if (en3)
    {
        AddEmatSlot(out, emat, "BCR_3", MaterialSlot::LayerColour2);
        AddEmatSlot(out, emat, "NMO_3", MaterialSlot::LayerNormal2);
    }
    if (en4)
    {
        AddEmatSlot(out, emat, "BCR_4", MaterialSlot::LayerColour3);
        AddEmatSlot(out, emat, "NMO_4", MaterialSlot::LayerNormal3);
    }
    AddEmatSlot(out, emat, "EnvironmentMap", MaterialSlot::Environment);

    // Record authored PBR facts without silently converting them to legacy
    // Blinn-Phong constants. The first condition is deliberately strict:
    // OpacityMap/AlphaTest, glass, foliage and decals are not opaque solids;
    // converter-deployed PAAs also are not original BCR textures. In particular,
    // an enfa| face composite has replaced its BCR alpha with coverage.
    if (EmatPbrProvenanceEnabled())
    {
        auto pbr = std::make_shared<EmatPbrProvenance>();
        pbr->baseLayerUsesTile = baseIsLayerTile;
        if (EmatIsOpaqueSolid(emat))
        {
            const std::string baseBcr = emat.TextureOf(baseIsLayerTile ? "BCR_1" : "BCRMap");
            pbr->bcrAlphaIsRoughness[0] = out.Find(MaterialSlot::BaseColour) && IsOriginalBcrTexture(baseBcr);
            const MaterialSlot layerSlots[3] = {MaterialSlot::LayerColour1, MaterialSlot::LayerColour2,
                                                MaterialSlot::LayerColour3};
            for (int i = 1; i < 4; ++i)
            {
                std::string bcr = emat.TextureOf("BCR_" + std::to_string(i + 1));
                if (i == 1 && bcr.empty())
                    bcr = emat.TextureOf("BCRMap2");
                pbr->bcrAlphaIsRoughness[i] = out.Find(layerSlots[i - 1]) && IsOriginalBcrTexture(bcr);
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            const std::string index = std::to_string(i + 1);
            pbr->roughness[i] = ReadEmatPbrScalar(emat, "Roughness_" + index);
            pbr->metalness[i] = ReadEmatPbrScalar(emat, "Metalness_" + index);
        }
        pbr->roughnessScale = ReadEmatPbrScalar(emat, "RoughnessScale");
        pbr->metalnessScale = ReadEmatPbrScalar(emat, "MetalnessScale");
        pbr->roughnessScale2 = ReadEmatPbrScalar(emat, "RoughnessScale2");
        pbr->metalnessScale2 = ReadEmatPbrScalar(emat, "MetalnessScale2");
        out.ematPbr = std::move(pbr);
    }

    // Each layer at its own scale. Without this every layer samples at the section's raw
    // UV, which collapses a 1.3x brick and a 0.7x roof tile onto one frame -- the scale
    // difference IS what the surface reads as.
    SetEmatSlotUv(out, MaterialSlot::BaseColour, emat, "UVTransform_1", nullptr);
    SetEmatSlotUv(out, MaterialSlot::LayerColour1, emat, "UVTransform_2", nullptr);
    SetEmatSlotUv(out, MaterialSlot::LayerColour2, emat, "UVTransform_3", nullptr);
    SetEmatSlotUv(out, MaterialSlot::LayerColour3, emat, "UVTransform_4", nullptr);
    // `MatPBR2Layers` spells its layer transform `BCRMap2UVTransform`
    // (GraniteStone_01, 6x6); used only when the `UVTransform_2` spelling is
    // absent, so a hybrid material keeps its Multi answer.
    if (!emat.Find("UVTransform_2"))
        SetEmatSlotUv(out, MaterialSlot::LayerColour1, emat, "BCRMap2UVTransform", nullptr);
    // RFG-072: and the mask on the SECOND UV set when the material says so.
    //
    // `UVSrcGlobalMaps "UV set 2"` (3,439 of the corpus's materials) names the `.xob`'s
    // second UV stream, and that stream IS the unwrap the GLOBAL_MASK is painted in.
    // Measured on Church_01.xob LOD0, ExtWalls part: set 0 spans u[-15.96..31.64]
    // v[-29.59..17.34] -- a metre-scale tiling unwrap for the one-metre library tiles --
    // and set 1 spans u[0.002..0.999] v[0.001..0.997], the unique unwrap. Every part with
    // a global mask has two sets (69,194 of 93,597 parts corpus-wide, XobModel.hpp).
    //
    // This block used to say the opposite -- that "UV set 2" numbers the source and the
    // converter writes the mask unwrap as set 0 -- on the evidence that
    // `WGR_MATERIAL_MASK_UV1=1` drew diagonal ribbons on church_01. That A/B was vacuous
    // twice over: the converted `church_01.p3d` carries NO `#UVSet#` block (the exporter
    // was run without `--second-uv`, so `uv1` fell back to `uv` in MeshBuild), and the
    // ribbons were the DXT1-compressed mask that the same commit (b5d85209) fixed with
    // `maxMaskSize`. `tex1` is recorded here as the material's answer; the renderer still
    // takes it only when the shape actually carries a second set (ClassifyGpuSection), so
    // a converted model without one keeps uv0 exactly as before.
    if (EmatNamesUvSet2(emat, "UVSrcGlobalMaps"))
        SetEmatSlotUv(out, MaterialSlot::Mask, emat, std::string(), "tex1");
    // And the global normal map on the same second set when the material says
    // so (`UVSrcGlobNormal "UV set 2"`); the renderer takes it only when the
    // shape actually carries a second set, exactly like the mask above.
    if (EmatNamesUvSet2(emat, "UVSrcGlobNormal"))
        out.globalNormalUv1 = true;

    // RFG-072: each layer's `Color_N`, recorded beside its tile for a shader that can
    // multiply per layer. Only for a material served by the NATIVE mount: the converting
    // exporter already bakes the blended colour into the base `.paa` it deploys (MAT-054),
    // and multiplying that again would tint it twice.
    {
        const auto& mount = Poseidon::Asset::Formats::Enfusion::EnfusionMount::Instance();
        if (mount.IsOpen() && mount.Has(origin))
        {
            if (baseIsLayerTile)
                SetEmatSlotTint(out, MaterialSlot::BaseColour, emat, "Color_1");
            SetEmatSlotTint(out, MaterialSlot::LayerColour1, emat, "Color_2");
            SetEmatSlotTint(out, MaterialSlot::LayerColour2, emat, "Color_3");
            SetEmatSlotTint(out, MaterialSlot::LayerColour3, emat, "Color_4");
            // `MatPBR2Layers` spells its layer tint `Color2` (GraniteStone_01);
            // SetEmatSlotTint only fills a slot the material already bound, so
            // this is a no-op for every other family; guarded so a hybrid keeps
            // its `Color_2` answer.
            if (!emat.Find("Color_2"))
                SetEmatSlotTint(out, MaterialSlot::LayerColour1, emat, "Color2");
        }
    }

    // `GlobalMCRMap` (2,041 files) is NOT bound, and the omission is the point. The
    // slots this engine has are the ones whose channel layout was measured -- see
    // MaterialChannels.hpp, where every entry names the sample it came from. What an
    // Enfusion MCR map holds per channel has not been measured here, and the nearest
    // free slot (Macro) already means something else: Super's Stage3 macro COLOUR.
    // Binding a data map there would make a future consumer sample it as colour. The
    // key stays in the `EmatMaterial`, where whoever measures it can reach it.

    // `Color` is the surface's tint and, in its fourth component, its opacity -- pond
    // water is 0.37, Sakhal's ice is 1.0. Diffuse is where the rest of the pipeline
    // already looks for exactly that.
    float colour[4] = {};
    const bool hasColour = emat.Vec4Of("Color", colour);
    if (hasColour)
        out.diffuse = {colour[0], colour[1], colour[2], colour[3]};

    // A river or a pond authors NO albedo texture -- its whole appearance is that one
    // constant. Left there, the surface reaches the renderer with no base colour and
    // draws untextured, which is the state this adapter exists to end.
    //
    // Real Virtuality already has a notation for "this stage is a flat colour", and the
    // engine already generates it: a procedural `color` texture. Emitting one is
    // therefore not a new mechanism, it is the existing one spelled correctly -- the
    // same shape `mil_wallbig_multi` uses for a surface that is entirely one colour.
    // The alternative, teaching the renderer to honour a constant when no albedo is
    // bound, is a change to every backend for a case RV had already solved.
    if (hasColour && !out.Find(MaterialSlot::BaseColour))
    {
        char generated[128];
        std::snprintf(generated, sizeof(generated), "#(argb,8,8,3)color(%.6g,%.6g,%.6g,%.6g,CO)", colour[0], colour[1],
                      colour[2], colour[3]);
        TranslatedSlot entry;
        entry.slot = MaterialSlot::BaseColour;
        entry.present = true;
        entry.sourceStage = -1;
        entry.texture = RvTextureRef::Parse(generated);
        entry.channels = ChannelsForSlot(MaterialSlot::BaseColour);
        out.slots.push_back(std::move(entry));
    }

    // `NormalPower` is the material's normal-map intensity: a multiplier on the
    // decoded tangent-space deviation (crowns use 0 for deliberately flat
    // through 2-3 for exaggerated leaf relief). Parsed, never reinterpreted;
    // the shader scales XY and renormalises, so the 1.0 default is a no-op.
    // Negative values are meaningless; clamp at parse so no consumer repeats it.
    float normalPower = 1.0f;
    if (emat.FloatOf("NormalPower", normalPower))
        out.normalPower = normalPower < 0.0f ? 0.0f : normalPower;

    // `Cull none` disables backface culling for the surface (wire mesh, glass
    // panes authored single-sided). Parsed, never reinterpreted; anything but
    // an explicit `none` keeps the default cull.
    if (const EmatProperty* cull = emat.Find("Cull");
        cull && !cull->values.empty() && !cull->values[0].isNumber)
        out.doubleSided = EmatMaterial::NameMatches(cull->values[0].text, "none");

    // Crown self-occlusion volume. Parsed whole or not at all: a center without
    // dimensions (or dimensions without an intensity) cannot evaluate, and a
    // half-read volume would darken the wrong leaves. Intensity clamps to
    // [0,1] -- occlusion past full black is meaningless.
    float aoCenter[3] = {};
    float aoHeight = 0.0f, aoWidth = 0.0f, aoIntensity = 0.0f;
    if (emat.Vec3Of("GeometryAOCenter", aoCenter) && emat.FloatOf("GeometryAOHeight", aoHeight) &&
        emat.FloatOf("GeometryAOWidth", aoWidth) && emat.FloatOf("GeometryAOIntensity", aoIntensity) &&
        aoIntensity > 0.0f && aoWidth > 0.0f)
    {
        out.crownAoCenter[0] = aoCenter[0];
        out.crownAoCenter[1] = aoCenter[1];
        out.crownAoCenter[2] = aoCenter[2];
        out.crownAoHeight = aoHeight;
        out.crownAoWidth = aoWidth;
        out.crownAoIntensity = aoIntensity < 1.0f ? aoIntensity : 1.0f;
    }

    return out;
}

// Whether an `.emat` describes an ALPHA-TESTED surface -- the Enfusion counterpart of
// Real Virtuality's `TreeAdv`/`TreeAdvTrans` answer that `GpuSectionMaterialUsesLeafCards`
// gives for an rvmat. The renderer asks this only when a texture's alpha statistics are
// ambiguous (Blend, i.e. more than 2% partial-alpha texels): a leaf card with soft mip
// edges reads the same as a window pane, and only the authored material can say which
// it is.
//
// What answers yes, and why -- measured over the 10,312 `.emat` Reforger ships, with
// inheritance resolved (`%TEMP%\rf\emat`, 2026-08-16):
//
//   MatPBRTreeCrown   468 files, 463 name an OpacityMap once the parent is folded in
//                     (the 5 that do not ARE the parents: crown_base, plant_base, ...)
//   Grass             clutter polyplanes; every leaf inherits its OpacityMap
//   AlphaTest > 0     47 files, 44 with an OpacityMap: flags, nets, MatPBRBasic props
//                     that Enfusion alpha-tests. `Particle` also spells a threshold
//                     this way (`AlphaTest 10`), so a particle material would say yes
//                     too -- none of them reaches a model section.
//   OpacityMap        1,715 files. Enfusion binds it as coverage, never as a blend
//                     weight; a material that names one is a cutout.
//
// What does NOT: `BCRMap` alpha. It varies in 1,112 of 1,312 baked albedos and it is
// ROUGHNESS (measured: `WaterTank_01_MLOD_BCR` 99.8% partial alpha, `polyplane_
// MountainGrass_01_aut_BCR` 100% partial -- no cutout is 100% partial). Reading it as
// opacity would punch holes in every wall. The premise "Reforger foliage opacity lives
// in BCR alpha" is false; it lives in `OpacityMap`, and the converter folds that into
// the face texture's alpha already.
inline bool EmatUsesLeafCards(const EmatMaterial& emat)
{
    if (EmatMaterial::NameMatches(emat.className, "MatPBRTreeCrown") ||
        EmatMaterial::NameMatches(emat.className, "Grass"))
        return true;
    if (!emat.TextureOf("OpacityMap").empty())
        return true;
    float alphaTest = 0.0f;
    return emat.FloatOf("AlphaTest", alphaTest) && alphaTest > 0.0f;
}

// Whether an `.emat` is a solid building/prop surface (as opposed to foliage,
// glass, a decal, or anything else whose alpha means something). A section of
// one of these over a Blend-classified texture is an opaque surface with a
// data alpha -- BCR roughness (RFG-033), never coverage -- not translucency,
// so routing it to the blend pass draws the wall see-through by its roughness.
// Allow-listed by family so glass (MatPBRBasicGlass), decals (MatPBRDecal),
// foliage (MatPBRTreeCrown/Trunk, Grass) and particles keep today's routing;
// EmatUsesLeafCards additionally excludes anything naming an OpacityMap or
// AlphaTest, which is where Enfusion puts real cutout coverage.
inline bool EmatIsOpaqueSolid(const EmatMaterial& emat)
{
    if (EmatUsesLeafCards(emat))
        return false;
    return EmatMaterial::NameMatches(emat.className, "MatPBRMulti") ||
           EmatMaterial::NameMatches(emat.className, "MatPBRBasic") ||
           EmatMaterial::NameMatches(emat.className, "MatPBR2Layers");
}

// True when the material class is one of Enfusion's vegetation families -- what a
// consumer that keys on `TreeAdv*` for RV wants to key on for Reforger.
inline bool IsEmatVegetationFamily(std::string_view family)
{
    return EmatMaterial::NameMatches(family, "MatPBRTreeCrown") ||
           EmatMaterial::NameMatches(family, "MatPBRTreeTrunk") || EmatMaterial::NameMatches(family, "Grass");
}

// The material the converter DEPLOYS beside the textures it baked: `resolved` with its
// texture keys rewritten to the files that now exist, and nothing that does not.
//
//   BCRMap          -> `colourPaa`, the baked albedo (`_ca` variant when the OpacityMap
//                      was folded into its alpha). For a MatPBRMulti this is the tinted
//                      library tile the converter chose as the object's colour, so it is
//                      written under BCRMap -- the key TranslateEmat binds as BaseColour
//                      -- and not under the BCR_N it came from, which would land in a
//                      layer slot nothing samples.
//   `normalKey`     -> `normalPaa` when one was baked, under the SAME key the source
//                      used (NMOMap / GlobalNMOMap / NTCMap), all of which TranslateEmat
//                      binds as NormalMap.
//   OpacityMap      -> dropped: it is inside `colourPaa`'s alpha now -- and, so the
//                      deployed file still SAYS it is a cutout (EmatUsesLeafCards reads
//                      OpacityMap), `AlphaTest 1` is written in its place when
//                      `opacityFolded` and the source had no AlphaTest of its own. That
//                      is Enfusion's own key with Enfusion's own meaning: the alpha of
//                      the base colour is coverage.
//   every other texture-valued key -> dropped. A deployed material must not name a
//                      file that is not deployed; `MaskMap` and `BCR_1..4` pointing at
//                      `.edds` inside a `.pak` would be exactly the unresolvable
//                      references this file exists to replace, and a MaskMap that
//                      resolves with no layers behind it would put the section on the
//                      layered path with three empty layers.
//   every non-texture key -> kept verbatim (Color, SpecularMul, AlphaTest, wind, AO,
//                      the UVTransform blocks flattened). They cost nothing and are what
//                      a later shading pass will want to read.
//
// The result carries no parent: `resolved` must already be inheritance-resolved.
inline EmatMaterial MakeDeployedEmat(const EmatMaterial& resolved, const std::string& colourPaa, bool opacityFolded,
                                     const std::string& normalKey, const std::string& normalPaa,
                                     // Every OTHER texture key the converter managed to deploy, as
                                     // (source key -> deployed .paa). This is how MaskMap and the
                                     // BCR_N / NMO_N layer tiles survive the trip: without them the
                                     // deployed file keeps the layer PARAMETERS (Enabled_N, Color_N,
                                     // UVTransform_N) and none of the images they describe, and the
                                     // runtime's blend -- which is complete -- has nothing to blend.
                                     // Measured before this existed: 0 of 1,793 deployed materials
                                     // named a MaskMap or a BCR_N, against 4,044 and 3,905 in the
                                     // source corpus. Empty restores the old collapse exactly.
                                     const std::vector<std::pair<std::string, std::string>>& extraTextures = {})
{
    EmatMaterial out;
    out.className = resolved.className;
    bool droppedOpacity = false;
    const auto textureValue = [](const std::string& path, const std::string& guid)
    {
        EmatValue value;
        value.text = path;
        // A GUID is what makes TextureOf() see a texture at all; the source's own id
        // is kept when there is one so the deployed file still says which Enfusion
        // asset it stands for, and a placeholder is written when there is not.
        value.guid = guid.empty() ? "0000000000000000" : guid;
        return value;
    };
    bool wroteColour = false;
    for (const EmatProperty& property : resolved.properties)
    {
        bool isTexture = false;
        std::string guid;
        for (const EmatValue& value : property.values)
            if (value.isTexture() && !value.text.empty())
            {
                isTexture = true;
                guid = value.guid;
                break;
            }
        if (!isTexture)
        {
            out.properties.push_back(property);
            continue;
        }
        if (EmatMaterial::NameMatches(property.name, "BCRMap") && !colourPaa.empty())
        {
            EmatProperty colour;
            colour.name = "BCRMap";
            colour.values.push_back(textureValue(colourPaa, guid));
            out.properties.push_back(std::move(colour));
            wroteColour = true;
            continue;
        }
        if (!normalKey.empty() && !normalPaa.empty() && EmatMaterial::NameMatches(property.name, normalKey))
        {
            EmatProperty normal;
            normal.name = property.name;
            normal.values.push_back(textureValue(normalPaa, guid));
            out.properties.push_back(std::move(normal));
            continue;
        }
        bool deployedExtra = false;
        for (const auto& [key, paa] : extraTextures)
        {
            if (paa.empty() || !EmatMaterial::NameMatches(property.name, key))
                continue;
            EmatProperty kept;
            kept.name = property.name;
            kept.values.push_back(textureValue(paa, guid));
            out.properties.push_back(std::move(kept));
            deployedExtra = true;
            break;
        }
        if (deployedExtra)
            continue;
        if (EmatMaterial::NameMatches(property.name, "OpacityMap"))
            droppedOpacity = true;
        // OpacityMap, WorldColorizeMap, ... -- and any layer map the converter could not
        // bake -- still not deployed. A key naming a file that is not there is worse than
        // a key that is absent: the engine opens it, fails, and reports a bound material.
    }
    if (!wroteColour && !colourPaa.empty())
    {
        // The source named no BCRMap (a MatPBRMulti took its colour from BCR_1): the
        // baked file still goes under BCRMap so it binds as the base colour.
        EmatProperty colour;
        colour.name = "BCRMap";
        colour.values.push_back(textureValue(colourPaa, ""));
        out.properties.push_back(std::move(colour));
    }
    if (droppedOpacity && opacityFolded && !colourPaa.empty() && !out.Has("AlphaTest"))
    {
        EmatProperty alphaTest;
        alphaTest.name = "AlphaTest";
        EmatValue one;
        one.text = "1";
        one.number = 1.0;
        one.isNumber = true;
        alphaTest.values.push_back(one);
        out.properties.push_back(std::move(alphaTest));
    }
    return out;
}

// The whole operation: given the rvmat path a section named and the material that came
// out of it, replace it from the `.emat` sibling when there is one and the rvmat bound
// nothing. Returns true when it did.
//
// Guarded on `BindsNothing` rather than on the family name so that this can never take a
// material away from a shader family that did author its own stages -- an `.emat` beside
// a fully-specified rvmat is not a thing DayZ writes, but the guard costs nothing and the
// failure it prevents is silent.
// True for the shader family DayZ uses for every water surface it ships.
inline bool IsCalmWater(const TranslatedMaterial& material)
{
    return EmatMaterial::NameMatches(material.shaderFamily, "CalmWater");
}

// DayZ's built-in water appearance, for a `CalmWater` surface that ships no `.emat`.
//
// This is not invented. Enoch's pond and river materials are the only two DayZ authors
// with an `.emat`, and they agree exactly: `Color 0.0392 0.0549 0.0667 0.3725`. Chernarus's
// 4,044 water placements use the older `dz\water\` addon, which ships **no `.emat` at all** --
// its `water_lake.rvmat` is byte-for-byte identical to Enoch's stub. `CalmWater` is a family
// the engine is expected to implement, so the engine has to supply the constant those
// surfaces never carry.
//
// Without this they bind NOTHING and fall back to white, and once a reflective term is
// added on top the ponds render as saturated white blobs -- measured on Chernarus before
// this existed. A surface whose whole appearance is "reflection over a near-black base"
// cannot be given a white base.
inline TranslatedMaterial DefaultCalmWater(const std::string& origin, const std::string& family)
{
    EmatMaterial builtin;
    builtin.className = "MatWaterPool";
    EmatProperty colour;
    colour.name = "Color";
    for (const double component : {0.0392, 0.0549, 0.0667, 0.3725})
    {
        EmatValue value;
        value.number = component;
        value.isNumber = true;
        colour.values.push_back(value);
    }
    builtin.properties.push_back(std::move(colour));
    return TranslateEmat(builtin, origin, family);
}

inline bool AugmentFromEmatSibling(const std::string& rvmatPath, TranslatedMaterial& material)
{
    if (!BindsNothing(material))
        return false;
    // A path that IS an `.emat` is its own material file, not a stub with a sibling.
    const std::string ematPath = IsEmatPath(rvmatPath) ? rvmatPath : EmatSiblingOf(rvmatPath);
    if (ematPath.empty())
        return false;
    EmatMaterial emat;
    if (!ReadEmatFile(ematPath, emat))
    {
        // No `.emat`, but the family still says this is water. Give it the built-in
        // constant rather than leaving it to the white fallback.
        if (!IsCalmWater(material))
            return false;
        material = DefaultCalmWater(rvmatPath, material.shaderFamily);
        return true;
    }
    TranslatedMaterial replacement = TranslateEmat(emat, ematPath, material.shaderFamily);
    if (BindsNothing(replacement))
        return false; // an .emat with no textures leaves the surface no better off
    material = std::move(replacement);
    return true;
}

// Parse, translate, and take the `.emat` sibling into account -- the whole of what a
// backend needs to turn a material path into a bound material.
//
// This exists so the `.emat` step reaches every backend from one place. It was three
// separate lines repeated at each call site before, and a fourth line repeated three
// times is how one backend quietly ends up not doing it.
//
// The text of a minimal Super RVMAT binding one normal map.
//
// This lives here, beside the reader, and not in the converter that calls it, for one
// reason: a format emitted in one place and parsed in another drifts, and the drift is
// silent -- the material still loads, it just stops binding what it meant to. Written
// here it is covered by a test that hands the output straight back to
// `LoadTranslatedMaterial`.
//
// Super, because its stage table is the most thoroughly measured one this engine has
// (ShaderSchema.hpp, over 2,853 materials) and Stage1 is its normal map.
//
// Stage0 is deliberately absent. The albedo arrives through the MLOD face texture, and
// `ClassifyGpuSection` takes a section's cutout decision from the face texture and from
// nothing else -- so moving the colour into the material would quietly turn every
// alpha-tested leaf card back into an opaque rectangle. This file adds a normal and
// changes nothing else.
inline std::string MakeSuperRvmatText(const std::string& origin, const std::string& normalTexture)
{
    std::string text;
    if (!origin.empty())
        text += "// Generated by `poseidon xob` from " + origin + "\n";
    text += "ambient[]={1,1,1,1};\n"
            "diffuse[]={1,1,1,1};\n"
            "forcedDiffuse[]={0,0,0,0};\n"
            // Spelled the way the format spells it. `emissive` is read by nothing.
            "emmisive[]={0,0,0,0};\n"
            "specular[]={0,0,0,0};\n"
            "specularPower=0;\n"
            "PixelShaderID=\"Super\";\n"
            "VertexShaderID=\"Super\";\n"
            "class Stage1\n"
            "{\n"
            " texture=\"" +
            normalTexture +
            "\";\n"
            " uvSource=\"tex\";\n"
            "};\n";
    return text;
}

// Throws whatever `ParseRvMaterialFile` throws; callers already cache the miss.
//
// Defect 1 of the Reforger report is answered at the top: a path ending in `.emat` is
// not an RVMAT and must never be handed to `ParseRvMaterialFile`. That call opens the
// file, fails, and throws "failed to open RVMAT" -- which is what happened for every
// single material on Everon, all 1,189 of them, and is why they all logged an empty
// shader family. `.emat` is a complete material description in its own right, so read
// it as one.
inline TranslatedMaterial LoadTranslatedMaterial(const std::string& rvmatPath)
{
    if (IsEmatPath(rvmatPath))
    {
        EmatMaterial emat;
        if (!ReadEmatFile(rvmatPath, emat))
            throw std::runtime_error("failed to open EMAT: " + rvmatPath);
        return TranslateEmat(emat, rvmatPath, std::string());
    }
    const RvMaterialSource source = ParseRvMaterialFile(rvmatPath);
    TranslatedMaterial translated = TranslateSemantics(source, TranslateMaterial(source));
    AugmentFromEmatSibling(rvmatPath, translated);
    return translated;
}

} // namespace Poseidon::Asset::Material
