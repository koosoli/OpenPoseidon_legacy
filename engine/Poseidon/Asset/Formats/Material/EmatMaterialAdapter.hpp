#pragma once

#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cstdio>
#include <string>
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

// Reads an `.emat` through the same resolver model loading uses, so an addon-bank path
// resolves the way an RVMAT reference does. Returns false when the file is absent --
// which is the ordinary case for every non-DayZ material and must not be an error.
inline bool ReadEmatFile(const std::string& path, EmatMaterial& out)
{
    QIFStreamB file;
    file.AutoOpen(path.c_str());
    if (file.fail())
        return false;
    const int length = file.rest();
    if (length <= 0)
        return false;
    std::string text(static_cast<size_t>(length), '\0');
    file.read(text.data(), length);
    if (file.fail())
        return false;
    out = ParseEmat(text);
    return out.valid();
}

// Fills a slot from a named `.emat` texture key, if the key names one.
inline void AddEmatSlot(TranslatedMaterial& material, const EmatMaterial& emat, const char* key, MaterialSlot slot)
{
    const std::string path = emat.TextureOf(key);
    if (path.empty())
        return;
    TranslatedSlot entry;
    entry.slot = slot;
    entry.present = true;
    // No stage index: an .emat has no stages, and reporting a fake one would make a
    // consumer that logs "Stage N" print a number no file contains.
    entry.sourceStage = -1;
    entry.texture = RvTextureRef::Parse(path);
    entry.channels = ChannelsForSlot(slot);
    material.slots.push_back(std::move(entry));
}

// Translates an `.emat` into the engine's material form. `family` is carried through from
// the rvmat that pointed here.
inline TranslatedMaterial TranslateEmat(const EmatMaterial& emat, const std::string& origin, const std::string& family)
{
    TranslatedMaterial out;
    out.origin = origin;
    out.shaderFamily = family;
    // The material is now described by something real, so a consumer may trust the slots.
    out.schemaKnown = true;

    AddEmatSlot(out, emat, "AlbedoMap", MaterialSlot::BaseColour);
    AddEmatSlot(out, emat, "NormalMap", MaterialSlot::NormalMap);
    AddEmatSlot(out, emat, "EnvironmentMap", MaterialSlot::Environment);

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
    const std::string ematPath = EmatSiblingOf(rvmatPath);
    if (ematPath == rvmatPath)
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
// Throws whatever `ParseRvMaterialFile` throws; callers already cache the miss.
inline TranslatedMaterial LoadTranslatedMaterial(const std::string& rvmatPath)
{
    const RvMaterialSource source = ParseRvMaterialFile(rvmatPath);
    TranslatedMaterial translated = TranslateSemantics(source, TranslateMaterial(source));
    AugmentFromEmatSibling(rvmatPath, translated);
    return translated;
}

} // namespace Poseidon::Asset::Material
