#pragma once

#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Reads a file through the same resolver model loading uses, so an addon-bank path
// resolves the way an RVMAT reference does. Returns false when the file is absent --
// which is the ordinary case for every non-DayZ material and must not be an error.
inline bool ReadEmatText(const std::string& path, std::string& out)
{
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
    AddFirstEmatSlot(out, emat, {"BCRMap", "AlbedoMap"}, MaterialSlot::BaseColour);
    AddFirstEmatSlot(out, emat, {"NMOMap", "GlobalNMOMap", "NTCMap", "NormalMap"}, MaterialSlot::NormalMap);
    AddEmatSlot(out, emat, "MaskMap", MaterialSlot::Mask);
    AddEmatSlot(out, emat, "BCR_1", MaterialSlot::LayerColour1);
    AddEmatSlot(out, emat, "BCR_2", MaterialSlot::LayerColour2);
    AddEmatSlot(out, emat, "BCR_3", MaterialSlot::LayerColour3);
    AddEmatSlot(out, emat, "NMO_1", MaterialSlot::LayerNormal1);
    AddEmatSlot(out, emat, "NMO_2", MaterialSlot::LayerNormal2);
    AddEmatSlot(out, emat, "NMO_3", MaterialSlot::LayerNormal3);
    AddEmatSlot(out, emat, "EnvironmentMap", MaterialSlot::Environment);

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
