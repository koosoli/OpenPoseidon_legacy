// DIAG-001 core: event log, frame stats, projectile tracking, damage bracket, time control, watched units.
// Compiled only with POSEIDON_DIAG=1 (see OpDiag.hpp).
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source), engine/Poseidon/Dev/Diag/MwDiag.cpp.
// Not ported: the Cry of Fear parts (undead health pool in "hit", melee sweep / hit / miss / melee_stuck
// events) -- this engine has no undead or melee. Time control was rewritten to drive Oli's dev pause
// (Dev/Diag/DiagPause.hpp) and fixed-step simulation instead of a second World::SetSimulationEnabled pause.
//
// events.jsonl - every line has "ev" (type), "t" (mission time, s), "rt" (real time since start, s),
// "f" (frame number). Types and their fields:
//   start      dir, cats, draw
//   boot       ok, mission, errorLevel, reason (the problems recorded while loading, when it failed)
//   addon      entry, owner              class used by the mission but not in its addOns[]
//   script     pos, error                SQF/SQS error
//   sound      what, name                a sound that could not be found or opened
//   sound_play file, is3D, ok, n         (OpDiagPollers.cpp) a wave was opened
//   shot       id, by, ammo, pos, vel, speed, elev, heading, target, targetDist, weapon
//   impact     id, by, ammo, hit, pos, dir, dist, flight, path   projectile hit an object or the ground
//   shot_end   id, by, ammo, reason, pos, dist, path             projectile gone without an impact
//   hit        target, by, ammo, direct, pos, dist, val, before, after, delta, zones{}, dead
//   death      unit, killer, dist, move
//   move       unit, from, to            watched units and corpses (diag_watch), or every Man with "moveall"
//   frame_slow ms, phases{}, draws, vd
//   perf       every real second: fps, avg, max, p95, draws, vd, vehicles, fast, units, groups, shots, paused
//   mark       text                      from diag_mark (and pause / resume / step / seed)
//   exit       frames
// OpDiagPollers.cpp adds ai, stuck, spin, corpse, trail, sound_stats and crash.

#include <Poseidon/Dev/Diag/OpDiag.hpp>
#include <Poseidon/Dev/Diag/OpDiagInternal.hpp>

#if POSEIDON_DIAG

