// DIAG-001 harness commands. Registered on the game's harness server (GameApplication CreateGameHarness)
// when the exe is a POSEIDON_DIAG build; they work with or without --diag (events need --diag).
// A "unit" argument is SQF evaluated to an object: a variable name, "player", "nearestObject [...]".
//
//   diag_inspect   {unit}                  one unit/object: position, damage, hit zones, weapons, animation, AI
//   diag_groups    {side?}                 every AI group: units alive, leader, spread, still units, modes
//   diag_near      {unit | pos:[x,z], r?}  objects within r m (default 15): class, shape, type, damage, LODs
//   diag_geometry  {unit, draw?, ttl?}     an object's LODs and convex components (geometry, fire, view),
//                                          drawn as boxes in the overlay with draw=true (needs --diag-draw)
//   diag_perf      {}                      frame time stats of the last frames (Dev::FrameProfiler), counters
//   diag_pause {}  diag_resume {}          the dev pause (Dev/Diag/DiagPause.hpp)
//   diag_step      {ticks? (1)}            run N fixed simulation ticks, one per frame, then stay paused
//   diag_watch     {unit, on?}             label the unit in the overlay, log its moves, AI changes, trail
//   diag_mark      {text}                  a marker line in events.jsonl
//   diag_draw      {on}                    overlay on/off at run time
//   diag_stuck     {sec}                   seconds without moving before a "stuck" event
//   diag_anim      {unit}                  current animation: move, RTM file, keyframes, step, speed, blend
//   diag_pose      {unit, move, time?, pause?}  put a man into a move at a fixed phase (0..1) and pause
//   diag_kill      {unit, move?}           kill the unit (setDammage 1), optionally forcing a move; corpse followed
//   diag_movecheck {cfg?}                  CfgMoves* check: RTM files exist, connectTo / interpolateTo / actions
//                                          name existing moves, death moves terminal, unreachable moves
//   diag_scripts   {}                      running SQS scripts: name, current line and its statement
//   diag_camera    (OpDiagCamera.cpp)
//
// Ported from Malprave (Dec's GPL fork of the same Poseidon source): MwDiagHarness.cpp and MwDiagHarness2.cpp.
// Not ported: diag_config_check (melee / undead config), diag_hitzone, diag_melee, diag_explode, diag_xray
// (Cry of Fear or renderer-specific), diag_render, diag_model, diag_skeleton, diag_vdsweep (left for a later
// round). The undead / stagger / melee parts of the Man inspector are gone with them.

#include <Poseidon/Dev/Diag/OpDiag.hpp>
#include <Poseidon/Dev/Diag/OpDiagInternal.hpp>

#if POSEIDON_DIAG

// GameStateExtCommon.hpp first: the harness headers pull in windows.h, whose GetObject macro
// (GetObjectA) would rename the SQF helper GetObject() declared there (as HarnessBuiltins.cpp does).
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/Game/Scripting/Scripts.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Evaluator/express.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef GetObject
#undef GetObject
#endif

namespace Poseidon::Dev::OpDiag
{
namespace
{
using J = Json::Object;

J& VecField(J& j, const char* key, Vector3Par v)
{
    return j.Pos(key, v.X(), v.Y(), v.Z());
}

GameVarSpace s_diagScope;

void ExecSqf(const std::string& code)
{
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs)
        return;
    gs->BeginContext(&s_diagScope);
    gs->Execute(code.c_str());
    gs->EndContext();
}

double NumArg(cJSON* root, const char* key, double def)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return v && cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool BoolArg(cJSON* root, const char* key, bool def)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!v)
        return def;
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valuedouble != 0;
    return def;
}

const char* StrArg(cJSON* root, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
    return v && cJSON_IsString(v) ? v->valuestring : nullptr;
}

bool GetPosArg(cJSON* root, Vector3& pos)
{
    cJSON* p = cJSON_GetObjectItemCaseSensitive(root, "pos");
    if (!p || !cJSON_IsArray(p) || cJSON_GetArraySize(p) < 2)
        return false;
    const float x = static_cast<float>(cJSON_GetArrayItem(p, 0)->valuedouble);
    const float z = static_cast<float>(cJSON_GetArrayItem(p, 1)->valuedouble);
    const float y = GLandscape ? GLandscape->SurfaceY(x, z) : 0;
    pos = Vector3(x, y, z);
    return true;
}

std::string ShapeOf(const Object* obj)
{
    return obj && obj->GetShape() ? std::string(obj->GetShape()->Name()) : std::string();
}

float DirDeg(Vector3Par d)
{
    const float a = std::atan2(d.X(), d.Z()) * (180.0f / 3.14159265f);
    return a < 0 ? a + 360 : a;
}

std::string Err(const std::string& what)
{
    return HarnessProtocol::ErrorResponse(what.c_str());
}

