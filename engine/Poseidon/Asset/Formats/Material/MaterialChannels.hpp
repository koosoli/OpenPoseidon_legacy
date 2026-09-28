#pragma once

#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <memory>

namespace Poseidon::Asset::Material
{

// MAT-020 -- what each slot's texture channels actually carry.
//
// MAT-030 decided which slot a stage occupies. This decides how to read the pixels
// once they are there, and every entry below was MEASURED by decoding real Arma 3
// textures rather than taken from folklore. The counts are how many of the sampled
// textures held a channel dead flat, which is what identifies an unused channel:
//
//   SMDI (n=40)  R constant 255 in 40/40, A constant 255 in 40/40.
//                G varies (mean 47), B varies (mean 110).
//                -> G is the specular level, B is gloss. R and A carry nothing.
//   AS   (n=25)  R, B, A all constant 255 in 25/25; only G varies (mean 146).
//                -> the ambient-shadow term is in G alone.
//   NOHQ (n=40)  R constant 0 in 40/40; G, B, A vary.
//                -> DXT5nm: X in A, Y in G, Z reconstructed. R carries nothing.
//                Independently corroborated: for a NOHQ texture, G+A is the only
//                channel pair where every texel satisfies x^2 + y^2 <= 1.
//
// The roadmap's two standing prohibitions fall straight out of this. SMDI must not
// be mapped to roughness: roughness is not even in the same channel as the value
// people reach for, and B is a GLOSS term, so a direct mapping is wrong in channel
// and inverted in polarity. And a normal map's X cannot be read from R, which is
// flat zero in every sample.

enum class TextureChannel
{
    R,
    G,
    B,
    A,
    None, // the quantity is not stored; derive it or leave it at its default
};

inline const char* ToString(TextureChannel channel)
{
    switch (channel)
    {
        case TextureChannel::R: return "R";
        case TextureChannel::G: return "G";
        case TextureChannel::B: return "B";
        case TextureChannel::A: return "A";
        default: return "None";
    }
}

// How to read one slot's texture. Only the fields a slot actually uses are set.
struct SlotChannelMap
{
    TextureChannel normalX = TextureChannel::None;
    TextureChannel normalY = TextureChannel::None;
    // Z is reconstructed as sqrt(1 - x^2 - y^2) rather than stored; B sits at ~254
    // in every sampled NOHQ, which is a constant, not a usable component.
    bool normalZReconstructed = false;

    TextureChannel specularLevel = TextureChannel::None;
    TextureChannel gloss         = TextureChannel::None;
    TextureChannel ambientShadow = TextureChannel::None;
    TextureChannel colour        = TextureChannel::None; // RGB triple when set to R
    TextureChannel alpha         = TextureChannel::None;

