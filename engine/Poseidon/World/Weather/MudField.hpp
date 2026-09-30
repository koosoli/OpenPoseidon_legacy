#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    static constexpr float DefaultLayerDepth = 0.24f;
    static constexpr float MaxDepth = 0.35f;
    static constexpr float MaxCellSlope = 0.45f;
    static constexpr float MaxContactRadius = 4.0f * MaxDepth;
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
    // Thickness is capacity, not an automatic lowering of untouched terrain.
    // Changing capacity never clips previously stored depressions.
    bool SetLayerDepth(float metres)
    {
        if (!std::isfinite(metres) || metres < 0.05f || metres > MaxDepth) return false;
        _layerDepth = metres;
        return true;
    }
    float LayerDepth() const { return _layerDepth; }
    static float ContactRadius(float depth) { return std::clamp(4.0f * depth, 0.35f, MaxContactRadius); }
    // Rain gradually softens a decimetre-scale layer. Admission is caller-owned.
    float ContactDepth() const
    {
        const float t = std::clamp((_wetness - 0.15f) / 0.85f, 0.0f, 1.0f);
        return enabled ? _layerDepth * t * t * (3.0f - 2.0f * t) : 0.0f;
    }
    size_t Chunks() const { return _chunks.size(); }
    size_t Rejected() const { return _rejected; }
    uint64_t Revision() const { return _revision; }

    // One real loading edge compresses only a fraction of remaining capacity.
    bool CompressBoot(float x, float z, float pressure = 0.65f)
    {
        if (!ContactActive()) return false;
        const float depth = ContactDepth();
        return Stamp(x, z, ContactRadius(depth), depth, pressure);
    }

    // Smooth finite support disc; neighbour limits bound the actual bilinear
    // physical slope, including overlaps and repeated differently placed boots.
    bool Stamp(float x, float z, float radius, float depth, float pressure = 1.0f)
    {
        if (!enabled || !FinitePosition(x, z) || !std::isfinite(radius) || radius <= 0.0f ||
            !std::isfinite(depth) || depth <= 0.0f || !std::isfinite(pressure) || pressure <= 0.0f) return false;
        pressure = std::min(pressure, 1.0f);
        radius = std::clamp(radius, CellSize, MaxContactRadius);
        depth = std::min(depth, MaxDepth);
        const int x0 = int(std::floor((x - radius) / CellSize));
        const int x1 = int(std::floor((x + radius) / CellSize));
        const int z0 = int(std::floor((z - radius) / CellSize));
        const int z1 = int(std::floor((z + radius) / CellSize));
        struct Target { int x, z; float depth; };
        // Maximum diameter is 2.8m, at most 24x24 cell centres. Fixed scratch
        // avoids a per-contact heap allocation and repeated pressure application.
        std::array<Target, 26 * 26> targets{};
        size_t targetCount = 0;
        bool changed = false;
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float r = std::hypot((ix + 0.5f) * CellSize - x, (iz + 0.5f) * CellSize - z) / radius;
                if (r >= 1.0f) continue;
                const float value = depth * (1.0f - r * r * (3.0f - 2.0f * r));
                if (value <= 0.000001f) continue;
                const int cx = FloorChunk(ix), cz = FloorChunk(iz);
                auto it = _chunks.find(Key(cx, cz));
                if (it == _chunks.end())
                {
                    if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                    it = _chunks.emplace(Key(cx, cz), Chunk{}).first;
                }
                float& stored = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
                targets[targetCount++] = {ix, iz, stored + pressure * std::max(0.0f, value - stored)};
            }
        // Alternating sweeps grow a smooth shoulder together with its centre;
        // each target was fixed from the pre-contact state above.
        for (int pass = 0; pass < 4; ++pass)
            for (size_t j = 0; j < targetCount; ++j)
            {
                const auto& target = targets[(pass & 1) ? targetCount - 1 - j : j];
                const int ix = target.x, iz = target.z, cx = FloorChunk(ix), cz = FloorChunk(iz);
                auto it = _chunks.find(Key(cx, cz));
                float& stored = it->second.cells[(iz-cz*ChunkSize)*ChunkSize+ix-cx*ChunkSize];
                const float neighbourLimit = std::min({CellDepth(ix-1, iz), CellDepth(ix+1, iz),
                    CellDepth(ix, iz-1), CellDepth(ix, iz+1)}) + MaxCellSlope * CellSize;
                const float next = std::min(target.depth, neighbourLimit);
                if (next > stored + 0.000001f) { stored = next; changed = true; }
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
    float _layerDepth = DefaultLayerDepth;
    size_t _rejected = 0;
    uint64_t _revision = 0;
};

inline MudField& GMud()
{
    static MudField mud = [] {
        MudField field;
#ifdef _WIN32
        char* text = nullptr;
        size_t length = 0;
        const bool available = _dupenv_s(&text, &length, "POSEIDON_MUD_LAYER_DEPTH") == 0;
#else
        const char* text = std::getenv("POSEIDON_MUD_LAYER_DEPTH");
        const bool available = true;
#endif
        if (available && text)
        {
            char* end = nullptr;
            const float depth = std::strtof(text, &end);
            if (end != text && end && *end == '\0') field.SetLayerDepth(depth);
        }
#ifdef _WIN32
        std::free(text);
#endif
        return field;
    }();
    return mud;
}
} // namespace Poseidon
