#pragma once

#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp> // RFG-070 `enft|` tint wrapper
#include <cstdint>
#include <cstdlib>
#include <cstring>

// `BuildRenderPassDescriptor` is the *single* translation function from typed
// `LegacySpec` + scene context to the `RenderPassDescriptor` consumed by the
// backend.  All spec-bit interpretation lives here — downstream code reads
// descriptor fields, not bitmasks.

namespace Poseidon
{
namespace render
{

// Minimal context the descriptor build needs beyond the spec.  Bigger
// contexts (object identity, scene phase, etc.) can extend this without
// changing the descriptor itself — they only affect *how* `PassKind` is
// chosen.  Defaults match the common case (in 3D pass, no
// multitexturing, opaque shadow blend ref).
struct BuildContext
{
    bool isIn3DPass = true;
    bool isMultitexturing = false;

    // Alpha-test reference for the shadow path (`(shadowFactor * 7) >> 4`
    // in the legacy ApplyPassState body).  0..255.
    std::uint8_t shadowAlphaRef = 0;

    // Explicit pass routing.  Cockpit / ScreenSpace3D pass families are
    // selected only through this hint — set by the draw scopes that own
    // the decision (Scene::DrawSortObject for inside-view LODs, the
    // soldier first-person proxies).  `Routing::NoDropdown` is not used for
    // pass routing; it remains input metadata for fog and the LOD/PassNum logic.
    PassKindHint passKindHint = PassKindHint::None;
};

// MAT-048 — the back-to-front blend pass must not write depth.
//
// `Scene::DrawObjectsAndShadowsPass2` (World/Scene/SceneDraw.cpp:2406-2436) revisits an
// otherwise-opaque object a second time with `GSectionFilter = BlendOnly` to draw only its
// translucent sections — a vehicle's glass — over the already-drawn scene behind them.  Until
// MAT-048 those draws kept `DepthMode::Normal`, i.e. depth-write ON, and the code said that was
// "original parity".  It is not parity with anything; it is a bug, and it has a very specific
// signature.
//
// Back-to-front is a per-OBJECT algorithm.  The radix sort at SceneDraw.cpp:2364 orders the
// object list by `SortObject::zCoord`, and `Shape::Draw`
// (Graphics/Rendering/Shape/ShapeDraw.cpp:217) then walks that object's sections in MESH INDEX
// order — there is no intra-object sort, and there never has been.  For most objects that is
// harmless, because their blend sections do not overlap each other in screen space.  A cockpit is
// the counterexample: it is ONE object whose glass spans the whole view depth.  An OH-58 canopy is
// seven separate panes (six `oh58_sklotest*` plus `kiowa_topsklo`), so from the pilot seat, looking
// ACROSS the cockpit rather than straight ahead, two of them are in line of sight.
//
// With depth-write on, the failure is not a wrong blend — it is a missing pane.  Whichever pane
// holds the lower mesh index draws first and writes depth; the pane behind it then fails the depth
// test (`GreaterEqual` under wgpu's reversed-Z; `LEQUAL` on GL33 — the same rejection either way)
// and is discarded ENTIRELY.  You look through two panes and see one.  Both backends build state
// through this one function, so both have always had it.  And because the winner is fixed by
// section index, the artefact is stable per model rather than flickering — which is why it reads
// as "that pane is missing" rather than "the glass sorts badly".
//
// Depth-TEST stays on: the glass must still be occluded by the fuselage in front of it and by
// terrain.  Only the WRITE goes.  Once it is off no pane can erase another, all seven composite,
// and the residual error is confined to the COLOUR of the overlap rather than its presence.  For
// CWA glass that residual is small — `apach_in_skla.paa` measures TAGG AVGC alpha 46/255 (~18%,
// MAT-048's raw-byte check) — so two panes composited in the wrong order differ by a slight tint,
// not by a hole.  That is the reason this change deliberately does NOT also add an intra-object
// back-to-front section sort: the sort would buy a subtle tint correction at the cost of breaking
// Shape::Draw's adjacent-section merge (ShapeDraw.cpp:255-260 batches by texture + spec + material
// and requires `i == secEnd`, so reordering sections shatters every batch), and a per-section
// centroid order is ill-defined for interpenetrating panes anyway.  Order stops mattering for
// VISIBILITY the moment the write is off; it only ever mattered because of the write.
//
// Scoped, never global.  The flag is set only around the object-blend branch and cleared
// immediately after, so:
//   - the depth-write rule ~20 lines above is untouched — `IsAlpha` deliberately does NOT disable
//     depth-write, and poles / fences / signs still occlude, because those sections classify
//     Cutout or Opaque and draw in pass 1 with the filter off;
//   - the surface-overlay BlendOnly pass (roads / decals, SceneDraw.cpp:2296) is left alone on
//     purpose and stays bit-identical;
//   - `passNum == 2` whole-alpha objects and cloudlets sharing the same loop are untouched.
//
// One accepted cost, written down here so it is not rediscovered as a fresh bug:
// World/Scene/AlphaSortOrder.hpp sorts a blend-owning object by its FAR extent
// (`centreZ + radius`) precisely BECAUSE those sections used to write depth — dust that
// interpenetrates the object was then depth-clipped by the glass.  With the write off, dust drawn
// after the object composites over its glass unconditionally.  Dust is short-lived and already
// semi-transparent; a permanently missing canopy pane is not.  `WGR_COCKPIT_BLEND_LEGACY=1`
// restores the old depth-writing behaviour in the same binary, so that trade is one env var and
// no rebuild away from being re-examined.
inline bool GBlendSectionDepthReadOnly = false;

// Header-implemented to keep the seam visible.  Inline so callers can specialize for their
// context inputs at compile time when possible.  A function of (spec, ctx) alone, with the ONE
// documented exception of `GBlendSectionDepthReadOnly` above — which is deliberately not a
// `BuildContext` field, because both wgpu call sites (EngineWgpu.cpp:2078, :2702) and GL33's
// (EngineGL33_Queue.cpp:255) default-construct their context and adding a field there would have
// changed nothing without editing backend files this pass is not allowed to touch.
inline RenderPassDescriptor BuildRenderPassDescriptor(const LegacySpec& spec, const BuildContext& ctx = {})
{
    const Routing routing = spec.routing;
    const Material material = spec.material;
    const Backend backend = spec.backend;

    RenderPassDescriptor d;

    // Sampler.
    d.sampler.filter = Has(backend, Backend::PointSampling) ? SamplerFilter::Point : SamplerFilter::Linear;
    d.sampler.clampU = Has(backend, Backend::ClampU);
    d.sampler.clampV = Has(backend, Backend::ClampV);

    // Surface (decal / road polygon-offset).
    d.surface = IsOnSurfaceRouting(routing) ? SurfaceMode::OnSurface : SurfaceMode::Default;

    // Depth + Stencil.
    const bool isShadow = Has(backend, Backend::IsShadow);
    if (isShadow)
    {
        // Single per-poly path: stencil EQUAL 0 / INCR with color writes on,
        // so each shadow draw darkens the framebuffer directly (1-srcA blend).
        d.depth = DepthMode::Shadow;
    }
    else if (Has(backend, Backend::NoZBuf))
    {
        d.depth = DepthMode::Disabled;
    }
    else if (Has(backend, Backend::NoZWrite))
    {
        // Depth-write is disabled ONLY for surfaces explicitly flagged NoZWrite
        // (roads, decals, craters, cloudlets, grass, landscape sky) — matching the
        // original engine (engD3D.cpp: ZWRITEENABLE=FALSE for IsShadow/NoZBuf/
        // NoZWrite, TRUE otherwise).  IsAlpha / IsAlphaFog do NOT disable
        // depth-write: IsAlpha only means "texture has an alpha channel", not
        // "translucent", so opaque props with an alpha-channel texture (poles,
        // fences, signs) must still write depth or geometry behind them leaks
        // through.
        d.depth = DepthMode::ReadOnly;
    }
    else
    {
        d.depth = DepthMode::Normal;
    }

    // MAT-048: the back-to-front blend pass demotes its draws to depth-test-only for the
    // duration of that pass (see `GBlendSectionDepthReadOnly` above for the full reasoning).
    // Applied AFTER the branch, and only to `Normal`, so the Shadow / NoZBuf / NoZWrite cases
    // keep their own already-non-writing modes verbatim and nothing above this line changes.
    if (GBlendSectionDepthReadOnly && d.depth == DepthMode::Normal)
    {
        d.depth = DepthMode::ReadOnly;
    }

    // Lighting (sun gate).
    if (Has(material, Material::DisableSun))
        d.lighting = LightingMode::SunDisabled;
    // (Other lighting modes are decided per-shader-family below.)

    // Pass-family branch.
    // Order mirrors `EngineGL33::ApplyPassState` so the descriptor build
    // is bit-for-bit equivalent to that switch.
    if (isShadow)
    {
        d.pass = PassKind::WorldShadow;
        d.shader = ShaderFamily::Shadow;
        d.blend = BlendMode::Shadow;
        d.fog = FogMode::Disabled;
        d.alpha = AlphaMode::Test;
        d.alphaRef = ctx.shadowAlphaRef;
        d.stencilExclusion = true;
        d.lighting = LightingMode::ShadowDarkPolygon;
    }
    else if (Has(backend, Backend::IsLight))
    {
        d.pass = PassKind::WorldLight;
        d.shader = ShaderFamily::Normal;
        d.blend = BlendMode::Additive;
        d.fog = FogMode::Disabled;
        d.alpha = AlphaMode::Test;
        d.alphaRef = 1;
        d.lighting = LightingMode::Unlit;
    }
    else if (Has(backend, Backend::IsWater))
    {
        d.pass = PassKind::WorldWater;
        d.shader = ShaderFamily::Water;
        d.blend = BlendMode::Opaque;
        d.fog = Has(routing, Routing::FogDisabled) ? FogMode::Disabled : FogMode::Enabled;
        d.alpha = AlphaMode::Disabled;
        d.texGen = ctx.isIn3DPass ? TexGenMode::Water : TexGenMode::None;
    }
    else if (Has(backend, Backend::IsAlphaFog))
    {
        d.pass = PassKind::WorldTransparent;
        d.shader = ShaderFamily::Normal;
        d.blend = BlendMode::AlphaBlend;
        d.fog = FogMode::AlphaFog;
        d.alpha = AlphaMode::Test;
        d.alphaRef = 1;
    }
    else
    {
        // Generic opaque / cutout / transparent path.  Fog depends on
        // Backend + Routing bits; PassKind comes from the explicit
        // hint only.
        constexpr Routing fogOffMask = Routing::NoDropdown | Routing::FogDisabled;
        d.fog = ((routing & fogOffMask) == Routing::None) ? FogMode::Enabled : FogMode::Disabled;

        // FirstPersonBody routes identically to Cockpit here: both are late
        // first-person draws.  They differ only in shading (see PassKindHint).
        const bool routeAsCockpit =
            (ctx.passKindHint == PassKindHint::Cockpit || ctx.passKindHint == PassKindHint::FirstPersonBody);
        const bool routeAsScreenSpace = (ctx.passKindHint == PassKindHint::ScreenSpace3D);

        if (Has(backend, Backend::IsAlpha))
        {
            d.blend = BlendMode::AlphaBlend;
            d.alpha = AlphaMode::Test;
            d.alphaRef = 1;
            d.pass = routeAsScreenSpace ? PassKind::ScreenSpace3D
                     : routeAsCockpit   ? PassKind::CockpitTransparent
                                        : PassKind::WorldTransparent;
        }
        else if (Has(backend, Backend::IsTransparent))
        {
            d.blend = BlendMode::Opaque;
            d.alpha = AlphaMode::Test;
            d.alphaRef = 0xc0;
            d.pass = routeAsScreenSpace ? PassKind::ScreenSpace3D
                     : routeAsCockpit   ? PassKind::CockpitCutout
                                        : PassKind::WorldCutout;
        }
        else
        {
            d.blend = BlendMode::Opaque;
            d.alpha = AlphaMode::Disabled;
            d.alphaRef = 0xc0;
            d.pass = routeAsScreenSpace ? PassKind::ScreenSpace3D
                     : routeAsCockpit   ? PassKind::CockpitOpaque
                                        : PassKind::WorldOpaque;
        }

        // Multitexturing shader family + texGen — only for the generic
        // (non-special) path; water/shadow/light all set their own.
        constexpr Backend mtMask = Backend::DetailTexture | Backend::SpecularTexture | Backend::GrassTexture;
        if (ctx.isMultitexturing && (backend & mtMask) != Backend::None)
        {
            const bool grass = Has(backend, Backend::GrassTexture);
            d.shader = grass ? ShaderFamily::Grass : ShaderFamily::Detail;
            if (ctx.isIn3DPass)
                d.texGen = grass ? TexGenMode::Grass : TexGenMode::Detail;
        }
        else
        {
            d.shader = ShaderFamily::Normal;
        }
    }

    // Surface overlay overrides PassKind.
    // Roads / decals attach to terrain regardless of their alpha / blend.
    if (d.surface == SurfaceMode::OnSurface && d.pass != PassKind::WorldShadow)
    {
        d.pass = PassKind::SurfaceOverlay;
    }

    return d;
}

// Legacy Arma 1 foliage (`ca\plants\`) is authored as ALPHA-TESTED cards, but the alpha
// histogram cannot see that: the same authored card, downscaled per LOD, straddles the
// Blend/Cutout boundary. Measured on the shipped A1 plants corpus (2026-08-27,
// `PoseidonTools model inspect --classify`), where a single plant's four LOD maps split
// across both classes:
//
//   ker_deravej_0_ca BLEND / _1_ca CUTOUT / _2_ca CUTOUT / _3_ca BLEND
//   trnka_0_ca CUTOUT / _1_ca BLEND / _2_ca BLEND / _3_ca CUTOUT
//   krovi_begest_0_ca CUTOUT / _1_ca BLEND / _2_ca BLEND / _3_ca CUTOUT
//
// A family cannot be translucent at LOD 1 and alpha-tested at LOD 2; the histogram is
// simply reading antialiased downscale edges. Whichever LOD lands on the Blend side then
// leaves the opaque pass for the back-to-front pass with NO depth write, and the canopy
// goes see-through against the sky — the "some tree canopies have become transparent"
// report on Sahrani. This is the same failure `ShaderFamilyAlphaRoute` (ShapeDraw.cpp)
// already corrects for A3's TreeAdv families; A1 plants carry no RVMAT at all, so nothing
// was authoritative for them and the histogram won by default.
//
// Deliberately keyed on `\plants\`, which is the A1 prefix. A2/OA and A3 keep their own
// (`plants2`, `plants_e`, `plants_f`) and are handled by their shader family, so this
// cannot touch content it was not measured against. `WGR_A1_PLANT_CUTOUT=0` restores the
// histogram answer for an A/B in the same binary.
//! RFG-036: an `enfa|` composite is coverage, by construction.
//!
//! The name exists at all only because the material declared an `OpacityMap`, and
//! Enfusion binds that exclusively as coverage -- never as a blend weight. So the
//! composite IS the authority here, and the pixel histogram is not: measured over
//! Everon's polyplanes every one of them lands on Blend (betula 74.3% clear / 6.6%
//! partial, pinus 77.3% / 11.2%), because antialiased leaf edges are exactly what a
//! partial-alpha count rewards.
//!
//! Falling to Blend costs a leaf card the whole cutout route -- in both model shaders
//! `veg_cutout` is `is_veg && alpha_ref > 0`, and with it goes the two-sided normal
//! flip (half of every card unlit: the "trees look flat"), the crown normals, the leaf
//! sub-surface term, the depth write, and the card's place in the retained GPU set.
//!
//! The other candidate fix was to have the material sink name the `.emat`, so the
//! section would carry a surface material and answer through `EmatUsesLeafCards`.
//! Measured: the sections still report `mat '(none)'`, because the engine's material
//! loader does not read `.emat`. The name is the only signal that survives the trip.
//! RFG-070: read through an `enft|` layer tint. The tint changes the tile's colour and
//! nothing else; the material's own statement that this surface is coverage is unaffected.
inline bool IsEnfusionCoverageTexture(const char* texturePath)
{
    return texturePath != nullptr && std::strncmp(Enfusion::SkipLayerTint(texturePath), "enfa|", 5) == 0;
}

inline bool IsLegacyPlantCutoutTexture(const char* texturePath)
{
    if (!texturePath || !*texturePath)
        return false;
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_A1_PLANT_CUTOUT");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    if (!enabled)
        return false;
    // Case-insensitive `\plants\` … `_ca.` — no allocation, this runs per section per frame.
    const size_t n = std::strlen(texturePath);
    auto lower = [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    bool inPlants = false;
    for (size_t i = 0; i + 8 <= n && !inPlants; i++)
    {
        const char* p = texturePath + i;
        inPlants = (p[0] == '\\' || p[0] == '/') && lower(p[1]) == 'p' && lower(p[2]) == 'l' &&
                   lower(p[3]) == 'a' && lower(p[4]) == 'n' && lower(p[5]) == 't' && lower(p[6]) == 's' &&
                   (p[7] == '\\' || p[7] == '/');
    }
    if (!inPlants)
        return false;
    const char* dot = std::strrchr(texturePath, '.');
    const size_t stem = dot ? size_t(dot - texturePath) : n;
    return stem >= 3 && texturePath[stem - 3] == '_' && lower(texturePath[stem - 2]) == 'c' &&
           lower(texturePath[stem - 1]) == 'a';
}

// A shape section is "GPU-owned" on the wgpu GPU-driven retained path when it is plain
// opaque, Default-surface geometry — exactly the class the wgpu backend hands to the
// retained scene (see EngineWgpu::ClassifyGpuSection). This is the SINGLE source of truth
// for section ownership: the wgpu registration (what the GPU *takes*) and Shape::Draw's
// partial-suppression skip (what the CPU *drops* for a partially-GPU-driven object) MUST
// both route through it. Divergence => a section drawn twice (z-fight / overdraw) or not
// at all (hole). Cutout (alpha-tested, blend==Opaque) IS owned; alpha-blend and on-surface
// decals are the CPU complement. Hidden sections are filtered by the callers, not here.
inline bool IsGpuOwnedSection(const RenderPassDescriptor& d)
{
    return d.blend == BlendMode::Opaque && d.surface == SurfaceMode::Default;
}

// Convenience for callers that only have the raw section spec (Shape::Draw). Builds the
// descriptor in the default 3D-pass context — identical to ClassifyGpuSection's context.
inline bool IsGpuOwnedSectionSpec(int special)
{
    return IsGpuOwnedSection(BuildRenderPassDescriptor(SplitLegacy(special)));
}

} // namespace render

} // namespace Poseidon
