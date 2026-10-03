#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <mutex>
#include <vector>

namespace Poseidon
{
namespace
{
std::array<HydroWaterInteractionEvent, HydroMaxWaterInteractions> pendingEvents;
uint32_t pendingCount = 0;
std::mutex pendingEventsMutex;
std::atomic<float> playerWaterDepth{0.0f};
std::atomic<bool> rifleWaterImpactSprayEnabled{true};
// Diagnostics for the Water tab. Player ripples stopped working and the code that emits them was
// still intact, so the useful question is WHICH end is silent: nothing submitted, or submitted and
// never drained. Two counters answer it without a debugger.
std::atomic<uint32_t> submittedTotal{0};
std::atomic<uint32_t> lastDrained{0};
} // namespace

void SubmitWaterInteraction(const HydroWaterInteractionEvent& event)
{
    std::lock_guard<std::mutex> lock(pendingEventsMutex);
    if (pendingCount == HydroMaxWaterInteractions)
    {
        // Preserve the newest visual evidence when a simulation frame overproduces.
        for (uint32_t i = 1; i < pendingCount; ++i)
        {
            pendingEvents[i - 1] = pendingEvents[i];
        }
        --pendingCount;
    }
    pendingEvents[pendingCount++] = event;
    submittedTotal.fetch_add(1, std::memory_order_relaxed);
}

uint32_t DrainWaterInteractions(HydroWaterInteractionEvent* events, uint32_t capacity)
{
    if (events == nullptr || capacity == 0)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(pendingEventsMutex);
    const uint32_t count = pendingCount < capacity ? pendingCount : capacity;
    for (uint32_t i = 0; i < count; ++i)
    {
        events[i] = pendingEvents[i];
    }
    for (uint32_t i = count; i < pendingCount; ++i)
    {
        pendingEvents[i - count] = pendingEvents[i];
    }
    pendingCount -= count;
    lastDrained.store(count, std::memory_order_relaxed);
    return count;
}

uint32_t TotalWaterInteractionsSubmitted()
{
    return submittedTotal.load(std::memory_order_relaxed);
}

uint32_t LastWaterInteractionsDrained()
{
    return lastDrained.load(std::memory_order_relaxed);
}

void SetPlayerWaterDepth(float depth)
{
    playerWaterDepth.store(depth, std::memory_order_relaxed);
}

float GetPlayerWaterDepth()
{
    return playerWaterDepth.load(std::memory_order_relaxed);
}

void SetRifleWaterImpactSprayEnabled(bool enabled)
{
    rifleWaterImpactSprayEnabled.store(enabled, std::memory_order_relaxed);
}

bool RifleWaterImpactSprayEnabled()
{
    return rifleWaterImpactSprayEnabled.load(std::memory_order_relaxed);
}

namespace
{
// Sinkhole W3: the drawn surface around the camera (SetDrawnWaterGrid)
std::mutex drawnMutex;
std::vector<float> drawnXyz;
int drawnN = 0;
float drawnLanes[4] = {};
} // namespace

void SetDrawnWaterGrid(const float* xyz, int n, const float lanes[4])
{
    std::lock_guard<std::mutex> lock(drawnMutex);
    if (!xyz || n < 2 || !lanes)
    {
        drawnN = 0;
        return;
    }
    drawnXyz.assign(xyz, xyz + size_t(n) * n * 3);
    drawnN = n;
    for (int i = 0; i < 4; i++)
    {
        drawnLanes[i] = lanes[i];
    }
}

bool DrawnWaterHeightAt(float x, float z, float& y)
{
    std::lock_guard<std::mutex> lock(drawnMutex);
    const int n = drawnN;
    const float step = drawnLanes[2];
    if (n < 2 || step <= 0)
    {
        return false;
    }
    // texel (row j, column i) is the drawn vertex of lattice point (base x + i step, base z + j step),
    // displaced sideways as well as up: find the lattice point whose vertex lies over (x, z) (three
    // fixed-point steps, as the probe shader inverts the displacement), then its height
    auto sample = [&](float u, float v, int lane)
    {
        const int i = std::clamp(int(std::floor(u)), 0, n - 2);
        const int j = std::clamp(int(std::floor(v)), 0, n - 2);
        const float fu = std::clamp(u - i, 0.0f, 1.0f);
        const float fv = std::clamp(v - j, 0.0f, 1.0f);
        auto at = [&](int ii, int jj) { return drawnXyz[(size_t(jj) * n + ii) * 3 + lane]; };
        const float a = at(i, j) + (at(i + 1, j) - at(i, j)) * fu;
        const float b = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fu;
        return a + (b - a) * fv;
    };
    float u = (x - drawnLanes[0]) / step;
    float v = (z - drawnLanes[1]) / step;
    // one texel of margin: the inversion needs the neighbours
    if (u < 1.0f || v < 1.0f || u > n - 2.0f || v > n - 2.0f)
    {
        return false;
    }
    for (int k = 0; k < 3; k++)
    {
        u += (x - sample(u, v, 0)) / step;
        v += (z - sample(u, v, 2)) / step;
    }
    if (u < 0.0f || v < 0.0f || u > n - 1.0f || v > n - 1.0f)
    {
        return false;
    }
    y = sample(u, v, 1);
    return std::isfinite(y);
}

} // namespace Poseidon
