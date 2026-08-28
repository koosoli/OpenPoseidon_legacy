#pragma once

#include <cstdint>

//
// `RenderPassDescriptor` is the typed contract for a single submitted
// draw.  Every backend-relevant state fact is named in one of the strong
// enum-class fields below.  The descriptor is built once per draw by
// `BuildRenderPassDescriptor` from the typed `LegacySpec` triplet + scene
// context; the GL33 backend's `ApplyPipeline` reads from the descriptor
// instead of reconstructing meaning from spec-bit combinations on its own.
//
// Design notes:
//   - Strong-typed enums make every state choice explicit and grep-able.
//   - One translation function (`Build...Descriptor`) is the *only*
//     place that decodes the int spec bits.  Subsequent consumers read
//     fields, not bitmasks.
//   - `PassKind` is the high-level routing axis.  The backend doesn't
//     branch on it directly — it branches on the derived state fields
//     (depth/blend/fog/etc.) — but `PassKind` is the seam where
//     validation can assert "no `CockpitOpaque` draw ever binds world-fog
//     state" without re-deriving that contract from the spec bits.

namespace Poseidon
{
namespace render
{

// High-level pass routing.  Names match the design-doc table.
enum class PassKind : std::uint8_t
{
    WorldOpaque,        // Pass 1: normal opaque world geometry.
    WorldCutout,        // Pass 1: alpha-tested world geometry (foliage etc).
    WorldTransparent,   // Pass 2: alpha-blended, reverse-distance sorted.
    WorldShadow,        // Shadow projection / accumulation.
    WorldLight,         // Additive light volumes.
    WorldWater,         // Water-special path.
    SurfaceOverlay,     // Roads / decals / surface-attached geometry.
    CockpitOpaque,      // Pass 3: late opaque cockpit / first-person meshes.
    CockpitCutout,      // Pass 3: late cutout cockpit / weapon meshes.
    CockpitTransparent, // Pass 3: late transparent cockpit meshes.
    ScreenSpace3D,      // TL / post-world 3D overlay-like meshes (HUD, notebook).
};

// Explicit cockpit / first-person routing.  Producers wrap cockpit / overlay
// draws with a hint; the descriptor uses it to pick the correct `PassKind`
// family rather than inferring it from `NoDropdown` spec bits.  `None` falls
// back to deriving the `PassKind` from the spec bits.
enum class PassKindHint : std::uint8_t
{
    None,          // No explicit hint — derive PassKind from spec bits.
    Cockpit,       // Producer says "this is a late cockpit/first-person
                   // draw"; descriptor picks
                   // CockpitOpaque/Cutout/Transparent based on alpha state.
    ScreenSpace3D, // Producer says "this is a 3D-mesh UI overlay"
                   // (notebook, HUD geometry); routes as ScreenSpace3D
                   // regardless of alpha.
    FirstPersonBody, // Producer says "this is the player's OWN body or held
                     // weapon at its inside-view LOD".  Routes into the same
                     // Cockpit* pass family as `Cockpit` -- it is a late
                     // first-person draw and needs that depth handling -- but
                     // it is NOT an enclosed interior, so backends must not
                     // apply cockpit ambient damping to it.  MAT-052's damping
                     // was meant for a vehicle cabin no ambient occluder can
                     // see; it was also reaching the soldier's sleeves and
                     // rifle, which stand in the open sky.
};

// Depth test + write + stencil interaction.  Names align with the
// `DepthMode` enum already in `EngineGL33_State.cpp::ApplyDepthMode`.
enum class DepthMode : std::uint8_t
{
    Normal,   // depth test (LEQUAL), depth write on.
    ReadOnly, // depth test, depth write off.
    Disabled, // depth test always, depth write off.
    Shadow,   // shadow geometry pass — stencil EQUAL 0 / INCR; per-poly
              //   (1-srcA) blend darkens the framebuffer directly.
};

// Blend mode for the final color write.
enum class BlendMode : std::uint8_t
{
    Opaque,     // GL_BLEND off.
    AlphaBlend, // SRC_ALPHA, ONE_MINUS_SRC_ALPHA.
    Additive,   // SRC_ALPHA, ONE — volumetric light.
    Shadow,     // ZERO, ONE_MINUS_SRC_ALPHA — shadow darken.
};

// Fog application mode.  Routes the fragment-shader fog uniform.
enum class FogMode : std::uint8_t
{
    Enabled,  // standard distance fog.
    Disabled, // no fog (cockpit / dropdown / first-person / sky overlay).
    AlphaFog, // alpha-channel attenuation replaces depth fog
              //   (cloudlets, decals, alpha-fog particles).
};

// Polygon culling.
enum class CullMode : std::uint8_t
{
    Back,  // GL_CULL_FACE + GL_BACK — D3D / world default.
    Front, // GL_CULL_FACE + GL_FRONT — water reflection, mirrored draws.
    None,  // GL_CULL_FACE disabled — double-sided foliage, shadows.
};

// Winding convention for what counts as a "front" face.
enum class FrontFaceMode : std::uint8_t
{
    CW,  // D3D / OFP default.
    CCW, // GL default; used by mirrored / reflected draws.
};

// Alpha-channel handling.  Alpha-test cuts out pixels; alpha-blend mixes.
enum class AlphaMode : std::uint8_t
{
    Disabled,     // no alpha test, no alpha blend (opaque).
    Test,         // alpha-test cutout (foliage, sprites with chromakey).
    Blend,        // alpha-blended (smoke, glass, water reflection).
    TestAndBlend, // some shadow / light paths combine both.
};

// CPU- and shader-side lighting state.
enum class LightingMode : std::uint8_t
{
    Lit,               // sun + selected positioned lights, full vertex lighting.
    SunDisabled,       // sun off, positioned lights on (cockpit interior, etc.).
    Unlit,             // no lights — constant color (HUD, light volumes).
    ShadowDarkPolygon, // shadow draw — black alpha-modulated polygon.
};

// Texture-coordinate generation strategy.  Matches the existing
// `TGMode` enum in EngineGL33 (TGNone / TGFixed / TGWater / TGDetail
// / TGGrass).
enum class TexGenMode : std::uint8_t
{
    None,   // pass-through vertex UV.
    Fixed,  // 2D screen-space UVs (UI / 2D draw).
    Water,  // water shader generates reflection / refraction UVs.
    Detail, // detail-texture stage generates secondary UVs.
    Grass,  // grass-cluster texgen.
};

// Surface attachment.  `OnSurface` draws need polygon-offset bias to
// stay at terrain depth without z-fighting.
enum class SurfaceMode : std::uint8_t
{
    Default,   // no special surface handling.
    OnSurface, // road / decal / footprint — glPolygonOffset on.
};

// Texture sampler filtering + clamp.  Clamp axes are orthogonal so
// they ride alongside the filter mode.
enum class SamplerFilter : std::uint8_t
{
    Linear, // bi-/tri-linear (default).
    Point,  // nearest (pixel art, point-sampled debug textures).
};

struct SamplerMode
{
    SamplerFilter filter = SamplerFilter::Linear;
    bool clampU = false;
    bool clampV = false;

