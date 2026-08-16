#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/Graphics/Core/ZBiasMath.hpp>
#include <Poseidon/Graphics/Dummy/EngineDummy.hpp>

using Poseidon::EngineDummy;

using Poseidon::render::zbias::SoftwareCoefs;

// I-11: backend capability flags are truthful — `CanZBias()` returns
// true iff the backend actually implements hardware z-bias.  Lying
// caused B-024 (terrain occluded UI: GL33 said "I do hardware
// z-bias" but didn't actually apply it, so software fallback skipped
// too).
//
// These tests pin:
//   - The capability flag's current value (false on both backends
//     that ship today — GL33 + Dummy).  A flip-to-true without
//     simultaneous hardware-z-bias implementation regresses B-024.
//   - The software-z-bias coefficient math the false-branch callers
//     in `transLight.cpp` / `v3QuadsP3.cpp` apply via `GetZCoefs`.

TEST_CASE("CanZBias is false on shipping backends (B-024 contract)", "[Graphics][ZBias][I-11]")
{
    // Dummy backend reports false — software z-bias path is active.
    // EngineDummy's GetZCoefs returns (0, 1) — i.e., no-op coefs
    // because the dummy backend has no real frame, but the *contract*
    // is "CanZBias false implies callers apply zMult/zAdd".
    EngineDummy d;
    REQUIRE(d.CanZBias() == false);

    // EngineGL33::CanZBias is hardcoded to false (see the comment in
    // EngineGL33_Draw.cpp).  We can't instantiate EngineGL33 without
    // a GL context, so this test pins the dummy + the file-level
    // contract.  If a future backend switches to true, the
    // accompanying hardware-z-bias implementation needs review.
}

TEST_CASE("SoftwareCoefs: zero bias yields identity transform", "[Graphics][ZBias][I-11]")
{
    const auto c = SoftwareCoefs(0);
    REQUIRE(c.zMult == Catch::Approx(1.0f));
    REQUIRE(c.zAdd == Catch::Approx(0.0f));
}

TEST_CASE("SoftwareCoefs: positive bias produces depth offset and shrink", "[Graphics][ZBias][I-11]")
{
    // Typical bias values used by the engine: 0..160 (range from
    // EngineGL33::SetBias).  Coefficients must be deterministic
    // and applied consistently across callers.
    const auto c = SoftwareCoefs(100);
    REQUIRE(c.zMult == Catch::Approx(1.0f - 100 * 1e-7f).margin(1e-9f));
    REQUIRE(c.zAdd == Catch::Approx(100 * -2e-7f).margin(1e-9f));

    // Bias > 0 must produce zMult < 1 so the perturbed projection's
    // z scale is reduced (pushing fragments forward); and zAdd < 0
    // so the constant offset pulls them further forward.  These
    // signs are what `mcAdjusted(2, 2) = mc(2, 2) * zMult + zAdd`
    // relies on in transLight.cpp.
    REQUIRE(c.zMult < 1.0f);
    REQUIRE(c.zAdd < 0.0f);
}

TEST_CASE("SoftwareCoefs: bias scales linearly", "[Graphics][ZBias][I-11]")
{
    const auto c1 = SoftwareCoefs(10);
    const auto c2 = SoftwareCoefs(20);
    REQUIRE((1.0f - c2.zMult) == Catch::Approx(2.0f * (1.0f - c1.zMult)).margin(1e-9f));
    REQUIRE(c2.zAdd == Catch::Approx(2.0f * c1.zAdd).margin(1e-9f));
}

TEST_CASE("SoftwareCoefs: negative bias inverts the offset direction", "[Graphics][ZBias][I-11]")
{
    const auto c = SoftwareCoefs(-50);
    REQUIRE(c.zMult > 1.0f); // pushes fragments back
    REQUIRE(c.zAdd > 0.0f);
}

