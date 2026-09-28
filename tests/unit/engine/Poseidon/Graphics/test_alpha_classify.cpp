#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include <limits>
#include <utility>

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
    // the surface renders completely invisible. That is wrong for packed-alpha
    // modern materials; legacy AI88 coverage is handled by its format-aware route.
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

TEST_CASE("Legacy AI88 all-clear coverage remains invisible without changing packed-alpha textures",
          "[Graphics][AlphaClassify][AI88]")
{
    auto clear = rgbaWithAlphas(std::vector<int>(256, 0));
    auto stats = ClassifyAlpha(clear.data(), 256);
    REQUIRE(stats.kind == AlphaStats::Opaque);
    REQUIRE(LegacyAi88CoverageClass(true, stats) == AlphaStats::Cutout);
    REQUIRE(LegacyAi88CoverageClass(false, stats) == AlphaStats::Opaque);

    auto mostlyClear = rgbaWithAlphas(std::vector<int>(256, 0));
    mostlyClear[3 * 128 + 3] = 255;
    stats = ClassifyAlpha(mostlyClear.data(), 256);
    REQUIRE(LegacyAi88CoverageClass(true, stats) == AlphaStats::Cutout);
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

namespace
{
// Literal original classifier, independent of the optimized production loops.
AlphaStats ReferenceAlphaSamples(const uint8_t* alpha, size_t pixelCount, size_t stride,
                                double partialThreshold = 2.0, double clearThreshold = 2.0)
{
    AlphaStats s;
    if (!alpha || pixelCount == 0 || stride == 0 ||
        pixelCount - 1 > std::numeric_limits<size_t>::max() / stride) return s;
    size_t clear = 0, opaque = 0, partial = 0, mid = 0;
    int aMin = 255, aMax = 0;
    unsigned long long aSum = 0;
    for (size_t i = 0; i < pixelCount; i++)
    {
        const int a = alpha[i * stride];
        aSum += static_cast<unsigned long long>(a);
        if (a < aMin) aMin = a;
        if (a > aMax) aMax = a;
        if (a == 0) clear++;
        else if (a == 255) opaque++;
        else { partial++; if (a >= 16 && a <= 239) mid++; }
    }
    s.aMin = aMin; s.aMax = aMax; s.aMean = static_cast<int>(aSum / pixelCount);
    s.pctClear = 100.0 * static_cast<double>(clear) / static_cast<double>(pixelCount);
    s.pctOpaque = 100.0 * static_cast<double>(opaque) / static_cast<double>(pixelCount);
    s.pctPartial = 100.0 * static_cast<double>(partial) / static_cast<double>(pixelCount);
    s.pctMid = 100.0 * static_cast<double>(mid) / static_cast<double>(pixelCount);
    if (s.pctPartial >= partialThreshold) s.kind = AlphaStats::Blend;
    else if (s.pctClear >= clearThreshold) s.kind = AlphaStats::Cutout;
    else s.kind = AlphaStats::Opaque;
    if (s.kind == AlphaStats::Cutout && s.aMax == 0) s.kind = AlphaStats::Opaque;
    return s;
}

void CheckEveryAlphaField(const AlphaStats& reference, const AlphaStats& actual)
{
    CHECK(actual.kind == reference.kind);
    CHECK(actual.aMin == reference.aMin);
    CHECK(actual.aMax == reference.aMax);
    CHECK(actual.aMean == reference.aMean);
    CHECK(actual.pctClear == reference.pctClear);
    CHECK(actual.pctOpaque == reference.pctOpaque);
    CHECK(actual.pctPartial == reference.pctPartial);
    CHECK(actual.pctMid == reference.pctMid);
}

void CheckHistogramLayouts(const std::vector<uint8_t>& alpha, double partialThreshold = 2.0,
                           double clearThreshold = 2.0)
{
    const auto reference = ReferenceAlphaSamples(alpha.data(), alpha.size(), 1, partialThreshold, clearThreshold);
    for (const auto layout : {std::pair<size_t, size_t>{1,0}, {1,3}, {4,3}, {4,0}, {3,1}, {7,5}})
    {
        std::vector<uint8_t> pixels(layout.second + alpha.size() * layout.first + 1);
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = i % 3 == 0 ? 0 : i % 3 == 1 ? 255 : 47;
        for (size_t i = 0; i < alpha.size(); ++i) pixels[layout.second + i * layout.first] = alpha[i];
        const auto* channel = pixels.data() + layout.second;
        CheckEveryAlphaField(reference, ClassifyAlphaSamples(channel, alpha.size(), layout.first, partialThreshold, clearThreshold));
        CheckEveryAlphaField(ReferenceAlphaSamples(channel, alpha.size(), layout.first, partialThreshold, clearThreshold),
                            ClassifyAlphaSamples(channel, alpha.size(), layout.first, partialThreshold, clearThreshold));
        if (layout.first == 4 && layout.second == 3)
            CheckEveryAlphaField(reference, ClassifyAlpha(pixels.data(), alpha.size(), partialThreshold, clearThreshold));
    }
}
} // namespace

TEST_CASE("Alpha histogram fast strides preserve every byte value and boundary count", "[Graphics][AlphaClassify][histogram]")
{
    for (int value = 0; value < 256; ++value) CheckHistogramLayouts({static_cast<uint8_t>(value)});
    for (const size_t count : {size_t(0), size_t(1), size_t(2), size_t(15), size_t(16), size_t(17),
                               size_t(31), size_t(32), size_t(33), size_t(255), size_t(256), size_t(257)})
    {
        for (const uint8_t value : {uint8_t(0), uint8_t(1), uint8_t(15), uint8_t(16), uint8_t(239), uint8_t(240), uint8_t(254), uint8_t(255)})
            CheckHistogramLayouts(std::vector<uint8_t>(count, value));
        std::vector<uint8_t> sweep(count);
        for (size_t i = 0; i < count; ++i) sweep[i] = static_cast<uint8_t>(i);
        CheckHistogramLayouts(sweep);
    }
    // Fixed expected facts also pin the inclusive mid endpoints and mean floor.
    const uint8_t edges[] = {0, 15, 16, 239, 240, 255};
    const auto facts = ClassifyAlphaSamples(edges, 6, 1);
    CHECK(facts.aMin == 0); CHECK(facts.aMax == 255); CHECK(facts.aMean == 127);
    CHECK(facts.pctMid == 100.0 * 2.0 / 6.0);
    CHECK(facts.pctPartial == 100.0 * 4.0 / 6.0);
}

TEST_CASE("Alpha histogram fast strides preserve sparse stripes and deterministic mixed samples", "[Graphics][AlphaClassify][histogram]")
{
    uint32_t seed = 0x83ac1047u;
    for (const size_t count : {size_t(3), size_t(63), size_t(64), size_t(65), size_t(999), size_t(1000), size_t(1001), size_t(4097)})
    {
        std::vector<uint8_t> alpha(count);
        for (int pattern = 0; pattern < 5; ++pattern)
        {
            for (size_t i = 0; i < count; ++i)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                if (pattern == 0) alpha[i] = static_cast<uint8_t>(seed >> 24);
                else if (pattern == 1) alpha[i] = i % 9 < 3 ? 0 : i % 9 < 6 ? 128 : 255;
                else if (pattern == 2) alpha[i] = i % 1000 == 0 ? 0 : 255;
                else if (pattern == 3) alpha[i] = seed % 100 < 3 ? 0 : seed % 100 < 5 ? 128 : 255;
                else alpha[i] = i % 2 ? 255 : 0;
            }
            CheckHistogramLayouts(alpha);
        }
    }
}

