#pragma once

#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <array>
#include <string>
#include <vector>

namespace Poseidon::Asset::Material
{

// MAT-030 -- the first real shader-family schema.
//
// AST-014 deliberately refused to give a stage index any meaning, because the
// meaning belongs to the shader family, not to the format. This is where that
// mapping lives, and it exists per family: nothing here is global.
//
// The Super mapping was MEASURED, not assumed, across the 2,853 Super materials in
// the indexed corpora. The corpus is unusually cooperative about this because a
// procedural stage names its own role -- `#(argb,8,8,3)color(0.5,0.5,0.5,1,DT)` is
// literally tagged DT -- so the file texture suffixes and the procedural tags are
// two independent witnesses that agree:
//
//   Stage0  BaseColour       43 present of 2,853; usually absent, the face texture supplies it
//   Stage1  NormalMap        ~99%  (_NOHQ/_NO files, plus procedural NOHQ stand-ins)
//   Stage2  Detail           ~96%  (procedural DT, or a _DT file)
//   Stage3  Macro            ~93%  (procedural MC, or a _MC file)
//   Stage4  AmbientShadow    ~90%  (procedural AS, or an _AS file)
//   Stage5  SpecularDetail   ~99%  (_SMDI; this is SMDI, NOT roughness)
//   Stage6  Fresnel          ~99%  (procedural fresnel/fresnelGlass)
//   Stage7  Environment      2,529 of 2,844 are ca\data\env_land_co.tga
//
// Stage7 is the reason a suffix cannot drive this. Its texture is named `_co`, so
// suffix-based classification calls it a base colour; it is an environment map.
// Role comes from the slot the family assigns, and the suffix is kept only as a
// second opinion that can disagree out loud.

enum class MaterialSlot
{
    BaseColour,
    NormalMap,
    Detail,
    Macro,
    AmbientShadow,
    SpecularDetail,
    Fresnel,
    Environment,
    Count,
};

inline const char* ToString(MaterialSlot slot)
{
    switch (slot)
    {
        case MaterialSlot::BaseColour: return "BaseColour";
        case MaterialSlot::NormalMap: return "NormalMap";
        case MaterialSlot::Detail: return "Detail";
        case MaterialSlot::Macro: return "Macro";
        case MaterialSlot::AmbientShadow: return "AmbientShadow";
        case MaterialSlot::SpecularDetail: return "SpecularDetail";
        case MaterialSlot::Fresnel: return "Fresnel";
        case MaterialSlot::Environment: return "Environment";
        default: return "?";
    }
}

struct MaterialSlotBinding
{
    bool         present = false;
    RvTextureRef texture;
    int          sourceStage = -1;
    std::string  uvSource; // "tex", "tex1", "none" -- selects an AST-011C UV channel

    // What the filename suffix suggests, and whether it agrees with the slot the
    // schema assigned. A disagreement is reported, never used to override: if the
    // schema is wrong, this is the signal, and Stage7's `_co` environment map is
    // the standing example of a suffix that lies.
    std::string suffixRole;
    bool        suffixAgrees = true;
};

struct MaterialIR
{
    std::string origin;
    std::string shaderFamily;

    // False when no schema is registered for this family. Nothing is bound in that
    // case: the source IR is still complete, and inventing a mapping for an
    // unrecognised family is exactly the "silently guess unknown shader stages"
    // the roadmap forbids.
    bool schemaKnown = false;

    std::array<MaterialSlotBinding, static_cast<size_t>(MaterialSlot::Count)> slots;

    // Stages the family's schema does not describe, kept by index so an extra
    // stage is visible rather than dropped.
    std::vector<int> unmappedStages;
    // Slots whose assigned suffix disagrees with the schema.
    std::vector<MaterialSlot> suffixDisagreements;

