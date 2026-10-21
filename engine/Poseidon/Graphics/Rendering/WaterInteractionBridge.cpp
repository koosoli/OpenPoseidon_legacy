#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <map>
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

bool DrawnWaterHeightAt(float x, float z, float& y, float* edgeWeight)
{
    std::lock_guard<std::mutex> lock(drawnMutex);
    const int n = drawnN;
    const float step = drawnLanes[2];
    if (n < 2 || step <= 0)
    {
        return false;
    }
    // texel (row j, column i) is the drawn vertex of lattice point (base x + i step, base z + j step),
    // displaced sideways as well as up: find the lattice point whose vertex lies over (x, z) (Newton
    // steps, as the underwater composite inverts the displacement), then its height
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
    // Newton steps (as wgpu_renderer's probe.rs surface_height): the plain step above contracts by
    // only 1 - kQ per step, and on a choppy crest three of them left the point metres off
    for (int k = 0; k < 6; k++)
    {
        const float e = 0.05f;
        const float rx = (sample(u, v, 0) - x) / step;
        const float rz = (sample(u, v, 2) - z) / step;
        const float xu = (sample(u + e, v, 0) - sample(u - e, v, 0)) / (2 * e * step);
        const float xv = (sample(u, v + e, 0) - sample(u, v - e, 0)) / (2 * e * step);
        const float zu = (sample(u + e, v, 2) - sample(u - e, v, 2)) / (2 * e * step);
        const float zv = (sample(u, v + e, 2) - sample(u, v - e, 2)) / (2 * e * step);
        const float det = xu * zv - xv * zu;
        float du = rx, dv = rz; // folded: the plain step
        if (det > 0.05f)
        {
            du = (zv * rx - xv * rz) / det;
            dv = (xu * rz - zu * rx) / det;
        }
        const float len = std::sqrt(du * du + dv * dv);
        const float maxStep = 1.0f / step; // at most a metre per step
        if (len > maxStep)
        {
            du *= maxStep / len;
            dv *= maxStep / len;
        }
        u -= du;
        v -= dv;
    }
    if (u < 0.0f || v < 0.0f || u > n - 1.0f || v > n - 1.0f)
    {
        return false;
    }
    y = sample(u, v, 1);
    if (edgeWeight)
    {
        // texels from the usable edge (the inversion needs one of margin), over 2 m of fade
        const float edge = std::min(std::min(u - 1.0f, v - 1.0f), std::min(n - 2.0f - u, n - 2.0f - v));
        *edgeWeight = std::clamp(edge * step / 2.0f, 0.0f, 1.0f);
    }
    return std::isfinite(y);
}


namespace
{
std::mutex queryMutex;
std::map<uint64_t, std::vector<float>> queryRequests;
std::vector<float> queryXz, queryValues; // results: x, z pairs; height, dh/dx, dh/dz, valid
uint32_t queryVersion = 0;
double querySeconds = 0;
} // namespace

void RequestWaterQueries(uint64_t key, const float* xz, int n)
{
    std::lock_guard<std::mutex> lock(queryMutex);
    if (!xz || n <= 0)
    {
        queryRequests.erase(key);
        return;
    }
    queryRequests[key].assign(xz, xz + size_t(n) * 2);
}

int TakeWaterQueryRequests(float* xz, int cap)
{
    std::lock_guard<std::mutex> lock(queryMutex);
    int n = 0;
    for (const auto& [key, pts] : queryRequests)
    {
        const int m = int(pts.size() / 2);
        if (n + m > cap)
        {
            continue; // the whole hull or nothing: a boat with half its samples would be lopsided
        }
        std::copy(pts.begin(), pts.end(), xz + size_t(n) * 2);
        n += m;
    }
    queryRequests.clear();
    return n;
}

void SetWaterQueryResults(const float* xz, const float* values, int n)
{
    std::lock_guard<std::mutex> lock(queryMutex);
    if (!xz || !values || n <= 0)
    {
        queryXz.clear();
        queryValues.clear();
        return;
    }
    // the same results handed again (the renderer had nothing newer) keep their version
    if (queryXz.size() == size_t(n) * 2 && std::equal(queryXz.begin(), queryXz.end(), xz) &&
        std::equal(queryValues.begin(), queryValues.end(), values))
    {
        return;
    }
    queryXz.assign(xz, xz + size_t(n) * 2);
    queryValues.assign(values, values + size_t(n) * 4);
    ++queryVersion;
    querySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool DrawnWaterQuery(float x, float z, float& y, float& slopeX, float& slopeZ, uint32_t* version, double* seconds)
{
    std::lock_guard<std::mutex> lock(queryMutex);
    const int n = int(queryXz.size() / 2);
    int best = -1;
    float bestD2 = 1.5f * 1.5f;
    for (int i = 0; i < n; ++i)
    {
        if (queryValues[size_t(i) * 4 + 3] < 0.5f)
        {
            continue;
        }
        const float dx = x - queryXz[size_t(i) * 2], dz = z - queryXz[size_t(i) * 2 + 1];
        const float d2 = dx * dx + dz * dz;
        if (d2 < bestD2)
        {
            bestD2 = d2;
            best = i;
        }
    }
    if (best < 0)
    {
        return false;
    }
    const float* v = &queryValues[size_t(best) * 4];
    slopeX = std::clamp(v[1], -3.0f, 3.0f);
    slopeZ = std::clamp(v[2], -3.0f, 3.0f);
    y = v[0] + slopeX * (x - queryXz[size_t(best) * 2]) + slopeZ * (z - queryXz[size_t(best) * 2 + 1]);
    if (version)
    {
        *version = queryVersion;
    }
    if (seconds)
    {
        *seconds = querySeconds;
    }
    return std::isfinite(y);
}
} // namespace Poseidon
