#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <Poseidon/World/Terrain/Landscape.hpp>

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
    // Alpine snowline: permanent terrain cover above snowlineHeight, ramping
    // from first flakes to full cover over snowlineRange metres. This changes
    // WHERE snow lies, never the snowfall itself. Off when disabled.
    bool snowlineEnabled = true;
    float snowlineHeight = 644.88f;
    float snowlineRange = 25.0f;
    // Depth of the alpine cover itself (m). Full white coverage is reached at
    // ~4 cm; more depth mainly lifts the surface and brightens the powder.
    float snowlineDepth = 0.06f;

    void ApplySnowstormPreset()
    {
        enabled = falling = true;
        flakeMultiplier = 8.0f;
        metresPerMinute = 0.1f;
    }

    void ResetSettings()
    {
        // A settings reset must not erase mission-local surface edits.
        const SnowField defaults;
        enabled = defaults.enabled;
        falling = defaults.falling;
        detailedGeometry = defaults.detailedGeometry;
        metresPerMinute = defaults.metresPerMinute;
        maxDepth = defaults.maxDepth;
        flakeMultiplier = defaults.flakeMultiplier;
        snowlineEnabled = defaults.snowlineEnabled;
        snowlineHeight = defaults.snowlineHeight;
        snowlineRange = defaults.snowlineRange;
        snowlineDepth = defaults.snowlineDepth;
    }

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
    float RenderFog(float authored) const
    {
        const float intensity = FallingIntensity();
        // Snow contributes 0.15..0.60. Explicitly denser mission/dev fog wins;
        // stopping snowfall restores its authored value without overwriting it.
        return intensity > 0.0f ? std::max(authored, std::min(0.6f, 0.15f + 0.45f * intensity)) : authored;
    }

    void Reset()
    {
        _chunks.clear();
        _chunkKeys.clear();
        _reclaimCursor = 0;
        _depth = 0.0f;
        _precipitation = 0.0;
        _rejected = 0;
    }
    void Advance(float seconds)
    {
        if (enabled && falling && std::isfinite(seconds) && seconds > 0.0f)
            Deposit(std::min(seconds, 1.0f) * std::clamp(metresPerMinute, 0.0f, 1.0f) / 60.0f);
        if (enabled && std::isfinite(seconds) && seconds > 0.0f)
            ReclaimFilledChunks(128);
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

    // Headless A/B override POSEIDON_SNOWLINE="off" | "<height m>". Read once;
    // the panel keeps showing the stored values while the override wins.
    struct SnowlineOverride { bool active = false; bool off = false; float height = 0.0f; };
    static SnowlineOverride ResolveSnowlineOverride()
    {
        static const SnowlineOverride o = [] {
            const auto parse = [](const char* v) {
                SnowlineOverride r;
                if (v == nullptr || v[0] == '\0') return r;
                r.active = true;
                if (v[0] == 'o' || v[0] == 'O') { r.off = true; return r; }
                const float h = static_cast<float>(std::atof(v));
                if (std::isfinite(h)) r.height = h; else r.active = false;
                return r;
            };
#if defined(_WIN32)
            char* value = nullptr; size_t size = 0;
            _dupenv_s(&value, &size, "POSEIDON_SNOWLINE");
            const auto result = parse(value); std::free(value); return result;
#else
            return parse(std::getenv("POSEIDON_SNOWLINE"));
#endif
        }();
        return o;
    }
    bool EffSnowlineEnabled() const
    {
        const SnowlineOverride o = ResolveSnowlineOverride();
        return o.active ? !o.off : snowlineEnabled;
    }
    float EffSnowlineHeight() const
    {
        const SnowlineOverride o = ResolveSnowlineOverride();
        return o.active && !o.off ? o.height : snowlineHeight;
    }
    // Altitude snowline as volumetric base depth at (x, z): the permanent cover
    // above snowlineHeight ramping over snowlineRange (smoothstep, mirroring the
    // terrain shader). 0 below the line or when disabled. This is what makes the
    // snowline real volumetric snow rather than white paint: stamping below
    // carves into it exactly like snowfall deposit.
    float AltitudeDepthAt(float x, float z) const
    {
        if (!EffSnowlineEnabled() || snowlineRange <= 0.0f || snowlineDepth <= 0.0f) return 0.0f;
        if (GLandscape == nullptr) return 0.0f;
        const float h = GLandscape->SurfaceY(x, z);
        if (!std::isfinite(h)) return 0.0f;
        const float t = (h - EffSnowlineHeight()) / snowlineRange;
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return snowlineDepth;
        return snowlineDepth * (t * t * (3.0f - 2.0f * t));
    }
    // Effective snow base depth: snowfall deposit (only while enabled) or
    // altitude cover, whichever is deeper. Tracks and presses scale against this.
    float BaseDepthAt(float x, float z) const { return std::max(enabled ? _depth : 0.0f, AltitudeDepthAt(x, z)); }

    void Stamp(float x, float z, float radius, float strength = 0.9f)
    {
        ModifyDisc(x, z, radius, strength, false);
    }
    // Actual projectile terrain impacts remove a bounded entry groove in
    // snow, ending at the terrain hit. Vertical shots retain the round bowl.
    // The intact outer powder forms its rim; deficits remain nonnegative,
    // preserving the existing geometry/snapshot/precipitation contract.
    bool BulletImpact(float x, float z, float radius, float depth,
        float directionX = 0, float directionZ = 0, float length = 0)
    {
        if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(depth) || depth <= 0 ||
            !std::isfinite(directionX) || !std::isfinite(directionZ) || !std::isfinite(length) ||
            length < 0 || length > 0.8f) return false;
        const float directionLength = std::hypot(directionX, directionZ);
        if (length > 0 && (!std::isfinite(directionLength) || std::abs(directionLength - 1.0f) > 0.001f)) return false;
        return ModifyDisc(x, z, std::clamp(radius, CellSize, 0.35f), std::min(depth, 0.16f),
            true, true, directionX, directionZ, length);
    }
    // Rate-integrated removal, unlike a footprint's absolute compression depth.
    void ErodeRotor(float x, float z, float radius, float rotorSpeed, float height, float seconds)
    {
        if (!std::isfinite(rotorSpeed) || !std::isfinite(height) ||
            !std::isfinite(seconds) || seconds <= 0 || height < 0 || height >= 30) return;
        const float speed = std::clamp(rotorSpeed, 0.0f, 1.0f);
        const float groundEffect = 1.0f - height / 30.0f;
        const float metres = 0.08f * speed * speed * groundEffect * groundEffect * std::min(seconds, 0.25f);
        ModifyDisc(x, z, std::min(radius, 8.0f), metres, true);
    }