// ---- inspect -------------------------------------------------------------------------------------
std::string InspectEntity(Object* obj)
{
    J j;
    j.Str("name", ObjName(obj).c_str()).Str("cls", ClassName(obj).c_str()).Str("shape", ShapeOf(obj).c_str());
    VecField(j, "pos", obj->Position()).Num("dir", DirDeg(obj->Direction()), "%.1f");
    j.Num("damage", obj->GetTotalDammage(), "%.3f").Bool("destroyed", obj->IsDammageDestroyed());
    if (Entity* ent = dyn_cast<Entity>(obj))
        j.Num("speed", ent->Speed().Size(), "%.2f");
    j.Bool("watched", Watching(obj));
    if (EntityAI* ai = dyn_cast<EntityAI>(obj))
    {
        j.Int("side", static_cast<int>(ai->GetTargetSide()));
        std::string zones = "{";
        const HitPointList& list = ai->GetType()->GetHitPoints();
        for (int i = 0; i < list.Size(); i++)
        {
            if (i)
                zones += ',';
            Json::AppendString(zones, static_cast<const char*>(ai->HitpointName(i)));
            zones += ':';
            Json::AppendNum(zones, ai->GetHit(*list[i]), "%.3f");
        }
        zones += '}';
        j.Raw("zones", zones);
        std::string w = "[";
        for (int i = 0; i < ai->NWeaponSystems(); i++)
        {
            const WeaponType* wt = ai->GetWeaponSystem(i);
            if (!wt)
                continue;
            if (w.size() > 1)
                w += ',';
            Json::AppendString(w, static_cast<const char*>(wt->GetName()));
        }
        w += ']';
        j.Raw("weapons", w);
        const int sel = ai->SelectedWeapon();
        if (sel >= 0 && sel < ai->NMagazineSlots())
        {
            const MagazineSlot& slot = ai->GetMagazineSlot(sel);
            J s;
            s.Str("weapon", slot._weapon ? static_cast<const char*>(slot._weapon->GetName()) : "");
            s.Str("muzzle", slot._muzzle ? static_cast<const char*>(slot._muzzle->GetName()) : "");
            s.Bool("magazine", slot._magazine != nullptr);
            if (slot._magazine)
                s.Int("ammo", slot._magazine->_ammo);
            s.Bool("loaded", ai->GetWeaponLoaded(sel));
            j.Raw("selected", s.Done());
        }
        j.Int("magazines", ai->NMagazines());
        Target* tgt = ai->GetFireTarget();
        if (tgt && tgt->idExact)
            j.Str("fireTarget", ObjName(tgt->idExact).c_str());
        if (AIUnit* unit = ai->CommanderUnit())
        {
            J b;
            AIGroup* grp = unit->GetGroup();
            b.Str("group", grp ? static_cast<const char*>(grp->GetName()) : "");
            b.Bool("groupLeader", unit->IsGroupLeader());
            b.Int("state", static_cast<int>(unit->GetState())).Int("life", static_cast<int>(unit->GetLifeState()));
            b.Int("combatMode", static_cast<int>(unit->GetCombatMode()));
            b.Int("semaphore", static_cast<int>(unit->GetSemaphore()));
            b.Int("planning", static_cast<int>(unit->GetPlanningMode())).Int("mode", static_cast<int>(unit->GetMode()));
            b.Int("path", unit->GetPath().Size());
            j.Raw("brain", b.Done());
        }
    }
    if (Man* man = dyn_cast<Man>(obj))
    {
        std::string m;
        man->DiagInspect(m);
        j.Raw("man", m);
    }
    return j.Ok();
}

