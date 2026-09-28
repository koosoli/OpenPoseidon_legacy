#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

//! RFG-070: the composite texture NAMES the Enfusion native path uses, in one place.
//!
//! The native world loader can hand the renderer nothing but a texture NAME
//! (`XobMaterialSink::TexturePath`, XobConvert.hpp:52) -- it writes no files, unlike the
//! converting exporter, which bakes a `.paa`. So every piece of information that has to
//! travel from an `.emat` to the uploaded image travels encoded in that name, and is
//! decoded once, in DdsImport.cpp's texture source.
//!
//! Two forms exist and they NEST, outermost first:
//!
//!   `enft|r,g,b|<inner>`            RFG-070 -- rescale `<inner>` so its per-channel mean
//!                                   is the authored linear colour `r,g,b`.
//!   `enfa|<coverage>|<colour>`      RFG-023 -- fold `<coverage>`'s brightest channel into
//!                                   `<colour>`'s alpha.
//!   `enfc|r,g,b`                    RFG-039 -- a flat colour with no file behind it.
//!
//! `enft|` wrapping `enfa|` is the common case for a cutout on a shared layer tile, and
//! the parse order below is the only thing that makes the two composable: the tint is
//! stripped first and everything downstream sees the name it would have seen anyway.
//!
//! Kept in a dependency-free header because the CONSUMERS are spread across both
//! libraries -- the texture source in Poseidon, the alpha classifier and the material
//! override in WgpuRenderer, the pass-descriptor build in a hot Poseidon header. Each of
//! those already tested `strncmp(name, "enfa|", 5)` by hand, and every one of them has to
//! keep answering the same way when a tint is wrapped around it. Three hand-written
//! prefix tests that must agree is exactly how one of them ends up not agreeing.
namespace Poseidon
{
namespace Enfusion
{

inline constexpr const char* kLayerTintPrefix = "enft|";
inline constexpr size_t kLayerTintPrefixLength = 5;

//! Parse `enft|r,g,b|<inner>`. `inner` points INTO `name`, so it lives as long as it does.
inline bool SplitLayerTintName(const char* name, float rgb[3], const char** inner)
{
    if (name == nullptr || std::strncmp(name, kLayerTintPrefix, kLayerTintPrefixLength) != 0)
        return false;
    const char* rest = name + kLayerTintPrefixLength;
    const char* bar = std::strchr(rest, '|');
    if (bar == nullptr || bar[1] == '\0')
        return false;
    // Stops at the '|' on its own; the three floats are the whole of the head.
    if (std::sscanf(rest, "%f,%f,%f", &rgb[0], &rgb[1], &rgb[2]) != 3)
        return false;
    if (inner != nullptr)
        *inner = bar + 1;
    return true;
}

//! The name UNDER any tint wrapper -- `name` itself when there is none.
//!
//! Every existing test for one of the inner forms goes through this first. A tinted
//! cutout must still classify as a cutout (TextureWgpu's GetAlphaClass), must still be
//! protected from the material's Stage0 (EngineWgpu's FaceTextureIsEnfusionComposite),
//! and must still route to the coverage pass (BuildRenderPassDescriptor) -- otherwise
//! adding a tint silently changes three decisions that have nothing to do with colour.
inline const char* SkipLayerTint(const char* name)
{
    const char* inner = nullptr;
    float rgb[3];
    return SplitLayerTintName(name, rgb, &inner) ? inner : name;
}

inline bool IsOpacityCompositeName(const char* name)
{
    const char* inner = SkipLayerTint(name);
    return inner != nullptr && std::strncmp(inner, "enfa|", 5) == 0;
}

//! An original Enfusion BCR image stores roughness, not coverage, in alpha.
//! A tint wrapper changes RGB only; an enfa| composite replaces alpha with
//! coverage and must never take this route.
inline bool IsOriginalBcrRoughnessName(const char* name)
{
    const char* inner = SkipLayerTint(name);
    if (inner == nullptr || std::strchr(inner, '|') != nullptr)
        return false;
    constexpr char suffix[] = "_bcr.edds";
    constexpr size_t suffixLength = sizeof(suffix) - 1;
    const size_t length = std::strlen(inner);
    if (length < suffixLength)
        return false;
    for (size_t i = 0; i < suffixLength; ++i)
    {
        const char c = inner[length - suffixLength + i];
        if (c != suffix[i] && !(c >= 'A' && c <= 'Z' && c - 'A' + 'a' == suffix[i]))
            return false;
    }
    return true;
}

//! Build `enft|r,g,b|<inner>`. Four decimals, which is what keeps two materials that
//! share a tile but not a colour apart in the texture bank's name-keyed cache -- the
//! same precision the converting exporter uses for its own per-tint file key
//! (XobCommand.cpp, MAT-054).
inline std::string MakeLayerTintName(const float rgb[3], const std::string& inner)
{
    char head[64];
    std::snprintf(head, sizeof(head), "enft|%.4f,%.4f,%.4f|", rgb[0], rgb[1], rgb[2]);
    return std::string(head) + inner;
}

//! RFG-070 A/B switch: build `enft|` names at all, or bind the raw shared tile the way
//! the native path did before.
//!
//! It governs NAME CONSTRUCTION, in the world loader, and not the texture source -- which
//! is what makes it a clean A/B and also what makes it a LOAD-TIME switch: a texture
//! already uploaded keeps the colour it was uploaded in, and the section already points at
//! the name it was given. Reload the world (or relaunch; the dev panel persists it to
//! graphics.cfg the way RFG-047's does) to compare.
//!
//! Atomic because the object-stream workers resolve materials off the main thread while the
//! dev panel flips this from the UI thread. `-1` is "not asked yet": the default comes from
//! `POSEIDON_ENFUSION_LAYER_TINT`, which GameApplication seeds from graphics.cfg at boot,
//! and an absent variable means the panel default, which is ON.
inline std::atomic<int> GLayerTintEnabled{-1};

inline bool LayerTintEnabled()
{
    int value = GLayerTintEnabled.load(std::memory_order_relaxed);
    if (value < 0)
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_LAYER_TINT");
        value = (env != nullptr && env[0] == '0') ? 0 : 1;
        GLayerTintEnabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

inline void SetLayerTintEnabled(bool on) { GLayerTintEnabled.store(on ? 1 : 0, std::memory_order_relaxed); }

//! RFG-071 A/B switch: WHAT `Color_N` means once RFG-070 has decided to apply it.
//!
//! RFG-070 read it as the surface's LINEAR mean and rescaled the tile so its sRGB mean is
//! `linearToSrgb(Color_N)`. Measured against Enfusion's own baked albedos -- the
//! `*_MLOD_BCR.edds` its toolchain bakes from the very same layered material, which is the
//! only ground truth in the corpus for what the surface is supposed to look like -- that
//! rule comes out 1.82x too bright (median over 35 paired materials, 105 channels) and
//! flatter than the bake (median max/min channel ratio 1.040 against the bake's 1.132),
//! because encoding the target compresses the channel ratios. Reading `Color_N` as a
//! LINEAR MULTIPLIER on the tile instead -- `out = linearToSrgb(srgbToLinear(texel) * C)`,
//! the physical meaning of a colour tint -- lands at 1.29x with a third less scatter
//! (mean |log ratio| 0.403 against 0.644) and a chroma of 1.088. Two other readings were
//! measured and are worse than both: rescaling to a raw gamma target 0.83x/0.588, and
//! multiplying in gamma 0.656x/0.656.
//!
//! ON is the multiply. OFF restores RFG-070's rescale exactly, which is the A/B.
//!
//! Load-time, like the switch above and for the same reason: this decides the TEXELS a
//! tinted tile is uploaded with, and an uploaded texture keeps what it was uploaded with.
//! Reload the world after flipping it.
inline std::atomic<int> GLayerTintLinearMultiply{-1};

inline bool LayerTintLinearMultiply()
{
    int value = GLayerTintLinearMultiply.load(std::memory_order_relaxed);
    if (value < 0)
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_TINT_MULTIPLY");
        value = (env != nullptr && env[0] == '0') ? 0 : 1;
        GLayerTintLinearMultiply.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

inline void SetLayerTintLinearMultiply(bool on)
{
    GLayerTintLinearMultiply.store(on ? 1 : 0, std::memory_order_relaxed);
}

//! RFG-072 A/B switch: feed the renderer's four-layer blend shader from a NATIVE `MatPBRMulti`
//! the way the material describes itself, instead of one flattened image.
//!
//! What "on" changes, all of it registration-time (the material record is baked when a
//! shape registers, so reload the world after flipping it):
//!   * the mask samples the model's SECOND UV set when the material says
//!     `UVSrcGlobalMaps "UV set 2"` -- measured on Church_01.xob, set 0 spans u[-16..32]
//!     v[-30..17] (a metre-scale tiling unwrap) and set 1 spans [0..1] (the unique unwrap
//!     the GLOBAL_MASK is painted in), so on set 0 the mask repeated ~47 times across the
//!     wall as dark patches;
//!   * every layer's `Color_N` multiplies its tile in linear space (RFG-071's rule) inside
//!     the shader, so layer 2..4 wear their colour and layer 1 no longer needs the baked
//!     face tint that the material's own raw `BCR_1` used to override anyway.
//! Off restores the RFG-071 state: mask on uv0, raw tiles, one tinted face image.
inline std::atomic<int> GMultiLayersEnabled{-1};

inline bool MultiLayersEnabled()
{
    int value = GMultiLayersEnabled.load(std::memory_order_relaxed);
    if (value < 0)
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_MULTI_LAYERS");
        value = (env != nullptr && env[0] == '0') ? 0 : 1;
        GMultiLayersEnabled.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

inline void SetMultiLayersEnabled(bool on) { GMultiLayersEnabled.store(on ? 1 : 0, std::memory_order_relaxed); }

} // namespace Enfusion
} // namespace Poseidon
