#pragma once

#include <array>
#include <vector>

// Pure shadow-map math: no GL, no engine state. The GPU shadow test is a direct
// port of SampleShadow below, so the conventions here ARE the conventions the
// shader must use.
//
// Conventions (OpenGL clip space):
//   - Right-handed world. Column vectors; v' = M * v.
//   - Mat4 is column-major: element (row r, col c) lives at m[c*4 + r], so the
//     raw array uploads 1:1 to a GLSL `mat4`.
//   - Clip space after a projection: NDC = clip.xyz / clip.w. NDC x and y are in
//     [-1, 1]; NDC z is in [0, 1] — the D3D / Vulkan "zero-to-one" range, because
//     the GL33 backend runs glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE). Matrices
//     here therefore use zero-to-one projections so SampleShadow ports 1:1 to it.
//   - View matrices (LookAt) look down -Z, so a point in front of the camera /
//     light has negative eye-space z.
//   - Shadow-map depth is the NDC z directly, in [0, 1]: the near plane (closest
//     to the light) is 0 and the far plane is 1. A receiver is shadowed when the
//     stored nearest-occluder depth is smaller (closer to the light) than the
//     receiver's own depth.

namespace Poseidon::shadow
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

float Dot(const Vec3& a, const Vec3& b);
Vec3 Cross(const Vec3& a, const Vec3& b);
float Length(const Vec3& v);
Vec3 Normalize(const Vec3& v);
Vec3 Lerp(const Vec3& a, const Vec3& b, float t);
Vec3 Min(const Vec3& a, const Vec3& b);
Vec3 Max(const Vec3& a, const Vec3& b);

struct Vec4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

// Column-major 4x4. at(r, c) == m[c * 4 + r].
struct Mat4
{
    std::array<float, 16> m{};

    float& at(int r, int c) { return m[static_cast<size_t>(c) * 4 + static_cast<size_t>(r)]; }
    float at(int r, int c) const { return m[static_cast<size_t>(c) * 4 + static_cast<size_t>(r)]; }
};

Mat4 Identity();
Mat4 Mul(const Mat4& a, const Mat4& b);
Mat4 Inverse(const Mat4& a); // general 4x4; returns Identity if singular
Vec4 Transform(const Mat4& a, const Vec4& v);
Vec3 TransformPoint(const Mat4& a, const Vec3& p); // applies (p, 1), divides by w

// Camera/light matrix builders (GL conventions described above).
Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up);
Mat4 Ortho(float l, float r, float b, float t, float n, float f);
Mat4 Perspective(float fovYRadians, float aspect, float n, float f);

// Cascade split distances.
// Practical split scheme (blend of uniform and logarithmic by lambda in [0,1]).
// Returns n+1 ascending distances; result[0] == nearD, result[n] == farD.
std::vector<float> CascadeSplits(float nearD, float farD, int n, float lambda);

// Camera frustum corners in world space.
// Unprojects the 8 NDC cube corners through invViewProj. Order: indices 0..3 are
// the near face (z=-1), 4..7 the far face (z=+1), each face CCW from (-1,-1).
std::array<Vec3, 8> FrustumCornersWS(const Mat4& invViewProj);
// Sub-slice of a frustum by fraction along the 4 near->far edges (t in [0,1]).
std::array<Vec3, 8> SliceFrustum(const std::array<Vec3, 8>& corners, float t0, float t1);

// Light view + orthographic fit.
Mat4 LightView(const Vec3& sunDir, const Vec3& up); // looks along sunDir (photon travel dir)

struct OrthoFit
{
    Mat4 proj; // maps the light-space AABB of the points into the NDC cube
    Vec3 minB; // tight AABB of the points, in light space
    Vec3 maxB;
};
OrthoFit FitOrtho(const Mat4& lightView, const Vec3* pts, int count);

// Texel-grid snap (anti-swim).
// Snaps the fit's light-space origin to the shadow-texel grid so translating the
// fitted region by < 1 texel produces the identical matrix (no edge crawl).
OrthoFit SnapToTexelGrid(const OrthoFit& fit, int resolution);