// ---- groups --------------------------------------------------------------------------------------
std::string Groups(cJSON* root)
{
    const int onlySide = static_cast<int>(NumArg(root, "side", -1));
    std::string arr = "[";
    int total = 0;
    AICenter* centers[] = {GWorld->GetWestCenter(), GWorld->GetEastCenter(), GWorld->GetGuerrilaCenter(),
                           GWorld->GetCivilianCenter()};
    for (AICenter* c : centers)
    {
        if (!c)
            continue;
        if (onlySide >= 0 && static_cast<int>(c->GetSide()) != onlySide)
            continue;
        for (int g = 0; g < c->NGroups(); g++)
        {
            AIGroup* grp = c->GetGroup(g);
            if (!grp)
                continue;
            AIUnit* leader = grp->Leader();
            const Vector3 lp = leader ? leader->Position() : VZero;
            int n = 0, alive = 0, still = 0;
            float spreadSum = 0, spreadMax = 0;
            for (int i = 0; i < MAX_UNITS_PER_GROUP; i++)
            {
                AIUnit* u = grp->UnitWithID(i + 1);
                if (!u)
                    continue;
                n++;
                EntityAI* v = u->GetVehicle();
                if (!v || v->IsDammageDestroyed())
                    continue;
                alive++;
                const float d = v->Position().Distance(lp);
                spreadSum += d;
                spreadMax = std::max(spreadMax, d);
                if (v->Speed().Size() < 0.2f)
                    still++;
            }
            J j;
            j.Str("name", static_cast<const char*>(grp->GetName())).Int("side", static_cast<int>(c->GetSide()));
            j.Int("units", n).Int("alive", alive);
            j.Str("leader", leader && leader->GetVehicle() ? ObjName(leader->GetVehicle()).c_str() : "");
            VecField(j, "leaderPos", lp).Num("spreadAvg", alive ? spreadSum / alive : 0, "%.1f");
            j.Num("spreadMax", spreadMax, "%.1f").Int("still", still);
            j.Int("formation", grp->MainSubgroup() ? static_cast<int>(grp->MainSubgroup()->GetFormation()) : -1);
            j.Int("semaphore", static_cast<int>(grp->GetSemaphore()));
            j.Int("combatMinor", static_cast<int>(grp->GetCombatModeMinor()));
            if (arr.size() > 1)
                arr += ',';
            arr += j.Done();
            total++;
        }
    }
    arr += ']';
    J r;
    r.Int("count", total).Raw("groups", arr);
    return r.Ok();
}

// ---- near / geometry -------------------------------------------------------------------------------
std::string LodInfo(const LODShape* shape)
{
    if (!shape)
        return "null";
    J l;
    l.Str("name", shape->Name()).Int("lods", shape->NLevels());
    l.Bool("geometry", shape->GeometryLevel() != nullptr).Bool("fire", shape->FireGeometryLevel() != nullptr);
    l.Bool("view", shape->ViewGeometryLevel() != nullptr).Bool("landContact", shape->LandContactLevel() != nullptr);
    l.Bool("memory", shape->MemoryLevel() != nullptr).Bool("hitpoints", shape->HitpointsLevel() != nullptr);
    l.Num("boundingSphere", shape->BoundingSphere(), "%.2f").Num("geometrySphere", shape->GeometrySphere(), "%.2f");
    VecField(l, "min", shape->Min());
    VecField(l, "max", shape->Max());
    return l.Done();
}

std::string Near(cJSON* root)
{
    Vector3 pos;
    std::string err;
    const char* unit = StrArg(root, "unit");
    Object* center = nullptr;
    if (unit && unit[0])
    {
        center = Internal::EvalObject(unit, err);
        if (!center)
            return Err(err);
        pos = center->Position();
    }
    else if (!GetPosArg(root, pos))
        return Err("diag_near needs unit or pos:[x,z]");
    const float r = static_cast<float>(NumArg(root, "r", 15));
    struct Item
    {
        Object* obj;
        float dist;
    };
    std::vector<Item> items;
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, r);
    for (int x = xMin; x <= xMax; x++)
        for (int z = zMin; z <= zMax; z++)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* o = list[i];
                if (!o || o == center)
                    continue;
                const float d = o->Position().Distance(pos);
                if (d <= r + o->GetRadius())
                    items.push_back({o, d});
            }
        }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist < b.dist; });
    std::string arr = "[";
    int n = 0;
    for (const Item& it : items)
    {
        if (n >= 200)
            break;
        J j;
        j.Str("name", ObjName(it.obj).c_str()).Str("cls", ClassName(it.obj).c_str());
        j.Str("shape", ShapeOf(it.obj).c_str());
        j.Int("type", static_cast<int>(it.obj->GetType())).Bool("static", it.obj->Static());
        VecField(j, "pos", it.obj->Position()).Num("dist", it.dist, "%.2f").Num("radius", it.obj->GetRadius(), "%.2f");
        j.Num("damage", it.obj->GetTotalDammage(), "%.3f");
        const LODShape* sh = it.obj->GetShape();
        j.Bool("geometry", sh && sh->GeometryLevel()).Bool("fire", sh && sh->FireGeometryLevel());
        j.Bool("view", sh && sh->ViewGeometryLevel());
        if (arr.size() > 1)
            arr += ',';
        arr += j.Done();
        n++;
    }
    arr += ']';
    J r2;
    VecField(r2, "pos", pos).Num("r", r).Int("count", static_cast<long long>(items.size())).Raw("objects", arr);
    return r2.Ok();
}

void DrawBox(const Object* obj, Vector3Par mn, Vector3Par mx, uint32_t rgba, float ttl)
{
    Vector3 c[8];
    for (int i = 0; i < 8; i++)
        c[i] = obj->PositionModelToWorld(
            Vector3((i & 1) ? mx.X() : mn.X(), (i & 2) ? mx.Y() : mn.Y(), (i & 4) ? mx.Z() : mn.Z()));
    static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                 {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& k : e)
        DrawLine(c[k[0]], c[k[1]], rgba, ttl);
}

