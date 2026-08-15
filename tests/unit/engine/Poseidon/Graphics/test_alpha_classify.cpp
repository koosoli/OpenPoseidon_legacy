#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <vector>
#include <stddef.h>
#include <stdint.h>

using namespace Poseidon;

// Build an RGBA8888 buffer with the given per-texel alpha values (RGB filled arbitrarily).
static std::vector<uint8_t> rgbaWithAlphas(const std::vector<int>& alphas)
{
    std::vector<uint8_t> v(alphas.size() * 4, 200);
    for (size_t i = 0; i < alphas.size(); i++)
        v[i * 4 + 3] = static_cast<uint8_t>(alphas[i]);
    return v;
}

TEST_CASE("ClassifyAlpha: all-opaque is Opaque", "[Graphics][AlphaClassify]")
{
    auto v = rgbaWithAlphas(std::vector<int>(100, 255));
    auto s = ClassifyAlpha(v.data(), 100);
    REQUIRE(s.kind == AlphaStats::Opaque);
    REQUIRE(s.aMin == 255);
    REQUIRE(s.aMax == 255);
    REQUIRE(s.pctPartial == 0.0);
}

TEST_CASE("ClassifyAlpha: binary 0/255 is Cutout", "[Graphics][AlphaClassify]")
{
    std::vector<int> a;
    for (int i = 0; i < 100; i++)
        a.push_back(i < 30 ? 0 : 255); // 30% holes, no partial
    auto v = rgbaWithAlphas(a);
    auto s = ClassifyAlpha(v.data(), 100);
    REQUIRE(s.kind == AlphaStats::Cutout);
    REQUIRE(s.pctClear >= 30.0);
    REQUIRE(s.pctPartial == 0.0);
}

TEST_CASE("ClassifyAlpha: partial alpha is Blend (the glass case)", "[Graphics][AlphaClassify]")
{
    auto v = rgbaWithAlphas(std::vector<int>(100, 128));
    auto s = ClassifyAlpha(v.data(), 100);
    REQUIRE(s.kind == AlphaStats::Blend);
    REQUIRE(s.pctPartial == 100.0);
    REQUIRE(s.aMean == 128);
}

TEST_CASE("ClassifyAlpha: glass-like partial range (no 0, no 255) is Blend", "[Graphics][AlphaClassify]")
{
    // mirrors jeep_kab_sklo: alpha all in (0,255), e.g. 85..119
    std::vector<int> a;
    for (int i = 0; i < 100; i++)
        a.push_back(85 + (i % 35));
    auto v = rgbaWithAlphas(a);
    auto s = ClassifyAlpha(v.data(), 100);
    REQUIRE(s.kind == AlphaStats::Blend);
    REQUIRE(s.aMin > 0);
    REQUIRE(s.aMax < 255);
}

TEST_CASE("ClassifyAlpha: AA-edge partial below threshold stays Cutout", "[Graphics][AlphaClassify]")
{
    // 1000 texels: 200 holes, 799 opaque, 1 partial (0.1%) — not enough to be Blend
    std::vector<int> a;
    for (int i = 0; i < 1000; i++)
        a.push_back(i < 200 ? 0 : 255);
    a[500] = 128;
    auto v = rgbaWithAlphas(a);
    auto s = ClassifyAlpha(v.data(), 1000);
    REQUIRE(s.kind == AlphaStats::Cutout);
    REQUIRE(s.pctPartial < 2.0);
}

TEST_CASE("ClassifyAlpha: a few holes below threshold stays Opaque", "[Graphics][AlphaClassify]")
{
    // 1000 texels, 1% holes, no partial — below the clear threshold → Opaque
    std::vector<int> a(1000, 255);
    for (int i = 0; i < 10; i++)
        a[i] = 0;
    auto v = rgbaWithAlphas(a);
    auto s = ClassifyAlpha(v.data(), 1000);
    REQUIRE(s.kind == AlphaStats::Opaque);
}

TEST_CASE("ClassifyAlpha: empty buffer defaults to Opaque", "[Graphics][AlphaClassify]")
{
    auto s = ClassifyAlpha(nullptr, 0);
    REQUIRE(s.kind == AlphaStats::Opaque);
}