    // Channels measured to be dead in this slot. Recorded so a consumer sampling
    // one knows it is reading a constant, not data.
    std::vector<TextureChannel> unused;
};

inline SlotChannelMap ChannelsForSlot(MaterialSlot slot)
{
    SlotChannelMap map;
    switch (slot)
    {
        case MaterialSlot::NormalMap:
            // `_nopx` (the NormalMapDiffuse family's normal) shares this packing: G+A span
            // the full range and satisfy x^2+y^2<=1 in 31 of 31 sampled textures, with B
            // near-constant high. It differs in R, which is flat 0 on NOHQ but varies here
            // -- the "PX" payload. So R is unused in the sense that matters (nothing below
            // samples it) but is NOT dead data on every texture that lands in this slot.
            map.normalX              = TextureChannel::A;
            map.normalY              = TextureChannel::G;
            map.normalZReconstructed = true;
            map.unused               = {TextureChannel::R};
            break;

        case MaterialSlot::SpecularDetail: // SMDI
            map.specularLevel = TextureChannel::G;
            map.gloss         = TextureChannel::B;
            map.unused        = {TextureChannel::R, TextureChannel::A};
            break;

        case MaterialSlot::AmbientShadow:
            // MEASURED on `_as` (n=25). `_ads`, the spelling the Multi family prefers, does
            // NOT match: over 47 of them R is constant 255 in 46 and A in 47/47, but B
            // varies in 47/47 as well as G, and its mean tracks G's within 2 in only 27 of
            // 47 -- so B is a second quantity, not a copy. What it is has not been
            // established, and the name (`ADS`, against plain `AS`) says the family thinks
            // it carries something extra.
            //
            // Nothing samples this slot yet, so the declaration below is not wrong in
            // effect -- but `unused` means "measured dead", and for `_ads` B is not.
            map.ambientShadow = TextureChannel::G;
            map.unused        = {TextureChannel::R, TextureChannel::B, TextureChannel::A};
            break;

        case MaterialSlot::BaseColour:
        case MaterialSlot::Detail:
        case MaterialSlot::Macro:
        case MaterialSlot::Environment:
            // Ordinary colour textures: RGB is colour, A is alpha where present.
            map.colour = TextureChannel::R; // marks "RGB triple starting at R"
            map.alpha  = TextureChannel::A;
            break;

        case MaterialSlot::MacroAmbient:
            // MCA, measured over the 181 `_mca` textures in the four shipped Arma 3
            // vegetation PBOs. RGB varies in 179 of 181 and is chromatic rather than
            // grey, so it is a colour triple and not a replicated scalar.
            //
            // Alpha is the interesting half, and the corpus splits cleanly: 128 of
            // the 181 are DXT1 and carry no alpha at all, while all 53 DXT5 files do
            // -- 47 varying over the full 0..255 range, 6 constant at 174. The split
            // is exact, with every constant-255 alpha belonging to a DXT1 file, which
            // matches BI's documentation that the occlusion term is optional and
            // simply absent when the alpha channel is.
            //
            // Declaring it here is safe for the DXT1 majority precisely because their
            // decoded alpha is a constant 255: an occlusion term of "none".
            map.colour        = TextureChannel::R; // RGB triple starting at R
            map.ambientShadow = TextureChannel::A; // not transparency -- occlusion
            break;

        case MaterialSlot::Fresnel:
            // Generated, not sampled as colour: `#(ai,64,64,1)fresnel(2.0,0.1)` is a
            // lookup built from its parameters, so no channel map applies.
            break;

        default:
            break;
    }
    return map;
}

// Gloss and roughness are not the same quantity, and this is the only place that
// says so out loud.
//
// The roadmap forbids mapping SMDI to roughness. SMDI's B channel is a gloss term:
// higher means shinier. A renderer wanting roughness must invert it, and that
// inversion is a RENDERING decision about a shading model, not a property of the
// source asset -- so it is offered here as a named function a consumer opts into,
// never applied while translating. Nothing in this programme has yet validated the
// conversion against a rendered reference.
inline float GlossToRoughnessUnvalidated(float gloss)
{
    return 1.0f - gloss;
}

// What a slot's texture means, resolved for one material.
struct TranslatedSlot
{
    MaterialSlot   slot = MaterialSlot::Count;
    bool           present = false;
    int            sourceStage = -1;
    RvTextureRef   texture;
    std::string    uvSource;
    // The stage's own transform, or the TexGenN block it selected. Multi gives each
    // surface layer a different one -- Takistan's brick is a finer scale than the rock
    // it sits beside -- so a translation that keeps only `uvSource` collapses every
    // layer onto one frame and loses the detail the surface reads as.
    RvUvTransform  uvTransform;
    SlotChannelMap channels;
    // RFG-072: the colour this layer's tile is worn in -- Enfusion's per-layer `Color_N`,
    // a LINEAR multiplier on the shared one-metre library tile (RFG-071 measured the
    // reading). Identity when the source names none, so a consumer may multiply blind.
    float          tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool           tintPresent = false;
};

// Source facts only. Enfusion's BCR alpha is roughness, while an OpacityMap
// composite replaces that alpha with cutout coverage. These values are kept out
// of the legacy specular/gloss lanes until a native PBR shader consumes them.
struct EmatAuthoredScalar
{
    float value = 0.0f;
    bool present = false;
};

struct EmatPbrProvenance
{
    // Index 0 is BCRMap (or BCR_1); indices 1..3 are BCR_2..4.
    // True only for an opaque Enfusion PBR surface whose bound source is an
    // original *_BCR.edds, never a converter PAA or an OpacityMap composite.
    std::array<bool, 4> bcrAlphaIsRoughness{};
    bool baseLayerUsesTile = false; // MatPBRMulti BCR_1, paired with NMO_1
    std::array<EmatAuthoredScalar, 4> roughness{}; // Roughness_1..4, as authored
    std::array<EmatAuthoredScalar, 4> metalness{}; // Metalness_1..4, as authored
    EmatAuthoredScalar roughnessScale;             // RoughnessScale
    EmatAuthoredScalar metalnessScale;             // MetalnessScale
    EmatAuthoredScalar roughnessScale2;            // RoughnessScale2 (MatPBR2Layers)
    EmatAuthoredScalar metalnessScale2;            // MetalnessScale2 (MatPBR2Layers)
};

struct TranslatedMaterial
{
    std::string                 origin;
    std::string                 shaderFamily;
    bool                        schemaKnown = false;
    std::vector<TranslatedSlot> slots; // only the slots the material actually binds