std::string Components(const Object* obj, const ConvexComponents& cc, uint32_t rgba, bool draw, float ttl)
{
    std::string arr = "[";
    for (int i = 0; i < cc.Size(); i++)
    {
        const ConvexComponent* c = cc[i];
        if (!c)
            continue;
        if (draw)
            DrawBox(obj, c->Min(), c->Max(), rgba, ttl);
        if (i >= 64)
            continue;
        J j;
        VecField(j, "min", c->Min());
        VecField(j, "max", c->Max());
        if (arr.size() > 1)
            arr += ',';
        arr += j.Done();
    }
    arr += ']';
    return arr;
}

std::string Geometry(cJSON* root)
{
    std::string err;
    Object* obj = Internal::EvalObject(StrArg(root, "unit"), err);
    if (!obj)
        return Err(err);
    const LODShape* sh = obj->GetShape();
    if (!sh)
        return Err("object has no shape");
    const bool draw = BoolArg(root, "draw", false) && DrawEnabled();
    const float ttl = static_cast<float>(NumArg(root, "ttl", 30));
    J j;
    j.Str("name", ObjName(obj).c_str()).Raw("lod", LodInfo(sh));
    j.Raw("geometry", Components(obj, sh->GetGeomComponents(), 0xff40ffff, draw, ttl)); // cyan
    j.Raw("fire", Components(obj, sh->GetFireComponents(), 0xffff40ff, draw, ttl));     // magenta
    j.Raw("view", Components(obj, sh->GetViewComponents(), 0x40a0ffff, draw, ttl));     // blue
    if (draw)
        DrawBox(obj, sh->Min(), sh->Max(), 0xffffffff, ttl);
    return j.Ok();
}

// ---- perf ----------------------------------------------------------------------------------------------
std::string Perf()
{
    FrameProfiler& perf = GFrameProfiler();
    J j;
    const FrameProfiler::PhaseStats t = perf.TotalStats();
    j.Int("frames", perf.FrameCount()).Num("fps", perf.AvgFps(), "%.1f");
    j.Num("avgMs", t.avgMs, "%.2f").Num("p95Ms", t.p95Ms, "%.2f").Num("maxMs", t.maxMs, "%.2f");
    j.Num("drawCalls", perf.AvgDrawCalls(), "%.0f");
    std::string ph = "{";
    for (int p = 0; p < FrameProfiler::PhaseCount; p++)
    {
        const FrameProfiler::PhaseStats s = perf.Stats(static_cast<FrameProfiler::Phase>(p));
        if (p)
            ph += ',';
        Json::AppendString(ph, FrameProfiler::PhaseName(p));
        char b[96];
        std::snprintf(b, sizeof(b), ":[%.2f,%.2f,%.2f]", s.avgMs, s.p95Ms, s.maxMs);
        ph += b;
    }
    ph += '}';
    j.Raw("phases", ph); // [avg, p95, max] ms
    if (GWorld)
    {
        j.Num("viewDistance", GWorld->GetScene()->GetFogMaxRange(), "%.0f");
        j.Int("vehicles", GWorld->NVehicles()).Int("fast", GWorld->NFastVehicles());
        j.Num("accTime", GWorld->GetAcceleratedTime(), "%.2f");
    }
    j.Bool("paused", Paused());
    return j.Ok();
}

// ---- anim / pose / kill ----------------------------------------------------------------------------------
std::string AnimCmd(cJSON* root)
{
    std::string err;
    Object* obj = Internal::EvalObject(StrArg(root, "unit"), err);
    if (!obj)
        return Err(err);
    Man* man = dyn_cast<Man>(obj);
    if (!man)
        return Err("not a Man");
    std::string info;
    man->DiagAnimInfo(info);
    J j;
    j.Str("name", ObjName(obj).c_str()).Raw("anim", info);
    return j.Ok();
}

std::string PoseCmd(cJSON* root)
{
    std::string err;
    Object* obj = Internal::EvalObject(StrArg(root, "unit"), err);
    if (!obj)
        return Err(err);
    Man* man = dyn_cast<Man>(obj);
    if (!man)
        return Err("not a Man");
    const char* move = StrArg(root, "move");
    const float t = static_cast<float>(NumArg(root, "time", 0));
    if (!man->DiagSetPose(move ? move : "", t))
        return Err("unknown move");
    if (BoolArg(root, "pause", true))
        Pause();
    J j;
    j.Str("name", ObjName(obj).c_str()).Str("move", man->DiagMoveName().c_str()).Num("time", t);
    j.Bool("paused", Paused());
    return j.Ok();
}