#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace Poseidon::Dev::OpDiag
{
using Poseidon::Foundation::AppConfig;

namespace
{
using Clock = std::chrono::steady_clock;

struct State
{
    bool enabled = false;
    bool draw = false;
    bool keepGoing = false;
    bool allCats = true;
    std::set<std::string> cats;
    float slowMs = 40;
    std::string dir;
    std::FILE* events = nullptr;
    std::mutex mutex;
    Clock::time_point start = Clock::now();
    long long frame = 0;
    std::map<std::string, long long> counts;

    // time control: Dev::SetDiagPause is the pause; stepTicks are released one per frame through it
    int stepTicks = 0;

    // per-second frame stats
    double secStart = 0;
    int secFrames = 0;
    double secSumMs = 0;
    float secMaxMs = 0;
    long long secDraws = 0;
    std::vector<float> secMs;
    // whole-run frame stats (summary.json)
    long long runFrames = 0;
    double runSumMs = 0;
    float runMaxMs = 0;
    long long runSlow = 0;
};

State& S()
{
    static State s;
    return s;
}

double RealTime()
{
    return std::chrono::duration<double>(Clock::now() - S().start).count();
}

double MissionTime()
{
    return Glob.time.toFloat();
}

void WriteLine(const std::string& line)
{
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.events)
        return;
    std::fwrite(line.data(), 1, line.size(), s.events);
    std::fputc('\n', s.events);
    std::fflush(s.events); // the process may end with ExitProcess (no destructors)
}

void AppendVec(std::string& out, Vector3Par v)
{
    Json::AppendPos(out, v.X(), v.Y(), v.Z());
}

// ---- projectiles --------------------------------------------------------------------------------
struct ShotTrack
{
    OLink<Object> link;
    const Shot* raw = nullptr;
    int id = 0;
    std::string by;
    std::string ammo;
    Vector3 start = VZero;
    double startT = 0;
    std::vector<Vector3> path;
};
std::vector<ShotTrack> GShots;
int GNextShotId = 1;
const int kMaxPath = 48;

ShotTrack* FindShot(const Shot* shot)
{
    for (ShotTrack& t : GShots)
        if (t.raw == shot && t.link.GetLink() == shot)
            return &t;
    return nullptr;
}

void AppendPath(std::string& out, const std::vector<Vector3>& path)
{
    out += '[';
    for (size_t i = 0; i < path.size(); i++)
    {
        if (i)
            out += ',';
        AppendVec(out, path[i]);
    }
    out += ']';
}

void AddPathPoint(ShotTrack& t, Vector3Par p)
{
    if (!t.path.empty() && t.path.back().Distance2(p) < 0.01f)
        return;
    t.path.push_back(p);
    if (static_cast<int>(t.path.size()) > kMaxPath)
    {
        // keep the first point, thin out the rest
        std::vector<Vector3> thin;
        thin.push_back(t.path.front());
        for (size_t i = 2; i < t.path.size(); i += 2)
            thin.push_back(t.path[i]);
        if (thin.back().Distance2(t.path.back()) > 0.01f)
            thin.push_back(t.path.back());
        t.path.swap(thin);
    }
}

// ---- 3D overlay store ---------------------------------------------------------------------------
struct DrawItem
{
    int kind; // 0 line, 1 cross, 2 text
    Vector3 a, b;
    float size;
    uint32_t rgba;
    double until;
    std::string text;
};
std::vector<DrawItem> GDraw;
const size_t kMaxDraw = 4000;

void PushDraw(DrawItem&& it)
{
    if (!S().draw)
        return;
    if (GDraw.size() >= kMaxDraw)
        GDraw.erase(GDraw.begin(), GDraw.begin() + kMaxDraw / 4);
    GDraw.push_back(std::move(it));
}

// ---- watched units --------------------------------------------------------------------------------
std::vector<OLink<Object>> GWatched;

// last primary move seen per man (OnMoveTick); pruned when the man is gone
struct MoveSeen
{
    OLink<Object> obj;
    int id = -1;
};
std::vector<MoveSeen> GMoveSeen;

} // namespace

// ================================================================================================
// lifetime

bool Start(const std::string& dir, const std::string& cats, bool draw, bool keepGoing, float slowMs)
{
    State& s = S();
    if (s.enabled || dir.empty())
        return s.enabled;
    s.dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(s.dir, ec);
    const std::string path = (std::filesystem::path(s.dir) / "events.jsonl").string();
    s.events = std::fopen(path.c_str(), "wb");
    if (!s.events)
    {
        LOG_ERROR(Core, "diag: cannot write {}", path);
        return false;
    }
    s.enabled = true;
    s.draw = draw;
    s.keepGoing = keepGoing;
    s.slowMs = slowMs > 0 ? slowMs : 40.0f;
    s.allCats = true;
    s.cats.clear();
    s.counts.clear();
    s.frame = s.runFrames = s.runSlow = 0;
    s.runSumMs = 0;
    s.runMaxMs = 0;
    s.secStart = 0;
    s.secFrames = 0;
    s.secMs.clear();
    if (!cats.empty() && cats != "all")
    {
        s.allCats = false;
        std::stringstream ss(cats);
        std::string item;
        while (std::getline(ss, item, ','))
            if (!item.empty())
                s.cats.insert(item);
    }
    s.start = Clock::now();
    LOG_INFO(Core, "diag: events -> {} (cats {}, draw {}, slow {} ms)", path, cats.empty() ? "all" : cats.c_str(),
             s.draw, s.slowMs);
    {
        Ev e("start");
        e.Str("dir", s.dir.c_str()).Str("cats", cats.empty() ? "all" : cats.c_str()).Bool("draw", s.draw);
    }
    return true;
}