private:
    bool ModifyDisc(float x, float z, float radius, float strength, bool additive, bool bowl = false,
        float directionX = 0, float directionZ = 0, float length = 0)
    {
        if (!CoverActive() || !std::isfinite(x) || !std::isfinite(z) ||
            std::abs(x) > 1000000.0f || std::abs(z) > 1000000.0f ||
            !std::isfinite(radius) || radius <= 0.0f || !std::isfinite(strength) || strength <= 0.0f) return false;
        // Tracks carve into whatever snow is actually there: snowfall deposit
        // or altitude snowline cover. Below both there is nothing to press.
        const float base = BaseDepthAt(x, z);
        if (base <= 0.0f) return false;
        // A manual deposit can fill the store between simulation ticks.
        if (_chunks.size() >= MaxChunks) ReclaimFilledChunks(128);
        radius = std::clamp(radius, CellSize, 15.0f);
        const float halfLength = length * 0.5f;
        const float centreX = x - directionX * halfLength, centreZ = z - directionZ * halfLength;
        const float extentX = radius + std::abs(directionX) * halfLength;
        const float extentZ = radius + std::abs(directionZ) * halfLength;
        const int x0 = static_cast<int>(std::floor((centreX - extentX) / CellSize));
        const int z0 = static_cast<int>(std::floor((centreZ - extentZ) / CellSize));
        const int x1 = static_cast<int>(std::floor((centreX + extentX) / CellSize));
        const int z1 = static_cast<int>(std::floor((centreZ + extentZ) / CellSize));
        bool changed = false;
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float dx = (ix + 0.5f) * CellSize - centreX;
                const float dz = (iz + 0.5f) * CellSize - centreZ;
                float r = std::sqrt(dx * dx + dz * dz) / radius;
                if (length > 0)
                {
                    const float along = dx * directionX + dz * directionZ;
                    const float outside = along - std::clamp(along, -halfLength, halfLength);
                    const float across = -dx * directionZ + dz * directionX;
                    r = std::hypot(outside, across) / radius;
                }
                if (r >= 1.0f) continue;
                const int cx = FloorChunk(ix), cz = FloorChunk(iz);
                const auto key = Key(cx, cz);
                auto it = _chunks.find(key);
                if (it == _chunks.end())
                {
                    if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                    // Keep a stable-key sweep rather than an iterator invalidated by rehash.
                    _chunkKeys.push_back(key);
                    try
                    {
                        it = _chunks.emplace(key, Chunk{}).first;
                    }
                    catch (...)
                    {
                        _chunkKeys.pop_back();
                        throw;
                    }
                }
                Cell& cell = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
                const float edge = bowl ? (1.0f-r*r)*(1.0f-r*r) : std::clamp((1.0f - r) * 4.0f, 0.0f, 1.0f);
                const float remaining = Remaining(cell);
                cell.deficit = additive ? std::min(base, remaining + strength * edge)
                    : std::max(remaining, base * std::clamp(strength, 0.0f, 1.0f) * edge);
                changed |= cell.deficit > remaining;
                cell.precipitation = _precipitation;
                it->second.filledAt = std::max(it->second.filledAt, _precipitation + cell.deficit);
            }
        return changed;
    }
