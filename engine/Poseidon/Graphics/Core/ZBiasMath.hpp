#pragma once

// Software z-bias coefficient math.
//
// When a backend reports `CanZBias() == false`, callers apply z-bias in
// software by perturbing the projection matrix: `zMult = 1 - zBias * 1e-7`,
// `zAdd = zBias * -2e-7`.  Pure arithmetic, shared between EngineGL33::GetZCoefs
// and any other backend that opts into the software path.
//
// Capability invariant: when CanZBias returns true the backend must implement
// glPolygonOffset (or equivalent) and callers skip the software path — these
// coefficients are NOT applied.  When CanZBias returns false, callers MUST
// apply them, or terrain occludes the UI.
//
// ---------------------------------------------------------------------------
// HOW THE COEFFICIENTS ARE APPLIED  (TransLight.cpp:311-327, V3QuadsP3.cpp:226-241)
//
//     m(2,2) := m(2,2) * zMult + zAdd
//     m.pos.z := m.pos.z * zMult
//
// Both call sites perturb `Camera::Projection()`, whose depth row is the classic
// D3D finite form (Camera.cpp:42-51):
//
//     q     = ccFar / (ccFar - cNear),   ccFar = max(500, cFar * 1.01)
//     m22   = q,   m32 = -q * cNear
//     z_ndc = (m22 * z_view + m32) / z_view = m22 + m32 / z_view
//
// So `zAdd` moves the CONSTANT term of z_ndc and `zMult` scales BOTH terms.
// That distinction is the whole content of this header, because a constant shift
// of z_ndc is not a constant shift in world space — see below.