void Init()
{
    const AppConfig& cfg = AppConfig::Instance();
    if (cfg.GetDiagDir().empty())
        return;
    if (!Start(cfg.GetDiagDir(), cfg.GetDiagCats(), cfg.DiagDraw(), cfg.DiagKeepGoing(), cfg.DiagSlowMs()))
        return;
    Internal::InitPollers();
}

void Shutdown()
{
    State& s = S();
    if (!s.enabled)
        return;
    Internal::ShutdownPollers();
    {
        Ev e("exit");
        e.Int("frames", s.runFrames);
    }
    std::string sum = "{";
    sum += "\"frames\":" + std::to_string(s.runFrames);
    sum += ",\"avgMs\":";
    Json::AppendNum(sum, s.runFrames ? s.runSumMs / static_cast<double>(s.runFrames) : 0, "%.3f");
    sum += ",\"maxMs\":";
    Json::AppendNum(sum, s.runMaxMs, "%.3f");
    sum += ",\"slowFrames\":" + std::to_string(s.runSlow);
    sum += ",\"realSeconds\":";
    Json::AppendNum(sum, RealTime(), "%.1f");
    sum += ",\"events\":{";
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        bool first = true;
        for (auto& kv : s.counts)
        {
            if (!first)
                sum += ',';
            first = false;
            Json::AppendString(sum, kv.first.c_str());
            sum += ':' + std::to_string(kv.second);
        }
    }
    sum += "}}\n";
    const std::string path = (std::filesystem::path(s.dir) / "summary.json").string();
    if (std::FILE* f = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(sum.data(), 1, sum.size(), f);
        std::fclose(f);
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.events)
    {
        std::fclose(s.events);
        s.events = nullptr;
    }
    s.enabled = false;
}

bool Enabled()
{
    return S().enabled;
}

bool On(const char* cat)
{
    State& s = S();
    if (!s.enabled)
        return false;
    if (s.allCats)
        return std::strcmp(cat, "moveall") != 0; // only when asked for
    return s.cats.count(cat) > 0;
}

bool DrawEnabled()
{
    return S().enabled && S().draw;
}

void SetDraw(bool on)
{
    S().draw = on;
}

bool KeepGoingOnScriptError()
{
    return S().enabled && S().keepGoing;
}

// ================================================================================================
// event writer

std::string ClassName(const Object* obj)
{
    if (!obj)
        return std::string();
    const Entity* ent = dyn_cast<Entity>(const_cast<Object*>(obj));
    if (ent && ent->GetNonAIType())
        return std::string(static_cast<const char*>(ent->GetNonAIType()->GetName()));
    if (obj->GetShape())
        return std::string(static_cast<const char*>(obj->GetShape()->Name()));
    return std::string();
}

std::string ObjName(const Object* obj)
{
    if (!obj)
        return std::string();
    RString var = obj->GetVarName();
    if (var.GetLength() > 0)
        return std::string(static_cast<const char*>(var));
    RString dbg = obj->GetDebugName();
    if (dbg.GetLength() > 0)
        return std::string(static_cast<const char*>(dbg));
    return ClassName(obj);
}

Ev::Ev(const char* type) : _active(S().enabled)
{
    if (!_active)
        return;
    State& s = S();
    {
        std::lock_guard<std::mutex> lock(s.mutex); // sound failures can come from the audio loader thread
        s.counts[type]++;
    }
    _buf.reserve(256);
    _buf += "{\"ev\":";
    Json::AppendString(_buf, type);
    _buf += ",\"t\":";
    Json::AppendNum(_buf, MissionTime(), "%.3f");
    _buf += ",\"rt\":";
    Json::AppendNum(_buf, RealTime(), "%.3f");
    _buf += ",\"f\":" + std::to_string(s.frame);
}

Ev::~Ev()
{
    if (!_active)
        return;
    _buf += '}';
    WriteLine(_buf);
}

void Ev::Key(const char* key)
{
    _buf += ',';
    Json::AppendString(_buf, key);
    _buf += ':';
}

Ev& Ev::Str(const char* key, const char* value)
{
    if (!_active)
        return *this;
    Key(key);
    Json::AppendString(_buf, value);
    return *this;
}

