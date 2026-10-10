// SPDX-License-Identifier: GPL-3.0-or-later
#include "FireVolumes.hpp"

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace Poseidon
{
namespace
{
std::unordered_map<const void*, FireVolumeReport>& Live()
{
    static std::unordered_map<const void*, FireVolumeReport> live;
    return live;
}

struct TimedFire
{
    FireVolumeReport report;
    float start = 0, duration = 0;
};
std::vector<TimedFire>& Timed()
{
    static std::vector<TimedFire> timed;
    return timed;
}
uint32_t NextTimedKey()
{
    static uint32_t next = 0x40000001u;
    next += 2;
    return next;
}

std::unordered_set<const void*>& Drawn()
{
    static std::unordered_set<const void*> drawn;
    return drawn;
}

// The renderer's box for this fire (fire/mod.rs Grid::new uses the width it is sent).
float BoxWidth(const FireVolumeReport& r)
{
    if (r.kind == FireKindSmoke)
        // a cloud spreads wider than its source; the box's cells (width / 48) must stay small enough
        // for its billows, so the length downwind comes from the wind-turned box, not the width
        return std::max(r.radius * 10.0f, 8.0f);
    return r.kind == FireKindExplosion ? r.radius * 4.0f : std::max(r.radius * 5.0f, 2.4f);
}

// Probe the world around the fire once: floors everywhere; walls and ceilings where they can
// matter (a fireplace anywhere -- it may stand against a wall or in a room; a burst only under
// a roof). A wreck is skipped: the probes would find its own hull and wall the flames in.
void ProbeSolids(FireVolumeReport& r)
{
    const ISmokeWorldQuery& q = GSmokeWorldQuery();
    const int n = FireSolidColumns;
    r.boxWidth = BoxWidth(r);
    const float col = r.boxWidth / n;
    const float height = r.boxWidth * 1.8f;
    const Vector3 bed = r.base + Vector3(0, 0.3f + 0.6f * std::min(r.radius, 1.0f), 0);
    bool walls = r.kind == FireKindBed || r.kind == FireKindSmoke;
    if (r.kind == FireKindExplosion)
        walls = q.IsSheltered(bed, 12.0f);
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
        {
            const float cx = r.base.X() - r.boxWidth * 0.5f + (i + 0.5f) * col;
            const float cz = r.base.Z() - r.boxWidth * 0.5f + (j + 0.5f) * col;
            const int k = j * n + i;
            // over the bed itself the floor is the bed: the fireplace's own stones (its roadway
            // and fire geometry) would otherwise read as a floor above the flame and a wall
            // around it, and the solver would put the fire out
            const float dx = cx - r.base.X(), dz = cz - r.base.Z();
            const bool overBed = dx * dx + dz * dz < Square(r.radius + col * 0.75f);
            const float fl = overBed ? r.base.Y() : q.FloorHeightBelow(Vector3(cx, bed.Y() + 0.5f, cz));
            r.floor[k] = std::max(fl, r.base.Y() - 2.0f);
            r.ceiling[k] = 1e6f;
            if (r.kind == FireKindWreck || !walls)
                continue;
            Vector3 hit;
            if (!overBed && q.ProbeSegment(bed, Vector3(cx, bed.Y(), cz), hit))
            {
                r.ceiling[k] = -1e6f; // behind a wall
                continue;
            }
            if (q.ProbeSegment(Vector3(cx, std::max(fl, r.base.Y()) + (overBed ? 1.0f : 0.3f), cz),
                               Vector3(cx, r.base.Y() + height, cz), hit))
                r.ceiling[k] = hit.Y();
        }
    }
    r.solidsAt = r.base;
    r.solidsValid = true;
    int wallCols = 0, roofCols = 0;
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < n * n; ++k)
    {
        wallCols += r.ceiling[k] < r.floor[k];
        roofCols += r.ceiling[k] >= r.floor[k] && r.ceiling[k] < 1e5f;
        lo = std::min(lo, r.floor[k] - r.base.Y());
        hi = std::max(hi, r.floor[k] - r.base.Y());
    }
    LOG_INFO(World, "Fire solids: kind {} r {:.2f} box {:.1f} m at {:.0f} {:.0f}: {} wall columns, {} under a roof, floor {:+.2f}..{:+.2f} m",
             r.kind, r.radius, r.boxWidth, r.base.X(), r.base.Z(), wallCols, roofCols, lo, hi);
}

