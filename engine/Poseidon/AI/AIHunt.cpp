#include <cstdlib>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/AI/AIHunt.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Random/randomGen.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
namespace Hunt
{
namespace
{

constexpr int kTag = 0x48554E54; // 'HUNT' in Command::_param marks our own orders

struct Settings
{
    // Oli: hunting and the grenade interval are off by default until the X-Ray play-test (AI-XRAY-001);
    // CfgAIFork { hunting = 1; grenadeInterval = 5; } turns them on (Malprave's values)
    bool enabled = false;
    float unseenAfter = 8;   // s without sight before hunting starts
    float holdMin = 5, holdMax = 10;
    float flankMin = 30, flankMax = 80; // m from the last known position
    float maxTravel = 300;   // m: don't hunt contacts further than this
    float phaseTimeout = 60; // s per move before giving up on it
    float cooldown = 30;     // s before the same soldier hunts again
    float lostAttack = 20;   // s: an attack on a target nobody has seen this long hands over to hunting
    float grenadeInterval = 0; // s between hand grenades from one group (0 = no limit)
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
            s.enabled = num("hunting", 0) != 0;
            s.unseenAfter = std::max(1.0f, num("huntUnseenAfter", s.unseenAfter));
            s.holdMin = std::max(0.0f, num("huntHoldMin", s.holdMin));
            s.holdMax = std::max(s.holdMin, num("huntHoldMax", s.holdMax));
            s.flankMin = std::max(5.0f, num("huntFlankMin", s.flankMin));
            s.flankMax = std::max(s.flankMin, num("huntFlankMax", s.flankMax));
            s.maxTravel = std::max(20.0f, num("huntMaxTravel", s.maxTravel));
            s.lostAttack = std::max(5.0f, num("huntLostAttack", s.lostAttack));
            s.grenadeInterval = std::max(0.0f, num("grenadeInterval", s.grenadeInterval));
        }
        LOG_INFO(World, "CfgAIFork (AIHunt): {} hunting={} grenadeInterval={:.1f}",
                 Pars.FindEntry("CfgAIFork") ? "found" : "absent", s.enabled ? 1 : 0, s.grenadeInterval);
    }
    return s;
}

enum Phase
{
    Idle,
    Hold,
    Flank,
    Search,
    Rest
};

struct Rec
{
    OLink<AIUnit> unit;
    const AIUnit* key = nullptr;
    OLink<AIGroup> group;
    LLink<Target> target;
    Phase phase = Idle;
    float until = 0;
    float next = 0;
    Vector3 goal = VZero;
    float logNext = 0; // POSEIDON_AI_HUNT_LOG throttle
};

std::unordered_map<const AIUnit*, Rec> s_recs;
float s_purge = 0;

float Now()
{
    return Glob.time.toFloat();
}

float Rand(float a, float b)
{
    return a + (b - a) * GRandGen.RandomValue();
}

void Purge()
{
    const float now = Now();
    if (now - s_purge < 10)
        return;
    s_purge = now;
    for (auto it = s_recs.begin(); it != s_recs.end();)
        it = it->second.unit ? std::next(it) : s_recs.erase(it);
}

//! the group's own subgroup is busy with a waypoint move / attack / get-in ...
bool MainBusy(AIGroup* grp)
{
    AISubgroup* main = grp->MainSubgroup();
    const Command* c = main ? main->GetCommand() : nullptr;
    return c && c->_message != Command::Wait && c->_message != Command::Hide;
}

bool WaypointAllows(AIGroup* grp)
{
    if (grp->GetScript())
        return false;
    const Mission* m = grp->GetMission();
    if (!m || m->_action != Mission::Arcade || !grp->GetCurrent())
        return true; // no waypoints: free to react
    const int idx = grp->GetCurrent()->_fsm->Var(0);
    if (idx < 0 || idx >= grp->NWaypoints())
        return true;
    switch (grp->GetWaypoint(idx).type)
    {
        case ACHOLD:
        case ACSENTRY:
        case ACSCRIPTED:
        case ACGETIN:
        case ACGETOUT:
        case ACLOAD:
        case ACUNLOAD:
        case ACTRANSPORTUNLOAD:
        case ACSUPPORT:
        case ACJOIN:
        case ACLEADER:
            return false;
        default:
            return true;
    }
}

