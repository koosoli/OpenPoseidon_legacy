#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
// Mission-local soft-ground geometry, independent of snow and of camera residency.
// Stored values are positive depression metres; all public height offsets are
// negative. Drying affects new contacts, never erases an existing imprint.
class MudField
{
public:
    static constexpr int WindowSize = 512;
    static constexpr int ChunkSize = 16;
    static constexpr float CellSize = 0.125f;
    static constexpr float MaxDepth = 0.06f;
    static constexpr size_t MaxChunks = 8192;
    bool enabled = true;

    void Reset()
    {
        _chunks.clear();
        _wetness = 0.0f;
        _rejected = 0;
        ++_revision;
    }
    void Advance(float seconds, float liquidRain)
    {
        if (!enabled || !std::isfinite(seconds) || seconds <= 0.0f) return;
        liquidRain = std::isfinite(liquidRain) ? std::clamp(liquidRain, 0.0f, 1.0f) : 0.0f;
        const double fill = double(liquidRain) / 30.0;
        const double dry = (1.0 - double(liquidRain)) / 180.0;
        const double rate = fill + dry;
        _wetness = float(std::clamp(fill / rate + (double(_wetness) - fill / rate) *
            std::exp(-rate * seconds), 0.0, 1.0));
    }
    float Wetness() const { return _wetness; }
    bool ContactActive() const { return enabled && _wetness > 0.15f; }
    // Wet soil gradually supports a 0..6cm impression, not a mandatory 2cm sink
    // on the first rainy frame. The caller must independently prove soft ground.
    float ContactDepth() const
    {
        const float t = std::clamp((_wetness - 0.15f) / 0.85f, 0.0f, 1.0f);
        return enabled ? MaxDepth * t * t * (3.0f - 2.0f * t) : 0.0f;
    }
    size_t Chunks() const { return _chunks.size(); }
    size_t Rejected() const { return _rejected; }
    uint64_t Revision() const { return _revision; }

    // A finite bounded contact disc. Overlapping/repeated feet take the maximum
    // depression rather than digging infinitely through repeated animation edges.
    bool Stamp(float x, float z, float radius, float depth)
    {
        if (!enabled || !FinitePosition(x, z) || !std::isfinite(radius) || radius <= 0.0f ||
            !std::isfinite(depth) || depth <= 0.0f) return false;
        radius = std::clamp(radius, CellSize, 0.25f);
        depth = std::min(depth, MaxDepth);
        const int x0 = int(std::floor((x - radius) / CellSize));
        const int x1 = int(std::floor((x + radius) / CellSize));
        const int z0 = int(std::floor((z - radius) / CellSize));
        const int z1 = int(std::floor((z + radius) / CellSize));
        bool changed = false;
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float r = std::hypot((ix + 0.5f) * CellSize - x, (iz + 0.5f) * CellSize - z) / radius;
                if (r >= 1.0f) continue;
                const float value = depth * (1.0f - r * r * (3.0f - 2.0f * r));
                const int cx = FloorChunk(ix), cz = FloorChunk(iz);
                auto it = _chunks.find(Key(cx, cz));
                if (it == _chunks.end())
                {
                    if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                    it = _chunks.emplace(Key(cx, cz), Chunk{}).first;
                }
                float& stored = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
                if (value > stored) { stored = value; changed = true; }
            }
        if (changed) ++_revision;
        return changed;
    }

    // This cell-centre bilinear rule is the shared signed geometry contract.
    // A renderer should copy it exactly, not sample offsets as a colour decal.
    float HeightOffsetAt(float x, float z) const
    {
        if (!enabled || _chunks.empty() || !FinitePosition(x, z)) return 0.0f;
        const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
        const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
        const float fx = tx - ix, fz = tz - iz;
        const float a = std::lerp(CellDepth(ix, iz), CellDepth(ix + 1, iz), fx);
        const float b = std::lerp(CellDepth(ix, iz + 1), CellDepth(ix + 1, iz + 1), fx);
        return -std::lerp(a, b, fz);
    }
    std::array<float, 2> HeightGradientAt(float x, float z) const
    {
        if (!enabled || _chunks.empty() || !FinitePosition(x, z)) return {};
        const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
        const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
        const float fx = tx - ix, fz = tz - iz;
        const float a = CellDepth(ix, iz), b = CellDepth(ix + 1, iz);
        const float c = CellDepth(ix, iz + 1), d = CellDepth(ix + 1, iz + 1);
        return {-std::lerp(b - a, d - c, fz) / CellSize,
                -std::lerp(c - a, d - b, fx) / CellSize};
    }

    // Exact float packing: [originX, originZ, CellSize, MaxDepth] then
    // 512*512 row-major SIGNED offsets at world cell centres. Disabled/invalid
    // snapshots have info.w=0 and no deformation. Revision plus origin permits
    // avoiding unchanged uploads; camera movement never deletes stored cells.
    std::vector<float> Snapshot(float cameraX, float cameraZ) const
    {
        std::vector<float> data(4 + WindowSize * WindowSize, 0.0f);
        data[2] = CellSize;
        if (!enabled || !FinitePosition(cameraX, cameraZ)) return data;
        const int ox = FloorChunk(int(std::floor(cameraX / CellSize))) * ChunkSize - WindowSize / 2;
        const int oz = FloorChunk(int(std::floor(cameraZ / CellSize))) * ChunkSize - WindowSize / 2;
        data[0] = ox * CellSize;
        data[1] = oz * CellSize;
        data[3] = MaxDepth;
        for (int z = 0; z < WindowSize; z += ChunkSize)
            for (int x = 0; x < WindowSize; x += ChunkSize)
            {
                const auto it = _chunks.find(Key(FloorChunk(ox + x), FloorChunk(oz + z)));
                if (it == _chunks.end()) continue;
                for (int dz = 0; dz < ChunkSize; ++dz)
                    for (int dx = 0; dx < ChunkSize; ++dx)
                        data[4 + (z + dz) * WindowSize + x + dx] = -it->second.cells[dz * ChunkSize + dx];
            }
        return data;
    }

private:
    struct Chunk { std::array<float, ChunkSize * ChunkSize> cells{}; };
    static bool FinitePosition(float x, float z)
    {
        return std::isfinite(x) && std::isfinite(z) && std::abs(x) <= 1000000.0f && std::abs(z) <= 1000000.0f;
    }
    static int FloorChunk(int x) { return int(std::floor(double(x) / ChunkSize)); }
    static uint64_t Key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
    float CellDepth(int ix, int iz) const
    {
        const int cx = FloorChunk(ix), cz = FloorChunk(iz);
        const auto it = _chunks.find(Key(cx, cz));
        return it == _chunks.end() ? 0.0f : it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
    }
    std::unordered_map<uint64_t, Chunk> _chunks;
    float _wetness = 0.0f;
    size_t _rejected = 0;
    uint64_t _revision = 0;
};

inline MudField& GMud() { static MudField mud; return mud; }
} // namespace Poseidon