std::string KillCmd(cJSON* root)
{
    std::string err;
    const char* unit = StrArg(root, "unit");
    Object* obj = Internal::EvalObject(unit, err);
    if (!obj)
        return Err(err);
    Watch(obj, true);
    ExecSqf(std::string(unit) + " setDammage 1");
    const char* move = StrArg(root, "move");
    J j;
    j.Str("name", ObjName(obj).c_str()).Bool("dead", obj->IsDammageDestroyed());
    if (move && move[0])
    {
        if (Man* man = dyn_cast<Man>(obj))
        {
            man->SwitchMove(RStringB(move));
            j.Str("move", man->DiagMoveName().c_str());
        }
    }
    return j.Ok();
}

// ---- move config check ------------------------------------------------------------------------------------
std::string Lower(const char* s)
{
    std::string out = s ? s : "";
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool IsMoveCfgWithStates(const ParamEntry& cls)
{
    return cls.IsClass() && cls.FindEntry("States") && cls.FindEntry("States")->IsClass();
}

void CheckMoves(const ParamEntry& cfg, std::string& out, int& problems, int& moves, int& files)
{
    const ParamEntry& states = cfg >> "States";
    std::set<std::string> names;
    for (int i = 0; i < states.GetEntryCount(); i++)
        if (states.GetEntry(i).IsClass())
            names.insert(Lower(states.GetEntry(i).GetName()));
    auto has = [&](const char* n) { return names.count(Lower(n)) > 0; };
    auto add = [&](const char* kind, const std::string& where, const std::string& what)
    {
        if (std::strncmp(kind, "info", 4) != 0)
            problems++; // "info-..." notes are listed but do not count as problems
        if (out.size() > 800000)
            return;
        if (out.size() > 1)
            out += ',';
        out += "{\"kind\":" + Json::Quote(kind) + ",\"where\":" + Json::Quote(where.c_str()) +
               ",\"what\":" + Json::Quote(what.c_str()) + "}";
    };
    const std::string cfgName = static_cast<const char*>(cfg.GetName());
    std::map<std::string, std::vector<std::string>> edges;
    std::set<std::string> roots;
    std::set<std::string> dieMoves;
    std::set<std::string> fileChecked;
    for (int i = 0; i < states.GetEntryCount(); i++)
    {
        const ParamEntry& st = states.GetEntry(i);
        if (!st.IsClass())
            continue;
        moves++;
        const std::string n = static_cast<const char*>(st.GetName());
        const std::string ln = Lower(n.c_str());
        const std::string where = cfgName + "/" + n;
        if (const ParamEntry* f = st.FindEntry("file"))
        {
            RString file = *f;
            if (file.GetLength() > 0)
            {
                RString full = GetAnimationName(file);
                if (fileChecked.insert(static_cast<const char*>(full)).second)
                {
                    files++;
                    if (!QIFStreamB::FileExist(full))
                        add("rtm", where, std::string("animation file not found: ") + static_cast<const char*>(full));
                }
            }
        }
        static const char* kLinks[] = {"connectTo", "interpolateTo", "connectFrom"};
        for (const char* key : kLinks)
        {
            const ParamEntry* a = st.FindEntry(key);
            if (!a || !a->IsArray())
                continue;
            for (int k = 0; k < a->GetSize(); k += 2)
            {
                RString to = (*a)[k];
                if (to.GetLength() == 0)
                    continue;
                if (!has(to))
                    add("link", where + " " + key, std::string("no move ") + static_cast<const char*>(to));
                else if (std::strcmp(key, "connectFrom") != 0)
                    edges[ln].push_back(Lower(to));
            }
        }
        if (const ParamEntry* sp = st.FindEntry("speed"))
            if (static_cast<float>(*sp) == 0 &&
                !(st.FindEntry("looped") && static_cast<int>(*st.FindEntry("looped")) == 0))
                add("speed", where, "speed = 0 (the move never advances)");
    }
    // actions: every text value must name a move
    if (const ParamEntry* actions = cfg.FindEntry("Actions"))
    {
        for (int i = 0; i < actions->GetEntryCount(); i++)
        {
            const ParamEntry& map = actions->GetEntry(i);
            if (!map.IsClass())
                continue;
            for (int k = 0; k < map.GetEntryCount(); k++)
            {
                const ParamEntry& v = map.GetEntry(k);
                if (v.IsClass() || v.IsArray() || !v.IsTextValue())
                    continue;
                RString mv = v;
                if (mv.GetLength() == 0)
                    continue;
                const std::string key = static_cast<const char*>(v.GetName());
                if (Lower(key.c_str()) == "updegree" || Lower(mv).rfind("manpos", 0) == 0)
                    continue; // posture name, not a move
                if (!has(mv))
                {
                    // a number stored as text (turnSpeed, upDegree, limitFast) is not a move
                    const char* p = mv;
                    const bool numeric = (*p == '-' || (*p >= '0' && *p <= '9'));
                    if (!numeric)
                        add("action", cfgName + "/Actions/" + static_cast<const char*>(map.GetName()) + " " + key,
                            std::string("no move ") + static_cast<const char*>(mv));
                    continue;
                }
                const std::string lm = Lower(mv);
                roots.insert(lm);
                const std::string lk = Lower(key.c_str());
                if (lk == "die" || lk == "diver" || lk == "dieback")
                    dieMoves.insert(lm);
            }
        }
    }
    // death moves should be terminal, or connect to a terminal (dead) state
    auto isTerminal = [&](const char* name)
    {
        const ParamEntry* s2 = states.FindEntry(name);
        const ParamEntry* t = s2 ? s2->FindEntry("terminal") : nullptr;
        return t && static_cast<int>(*t) != 0;
    };
    for (int i = 0; i < states.GetEntryCount(); i++)
    {
        const ParamEntry& st = states.GetEntry(i);
        if (!st.IsClass() || !dieMoves.count(Lower(st.GetName())))
            continue;
        bool ok = isTerminal(st.GetName());
        const ParamEntry* ct = st.FindEntry("connectTo");
        if (!ok && ct && ct->IsArray())
            for (int j = 0; j + 1 < ct->GetSize() && !ok; j += 2)
                if ((*ct)[j].IsTextValue())
                {
                    RString nm = (*ct)[j];
                    ok = isTerminal(nm);
                }
        if (!ok)
            add("death", cfgName + "/" + static_cast<const char*>(st.GetName()),
                "death move is not terminal and does not connect to a terminal state");
    }
    // reachability from the moves the actions use
    std::set<std::string> seen = roots;
    std::vector<std::string> todo(roots.begin(), roots.end());
    while (!todo.empty())
    {
        const std::string m = todo.back();
        todo.pop_back();
        for (const std::string& n : edges[m])
            if (seen.insert(n).second)
                todo.push_back(n);
    }
    int unreachable = 0;
    std::string un;
    for (const std::string& n : names)
        if (!seen.count(n))
        {
            unreachable++;
            if (unreachable <= 30)
                un += (un.empty() ? "" : ", ") + n;
        }
    if (unreachable && !roots.empty())
        add("info-unreachable", cfgName,
            std::to_string(unreachable) +
                " move(s) not reachable from any action (only switchMove/playMove can "
                "start them): " +
                un);
}

std::string MoveCheck(cJSON* root)
{
    const char* only = StrArg(root, "cfg");
    std::string out = "[";
    int problems = 0, moves = 0, files = 0, classes = 0;
    std::string names = "[";
    for (int i = 0; i < Pars.GetEntryCount(); i++)
    {
        const ParamEntry& e = Pars.GetEntry(i);
        const std::string n = static_cast<const char*>(e.GetName());
        if (n.rfind("CfgMoves", 0) != 0 || !IsMoveCfgWithStates(e))
            continue;
        if (only && only[0] && n != only)
            continue;
        classes++;
        if (names.size() > 1)
            names += ',';
        names += Json::Quote(n.c_str());
        CheckMoves(e, out, problems, moves, files);
    }
    out += ']';
    names += ']';
    J j;
    j.Raw("classes", names).Int("moves", moves).Int("files", files).Int("problems", problems).Raw("list", out);
    return j.Ok();
}

// ---- scripts --------------------------------------------------------------------------------------------
std::string Scripts()
{
    std::string arr = "[";
    const int n = GWorld ? GWorld->DiagNScripts() : 0;
    for (int i = 0; i < n; i++)
    {
        Script* s = GWorld->DiagScript(i);
        if (!s)
            continue;
        J j;
        j.Str("name", static_cast<const char*>(s->GetDebugName())).Int("line", s->DiagLine());
        j.Int("lines", s->DiagNLines()).Str("statement", s->DiagStatement(s->DiagLine()));
        j.Num("time", s->DiagTime(), "%.2f").Bool("terminated", s->IsTerminated());
        if (arr.size() > 1)
            arr += ',';
        arr += j.Done();
    }
    arr += ']';
    J j;
    j.Int("count", n).Raw("scripts", arr);
    return j.Ok();
}

std::string OkResp()
{
    return HarnessProtocol::OkResponse();
}
} // namespace