    const MaterialSlotBinding& Slot(MaterialSlot slot) const
    {
        return slots[static_cast<size_t>(slot)];
    }
};

namespace detail
{
// Suffix -> the role a filename claims. Only used as the second opinion above.
inline std::string SuffixRole(const std::string& raw)
{
    if (raw.empty() || raw[0] == '#')
        return {};
    std::string stem(raw);
    const size_t slash = stem.find_last_of("\\/");
    if (slash != std::string::npos)
        stem = stem.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos)
        stem = stem.substr(0, dot);
    const size_t under = stem.find_last_of('_');
    if (under == std::string::npos)
        return {};
    std::string suffix = stem.substr(under + 1);
    for (char& ch : suffix)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (suffix == "co" || suffix == "ca") return "BaseColour";
    if (suffix == "nohq" || suffix == "no" || suffix == "novhq") return "NormalMap";
    if (suffix == "dt") return "Detail";
    if (suffix == "mc" || suffix == "mco") return "Macro";
    if (suffix == "as" || suffix == "adshq") return "AmbientShadow";
    if (suffix == "smdi" || suffix == "sm") return "SpecularDetail";
    return {};
}
} // namespace detail

// A family's stage -> slot table. An entry of Count means "this family does not
// use that stage", which is different from "the stage is missing".
struct ShaderSchema
{
    const char*               family;
    std::vector<MaterialSlot> stageSlots; // indexed by stage number
};

inline const ShaderSchema* FindShaderSchema(const std::string& family)
{
    // BI's RVMAT documentation describes Normal as "diffuse color modulate".
    // It has one colour stage, unlike Super's multi-stage material program.
    static const ShaderSchema kNormal{
        "Normal",
        {MaterialSlot::BaseColour}};
    // BI's NormalMap material documentation identifies Stage1 as its tangent
    // space normal map. Stage0 remains face-supplied colour and Stage3 is an
    // irradiance lookup, neither of which is a MaterialSlot in this first slice.
    static const ShaderSchema kNormalMap{
        "NormalMap",
        {MaterialSlot::Count, MaterialSlot::NormalMap}};
    static const ShaderSchema kSuper{
        "Super",
        {
            MaterialSlot::BaseColour,     // 0
            MaterialSlot::NormalMap,      // 1
            MaterialSlot::Detail,         // 2
            MaterialSlot::Macro,          // 3
            MaterialSlot::AmbientShadow,  // 4
            MaterialSlot::SpecularDetail, // 5
            MaterialSlot::Fresnel,        // 6
            MaterialSlot::Environment,    // 7
        }};
    static const ShaderSchema* kAll[] = {&kNormal, &kNormalMap, &kSuper};

    for (const ShaderSchema* schema : kAll)
    {
        const std::string name(schema->family);
        if (name.size() != family.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < name.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(family[i])))
            {
                same = false;
                break;
            }
        if (same)
            return schema;
    }
    return nullptr;
}

// Translate a parsed source material into named slots for its shader family.
//
// Only the mapping happens here. No value is reinterpreted -- SMDI lands in
// SpecularDetail and stays SMDI, because turning it into a roughness channel is a
// decision with its own evidence requirement and belongs to MAT-020.
inline MaterialIR TranslateMaterial(const RvMaterialSource& source)
{
    MaterialIR ir;
    ir.origin       = source.origin;
    ir.shaderFamily = source.pixelShaderId;

    const ShaderSchema* schema = FindShaderSchema(source.pixelShaderId);
    if (!schema)
    {
        for (const auto& stage : source.stages)
            ir.unmappedStages.push_back(stage.index);
        return ir;
    }
    ir.schemaKnown = true;

    for (const auto& stage : source.stages)
    {
        if (stage.index < 0 || stage.index >= static_cast<int>(schema->stageSlots.size()))
        {
            ir.unmappedStages.push_back(stage.index);
            continue;
        }
        const MaterialSlot slot = schema->stageSlots[static_cast<size_t>(stage.index)];
        if (slot == MaterialSlot::Count)
        {
            ir.unmappedStages.push_back(stage.index);
            continue;
        }

        MaterialSlotBinding binding;
        binding.present     = true;
        binding.texture     = stage.texture;
        binding.sourceStage = stage.index;
        binding.uvSource    = stage.uvSource;
        binding.suffixRole  = detail::SuffixRole(stage.texture.raw);
        // A procedural texture has no filename to disagree with, and an unknown
        // suffix is silence rather than dissent.
        binding.suffixAgrees =
            binding.suffixRole.empty() || binding.suffixRole == ToString(slot);

        ir.slots[static_cast<size_t>(slot)] = std::move(binding);
        if (!ir.slots[static_cast<size_t>(slot)].suffixAgrees)
            ir.suffixDisagreements.push_back(slot);
    }
    return ir;
}

} // namespace Poseidon::Asset::Material