Ev& Ev::Num(const char* key, double value)
{
    if (!_active)
        return *this;
    Key(key);
    Json::AppendNum(_buf, value);
    return *this;
}

Ev& Ev::Int(const char* key, long long value)
{
    if (!_active)
        return *this;
    Key(key);
    _buf += std::to_string(value);
    return *this;
}

Ev& Ev::Bool(const char* key, bool value)
{
    if (!_active)
        return *this;
    Key(key);
    _buf += value ? "true" : "false";
    return *this;
}

Ev& Ev::Vec(const char* key, Vector3Par v)
{
    if (!_active)
        return *this;
    Key(key);
    AppendVec(_buf, v);
    return *this;
}

Ev& Ev::Obj(const char* key, const Object* obj)
{
    if (!_active)
        return *this;
    Key(key);
    if (!obj)
    {
        _buf += "null";
        return *this;
    }
    _buf += "{\"name\":";
    Json::AppendString(_buf, ObjName(obj).c_str());
    _buf += ",\"cls\":";
    Json::AppendString(_buf, ClassName(obj).c_str());
    _buf += ",\"pos\":";
    AppendVec(_buf, obj->Position());
    _buf += '}';
    return *this;
}

Ev& Ev::Raw(const char* key, const char* json)
{
    if (!_active)
        return *this;
    Key(key);
    _buf += json && json[0] ? json : "null";
    return *this;
}

// ================================================================================================
// hooks

namespace
{
// problems seen so far, for the boot failure reason (kept even without --diag)
std::mutex GProblemMutex;
std::vector<std::string> GProblems;
int GProblemCount = 0;

void NoteProblem(const std::string& p)
{
    std::lock_guard<std::mutex> lock(GProblemMutex);
    GProblemCount++;
    for (const std::string& q : GProblems)
        if (q == p)
            return;
    if (GProblems.size() < 12)
        GProblems.push_back(p);
}
} // namespace

void OnBoot(bool ok, const char* mission, int errorLevel)
{
    if (ok)
        OnMissionStarted(); // --diag-seed
    std::string reason;
    if (!ok)
    {
        std::lock_guard<std::mutex> lock(GProblemMutex);
        reason =
            "error level " + std::to_string(errorLevel) + (errorLevel >= 2 ? " (EMError: a load step failed)" : "");
        if (GProblems.empty())
            reason += "; no addon, script or sound problem recorded";
        else
        {
            reason += "; " + std::to_string(GProblemCount) + " problem(s), first: ";
            for (size_t i = 0; i < GProblems.size(); i++)
                reason += (i ? " | " : "") + GProblems[i];
        }
        if (Enabled())
            LOG_ERROR(Core, "diag: mission failed to load: {}", reason);
    }
    if (!On("boot"))
        return;
    Ev e("boot");
    e.Bool("ok", ok).Str("mission", mission);
    if (!ok)
        e.Int("errorLevel", errorLevel).Str("reason", reason.c_str());
}

void OnAddonMissing(const char* entry, const char* owner)
{
    NoteProblem(std::string("addon class not activated: ") + (entry ? entry : "") + " (addon " + (owner ? owner : "") +
                ", add it to the mission's addOns[])");
    if (!On("addon"))
        return;
    Ev e("addon");
    e.Str("entry", entry).Str("owner", owner);
}

void OnScriptError(const char* position, const char* error)
{
    NoteProblem(std::string("script error: ") + (error ? error : ""));
    if (!On("script"))
        return;
    Ev e("script");
    e.Str("pos", position).Str("error", error);
}

void OnSoundMissing(const char* what, const char* name)
{
    NoteProblem(std::string("sound ") + (what ? what : "") + " failed: " + (name ? name : ""));
    if (!On("sound"))
        return;
    Ev e("sound");
    e.Str("what", what).Str("name", name);
}

