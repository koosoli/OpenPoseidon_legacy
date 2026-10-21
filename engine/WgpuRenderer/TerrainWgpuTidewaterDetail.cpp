// SPDX-License-Identifier: GPL-3.0-or-later
// Tidewater's terrain detail texture (matthewcosier/tidewater, island-expansion,
// src/world/terrain/DetailTextures.js, MIT): one tileable 512 x 512 RGBA8 image, four height fields
//   R rock: fractured plates (warped Voronoi cracks) with chipped facets and grain
//   G soil: leaf litter, clumps, a few stones
//   B sand: grains, grit and sparse pebbles
//   A fbm:  smooth multi-octave noise (macro variation at large scales)
// A line-for-line port: the same lattices, seeds and constants, so the terrain shader's rock
// (terrain.wgsl tw_rock) reads the field Tidewater's terrainRockSurface was tuned on.
#include "TerrainWgpuTidewaterDetail.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon
{
namespace
{
// TerrainNoise.js hash2 (Math.imul arithmetic: 32-bit wrap)
float Hash2(int i, int j, int s)
{
    uint32_t h = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u + uint32_t(s) * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(double(h) / 4294967296.0);
}

float Smooth(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// periodic gradient noise over a P x P lattice, roughly [-1, 1]
struct Noise
{
    int p;
    std::vector<float> gx, gy;
    Noise(int period, int seed) : p(period), gx(size_t(period) * period), gy(size_t(period) * period)
    {
        for (int j = 0; j < p; ++j)
            for (int i = 0; i < p; ++i)
            {
                const float a = Hash2(i, j, seed) * 6.2831853f;
                gx[size_t(j) * p + i] = std::cos(a);
                gy[size_t(j) * p + i] = std::sin(a);
            }
    }
    float operator()(float x, float y) const
    {
        const float xi = std::floor(x), yi = std::floor(y);
        const float xf = x - xi, yf = y - yi;
        const int x0 = ((int(xi) % p) + p) % p, y0 = ((int(yi) % p) + p) % p;
        const int x1 = x0 + 1 == p ? 0 : x0 + 1, y1 = y0 + 1 == p ? 0 : y0 + 1;
        const size_t k00 = size_t(y0) * p + x0, k10 = size_t(y0) * p + x1, k01 = size_t(y1) * p + x0,
                     k11 = size_t(y1) * p + x1;
        const float a = gx[k00] * xf + gy[k00] * yf;
        const float b = gx[k10] * (xf - 1) + gy[k10] * yf;
        const float c = gx[k01] * xf + gy[k01] * (yf - 1);
        const float d = gx[k11] * (xf - 1) + gy[k11] * (yf - 1);
        const float u = xf * xf * xf * (xf * (xf * 6 - 15) + 10);
        const float v = yf * yf * yf * (yf * (yf * 6 - 15) + 10);
        return (a + (b - a) * u + (c - a + (a - b - c + d) * u) * v) * 1.414f;
    }
};

struct Fbm
{
    std::vector<Noise> oct;
    Fbm(int p, int octaves, int seed)
    {
        for (int o = 0; o < octaves; ++o)
            oct.emplace_back(p << o, seed + o * 17);
    }
    float operator()(float x, float y) const
    {
        float s = 0, amp = 1, norm = 0, f = 1;
        for (const Noise& n : oct)
        {
            s += n(x * f, y * f) * amp;
            norm += amp;
            amp *= 0.5f;
            f *= 2;
        }
        return s / norm;
    }
};

// periodic Voronoi over P x P cells, feature points with a 2-cell apron
struct Worley
{
    struct Out
    {
        float f1, edge, id, dx, dy;
    };
    int p, q;
    std::vector<float> px, py, pid;
    Worley(int period, int seed) : p(period), q(period + 4)
    {
        px.resize(size_t(q) * q);
        py.resize(size_t(q) * q);
        pid.resize(size_t(q) * q);
        for (int j = 0; j < q; ++j)
            for (int i = 0; i < q; ++i)
            {
                const int wi = (i - 2 + p) % p, wj = (j - 2 + p) % p;
                px[size_t(j) * q + i] = Hash2(wi, wj, seed) + float(i - 2);
                py[size_t(j) * q + i] = Hash2(wi, wj, seed + 1) + float(j - 2);
                pid[size_t(j) * q + i] = Hash2(wi, wj, seed + 2);
            }
    }
    Out operator()(float x, float y, int r = 2) const
    {
        const int xi = int(std::floor(x)), yi = int(std::floor(y));
        float f1 = 99, id = 0;
        size_t kn = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const size_t k = size_t(yi + j + 2) * q + 2 + xi + i;
                const float dx = px[k] - x, dy = py[k] - y, d = dx * dx + dy * dy;
                if (d < f1)
                {
                    f1 = d;
                    id = pid[k];
                    kn = k;
                }
            }
        float edge = 99;
        if (r > 1)
        {
            const float ax = px[kn], ay = py[kn];
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i)
                {
                    const size_t k = size_t(yi + j + 2) * q + 2 + xi + i;
                    if (k == kn)
                        continue;
                    const float ex = px[k] - ax, ey = py[k] - ay, el = std::sqrt(ex * ex + ey * ey);
                    const float e = ((ax + px[k]) * 0.5f - x) * ex / el + ((ay + py[k]) * 0.5f - y) * ey / el;
                    edge = std::min(edge, e);
                }
        }
        return {std::sqrt(f1), edge, id, x - px[kn], y - py[kn]};
    }
};
} // namespace