bool Allowed(AIUnit* unit, AIGroup* grp)
{
    if (!Cfg().enabled || !grp || unit->IsAnyPlayer() || grp->IsAnyPlayerGroup() || grp->GetFlee())
        return false;
    if (unit == grp->Leader() || !unit->IsFreeSoldier())
        return false;
    const CombatMode cm = unit->GetCombatMode();
    if (cm != CMCombat && cm != CMStealth)
        return false;
    if (unit->IsKeepingFormation())
        return false; // Blue / Green / Yellow: stay with the group
    if (unit->GetAIDisabled() & AIUnit::DAMove)
        return false;
    Person* p = unit->GetPerson();
    if (!p || p->IsUserStopped())
        return false;
    if (unit->IsPinnedDown())
        return false;
    if (grp->CommandSent(unit, Command::Stop) || grp->CommandSent(unit, Command::Expect))
        return false;
    if (MainBusy(grp) || !WaypointAllows(grp))
        return false;
    return true;
}

//! the first gate of Allowed that blocks (for the hunt log)
const char* BlockReason(AIUnit* unit, AIGroup* grp)
{
    if (!grp) return "no group";
    if (unit->IsAnyPlayer()) return "player";
    if (grp->IsAnyPlayerGroup()) return "player group";
    if (grp->GetFlee()) return "fleeing";
    if (unit == grp->Leader()) return "leader";
    if (!unit->IsFreeSoldier()) return "not a free soldier";
    const CombatMode cm = unit->GetCombatMode();
    if (cm != CMCombat && cm != CMStealth) return "not combat/stealth";
    if (unit->IsKeepingFormation()) return "keeping formation (blue/green/yellow)";
    if (unit->GetAIDisabled() & AIUnit::DAMove) return "move disabled";
    Person* p = unit->GetPerson();
    if (!p || p->IsUserStopped()) return "stopped";
    if (unit->IsPinnedDown()) return "pinned down";
    if (grp->CommandSent(unit, Command::Stop) || grp->CommandSent(unit, Command::Expect)) return "stop/expect order";
    if (MainBusy(grp)) return "main subgroup busy";
    if (!WaypointAllows(grp)) return "waypoint";
    return "allowed";
}

//! nearest known enemy nobody has seen for a while; seenNow = some enemy is in sight
Target* PickUnseen(AIUnit* unit, AIGroup* grp, bool& seenNow)
{
    seenNow = false;
    AICenter* center = grp->GetCenter();
    if (!center)
        return nullptr;
    const Settings& cfg = Cfg();
    const TargetList& list = grp->GetTargetList();
    Target* best = nullptr;
    float bestD2 = FLT_MAX;
    for (int i = 0; i < list.Size(); i++)
    {
        Target* t = list[i];
        if (!t || t->destroyed || t->vanished || !t->idExact || !center->IsEnemy(t->side) || !t->IsKnownBy(unit))
            continue;
        const float since = Glob.time - t->lastSeen;
        if (since < cfg.unseenAfter)
        {
            seenNow = true;
            continue;
        }
        if (since > 120)
            continue;
        const float d2 = t->AimingPosition().Distance2(unit->Position());
        if (d2 > Square(cfg.maxTravel) || d2 >= bestD2)
            continue;
        bestD2 = d2;
        best = t;
    }
    return best;
}