void OnDeath(EntityAI* victim, EntityAI* killer)
{
    if (!Enabled())
        return;
    if (On("death"))
    {
        Ev e("death");
        e.Obj("unit", victim).Obj("killer", killer);
        if (victim && killer && killer != victim)
            e.Num("dist", victim->Position().Distance(killer->Position()));
        if (Man* m = dyn_cast<Man>(victim))
            e.Str("move", Internal::MoveNameOf(m).c_str());
    }
    Internal::OnDeathTracked(victim);
    if (DrawEnabled() && victim)
        DrawLabel(victim->Position() + VUp * 2.0f, ("DEAD " + ObjName(victim)).c_str(), 0xff3030ff, 8);
}

void OnMoveTick(Man* man, int primaryMoveId)
{
    if (!man || !Enabled())
        return;
    const bool followed = Watching(man) || Internal::Corpse(man);
    if (!(followed ? On("move") : On("moveall")))
        return;
    MoveSeen* seen = nullptr;
    for (MoveSeen& m : GMoveSeen)
        if (m.obj.GetLink() == man)
            seen = &m;
    if (!seen)
    {
        GMoveSeen.erase(std::remove_if(GMoveSeen.begin(), GMoveSeen.end(),
                                       [](const MoveSeen& m) { return m.obj.GetLink() == nullptr; }),
                        GMoveSeen.end());
        GMoveSeen.push_back(MoveSeen{OLink<Object>(man), -1});
        seen = &GMoveSeen.back();
    }
    if (seen->id == primaryMoveId)
        return;
    const int prev = seen->id;
    seen->id = primaryMoveId;
    const ManType* type = man->Type();
    RStringB from = prev >= 0 && type ? type->GetMoveName(static_cast<MoveId>(prev)) : RStringB("");
    RStringB to = primaryMoveId >= 0 && type ? type->GetMoveName(static_cast<MoveId>(primaryMoveId)) : RStringB("");
    Ev e("move");
    e.Obj("unit", man).Str("from", static_cast<const char*>(from)).Str("to", static_cast<const char*>(to));
}