std::vector<uint8_t> BuildTidewaterDetail()
{
    constexpr int S = TidewaterDetailSize;
    const Fbm warpA(4, 2, 11), warpB(4, 2, 23);
    const Worley rockCells(7, 101), rockChips(23, 211);
    const Fbm rockGrain(16, 3, 307);
    const Worley leafCells(26, 401), stoneCells(12, 601);
    const Fbm soilFbm(10, 3, 503);
    const Noise grainA(180, 701), grainB(90, 709);
    const Worley pebbles(18, 801);
    const Fbm macro(4, 5, 907);

    std::vector<uint8_t> data(size_t(S) * S * 4);
    for (int j = 0; j < S; ++j)
    {
        const float v = float(j) / S;
        for (int i = 0; i < S; ++i)
        {
            const float u = float(i) / S;
            uint8_t* o = &data[(size_t(j) * S + i) * 4];

            // ---- rock: plates split by warped cracks, each plate a tilted facet, chipped edges, grain
            float wu = u + warpA(u * 4, v * 4) * 0.035f, wv = v + warpB(u * 4, v * 4) * 0.035f;
            wu -= std::floor(wu);
            wv -= std::floor(wv);
            Worley::Out w = rockCells(wu * 7, wv * 7);
            const float ta = w.id * 40.0f;
            const float bevel = Smooth(0.0f, 0.2f, w.edge);
            const float facet = ((w.dx * std::cos(ta) + w.dy * std::sin(ta)) * 0.5f + (w.id - 0.5f) * 0.22f) * bevel;
            const float joint = Smooth(0.0f, 0.1f + 0.08f * w.id, w.edge);
            const Worley::Out chips = rockChips(wu * 23, wv * 23, 2);
            const float tb = chips.id * 40.0f;
            const float chipB = Smooth(0.0f, 0.14f, chips.edge);
            const float chip = ((chips.dx * std::cos(tb) + chips.dy * std::sin(tb)) * 0.22f + (chips.id - 0.5f) * 0.1f) * chipB;
            const float rg = rockGrain(u * 16, v * 16);
            float r = (0.45f + facet + chip * 0.6f + 0.16f * rg) * (0.66f + 0.34f * joint) * (0.94f + 0.06f * chipB);

            // ---- soil / litter
            w = leafCells(u * 26, v * 26, 1);
            const float leaf = w.id < 0.6f ? (1 - Smooth(0.1f + 0.15f * w.id, 0.45f, w.f1)) * (0.4f + 0.6f * w.id) : 0.0f;
            const float clump = soilFbm(u * 10, v * 10);
            const Worley::Out st = stoneCells(u * 12, v * 12, 1);
            const float stone = st.id < 0.15f ? (1 - Smooth(0.06f, 0.28f, st.f1)) : 0.0f;
            float g = 0.34f + 0.3f * clump + 0.26f * leaf + 0.45f * stone;

            // ---- sand
            const float grain = grainA(u * 180, v * 180) * 0.55f + grainB(u * 90, v * 90) * 0.45f;
            const Worley::Out pw = pebbles(u * 18, v * 18, 1);
            const float pr = pw.f1 / (0.14f + 0.12f * pw.id * 5);
            const float pebble = pw.id < 0.2f ? std::max(0.0f, 1 - pr * pr) : 0.0f;
            float b = 0.45f + 0.2f * grain + 0.5f * std::sqrt(pebble);

            // ---- generic fbm
            float a = 0.5f + 0.55f * macro(u * 4, v * 4);

            o[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255);
            o[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255);
            o[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255);
            o[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255);
        }
    }
    return data;
}
} // namespace Poseidon