int Hunters(AIGroup* grp)
{
    int n = 0;
    for (const auto& kv : s_recs)
    {
        const Rec& r = kv.second;
        if (r.unit && r.group == grp && (r.phase == Flank || r.phase == Search))
            n++;
    }
    return n;
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

void Abort(Rec& r, AIUnit* unit, float rest)
{
    if (OurOrderOnTop(unit))
    {
        // drop only our move; whatever was under it (e.g. the engine's Hide) resumes,
        // and a subgroup left without orders rejoins the group by itself
        AISubgroup* sub = unit->GetSubgroup();
        AISubgroupContext ctx(sub);
        sub->PopTask(&ctx);
    }
    r.phase = Rest;
    r.until = Now() + rest;
    r.target = nullptr;
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

Vector3 FlankPoint(AIUnit* unit, Vector3Par target)
{
    const Settings& cfg = Cfg();
    Vector3 dir = target - unit->Position();
    dir[1] = 0;
    float dist = dir.Size();
    if (dist < 1)
    {
        dir = unit->GetVehicle()->Direction();
        dir[1] = 0;
        dist = 1;
    }
    dir /= dir.Size();
    // swing round the contact, 60-90 degrees off our line of approach
    const float ang = (GRandGen.RandomValue() < 0.5f ? -1.0f : 1.0f) * Rand(60, 90) * (3.14159265f / 180.0f);
    const float bx = -dir[0], bz = -dir[2];
    const float c = std::cos(ang), s = std::sin(ang);
    Vector3 off(bx * c - bz * s, 0, bx * s + bz * c);
    const float r = std::clamp(dist * 0.6f, cfg.flankMin, cfg.flankMax);
    Vector3 p = target + off * r;
    p[1] = GLandscape ? GLandscape->SurfaceY(p[0], p[2]) : target[1];
    return p;
}

Vector3 SearchPoint(const Target* t)
{
    Vector3 p = t->AimingPosition();
    const float spread = std::min(15.0f, std::sqrt(t->posError[0] * t->posError[0] + t->posError[2] * t->posError[2]));
    p[0] += Rand(-spread, spread);
    p[2] += Rand(-spread, spread);
    p[1] = GLandscape ? GLandscape->SurfaceY(p[0], p[2]) : p[1];
    return p;
}

} // namespace

// POSEIDON_AI_HUNT_LOG=1: one line per hunting phase change, grenade recorded or refused (X-Ray play-test,
// AI-XRAY-001 -- Malprave's own next step was this logging)
bool HuntLog()
{
    static const bool on = []
    {
        const char* v = std::getenv("POSEIDON_AI_HUNT_LOG");
        return v && v[0] == '1';
    }();
    return on;
}

const char* PhaseName(int p)
{
    static const char* const names[] = {"Idle", "Hold", "Flank", "Search", "Rest"};
    return p >= 0 && p < 5 ? names[p] : "?";
}

void ThinkImpl(AIUnit* unit);

void Think(AIUnit* unit)
{
    if (!unit || !Cfg().enabled)
        return;
    if (!HuntLog())
    {
        ThinkImpl(unit);
        return;
    }
    auto it = s_recs.find(unit);
    const int before = it != s_recs.end() && it->second.unit ? int(it->second.phase) : int(Idle);
    ThinkImpl(unit);
    it = s_recs.find(unit);
    const int after = it != s_recs.end() ? int(it->second.phase) : int(Idle);
    if (after != before)
    {
        const Vector3 g = it->second.goal;
        LOG_INFO(World, "Hunt: {} {} -> {} goal=({:.0f},{:.0f}) t={:.1f}", (const char*)unit->GetDebugName(),
                 PhaseName(before), PhaseName(after), g.X(), g.Z(), Now());
    }
}

void ThinkImpl(AIUnit* unit)
{
    Purge();
    const float now = Now();
    AIGroup* grp = unit->GetGroup();

    Rec& r = s_recs[unit];
    if (!r.unit)
    {
        r = Rec{}; // new soldier (or a new one at a reused address)
        r.unit = unit;
        r.key = unit;
    }
    if (now < r.next)
        return; // once a second per soldier
    r.next = now + 1.0f;
    r.group = grp;

    if (!Allowed(unit, grp))
    {
        if (HuntLog() && now >= r.logNext)
        {
            r.logNext = now + 5;
            LOG_INFO(World, "Hunt: {} blocked: {} t={:.1f}", (const char*)unit->GetDebugName(), BlockReason(unit, grp), now);
        }
        if (r.phase == Flank || r.phase == Search || r.phase == Hold)
            Abort(r, unit, 5);
        return;
    }

    // an Attack order on a target nobody has seen for a while only walks to a stale
    // guess: end it and let the search below take over
    if (AISubgroup* sub = unit->GetSubgroup(); sub && sub != grp->MainSubgroup())
    {
        const Command* c = sub->GetCommand();
        if (c && (c->_message == Command::Attack || c->_message == Command::AttackAndFire) &&
            (c->_context == Command::CtxAuto || c->_context == Command::CtxAutoSilent))
        {
            const Target* at = c->_targetE;
            if (at && !at->destroyed && Glob.time - at->lastSeen > Cfg().lostAttack)
            {
                sub->ClearAttackCommands();
            }
        }
    }

    bool seenNow = false;
    Target* t = PickUnseen(unit, grp, seenNow);
    const bool attacking = grp->CommandSent(unit, Command::Attack) || grp->CommandSent(unit, Command::AttackAndFire);

    switch (r.phase)
    {
        case Rest:
            if (now >= r.until)
                r.phase = Idle;
            return;
        case Idle:
            if (!t || seenNow || attacking)
            {
                if (HuntLog() && now >= r.logNext)
                {
                    r.logNext = now + 5;
                    LOG_INFO(World, "Hunt: {} idle: {} t={:.1f}", (const char*)unit->GetDebugName(),
                             attacking ? "attacking" : seenNow ? "an enemy in sight" : "no known enemy unseen for 8-120 s",
                             now);
                }
                return;
            }
            r.target = t;
            r.phase = Hold; // stay in cover (the engine's own hide logic) and watch
            r.until = now + Rand(Cfg().holdMin, Cfg().holdMax);
            return;
        default:
            break;
    }

    // an enemy back in sight, or the contact gone: normal combat takes over
    Target* cur = r.target;
    if (seenNow || attacking || !cur || cur->destroyed || cur->vanished || Glob.time - cur->lastSeen > 120)
    {
        Abort(r, unit, 5);
        return;
    }

    switch (r.phase)
    {
        case Hold:
            if (now < r.until)
                return;
            if (Hunters(grp) < std::max(1, grp->NUnits() / 2))
            {
                r.goal = FlankPoint(unit, cur->AimingPosition());
                Go(unit, grp, r.goal);
                r.phase = Flank;
                r.until = now + Cfg().phaseTimeout;
            }
            else
            {
                r.until = now + Rand(Cfg().holdMin, Cfg().holdMax); // half the squad already out: keep covering
            }
            return;
        case Flank:
        case Search:
        {
            const bool arrived = unit->Position().Distance2(r.goal) < Square(8) ||
                                 (!OurOrderOnTop(unit) && unit->GetSubgroup() == grp->MainSubgroup());
            if (!arrived && now < r.until)
                return;
            if (r.phase == Flank)
            {
                r.goal = SearchPoint(cur);
                Go(unit, grp, r.goal);
                r.phase = Search;
                r.until = now + Cfg().phaseTimeout;
            }
            else
            {
                Abort(r, unit, Cfg().cooldown); // searched: back to the group
            }
            return;
        }
        default:
            return;
    }
}

bool MayHunt(AIUnit* unit)
{
    return unit && Allowed(unit, unit->GetGroup());
}

// ---- squad rules ----------------------------------------------------------------

namespace
{
std::unordered_map<const AIGroup*, float> s_lastGrenade;
}

void OnGrenadeThrown(const AIGroup* grp)
{
    if (!grp)
        return;
    if (s_lastGrenade.size() > 512)
        s_lastGrenade.clear(); // stale groups; worst case one early throw
    s_lastGrenade[grp] = Now();
    if (HuntLog())
    {
        LOG_INFO(World, "Hunt: grenade thrown by group {} t={:.1f}", (const char*)grp->GetDebugName(), Now());
    }
}

bool GrenadeAllowed(const AIGroup* grp)
{
    if (!grp)
        return true;
    auto it = s_lastGrenade.find(grp);
    const bool ok = it == s_lastGrenade.end() || Now() - it->second >= Cfg().grenadeInterval;
    if (!ok && HuntLog())
    {
        LOG_INFO(World, "Hunt: grenade refused for group {} ({:.1f} s after the last) t={:.1f}",
                 (const char*)grp->GetDebugName(), Now() - it->second, Now());
    }
    return ok;
}

bool SplashSafe(const AIUnit* firer, Vector3Par aimPoint, float indirectHitRange)
{
    const AIGroup* grp = firer ? firer->GetGroup() : nullptr;
    if (!grp)
        return true;
    const float r = std::max(5.0f, indirectHitRange * 1.5f);
    for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
    {
        AIUnit* u = grp->UnitWithID(i + 1);
        if (!u || u == firer || u->GetLifeState() != AIUnit::LSAlive)
            continue;
        EntityAI* veh = u->GetVehicle();
        if (!veh)
            continue;
        if (veh->Position().Distance2(aimPoint) < Square(r))
            return false;
        // his cover counts too: he may be about to run there
        if (const Object* cover = veh->GetHideBehind(); cover && cover->Position().Distance2(aimPoint) < Square(r))
            return false;
    }
    return true;
}

} // namespace Hunt
} // namespace Poseidon