void OnShotStep(Shot* shot)
{
    if (!shot || !(On("shot") || DrawEnabled()))
        return;
    ShotTrack* t = FindShot(shot);
    if (!t)
    {
        // new projectile (first simulation step): drop tracks whose shot is gone
        GShots.erase(std::remove_if(GShots.begin(), GShots.end(),
                                    [](const ShotTrack& x) { return x.link.GetLink() == nullptr; }),
                     GShots.end());
        ShotTrack nt;
        nt.link = OLink<Object>(shot);
        nt.raw = shot;
        nt.id = GNextShotId++;
        nt.by = ObjName(shot->GetOwner());
        nt.ammo = shot->Type() ? std::string(static_cast<const char*>(shot->Type()->GetName())) : std::string();
        nt.start = shot->Position();
        nt.startT = MissionTime();
        GShots.push_back(std::move(nt));
        t = &GShots.back();
        if (On("shot"))
        {
            Ev e("shot");
            e.Int("id", t->id).Str("by", t->by.c_str()).Str("ammo", t->ammo.c_str()).Vec("pos", shot->Position());
            e.Vec("vel", shot->Speed());
            const Vector3 v = shot->Speed();
            const float sp = v.Size();
            e.Num("speed", sp);
            if (sp > 1e-3f)
            {
                // elevation above the horizontal and compass heading of the launch: a bad aim shows at once
                const float elev = std::asin(std::clamp(v.Y() / sp, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
                const float head = std::atan2(v.X(), v.Z()) * (180.0f / 3.14159265f);
                e.Num("elev", elev).Num("heading", head < 0 ? head + 360 : head);
            }
            if (EntityAI* owner = shot->GetOwner())
            {
                Target* tgt = owner->GetFireTarget();
                if (tgt && tgt->idExact)
                {
                    e.Str("target", ObjName(tgt->idExact).c_str());
                    e.Num("targetDist", tgt->idExact->Position().Distance(shot->Position()));
                }
                else
                    e.Raw("target", "null");
                const int sel = owner->SelectedWeapon();
                if (sel >= 0 && sel < owner->NMagazineSlots())
                {
                    const MagazineSlot& slot = owner->GetMagazineSlot(sel);
                    e.Str("weapon", slot._weapon ? static_cast<const char*>(slot._weapon->GetName()) : "");
                }
            }
        }
    }
    if (DrawEnabled() && !t->path.empty())
        DrawLine(t->path.back(), shot->Position(), 0xffe040c0, 3);
    AddPathPoint(*t, shot->Position());
}

void OnImpact(EntityAI* owner, Shot* shot, Object* directHit, Vector3Par pos, Vector3Par dir, const AmmoType* ammo)
{
    if (!Enabled())
        return;
    ShotTrack* t = shot ? FindShot(shot) : nullptr;
    if (t)
    {
        if (DrawEnabled() && !t->path.empty())
            DrawLine(t->path.back(), pos, 0xffe040c0, 3);
        AddPathPoint(*t, pos);
    }
    if (DrawEnabled())
        DrawCross(pos, directHit ? 0.3f : 0.2f, directHit ? 0xff3030ff : 0xffa0a0a0, 5);
    if (On("shot"))
    {
        Ev e("impact");
        e.Int("id", t ? t->id : 0).Obj("by", owner);
        e.Str("ammo", ammo ? static_cast<const char*>(ammo->GetName()) : "");
        e.Obj("hit", directHit).Vec("pos", pos).Vec("dir", dir);
        if (t)
        {
            e.Num("dist", t->start.Distance(pos));
            e.Num("flight", MissionTime() - t->startT);
            std::string path;
            AppendPath(path, t->path);
            e.Raw("path", path.c_str());
        }
    }
    if (t)
        t->raw = nullptr; // ended: a later step of the same pointer starts a new track
}

void OnShotEnd(Shot* shot, const char* reason)
{
    if (!shot || !Enabled())
        return;
    ShotTrack* t = FindShot(shot);
    if (!t)
        return;
    AddPathPoint(*t, shot->Position());
    if (On("shot"))
    {
        Ev e("shot_end");
        e.Int("id", t->id).Str("by", t->by.c_str()).Str("ammo", t->ammo.c_str()).Str("reason", reason);
        e.Vec("pos", shot->Position()).Num("dist", t->start.Distance(shot->Position()));
        std::string path;
        AppendPath(path, t->path);
        e.Raw("path", path.c_str());
    }
    t->raw = nullptr;
}

void OnFrameEnd(float totalMs, const float* phaseMs, int phaseCount, int drawCalls, float viewDistance)
{
    Internal::TickCamera(); // diag_camera follows with or without --diag
    State& s = S();
    if (!s.enabled)
        return;
    s.frame++;
    s.runFrames++;
    s.runSumMs += totalMs;
    s.runMaxMs = std::max(s.runMaxMs, totalMs);
    s.secFrames++;
    s.secSumMs += totalMs;
    s.secMaxMs = std::max(s.secMaxMs, totalMs);
    s.secDraws += drawCalls;
    s.secMs.push_back(totalMs);

    if (totalMs >= s.slowMs)
    {
        s.runSlow++;
        if (On("frame"))
        {
            std::string ph = "{";
            for (int i = 0; i < phaseCount; i++)
            {
                if (i)
                    ph += ',';
                Json::AppendString(ph, FrameProfiler::PhaseName(i));
                ph += ':';
                Json::AppendNum(ph, phaseMs[i], "%.2f");
            }
            ph += '}';
            Ev e("frame_slow");
            e.Num("ms", totalMs).Raw("phases", ph.c_str()).Int("draws", drawCalls).Num("vd", viewDistance);
        }
    }
    const double now = RealTime();
    Internal::TickPollers(now);

    if (now - s.secStart >= 1.0)
    {
        if (On("perf") && s.secFrames > 0)
        {
            std::vector<float> v = s.secMs;
            std::sort(v.begin(), v.end());
            const float p95 = v[std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.95))];
            int units = 0, groups = 0;
            if (GWorld)
            {
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
                        groups++;
                        units += grp->NUnits();
                    }
                }
            }
            int shots = 0;
            for (const ShotTrack& t : GShots)
                if (t.raw && t.link.GetLink())
                    shots++;
            Ev e("perf");
            e.Num("fps", s.secFrames / (now - s.secStart)).Num("avg", s.secSumMs / s.secFrames).Num("max", s.secMaxMs);
            e.Num("p95", p95).Num("draws", static_cast<double>(s.secDraws) / s.secFrames).Num("vd", viewDistance);
            e.Int("vehicles", GWorld ? GWorld->NVehicles() : 0).Int("fast", GWorld ? GWorld->NFastVehicles() : 0);
            e.Int("units", units).Int("groups", groups).Int("shots", shots);
            e.Bool("paused", Paused());
        }
        s.secStart = now;
        s.secFrames = 0;
        s.secSumMs = 0;
        s.secMaxMs = 0;
        s.secDraws = 0;
        s.secMs.clear();
    }
}

