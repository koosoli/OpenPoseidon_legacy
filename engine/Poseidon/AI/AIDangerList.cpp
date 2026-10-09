// AI-XRAY-004: a typed danger list per soldier. See AIDangerList.hpp.

#include <cstdlib>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AIDangerList.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <unordered_map>

namespace Poseidon
{
namespace DangerList
{
namespace
{
struct TypeInfo
{
    const char* name;
    float weight; // X-Ray CDangerManager base weight
    float range;  // m
    float memory; // s
};

constexpr TypeInfo kTypes[int(Type::Count)] = {
    {"hit", 2000, 150, 60},           {"grenade", 1000, 15, 20},       {"corpse", 2250, 20, 10},
    {"gunfire", 2500, 150, 15},       {"squadmate died", 3000, 20, 15}, {"squadmate hit", 2000, 20, 15},
    {"ricochet", 3000, 10, 30},
};

constexpr int kMaxPerUnit = 8;

struct Entry
{
    Type type = Type::Hit;
    Vector3 pos = VZero;
    float time = 0;
};

struct Rec
{
    OLink<AIUnit> unit;
    Entry entries[kMaxPerUnit];
    int count = 0;
};

std::unordered_map<const AIUnit*, Rec> s_recs;
float s_lastNow = 0;

float Now()
{
    return Glob.time.toFloat();
}

bool LogOn()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_AI_DANGER_LOG");
        return v && v[0] == '1';
    }();
    return on;
}

//! mission time went back (restart, load): what was recorded belongs to another session
void CheckClock(float now)
{
    if (now + 1.0f < s_lastNow)
    {
        s_recs.clear();
    }
    s_lastNow = now;
}

void Expire(Rec& r, float now)
{
    int n = 0;
    for (int i = 0; i < r.count; i++)
    {
        const Entry& e = r.entries[i];
        if (now - e.time <= kTypes[int(e.type)].memory)
        {
            r.entries[n++] = e;
        }
    }
    r.count = n;
}
} // namespace

bool Enabled()
{
    static bool enabled = false;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (const ParamEntry* cls = Pars.FindEntry("CfgAIFork"))
        {
            if (const ParamEntry* e = cls->FindEntry("dangerList"))
            {
                enabled = static_cast<float>(*e) != 0;
            }
        }
    }
    return enabled;
}

float Priority(Type type, float ageMs)
{
    return kTypes[int(type)].weight * 10 + ageMs;
}

float Range(Type type)
{
    return kTypes[int(type)].range;
}

float Memory(Type type)
{
    return kTypes[int(type)].memory;
}

const char* Name(Type type)
{
    return kTypes[int(type)].name;
}

void Note(AIUnit* unit, Type type, Vector3Par pos)
{
    if (!Enabled() || !unit || unit->IsAnyPlayer() || !unit->IsLocal() || unit->GetLifeState() != AIUnit::LSAlive)
        return;
    if (unit->Position().DistanceXZ2(pos) > Square(Range(type)))
        return;
    const float now = Now();
    CheckClock(now);
    if (s_recs.size() > 2048)
    {
        for (auto it = s_recs.begin(); it != s_recs.end();)
            it = !it->second.unit ? s_recs.erase(it) : std::next(it);
    }
    Rec& r = s_recs[unit];
    if (!r.unit)
    {
        r = Rec{};
        r.unit = unit;
    }
    Expire(r, now);
    // the same kind of thing at the same place again: refresh it rather than fill the list
    for (int i = 0; i < r.count; i++)
    {
        Entry& e = r.entries[i];
        if (e.type == type && e.pos.DistanceXZ2(pos) < Square(5.0f))
        {
            e.pos = pos;
            e.time = now;
            return;
        }
    }
    Entry fresh{type, pos, now};
    if (r.count < kMaxPerUnit)
    {
        r.entries[r.count++] = fresh;
    }
    else
    {
        // full: replace the least urgent
        int worst = 0;
        for (int i = 1; i < r.count; i++)
        {
            if (Priority(r.entries[i].type, (now - r.entries[i].time) * 1000) >
                Priority(r.entries[worst].type, (now - r.entries[worst].time) * 1000))
            {
                worst = i;
            }
        }
        r.entries[worst] = fresh;
    }
    if (!unit->IsDanger())
    {
        unit->SetDanger();
    }
    if (LogOn())
    {
        LOG_INFO(World, "Danger: {} notes {} at ({:.0f},{:.0f}), {:.0f} m t={:.1f}", (const char*)unit->GetDebugName(),
                 Name(type), pos.X(), pos.Z(), std::sqrt(unit->Position().DistanceXZ2(pos)), now);
    }
}

void LogTurn(const AIUnit* unit, Vector3Par pos)
{
    if (!LogOn())
        return;
    static std::unordered_map<const AIUnit*, float> s_next;
    const float now = Now();
    if (s_next.size() > 4096)
        s_next.clear();
    float& next = s_next[unit];
    if (now < next && next - now < 10)
        return;
    next = now + 5;
    LOG_INFO(World, "Danger: {} looks towards ({:.0f},{:.0f}) t={:.1f}", (const char*)unit->GetDebugName(), pos.X(),
             pos.Z(), now);
}

bool Top(const AIUnit* unit, Vector3& pos, Type* type)
{
    if (!Enabled())
        return false;
    auto it = s_recs.find(unit);
    if (it == s_recs.end())
        return false;
    const float now = Now();
    Rec& r = it->second;
    Expire(r, now);
    int best = -1;
    float bestValue = 0;
    for (int i = 0; i < r.count; i++)
    {
        const float v = Priority(r.entries[i].type, (now - r.entries[i].time) * 1000);
        if (best < 0 || v < bestValue)
        {
            best = i;
            bestValue = v;
        }
    }
    if (best < 0)
        return false;
    pos = r.entries[best].pos;
    if (type)
        *type = r.entries[best].type;
    return true;
}

} // namespace DangerList
} // namespace Poseidon