// Receiver bias.
struct Bias
{
    float depthBias;    // added to the receiver depth before the compare
    float normalOffset; // world distance to push the receiver along its normal
};
// normal/lightDir need not be normalized. texelWorldSize scales the normal offset.
Bias ShadowBias(const Vec3& normal, const Vec3& lightDir, float texelWorldSize);

// World <-> shadow UV.
struct ShadowUV
{
    float u, v;  // [0,1] across the shadow map
    float depth; // [0,1], 0 = near (at light), 1 = far
    bool inMap;  // false when the point projects outside [0,1]^3
};
ShadowUV WorldToShadowUV(const Mat4& lightVP, const Vec3& p);
Vec3 ShadowUVToWorld(const Mat4& invLightVP, float u, float v, float depth);

// The occlusion reference (the GPU shader mirrors this).
struct DepthMap
{
    int w = 0;
    int h = 0;
    std::vector<float> depth; // row-major, [0,1]; 1.0 = empty/far
};
// Returns visibility in [0,1]: 1 = fully lit, 0 = fully shadowed. pcfRadius 0 is a
// single tap; r>0 averages a (2r+1)^2 box for soft, temporally stable edges.
float SampleShadow(const DepthMap& map, const Mat4& lightVP, const Vec3& worldP, float depthBias, int pcfRadius);

// Tiny software depth raster.
struct Tri
{
    Vec3 a, b, c;
};
// Rasterizes the casters from the light, keeping the nearest depth per texel.
DepthMap CpuRasterDepth(const Tri* tris, int count, const Mat4& lightVP, int resolution);

// Cascade composition (the engine depth pass uses these).
// Bounding-sphere fit + texel snap. The sphere gives a rotation-invariant extent
// (texel density never changes as the camera turns) and the snap pins the frustum
// to whole-texel steps (the shadow edge never crawls under sub-texel motion) —
// the two properties that make shadow maps "stable while moving".

struct BoundingSphere
{
    Vec3 center;
    float radius = 0.0f;
};
BoundingSphere FrustumBoundingSphere(const std::array<Vec3, 8>& corners);

// Fit a snapped ortho around a world-space sphere, viewed through lightView. zPad
// extends the near plane toward the light so casters above the receivers are
// captured. The window is padded one texel so snapping never clips the sphere.
OrthoFit FitOrthoSphere(const Mat4& lightView, const Vec3& center, float radius, int resolution, float zPad);

// Per-frame light view-projection for one cascade: slice the camera frustum to
// [t0, t1] (fractions along near->far), bound it with a sphere, and fit a snapped
// ortho from the sun. Returns proj*view, ready to upload to a shader UBO.
Mat4 ComputeCascadeLightVP(const Vec3& sunDir, const Vec3& up, const Mat4& invCameraViewProj, float t0, float t1,
                           int resolution, float zPad);

// Cascaded-shadow kernel (pure).
// Mirrors the FP source (cascadedShadowGL.cpp / light_calculation.inc): a
// world-space cascade fit, a camera-relative bake, depth-based cascade selection
// and a far-edge fade. Each piece is independently unit-tested and touches no GL.
// The point of the world-space fit + bake is that the shadow stays anchored to
// the world while the camera moves — a camera-relative fit would snap to a
// camera-locked grid and crawl/jump (worst in a heli).

// Column-major translation matrix.
Mat4 Translate(const Vec3& t);

// 8 world-space camera frustum corners from the camera basis + half-FOV tangents
// (the engine camera's Left()/Top() == tan(hfov/2)/tan(vfov/2)). Near face =
// indices 0..3, far face = 4..7, each CCW from (-x,-y).
std::array<Vec3, 8> CameraFrustumCornersWorld(const Vec3& camPos, const Vec3& forward, const Vec3& right,
                                              const Vec3& up, float tanHalfX, float tanHalfY, float nearD, float farD);

// World-space light view-projection for one cascade = sub-slice [t0,t1] of the
// world-space camera frustum, bounding-sphere fit + texel-snapped. Snapping is in
// world light-space, so the shadow is world-anchored (no camera-relative crawl).
Mat4 CascadeLightVPWorld(const std::array<Vec3, 8>& worldCorners, float t0, float t1, const Vec3& sunDir,
                         const Vec3& up, int resolution, float zPad);