void Mark(const char* text)
{
    if (!Enabled())
        return;
    Ev e("mark");
    e.Str("text", text);
}

// ================================================================================================
// damage bracket

struct DamageScope::Impl
{
    OLink<Object> target;
    OLink<EntityAI> owner;
    std::string ammo;
    Vector3 pos;
    float val, valRange;
    float before;
    std::vector<float> hits;
};

namespace
{
int GDamageDepth = 0;

void SnapshotHits(Object* obj, std::vector<float>& out)
{
    out.clear();
    EntityAI* ai = dyn_cast<EntityAI>(obj);
    if (!ai)
        return;
    const HitPointList& list = ai->GetType()->GetHitPoints();
    for (int i = 0; i < list.Size(); i++)
        out.push_back(ai->GetHit(*list[i]));
}
} // namespace

DamageScope::DamageScope(Object* target, EntityAI* owner, const char* ammo, Vector3Par worldPos, float val,
                         float valRange)
    : _impl(nullptr)
{
    GDamageDepth++;
    if (GDamageDepth != 1 || !target || !(On("hit") || DrawEnabled()))
        return;
    _impl = new Impl;
    _impl->target = OLink<Object>(target);
    _impl->owner = OLink<EntityAI>(owner);
    _impl->ammo = ammo ? ammo : "";
    _impl->pos = worldPos;
    _impl->val = val;
    _impl->valRange = valRange;
    _impl->before = target->GetTotalDammage();
    SnapshotHits(target, _impl->hits);
}

DamageScope::~DamageScope()
{
    GDamageDepth--;
    if (!_impl)
        return;
    Object* target = _impl->target.GetLink();
    if (target)
    {
        const float after = target->GetTotalDammage();
        if (DrawEnabled() && after > _impl->before)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.2f", after - _impl->before);
            DrawLabel(_impl->pos, buf, 0xff60c0ff, 4);
        }
        if (On("hit"))
        {
            EntityAI* owner = _impl->owner.GetLink();
            std::string zones = "{";
            if (EntityAI* ai = dyn_cast<EntityAI>(target))
            {
                const HitPointList& list = ai->GetType()->GetHitPoints();
                bool first = true;
                for (int i = 0; i < list.Size() && i < static_cast<int>(_impl->hits.size()); i++)
                {
                    const float d = ai->GetHit(*list[i]) - _impl->hits[i];
                    if (std::fabs(d) < 1e-4f)
                        continue;
                    if (!first)
                        zones += ',';
                    first = false;
                    Json::AppendString(zones, static_cast<const char*>(ai->HitpointName(i)));
                    zones += ':';
                    Json::AppendNum(zones, d, "%.3f");
                }
            }
            zones += '}';
            Ev e("hit");
            e.Obj("target", target).Obj("by", owner).Str("ammo", _impl->ammo.c_str());
            e.Bool("direct", _impl->valRange < 0).Vec("pos", _impl->pos);
            if (owner)
                e.Num("dist", owner->Position().Distance(target->Position()));
            e.Num("val", _impl->val).Num("before", _impl->before).Num("after", after);
            e.Num("delta", after - _impl->before).Raw("zones", zones.c_str());
            e.Bool("dead", target->IsDammageDestroyed());
        }
    }
    delete _impl;
}

