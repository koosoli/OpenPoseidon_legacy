#include <Poseidon/World/Weather/SandField.hpp>
#include <algorithm>
#include <cmath>

namespace Poseidon
{
SandField& GSand() { static SandField sand; return sand; }
bool SandField::FinitePosition(float x, float z)
{
    return std::isfinite(x) && std::isfinite(z) && std::abs(x) <= 1000000.0f && std::abs(z) <= 1000000.0f;
}
int SandField::FloorChunk(int x) { return int(std::floor(double(x) / ChunkSize)); }
uint64_t SandField::Key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }

void SandField::Reset()
{
    _chunks.clear();
    _wetness = 0.0f;
    _rejected = 0;
    ++_revision;
}
void SandField::Advance(float seconds, float liquidRain)
{
    if (!enabled || !std::isfinite(seconds) || seconds <= 0.0f) return;
    liquidRain = std::isfinite(liquidRain) ? std::clamp(liquidRain, 0.0f, 1.0f) : 0.0f;
    const double fill = double(liquidRain) / 30.0;
    const double dry = (1.0 - double(liquidRain)) / 180.0;
    const double rate = fill + dry;
    _wetness = float(std::clamp(fill / rate + (double(_wetness) - fill / rate) *
        std::exp(-rate * seconds), 0.0, 1.0));
}
float SandField::ContactDepth() const
{
    const float t = _wetness * _wetness * (3.0f - 2.0f * _wetness);
    return enabled ? std::lerp(0.10f, 0.055f, t) : 0.0f;
}
float SandField::ContactRim() const
{
    const float t = _wetness * _wetness * (3.0f - 2.0f * _wetness);
    return enabled ? std::lerp(0.014f, 0.008f, t) : 0.0f;
}

bool SandField::StampBoot(float x, float z, float directionX, float directionZ, float depth, float rim)
{
    if (!enabled || !FinitePosition(x, z) || !std::isfinite(directionX) || !std::isfinite(directionZ) ||
        !std::isfinite(depth) || depth <= 0 || !std::isfinite(rim) || rim < 0) return false;
    const float directionLength = std::hypot(directionX, directionZ);
    if (!std::isfinite(directionLength) || directionLength < 0.001f) return false;
    directionX /= directionLength;
    directionZ /= directionLength;
    depth = std::min(depth, MaxDepth);
    rim = std::min(rim, MaxRim);
    constexpr float halfLength = 0.075f, radius = 0.20f, outerRadius = 0.34f;
    constexpr float guard = halfLength + outerRadius;
    const int x0 = int(std::floor((x - guard) / CellSize)), x1 = int(std::floor((x + guard) / CellSize));
    const int z0 = int(std::floor((z - guard) / CellSize)), z1 = int(std::floor((z + guard) / CellSize));
    bool changed = false;
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix)
        {
            const float px = (ix + 0.5f) * CellSize - x, pz = (iz + 0.5f) * CellSize - z;
            const float along = std::clamp(px * directionX + pz * directionZ, -halfLength, halfLength);
            const float distance = std::hypot(px - along * directionX, pz - along * directionZ);
            if (distance >= outerRadius) continue;
            float value;
            if (distance < radius)
            {
                const float r = distance / radius;
                value = -depth * (1.0f - r * r * (3.0f - 2.0f * r));
            }
            else
            {
                const float t = (distance - radius) / (outerRadius - radius);
                value = rim * 16.0f * t * t * (1.0f - t) * (1.0f - t);
            }
            if (std::abs(value) < 0.000001f) continue;
            const int cx = FloorChunk(ix), cz = FloorChunk(iz);
            auto it = _chunks.find(Key(cx, cz));
            if (it == _chunks.end())
            {
                if (_chunks.size() >= MaxChunks) { ++_rejected; continue; }
                it = _chunks.emplace(Key(cx, cz), Chunk{}).first;
            }
            float& stored = it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
            // Depression wins over any rim, independent of contact order. An
            // existing depression never turns into a raised hump on drying.
            const float next = value < 0 ? std::min(stored, value) : (stored < 0 ? stored : std::max(stored, value));
            if (next != stored) { stored = next; changed = true; }
        }
    if (changed) ++_revision;
    return changed;
}

float SandField::CellOffset(int ix, int iz) const
{
    const int cx = FloorChunk(ix), cz = FloorChunk(iz);
    const auto it = _chunks.find(Key(cx, cz));
    return it == _chunks.end() ? 0.0f : it->second.cells[(iz - cz * ChunkSize) * ChunkSize + ix - cx * ChunkSize];
}
float SandField::HeightOffsetAt(float x, float z) const
{
    if (!enabled || _chunks.empty() || !FinitePosition(x, z)) return 0.0f;
    const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
    const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
    const float fx = tx - ix, fz = tz - iz;
    return std::lerp(std::lerp(CellOffset(ix, iz), CellOffset(ix + 1, iz), fx),
                     std::lerp(CellOffset(ix, iz + 1), CellOffset(ix + 1, iz + 1), fx), fz);
}
std::array<float, 2> SandField::HeightGradientAt(float x, float z) const
{
    if (!enabled || _chunks.empty() || !FinitePosition(x, z)) return {};
    const float tx = x / CellSize - 0.5f, tz = z / CellSize - 0.5f;
    const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
    const float fx = tx - ix, fz = tz - iz;
    const float a = CellOffset(ix, iz), b = CellOffset(ix + 1, iz);
    const float c = CellOffset(ix, iz + 1), d = CellOffset(ix + 1, iz + 1);
    return {std::lerp(b - a, d - c, fz) / CellSize, std::lerp(c - a, d - b, fx) / CellSize};
}
std::vector<float> SandField::Snapshot(float cameraX, float cameraZ) const
{
    std::vector<float> data(4 + WindowSize * WindowSize, 0.0f);
    data[2] = CellSize;
    if (!enabled || !FinitePosition(cameraX, cameraZ)) return data;
    const int ox = FloorChunk(int(std::floor(cameraX / CellSize))) * ChunkSize - WindowSize / 2;
    const int oz = FloorChunk(int(std::floor(cameraZ / CellSize))) * ChunkSize - WindowSize / 2;
    data[0] = ox * CellSize; data[1] = oz * CellSize; data[3] = MaxDepth;
    for (int z = 0; z < WindowSize; z += ChunkSize)
        for (int x = 0; x < WindowSize; x += ChunkSize)
        {
            const auto it = _chunks.find(Key(FloorChunk(ox + x), FloorChunk(oz + z)));
            if (it == _chunks.end()) continue;
            for (int dz = 0; dz < ChunkSize; ++dz)
                for (int dx = 0; dx < ChunkSize; ++dx)
                    data[4 + (z + dz) * WindowSize + x + dx] = it->second.cells[dz * ChunkSize + dx];
        }
    return data;
}
} // namespace Poseidon