    bool operator==(const SamplerMode& rhs) const
    {
        return filter == rhs.filter && clampU == rhs.clampU && clampV == rhs.clampV;
    }
    bool operator!=(const SamplerMode& rhs) const { return !(*this == rhs); }
};

// Pixel-shader family.  The backend resolves a `PSSel` from this plus
// `TexGenMode` + multitexturing texture binding.
enum class ShaderFamily : std::uint8_t
{
    Normal, // PSNormal — single-textured opaque / cutout / transparent.
    Shadow, // PSShadow — alpha-cutout discard, stencil-friendly.
    Water,  // PSWater.
    Detail, // PSDetail — multitex detail.
    Grass,  // PSGrass — multitex grass cluster.
    Flat,   // PSFlat — pure vertex-color passthrough (fullscreen
            //   darken quad, debug overlays).  In screen-space pass,
            //   binds VSScreen + PSFlat; not used in 3D pass.
};

// The full per-draw render-pass contract.  Defaults are the most
// permissive / common state (opaque world geometry with fog, depth,
// back-face culling, CW winding).
struct RenderPassDescriptor
{
    PassKind pass = PassKind::WorldOpaque;
    DepthMode depth = DepthMode::Normal;
    BlendMode blend = BlendMode::Opaque;
    FogMode fog = FogMode::Enabled;
    CullMode cull = CullMode::Back;
    FrontFaceMode frontFace = FrontFaceMode::CW;
    AlphaMode alpha = AlphaMode::Disabled;
    LightingMode lighting = LightingMode::Lit;
    TexGenMode texGen = TexGenMode::None;
    SurfaceMode surface = SurfaceMode::Default;
    SamplerMode sampler = SamplerMode{};
    ShaderFamily shader = ShaderFamily::Normal;

    // Alpha-test reference value (only meaningful when alpha == Test or
    // TestAndBlend).  0..255.
    std::uint8_t alphaRef = 0;

    // Stencil exclusion (shadow path).  True for shadow draws that must
    // respect the stencil mask (EQUAL 0 / INCR per-poly within-caster).
    bool stencilExclusion = false;

    bool operator==(const RenderPassDescriptor& rhs) const
    {
        return pass == rhs.pass && depth == rhs.depth && blend == rhs.blend && fog == rhs.fog && cull == rhs.cull &&
               frontFace == rhs.frontFace && alpha == rhs.alpha && lighting == rhs.lighting && texGen == rhs.texGen &&
               surface == rhs.surface && sampler == rhs.sampler && shader == rhs.shader && alphaRef == rhs.alphaRef &&
               stencilExclusion == rhs.stencilExclusion;
    }
    bool operator!=(const RenderPassDescriptor& rhs) const { return !(*this == rhs); }
};

} // namespace render

} // namespace Poseidon