// ================================================================================================
// watched units

bool Watching(const Object* obj)
{
    if (!obj || GWatched.empty())
        return false;
    for (const OLink<Object>& w : GWatched)
        if (w.GetLink() == obj)
            return true;
    return false;
}

void Watch(Object* obj, bool on)
{
    if (!obj)
        return;
    GWatched.erase(std::remove_if(GWatched.begin(), GWatched.end(),
                                  [obj](const OLink<Object>& w) { return !w.GetLink() || w.GetLink() == obj; }),
                   GWatched.end());
    if (on)
        GWatched.push_back(OLink<Object>(obj));
}

// ================================================================================================
// time control: the dev pause (DiagPause.hpp) is the pause; a step lets single fixed ticks through it

void Pause()
{
    S().stepTicks = 0;
    SetDiagPause(true);
    Mark("pause");
}

void Resume()
{
    S().stepTicks = 0;
    SetDiagPause(false);
    Mark("resume");
}

void Step(int ticks)
{
    State& s = S();
    SetDiagPause(true);
    s.stepTicks = std::max(0, ticks);
    if (Enabled())
    {
        Ev e("mark");
        e.Str("text", "step").Int("ticks", s.stepTicks);
    }
}

bool Paused()
{
    return DiagPauseActive();
}

bool BeginStepTick()
{
    State& s = S();
    if (s.stepTicks <= 0 || !DiagPauseActive())
        return false;
    s.stepTicks--;
    // straight to the flag, not SetDiagPause: that logs every toggle, and this is one tick
    g_diagPauseActive = false;
    return true;
}

void EndStepTick(bool stepped)
{
    if (stepped)
        g_diagPauseActive = true;
}

// ================================================================================================
// overlay store (drawing itself: OpDiagOverlay.cpp)

void DrawLine(Vector3Par a, Vector3Par b, uint32_t rgba, float ttlSec)
{
    PushDraw(DrawItem{0, a, b, 0, rgba, RealTime() + ttlSec, std::string()});
}

void DrawCross(Vector3Par p, float size, uint32_t rgba, float ttlSec)
{
    PushDraw(DrawItem{1, p, p, size, rgba, RealTime() + ttlSec, std::string()});
}

void DrawLabel(Vector3Par p, const char* text, uint32_t rgba, float ttlSec)
{
    PushDraw(DrawItem{2, p, p, 0, rgba, RealTime() + ttlSec, std::string(text ? text : "")});
}

namespace Internal
{
void ForEachDrawItem(void (*fn)(int kind, Vector3Par a, Vector3Par b, float size, uint32_t rgba, const char* text,
                                void* ctx),
                     void* ctx)
{
    const double now = RealTime();
    GDraw.erase(std::remove_if(GDraw.begin(), GDraw.end(), [now](const DrawItem& d) { return d.until < now; }),
                GDraw.end());
    for (const DrawItem& d : GDraw)
        fn(d.kind, d.a, d.b, d.size, d.rgba, d.text.c_str(), ctx);
}

std::vector<Object*> Watched()
{
    std::vector<Object*> out;
    for (const OLink<Object>& w : GWatched)
        if (Object* o = w.GetLink())
            out.push_back(o);
    return out;
}

double RealTimeNow()
{
    return RealTime();
}

double MissionTimeNow()
{
    return MissionTime();
}

long long FrameNo()
{
    return S().frame;
}

void Vec(std::string& out, Vector3Par v)
{
    AppendVec(out, v);
}

void AddDraw(int kind, Vector3Par a, Vector3Par b, float size, uint32_t rgba, float ttl, const char* text)
{
    PushDraw(DrawItem{kind, a, b, size, rgba, RealTime() + ttl, std::string(text ? text : "")});
}
} // namespace Internal

} // namespace Poseidon::Dev::OpDiag

#endif // POSEIDON_DIAG
