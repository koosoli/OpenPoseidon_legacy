#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon::shadow
{

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(const Vec3& v)
{
    return std::sqrt(Dot(v, v));
}

Vec3 Normalize(const Vec3& v)
{
    float len = Length(v);
    if (len <= 1e-20f)
    {
        return {0.0f, 0.0f, 0.0f};
    }
    float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

Vec3 Min(const Vec3& a, const Vec3& b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3 Max(const Vec3& a, const Vec3& b)
{
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

Mat4 Identity()
{
    Mat4 r;
    r.at(0, 0) = 1.0f;
    r.at(1, 1) = 1.0f;
    r.at(2, 2) = 1.0f;
    r.at(3, 3) = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                s += a.at(i, k) * b.at(k, j);
            }
            r.at(i, j) = s;
        }
    }
    return r;
}

Mat4 Inverse(const Mat4& a)
{
    const std::array<float, 16>& m = a.m;
    std::array<float, 16> inv{};

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f)
    {
        return Identity();
    }
    float invDet = 1.0f / det;
    Mat4 out;
    for (int i = 0; i < 16; i++)
    {
        out.m[i] = inv[i] * invDet;
    }
    return out;
}

Vec4 Transform(const Mat4& a, const Vec4& v)
{
    Vec4 r;
    r.x = a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w;
    r.y = a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w;
    r.z = a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w;
    r.w = a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w;
    return r;
}

Vec3 TransformPoint(const Mat4& a, const Vec3& p)
{
    Vec4 r = Transform(a, {p.x, p.y, p.z, 1.0f});
    float w = (r.w != 0.0f) ? r.w : 1.0f;
    return {r.x / w, r.y / w, r.z / w};
}

Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up)
{
    Vec3 f = Normalize(center - eye);
    Vec3 s = Normalize(Cross(f, up));
    Vec3 u = Cross(s, f);

    Mat4 r = Identity();
    r.at(0, 0) = s.x;
    r.at(0, 1) = s.y;
    r.at(0, 2) = s.z;
    r.at(1, 0) = u.x;
    r.at(1, 1) = u.y;
    r.at(1, 2) = u.z;
    r.at(2, 0) = -f.x;
    r.at(2, 1) = -f.y;
    r.at(2, 2) = -f.z;
    r.at(0, 3) = -Dot(s, eye);
    r.at(1, 3) = -Dot(u, eye);
    r.at(2, 3) = Dot(f, eye);
    return r;
}

Mat4 Ortho(float l, float r, float b, float t, float n, float f)
{
    Mat4 m;
    float rl = (r - l != 0.0f) ? (r - l) : 1e-6f;
    float tb = (t - b != 0.0f) ? (t - b) : 1e-6f;
    float fn = (f - n != 0.0f) ? (f - n) : 1e-6f;
    m.at(0, 0) = 2.0f / rl;
    m.at(1, 1) = 2.0f / tb;
    m.at(2, 2) = -1.0f / fn; // zero-to-one depth: near plane -> 0, far plane -> 1
    m.at(0, 3) = -(r + l) / rl;
    m.at(1, 3) = -(t + b) / tb;
    m.at(2, 3) = -n / fn;
    m.at(3, 3) = 1.0f;
    return m;
}

Mat4 Perspective(float fovYRadians, float aspect, float n, float f)
{
    float ty = std::tan(fovYRadians * 0.5f);
    Mat4 m;
    m.at(0, 0) = 1.0f / (aspect * ty);
    m.at(1, 1) = 1.0f / ty;
    m.at(2, 2) = -f / (f - n); // zero-to-one depth (matches glClipControl ZERO_TO_ONE)
    m.at(2, 3) = -(f * n) / (f - n);
    m.at(3, 2) = -1.0f;
    m.at(3, 3) = 0.0f;
    return m;
}

std::vector<float> CascadeSplits(float nearD, float farD, int n, float lambda)
{
    if (n < 1)
    {
        n = 1;
    }
    std::vector<float> r(static_cast<size_t>(n) + 1);
    r[0] = nearD;
    r[n] = farD;
    for (int i = 1; i < n; i++)
    {
        float p = static_cast<float>(i) / static_cast<float>(n);
        float logS = nearD * std::pow(farD / nearD, p);
        float uniS = nearD + (farD - nearD) * p;
        r[i] = lambda * logS + (1.0f - lambda) * uniS;
    }
    return r;
}

