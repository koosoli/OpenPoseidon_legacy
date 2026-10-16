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
// Centre of the displayed 64 m mud window: pushed MudViewLookAhead metres ahead
// of the camera along its horizontal view direction, so ruts stay visible to
// ~46 m in front (where the player looks) instead of 32 m all round. A camera
// looking straight down keeps the window centred on itself.
constexpr float MudViewLookAhead = 14.0f;
struct MudViewCentre { float x, z; };
inline MudViewCentre MudWindowCentre(float cameraX, float cameraZ, float dirX, float dirZ)
{
    if (!std::isfinite(cameraX) || !std::isfinite(cameraZ)) return {cameraX, cameraZ};
    const float len = std::hypot(dirX, dirZ);
    if (!std::isfinite(len) || len < 0.2f) return {cameraX, cameraZ};
    const float reach = MudViewLookAhead * std::min(1.0f, (len - 0.2f) / 0.3f);
    return {cameraX + dirX / len * reach, cameraZ + dirZ / len * reach};
}

// Mission-local soft-ground geometry, independent of snow and of camera residency.
// Stored values are positive depression metres; all public height offsets are
// negative. Drying affects new contacts, never erases an existing imprint.
// How full of water the ruts are, 0..1, from the soil wetness: none on damp
// ground, the deepest parts of ruts start holding water once the soil is
// soaked (~0.7), well filled on a soaked overcast day, brimming in a downpour. Drawn by the terrain
// shader (standing water in rut floors); the physics does not use it.
inline float MudRutWaterLevel(float wetness)
{
    if (!std::isfinite(wetness)) return 0.0f;
    const float t = std::clamp((wetness - 0.7f) / 0.4f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class MudField
{
public:
    static constexpr int WindowSize = 512;
    static constexpr int ChunkSize = 16;
    static constexpr float CellSize = 0.125f;
    static constexpr float DefaultLayerDepth = 0.24f;
    static constexpr float MaxDepth = 0.35f;
    // ~40 degrees: deep tyre ruts have steep walls (was 0.45 / ~24 degrees).
    static constexpr float MaxCellSlope = 0.85f;
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
    // Ground dampness that heavy cloud implies before any rain is simulated: the
    // mission's weather has been like this for a while, so the soil is already
    // soaked through. Nothing under half overcast; ~0.6 (deep field ruts) at 0.8;
    // ~0.95 at full overcast. Weather rain starts at zero and only ramps up over
    // minutes, which is why ruts used to stay shallow early in a rainy mission.
    static float WeatherDampness(float overcast)
    {
        if (!std::isfinite(overcast)) return 0.0f;
        const float t = std::clamp((overcast - 0.5f) / 0.5f, 0.0f, 1.0f);
        return 0.95f * t * t * (3.0f - 2.0f * t);
    }
    // Mission start: soil as wet as the starting weather implies (never drier
    // than it already is).
    void SeedWeather(float overcast)
    {
        _wetness = std::max(_wetness, WeatherDampness(overcast));
    }
    // overcast: current cloud cover. Under heavy cloud the soil does not dry
    // below WeatherDampness and slowly soaks up to it (10 minute time scale).
    void Advance(float seconds, float liquidRain, float overcast = 0.0f)
    {
        if (!enabled || !std::isfinite(seconds) || seconds <= 0.0f) return;
        // Test switch: POSEIDON_MUD_WETNESS=0..1 holds the soil at that wetness
        // (no waiting for rain to soak it, and mud testing without rain).
        static const float forced = [] {
            float result = -1.0f;
#ifdef _WIN32
            char* text = nullptr;
            size_t length = 0;
            const bool available = _dupenv_s(&text, &length, "POSEIDON_MUD_WETNESS") == 0;
#else
            const char* text = std::getenv("POSEIDON_MUD_WETNESS");
            const bool available = true;
#endif
            if (available && text && *text)
            {
                const float w = float(std::atof(text));
                if (std::isfinite(w)) result = std::clamp(w, 0.0f, 1.0f);
            }
#ifdef _WIN32
            std::free(text);
#endif
            return result;
        }();
        if (forced >= 0.0f) { _wetness = forced; return; }
        liquidRain = std::isfinite(liquidRain) ? std::clamp(liquidRain, 0.0f, 1.0f) : 0.0f;
        const double fill = double(liquidRain) / 30.0;
        const double dry = (1.0 - double(liquidRain)) / 180.0;
        const double rate = fill + dry;
        const float before = _wetness;
        _wetness = float(std::clamp(fill / rate + (double(_wetness) - fill / rate) *
            std::exp(-rate * seconds), 0.0, 1.0));
        const float damp = WeatherDampness(overcast);
        // Drying stops at the cloud's dampness; below it the soil soaks up slowly.
        _wetness = std::max(_wetness, std::min(before, damp));
        if (_wetness < damp)
            _wetness = float(std::clamp(double(damp) + (double(_wetness) - damp) * std::exp(-double(seconds) / 600.0), 0.0, 1.0));
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

    // Heap displaced soil (a rut's side ridge) up to `height` metres at the centre
    // of a smooth disc. Stored as a negative depression, so HeightOffsetAt and the
    // snapshot report it as a positive offset. Never heaps inside an existing rut,
    // never lowers anything, and a later wheel over a ridge presses it back down.
    static constexpr float MaxRim = 0.10f;
    bool Raise(float x, float z, float radius, float height)
    {
        if (!enabled || !FinitePosition(x, z) || !std::isfinite(radius) || radius <= 0.0f ||
            !std::isfinite(height) || height <= 0.0f) return false;
        radius = std::clamp(radius, CellSize, MaxContactRadius);
        height = std::min(height, MaxRim);
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
                const float value = height * (1.0f - r * r * (3.0f - 2.0f * r));
                if (value <= 0.000001f) continue;
                const int cx = FloorChunk(ix), cz = FloorChunk(iz);
                auto it = _chunks.find(Key(cx, cz));
                if (it == _chunks.end())
                {
                    if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                    it = _chunks.emplace(Key(cx, cz), Chunk{}).first;
                }
                float& stored = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
                if (stored > 0.004f) continue;          // inside a rut: soil does not pile there
                if (-value < stored - 0.000001f) { stored = -value; changed = true; }
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

    // Coarse far view for distant ruts: same packing as Snapshot but FarCellSize
    // (0.5 m) cells, so the 512x512 window covers 256 m. Each far cell holds the
    // deepest rut among its 4x4 fine cells (ridges are left out: at that range
    // the rut lines are what reads). Centred like Snapshot, chunk aligned.
    static constexpr float FarCellSize = 0.5f;
    static constexpr int FarRatio = 4;            // fine cells per far cell, each way
    std::vector<float> SnapshotFar(float centerX, float centerZ) const
    {
        std::vector<float> data(4 + WindowSize * WindowSize, 0.0f);
        data[2] = FarCellSize;
        if (!enabled || !FinitePosition(centerX, centerZ)) return data;
        const int span = WindowSize * FarRatio;   // fine cells across the far window
        const int ox = FloorChunk(int(std::floor(centerX / CellSize))) * ChunkSize - span / 2;
        const int oz = FloorChunk(int(std::floor(centerZ / CellSize))) * ChunkSize - span / 2;
        data[0] = ox * CellSize;
        data[1] = oz * CellSize;
        data[3] = MaxDepth;
        for (const auto& [key, chunk] : _chunks)
        {
            const int kx = int(int32_t(uint32_t(key >> 32))), kz = int(int32_t(uint32_t(key & 0xffffffffu)));
            const int fx0 = kx * ChunkSize - ox, fz0 = kz * ChunkSize - oz;   // fine offset in window
            if (fx0 < 0 || fz0 < 0 || fx0 >= span || fz0 >= span) continue;
            for (int cz = 0; cz < ChunkSize / FarRatio; ++cz)
                for (int cx = 0; cx < ChunkSize / FarRatio; ++cx)
                {
                    float deepest = 0.0f;
                    for (int dz = 0; dz < FarRatio; ++dz)
                        for (int dx = 0; dx < FarRatio; ++dx)
                            deepest = std::max(deepest, chunk.cells[(cz * FarRatio + dz) * ChunkSize + cx * FarRatio + dx]);
                    const int i = fx0 / FarRatio + cx, j = fz0 / FarRatio + cz;
                    data[4 + j * WindowSize + i] = -std::min(deepest, MaxDepth);
                }
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
