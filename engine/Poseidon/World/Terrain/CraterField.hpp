#pragma once

// BOOM-001: fine explosion craters on top of the terrain heightmap.
//
// The heightmap is 6.25 m per cell on the stock islands (12.5-50 m on others), so the
// ballistic terrain edit (Dev::RocketTerrainImpact -> PaintTerrain) can only ever make a
// wide, shallow dish there -- a real 120 mm shell crater is ~1.5 m across the bowl. The
// 2001 game hid that with a flat crater decal (CraterShell). This field is the crater as
// geometry: a signed height offset at 0.25 m cells (a bowl, a raised lip of thrown soil,
// clods across an ejecta blanket) plus a material channel (fresh soil, scorch).
//
// It is the same contract as MudField: Landscape::SurfaceY adds HeightOffsetAt, so men,
// vehicles, decals and AI stand IN the crater; the terrain shader adds the same bilinear
// offset to its vertices and normals and paints the material. The coarse terrain edit is
// kept for blasts big enough for the heightmap to show (see CraterShape::coarse...).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{

/// What one explosion digs, from its config. Pure, so tests pin the numbers.
struct CraterShape
{
    float radius = 0;     //!< bowl radius at the rim, metres
    float depth = 0;      //!< centre depth below the old ground, metres
    float lip = 0;        //!< rim height above the old ground, metres
    float ejecta = 0;     //!< outer radius of the thrown-soil blanket, metres
    float scorch = 0;     //!< 0..1 blackening of the bowl (high explosive)
    bool Valid() const { return radius > 0.05f && depth > 0.0f; }
};

/// Real-world anchors (field measurements of fresh craters in loam): a 120 mm tank HE round
/// ~0.7 m radius / 0.35 m deep; 152 mm artillery ~1.5-2 m / 0.8-1 m; a 250 kg bomb ~4-5 m /
/// 2 m. Config blast damage (indirectHit) is a gameplay proxy, so like the coarse edit this
/// scales by its cube root, anchored at the LAW (indirectHit 150). `heightAboveGround` is how
/// far above the ground the charge went off: an air burst digs nothing.
/// Linear size of a ground burst relative to the LAW/120 mm shell (indirectHit 150). Above it,
/// the cube root (charge mass ~ crater volume). Below it the size falls much faster: a cube
/// root kept a hand grenade (18) at half a shell -- a 1.6 m crater half a metre deep and a dirt
/// column to match (owner, 2026-10-19: "granades do not create such big craters or smoke").
/// Real ones scoop a few decimetres of soil and throw a puff of dust.
inline float BlastScale(float indirectHit)
{
    if (!std::isfinite(indirectHit) || indirectHit <= 0) return 0.0f;
    const float e = indirectHit / 150.0f;
    return std::clamp(e < 1.0f ? std::pow(e, 0.75f) : std::cbrt(e), 0.06f, 3.0f);
}

inline CraterShape CraterShapeFor(float indirectHit, float indirectHitRange, float heightAboveGround)
{
    CraterShape c;
    if (!std::isfinite(indirectHit) || indirectHit <= 0 || !std::isfinite(heightAboveGround)) return c;
    const float s = BlastScale(indirectHit);
    // A burst at the surface digs fully; the bowl shrinks to nothing by ~1.2 m (scaled) up.
    const float h = std::max(0.0f, heightAboveGround);
    // (a small charge still couples a few decimetres up: a grenade on the ground goes off at 0.1-0.4 m)
    const float coupling = std::clamp(1.0f - h / (1.2f * std::max(s, 0.35f)), 0.0f, 1.0f);
    if (coupling <= 0.0f) return c;
    const float k = std::sqrt(coupling);
    // (owner, 2026-10-18: "a real crater, at least 1 m deep" for a shell)
    c.radius = 1.6f * s * k;
    // small charges scoop shallow: depth falls faster than width below a shell
    c.depth = 1.0f * (s < 1.0f ? std::pow(s, 1.3f) : s) * coupling;
    c.lip = 0.35f * s * k;
    c.ejecta = c.radius * 3.6f;
    // Range is the fragmentation/blast proxy: a big indirectHitRange is a large HE charge.
    const float range = std::isfinite(indirectHitRange) ? indirectHitRange : 0.0f;
    c.scorch = std::clamp(0.35f + 0.08f * range, 0.0f, 0.9f) * k;
    return c;
}

