// AI-XRAY-003: soldiers react to live grenades. See AIGrenadeDanger.hpp.

#include <cstdlib>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AIGrenadeDanger.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Poseidon
{
namespace GrenadeDanger
{
namespace
{
constexpr int kTag = 0x444F4447; // 'DODG' in Command::_param marks our own orders
constexpr float kRunSpeed = 4.5f;  // m/s a soldier covers when running clear (reaction and turning included below)
constexpr float kReaction = 0.8f;  // s before he gets going
constexpr float kGravity = 9.81f;

struct Settings
{
    bool enabled = false;
    float dodgeRadius = 15; // m: grenades further than this are not noticed (Lost Alpha xr_danger: 15 m)
};

const Settings& Cfg()
{
    static Settings s;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (const ParamEntry* cls = Pars.FindEntry("CfgAIFork"))
        {
            auto num = [&](const char* name, float def) -> float
            {
                const ParamEntry* e = cls->FindEntry(name);
                return e ? static_cast<float>(*e) : def;
            };
            s.enabled = num("grenadeDodge", 0) != 0;
            s.dodgeRadius = std::max(1.0f, num("dodgeRadius", s.dodgeRadius));
        }
    }
    return s;
}

bool LogOn()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_AI_HUNT_LOG");
        return v && v[0] == '1';
    }();
    return on;
}

float Now()
{
    return Glob.time.toFloat();
}

struct Grenade
{
    Vector3 landing;  // where it lies, or will first touch the ground
    float explodesAt; // mission time
    float radius;     // blast danger radius (m)
    float seen;       // last report
};

std::unordered_map<const void*, Grenade> s_grenades;

enum class Reaction
{
    None,
    Run
};

struct Rec
{
    OLink<AIUnit> unit;
    const void* grenade = nullptr;
    Reaction reaction = Reaction::None;
    Vector3 goal = VZero;
    float next = 0;
};

std::unordered_map<const AIUnit*, Rec> s_recs;

void Purge(float now)
{
    for (auto it = s_grenades.begin(); it != s_grenades.end();)
        it = now - it->second.seen > 0.5f ? s_grenades.erase(it) : std::next(it);
    if (s_recs.size() > 1024)
    {
        for (auto it = s_recs.begin(); it != s_recs.end();)
            it = !it->second.unit ? s_recs.erase(it) : std::next(it);
    }
}

bool OurOrderOnTop(AIUnit* unit)
{
    AISubgroup* sub = unit->GetSubgroup();
    AIGroup* grp = unit->GetGroup();
    if (!sub || !grp || sub == grp->MainSubgroup())
        return false;
    const Command* c = sub->GetCommand();
    return c && c->_message == Command::Move && c->_param == kTag;
}

void Go(AIUnit* unit, AIGroup* grp, Vector3 pos)
{
    Vector3 normal = VUp;
    unit->FindFreePosition(pos, normal);
    Command cmd;
    cmd._message = Command::Move;
    cmd._destination = pos;
    cmd._discretion = Command::Undefined; // never touch the group's formation / combat mode
    cmd._param = kTag;
    grp->IssueAutoCommand(cmd, unit);
}

//! the danger is over (or no longer his): withdraw what we did
void Finish(Rec& r, AIUnit* unit, bool exploded)
{
    if (r.reaction == Reaction::Run && OurOrderOnTop(unit))
    {
        AISubgroup* sub = unit->GetSubgroup();
        AISubgroupContext ctx(sub);
        sub->PopTask(&ctx); // a subgroup left without orders rejoins the group by itself
    }
    if (exploded && !unit->IsDanger())
    {
        unit->SetDanger(); // X-Ray: look around after the explosion
    }
    if (LogOn())
    {
        LOG_INFO(World, "Grenade: {} done ({}) t={:.1f}", (const char*)unit->GetDebugName(),
                 exploded ? "it went off" : "out of danger", Now());
    }
    r.reaction = Reaction::None;
    r.grenade = nullptr;
}
} // namespace