TEST_CASE("Alpha histogram retains custom threshold comparisons and exact two percent boundary", "[Graphics][AlphaClassify][histogram]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto thresholds : {std::pair<double,double>{2,2}, {0,0}, {-1,-1}, {100,100}, {101,101}, {nan,2}, {2,nan}, {inf,-inf}})
    {
        CheckHistogramLayouts(std::vector<uint8_t>(100, 0), thresholds.first, thresholds.second);
        for (const size_t significant : {size_t(1), size_t(2), size_t(3)})
            for (const uint8_t value : {uint8_t(0), uint8_t(128)})
            {
                std::vector<uint8_t> alpha(100, 255);
                for (size_t i = 0; i < significant; ++i) alpha[i] = value;
                CheckHistogramLayouts(alpha, thresholds.first, thresholds.second);
            }
    }
}

TEST_CASE("Alpha histogram keeps invalid guards and single-sample arbitrary stride", "[Graphics][AlphaClassify][histogram]")
{
    const uint8_t sample = 128;
    const auto maximum = std::numeric_limits<size_t>::max();
    CheckEveryAlphaField({}, ClassifyAlphaSamples(nullptr, 1, 1));
    CheckEveryAlphaField({}, ClassifyAlphaSamples(&sample, 0, 1));
    CheckEveryAlphaField({}, ClassifyAlphaSamples(&sample, 1, 0));
    CheckEveryAlphaField({}, ClassifyAlphaSamples(&sample, maximum, 4));
    CheckEveryAlphaField({}, ClassifyAlphaSamples(&sample, maximum / 4 + 2, 4));
    CheckEveryAlphaField({}, ClassifyAlphaSamples(&sample, 3, maximum));
    CheckEveryAlphaField(ReferenceAlphaSamples(&sample, 1, maximum), ClassifyAlphaSamples(&sample, 1, maximum));
    CheckEveryAlphaField({}, ClassifyAlpha(nullptr, 1));
    CheckEveryAlphaField({}, ClassifyAlpha(&sample, 0));
}

TEST_CASE("Alpha histogram fixed strides widen exact bounded chunk totals", "[Graphics][AlphaClassify][histogram]")
{
    for (const size_t count : {size_t(65535), size_t(65536), size_t(65537), size_t(131073)})
    {
        // All255 maximizes each local sum. All0 maximizes clear counts and
        // exercises the original final all-clear policy after widening.
        CheckHistogramLayouts(std::vector<uint8_t>(count, 255));
        CheckHistogramLayouts(std::vector<uint8_t>(count, 0));
        std::vector<uint8_t> alpha(count);
        for (size_t i = 0; i < count; ++i) alpha[i] = static_cast<uint8_t>(i);
        CheckHistogramLayouts(alpha);
        // Place extrema and partial texels across consecutive chunk boundaries.
        for (size_t i = 0; i < count; ++i) alpha[i] = 255;
        for (size_t boundary = 65536; boundary < count; boundary += 65536)
        {
            alpha[boundary - 1] = 0; alpha[boundary] = 16;
            if (boundary + 1 < count) alpha[boundary + 1] = 239;
        }
        CheckHistogramLayouts(alpha);
    }
}