// ── ClassifyTextureAlpha: the texture-load tiering policy ────────────────────
// hasAlpha / isChromaKey / oneBitAlphaFormat are the cheap header signals; a
// decoded scan is consulted only for multi-bit-alpha formats. This is the
// decision a section-sort renderer makes once per texture at load time.

TEST_CASE("ClassifyTextureAlpha: no alpha channel, not chromakey -> Opaque (no decode)", "[Graphics][AlphaClassify]")
{
    REQUIRE(ClassifyTextureAlpha(false, false, false, nullptr) == AlphaStats::Opaque);
}

TEST_CASE("ClassifyTextureAlpha: chromakey without alpha channel -> Cutout (no decode)", "[Graphics][AlphaClassify]")
{
    REQUIRE(ClassifyTextureAlpha(false, true, false, nullptr) == AlphaStats::Cutout);
}

TEST_CASE("ClassifyTextureAlpha: 1-bit-alpha format (DXT1) -> Cutout without decoding", "[Graphics][AlphaClassify]")
{
    // A blend verdict from a (hypothetical) decode must be ignored for a 1-bit format.
    AlphaStats wouldBeBlend;
    wouldBeBlend.kind = AlphaStats::Blend;
    REQUIRE(ClassifyTextureAlpha(true, false, true, &wouldBeBlend) == AlphaStats::Cutout);
    REQUIRE(ClassifyTextureAlpha(true, true, true, nullptr) == AlphaStats::Cutout);
}

TEST_CASE("ClassifyTextureAlpha: multi-bit alpha defers to the decoded scan", "[Graphics][AlphaClassify]")
{
    AlphaStats blend;
    blend.kind = AlphaStats::Blend;
    AlphaStats cutout;
    cutout.kind = AlphaStats::Cutout;
    REQUIRE(ClassifyTextureAlpha(true, false, false, &blend) == AlphaStats::Blend);
    REQUIRE(ClassifyTextureAlpha(true, false, false, &cutout) == AlphaStats::Cutout);
}

TEST_CASE("ClassifyTextureAlpha: undecodable multi-bit alpha falls back to Opaque (no regression)",
          "[Graphics][AlphaClassify]")
{
    // Decode unavailable -> keep current behavior: occlude + batch, never auto-route to the blend pass.
    REQUIRE(ClassifyTextureAlpha(true, false, false, nullptr) == AlphaStats::Opaque);
}

TEST_CASE("ClassifyAlpha: an all-clear alpha channel is not a cutout mask", "[Graphics][AlphaClassify]")
{
    // Alpha 0 at every texel. Classified as Cutout, every fragment is discarded and
    // the surface renders completely invisible -- which no texture author intends.
    // In practice this is packed data in the alpha channel, not coverage: three
    // shipped Reforger materials (glass, mate metal paint, plastic base) are alpha-0
    // across 100% of their texels and vanished entirely once a FLAG tagg routed them
    // down the cutout path.
    //
    // Opaque is the safe fallback: a wrongly-opaque surface looks wrong, a
    // wrongly-invisible one looks broken.
    auto v = rgbaWithAlphas(std::vector<int>(256, 0));
    auto s = ClassifyAlpha(v.data(), 256);
    REQUIRE(s.aMax == 0);
    REQUIRE(s.pctClear == 100.0);
    REQUIRE(s.kind == AlphaStats::Opaque);
}

TEST_CASE("ClassifyAlpha: one opaque texel keeps a mostly-clear mask a Cutout",
          "[Graphics][AlphaClassify]")
{
    // The all-clear guard must be exactly that. A mask that is 99.6% clear is still a
    // legitimate mask -- narrow foliage and chain-link do exactly this -- so the guard
    // keys on aMax == 0 rather than on a percentage, and a single opaque texel is
    // enough to keep it out.
    std::vector<int> alphas(256, 0);
    alphas[128] = 255;
    auto v = rgbaWithAlphas(alphas);
    auto s = ClassifyAlpha(v.data(), 256);
    REQUIRE(s.aMax == 255);
    REQUIRE(s.kind == AlphaStats::Cutout);
}