public:
    void Trail(float ax, float az, float bx, float bz, float radius, float strength = 0.8f)
    {
        if (!CoverActive() || !std::isfinite(radius) || radius <= 0.0f) return;
        const float length = std::hypot(bx - ax, bz - az);
        if (!std::isfinite(length) || length > 25.0f) return; // Teleport, not a driven track.
        const int steps = std::max(1, static_cast<int>(std::ceil(length / std::max(radius, CellSize))));
        for (int i = 0; i <= steps; ++i)
        {
            const float t = float(i) / steps;
            Stamp(ax + (bx - ax) * t, az + (bz - az) * t, radius, strength);
        }
    }
    // The snow system is live for cover, tracks and gameplay when snowfall is
    // enabled OR the alpine snowline is on. Falling flakes stay on `falling`
    // alone: the snowline changes where snow lies, never the snowfall itself.
    bool CoverActive() const { return enabled || snowlineEnabled; }
    bool NeedsWalkingChannel() const { return enabled && Depth() > 0.25f; }
    // Deposit-only global; kept for external callers. In-engine call sites with
    // a position use NeedsWalkingChannelAt so altitude cover wades too.
    // Position-aware variant: wading depth is deposit or altitude cover at the
    // walker's feet. Prefer this at call sites with a position in hand.
    bool NeedsWalkingChannelAt(float x, float z) const { return CoverActive() && BaseDepthAt(x, z) > 0.25f; }
    void PressVehicle(float ax, float az, float bx, float bz, float radius, float clearance)
    {
        if (!std::isfinite(clearance) || clearance < 0.0f) return;
        // Clearance is measured against the snow that is actually there: deposit
        // or altitude cover at the lane midpoint.
        const float base = BaseDepthAt((ax + bx) * 0.5f, (az + bz) * 0.5f);
        if (base <= clearance) return;
        // Leave snow below the underbody rather than scraping the whole lane to soil.
        Trail(ax, az, bx, bz, radius, (base - clearance) / base);
    }
    void PressWalking(float ax, float az, float bx, float bz)
    {
        if (!CoverActive()) return;
        const float base = BaseDepthAt((ax + bx) * 0.5f, (az + bz) * 0.5f);
        if (base <= 0.25f) return;
        const float depthFactor = std::clamp((base - 0.25f) / 0.4f, 0.0f, 1.0f);
        const float radius = 0.22f + 0.13f * depthFactor;
        Trail(ax, az, bx, bz, radius, 1.0f);
        // A teleport clears the destination footprint, never the intervening map.
        Stamp(bx, bz, radius, 1.0f);
    }
    void PressBody(float x, float z, float directionX, float directionZ)
    {
        const float length = std::hypot(directionX, directionZ);
        if (!std::isfinite(length) || length < 0.001f) return;
        const float dx = directionX / length * 0.55f;
        const float dz = directionZ / length * 0.55f;
        Trail(x-dx, z-dz, x+dx, z+dz, 0.3f, 1.0f);
    }
    void PressHead(float bodyX, float bodyZ, float headX, float headZ)
    {
        // The animated head can extend beyond the old root-centred capsule.
        // Keep the neck/head corridor clear too, including a near-plane margin.
        const float distance = std::hypot(headX - bodyX, headZ - bodyZ);
        if (!std::isfinite(distance) || distance > 2.5f) return;
        Trail(bodyX, bodyZ, headX, headZ, 0.4f, 1.0f);
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
        if (!CoverActive()) return data;
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
    struct Chunk
    {
        std::array<Cell, ChunkSize * ChunkSize> cells{};
        double filledAt = 0.0;
    };
    void ReclaimFilledChunks(size_t budget)
    {
        budget = std::min(budget, _chunkKeys.size());
        while (budget-- > 0 && !_chunkKeys.empty())
        {
            _reclaimCursor %= _chunkKeys.size();
            const auto it = _chunks.find(_chunkKeys[_reclaimCursor]);
            // This deadline is the latest fill point of every cell in the chunk.
            // Live depressions are never evicted, even far from all cameras.
            if (it != _chunks.end() && it->second.filledAt <= _precipitation)
            {
                _chunks.erase(it);
                _chunkKeys[_reclaimCursor] = _chunkKeys.back();
                _chunkKeys.pop_back();
            }
            else
                ++_reclaimCursor;
        }
    }
    static int FloorChunk(int x) { return static_cast<int>(std::floor(double(x) / ChunkSize)); }
    static uint64_t Key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }
    float Remaining(const Cell& c) const
    {
        return std::max(0.0f, c.deficit - static_cast<float>(_precipitation - c.precipitation));
    }
    std::unordered_map<uint64_t, Chunk> _chunks;
    std::vector<uint64_t> _chunkKeys;
    size_t _reclaimCursor = 0;
    double _precipitation = 0.0;
    float _depth = 0.0f;
    size_t _rejected = 0;
};

inline SnowField& GSnow() { static SnowField snow; return snow; }
} // namespace Poseidon