void Report(const void* shot, Vector3Par position, Vector3Par speed, float fuseLeft, float blastRadius, bool atRest)
{
    if (!Cfg().enabled)
        return;
    Vector3 landing = position;
    if (!atRest && GLandscape)
    {
        // first touch of the ground on a ballistic arc (a bounce adds a little; the radius below allows for it)
        const float h = std::max(0.0f, position.Y() - GLandscape->SurfaceYAboveWater(position.X(), position.Z()));
        const float vy = speed.Y();
        const float t = (vy + std::sqrt(vy * vy + 2 * kGravity * h)) / kGravity;
        landing = position + Vector3(speed.X(), 0, speed.Z()) * t;
        landing[1] = GLandscape->SurfaceYAboveWater(landing.X(), landing.Z());
    }
    Grenade& g = s_grenades[shot];
    g.landing = landing;
    g.explodesAt = Now() + fuseLeft;
    // X-Ray registers explosives as 10 m dangers. CWA's hand grenade (indirectHitRange 7 m) still kills at 9 m
    // in the AI1 test (damage reaches out to 4x the range, scaled by distance and cover); 13 m cost 0.1.
    g.radius = std::max(10.0f, blastRadius * 2 + 1);
    g.seen = Now();
}

void React(AIUnit* unit)
{
    const Settings& cfg = Cfg();
    if (!unit || !cfg.enabled)
        return;
    const float now = Now();
    Rec& r = s_recs[unit];
    if (!r.unit)
    {
        r = Rec{};
        r.unit = unit;
    }
    if (now < r.next)
        return;
    r.next = now + 0.2f;
    Purge(now);

    // the grenade he reacted to is gone: it went off
    if (r.reaction != Reaction::None && !s_grenades.count(r.grenade))
    {
        Finish(r, unit, true);
        return;
    }
    if (r.reaction == Reaction::Run)
    {
        // there, his order completes and he walks back to the formation -- towards the grenade, while it still
        // has a second or two (AI1 test): send him back to his spot until it has gone off
        AIGroup* g = unit->GetGroup();
        if (g && !OurOrderOnTop(unit) && unit->Position().DistanceXZ2(r.goal) > Square(2.0f))
        {
            Go(unit, g, r.goal);
        }
        return;
    }

    AIGroup* grp = unit->GetGroup();
    if (!grp || unit->IsAnyPlayer() || !unit->IsFreeSoldier() || unit->GetLifeState() != AIUnit::LSAlive)
        return;

    // the most urgent grenade he is inside the blast of
    const Vector3 me = unit->Position();
    const void* worst = nullptr;
    const Grenade* g = nullptr;
    for (const auto& kv : s_grenades)
    {
        const Grenade& c = kv.second;
        const float d2 = c.landing.DistanceXZ2(me);
        if (d2 > Square(cfg.dodgeRadius) || d2 > Square(c.radius))
            continue;
        if (!g || c.explodesAt < g->explodesAt)
        {
            g = &c;
            worst = kv.first;
        }
    }
    if (!g)
        return;

    const float dist = std::sqrt(g->landing.DistanceXZ2(me));
    const float timeLeft = g->explodesAt - now;
    const float runFor = g->radius + 2 - dist;
    const bool canMove = !(unit->GetAIDisabled() & AIUnit::DAMove) && unit->GetPerson() &&
                         !unit->GetPerson()->IsUserStopped();
    // Blast damage falls with distance and with cover (Landscape::ExplosionDammage); stance does not enter it,
    // so going prone saves nobody. Everyone who can move runs -- leaders too -- and a run cut short by the fuse
    // still takes him further from it. The time it needs is only logged.
    r.grenade = worst;
    if (canMove)
    {
        Vector3 away = me - g->landing;
        away[1] = 0;
        if (away.SquareSize() < 0.01f)
            away = -unit->GetVehicle()->Direction(); // right on top of it: back off
        away[1] = 0;
        away.Normalize();
        Vector3 goal = g->landing + away * (g->radius + 2);
        goal[1] = GLandscape ? GLandscape->SurfaceYAboveWater(goal.X(), goal.Z()) : goal.Y();
        Go(unit, grp, goal);
        r.goal = goal;
        r.reaction = Reaction::Run;
    }
    else
    {
        r.grenade = nullptr;
        return; // can't move (stopped, disableAI "MOVE"): nothing to do
    }
    if (LogOn())
    {
        LOG_INFO(World, "Grenade: {} runs clear{} -- {:.1f} m from it, {:.1f} s left (needs {:.1f}), blast {:.0f} m t={:.1f}",
                 (const char*)unit->GetDebugName(), unit == grp->Leader() ? " (leader)" : "", dist, timeLeft,
                 kReaction + runFor / kRunSpeed, g->radius, now);
    }
}

} // namespace GrenadeDanger
} // namespace Poseidon