class CraterField
{
public:
    static constexpr int WindowSize = 512;
    static constexpr int ChunkSize = 16;
    static constexpr float CellSize = 0.25f;       // 128 m near window
    static constexpr float FarCellSize = 1.0f;     // 512 m far window
    static constexpr int FarRatio = 4;
    static constexpr float MaxDepth = 6.0f;        // the deepest offset ever stored (and uploaded)
    static constexpr float MaxRise = 1.5f;         // the highest heap
    static constexpr size_t MaxChunks = 12288;     // ~1,300 shell craters before new ones are refused
    bool enabled = true;

    void Reset()
    {
        _chunks.clear();
        _discs.clear();
        _craters = 0;
        _rejected = 0;
        ++_revision;
    }
    size_t Chunks() const { return _chunks.size(); }
    size_t Craters() const { return _craters; }
    size_t Rejected() const { return _rejected; }
    uint64_t Revision() const { return _revision; }

    /// Dig one crater. `seed` makes the rim, the rays and the clods differ per crater.
    /// Returns false when nothing was changed (disabled, invalid, out of chunks).
    bool Dig(float x, float z, const CraterShape& c, uint32_t seed)
    {
        if (!enabled || !c.Valid() || !std::isfinite(x) || !std::isfinite(z) || std::abs(x) > 1e6f ||
            std::abs(z) > 1e6f)
            return false;
        const float reach = std::max(c.ejecta, c.radius * 1.2f) + 0.5f;
        const int x0 = int(std::floor((x - reach) / CellSize)), x1 = int(std::floor((x + reach) / CellSize));
        const int z0 = int(std::floor((z - reach) / CellSize)), z1 = int(std::floor((z + reach) / CellSize));
        // Rim irregularity: a few low angular harmonics with per-crater phases.
        float amp[4], phase[4];
        for (int i = 0; i < 4; ++i)
        {
            amp[i] = (0.05f + 0.07f * Hash01(seed, 11 + i)) / float(i + 1);
            phase[i] = 6.2831853f * Hash01(seed, 21 + i);
        }
        // Clods: a few dozen lumps thrown onto the blanket, more for a bigger crater.
        struct Clod { float x, z, r, h; };
        std::array<Clod, 48> clods{};
        const int clodCount = std::clamp(int(10 + c.radius * 14), 8, int(clods.size()));
        for (int i = 0; i < clodCount; ++i)
        {
            const float a = 6.2831853f * Hash01(seed, 100 + i);
            const float d = c.radius * (1.05f + 1.9f * std::pow(Hash01(seed, 200 + i), 1.6f));
            const float size = c.radius * (0.05f + 0.09f * Hash01(seed, 300 + i));
            clods[i] = {x + std::cos(a) * d, z + std::sin(a) * d, std::max(size, 0.12f),
                        std::min(size * 0.7f, 0.25f)};
        }
        bool changed = false;
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float wx = (ix + 0.5f) * CellSize, wz = (iz + 0.5f) * CellSize;
                const float dx = wx - x, dz = wz - z;
                const float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > reach) continue;
                const float ang = std::atan2(dz, dx);
                float wobble = 1.0f;
                for (int i = 0; i < 4; ++i) wobble += amp[i] * std::sin(float(i + 2) * ang + phase[i]);
                const float r = dist / (c.radius * wobble);
                // Rays of thrown soil: narrow angular streaks across the blanket.
                const float ray = std::pow(std::max(0.0f, std::sin(9.0f * ang + phase[0]) *
                                                           std::sin(5.0f * ang + phase[1])), 2.0f);
                float bowl = 0.0f, heap = 0.0f, soil = 0.0f, scorch = 0.0f;
                if (r < 1.0f)
                {
                    // Parabolic floor flattening at the bottom, rising into the lip.
                    const float floorShape = 1.0f - r * r;
                    bowl = -c.depth * floorShape * (0.85f + 0.15f * floorShape) + c.lip * r * r * r * r;
                    soil = 1.0f;
                    scorch = c.scorch * std::clamp(1.0f - r * 0.9f, 0.0f, 1.0f);
                }
                else
                {
                    const float rr = std::min(r, c.ejecta / c.radius);
                    const float outer = c.ejecta / c.radius;
                    // Ejecta thickness falls off ~r^-3 (McGetchin), pinned to zero at the outer edge.
                    const float inv = 1.0f / (rr * rr * rr), invEdge = 1.0f / (outer * outer * outer);
                    heap = c.lip * std::max(0.0f, (inv - invEdge) / (1.0f - invEdge)) * (0.8f + 0.5f * ray);
                    // Fresh soil: solid only on the lip and just past it, then broken rays of thrown
                    // earth. A solid blanket out to the ejecta edge (3.6 radii, ~11 m across for a
                    // shell) read from a distance as a flat brown stain with the bowl lost in it --
                    // a decal, not a hole (owner, 2026-10-19).
                    const float core = 1.0f - std::clamp((r - 1.25f) / 0.6f, 0.0f, 1.0f);
                    const float t = std::clamp((r - 1.0f) / (outer - 1.0f), 0.0f, 1.0f);
                    const float rays = ray * (1.0f - t) * (1.0f - t);
                    soil = std::clamp(std::max(core, rays * 1.1f), 0.0f, 1.0f);
                    scorch = c.scorch * 0.6f * std::clamp(1.0f - (r - 1.0f) * 1.6f, 0.0f, 1.0f);
                }
                for (int i = 0; i < clodCount; ++i)
                {
                    const float cx = wx - clods[i].x, cz = wz - clods[i].z;
                    const float q = (cx * cx + cz * cz) / (clods[i].r * clods[i].r);
                    if (q < 1.0f)
                    {
                        heap += clods[i].h * (1.0f - q) * (1.0f - q);
                        soil = std::max(soil, 1.0f - q);
                    }
                }
                if (bowl == 0.0f && heap <= 0.0005f && soil <= 0.002f && scorch <= 0.002f) continue;
                Chunk* chunk = ChunkFor(ix, iz, true);
                if (!chunk) { ++_rejected; continue; }
                const int i = (iz - FloorChunk(iz) * ChunkSize) * ChunkSize + ix - FloorChunk(ix) * ChunkSize;
                float& h = chunk->height[i];
                const float before = h;
                if (r < 1.0f)
                    h = std::min(h, bowl);          // excavation removes older lips and fill
                else
                    h = h + heap;                   // thrown soil lands on what is there
                h = std::clamp(h, -MaxDepth, MaxRise);
                const float s0 = chunk->soil[i], c0 = chunk->scorch[i];
                chunk->soil[i] = std::max(chunk->soil[i], soil);
                chunk->scorch[i] = std::clamp(std::max(chunk->scorch[i], scorch), 0.0f, 1.0f);
                changed |= h != before || chunk->soil[i] != s0 || chunk->scorch[i] != c0;
            }
        if (changed)
        {
            _discs.push_back({x, z, c.radius * 1.6f, 6.2831853f * Hash01(seed, 7)});
            ++_craters;
            ++_revision;
        }
        return changed;
    }

    /// The shared geometry contract (cell-centre bilinear), copied exactly by terrain.wgsl.
    float HeightOffsetAt(float x, float z) const
    {
        if (!enabled || _chunks.empty() || !std::isfinite(x) || !std::isfinite(z)) return 0.0f;
        const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
        const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
        const float fx = tx - ix, fz = tz - iz;
        const float a = std::lerp(CellHeight(ix, iz), CellHeight(ix + 1, iz), fx);
        const float b = std::lerp(CellHeight(ix, iz + 1), CellHeight(ix + 1, iz + 1), fx);
        return std::lerp(a, b, fz);
    }
    std::array<float, 2> HeightGradientAt(float x, float z) const
    {
        if (!enabled || _chunks.empty() || !std::isfinite(x) || !std::isfinite(z)) return {};
        const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
        const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
        const float fx = tx - ix, fz = tz - iz;
        const float a = CellHeight(ix, iz), b = CellHeight(ix + 1, iz);
        const float c = CellHeight(ix, iz + 1), d = CellHeight(ix + 1, iz + 1);
        return {std::lerp(b - a, d - c, fz) / CellSize, std::lerp(c - a, d - b, fx) / CellSize};
    }
    /// Fresh-soil coverage 0..1 at a point (nearest cell): grass is not drawn on it.
    float SoilAt(float x, float z) const
    {
        if (!enabled || _chunks.empty() || !std::isfinite(x) || !std::isfinite(z)) return 0.0f;
        const int ix = int(std::floor(x / CellSize)), iz = int(std::floor(z / CellSize));
        const Chunk* chunk = ChunkFor(ix, iz);
        return chunk ? chunk->soil[(iz - FloorChunk(iz) * ChunkSize) * ChunkSize + ix - FloorChunk(ix) * ChunkSize]
                     : 0.0f;
    }

    /// Grass placement's list: `[count, 0, 0, 0]` then the `maxCount` craters nearest (x, z) as
    /// `(x, z, soil radius, seed)` -- the dense fresh soil where no grass grows.
    std::vector<float> NearestDiscs(float x, float z, int maxCount) const
    {
        std::vector<std::array<float, 4>> near;
        if (enabled && std::isfinite(x) && std::isfinite(z))
        {
            near = _discs;
            const auto dist = [&](const std::array<float, 4>& d) { return (d[0] - x) * (d[0] - x) + (d[1] - z) * (d[1] - z); };
            const size_t keep = std::min(near.size(), size_t(std::max(maxCount, 0)));
            std::partial_sort(near.begin(), near.begin() + keep, near.end(),
                              [&](const auto& a, const auto& b) { return dist(a) < dist(b); });
            near.resize(keep);
        }
        std::vector<float> out(4 + near.size() * 4, 0.0f);
        out[0] = float(near.size());
        for (size_t i = 0; i < near.size(); ++i)
            std::copy(near[i].begin(), near[i].end(), out.begin() + 4 + i * 4);
        return out;
    }

    /// Material packing: soil and scorch quantised to 0..255 each, one exact integer in a float.
    static float PackMaterial(float soil, float scorch)
    {
        const int s = int(std::lround(std::clamp(soil, 0.0f, 1.0f) * 255.0f));
        const int k = int(std::lround(std::clamp(scorch, 0.0f, 1.0f) * 255.0f));
        return float(s + 256 * k);
    }

    /// GPU views, MudField's packing: [originX, originZ, cell, MaxDepth] then 512*512 row-major
    /// signed heights at cell centres, then (same header shape) the packed materials. `farView`
    /// selects 1 m cells over 512 m (averaged heights, max material). A view with no craters
    /// in it has info.w = 0 so the shader skips it.
    std::vector<float> Snapshot(float centreX, float centreZ, bool farView) const
    {
        const int view = 4 + WindowSize * WindowSize;
        std::vector<float> data(size_t(view) * 2, 0.0f);
        const float cell = farView ? FarCellSize : CellSize;
        data[2] = data[view + 2] = cell;
        if (!enabled || !std::isfinite(centreX) || !std::isfinite(centreZ)) return data;
        const int ratio = farView ? FarRatio : 1;
        const int span = WindowSize * ratio;   // fine cells across
        const int ox = FloorChunk(int(std::floor(centreX / CellSize))) * ChunkSize - span / 2;
        const int oz = FloorChunk(int(std::floor(centreZ / CellSize))) * ChunkSize - span / 2;
        data[0] = data[view] = ox * CellSize;
        data[1] = data[view + 1] = oz * CellSize;
        bool any = false;
        for (const auto& [key, chunk] : _chunks)
        {
            const int kx = int(int32_t(uint32_t(key >> 32))), kz = int(int32_t(uint32_t(key & 0xffffffffu)));
            const int fx0 = kx * ChunkSize - ox, fz0 = kz * ChunkSize - oz;
            if (fx0 < 0 || fz0 < 0 || fx0 >= span || fz0 >= span) continue;
            any = true;
            for (int cz = 0; cz < ChunkSize / ratio; ++cz)
                for (int cx = 0; cx < ChunkSize / ratio; ++cx)
                {
                    float sum = 0.0f, soil = 0.0f, scorch = 0.0f;
                    for (int dz = 0; dz < ratio; ++dz)
                        for (int dx = 0; dx < ratio; ++dx)
                        {
                            const int i = (cz * ratio + dz) * ChunkSize + cx * ratio + dx;
                            sum += chunk.height[i];
                            soil = std::max(soil, chunk.soil[i]);
                            scorch = std::max(scorch, chunk.scorch[i]);
                        }
                    const int i = fx0 / ratio + cx, j = fz0 / ratio + cz;
                    data[4 + j * WindowSize + i] = std::clamp(sum / float(ratio * ratio), -MaxDepth, MaxRise);
                    data[view + 4 + j * WindowSize + i] = PackMaterial(soil, scorch);
                }
        }
        data[3] = data[view + 3] = any ? MaxDepth : 0.0f;
        return data;
    }