// ---------------------------------------------------------------------------
// WLD-026 — the distance-proportional forms.
//
// These tests do not check the coefficients' values, they check the PROPERTY the
// coefficients exist to produce, by reproducing what the call sites actually do
// with them (TransLight.cpp:311-327):
//
//     m22' = m22 * zMult + zAdd
//     m32' = m32 * zMult
//     z_ndc(z_view) = m22' + m32' / z_view
//
// and then converting to the stored reversed-Z depth the wgpu backend compares,
// which is `1 - z_ndc`.

namespace
{
using Poseidon::render::zbias::Coefs;

// Depth as stored by the wgpu backend, for a surface at `zView`, under the CPU
// projection perturbed by `c`.
//
// Evaluated in DOUBLE deliberately. The quantity under test is `1 - z_ndc`, and
// z_ndc is ~0.99995 beyond a few hundred metres, so in float the subtraction
// throws away five significant digits and the answer is dominated by rounding
// rather than by the coefficients. (That cancellation is real in the shipping
// software path too — gfx2d/shader.wgsl computes `1.0 - pos.z` in f32 — but it
// costs ~0.1% of the offset, which is three orders of magnitude smaller than the
// error these tests are about. It is not what is being measured here.)
double CpuReversedDepth(const Coefs& c, double q, double cNear, double zView)
{
    const double m22 = q * static_cast<double>(c.zMult) + static_cast<double>(c.zAdd);
    const double m32 = -q * cNear * static_cast<double>(c.zMult);
    return 1.0 - (m22 + m32 / zView);
}

// Depth the GPU writes for everything else in the scene: the infinite-far
// reversed-Z form EngineWgpu::PushSceneCamera uploads (_33 = 1, _43 = -cNear).
double GpuReversedDepth(double cNear, double zView)
{
    return cNear / zView;
}

// The distance a CPU-projected surface at `zView` is depth-TESTED as standing at:
// invert the GPU's depth mapping through the CPU's depth value.
double ApparentDistance(const Coefs& c, double q, double cNear, double zView)
{
    return cNear / CpuReversedDepth(c, q, cNear, zView);
}

// The engine's own numbers: World.cpp:1326-1327 clamps cNear into [0.07, 0.2],
// and cFar is the scene fog max range.
constexpr float kNear = 0.0788f;
constexpr float kFar = 3000.0f;
constexpr int kOnSurfaceBias = 0x10; // ShapeDraw.cpp:204, ClipShape.cpp:218
} // namespace

TEST_CASE("ProjectionQ reproduces Camera::Adjust", "[Graphics][ZBias][WLD-026]")
{
    using Poseidon::render::zbias::ProjectionQ;
    // ccFar = max(500, cFar * 1.01)
    const float ccFar = kFar * 1.01f;
    REQUIRE(ProjectionQ(kNear, kFar) == Catch::Approx(ccFar / (ccFar - kNear)));
    // The floor bites for a short view distance.
    REQUIRE(ProjectionQ(kNear, 100.0f) == Catch::Approx(500.0f / (500.0f - kNear)));
    // The w-buffer branch uses a different pair.
    REQUIRE(ProjectionQ(kNear, 3000.0f, true) == Catch::Approx(3300.0f / (3300.0f - kNear)));
}