// Bake a world-space light-VP into one that consumes CAMERA-RELATIVE positions
// (p_world = p_camRel + camPos): returns worldLightVP * Translate(camPos). This
// reconciliation lets a world-anchored fit drive the engine's camera-relative
// draw path without the shadow crawling with the camera.
Mat4 ToCameraRelative(const Mat4& worldLightVP, const Vec3& camPos);

// Stable camera-relative cascade fit: `corners` are the camera-relative frustum
// corners (camera at the origin). Fits the slice [t0,t1] in camera-relative space
// — so all matrix coordinates stay small and float-safe — but snaps the ortho to
// a WORLD-aligned texel grid using `camPos`, with the snap computed in DOUBLE so
// the alignment survives at large world coordinates. Returns the camera-relative
// light-VP directly (no large-coordinate `worldVP * Translate(camPos)` bake), so
// the shadow stays pinned to the world without the float-precision crawl that
// bake introduces near the edges of large maps.
Mat4 CascadeLightVPStable(const std::array<Vec3, 8>& corners, float t0, float t1, const Vec3& sunDir, const Vec3& up,
                          int resolution, float zPad, const Vec3& camPos);

// Round a cascade sphere radius UP onto a fixed geometric ladder (minRadius * stepRatio^n).
float QuantizeShadowRadius(float radius, float minRadius = 2.0f, float stepRatio = 1.05f);

// Cascade index for an eye-space view depth given the per-cascade far view
// distances (ascending, `count` of them). Returns `count` (out of range) when the
// fragment is beyond the last cascade.
int SelectCascade(float eyeDepth, const float* splitViewDistances, int count);

// Far-edge shadow strength in [0,1]: 1.0 well inside the shadow range, ramping to
// 0.0 over the last `fadeRange` metres before `lastSplit`, so distant shadows
// dissolve instead of cutting off at a hard, camera-swept arc.
float ShadowFarFade(float eyeDepth, float lastSplit, float fadeRange);

// Composed cascade build (the whole computation, pure + testable).
// One call turns camera + sun + knobs into everything the engine uploads, so the
// Scene wiring is just "build, then push to the GPU". The decision/computation is
// a pure unit; the renderer only consumes it.
struct CascadeSet
{
    int count = 0;
    int omniCount = 0;      // leading tiers are camera-centred spheres (distance-selected)
    Mat4 camRelVP[4];       // camera-relative light view-projections (upload + draw)
    float splitViewDist[4]; // frustum tiers: FAR view-space distance (eye-depth select)
    float omniRadius[4];    // omni tiers: camera-relative 3D radius (distance select); 0 for frustum tiers
    Vec3 sunDir;            // normalized sun travel direction the set was built with
};