namespace Poseidon
{
namespace render::zbias
{

struct Coefs
{
    float zAdd;
    float zMult;
};

constexpr Coefs SoftwareCoefs(int bias) noexcept
{
    Coefs c{};
    c.zMult = 1.0f - bias * 1e-7f;
    c.zAdd = bias * -2e-7f;
    return c;
}

// ---------------------------------------------------------------------------
// WHY SoftwareCoefs IS WRONG AT RANGE, AND WHAT REPLACES IT
//
// STATUS: derived and unit-testable, NOT yet wired to a call site. Roads no
// longer reach the software path at all (see ObjectClasses.cpp, WLD-026), but
// footstep marks, tyre tracks (Tracks.cpp), projected shadows (Shadow.cpp:625)
// and the pre-projected UI meshes still do, and they still carry the defect.
// Wiring it is one line in EngineWgpu::GetZCoefs — see the note at the bottom.
//
// THE DEFECT. `SoftwareCoefs` is dominated by `zAdd`, a CONSTANT offset of the
// projected depth. Under the wgpu backend the depth buffer is REVERSED-Z, where
// stored depth is (very nearly) `cNear / z_view`: hyperbolic in distance. A
// constant offset `d` of that quantity therefore displaces a surface by
//
//     z_apparent = z / (1 + d * z / cNear)
//
// which is quadratic in z for small offsets — the same trap the gfx3d decal bias
// fell into (shader3d.wgsl `finish_vertex`, fixed there by scaling with ndc^2).
// Measured at the engine's own numbers (cNear = 0.0788 from World.cpp:1326-1327,
// fog range 3000 m, `bias` = 0x10 for on-surface, and EngineWgpu::GetZCoefs's
// extra 16x), the constant term is d = +5.08e-5 and the pull is:
//
//     true distance   100 m    300 m    1000 m    2000 m    3000 m
//     tested as        94 m    251 m     608 m     874 m    1022 m
//
// A decal at 2 km is depth-tested as though it stood at 874 m. Anything between
// those two distances fails to occlude it.
//
// THE SECOND DEFECT, which no single constant can fix. The CPU path projects with
// `Camera::Projection()` (finite far, `m22 = q`) while EngineWgpu::PushSceneCamera
// overrides the GPU's depth row to an INFINITE-far reversed-Z form (`_33 = 1`,
// `_43 = -cNear`). The two disagree by `(q - 1)(1 - cNear/z)` before any bias is
// applied — about 2.6e-5 of reversed depth, in the direction that sinks CPU-drawn
// geometry BEHIND the GPU-drawn geometry it is meant to sit on. That mismatch,
// not precision, is what EngineWgpu::GetZCoefs's "~16x GL33's software z-bias
// (empirically)" is actually compensating, and it is compensating a
// distance-dependent error with a constant.
//
// THE FIX. Ask for a bias that is a constant FRACTION of the distance instead:
// pull the surface from `z` to `z / (1 + eps)`. Because `z_ndc = m22 + m32/z`,
// evaluating the same projection at `z / (1 + eps)` means scaling `m32` by
// `(1 + eps)` and leaving `m22` alone. With one shared `zMult` that is:
//
//     zMult = 1 + eps        (scales both terms)
//     zAdd  = -eps * m22     (undoes it on the constant term)
//
// `eps` is then a pure world-space fraction, correct at every distance and in
// BOTH depth conventions: forward-Z (smaller z_ndc = nearer, GL33) and reversed-Z
// (larger stored depth = nearer, wgpu) both move toward the camera, because both
// derive from the same `m32 / z_view` term.
//
// FLOAT-PRECISION FLOOR, measured, not assumed. `m22 * zMult + zAdd` has to
// cancel to 1.0, and the call sites do that arithmetic in float32 on operands
// that are all ~1.0, so one ulp (~6e-8) survives as a residual CONSTANT offset —
// the very thing this replaces, just 1000x smaller. At cNear = 0.0788 that is a
// leftover pull of about 1 m at 2 km, against the legacy form's ~1100 m at the
// same distance (test_zbias_math.cpp pins both). Eliminating it entirely would
// mean changing what the call sites do, not what this header returns; it is far
// below the terrain triangle size and is recorded rather than chased.

// Fractional pull toward the camera per unit of `bias`. `bias` is 0x10 (16) for
// on-surface draws (ShapeDraw.cpp:204, ClipShape.cpp:218) and 0..15 for the
// ZBias steps, so 16 * 1e-5 = 1.6e-4: a 16 cm pull at 1 km, 1.6 mm at 10 m.
// Sized against the depth buffer's resolving power, not guessed: with reversed-Z
// in f32 the relative depth resolution is ~6e-8, so this is ~2600x the smallest
// separation the buffer can represent at any distance — ample to win a coplanar
// fight — while staying far below the ~1e-3 at which a decal would visibly
// detach from the surface it lies on.
inline constexpr float kEpsPerBias = 1e-5f;

// Distance-proportional software z-bias, for a backend whose GPU projection is
// the same one the CPU path uses (GL33). `m22` is the projection's (2,2) term,
// i.e. `q` for Camera::Projection(); passing 1.0f is correct to within
// eps * (q - 1) ~ 1e-10 and is a safe default if the caller cannot reach it.
constexpr Coefs ProportionalCoefs(int bias, float m22 = 1.0f, float epsPerBias = kEpsPerBias) noexcept
{
    const float eps = bias * epsPerBias;
    Coefs c{};
    c.zMult = 1.0f + eps;
    c.zAdd = -eps * m22;
    return c;
}

// As above, and additionally REBASES the CPU projection's depth row onto the
// infinite-far reversed-Z form the wgpu backend uploads, so the two projections
// agree exactly instead of being reconciled by a fudge factor.
//
// Solve for the coefficients that turn (m22 = q, m32 = -q*cNear) into
// (1, -cNear * (1 + eps)) — the GPU's `_33 = 1, _43 = -cNear` with the decal pull
// folded in:
//
//     m32 * zMult      = -cNear * (1 + eps)   ->  zMult = (1 + eps) / q
//     m22 * zMult+zAdd = 1                    ->  zAdd  = 1 - q * (1 + eps)/q = -eps
//
// `q` must be the value the CPU camera actually used: `ccFar / (ccFar - cNear)`
// with `ccFar = max(500, cFar * 1.01)`, or `max(100, cFar * 1.1)` on a w-buffer
// (Camera::Adjust, Camera.cpp:42-49). ProjectionQ() below computes it.
constexpr Coefs InfiniteReversedZCoefs(int bias, float q, float epsPerBias = kEpsPerBias) noexcept
{
    const float eps = bias * epsPerBias;
    Coefs c{};
    c.zMult = (q > 0.0f) ? (1.0f + eps) / q : (1.0f + eps);
    c.zAdd = -eps;
    return c;
}

// The `q` Camera::Adjust built, from the same inputs. Kept here so a backend can
// reproduce it without reaching into Camera, and so the relation is testable.
constexpr float ProjectionQ(float cNear, float cFar, bool wBuffer = false) noexcept
{
    const float ccFar =
        wBuffer ? (cFar * 1.1f > 100.0f ? cFar * 1.1f : 100.0f) : (cFar * 1.01f > 500.0f ? cFar * 1.01f : 500.0f);
    const float denom = ccFar - cNear;
    return denom > 0.0f ? ccFar / denom : 1.0f;
}

// WIRING NOTE (EngineWgpu.cpp is owned elsewhere; this is the exact change).
// EngineWgpu::GetZCoefs currently reads:
//
//     const auto c = render::zbias::SoftwareCoefs(_bias);
//     zAdd = c.zAdd * mult;  zMult = 1.0f - (1.0f - c.zMult) * mult;
//
// and should read, keeping WGR_SW_ZBIAS_MULT as the tuning knob on `eps` only:
//
//     const Camera* cam = GScene ? GScene->GetCamera() : nullptr;
//     const float q = cam ? render::zbias::ProjectionQ(cam->ClipNear(), cam->ClipFar(),
//                                                      HasWBuffer() && IsWBuffer())
//                         : 1.0f;
//     const auto c = render::zbias::InfiniteReversedZCoefs(
//         _bias, q, render::zbias::kEpsPerBias * mult);
//     zAdd = c.zAdd;  zMult = c.zMult;
//
// with the legacy arm kept behind an env gate (WGR_SW_ZBIAS_LEGACY=1) so the old
// constant-offset behaviour can be restored for an A/B without a rebuild.
// EngineGL33::GetZCoefs should switch to ProportionalCoefs(_bias) — NOT the
// reversed-Z form — because GL33 uploads the same finite projection it projects
// with, so there is no mismatch to rebase.

} // namespace render::zbias

} // namespace Poseidon
