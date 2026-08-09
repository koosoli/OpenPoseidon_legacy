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
    MacroAmbient,
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
        case MaterialSlot::MacroAmbient: return "MacroAmbient";
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
    if (suffix == "mca") return "MacroAmbient";
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
    // The Arma 3 vegetation families. MEASURED across the 547 TreeAdv-family
    // materials in the four shipped vegetation PBOs (plants_f, vegetation_f_exp,
    // vegetation_f_enoch, vegetation_f_argo), and corroborated by BI's "Arma 3:
    // Trees" material documentation.
    //
    //   TreeAdv       n=369  Stage1 369/369 normal (301 _nohq, 62 procedural nohq,
    //                                        2 _no, 1 procedural no; 3 _ca outliers)
    //                        Stage2 369/369 MCA   (228 procedural mca, 129 _mca,
    //                                        8 _mc, 3 procedural mc, 1 procedural dt)
    //                        Stage0 absent in 369/369 -- the face texture is the colour
    //   TreeAdvTrunk  n=172  Stage0   4/172 _co
    //                        Stage1 172/172 normal (158 _nohq, 11 _no)
    //                        Stage2 172/172 MCA   (65 procedural mca, 64 _mca, 39 _mc,
    //                                        4 procedural mc)
    //   TreeAdvTrans  n=6    same shape as TreeAdv
    //
    // Stage2 gets its own slot rather than being folded into Super's Macro. The
    // families tag it MCA, not MC, in 357 of 547 procedural stages -- a distinct
    // name the corpus keeps distinct -- and asserting the two are the same texture
    // semantic is a claim with its own evidence requirement. The `_mc`-suffixed
    // minority land here too and report a suffix disagreement rather than being
    // rerouted, because the slot the family assigns is what decides the role.
    //
    // Nothing beyond Stage2 is mapped: exactly one TreeAdv material in the corpus
    // declares Stage3..Stage7, and one witness does not describe a family.
    static const ShaderSchema kTreeAdv{
        "TreeAdv",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::MacroAmbient}};
    static const ShaderSchema kTreeAdvTrunk{
        "TreeAdvTrunk",
        {MaterialSlot::BaseColour, MaterialSlot::NormalMap, MaterialSlot::MacroAmbient}};
    static const ShaderSchema kTreeAdvTrans{
        "TreeAdvTrans",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::MacroAmbient}};
    // Skin is Super-shaped but SHIFTED, and that is the whole reason it needs its own
    // entry rather than borrowing Super's table. MEASURED over all 347 Skin materials in
    // the Arma 3 corpus:
    //
    //   Stage0    0/347  absent -- the face texture is the colour
    //   Stage1  347/347  _nohq
    //   Stage2  346/347  MC     (228 _mc files, 118 procedural mc; 1 procedural dt)
    //   Stage3  340/347  _co    <-- BASE COLOUR, where Super puts Macro
    //   Stage4  347/347  AS     (341 _as files, 6 procedural as)
    //   Stage5  347/347  _smdi
    //   Stage6  347/347  fresnel(...) procedural
    //
    // Stage3 is the trap. Reading this family as Super would bind a head's albedo into
    // the Macro slot and its macro into Detail, which is exactly the kind of plausible,
    // silent wrongness the per-family table exists to prevent.
    //
    // Stage7 is left unmapped although its 2 occurrences are both a3\data_f\env_co.paa --
    // the same environment map Super's Stage7 carries, `_co` name and all. Two materials
    // do not describe a family, and the rule that held for TreeAdv's tail holds here.
    static const ShaderSchema kSkin{
        "Skin",
        {
            MaterialSlot::Count,          // 0
            MaterialSlot::NormalMap,      // 1
            MaterialSlot::Macro,          // 2
            MaterialSlot::BaseColour,     // 3
            MaterialSlot::AmbientShadow,  // 4
            MaterialSlot::SpecularDetail, // 5
            MaterialSlot::Fresnel,        // 6
        }};
    static const ShaderSchema* kAll[] = {&kNormal,  &kNormalMap,    &kSuper,
                                         &kTreeAdv, &kTreeAdvTrunk, &kTreeAdvTrans,
                                         &kSkin};

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
