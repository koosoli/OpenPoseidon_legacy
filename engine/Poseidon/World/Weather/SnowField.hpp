#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
// Experimental, mission-local surface state. Camera movement never evicts tracks.
// Only new precipitation reduces a depression; the GPU window is just a view.
class SnowField
{
public:
    static constexpr int WindowSize = 512;
    static constexpr float CellSize = 0.125f;
    static constexpr int ChunkSize = 16;
    static constexpr size_t MaxChunks = 32768;
    bool enabled = false;
    bool falling = true;
    bool detailedGeometry = true;
    float metresPerMinute = 0.01f;
    float maxDepth = 0.5f;
    float flakeMultiplier = 1.0f;

    float FallingIntensity() const
    {
        return enabled && falling && std::isfinite(metresPerMinute)
            ? std::clamp(metresPerMinute * 30.0f, 0.0f, 1.0f) : 0.0f;
    }
    float BoundedFlakeMultiplier() const
    {
        return std::isfinite(flakeMultiplier) ? std::clamp(flakeMultiplier, 0.25f, 8.0f) : 1.0f;
    }
    float RenderOvercast(float authored) const
    {
        const float intensity = FallingIntensity();
        // Render-only cover: no mutation of the mission's rain/thunder state.
        return intensity > 0.0f ? std::max(authored, 0.75f + 0.2f * intensity) : authored;
    }

    void Reset()
    {
        _chunks.clear();
        _depth = 0.0f;
        _precipitation = 0.0;
        _rejected = 0;
    }
    void Advance(float seconds)
    {
        if (enabled && falling && std::isfinite(seconds) && seconds > 0.0f)
            Deposit(std::min(seconds, 1.0f) * std::clamp(metresPerMinute, 0.0f, 1.0f) / 60.0f);
    }
    void Deposit(float metres)
    {
        if (!std::isfinite(metres) || metres <= 0.0f || !std::isfinite(maxDepth)) return;
        _precipitation += metres;
        _depth = std::min(_depth + metres, std::clamp(maxDepth, 0.01f, 1.0f));
    }
    float Depth() const { return _depth; }
    size_t Chunks() const { return _chunks.size(); }
    size_t Rejected() const { return _rejected; }

    void Stamp(float x, float z, float radius, float strength = 0.9f)
    {
        if (!enabled || _depth <= 0.0f || !std::isfinite(x) || !std::isfinite(z) ||
            std::abs(x) > 1000000.0f || std::abs(z) > 1000000.0f ||
            !std::isfinite(radius) || radius <= 0.0f || !std::isfinite(strength)) return;
        radius = std::clamp(radius, CellSize, 15.0f);
        const int x0 = static_cast<int>(std::floor((x - radius) / CellSize));
        const int z0 = static_cast<int>(std::floor((z - radius) / CellSize));
        const int x1 = static_cast<int>(std::floor((x + radius) / CellSize));
        const int z1 = static_cast<int>(std::floor((z + radius) / CellSize));
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float dx = (ix + 0.5f) * CellSize - x;
                const float dz = (iz + 0.5f) * CellSize - z;
                const float r = std::sqrt(dx * dx + dz * dz) / radius;
                if (r >= 1.0f) continue;
                const int cx = FloorChunk(ix), cz = FloorChunk(iz);
                const auto key = Key(cx, cz);
                auto it = _chunks.find(key);
                if (it == _chunks.end())
                {
                    if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                    it = _chunks.emplace(key, Chunk{}).first;
                }
                Cell& cell = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
                const float edge = std::clamp((1.0f - r) * 4.0f, 0.0f, 1.0f);
                cell.deficit = std::max(Remaining(cell), _depth * std::clamp(strength, 0.0f, 1.0f) * edge);
                cell.precipitation = _precipitation;
            }
    }
    void Trail(float ax, float az, float bx, float bz, float radius)
    {
        if (!enabled || !std::isfinite(radius) || radius <= 0.0f) return;
        const float length = std::hypot(bx - ax, bz - az);
        if (!std::isfinite(length) || length > 25.0f) return; // Teleport, not a driven track.
        const int steps = std::max(1, static_cast<int>(std::ceil(length / std::max(radius, CellSize))));
        for (int i = 0; i <= steps; ++i)
        {
            const float t = float(i) / steps;
            Stamp(ax + (bx - ax) * t, az + (bz - az) * t, radius, 0.8f);
        }
    }
    void PressBody(float x, float z, float directionX, float directionZ)
    {
        const float length = std::hypot(directionX, directionZ);
        if (!std::isfinite(length) || length < 0.001f) return;
        const float dx = directionX / length * 0.55f;
        const float dz = directionZ / length * 0.55f;
        Trail(x-dx, z-dz, x+dx, z+dz, 0.3f);
    }
    float DeficitAt(int ix, int iz) const
    {
        const int cx = FloorChunk(ix), cz = FloorChunk(iz);
        const auto it = _chunks.find(Key(cx, cz));
        return it == _chunks.end() ? 0.0f : Remaining(it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize]);
    }
    std::vector<float> Snapshot(float cameraX, float cameraZ) const
    {
        std::vector<float> data(4 + WindowSize * WindowSize, 0.0f);
        data[2] = CellSize;
        if (!std::isfinite(cameraX) || !std::isfinite(cameraZ) ||
            std::abs(cameraX) > 1000000.0f || std::abs(cameraZ) > 1000000.0f) return data;
        const int ox = static_cast<int>(std::floor(cameraX / (CellSize * ChunkSize))) * ChunkSize - WindowSize / 2;
        const int oz = static_cast<int>(std::floor(cameraZ / (CellSize * ChunkSize))) * ChunkSize - WindowSize / 2;
        data[0] = ox * CellSize;
        data[1] = oz * CellSize;
        data[2] = CellSize;
        data[3] = enabled ? _depth : 0.0f;
        if (!enabled) return data;
        for (int z = 0; z < WindowSize; z += ChunkSize)
            for (int x = 0; x < WindowSize; x += ChunkSize)
            {
                const auto it = _chunks.find(Key(FloorChunk(ox + x), FloorChunk(oz + z)));
                if (it == _chunks.end()) continue;
                for (int dz = 0; dz < ChunkSize; ++dz)
                    for (int dx = 0; dx < ChunkSize; ++dx)
                        data[4 + (z + dz) * WindowSize + x + dx] = Remaining(it->second.cells[dz * ChunkSize + dx]);
            }
        return data;
    }
private:
    struct Cell { double precipitation = 0.0; float deficit = 0.0f; };
    struct Chunk { std::array<Cell, ChunkSize * ChunkSize> cells{}; };
    static int FloorChunk(int x) { return static_cast<int>(std::floor(double(x) / ChunkSize)); }
    static uint64_t Key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
    float Remaining(const Cell& c) const
    {
        return std::max(0.0f, c.deficit - static_cast<float>(_precipitation - c.precipitation));
    }
    std::unordered_map<uint64_t, Chunk> _chunks;
    double _precipitation = 0.0;
    float _depth = 0.0f;
    size_t _rejected = 0;
};

inline SnowField& GSnow() { static SnowField snow; return snow; }
} // namespace Poseidon
