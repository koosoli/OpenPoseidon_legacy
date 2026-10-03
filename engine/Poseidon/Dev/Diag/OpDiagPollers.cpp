// DIAG-001 pollers: per-frame and per-second checks of the world, plus run settings and the crash event.
// Compiled only with POSEIDON_DIAG=1 (see OpDiag.hpp).
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source): MwDiag2.cpp (AI state of watched units,
// stuck / spin detector, corpses, sound_play, crash, seed, fixed dt) and MwDiag7.cpp (trails).
// Not ported: the view distance sweep, per-frame file-load lists, anim_load / skeleton events and the
// per-group think cost (MwDiag2), the x-ray and the "walls" probe (MwDiag7, Cry of Fear chase AI), the
// chase state in "trail" (Cry of Fear). The stuck / spin logic itself lives in OpDiagStuck.hpp.
// --diag-fixed-dt is not a second fixed-step mechanism: it sets Oli's POSEIDON_LOCKSTEP_HZ (GameLoop.cpp).
//
// Events (events.jsonl, same line format as OpDiag.cpp):
//   sound_play   file, is3D, ok, n        a wave was opened (first time, then at most every 2 s per file)
//   sound_stats  files, top[[file, n]]    at exit: most opened sound files
//   ai           unit, target, assigned, state, combat, semaphore, planning, mode, path, disabled, wantDist,
//                pathEnd                  watched units, whenever one of the fields changed (polled 4x / s)
//   stuck        unit, pos, sec, wantDist, want, path, state, move   (OpDiagStuck.hpp)
//   spin         unit, pos, turned, sec, move
//   corpse       unit, pos, ground, above, speed, tilt, move, sec   6 s after a death
//   trail        unit, move, speed        every 0.25 s of mission time per watched man
//   crash        code, addr, dump, stack  unhandled exception (summary.json is written too)

#include <Poseidon/Dev/Diag/OpDiag.hpp>
#include <Poseidon/Dev/Diag/OpDiagInternal.hpp>
#include <Poseidon/Dev/Diag/OpDiagStuck.hpp>

#if POSEIDON_DIAG

#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Platform/CrashHandler.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Game/GameLoop.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Random/randomGen.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Poseidon::Dev::OpDiag
{
using Poseidon::Foundation::AppConfig;
using namespace Internal;

namespace
{
struct UnitTrack
{
    OLink<Object> obj;
    StuckTrack track;
    double seenT = 0;
};

struct CorpseTrack
{
    OLink<Object> obj;
    double deathT = 0;
};

struct Trail
{
    OLink<Object> obj;
    std::deque<Vector3> pts;
    float lastMission = -1;
};

struct Pollers
{
    std::mutex soundMutex;                                // waves can be opened from the audio loader thread
    std::map<std::string, std::pair<double, int>> sounds; // file -> (last logged real time, opens)
    std::map<const Object*, std::string> aiLast;          // watched units: last JSON of the polled fields
    double aiNext = 0;
    std::vector<UnitTrack> units;
    double unitsNext = 0;
    float stuckSec = 5;
    std::vector<CorpseTrack> corpses;
    std::vector<Trail> trails;
    double trailNext = 0;
    int seed = -1;
    bool seeded = false;
};

Pollers& P()
{
    static Pollers p;
    return p;
}

constexpr size_t kTrailMax = 40; // 10 s at 0.25 s

const char* kStateNames[] = {"wait",    "init",     "busy",   "completed", "delay",
                             "inCargo", "stopping", "replan", "stopped",   "sending"};
const char* kCombatNames[] = {"unchanged", "careless", "safe", "aware", "combat", "stealth"};
const char* kSemNames[] = {"blue", "green", "white", "yellow", "red"};
const char* kPlanNames[] = {"doNotPlan", "leaderPlanned", "leaderDirect", "formationPlanned", "vehiclePlanned"};
const char* kModeNames[] = {"directNormal", "directExact", "normal", "exact"};

template <size_t N>
const char* EnumName(const char* (&names)[N], int v)
{
    return v >= 0 && v < static_cast<int>(N) ? names[v] : "?";
}

// every AI unit (soldiers, crewed vehicles)
template <typename Fn>
void ForEachUnit(Fn fn)
{
    if (!GWorld)
        return;
    AICenter* centers[] = {GWorld->GetWestCenter(), GWorld->GetEastCenter(), GWorld->GetGuerrilaCenter(),
                           GWorld->GetCivilianCenter()};
    for (AICenter* c : centers)
    {
        if (!c)
            continue;
        for (int g = 0; g < c->NGroups(); g++)
        {
            AIGroup* grp = c->GetGroup(g);
            if (!grp)
                continue;
            for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
                if (AIUnit* u = grp->UnitWithID(i + 1))
                    fn(u);
        }
    }
}

float GroundY(Vector3Par p)
{
    return GLandscape ? GLandscape->SurfaceY(p.X(), p.Z()) : p.Y();
}

std::string MoveOf(Object* o)
{
    Man* m = dyn_cast<Man>(o);
    return m ? MoveNameOf(m) : std::string();
}

// the polled AI fields of one unit as a flat JSON object body (no braces), for change detection
std::string AiFields(EntityAI* ai)
{
    std::string f;
    Target* tgt = ai->GetFireTarget();
    f += "\"target\":" + (tgt && tgt->idExact ? Json::Quote(ObjName(tgt->idExact).c_str()) : std::string("null"));
    AIUnit* unit = ai->CommanderUnit();
    if (!unit)
        return f;
    Target* as = unit->GetTargetAssigned();
    f += ",\"assigned\":" + (as && as->idExact ? Json::Quote(ObjName(as->idExact).c_str()) : std::string("null"));
    f += ",\"state\":" + Json::Quote(EnumName(kStateNames, static_cast<int>(unit->GetState())));
    f += ",\"combat\":" + Json::Quote(EnumName(kCombatNames, static_cast<int>(unit->GetCombatMode())));
    f += ",\"semaphore\":" + Json::Quote(EnumName(kSemNames, static_cast<int>(unit->GetSemaphore())));
    f += ",\"planning\":" + Json::Quote(EnumName(kPlanNames, static_cast<int>(unit->GetPlanningMode())));
    f += ",\"mode\":" + Json::Quote(EnumName(kModeNames, static_cast<int>(unit->GetMode())));
    f += ",\"path\":" + std::to_string(unit->GetPath().Size());
    f += ",\"disabled\":" + std::to_string(unit->GetAIDisabled());
    return f;
}

void PollAi()
{
    Pollers& s = P();
    for (Object* o : Watched())
    {
        EntityAI* ai = dyn_cast<EntityAI>(o);
        if (!ai || ai->IsDammageDestroyed())
            continue;
        std::string f = AiFields(ai);
        std::string& last = s.aiLast[o];
        if (f == last)
            continue;
        last = f;
        if (!On("ai"))
            continue;
        Ev e("ai");
        e.Obj("unit", o);
        e.Buffer() += ',';
        e.Buffer() += f;
        if (AIUnit* unit = ai->CommanderUnit())
        {
            // no wanted position is stored as a far-off sentinel: report it as null
            const float wd = unit->GetWantedPosition().Distance(o->Position());
            if (wd < 1e6f)
                e.Num("wantDist", wd);
            else
                e.Raw("wantDist", "null");
            if (unit->GetPath().Size() > 0)
                e.Vec("pathEnd", unit->GetPath().End());
        }
    }
}

void PollUnits(double now)
{
    Pollers& s = P();
    // the tracked list: every AI unit that is alive and not the player
    ForEachUnit(
        [&](AIUnit* u)
        {
            EntityAI* v = u->GetVehicle();
            if (!v || v->IsDammageDestroyed() || u->IsPlayer())
                return;
            for (UnitTrack& t : s.units)
                if (t.obj.GetLink() == v)
                {
                    t.seenT = now;
                    return;
                }
            UnitTrack t;
            t.obj = OLink<Object>(v);
            t.seenT = now;
            s.units.push_back(t);
        });
    s.units.erase(std::remove_if(s.units.begin(), s.units.end(),
                                 [now](const UnitTrack& t) { return !t.obj.GetLink() || t.seenT < now - 0.5; }),
                  s.units.end());

    for (UnitTrack& t : s.units)
    {
        Object* o = t.obj.GetLink();
        EntityAI* ai = dyn_cast<EntityAI>(o);
        AIUnit* unit = ai ? ai->CommanderUnit() : nullptr;
        if (!unit)
            continue;
        const Vector3 pos = o->Position();
        const Vector3 dir = o->Direction();
        // a unit "wants to move" only with a real destination (no wanted position is a far-off sentinel)
        // and either a path or a planning state; a unit waiting at a waypoint is not stuck
        const float wantDist = unit->GetWantedPosition().Distance(pos);
        const bool realWant = unit->GetWantedPosition().SquareSize() > 1.0f && wantDist > 3.0f && wantDist < 1e6f;
        const AIUnit::State st = unit->GetState();
        const bool planning = st == AIUnit::Busy || st == AIUnit::Init || st == AIUnit::Replan;
        StuckSample sample;
        sample.x = pos.X(), sample.y = pos.Y(), sample.z = pos.Z();
        sample.dirX = dir.X(), sample.dirZ = dir.Z();
        sample.wantsToMove =
            (unit->GetAIDisabled() & AIUnit::DAMove) == 0 && realWant && (unit->GetPath().Size() > 0 || planning);
        double sec = 0;
        const unsigned ev = StuckUpdate(t.track, sample, now, s.stuckSec, &sec);
        if (ev & StuckEventSpin)
        {
            if (On("stuck"))
            {
                Ev e("spin");
                e.Obj("unit", o).Vec("pos", pos).Num("turned", t.track.turned * (180.0f / 3.14159265f)).Num("sec", sec);
                e.Str("move", MoveOf(o).c_str());
            }
            if (DrawEnabled())
                AddDraw(2, pos + VUp * 2.5f, pos, 0, 0xff8000ff, 5, "SPIN");
        }
        if (ev & StuckEventStuck)
        {
            if (On("stuck"))
            {
                Ev e("stuck");
                e.Obj("unit", o).Vec("pos", pos).Num("sec", sec);
                e.Num("wantDist", wantDist).Vec("want", unit->GetWantedPosition());
                e.Int("path", unit->GetPath().Size());
                e.Str("state", EnumName(kStateNames, static_cast<int>(st)));
                e.Str("move", MoveOf(o).c_str());
            }
            if (DrawEnabled())
                AddDraw(2, pos + VUp * 2.5f, pos, 0, 0xff8000ff, 5, "STUCK");
        }
    }
}

void PollCorpses(double now)
{
    Pollers& s = P();
    for (auto it = s.corpses.begin(); it != s.corpses.end();)
    {
        Object* o = it->obj.GetLink();
        if (!o)
        {
            it = s.corpses.erase(it);
            continue;
        }
        if (now - it->deathT < 6.0)
        {
            ++it;
            continue;
        }
        if (On("death"))
        {
            const Vector3 pos = o->Position();
            const float ground = GroundY(pos);
            Ev e("corpse");
            e.Obj("unit", o).Vec("pos", pos).Num("ground", ground).Num("above", pos.Y() - ground);
            e.Str("move", MoveOf(o).c_str());
            if (Entity* ent = dyn_cast<Entity>(o))
                e.Num("speed", ent->Speed().Size());
            e.Num("tilt", std::acos(std::clamp(o->DirectionUp().Y(), -1.0f, 1.0f)) * (180.0f / 3.14159265f));
            e.Num("sec", now - it->deathT);
        }
        it = s.corpses.erase(it);
    }
}

void TickTrails(double realNow)
{
    Pollers& s = P();
    if (realNow < s.trailNext)
        return;
    s.trailNext = realNow + 0.05; // poll often, sample on mission time
    const float mission = static_cast<float>(MissionTimeNow());
    const std::vector<Object*> watched = Watched();
    s.trails.erase(std::remove_if(s.trails.begin(), s.trails.end(),
                                  [&](const Trail& t)
                                  {
                                      Object* o = t.obj.GetLink();
                                      return !o || std::find(watched.begin(), watched.end(), o) == watched.end();
                                  }),
                   s.trails.end());
    for (Object* o : watched)
    {
        Man* m = dyn_cast<Man>(o);
        if (!m)
            continue;
        Trail* tr = nullptr;
        for (Trail& t : s.trails)
            if (t.obj.GetLink() == o)
                tr = &t;
        if (!tr)
        {
            s.trails.push_back(Trail{});
            tr = &s.trails.back();
            tr->obj = o;
        }
        if (tr->lastMission >= 0 && mission - tr->lastMission < 0.25f)
            continue;
        tr->lastMission = mission;
        tr->pts.push_back(m->Position());
        while (tr->pts.size() > kTrailMax)
            tr->pts.pop_front();
        if (On("trail"))
        {
            Ev e("trail");
            e.Obj("unit", m).Str("move", MoveNameOf(m).c_str()).Num("speed", m->Speed().Size());
        }
    }
}

// crash notify (called from the crash filter after the minidump: keep it short)
void OnCrashNotify(unsigned code, const void* addr, const char* dump, const char* stack)
{
    if (!Enabled())
        return;
    {
        Ev e("crash");
        char b[32];
        std::snprintf(b, sizeof(b), "0x%08X", code);
        e.Str("code", b);
        std::snprintf(b, sizeof(b), "%p", addr);
        e.Str("addr", b).Str("dump", dump).Str("stack", stack);
    }
    Shutdown(); // writes summary.json and closes events.jsonl
}

// --diag-fixed-dt: Oli's lockstep pacing (GameLoop.cpp LockstepDeltaT) reads POSEIDON_LOCKSTEP_HZ once
void ApplyFixedDt(float dt)
{
    if (dt <= 0)
        return;
    const char* have = std::getenv("POSEIDON_LOCKSTEP_HZ");
    if (have && have[0])
    {
        LOG_INFO(Core, "diag: --diag-fixed-dt ignored, POSEIDON_LOCKSTEP_HZ={} is already set", have);
        return;
    }
    char hz[32];
    std::snprintf(hz, sizeof(hz), "%.6g", 1.0 / dt);
#ifdef _WIN32
    _putenv_s("POSEIDON_LOCKSTEP_HZ", hz);
#else
    setenv("POSEIDON_LOCKSTEP_HZ", hz, 1);
#endif
    if (!Poseidon::LockstepPacingActive())
        LOG_WARN(Core,
                 "diag: --diag-fixed-dt {} came too late, lockstep pacing was already read; set "
                 "POSEIDON_LOCKSTEP_HZ={} instead",
                 dt, hz);
    else
        LOG_INFO(Core, "diag: fixed dt {} s (POSEIDON_LOCKSTEP_HZ={})", dt, hz);
}
} // namespace

// ================================================================================================
namespace Internal
{
void InitPollers()
{
    Pollers& s = P();
    const AppConfig& cfg = AppConfig::Instance();
    s.seed = cfg.DiagSeed();
    s.stuckSec = cfg.DiagStuckSec() > 0 ? cfg.DiagStuckSec() : 5.0f;
    ApplyFixedDt(cfg.DiagFixedDt());
    Poseidon::Foundation::SetCrashNotify(&OnCrashNotify);
    {
        Ev e("settings");
        e.Int("seed", s.seed).Num("fixedDt", cfg.DiagFixedDt()).Num("stuckSec", s.stuckSec);
        e.Bool("lockstep", Poseidon::LockstepPacingActive());
    }
}

void ShutdownPollers()
{
    Pollers& s = P();
    Poseidon::Foundation::SetCrashNotify(nullptr);
    // per-file open counts, most opened first
    std::vector<std::pair<std::string, int>> v;
    {
        std::lock_guard<std::mutex> lock(s.soundMutex);
        for (auto& kv : s.sounds)
            v.push_back({kv.first, kv.second.second});
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!v.empty() && On("sound"))
    {
        std::string arr = "[";
        for (size_t i = 0; i < v.size() && i < 40; i++)
        {
            if (i)
                arr += ',';
            arr += "[" + Json::Quote(v[i].first.c_str()) + "," + std::to_string(v[i].second) + "]";
        }
        arr += ']';
        Ev e("sound_stats");
        e.Int("files", static_cast<long long>(v.size())).Raw("top", arr.c_str());
    }
}

void TickPollers(double now)
{
    Pollers& s = P();
    if (now >= s.aiNext)
    {
        s.aiNext = now + 0.25;
        PollAi();
        PollCorpses(now);
    }
    if (now >= s.unitsNext)
    {
        s.unitsNext = now + 1.0;
        if (On("stuck"))
            PollUnits(now);
    }
    TickTrails(now);
}

void OnDeathTracked(EntityAI* victim)
{
    if (!victim)
        return;
    CorpseTrack c;
    c.obj = OLink<Object>(victim);
    c.deathT = RealTimeNow();
    P().corpses.push_back(c);
}

bool Corpse(const Object* obj)
{
    for (const CorpseTrack& c : P().corpses)
        if (c.obj.GetLink() == obj)
            return true;
    return false;
}

void SetStuckSeconds(float sec)
{
    if (sec > 0)
        P().stuckSec = sec;
}

float StuckSeconds()
{
    return P().stuckSec;
}

const std::deque<Vector3>* TrailOf(const Object* obj)
{
    for (const Trail& t : P().trails)
        if (t.obj.GetLink() == obj)
            return &t.pts;
    return nullptr;
}
} // namespace Internal

// ================================================================================================
// hooks

void OnSoundOpen(const char* file, bool is3D, bool ok)
{
    if (!file || !On("sound"))
        return;
    Pollers& s = P();
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(s.soundMutex);
        const double now = RealTimeNow();
        auto& e = s.sounds[file];
        e.second++;
        if (e.second > 1 && now - e.first < 2.0)
            return;
        e.first = now;
        n = e.second;
    }
    Ev ev("sound_play");
    ev.Str("file", file).Bool("is3D", is3D).Bool("ok", ok).Int("n", n);
}

void OnMissionStarted()
{
    Pollers& s = P();
    if (s.seeded || s.seed < 0)
        return;
    s.seeded = true;
    GRandGen.SetSeed(s.seed);
    std::srand(static_cast<unsigned>(s.seed));
    Mark(("seed " + std::to_string(s.seed)).c_str());
}

} // namespace Poseidon::Dev::OpDiag

#endif // POSEIDON_DIAG