private:
    struct Chunk
    {
        std::array<float, ChunkSize * ChunkSize> height{};
        std::array<float, ChunkSize * ChunkSize> soil{};
        std::array<float, ChunkSize * ChunkSize> scorch{};
    };
    static int FloorChunk(int cell) { return cell >= 0 ? cell / ChunkSize : -((-cell + ChunkSize - 1) / ChunkSize); }
    static uint64_t Key(int cx, int cz) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz); }
    static float Hash01(uint32_t seed, uint32_t salt)
    {
        uint32_t h = seed * 0x9E3779B1u ^ (salt + 0x7F4A7C15u) * 0x85EBCA77u;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
        return float(h & 0xFFFFFFu) / float(0x1000000);
    }
    const Chunk* ChunkFor(int ix, int iz) const
    {
        const auto it = _chunks.find(Key(FloorChunk(ix), FloorChunk(iz)));
        return it == _chunks.end() ? nullptr : &it->second;
    }
    Chunk* ChunkFor(int ix, int iz, bool create)
    {
        const uint64_t key = Key(FloorChunk(ix), FloorChunk(iz));
        auto it = _chunks.find(key);
        if (it != _chunks.end()) return &it->second;
        if (!create || _chunks.size() >= MaxChunks) return nullptr;
        return &_chunks.emplace(key, Chunk{}).first->second;
    }
    float CellHeight(int ix, int iz) const
    {
        const Chunk* chunk = ChunkFor(ix, iz);
        return chunk ? chunk->height[(iz - FloorChunk(iz) * ChunkSize) * ChunkSize + ix - FloorChunk(ix) * ChunkSize]
                     : 0.0f;
    }

    std::unordered_map<uint64_t, Chunk> _chunks;
    std::vector<std::array<float, 4>> _discs; // (x, z, soil radius, seed) per crater, for the grass
    size_t _craters = 0;
    size_t _rejected = 0;
    uint64_t _revision = 1;
};

inline CraterField& GCraters()
{
    static CraterField field;
    return field;
}

} // namespace Poseidon