uint32_t KeyOf(const void* owner)
{
    const auto bits = reinterpret_cast<std::uintptr_t>(owner);
    return static_cast<uint32_t>((bits >> 4) ^ (bits >> 36)) | 1u;
}
} // namespace

bool FireVolumesEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_FIRE");
        return !(v && v[0] == '0');
    }();
    return on;
}

void ReportFireVolume(const void* owner, Vector3Par base, float radius, float intensity, float smoke, int kind,
                      const float* color)
{
    if (!FireVolumesEnabled())
        return;
    FireVolumeReport& r = Live()[owner];
    if (r.key == 0)
        r.key = KeyOf(owner);
    r.base = base;
    r.radius = radius;
    r.intensity = std::clamp(intensity, 0.0f, 1.0f);
    r.smoke = std::clamp(smoke, 0.0f, 1.0f);
    r.kind = kind;
    r.time = Glob.time.toFloat();
    if (color)
        std::copy(color, color + 4, r.color);
    if (!r.solidsValid || (r.solidsAt - base).SquareSize() > 0.25f || std::fabs(r.boxWidth - BoxWidth(r)) > 0.01f)
        ProbeSolids(r);
}

void SpawnFireVolume(Vector3Par base, float radius, float smoke, float duration, int kind)
{
    if (!FireVolumesEnabled())
        return;
    TimedFire t;
    t.report.base = base;
    t.report.radius = radius;
    t.report.smoke = std::clamp(smoke, 0.0f, 1.0f);
    t.report.kind = kind;
    t.report.key = NextTimedKey();
    t.start = Glob.time.toFloat();
    t.duration = duration;
    ProbeSolids(t.report);
    auto& timed = Timed();
    // a barrage must not starve the steady fires of boxes: keep the newest few bursts
    if (kind == FireKindExplosion)
    {
        int bursts = 0;
        for (auto it = timed.rbegin(); it != timed.rend(); ++it)
            bursts += it->report.kind == FireKindExplosion;
        if (bursts >= 6)
        {
            auto oldest = std::find_if(timed.begin(), timed.end(),
                                       [](const TimedFire& f) { return f.report.kind == FireKindExplosion; });
            if (oldest != timed.end())
                timed.erase(oldest);
        }
    }
    timed.push_back(t);
}

void ExtinguishFireVolumes()
{
    Timed().clear();
}

int TimedFireVolumeCount()
{
    return static_cast<int>(Timed().size());
}

void CollectFireVolumes(std::vector<FireVolumeReport>& out, Vector3Par camera)
{
    out.clear();
    const float now = Glob.time.toFloat();
    auto& live = Live();
    std::vector<std::pair<float, const void*>> near;
    for (auto it = live.begin(); it != live.end();)
    {
        // a mission restart rewinds the clock: anything reported "in the future" is stale too
        if (now - it->second.time > 0.5f || it->second.time > now + 0.5f)
        {
            it = live.erase(it);
            continue;
        }
        out.push_back(it->second);
        near.emplace_back((it->second.base - camera).Size(), it->first);
        ++it;
    }
    auto& timed = Timed();
    timed.erase(std::remove_if(timed.begin(), timed.end(),
                               [&](const TimedFire& f)
                               {
                                   const float age = now - f.start;
                                   return age < -0.5f || (f.duration >= 0 && age > f.duration);
                               }),
                timed.end());
    for (TimedFire& f : timed)
    {
        const float age = std::max(now - f.start, 0.0f);
        float intensity = 1.0f;
        if (f.duration > 0)
        {
            const float u = std::clamp(age / f.duration, 0.0f, 1.0f);
            intensity = f.report.kind == FireKindExplosion ? std::sqrt(1.0f - u) : std::min(1.0f, (1.0f - u) * 5.0f);
        }
        FireVolumeReport r = f.report;
        r.intensity = intensity;
        r.time = now;
        out.push_back(r);
    }
    std::sort(near.begin(), near.end());
    auto& drawn = Drawn();
    drawn.clear();
    for (std::size_t i = 0; i < near.size() && i < static_cast<std::size_t>(MaxDrawnFireVolumes); ++i)
        if (near[i].first < FireVolumeRange)
            drawn.insert(near[i].second);
}

bool FireVolumeDrawn(const void* owner)
{
    return Drawn().count(owner) != 0;
}
} // namespace Poseidon