TEST_CASE("InfiniteReversedZCoefs rebases the CPU depth onto the GPU's, at every range", "[Graphics][ZBias][WLD-026]")
{
    using Poseidon::render::zbias::InfiniteReversedZCoefs;
    using Poseidon::render::zbias::kEpsPerBias;
    using Poseidon::render::zbias::ProjectionQ;

    const float q = ProjectionQ(kNear, kFar);
    const auto c = InfiniteReversedZCoefs(kOnSurfaceBias, q);
    const float eps = kOnSurfaceBias * kEpsPerBias;

    // The whole point: the pull is a constant FRACTION of the distance, so the
    // apparent distance is z / (1 + eps) at 10 m and at 3 km alike.
    for (const float z : {10.0f, 100.0f, 300.0f, 1000.0f, 2000.0f, 3000.0f})
    {
        const double apparent = ApparentDistance(c, q, kNear, z);
        REQUIRE(apparent == Catch::Approx(z / (1.0f + eps)).epsilon(1e-2));
        // The property that matters, and the one the legacy form violates: the pull
        // stays a small FRACTION of the distance at every range. (It is not exactly
        // z*eps, because `m22 * zMult + zAdd` has to cancel to 1.0 and the call site
        // does that in float32 — see the float-precision note in ZBiasMath.hpp. The
        // leftover ulp is worth ~1 m at 2 km, against the legacy form's ~1100 m.)
        REQUIRE(z - apparent < 0.01 * z);
        REQUIRE(z - apparent > 0.0); // and it is a pull, never a push
    }

    // And with no bias at all the two projections agree EXACTLY — this is the
    // half `SoftwareCoefs` never addressed, the finite-vs-infinite far plane.
    const auto zero = InfiniteReversedZCoefs(0, q);
    for (const float z : {10.0f, 1000.0f, 3000.0f})
    {
        REQUIRE(CpuReversedDepth(zero, q, kNear, z) == Catch::Approx(GpuReversedDepth(kNear, z)).epsilon(1e-4f));
    }
}

TEST_CASE("The legacy constant-offset form mis-tests distant surfaces by hundreds of metres",
          "[Graphics][ZBias][WLD-026]")
{
    using Poseidon::render::zbias::ProjectionQ;

    // Exactly what EngineWgpu::GetZCoefs produces today: SoftwareCoefs scaled by
    // its empirical 16x.
    constexpr float kWgpuMult = 16.0f;
    const auto base = SoftwareCoefs(kOnSurfaceBias);
    Coefs legacy{};
    legacy.zAdd = base.zAdd * kWgpuMult;
    legacy.zMult = 1.0f - (1.0f - base.zMult) * kWgpuMult;

    const float q = ProjectionQ(kNear, kFar);

    // Near the camera it is harmless; at range it is not. This is the regression
    // this file exists to keep anyone from reintroducing: a road at 2 km is
    // depth-tested as though it stood at well under half that distance, so any
    // ridge in between fails to occlude it.
    REQUIRE(100.0 - ApparentDistance(legacy, q, kNear, 100.0) < 10.0);
    REQUIRE(2000.0 - ApparentDistance(legacy, q, kNear, 2000.0) > 500.0);

    // The proportional form is bounded at the same distance.
    using Poseidon::render::zbias::InfiniteReversedZCoefs;
    const auto fixed = InfiniteReversedZCoefs(kOnSurfaceBias, q);
    REQUIRE(2000.0 - ApparentDistance(fixed, q, kNear, 2000.0) < 5.0);
}

TEST_CASE("ProportionalCoefs pulls toward the camera in both depth conventions", "[Graphics][ZBias][WLD-026]")
{
    using Poseidon::render::zbias::ProjectionQ;
    using Poseidon::render::zbias::ProportionalCoefs;

    const float q = ProjectionQ(kNear, kFar);
    const auto c = ProportionalCoefs(kOnSurfaceBias, q);

    // Forward-Z (GL33): a biased surface must get a SMALLER z_ndc than an
    // unbiased one at the same distance, at every range.
    const auto none = ProportionalCoefs(0, q);
    for (const float z : {10.0f, 1000.0f, 3000.0f})
    {
        const double biased = 1.0 - CpuReversedDepth(c, q, kNear, z);
        const double plain = 1.0 - CpuReversedDepth(none, q, kNear, z);
        REQUIRE(biased < plain);
        // Reversed-Z (wgpu): equivalently, a LARGER stored depth.
        REQUIRE(CpuReversedDepth(c, q, kNear, z) > CpuReversedDepth(none, q, kNear, z));
    }

    // Zero bias is the identity, so an unbiased draw is untouched.
    REQUIRE(none.zMult == Catch::Approx(1.0f));
    REQUIRE(none.zAdd == Catch::Approx(0.0f));
}
