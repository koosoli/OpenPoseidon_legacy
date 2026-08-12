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
    // Multi's blend mask. Preserved so a layered material's blend control is visible
    // rather than dropped; nothing samples it yet.
    Mask,
    // Multi's three further surface layers. Layer 0 is BaseColour/NormalMap above,
    // and each layer has its own TexGen -- measured on Takistan's brick houses,
    // where layer 1 is the brick and carries the fine detail the surface reads as.
    // Dropping them is why a Multi surface renders as its rock layer alone.
    LayerColour1,
    LayerColour2,
    LayerColour3,
    LayerNormal1,
    LayerNormal2,
    LayerNormal3,
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
        case MaterialSlot::Mask: return "Mask";
        case MaterialSlot::LayerColour1: return "LayerColour1";
        case MaterialSlot::LayerColour2: return "LayerColour2";
        case MaterialSlot::LayerColour3: return "LayerColour3";
        case MaterialSlot::LayerNormal1: return "LayerNormal1";
        case MaterialSlot::LayerNormal2: return "LayerNormal2";
        case MaterialSlot::LayerNormal3: return "LayerNormal3";
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
    // The stage's own uvTransform, or the TexGenN block it named. Multi gives each
    // surface layer its own, and that scale difference is the detail.
    RvUvTransform uvTransform;

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
    // `_nopx` is DXT5nm exactly as `_nohq` is, MEASURED over the 31 `_nopx` textures in
    // map_data: G and A span the full range and satisfy x^2+y^2<=1 in 31 of 31, and B sits
    // near-constant high (mean 226-254) -- the NOHQ signature. It differs in R, which NOHQ
    // holds flat at 0 and these vary over a narrow band: the "PX" payload, unsampled here.
    if (suffix == "nohq" || suffix == "no" || suffix == "novhq" || suffix == "nopx") return "NormalMap";
    if (suffix == "dt") return "Detail";
    if (suffix == "mc" || suffix == "mco") return "Macro";
    if (suffix == "mca") return "MacroAmbient";
    if (suffix == "mask" || suffix == "maska") return "Mask";
    // `_ads` is the majority spelling in the Multi family (808 of 1,628 Stage10 bindings,
    // against 388 `_as` and 274 `_adshq`), so leaving it out made the commonest form of
    // the commonest layered material report an unrecognised suffix.
    if (suffix == "as" || suffix == "ads" || suffix == "adshq") return "AmbientShadow";
    // `_dtsmdi` is a combined detail+specular texture whose specular half uses SMDI's own
    // channels (measured over 72 of them; see the Multi schema). It claims the same role.
    if (suffix == "smdi" || suffix == "sm" || suffix == "dtsmdi") return "SpecularDetail";
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
    // Multi is a LAYERED material and this schema is deliberately a fraction of it.
    // MEASURED over all 1,628 Multi materials in the Arma 3 corpus:
    //
    //   Stage0  1624/1628  _co 1561, procedural co 37       -- layer 0
    //   Stage1  1628/1628  _co 1592                         -- layer 1
    //   Stage2  1628/1628  _co 1494                         -- layer 2
    //   Stage3  1628/1628  _co 1264                         -- layer 3
    //   Stage4  1628/1628  _mask 1576                       -- the blend mask
    //   Stage5  1628/1628  _dtsmdi 869, proc dtsmdi 694     -- layer 0 detail+specular
    //   Stage6..8          _dtsmdi / procedural dtsmdi      -- layers 1-3
    //   Stage9  1626/1628  _mc 1168, procedural mc 443      -- macro, WHOLE SURFACE
    //   Stage10 1628/1628  _ads 808, _as 388, _adshq 274    -- ambient shadow, whole surface
    //   Stage11 1628/1628  _nohq 1486, procedural nohq 141  -- normal map, whole surface
    //
    // BI's Multimaterial documentation describes the layered half: four material layers, a
    // mask at Stage4 that controls how they blend, and combined detail/specular beyond.
    //
    // Stages 9-11 are the part that is NOT layered, and they were missed while the census
    // stopped at Stage8. They are per-surface exactly as Super's equivalents are, so they
    // take the ordinary slots and no layering question arises for them: a Multi material
    // has a normal map, a macro and an ambient-shadow map like anything else.
    //
    // Stage0 and Stage5 bind as LAYER 0 standing in for the whole surface -- colour and its
    // matching detail+specular. That is not a claim that layer 0 is the whole answer; a
    // masked blend of four layers has no representation in this slot set. The pairing is
    // the point: binding layer 0's colour but a different layer's specular would be worse
    // than binding neither.
    //
    // `_dtsmdi` earns the SpecularDetail slot on measurement, not on its name. Decoding 72
    // of them: A is constant 255 in 72/72, R sits centred on 128 in 64/72 (mean 135 -- a
    // detail multiplier around 0.5), G floors at 0 in 70/72 and varies in 72/72 (mean 27),
    // and B varies in 58/72 (mean 140). That is SMDI's channel assignment exactly -- G the
    // specular level, B gloss -- with the detail occupying R, where SMDI is dead. So the
    // combined texture needs no new channel map and no splitting guess: a consumer reading
    // it as SMDI reads the right channels, and the detail half is simply not sampled.
    //
    // Stages 1-3 and 6-8 stay unmapped because a second, third and fourth layer have no
    // slots; Stage4 gets a named Mask slot so the blend control is preserved rather than
    // silently dropped.
    static const ShaderSchema kMulti{
        "Multi",
        {
            MaterialSlot::BaseColour,     // 0  -- layer 0 colour
            MaterialSlot::LayerColour1,   // 1  -- layer 1 colour
            MaterialSlot::LayerColour2,   // 2  -- layer 2 colour
            MaterialSlot::LayerColour3,   // 3  -- layer 3 colour
            MaterialSlot::Mask,           // 4  -- the blend mask selecting between them
            MaterialSlot::SpecularDetail, // 5  -- layer 0 detail+specular, paired with Stage0
            MaterialSlot::Count,          // 6  -- layer 1 detail+specular, no slot
            MaterialSlot::Count,          // 7  -- layer 2 detail+specular, no slot
            MaterialSlot::Count,          // 8  -- layer 3 detail+specular, no slot
            MaterialSlot::Macro,          // 9  -- whole surface, not per layer
            MaterialSlot::AmbientShadow,  // 10 -- whole surface
            // Measured on ca\structures_e\housek\data\house_k_5_multi.rvmat: Stage11
            // is `rock\wall_01_nohq` at texGen0, pairing with Stage0's
            // `rock\wall_01_co` at texGen0, and 12/13/14 pair the same way with
            // stages 1/2/3. These normals are PER LAYER, not "whole surface" as this
            // table previously said, and 12-14 were not mapped at all.
            MaterialSlot::NormalMap,      // 11 -- layer 0 normal
            MaterialSlot::LayerNormal1,   // 12 -- layer 1 normal
            MaterialSlot::LayerNormal2,   // 13 -- layer 2 normal
            MaterialSlot::LayerNormal3,   // 14 -- layer 3 normal
        }};
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
    // SuperAToC is Super with alpha-to-coverage, and the corpus says so stage for stage.
    // MEASURED over all 8 (7 spelled `SuperAToC`, 1 `SuperAtoc`; the lookup below is
    // case-insensitive so they are one family here):
    //
    //   Stage0 7/7  colour (6 `_ca`, 1 `_co`)   <-- unlike Super, essentially always present
    //   Stage1 7/7  NOHQ    Stage2 7/7 DT/DTSMDI   Stage3 7/7 MC
    //   Stage4 7/7  AS      Stage5 7/7 SMDI        Stage6 7/7 fresnelGlass
    //   Stage7 7/7  a3\data_f\env_land_co.paa
    //
    // Stage0 is the point of adding it. These are the alpha-cutout surfaces -- wire fences,
    // barbed wire, metal doors -- whose colour AND cutout alpha live in the material rather
    // than on the face, so without a schema they draw as untextured white sheets.
    static const ShaderSchema kSuperAToC{
        "SuperAToC",
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
    // The `NormalMap*` cluster. These families NAME their own stage list: everything after
    // "NormalMap" is the stages that follow Stage1, in order. MEASURED across the whole
    // Arma 3 corpus, and the four independent families corroborate each other:
    //
    //   NormalMapSpecularDIMap        n=111  S1 normal 111/111, S2 SMDI 108/111
    //                                        (S0 `_co` in 13, absent in 98)
    //   NormalMapDetailSpecularMap    n=42   S1 normal 42/42, S2 DT 42/42, S3 SM/SMDI 42/42
    //   NormalMapSpecularMap          n=32   S1 normal 32/32, S2 SM/SMDI 32/32
    //   NormalMapDiffuse              n=27   S1 `_nopx` 26/27, S2 `_co` 26/27
    //   NormalMapDetailSpecularDIMap  n=12   S1 normal 12/12, S2 DT 12/12, S3 SMDI 12/12
    //   NormalMapMacroASSpecularDIMap n=4    S1 normal 4/4, S2 MC 4/4, S3 AS 4/4, S4 SMDI 4/4
    //
    // Two are worth calling out. NormalMapDiffuse is the only one of the cluster that carries
    // its albedo in the material, and it puts it at STAGE 2 -- reading it as any of its
    // siblings would bind a ground colour into a specular slot. And its Stage1 `_nopx` decodes
    // exactly like `_nohq` (see SuffixRole), so it earns the ordinary NormalMap slot.
    //
    // `NormalMapDetailMacroASSpecularDIMap` is deliberately absent: 2 materials do not
    // describe a family, the same bar that left Super's and Skin's Stage7 tails unmapped.
    static const ShaderSchema kNormalMapSpecularDIMap{
        "NormalMapSpecularDIMap",
        {MaterialSlot::BaseColour, MaterialSlot::NormalMap, MaterialSlot::SpecularDetail}};
    // Stage0 stays unmapped in the four below: not one material in any of them declares one,
    // so a BaseColour there would be a slot invented rather than measured.
    static const ShaderSchema kNormalMapSpecularMap{
        "NormalMapSpecularMap",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::SpecularDetail}};
    static const ShaderSchema kNormalMapDetailSpecularMap{
        "NormalMapDetailSpecularMap",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::Detail, MaterialSlot::SpecularDetail}};
    static const ShaderSchema kNormalMapDetailSpecularDIMap{
        "NormalMapDetailSpecularDIMap",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::Detail, MaterialSlot::SpecularDetail}};
    static const ShaderSchema kNormalMapMacroASSpecularDIMap{
        "NormalMapMacroASSpecularDIMap",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::Macro, MaterialSlot::AmbientShadow,
         MaterialSlot::SpecularDetail}};
    static const ShaderSchema kNormalMapDiffuse{
        "NormalMapDiffuse",
        {MaterialSlot::Count, MaterialSlot::NormalMap, MaterialSlot::BaseColour}};
    static const ShaderSchema* kAll[] = {&kNormal,
                                         &kNormalMap,
                                         &kSuper,
                                         &kSuperAToC,
                                         &kTreeAdv,
                                         &kTreeAdvTrunk,
                                         &kTreeAdvTrans,
                                         &kSkin,
                                         &kMulti,
                                         &kNormalMapSpecularDIMap,
                                         &kNormalMapSpecularMap,
                                         &kNormalMapDetailSpecularMap,
                                         &kNormalMapDetailSpecularDIMap,
                                         &kNormalMapMacroASSpecularDIMap,
                                         &kNormalMapDiffuse};

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
        // A stage normally names a TexGenN block rather than carrying its own
        // transform, so resolve the link here -- otherwise every layer of a Multi
        // material reports the identity and the per-layer scale is silently lost.
        binding.uvTransform = stage.uvTransform;
        if (!binding.uvTransform.present && stage.texGen >= 0)
        {
            for (const auto& texGen : source.texGens)
            {
                if (texGen.index != stage.texGen)
                    continue;
                binding.uvTransform = texGen.uvTransform;
                if (binding.uvSource.empty())
                    binding.uvSource = texGen.uvSource;
                break;
            }
        }
        binding.suffixRole  = detail::SuffixRole(stage.texture.raw);
        // A procedural texture has no filename to disagree with, and an unknown
        // suffix is silence rather than dissent.
        // The suffix names a ROLE, not a slot. Multi's further layers are colours and
        // normals like any other, so `wall_brick01_co` in LayerColour1 agrees just as
        // `wall_01_co` in BaseColour does -- comparing slot names alone would report
        // every layered material as a disagreement and drown the real ones.
        const auto roleOf = [](MaterialSlot s)
        {
            switch (s)
            {
                case MaterialSlot::LayerColour1:
                case MaterialSlot::LayerColour2:
                case MaterialSlot::LayerColour3:
                    return ToString(MaterialSlot::BaseColour);
                case MaterialSlot::LayerNormal1:
                case MaterialSlot::LayerNormal2:
                case MaterialSlot::LayerNormal3:
                    return ToString(MaterialSlot::NormalMap);
                default:
                    return ToString(s);
            }
        };
        binding.suffixAgrees = binding.suffixRole.empty() || binding.suffixRole == roleOf(slot);

        ir.slots[static_cast<size_t>(slot)] = std::move(binding);
        if (!ir.slots[static_cast<size_t>(slot)].suffixAgrees)
            ir.suffixDisagreements.push_back(slot);
    }
    return ir;
}

} // namespace Poseidon::Asset::Material
