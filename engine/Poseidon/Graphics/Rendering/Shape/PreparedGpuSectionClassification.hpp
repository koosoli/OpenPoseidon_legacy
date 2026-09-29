#pragma once

#include <Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Poseidon::render
{

// Facts from the CURRENT ShapeSection, Texture and TexMaterial, captured by the
// owner before trying a resumable GPU admission. The producer must obtain these
// through the existing SectionGpuOwned, texture alpha and .emat/RVMAT resolvers;
// this record deliberately does not reinterpret material families or names.
// `complete` is set only after every fact is available and the owner has bound
// its shape/material/texture incarnation to the planned registration.
struct PreparedGpuSectionFacts
{
    // The owner sets each bit only after taking the entire group from the live
    // section. A default-initialised `false` is never treated as observed fact.
    enum CaptureGroup : uint8_t
    {
        Section = 1u << 0, Texture = 1u << 1, Surface = 1u << 2,
        FoldedMaterial = 1u << 3, Descriptor = 1u << 4,
        All = Section | Texture | Surface | FoldedMaterial | Descriptor
    };
    uint8_t capturedGroups = 0;
    bool complete = false;
    bool gpuOwned = false; // actual SectionGpuOwned(sec), not spec-only ownership
    int indexCount = 0;
    RenderPassDescriptor descriptor; // BuildRenderPassDescriptor(SplitLegacy(spec), 3D context)

    bool texturePresent = false;
    AlphaStats::Kind alphaClass = AlphaStats::Opaque; // actual GetAlphaClass()
    bool alphaHoles = false; // actual HasAlphaHoles()
    bool legacyPlantName = false; // IsLegacyPlantCutoutTexture(Name())
    bool enfusionCoverageName = false; // IsEnfusionCoverageTexture(Name())
    bool barkOrTrunkName = false; // GpuTextureIsBarkOrTrunk(texture)

    bool leafCards = false; // GpuSectionMaterialUsesLeafCards(surfMat)
    bool ematBlends = false; // GpuEmatBlends(surfMat)
    bool ematGlass = false; // GpuEmatIsGlass(surfMat)
    bool ematOpaqueSolid = false; // GpuEmatIsOpaqueSolid(surfMat)
    bool ematCullNone = false; // GpuEmatCullNone(surfMat)
    bool ematVegetationFamily = false; // IsEmatVegetationFamily(GpuEmatFamily(surfMat))

    bool materialFullyOpaque = false; // MaterialIsFullyOpaque(CreateMaterial + Combine)
    std::array<float, 4> foldedEmissive{}; // m.emmisive RGBA, before renderer scale
    float emitterScale = 1.0f; // StaticEmitterRadianceScale(surfMat, m.emmisive)
};

enum class PreparedGpuSectionRoute : uint8_t
{
    Incomplete,
    CpuOwnership,
    EmptyGeometry,
    CpuBlend,
    CpuDoubleSidedCutout,
    Opaque,
    Cutout
};

struct PreparedGpuSectionDecision
{
    PreparedGpuSectionRoute route = PreparedGpuSectionRoute::Incomplete;
    uint32_t variant = 0;
    float alphaRef = 0.0f;
    std::array<float, 4> scaledEmissive{};
    bool Accepted() const { return route == PreparedGpuSectionRoute::Opaque || route == PreparedGpuSectionRoute::Cutout; }
};

// Pure tail of EngineWgpu's ClassifyGpuSection. `textureHandle` is the result of
// the ONE later owner upload; its zero/nonzero state matters to the opaque BCR
// route and the IsAlpha/opaque-texture override. No Load, EnsureUploaded, alpha
// scan, material parse, or mutable renderer state occurs here.
inline PreparedGpuSectionDecision ClassifyPreparedGpuSection(const PreparedGpuSectionFacts& f,
                                                             uint64_t textureHandle)
{
    PreparedGpuSectionDecision out;
    if (!f.complete || f.capturedGroups != PreparedGpuSectionFacts::All ||
        (!f.texturePresent && textureHandle != 0) ||
        (f.alphaClass != AlphaStats::Opaque && f.alphaClass != AlphaStats::Cutout &&
         f.alphaClass != AlphaStats::Blend)) return out;
    if (!f.gpuOwned) { out.route = PreparedGpuSectionRoute::CpuOwnership; return out; }
    if (f.indexCount <= 0) { out.route = PreparedGpuSectionRoute::EmptyGeometry; return out; }

    const bool hasTexture = f.texturePresent;
    const bool bark = hasTexture && f.barkOrTrunkName;
    const bool legacyPlantCutout = hasTexture &&
        (f.legacyPlantName || (f.enfusionCoverageName && f.alphaHoles && !f.ematBlends));
    const bool materialCutout = f.alphaClass == AlphaStats::Blend && !f.ematBlends &&
        hasTexture && !bark && f.alphaHoles && f.leafCards;
    const bool blendOpaqueRoute = f.alphaClass == AlphaStats::Blend && !bark && !materialCutout &&
        !legacyPlantCutout && !f.ematGlass && !f.ematBlends && f.leafCards;
    const bool enfusionOpaqueRoughness = f.alphaClass == AlphaStats::Blend && !bark && !materialCutout &&
        !legacyPlantCutout && !blendOpaqueRoute && textureHandle != 0 && hasTexture &&
        !f.alphaHoles && f.materialFullyOpaque && f.ematOpaqueSolid;
    if (f.alphaClass == AlphaStats::Blend && !bark && !materialCutout && !legacyPlantCutout &&
        !blendOpaqueRoute && !enfusionOpaqueRoughness)
    {
        out.route = PreparedGpuSectionRoute::CpuBlend;
        return out;
    }

    const bool test = f.descriptor.alpha == AlphaMode::Test || f.descriptor.alpha == AlphaMode::TestAndBlend;
    float alphaRef = test ? f.descriptor.alphaRef / 255.0f : 0.0f;
    if (bark || blendOpaqueRoute || enfusionOpaqueRoughness)
        alphaRef = 0.0f;
    else if (textureHandle != 0 && f.alphaClass == AlphaStats::Opaque && f.materialFullyOpaque)
        alphaRef = 0.0f;
    else if (textureHandle != 0 && (f.alphaClass == AlphaStats::Cutout || materialCutout ||
                                    legacyPlantCutout || f.descriptor.blend == BlendMode::AlphaBlend))
        alphaRef = std::max(alphaRef, 0.5f);

    if (alphaRef > 0.0f && !f.ematVegetationFamily && f.ematCullNone)
    {
        out.route = PreparedGpuSectionRoute::CpuDoubleSidedCutout;
        return out;
    }
    out.alphaRef = alphaRef;
    out.variant = alphaRef > 0.0f ? 1u : 0u;
    out.route = out.variant ? PreparedGpuSectionRoute::Cutout : PreparedGpuSectionRoute::Opaque;
    out.scaledEmissive = {f.foldedEmissive[0] * f.emitterScale,
                          f.foldedEmissive[1] * f.emitterScale,
                          f.foldedEmissive[2] * f.emitterScale,
                          f.foldedEmissive[3]};
    return out;
}

} // namespace Poseidon::render
