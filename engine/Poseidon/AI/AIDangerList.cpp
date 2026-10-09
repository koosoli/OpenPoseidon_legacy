// AI-XRAY-004: a typed danger list per soldier. See AIDangerList.hpp.

#include <cstdlib>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AIDangerList.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>
#include <Random/randomGen.hpp>

#include <algorithm>
#include <unordered_map>
#include <vector>

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
    {"hit", 2000, 2000, 45},          {"grenade", 1000, 15, 10},       {"corpse", 2250, 50, 30},
    {"gunfire", 2500, 150, 15},       {"squadmate died", 3000, 50, 20}, {"squadmate hit", 2000, 50, 15},
    {"ricochet", 3000, 40, 15},
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

// AI-XRAY-006
constexpr float kBadCoverRadius = 3.0f;
constexpr float kBadCoverMemory = 120.0f;
struct BadCover
{
    const AICenter* side;
    Vector3 pos;
    float time;
};
std::vector<BadCover> s_badCovers;

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
        s_badCovers.clear();
    }
    s_lastNow = now;
}

void Expire(Rec& r, float now)
{
    int n = 0;
    for (int i = 0; i < r.count; i++)
    {
        const Entry& e = r.entries[i];
        const float age = now - e.time;
        if (age >= 0 && age <= kTypes[int(e.type)].memory) // a negative age: from before mission time went back
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

//! his record, if it is really his (not one left at a reused address); after the clock check
Rec* Find(const AIUnit* unit)
{
    auto it = s_recs.find(unit);
    if (it == s_recs.end())
        return nullptr;
    if (it->second.unit.GetLink() != unit)
    {
        s_recs.erase(it);
        return nullptr;
    }
    return &it->second;
}

void Note(AIUnit* unit, Type type, Vector3Par where, float error)
{
    if (!Enabled() || !unit || unit->IsAnyPlayer() || !unit->IsLocal() || unit->GetLifeState() != AIUnit::LSAlive ||
        !unit->IsFreeSoldier())
        return;
    AIGroup* grp = unit->GetGroup();
    if (!grp || grp->IsAnyPlayerGroup())
        return;
    Vector3 pos = where;
    if (error > 0)
    {
        pos[0] += (GRandGen.RandomValue() * 2 - 1) * error;
        pos[2] += (GRandGen.RandomValue() * 2 - 1) * error;
    }
    if (unit->Position().DistanceXZ2(pos) > Square(Range(type)))
        return;
    const float now = Now();
    CheckClock(now);
    if (s_recs.size() > 2048)
    {
        for (auto it = s_recs.begin(); it != s_recs.end();)
            it = !it->second.unit ? s_recs.erase(it) : std::next(it);
    }
    Rec* found = Find(unit);
    Rec& r = found ? *found : s_recs[unit];
    if (!found)
    {
        r = Rec{};
        r.unit = unit;
    }
    Expire(r, now);
    // the same kind of thing at the same place again: refresh it rather than fill the list
    for (int i = 0; i < r.count; i++)
    {
        Entry& e = r.entries[i];
        // (within 5 m, plus the guess's spread: a heard shooter is placed afresh at every shot)
        if (e.type == type && e.pos.DistanceXZ2(pos) < Square(5.0f + 2 * error))
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

void Forget(const AIUnit* unit, Vector3Par pos, float radius)
{
    CheckClock(Now());
    Rec* found = Find(unit);
    if (!found)
        return;
    Rec& r = *found;
    int n = 0;
    for (int i = 0; i < r.count; i++)
    {
        if (r.entries[i].pos.DistanceXZ2(pos) > Square(radius))
        {
            r.entries[n++] = r.entries[i];
        }
    }
    r.count = n;
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

bool Top(const AIUnit* unit, Vector3& pos, Type* type, bool (*accept)(Type))
{
    if (!Enabled())
        return false;
    const float now = Now();
    CheckClock(now);
    Rec* found = Find(unit);
    if (!found)
        return false;
    Rec& r = *found;
    Expire(r, now);
    int best = -1;
    float bestValue = 0;
    for (int i = 0; i < r.count; i++)
    {
        if (accept && !accept(r.entries[i].type))
            continue;
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

bool BadCoverEnabled()
{
    static bool enabled = false;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (const ParamEntry* cls = Pars.FindEntry("CfgAIFork"))
        {
            if (const ParamEntry* e = cls->FindEntry("badCover"))
            {
                enabled = static_cast<float>(*e) != 0;
            }
        }
    }
    return enabled;
}

void NoteBadCover(const AICenter* side, Vector3Par pos)
{
    if (!BadCoverEnabled() || !side)
        return;
    const float now = Now();
    CheckClock(now);
    // drop the expired; the same spot again only refreshes it
    s_badCovers.erase(std::remove_if(s_badCovers.begin(), s_badCovers.end(),
                                     [now](const BadCover& c) { return now - c.time > kBadCoverMemory; }),
                      s_badCovers.end());
    for (BadCover& c : s_badCovers)
    {
        if (c.side == side && c.pos.DistanceXZ2(pos) < Square(kBadCoverRadius))
        {
            c.time = now;
            return;
        }
    }
    if (s_badCovers.size() >= 256)
    {
        // the stalest (refreshed entries keep their place, so the first is not necessarily the oldest)
        s_badCovers.erase(std::min_element(s_badCovers.begin(), s_badCovers.end(),
                                           [](const BadCover& a, const BadCover& b) { return a.time < b.time; }));
    }
    s_badCovers.push_back({side, pos, now});
    if (LogOn())
    {
        LOG_INFO(World, "Danger: bad cover at ({:.0f},{:.0f}) t={:.1f}", pos.X(), pos.Z(), now);
    }
}

bool IsBadCover(const AICenter* side, Vector3Par pos)
{
    if (!BadCoverEnabled() || s_badCovers.empty())
        return false;
    const float now = Now();
    CheckClock(now);
    if (s_badCovers.empty())
        return false;
    for (const BadCover& c : s_badCovers)
    {
        const float age = now - c.time;
        if (c.side == side && age >= 0 && age <= kBadCoverMemory && c.pos.DistanceXZ2(pos) < Square(kBadCoverRadius))
        {
            static float s_nextLog = 0;
            if (LogOn() && (now >= s_nextLog || s_nextLog - now > 10))
            {
                s_nextLog = now + 2;
                LOG_INFO(World, "Danger: cover at ({:.0f},{:.0f}) skipped (bad, {:.0f} s old) t={:.1f}", pos.X(),
                         pos.Z(), age, now);
            }
            return true;
        }
    }
    return false;
}

} // namespace DangerList
} // namespace Poseidon