struct CascadeBuildParams
{
    Vec3 camPos, forward, right, up; // camera world basis (Direction/Aside/Up)
    float tanHalfX = 1.0f;           // camera Left()  == tan(hfov/2)
    float tanHalfY = 1.0f;           // camera Top()   == tan(vfov/2)
    float nearD = 0.1f;              // camera near
    float farD = 900.0f;             // camera far / view distance
    Vec3 sunDir{0.0f, -1.0f, 0.0f};  // sun travel direction
    int count = 4;                   // cascade count (clamped 1..4)
    float distanceCoef = 0.12f;      // shadowFar = near + coef*(far-near)  (FP shadowDistance)
    float splitCoef = 0.90f;         // PSSM blend                          (FP shadowCoef)
    int resolution = 2048;
    float zPad = 50.0f;
    int omniCount = 0;                            // leading omni (camera-sphere) tiers; 0 = pure frustum
    float omniCoef[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // omni radii as a fraction of the shadow range (ascending)
};

CascadeSet BuildShadowCascades(const CascadeBuildParams& p);

// Camera-centred omnidirectional shadow cascade: fits a snapped ortho around a
// sphere of `radius` centred on the camera, so it covers ALL directions. A frustum
// fit misses casters behind/beside the camera whose shadow falls into the view;
// this does not. World-snapped in double like CascadeLightVPStable; returns the
// camera-relative light-VP (camera at the origin).
// LGT-010: the light view-projection for a SPOT (a vehicle headlight, a downward street
// lamp). The sun's cascades are orthographic because its rays are parallel; a spot has a
// position and a cone, so it is a perspective projection -- literally a second camera
// standing at the bulb and looking down the beam.
//
// `outerHalfAngle` is the HALF angle of the cone in radians (the beam's edge from its
// axis). The frustum is opened wider than that by `marginScale`, because a cone edge lying
// exactly on the frustum border loses its PCF taps: the neighbouring texels a soft-shadow
// filter reads are off the map, and the shadow frays along the rim of the light pool.
//
// `nearD` matters more here than for a cascade. Depth precision in a perspective projection
// is spent almost entirely between the near plane and a few times its distance, so a near
// plane of 1 cm on a 40 m beam throws the precision away and returns shadow acne. Callers
// pass something on the order of a tenth of the beam length.
//
// Returned CAMERA-RELATIVE, like every other matrix here, because the depth pass draws
// camera-relative casters.
Mat4 SpotLightVP(const Vec3& lightPos, const Vec3& lightDir, float outerHalfAngle, float nearD, float farD,
                 const Vec3& camPos, float marginScale = 1.15f);

// LGT-015: one face of a point light's shadow cube, camera-relative like everything else
// here. `face` is 0..5 in the order +X, -X, +Y, -Y, +Z, -Z -- the SAME order the shader's
// cube_face_of() derives from the major axis of light -> fragment, and the two must not
// drift apart: a mismatched order does not fail, it shadows the wrong side of the world.
//
// The field of view is exactly 90 degrees with no margin. A cube's faces tile the sphere
// only if they meet exactly; widening one for safety would overlap its neighbours and make
// the seam self-shadow instead of closing it. The seams are handled by the receiver picking
// the face by the same rule the matrix was built with, not by overlap.
Mat4 PointFaceVP(const Vec3& lightPos, int face, float nearD, float farD, const Vec3& camPos);

Mat4 OmniSphereLightVP(const Vec3& camPos, float radius, const Vec3& sunDir, const Vec3& up, int resolution,
                       float zPad);

// Tiered cascade build: the first `omniCount` tiers are camera-centred spheres
// (OmniSphereLightVP, distance-selected) for crisp all-direction near shadows; the
// rest are frustum slices (CascadeLightVPStable, eye-depth-selected) reaching out
// to distanceCoef*farD. Generalises BuildShadowCascades (omniCount 0 == identical).
CascadeSet BuildShadowCascadesTiered(const CascadeBuildParams& p);

// Tightest tier covering (dist3D, eyeDepth): omni tiers [0,omniCount) match when
// dist3D <= omniRadius[i]; frustum tiers match when eyeDepth <= splitViewDist[i].
// Returns `count` when beyond the shadow range. The shader starts at this tier and
// advances to the next matching tier if this one's UV is out of bounds (coverage
// fallthrough), which also fixes near shadows vanishing at a too-tight cascade edge.
int SelectShadowTier(float dist3D, float eyeDepth, const float* omniRadius, int omniCount, const float* splitViewDist,
                     int count);

// Per-face shadow-caster mode from its model "Special" flag bitmask. Pure policy,
// flag-agnostic — the engine passes the actual masks. `skipMask` = flags that
// mean "never cast" (NoShadow / ShadowDisabled / hidden / proxy); `alphaMask` =
// "casts, but alpha-test the caster texture" (cutout foliage). Anything else
// casts solid. This is what lets bushes cast leaf silhouettes instead of blobs.
enum class CasterMode
{
    Skip,
    Solid,
    AlphaTest
};
CasterMode ClassifyShadowCaster(int special, int skipMask, int alphaMask);

// Cross-cascade blend weight in [0,1] for an eye-space depth: 0 well inside the
// current cascade, ramping to 1.0 over the last `band` metres before the cascade
// far edge `cascadeFar`. At >0 the lit shader also samples the next (looser)
// cascade and lerps by this weight, so the resolution/bias change at a cascade
// boundary fades in instead of jumping as the camera moves.
float CascadeBlendWeight(float eyeDepth, float cascadeFar, float band);

} // namespace Poseidon::shadow