// ================================================================================================
namespace Internal
{
Object* EvalObject(const char* code, std::string& err)
{
    if (!code || !code[0])
    {
        err = "unit required";
        return nullptr;
    }
    GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
    if (!gs)
    {
        err = "no game state";
        return nullptr;
    }
    gs->BeginContext(&s_diagScope);
    GameValue v = gs->Evaluate(code);
    gs->EndContext();
    Object* obj = GetObject(v);
    if (!obj)
        err = std::string("'") + code + "' is not an object";
    return obj;
}
} // namespace Internal

void RegisterHarness(HarnessServer& hs)
{
    Internal::RegisterCamera(hs);
    hs.RegisterCommand({"diag_inspect", "diag: one unit/object's full state", {{"unit", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           std::string err;
                           Object* obj = Internal::EvalObject(StrArg(root, "unit"), err);
                           return obj ? InspectEntity(obj) : Err(err);
                       });
    hs.RegisterCommand({"diag_groups", "diag: every AI group, spread and modes", {{"side", "int", false}}},
                       [](const std::string&, cJSON* root) -> std::string
                       { return GWorld ? Groups(root) : Err("no world"); });
    hs.RegisterCommand({"diag_near",
                        "diag: objects near a unit or position",
                        {{"unit", "string", false}, {"pos", "array", false}, {"r", "number", false}}},
                       [](const std::string&, cJSON* root) -> std::string
                       { return GLandscape ? Near(root) : Err("no landscape"); });
    hs.RegisterCommand({"diag_geometry",
                        "diag: an object's LODs and convex components (drawn with draw=true)",
                        {{"unit", "string", true}, {"draw", "bool", false}, {"ttl", "number", false}}},
                       [](const std::string&, cJSON* root) -> std::string { return Geometry(root); });
    hs.RegisterCommand({"diag_perf", "diag: frame time stats (FrameProfiler window) and counters", {}},
                       [](const std::string&, cJSON*) -> std::string { return Perf(); });
    hs.RegisterCommand({"diag_pause", "diag: engage the dev pause (simulation stops, camera keeps flying)", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           Pause();
                           J j;
                           j.Bool("paused", Paused());
                           return j.Ok();
                       });
    hs.RegisterCommand({"diag_resume", "diag: release the dev pause", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           Resume();
                           return OkResp();
                       });
    hs.RegisterCommand({"diag_step",
                        "diag: run N fixed simulation ticks (one per frame), then stay paused",
                        {{"ticks", "int", false}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const int ticks = static_cast<int>(NumArg(root, "ticks", NumArg(root, "frames", 1)));
                           Step(ticks);
                           J j;
                           j.Int("ticks", ticks).Bool("paused", Paused());
                           return j.Ok();
                       });
    hs.RegisterCommand({"diag_watch",
                        "diag: label a unit in the overlay and log its moves, AI changes and trail",
                        {{"unit", "string", true}, {"on", "bool", false}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           std::string err;
                           Object* obj = Internal::EvalObject(StrArg(root, "unit"), err);
                           if (!obj)
                               return Err(err);
                           Watch(obj, BoolArg(root, "on", true));
                           return OkResp();
                       });
    hs.RegisterCommand({"diag_mark", "diag: marker line in events.jsonl", {{"text", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* text = StrArg(root, "text");
                           Mark(text ? text : "");
                           return OkResp();
                       });
    hs.RegisterCommand({"diag_draw", "diag: 3D overlay on/off", {{"on", "bool", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           SetDraw(BoolArg(root, "on", true));
                           return OkResp();
                       });
    hs.RegisterCommand({"diag_stuck", "diag: seconds without moving before a stuck event", {{"sec", "number", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           Internal::SetStuckSeconds(static_cast<float>(NumArg(root, "sec", 5)));
                           J j;
                           j.Num("sec", Internal::StuckSeconds());
                           return j.Ok();
                       });
    hs.RegisterCommand(
        {"diag_anim", "diag: current animation (move, RTM, keyframes, step, speed, blend)", {{"unit", "string", true}}},
        [](const std::string&, cJSON* root) -> std::string { return AnimCmd(root); });
    hs.RegisterCommand(
        {"diag_pose",
         "diag: put a man into a move at a fixed phase and pause",
         {{"unit", "string", true}, {"move", "string", true}, {"time", "number", false}, {"pause", "bool", false}}},
        [](const std::string&, cJSON* root) -> std::string { return PoseCmd(root); });
    hs.RegisterCommand({"diag_kill",
                        "diag: kill a unit (optionally forcing a move) and follow the corpse",
                        {{"unit", "string", true}, {"move", "string", false}}},
                       [](const std::string&, cJSON* root) -> std::string { return KillCmd(root); });
    hs.RegisterCommand(
        {"diag_movecheck", "diag: animation config check of the CfgMoves* classes", {{"cfg", "string", false}}},
        [](const std::string&, cJSON* root) -> std::string { return MoveCheck(root); });
    hs.RegisterCommand({"diag_scripts", "diag: running SQS scripts and their current line", {}},
                       [](const std::string&, cJSON*) -> std::string { return Scripts(); });
    hs.RegisterEvent({"diag_ready", "POSEIDON_DIAG build: diag_* commands available", {}});
}

} // namespace Poseidon::Dev::OpDiag

// ================================================================================================
// Man members used by the inspector, the pollers and the overlay (declared in SoldierOld.hpp under
// POSEIDON_DIAG). Ported from MwDiagHarness.cpp / MwDiagHarness2.cpp without the undead / melee fields.
namespace Poseidon
{
std::string Man::DiagMoveName() const
{
    if (_primaryMove.id < 0)
        return std::string();
    RStringB n = Type()->GetMoveName(_primaryMove.id);
    return std::string(static_cast<const char*>(n));
}

void Man::DiagInspect(std::string& out) const
{
    using namespace Dev::OpDiag;
    char b[160];
    out = "{\"move\":";
    Json::AppendString(out, DiagMoveName().c_str());
    std::snprintf(b, sizeof(b), ",\"time\":%.3f,\"factor\":%.3f", _primaryTime, _primaryFactor);
    out += b;
    out += ",\"secondary\":";
    Json::AppendString(out,
                       _secondaryMove.id >= 0 ? static_cast<const char*>(Type()->GetMoveName(_secondaryMove.id)) : "");
    out += ",\"external\":";
    Json::AppendString(
        out, _externalMove.id != MoveIdNone ? static_cast<const char*>(Type()->GetMoveName(_externalMove.id)) : "");
    std::snprintf(b, sizeof(b), ",\"queue\":%d,\"down\":%s,\"unitPos\":%d}", _queueMove.Size(),
                  IsDown() ? "true" : "false", static_cast<int>(_unitPos));
    out += b;
}

void Man::DiagAnimInfo(std::string& out) const
{
    using namespace Dev::OpDiag;
    auto one = [&](const char* key, MoveId id, float time)
    {
        out += '"';
        out += key;
        out += "\":{\"move\":";
        Json::AppendString(out, id >= 0 ? static_cast<const char*>(Type()->GetMoveName(id)) : "");
        const MoveInfo* mi = id >= 0 ? Type()->GetMoveInfo(id) : nullptr;
        const AnimationRT* a = mi ? static_cast<const AnimationRT*>(*mi) : nullptr;
        char b[256];
        if (a)
        {
            out += ",\"file\":";
            Json::AppendString(out, a->Name());
            const Vector3 st = a->GetStep();
            std::snprintf(b, sizeof(b), ",\"keyframes\":%d,\"looped\":%s,\"step\":[%.3f,%.3f,%.3f]",
                          a->GetKeyframeCount(), a->GetLooped() ? "true" : "false", st.X(), st.Y(), st.Z());
            out += b;
        }
        if (mi)
        {
            // speed 1e10 marks a frozen (dead) state: no ground speed
            const float speedZ =
                std::fabs(mi->GetSpeed()) > 1e6f ? 0.0f : mi->GetSpeed() * (a ? -a->GetStepLength() : 0);
            std::snprintf(b, sizeof(b),
                          ",\"speed\":%.3f,\"cycleSec\":%.3f,\"moveSpeed\":%.3f,\"relSpeed\":[%.2f,%.2f],"
                          "\"duty\":%.2f,\"terminal\":%s",
                          mi->GetSpeed(), mi->GetSpeed() != 0 ? 1.0f / std::fabs(mi->GetSpeed()) : 0.0f, speedZ,
                          mi->GetRelSpeedMin(), mi->GetRelSpeedMax(), mi->GetDuty(),
                          mi->GetTerminal() ? "true" : "false");
            out += b;
        }
        std::snprintf(b, sizeof(b), ",\"time\":%.3f}", time);
        out += b;
    };
    out = "{";
    one("primary", _primaryMove.id, _primaryTime);
    out += ',';
    one("secondary", _secondaryMove.id, _secondaryTime);
    char b[160];
    std::snprintf(b, sizeof(b), ",\"factor\":%.3f,\"unitSpeed\":%.3f,\"queue\":%d}", _primaryFactor, Speed().Size(),
                  _queueMove.Size());
    out += b;
}

bool Man::DiagSetPose(const char* move, float time)
{
    const MoveId id = Type()->GetMoveId(RStringB(move ? move : ""));
    if (id == MoveIdNone)
        return false;
    // SwitchMove puts the previous external move back into the external queue, so a pose sheet
    // (many poses in a row) left every earlier pose queued and the unit replayed them after
    // diag_resume (found in Malprave). A pose replaces them all.
    _externalQueue.Clear();
    _externalMove = MotionPathItem(static_cast<MoveId>(MoveIdNone));
    _forceMove = MotionPathItem(static_cast<MoveId>(MoveIdNone));
    SwitchMove(id, nullptr);
    if (time < 0)
        time = 0;
    if (time > 1)
        time = 1;
    _primaryTime = time;
    _secondaryTime = time;
    _primaryFactor = 1;
    return true;
}
} // namespace Poseidon

namespace Poseidon::Dev::OpDiag::Internal
{
std::string MoveNameOf(const Man* man)
{
    return man ? man->DiagMoveName() : std::string();
}
} // namespace Poseidon::Dev::OpDiag::Internal

#endif // POSEIDON_DIAG