    // Render state carried straight through from the source. Not reinterpreted:
    // `specularPower` stays the source's exponent rather than being converted into
    // whatever the current shading model prefers.
    std::array<float, 4> ambient{{1, 1, 1, 1}};
    std::array<float, 4> diffuse{{1, 1, 1, 1}};
    std::array<float, 4> forcedDiffuse{{0, 0, 0, 0}};
    std::array<float, 4> emissive{{0, 0, 0, 1}};
    std::array<float, 4> specular{{1, 1, 1, 1}};
    float                specularPower = 1.0f;
    // Enfusion's per-material normal-map intensity (`NormalPower`): a multiplier on
    // the decoded normal's deviation, 1 when the source names none. Crowns use
    // 0 (deliberately flat) through 3; the renderer scales the tangent-space XY
    // and renormalises, so 1.0 is bit-identical to no scaling.
    float                normalPower = 1.0f;
    // Enfusion's crown self-occlusion volume (`GeometryAOCenter/Height/Width/
    // Intensity`): a radial cylinder about the trunk axis in MODEL space that
    // darkens interior leaves toward the baked vertex colours. Intensity 0
    // (every material that names none, all legacy) disables it exactly.
    float                crownAoCenter[3] = {0.0f, 0.0f, 0.0f};
    float                crownAoHeight = 0.0f;
    float                crownAoWidth = 0.0f;
    float                crownAoIntensity = 0.0f;
    // Enfusion's global normal map (`GlobalNMOMap`, `UVSrcGlobNormal`) lives
    // in the object's own unwrap rather than the tiling frame: sample it
    // with the second UV set. False for every material that names neither.
    bool                 globalNormalUv1 = false;
    // Enfusion `Cull none`: the surface is authored single-sided but drawn
    // double-sided (wire mesh, glass panes). False for every material that
    // names no Cull key, and all legacy.
    bool                 doubleSided = false;
    // Default-null diagnostic. No per-material provenance allocation in normal
    // gameplay and no GPU field or shading effect.
    std::shared_ptr<const EmatPbrProvenance> ematPbr;

    const TranslatedSlot* Find(MaterialSlot slot) const
    {
        for (const auto& entry : slots)
            if (entry.slot == slot)
                return &entry;
        return nullptr;
    }
};

inline TranslatedMaterial TranslateSemantics(const RvMaterialSource& source, const MaterialIR& ir)
{
    TranslatedMaterial out;
    out.origin        = ir.origin;
    out.shaderFamily  = ir.shaderFamily;
    out.schemaKnown   = ir.schemaKnown;
    out.ambient       = source.ambient;
    out.diffuse       = source.diffuse;
    out.forcedDiffuse = source.forcedDiffuse;
    out.emissive      = source.emissive;
    out.specular      = source.specular;
    out.specularPower = source.specularPower;

    if (!ir.schemaKnown)
        return out; // no schema, no slots -- and so nothing to give channels to

    for (size_t i = 0; i < static_cast<size_t>(MaterialSlot::Count); ++i)
    {
        const MaterialSlotBinding& binding = ir.slots[i];
        if (!binding.present)
            continue;
        TranslatedSlot entry;
        entry.slot     = static_cast<MaterialSlot>(i);
        entry.present  = true;
        entry.sourceStage = binding.sourceStage;
        entry.texture  = binding.texture;
        entry.uvSource = binding.uvSource;
        entry.uvTransform = binding.uvTransform;
        entry.channels = ChannelsForSlot(entry.slot);
        out.slots.push_back(std::move(entry));
    }
    return out;
}

} // namespace Poseidon::Asset::Material