std::array<Vec3, 8> FrustumCornersWS(const Mat4& invViewProj)
{
    static const float nx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    static const float ny[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    std::array<Vec3, 8> out;
    for (int i = 0; i < 4; i++)
    {
        out[i] = TransformPoint(invViewProj, {nx[i], ny[i], 0.0f}); // near plane: zero-to-one NDC z = 0
    }
    for (int i = 0; i < 4; i++)
    {
        out[i + 4] = TransformPoint(invViewProj, {nx[i], ny[i], 1.0f});
    }
    return out;
}

std::array<Vec3, 8> SliceFrustum(const std::array<Vec3, 8>& corners, float t0, float t1)
{
    std::array<Vec3, 8> out;
    for (int i = 0; i < 4; i++)
    {
        out[i] = Lerp(corners[i], corners[i + 4], t0);
        out[i + 4] = Lerp(corners[i], corners[i + 4], t1);
    }
    return out;
}

Mat4 LightView(const Vec3& sunDir, const Vec3& up)
{
    Vec3 dir = Normalize(sunDir);
    return LookAt({0.0f, 0.0f, 0.0f}, dir, up);
}

OrthoFit FitOrtho(const Mat4& lightView, const Vec3* pts, int count)
{
    OrthoFit fit;
    fit.proj = Identity();
    if (count <= 0 || pts == nullptr)
    {
        fit.minB = {};
        fit.maxB = {};
        return fit;
    }

    Vec3 mn = TransformPoint(lightView, pts[0]);
    Vec3 mx = mn;
    for (int i = 1; i < count; i++)
    {
        Vec3 p = TransformPoint(lightView, pts[i]);
        mn = Min(mn, p);
        mx = Max(mx, p);
    }

    // Guard degenerate (zero-extent) axes so the ortho never divides by zero.
    const float eps = 1e-4f;
    if (mx.x - mn.x < eps)
    {
        mx.x += eps;
        mn.x -= eps;
    }
    if (mx.y - mn.y < eps)
    {
        mx.y += eps;
        mn.y -= eps;
    }
    if (mx.z - mn.z < eps)
    {
        mx.z += eps;
        mn.z -= eps;
    }

    fit.minB = mn;
    fit.maxB = mx;
    // Light space looks down -Z, so points in front have negative z; the near
    // plane (closest to the light) is at the largest z = -maxB.z.
    float n = -mx.z;
    float f = -mn.z;
    fit.proj = Ortho(mn.x, mx.x, mn.y, mx.y, n, f);
    return fit;
}

OrthoFit SnapToTexelGrid(const OrthoFit& fit, int resolution)
{
    OrthoFit out = fit;
    if (resolution <= 0)
    {
        return out;
    }
    float res = static_cast<float>(resolution);
    float texelX = (fit.maxB.x - fit.minB.x) / res;
    float texelY = (fit.maxB.y - fit.minB.y) / res;

    auto snap = [](float v, float texel)
    {
        if (texel <= 0.0f)
        {
            return v;
        }
        return std::floor(v / texel) * texel;
    };

    // Snap the min corner to the texel grid and hold the extent fixed, so the
    // matrix scale never changes and a sub-texel shift leaves it identical.
    float minX = snap(fit.minB.x, texelX);
    float minY = snap(fit.minB.y, texelY);
    out.minB.x = minX;
    out.minB.y = minY;
    out.maxB.x = minX + texelX * res;
    out.maxB.y = minY + texelY * res;

    float n = -out.maxB.z;
    float f = -out.minB.z;
    out.proj = Ortho(out.minB.x, out.maxB.x, out.minB.y, out.maxB.y, n, f);
    return out;
}

Bias ShadowBias(const Vec3& normal, const Vec3& lightDir, float texelWorldSize)
{
    Vec3 N = Normalize(normal);
    Vec3 L = Normalize(lightDir * -1.0f); // toward the light
    float c = Dot(N, L);
    c = std::max(1e-4f, std::min(1.0f, c));
    float sinT = std::sqrt(std::max(0.0f, 1.0f - c * c));
    float tanT = sinT / c;

    const float baseDepth = 0.0008f;
    const float slopeScale = 0.0025f;
    const float maxDepth = 0.01f;
    float depthBias = std::min(maxDepth, baseDepth + slopeScale * tanT);

    const float offsetScale = 2.0f;
    float normalOffset = std::min(texelWorldSize * offsetScale, texelWorldSize * offsetScale * sinT);
    return {depthBias, normalOffset};
}

ShadowUV WorldToShadowUV(const Mat4& lightVP, const Vec3& p)
{
    Vec4 clip = Transform(lightVP, {p.x, p.y, p.z, 1.0f});
    float w = (clip.w != 0.0f) ? clip.w : 1.0f;
    float nx = clip.x / w;
    float ny = clip.y / w;
    float nz = clip.z / w;

    ShadowUV r;
    r.u = nx * 0.5f + 0.5f;
    r.v = ny * 0.5f + 0.5f;
    r.depth = nz; // zero-to-one: NDC z is already the [0,1] depth
    r.inMap = r.u >= 0.0f && r.u <= 1.0f && r.v >= 0.0f && r.v <= 1.0f && r.depth >= 0.0f && r.depth <= 1.0f;
    return r;
}

Vec3 ShadowUVToWorld(const Mat4& invLightVP, float u, float v, float depth)
{
    return TransformPoint(invLightVP, {u * 2.0f - 1.0f, v * 2.0f - 1.0f, depth});
}

float SampleShadow(const DepthMap& map, const Mat4& lightVP, const Vec3& worldP, float depthBias, int pcfRadius)
{
    if (map.w <= 0 || map.h <= 0)
    {
        return 1.0f;
    }
    ShadowUV s = WorldToShadowUV(lightVP, worldP);
    if (!s.inMap)
    {
        return 1.0f; // outside the map → treat as lit
    }

    float fx = s.u * static_cast<float>(map.w) - 0.5f;
    float fy = s.v * static_cast<float>(map.h) - 0.5f;
    int cx = static_cast<int>(std::floor(fx + 0.5f));
    int cy = static_cast<int>(std::floor(fy + 0.5f));

    int r = pcfRadius < 0 ? 0 : pcfRadius;
    int lit = 0;
    int total = 0;
    for (int dy = -r; dy <= r; dy++)
    {
        for (int dx = -r; dx <= r; dx++)
        {
            int tx = cx + dx;
            int ty = cy + dy;
            if (tx < 0 || ty < 0 || tx >= map.w || ty >= map.h)
            {
                continue;
            }
            float stored = map.depth[static_cast<size_t>(ty) * map.w + tx];
            total++;
            // Lit when nothing closer to the light (smaller depth) occludes us.
            if (s.depth - depthBias <= stored)
            {
                lit++;
            }
        }
    }
    if (total == 0)
    {
        return 1.0f;
    }
    return static_cast<float>(lit) / static_cast<float>(total);
}

namespace
{
float EdgeFn(const Vec3& a, const Vec3& b, float px, float py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
} // namespace

DepthMap CpuRasterDepth(const Tri* tris, int count, const Mat4& lightVP, int resolution)
{
    DepthMap map;
    if (resolution <= 0)
    {
        return map;
    }
    map.w = resolution;
    map.h = resolution;
    map.depth.assign(static_cast<size_t>(resolution) * resolution, 1.0f);
    if (tris == nullptr)
    {
        return map;
    }

    const float res = static_cast<float>(resolution);
    for (int t = 0; t < count; t++)
    {
        const Vec3 vtx[3] = {tris[t].a, tris[t].b, tris[t].c};
        Vec3 sv[3];
        for (int k = 0; k < 3; k++)
        {
            Vec4 clip = Transform(lightVP, {vtx[k].x, vtx[k].y, vtx[k].z, 1.0f});
            float w = (clip.w != 0.0f) ? clip.w : 1.0f;
            float nx = clip.x / w;
            float ny = clip.y / w;
            float nz = clip.z / w;
            sv[k] = {(nx * 0.5f + 0.5f) * res, (ny * 0.5f + 0.5f) * res, nz}; // zero-to-one depth
        }

        float area2 = EdgeFn(sv[0], sv[1], sv[2].x, sv[2].y);
        if (std::fabs(area2) < 1e-9f)
        {
            continue;
        }

        float minxf = std::min({sv[0].x, sv[1].x, sv[2].x});
        float maxxf = std::max({sv[0].x, sv[1].x, sv[2].x});
        float minyf = std::min({sv[0].y, sv[1].y, sv[2].y});
        float maxyf = std::max({sv[0].y, sv[1].y, sv[2].y});

        int x0 = std::max(0, static_cast<int>(std::floor(minxf)));
        int x1 = std::min(resolution - 1, static_cast<int>(std::ceil(maxxf)));
        int y0 = std::max(0, static_cast<int>(std::floor(minyf)));
        int y1 = std::min(resolution - 1, static_cast<int>(std::ceil(maxyf)));

        const float biasEps = 1e-5f;
        for (int py = y0; py <= y1; py++)
        {
            for (int px = x0; px <= x1; px++)
            {
                float cx = static_cast<float>(px) + 0.5f;
                float cy = static_cast<float>(py) + 0.5f;
                float l0 = EdgeFn(sv[1], sv[2], cx, cy) / area2;
                float l1 = EdgeFn(sv[2], sv[0], cx, cy) / area2;
                float l2 = EdgeFn(sv[0], sv[1], cx, cy) / area2;
                if (l0 < -biasEps || l1 < -biasEps || l2 < -biasEps)
                {
                    continue;
                }
                float depth = l0 * sv[0].z + l1 * sv[1].z + l2 * sv[2].z;
                size_t idx = static_cast<size_t>(py) * resolution + px;
                if (depth < map.depth[idx])
                {
                    map.depth[idx] = depth;
                }
            }
        }
    }
    return map;
}

BoundingSphere FrustumBoundingSphere(const std::array<Vec3, 8>& corners)
{
    Vec3 center{0.0f, 0.0f, 0.0f};
    for (const Vec3& c : corners)
    {
        center = center + c;
    }
    center = center * (1.0f / 8.0f);

    float r2 = 0.0f;
    for (const Vec3& c : corners)
    {
        Vec3 d = c - center;
        r2 = std::max(r2, Dot(d, d));
    }
    return {center, std::sqrt(r2)};
}

OrthoFit FitOrthoSphere(const Mat4& lightView, const Vec3& center, float radius, int resolution, float zPad)
{
    OrthoFit fit;
    if (radius < 1e-4f)
    {
        radius = 1e-4f;
    }
    int res = resolution > 0 ? resolution : 1;

    Vec3 lc = TransformPoint(lightView, center);
    float diam = 2.0f * radius;
    float texel = diam / static_cast<float>(res);

    // Snap the min corner to the texel grid, then pad one texel each side so the
    // snap can never clip the sphere. Extent stays a fixed multiple of the texel,
    // so the projection scale is identical frame to frame.
    float minX = std::floor((lc.x - radius) / texel) * texel - texel;
    float minY = std::floor((lc.y - radius) / texel) * texel - texel;
    float ext = diam + 2.0f * texel;
    float maxX = minX + ext;
    float maxY = minY + ext;

    float zMin = lc.z - radius;
    float zMax = lc.z + radius + std::max(0.0f, zPad);

    fit.minB = {minX, minY, zMin};
    fit.maxB = {maxX, maxY, zMax};
    fit.proj = Ortho(minX, maxX, minY, maxY, -zMax, -zMin);
    return fit;
}

Mat4 ComputeCascadeLightVP(const Vec3& sunDir, const Vec3& up, const Mat4& invCameraViewProj, float t0, float t1,
                           int resolution, float zPad)
{
    std::array<Vec3, 8> full = FrustumCornersWS(invCameraViewProj);
    std::array<Vec3, 8> slice = SliceFrustum(full, t0, t1);
    BoundingSphere bs = FrustumBoundingSphere(slice);
    Mat4 lightView = LightView(sunDir, up);
    OrthoFit fit = FitOrthoSphere(lightView, bs.center, bs.radius, resolution, zPad);
    return Mul(fit.proj, lightView);
}

// Cascaded-shadow kernel (pure).

Mat4 Translate(const Vec3& t)
{
    Mat4 m = Identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

std::array<Vec3, 8> CameraFrustumCornersWorld(const Vec3& camPos, const Vec3& forward, const Vec3& right,
                                              const Vec3& up, float tanHalfX, float tanHalfY, float nearD, float farD)
{
    static const float sx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    static const float sy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    auto corner = [&](float d, int i) -> Vec3
    { return camPos + forward * d + right * (tanHalfX * d * sx[i]) + up * (tanHalfY * d * sy[i]); };
    std::array<Vec3, 8> out;
    for (int i = 0; i < 4; i++)
    {
        out[i] = corner(nearD, i);
    }
    for (int i = 0; i < 4; i++)
    {
        out[i + 4] = corner(farD, i);
    }
    return out;
}

Mat4 CascadeLightVPWorld(const std::array<Vec3, 8>& worldCorners, float t0, float t1, const Vec3& sunDir,
                         const Vec3& up, int resolution, float zPad)
{
    std::array<Vec3, 8> slice = SliceFrustum(worldCorners, t0, t1);
    BoundingSphere bs = FrustumBoundingSphere(slice);
    Mat4 lv = LightView(sunDir, up);
    OrthoFit fit = FitOrthoSphere(lv, bs.center, bs.radius, resolution, zPad);
    return Mul(fit.proj, lv);
}

Mat4 ToCameraRelative(const Mat4& worldLightVP, const Vec3& camPos)
{
    return Mul(worldLightVP, Translate(camPos));
}

float QuantizeShadowRadius(float radius, float minRadius, float stepRatio)
{
    if (!(radius > minRadius))
    {
        return minRadius;
    }
    const double n =
        std::ceil(std::log(static_cast<double>(radius) / minRadius) / std::log(static_cast<double>(stepRatio)) - 1e-9);
    return static_cast<float>(minRadius * std::pow(static_cast<double>(stepRatio), n));
}

Mat4 CascadeLightVPStable(const std::array<Vec3, 8>& corners, float t0, float t1, const Vec3& sunDir, const Vec3& up,
                          int resolution, float zPad, const Vec3& camPos)
{
    std::array<Vec3, 8> slice = SliceFrustum(corners, t0, t1);
    BoundingSphere bs = FrustumBoundingSphere(slice); // camera-relative centre (small)
    Mat4 lv = LightView(sunDir, up);
    Vec3 lc = TransformPoint(lv, bs.center); // camera-relative light-space centre
    // Quantized radius: the raw sphere scales with the camera FOV (aim zoom);
    // a fixed rung keeps the texel grid from rescaling every zoom frame.
    float radius = QuantizeShadowRadius(bs.radius < 1e-4f ? 1e-4f : bs.radius);
    int res = resolution > 0 ? resolution : 1;
    double texel = (2.0 * static_cast<double>(radius)) / static_cast<double>(res);

    // Camera's world position in light space, in DOUBLE — this is the only large
    // quantity, and it cancels out of the final (small) min corner, so the snap
    // grid is world-aligned without any large value entering the float matrix.
    double lvCamX = static_cast<double>(lv.at(0, 0)) * camPos.x + static_cast<double>(lv.at(0, 1)) * camPos.y +
                    static_cast<double>(lv.at(0, 2)) * camPos.z + static_cast<double>(lv.at(0, 3));
    double lvCamY = static_cast<double>(lv.at(1, 0)) * camPos.x + static_cast<double>(lv.at(1, 1)) * camPos.y +
                    static_cast<double>(lv.at(1, 2)) * camPos.z + static_cast<double>(lv.at(1, 3));

    // Snap the min corner to the WORLD texel grid (multiples of texel from the
    // world origin), then express it back in camera-relative space.
    double worldMinX = (static_cast<double>(lc.x) - radius) + lvCamX;
    double worldMinY = (static_cast<double>(lc.y) - radius) + lvCamY;
    double snappedX = std::floor(worldMinX / texel) * texel - texel;
    double snappedY = std::floor(worldMinY / texel) * texel - texel;
    float minX = static_cast<float>(snappedX - lvCamX);
    float minY = static_cast<float>(snappedY - lvCamY);
    float ext = static_cast<float>(2.0 * static_cast<double>(radius) + 2.0 * texel);
    float maxX = minX + ext;
    float maxY = minY + ext;

    float zMin = lc.z - radius;
    float zMax = lc.z + radius + std::max(0.0f, zPad);
    return Mul(Ortho(minX, maxX, minY, maxY, -zMax, -zMin), lv);
}

int SelectCascade(float eyeDepth, const float* splitViewDistances, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (eyeDepth < splitViewDistances[i])
        {
            return i;
        }
    }
    return count;
}

float ShadowFarFade(float eyeDepth, float lastSplit, float fadeRange)
{
    if (fadeRange <= 0.0f)
    {
        return eyeDepth <= lastSplit ? 1.0f : 0.0f;
    }
    float f = (lastSplit - eyeDepth) / fadeRange; // 1 at lastSplit-fadeRange, 0 at lastSplit
    if (f < 0.0f)
    {
        return 0.0f;
    }
    if (f > 1.0f)
    {
        return 1.0f;
    }
    return f;
}

CascadeSet BuildShadowCascades(const CascadeBuildParams& p)
{
    CascadeSet cs;
    int n = p.count < 1 ? 1 : (p.count > 4 ? 4 : p.count);
    cs.count = n;

    float farD = (p.farD > p.nearD + 1.0f) ? p.farD : p.nearD + 1.0f;
    float shadowFar = p.nearD + p.distanceCoef * (farD - p.nearD);
    if (shadowFar < p.nearD + 1.0f)
    {
        shadowFar = p.nearD + 1.0f;
    }

    // CAMERA-RELATIVE frustum corners (camera at the origin): the fit stays in
    // small coordinates and is snapped to a world grid in double inside
    // CascadeLightVPStable, so the result is precise AND world-anchored.
    std::array<Vec3, 8> corners = CameraFrustumCornersWorld({0.0f, 0.0f, 0.0f}, p.forward, p.right, p.up, p.tanHalfX,
                                                            p.tanHalfY, p.nearD, shadowFar);
    std::vector<float> splits = CascadeSplits(p.nearD, shadowFar, n, p.splitCoef);

    // Light up: avoid degeneracy when the sun is near-vertical.
    Vec3 sd = Normalize(p.sunDir);
    float ay = sd.y < 0.0f ? -sd.y : sd.y;
    Vec3 up = (ay > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    cs.sunDir = sd;

    float span = shadowFar - p.nearD;
    for (int i = 0; i < n; i++)
    {
        float t0 = (splits[i] - p.nearD) / span;
        float t1 = (splits[i + 1] - p.nearD) / span;
        cs.camRelVP[i] = CascadeLightVPStable(corners, t0, t1, sd, up, p.resolution, p.zPad, p.camPos);
        cs.splitViewDist[i] = splits[i + 1];
    }
    return cs;
}

Mat4 OmniSphereLightVP(const Vec3& camPos, float radius, const Vec3& sunDir, const Vec3& up, int resolution, float zPad)
{
    // Identical world-anchored snap to CascadeLightVPStable, but the bounded region
    // is a sphere centred on the camera (the origin in camera-relative space), not a
    // frustum slice — so it covers every direction around the player.
    Mat4 lv = LightView(sunDir, up);
    Vec3 lc = TransformPoint(lv, Vec3{0.0f, 0.0f, 0.0f}); // camera-origin in light space
    float r = radius < 1e-4f ? 1e-4f : radius;
    int res = resolution > 0 ? resolution : 1;
    double texel = (2.0 * static_cast<double>(r)) / static_cast<double>(res);

    double lvCamX = static_cast<double>(lv.at(0, 0)) * camPos.x + static_cast<double>(lv.at(0, 1)) * camPos.y +
                    static_cast<double>(lv.at(0, 2)) * camPos.z + static_cast<double>(lv.at(0, 3));
    double lvCamY = static_cast<double>(lv.at(1, 0)) * camPos.x + static_cast<double>(lv.at(1, 1)) * camPos.y +
                    static_cast<double>(lv.at(1, 2)) * camPos.z + static_cast<double>(lv.at(1, 3));

    double worldMinX = (static_cast<double>(lc.x) - r) + lvCamX;
    double worldMinY = (static_cast<double>(lc.y) - r) + lvCamY;
    double snappedX = std::floor(worldMinX / texel) * texel - texel;
    double snappedY = std::floor(worldMinY / texel) * texel - texel;
    float minX = static_cast<float>(snappedX - lvCamX);
    float minY = static_cast<float>(snappedY - lvCamY);
    float ext = static_cast<float>(2.0 * static_cast<double>(r) + 2.0 * texel);
    float maxX = minX + ext;
    float maxY = minY + ext;

    float zMin = lc.z - r;
    float zMax = lc.z + r + std::max(0.0f, zPad);
    return Mul(Ortho(minX, maxX, minY, maxY, -zMax, -zMin), lv);
}

Mat4 PointFaceVP(const Vec3& lightPos, int face, float nearD, float farD, const Vec3& camPos)
{
    static const Vec3 kDir[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                                 {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
    // The up vector only has to be perpendicular to the face's axis; which perpendicular does
    // not matter, because the receiver reads the map through the very same matrix.
    static const Vec3 kUp[6] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
                                {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const int f6 = (face < 0 || face > 5) ? 0 : face;
    const Mat4 view = LookAt(lightPos, lightPos + kDir[f6], kUp[f6]);
    float n = nearD > 1e-3f ? nearD : 1e-3f;
    float fa = farD > n * 1.01f ? farD : n * 1.01f;
    const Mat4 proj = Perspective(1.57079632679f, 1.0f, n, fa); // exactly 90 degrees, square
    return ToCameraRelative(Mul(proj, view), camPos);
}

Mat4 SpotLightVP(const Vec3& lightPos, const Vec3& lightDir, float outerHalfAngle, float nearD, float farD,
                 const Vec3& camPos, float marginScale)
{
    Vec3 dir = Normalize(lightDir);
    if (!(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z > 0.5f))
    {
        dir = Vec3{0.0f, -1.0f, 0.0f}; // a degenerate direction: point it at the ground
    }
    // LookAt's basis is undefined when `up` is parallel to the view direction, and a street
    // lamp points STRAIGHT DOWN -- the one case a naive (0,1,0) gets wrong every time.
    const Vec3 up = (std::fabs(dir.y) > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Mat4 view = LookAt(lightPos, lightPos + dir, up);

    float half = outerHalfAngle * (marginScale > 0.0f ? marginScale : 1.0f);
    // Clamped well short of 90 degrees: tan() runs away there, and a cone that wide is not a
    // spot any more -- it wants the six faces of a point light instead.
    const float kMaxHalf = 1.4f; // ~80 degrees
    const float kMinHalf = 0.02f;
    half = half < kMinHalf ? kMinHalf : (half > kMaxHalf ? kMaxHalf : half);

    float n = nearD > 1e-3f ? nearD : 1e-3f;
    float f = farD > n * 1.01f ? farD : n * 1.01f;
    const Mat4 proj = Perspective(2.0f * half, 1.0f, n, f);
    return ToCameraRelative(Mul(proj, view), camPos);
}

CascadeSet BuildShadowCascadesTiered(const CascadeBuildParams& p)
{
    CascadeSet cs;
    int n = p.count < 1 ? 1 : (p.count > 4 ? 4 : p.count);
    int omniN = p.omniCount < 0 ? 0 : (p.omniCount > n ? n : p.omniCount);
    cs.count = n;
    cs.omniCount = omniN;

    float farD = (p.farD > p.nearD + 1.0f) ? p.farD : p.nearD + 1.0f;
    float shadowFar = p.nearD + p.distanceCoef * (farD - p.nearD);
    if (shadowFar < p.nearD + 1.0f)
    {
        shadowFar = p.nearD + 1.0f;
    }

    Vec3 sd = Normalize(p.sunDir);
    float ay = sd.y < 0.0f ? -sd.y : sd.y;
    Vec3 up = (ay > 0.99f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    cs.sunDir = sd;

    // Omni tiers: camera-centred spheres. Radius is a fraction of the shadow range,
    // BUT capped so the near tiers stay crisp even when the shadow distance is set
    // large. A sphere of radius R rendered at `resolution` texels gives 2R/res metres
    // per texel; without a cap a big shadow distance made even close shadows coarse
    // ("toothy"). Cap each tier to a target texel size (the tightest tier ~3 cm, each
    // looser tier proportionally more), which scales with resolution.
    const int omniRes = p.resolution > 0 ? p.resolution : 1;
    const float kOmniTexelBase = 0.03f; // metres/texel target for the tightest omni tier
    float lastOmniR = 0.0f;
    for (int i = 0; i < omniN; i++)
    {
        float r = p.omniCoef[i] * shadowFar;
        const float rMax = kOmniTexelBase * static_cast<float>(i + 1) * 0.5f * static_cast<float>(omniRes);
        if (r > rMax)
        {
            r = rMax;
        }
        if (r < 1.0f)
        {
            r = 1.0f;
        }
        cs.camRelVP[i] = OmniSphereLightVP(p.camPos, r, sd, up, p.resolution, p.zPad);
        cs.omniRadius[i] = r;
        cs.splitViewDist[i] = r; // unused by selection for omni, kept sane for fallthrough
        lastOmniR = r;
    }

    // Frustum tiers: slices of the camera frustum from the last omni radius (or the
    // camera near, if no omni tiers) out to the shadow range, PSSM-split.
    int frustumN = n - omniN;
    if (frustumN > 0)
    {
        float fNear = (omniN > 0) ? lastOmniR : p.nearD;
        if (fNear < p.nearD)
        {
            fNear = p.nearD;
        }
        std::array<Vec3, 8> corners = CameraFrustumCornersWorld({0.0f, 0.0f, 0.0f}, p.forward, p.right, p.up,
                                                                p.tanHalfX, p.tanHalfY, p.nearD, shadowFar);
        std::vector<float> fsplits = CascadeSplits(fNear, shadowFar, frustumN, p.splitCoef);
        float span = shadowFar - p.nearD;
        for (int k = 0; k < frustumN; k++)
        {
            int gi = omniN + k;
            float t0 = (fsplits[k] - p.nearD) / span;
            float t1 = (fsplits[k + 1] - p.nearD) / span;
            cs.camRelVP[gi] = CascadeLightVPStable(corners, t0, t1, sd, up, p.resolution, p.zPad, p.camPos);
            cs.splitViewDist[gi] = fsplits[k + 1];
            cs.omniRadius[gi] = 0.0f;
        }
    }
    return cs;
}

int SelectShadowTier(float dist3D, float eyeDepth, const float* omniRadius, int omniCount, const float* splitViewDist,
                     int count)
{
    for (int i = 0; i < omniCount && i < count; i++)
    {
        if (dist3D <= omniRadius[i])
        {
            return i;
        }
    }
    for (int i = omniCount; i < count; i++)
    {
        if (eyeDepth <= splitViewDist[i])
        {
            return i;
        }
    }
    return count;
}

CasterMode ClassifyShadowCaster(int special, int skipMask, int alphaMask)
{
    if (special & skipMask)
    {
        return CasterMode::Skip;
    }
    if (special & alphaMask)
    {
        return CasterMode::AlphaTest;
    }
    return CasterMode::Solid;
}

float CascadeBlendWeight(float eyeDepth, float cascadeFar, float band)
{
    if (band <= 0.0f)
    {
        return 0.0f;
    }
    float t = (eyeDepth - (cascadeFar - band)) / band; // 0 at far-band, 1 at far edge
    if (t < 0.0f)
    {
        return 0.0f;
    }
    if (t > 1.0f)
    {
        return 1.0f;
    }
    return t;
}

} // namespace Poseidon::shadow
