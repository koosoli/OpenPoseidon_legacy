// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Conquest/ConquestReferee.hpp>
#include <Poseidon/Game/Conquest/ConquestView.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Network/NetworkIface.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

GameValue A1UnitCreate(const GameState* state, GameValuePar oper1, GameValuePar oper2); // GameStateExtWorld.cpp
void DeleteVehicle(Poseidon::Entity* veh);                                              // GameStateExtWorld.cpp

namespace Poseidon
{
RString GetMissionDirectory();
}

namespace Poseidon::Conquest
{
namespace
{
constexpr float TickSeconds = 1.0f;
constexpr float StatusSeconds = 60.0f; // game seconds between status lines
constexpr float KnowledgeAgeSeconds = 120.0f; // older contacts are not counted
constexpr float KnowledgeReach = 300.0f;      // a contact this far outside a zone still threatens it
constexpr float StallSeconds = 240.0f;
constexpr float ReorderSeconds = 90.0f;
constexpr float BodySeconds = 300.0f;
constexpr float BodyHideDistance = 300.0f;
constexpr size_t LogLines = 200;
constexpr float HumanLockSeconds = 600.0f; // a commander's order outranks the AI planner this long
constexpr float PickRadius = 150.0f;
constexpr float DismountDistance = 180.0f; // under AIGroup::Move's 200 m mount distance, so they do not fight       // a map click this close to a squad selects it

struct GroupRecord
{
    int id = 0;
    int side = NoSide;
    OLink<AIGroup> group;
    TaskKind task = TaskKind::None;
    int objective = -1;
    Vector3 destination = VZero;
    float orderedAt = -1.0e9f;
    float bestDistance = 1.0e9f;
    float bestAt = 0.0f;
    bool stalled = false;
    float lockedUntil = -1.0e9f; // a human commander's order holds until then
    bool selected = false;       // in the human commander's map selection
    int type = 0;                // index into Config::squads
};

struct UnitRecord
{
    OLink<EntityAI> unit;
    int side = NoSide;
    float lastDamage = 0.0f;
    float deadAt = -1.0f;
    bool vehicle = false; // a squad's car or helicopter: its loss is not a soldier's
};

// One kind of squad a side can buy: a name, a price and the classes it is made of per side.
struct SquadDef
{
    std::string id; // the config class name
    std::string name;
    float cost = 80.0f;
    std::string role = "foot"; // foot, motorised, mechanised, air: how the AI commanders choose
    bool aiBuys = true;
    std::array<std::vector<RString>, MaxSides> classes;
    std::array<std::vector<RString>, MaxSides> vehicles; // spawned empty beside the squad, given to it
};

struct Config
{
    std::array<TargetSide, MaxSides> engineSide{TWest, TEast, TGuerrila};
    std::vector<SquadDef> squads; // [0] is the rifle squad the AI commanders buy
    float startPool = 300.0f;
    int cap = 60;
    float squadCost = 80.0f;
    std::vector<int> startSquads{0, 0, 0}; // squad types each HQ starts with, free
    float planSeconds = 20.0f;
    bool groupMarkers = true; // every squad on the map with its task (observer / debugging view)
    bool possession = true;   // the commander may take control of any friendly soldier
};

struct Referee
{
    std::string mission;
    bool active = false;
    bool started = false;
    bool ended = false;
    Config config;
    Tuning tuning;
    CommanderTuning commander;
    War war;
    std::vector<GroupRecord> groups;
    std::vector<UnitRecord> units;
    std::array<float, MaxSides> lastPlan{};
    std::array<bool, MaxSides> replan{};
    std::array<int, MaxSides> live{};
    std::array<int, MaxSides> losses{};
    std::array<int, MaxSides> bought{};
    int nextGroupId = 1;
    int playerSide = NoSide; // the side the player can command from the map
    bool humanCommander = false; // the player has the commander view open: purchases are theirs
    OLink<Person> commanderBody;  // the player's own body while they are inside a soldier
    bool reopenView = false;      // the possessed soldier died: the view opens again
    float lastTick = -1.0f;
    float lastStatus = -1.0e9f;
    std::vector<std::string> markerText; // per objective, what its label marker last said
    std::vector<int> markerOwner;
    std::map<int, std::string> groupMarkerText; // group id -> what its map marker last said
    Snapshot snapshot;
};

Referee& R()
{
    static Referee r;
    return r;
}

// A bar that is neither full for its owner nor an untouched neutral: someone is taking it.
bool InProgress(const ObjectiveState& s)
{
    return s.owner == NoSide ? s.level > 0.0f : (s.holder != s.owner || s.level < 1.0f);
}

float Now()
{
    return Glob.time - Foundation::Time(0);
}

const char* SideLabel(TargetSide side)
{
    switch (side)
    {
        case TWest: return "WEST";
        case TEast: return "EAST";
        case TGuerrila: return "GUER";
        default: break;
    }
    return "?";
}

bool SideFromName(const std::string& name, TargetSide& side)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (n == "WEST" || n == "BLUFOR")
        side = TWest;
    else if (n == "EAST" || n == "OPFOR")
        side = TEast;
    else if (n == "GUER" || n == "RESISTANCE" || n == "INDEPENDENT")
        side = TGuerrila;
    else
        return false;
    return true;
}

void WarLog(const std::string& line)
{
    Referee& r = R();
    char stamp[16];
    const int t = static_cast<int>(Now());
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d ", t / 60, t % 60);
    r.snapshot.log.push_back(stamp + line);
    while (r.snapshot.log.size() > LogLines)
        r.snapshot.log.pop_front();
    LOG_INFO(Mission, "Conquest: {}", line);
}

std::string Name(int side)
{
    return side == NoSide ? std::string("nobody") : std::string(SideLabel(R().config.engineSide[side]));
}

// ---- config ------------------------------------------------------------------------------

float Number(const ParamEntry& cls, const char* name, float fallback)
{
    const ParamEntry* e = cls.FindEntry(name);
    return e && !e->IsClass() && !e->IsArray() ? static_cast<float>(*e) : fallback;
}

std::string Text(const ParamEntry& cls, const char* name, const char* fallback)
{
    const ParamEntry* e = cls.FindEntry(name);
    if (!e || e->IsClass() || e->IsArray())
        return fallback;
    const RStringB v = *e;
    return std::string(static_cast<const char*>(v));
}

std::vector<std::string> Strings(const ParamEntry& cls, const char* name)
{
    std::vector<std::string> out;
    const ParamEntry* e = cls.FindEntry(name);
    if (!e || !e->IsArray())
        return out;
    for (int i = 0; i < e->GetSize(); i++)
    {
        const RStringB v = (*e)[i];
        out.emplace_back(static_cast<const char*>(v));
    }
    return out;
}

bool LoadConfig(Referee& r, const ParamEntry& cfg)
{
    r.config = Config();
    r.tuning = Tuning();
    r.war = War();
    const std::vector<std::string> sides = Strings(cfg, "sides");
    r.war.sideCount = 0;
    for (const std::string& s : sides)
    {
        TargetSide side;
        if (r.war.sideCount < MaxSides && SideFromName(s, side))
            r.config.engineSide[r.war.sideCount++] = side;
    }
    if (r.war.sideCount < 2)
    {
        LOG_WARN(Mission, "Conquest: CfgConquest needs sides[] with at least two of WEST, EAST, GUER");
        return false;
    }
    r.config.startPool = Number(cfg, "startPool", r.config.startPool);
    r.config.cap = static_cast<int>(Number(cfg, "cap", static_cast<float>(r.config.cap)));
    r.config.squadCost = Number(cfg, "squadCost", r.config.squadCost);
    r.config.planSeconds = std::max(5.0f, Number(cfg, "planSeconds", r.config.planSeconds));
    r.config.groupMarkers = Number(cfg, "groupMarkers", 1.0f) != 0.0f;
    r.config.possession = Number(cfg, "possession", 1.0f) != 0.0f;
    r.tuning.baseCaptureSeconds = Number(cfg, "captureSeconds", r.tuning.baseCaptureSeconds);
    r.tuning.securityRadius = Number(cfg, "securityRadius", r.tuning.securityRadius);
    r.tuning.startScore = Number(cfg, "startScore", r.tuning.startScore);
    r.tuning.scoreDrainPerObjective = Number(cfg, "scoreDrain", r.tuning.scoreDrainPerObjective);

    // class Squads { class Rifle { name; cost; WEST[]; EAST[]; }; ... } -- or, older, one
    // class Rosters { WEST[]; EAST[]; } priced by squadCost.
    if (const ParamEntry* squads = cfg.FindEntry("Squads"); squads && squads->IsClass())
        for (int i = 0; i < squads->GetEntryCount(); i++)
        {
            const ParamEntry& e = squads->GetEntry(i);
            if (!e.IsClass())
                continue;
            SquadDef def;
            def.id = static_cast<const char*>(e.GetName());
            def.name = Text(e, "name", def.id.c_str());
            def.cost = Number(e, "cost", r.config.squadCost);
            def.role = Text(e, "role", "foot");
            def.aiBuys = Number(e, "ai", 1.0f) != 0.0f;
            for (int s = 0; s < r.war.sideCount; s++)
            {
                const std::string side = SideLabel(r.config.engineSide[s]);
                for (const std::string& cls : Strings(e, side.c_str()))
                    def.classes[s].push_back(RString(cls.c_str()));
                for (const std::string& cls : Strings(e, (side + "vehicles").c_str()))
                    def.vehicles[s].push_back(RString(cls.c_str()));
            }
            r.config.squads.push_back(def);
        }
    if (r.config.squads.empty())
        if (const ParamEntry* rosters = cfg.FindEntry("Rosters"); rosters && rosters->IsClass())
        {
            SquadDef def;
            def.name = "Rifle squad";
            def.cost = r.config.squadCost;
            for (int s = 0; s < r.war.sideCount; s++)
                for (const std::string& cls : Strings(*rosters, SideLabel(r.config.engineSide[s])))
                    def.classes[s].push_back(RString(cls.c_str()));
            r.config.squads.push_back(def);
        }
    if (r.config.squads.empty())
    {
        LOG_WARN(Mission, "Conquest: CfgConquest needs class Squads (or Rosters)");
        return false;
    }
    // startSquads = 3 (that many of the first type) or startSquads[] = {"Motorised", "Rifle"}.
    if (const ParamEntry* start = cfg.FindEntry("startSquads"); start && start->IsArray())
    {
        r.config.startSquads.clear();
        for (const std::string& id : Strings(cfg, "startSquads"))
            for (size_t t = 0; t < r.config.squads.size(); t++)
                if (r.config.squads[t].id == id)
                    r.config.startSquads.push_back(static_cast<int>(t));
    }
    else
        r.config.startSquads.assign(static_cast<size_t>(Number(cfg, "startSquads", 3.0f)), 0);
    for (int s = 0; s < r.war.sideCount; s++)
        if (r.config.squads[0].classes[s].empty())
        {
            LOG_WARN(Mission, "Conquest: the first squad type has no units for {}", SideLabel(r.config.engineSide[s]));
            return false;
        }

    const ParamEntry* objectives = cfg.FindEntry("Objectives");
    if (!objectives || !objectives->IsClass())
    {
        LOG_WARN(Mission, "Conquest: CfgConquest has no class Objectives");
        return false;
    }
    std::map<std::string, int> byClass;
    std::vector<std::vector<std::string>> links;
    for (int i = 0; i < objectives->GetEntryCount(); i++)
    {
        const ParamEntry& o = objectives->GetEntry(i);
        if (!o.IsClass())
            continue;
        ObjectiveDef d;
        const std::string cls(static_cast<const char*>(o.GetName()));
        d.name = Text(o, "name", cls.c_str());
        const ParamEntry* pos = o.FindEntry("position");
        if (!pos || !pos->IsArray() || pos->GetSize() < 2)
        {
            LOG_WARN(Mission, "Conquest: objective {} has no position[]", cls);
            return false;
        }
        d.x = static_cast<float>((*pos)[0]);
        d.y = static_cast<float>((*pos)[1]);
        d.radius = Number(o, "radius", d.radius);
        d.value = Number(o, "value", d.value);
        KindFromName(Text(o, "kind", "town"), d.kind);
        TargetSide owner;
        if (SideFromName(Text(o, "owner", ""), owner))
            for (int s = 0; s < r.war.sideCount; s++)
                if (r.config.engineSide[s] == owner)
                    d.initialOwner = s;
        byClass[cls] = static_cast<int>(r.war.defs.size());
        r.war.defs.push_back(d);
        links.push_back(Strings(o, "neighbours"));
    }
    // Neighbours by class name, made symmetric.
    for (size_t i = 0; i < links.size(); i++)
        for (const std::string& n : links[i])
        {
            const auto it = byClass.find(n);
            if (it == byClass.end())
            {
                LOG_WARN(Mission, "Conquest: {} lists unknown neighbour {}", r.war.defs[i].name, n);
                continue;
            }
            auto& a = r.war.defs[i].neighbours;
            auto& b = r.war.defs[it->second].neighbours;
            if (std::find(a.begin(), a.end(), it->second) == a.end())
                a.push_back(it->second);
            if (std::find(b.begin(), b.end(), static_cast<int>(i)) == b.end())
                b.push_back(static_cast<int>(i));
        }
    if (r.war.defs.size() < 2)
        return false;
    StartWar(r.war, r.tuning, r.config.startPool);
    r.markerText.assign(r.war.defs.size(), std::string());
    r.markerOwner.assign(r.war.defs.size(), -2);
    LOG_INFO(Mission, "Conquest: {} objectives, {} sides, cap {} per side", r.war.defs.size(), r.war.sideCount,
             r.config.cap);
    return true;
}

// ---- world helpers -----------------------------------------------------------------------

Vector3 Ground(float x, float y)
{
    return Vector3(x, GLandscape ? GLandscape->SurfaceYAboveWater(x, y) : 0.0f, y);
}

float Dist(const Vector3& p, const ObjectiveDef& d)
{
    return std::hypot(p.X() - d.x, p.Z() - d.y);
}

AICenter* Center(int side)
{
    const TargetSide ts = R().config.engineSide[side];
    AICenter* center = GWorld->GetCenter(ts);
    if (!center)
    {
        center = GWorld->CreateCenter(ts);
        if (center)
            GetNetworkManager().CreateObject(center);
    }
    return center;
}

bool Alive(AIUnit* unit)
{
    return unit && unit->GetLifeState() == AIUnit::LSAlive && !unit->GetCaptive();
}

// A point inside objective `o`, on the side away from the nearest objective `side` does not own.
Vector3 SpawnPoint(const Referee& r, int side, int o)
{
    const ObjectiveDef& d = r.war.defs[o];
    float bestDist = 1.0e9f;
    float dx = 0.0f, dy = 0.0f;
    for (size_t i = 0; i < r.war.defs.size(); i++)
    {
        if (r.war.states[i].owner == side)
            continue;
        const float ex = r.war.defs[i].x - d.x, ey = r.war.defs[i].y - d.y;
        const float len = std::hypot(ex, ey);
        if (len > 1.0f && len < bestDist)
        {
            bestDist = len;
            dx = ex / len;
            dy = ey / len;
        }
    }
    return Ground(d.x - dx * d.radius * 0.6f, d.y - dy * d.radius * 0.6f);
}

// Where a group goes for its task: a spread of points around the centre so squads do not stack.
Vector3 TaskPoint(const ObjectiveDef& d, int groupId)
{
    const float angle = static_cast<float>(groupId) * 2.39996f; // golden angle
    const float reach = d.radius * 0.35f;
    return Ground(d.x + std::cos(angle) * reach, d.y + std::sin(angle) * reach);
}

void Order(GroupRecord& rec, const ObjectiveDef& d, float now)
{
    AIGroup* grp = rec.group;
    if (!grp || !grp->MainSubgroup())
        return;
    rec.destination = TaskPoint(d, rec.id);
    grp->Move(grp->MainSubgroup(), rec.destination, Command::Undefined);
    rec.orderedAt = now;
}

void OrderPoint(GroupRecord& rec, const Vector3& destination, float now)
{
    AIGroup* grp = rec.group;
    if (!grp || !grp->MainSubgroup())
        return;
    rec.destination = destination;
    grp->Move(grp->MainSubgroup(), rec.destination, Command::Undefined);
    rec.orderedAt = now;
}

// An empty vehicle of class `cls` at (x, y), through the createVehicle command.
EntityAI* CreateVehicleAt(const RString& cls, float x, float y)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return nullptr;
    GameValue pos = gs->CreateGameValue(GameArray);
    GameArrayType& p = pos;
    p.Add(GameValue(x));
    p.Add(GameValue(y));
    p.Add(GameValue(0.0f));
    GameVarSpace space;
    gs->BeginContext(&space);
    gs->VarSetLocal("_p", pos, true, true);
    gs->VarSetLocal("_t", GameValue(cls), true, true);
    const GameValue made = gs->EvaluateMultiple("_t createVehicle _p");
    gs->EndContext();
    return dyn_cast<EntityAI>(GetObject(made));
}

// Returns the new squad's id, 0 when nothing could be created.
int SpawnSquad(Referee& r, int side, int base, bool free, int type = 0)
{
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
        return 0;
    const SquadDef& squad = r.config.squads[type];
    AICenter* center = Center(side);
    GameState* gs = GWorld->GetGameState();
    if (!center || !gs || center->NGroups() >= MaxGroups || squad.classes[side].empty())
        return 0;
    const Vector3 at = SpawnPoint(r, side, base);
    Ref<AIGroup> group = new AIGroup();
    center->AddGroup(group);
    group->AddFirstWaypoint(at);
    Mission mis;
    mis._action = Mission::Arcade;
    center->SendMission(group, mis);
    GetNetworkManager().CreateObject(group);

    int made = 0;
    for (const RString& cls : squad.classes[side])
    {
        GameValue pos = gs->CreateGameValue(GameArray);
        GameArrayType& p = pos;
        p.Add(GameValue(at.X()));
        p.Add(GameValue(at.Z()));
        GameValue args = gs->CreateGameValue(GameArray);
        GameArrayType& a = args;
        a.Add(GameValue(cls));
        a.Add(pos);
        const GameValue made1 = A1UnitCreate(gs, GameValueExt(group.GetRef()), args);
        if (EntityAI* unit = dyn_cast<EntityAI>(GetObject(made1)))
        {
            UnitRecord u;
            u.unit = unit;
            u.side = side;
            r.units.push_back(u);
            made++;
        }
    }
    if (made == 0)
    {
        group->RemoveFromCenter();
        LOG_WARN(Mission, "Conquest: no unit of the {} {} could be created", Name(side), squad.name);
        return 0;
    }
    // The squad's own cars or helicopter, parked beside it. A move of more than 200 m mounts
    // the squad (AIGroup::Move); TrackGroups dismounts it near the destination.
    int parked = 0;
    for (const RString& cls : squad.vehicles[side])
    {
        const float off = 14.0f * static_cast<float>(++parked);
        if (EntityAI* veh = CreateVehicleAt(cls, at.X() + off, at.Z() + off * 0.5f))
        {
            if (Transport* transport = dyn_cast<Transport>(veh))
                group->AddVehicle(transport);
            UnitRecord u;
            u.unit = veh;
            u.side = side;
            u.vehicle = true;
            r.units.push_back(u);
        }
    }
    if (parked)
        group->AssignVehicles();
    GroupRecord rec;
    rec.id = r.nextGroupId++;
    rec.side = side;
    rec.type = type;
    rec.group = group.GetRef();
    rec.bestAt = Now();
    r.groups.push_back(rec);
    r.live[side] += made;
    if (!free)
    {
        r.war.pool[side] -= squad.cost;
        r.bought[side]++;
    }
    return rec.id;
}

// ---- markers -----------------------------------------------------------------------------

const char* Colour(int side)
{
    if (side == NoSide)
        return "ColorBlack";
    switch (R().config.engineSide[side])
    {
        case TWest: return "ColorBlue";
        case TEast: return "ColorRed";
        case TGuerrila: return "ColorGreen";
        default: break;
    }
    return "ColorYellow";
}

// Every squad as a dot in its side's colour, labelled "W3 attack Montignac (8)": where each
// one is and what it was told to do. Ground truth for both sides, so it is an observer view.
void UpdateGroupMarkers(Referee& r)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs || !r.config.groupMarkers)
        return;
    std::map<int, std::string> live;
    char code[384];
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader())
            continue;
        int men = 0;
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (Alive(grp->UnitWithID(id)))
                men++;
        std::string text = Name(rec.side).substr(0, 1) + std::to_string(rec.id) + " ";
        if (rec.task != TaskKind::None && rec.objective >= 0)
            text += std::string(TaskName(rec.task)) + " " + r.war.defs[rec.objective].name;
        else
            text += "idle";
        text += " (" + std::to_string(men) + ")";
        if (rec.stalled)
            text += " stalled";
        if (Now() < rec.lockedUntil)
            text += " [cmd]";
        if (rec.selected)
            text = "> " + text;
        char name[40];
        std::snprintf(name, sizeof(name), "conquest_grp_%d", rec.id);
        const Vector3 p = grp->Leader()->Position();
        const auto it = r.groupMarkerText.find(rec.id);
        if (it == r.groupMarkerText.end())
        {
            std::snprintf(code, sizeof(code),
                          "createMarker [\"%s\", [%.0f, %.0f]]; \"%s\" setMarkerType \"Dot\"; \"%s\" setMarkerColor \"%s\"",
                          name, p.X(), p.Z(), name, name, Colour(rec.side));
            gs->Execute(code);
        }
        else
        {
            std::snprintf(code, sizeof(code), "\"%s\" setMarkerPos [%.0f, %.0f]", name, p.X(), p.Z());
            gs->Execute(code);
        }
        if (it == r.groupMarkerText.end() || it->second != text)
        {
            std::snprintf(code, sizeof(code), "\"%s\" setMarkerText \"%s\"", name, text.c_str());
            gs->Execute(code);
        }
        live[rec.id] = text;
    }
    for (const auto& [id, text] : r.groupMarkerText)
        if (!live.count(id))
        {
            std::snprintf(code, sizeof(code), "deleteMarker \"conquest_grp_%d\"", id);
            gs->Execute(code);
        }
    r.groupMarkerText = std::move(live);
}

void UpdateMarkers(Referee& r)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return;
    for (size_t i = 0; i < r.war.defs.size(); i++)
    {
        const ObjectiveDef& d = r.war.defs[i];
        const ObjectiveState& s = r.war.states[i];
        char area[48], label[48];
        std::snprintf(area, sizeof(area), "conquest_area_%d", static_cast<int>(i));
        std::snprintf(label, sizeof(label), "conquest_label_%d", static_cast<int>(i));
        std::string text = d.name;
        if (s.contested)
            text += " (contested)";
        else if (InProgress(s))
        {
            char pct[32];
            std::snprintf(pct, sizeof(pct), " %s %d%%", Name(s.holder).c_str(), static_cast<int>(s.level * 100.0f + 0.5f));
            text += pct;
        }
        if (r.markerOwner[i] == -2)
        {
            char code[512];
            std::snprintf(code, sizeof(code),
                          "createMarker [\"%s\", [%.0f, %.0f]]; \"%s\" setMarkerShape \"ELLIPSE\"; \"%s\" setMarkerSize "
                          "[%.0f, %.0f]; createMarker [\"%s\", [%.0f, %.0f]]; \"%s\" setMarkerType \"Dot\"",
                          area, d.x, d.y, area, area, d.radius, d.radius, label, d.x, d.y - d.radius - 60.0f, label);
            gs->Execute(code);
        }
        if (r.markerOwner[i] != s.owner)
        {
            char code[160];
            std::snprintf(code, sizeof(code), "\"%s\" setMarkerColor \"%s\"; \"%s\" setMarkerColor \"%s\"", area,
                          Colour(s.owner), label, Colour(s.owner));
            gs->Execute(code);
            r.markerOwner[i] = s.owner;
        }
        if (r.markerText[i] != text)
        {
            char code[192];
            std::snprintf(code, sizeof(code), "\"%s\" setMarkerText \"%s\"", label, text.c_str());
            gs->Execute(code);
            r.markerText[i] = text;
        }
    }
}

// ---- the tick ----------------------------------------------------------------------------

void Measure(Referee& r, std::vector<Presence>& presence)
{
    const size_t n = r.war.defs.size();
    presence.assign(n, Presence());
    std::vector<std::array<int, MaxSides>> near(n);
    r.live.fill(0);
    const float security2 = r.tuning.securityRadius * r.tuning.securityRadius;
    for (int s = 0; s < r.war.sideCount; s++)
    {
        AICenter* center = GWorld->GetCenter(r.config.engineSide[s]);
        if (!center)
            continue;
        for (int g = 0; g < center->NGroups(); g++)
        {
            AIGroup* grp = center->GetGroup(g);
            if (!grp)
                continue;
            for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            {
                AIUnit* unit = grp->UnitWithID(id);
                if (!Alive(unit))
                    continue;
                r.live[s]++;
                const Vector3 p = unit->Position();
                for (size_t o = 0; o < n; o++)
                {
                    const ObjectiveDef& d = r.war.defs[o];
                    const float dx = p.X() - d.x, dy = p.Z() - d.y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 <= d.radius * d.radius)
                        presence[o].strength[s] += 1.0f;
                    if (d2 <= security2)
                        near[o][s]++;
                }
            }
        }
    }
    for (size_t o = 0; o < n; o++)
        for (int s = 0; s < r.war.sideCount; s++)
        {
            int enemies = 0;
            for (int e = 0; e < r.war.sideCount; e++)
                if (e != s)
                    enemies += near[o][e];
            r.war.states[o].enemiesNear[s] = enemies;
        }
}

void TrackUnits(Referee& r, float now)
{
    const Vector3 camera = GScene && GScene->GetCamera() ? GScene->GetCamera()->Position() : VZero;
    for (size_t i = 0; i < r.units.size();)
    {
        UnitRecord& u = r.units[i];
        EntityAI* unit = u.unit;
        if (!unit)
        {
            r.units.erase(r.units.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (u.deadAt < 0.0f)
        {
            if (unit->IsDammageDestroyed())
            {
                u.deadAt = now;
                if (!u.vehicle)
                {
                    r.losses[u.side]++;
                    RecordLoss(r.war, u.side, r.tuning);
                }
            }
            else
            {
                const float damage = unit->GetTotalDammage();
                if (damage > u.lastDamage + 0.01f)
                    for (size_t o = 0; o < r.war.defs.size(); o++)
                        if (r.war.states[o].owner == u.side && Dist(unit->Position(), r.war.defs[o]) <= r.tuning.securityRadius)
                            r.war.states[o].lastHitAt = now;
                u.lastDamage = damage;
            }
        }
        else if (now - u.deadAt > BodySeconds && unit->Position().Distance(camera) > BodyHideDistance)
        {
            DeleteVehicle(unit);
            r.units.erase(r.units.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

void TrackGroups(Referee& r, float now)
{
    for (size_t i = 0; i < r.groups.size();)
    {
        GroupRecord& rec = r.groups[i];
        AIGroup* grp = rec.group;
        if (!grp || grp->NUnits() == 0 || !grp->Leader())
        {
            if (grp && grp->NUnits() == 0)
                grp->RemoveFromCenter();
            r.groups.erase(r.groups.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // Mounted squads get out once they are close: a truck is not a place to fight from.
        if (grp->NVehicles() > 0 && rec.task != TaskKind::None &&
            std::hypot(grp->Leader()->Position().X() - rec.destination.X(),
                       grp->Leader()->Position().Z() - rec.destination.Z()) < DismountDistance)
        {
            bool mounted = false;
            for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
                if (AIUnit* unit = grp->UnitWithID(id); unit && !unit->IsFreeSoldier())
                {
                    unit->OrderGetIn(false);
                    mounted = true;
                }
            if (mounted)
                grp->GetInVehicles();
        }
        if (rec.task == TaskKind::Move)
        {
            if (now >= rec.lockedUntil)
            {
                rec.task = TaskKind::None; // the commander's point order has run out: back to the AI
                r.replan[rec.side] = true;
            }
            else if (std::hypot(grp->Leader()->Position().X() - rec.destination.X(),
                                grp->Leader()->Position().Z() - rec.destination.Z()) > 30.0f &&
                     now - rec.orderedAt > ReorderSeconds && grp->MainSubgroup() && !grp->MainSubgroup()->HasCommand())
                OrderPoint(rec, rec.destination, now);
        }
        else if (rec.task != TaskKind::None && rec.objective >= 0)
        {
            const ObjectiveDef& d = r.war.defs[rec.objective];
            const float dist = Dist(grp->Leader()->Position(), d);
            if (dist < rec.bestDistance - 25.0f)
            {
                rec.bestDistance = dist;
                rec.bestAt = now;
            }
            if (rec.task == TaskKind::Attack && dist > d.radius && now - rec.bestAt > StallSeconds && !rec.stalled)
            {
                rec.stalled = true;
                r.replan[rec.side] = true;
                WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stalled on the way to " + d.name);
            }
            // The order can be used up (reached, or replaced by combat); give it again if still far.
            if (!grp->IsAnyPlayerGroup() && dist > d.radius * 0.8f && now - rec.orderedAt > ReorderSeconds && grp->MainSubgroup() &&
                !grp->MainSubgroup()->HasCommand())
                Order(rec, d, now);
        }
        i++;
    }
}

SideView BuildView(Referee& r, int side, float now)
{
    SideView v;
    v.side = side;
    v.now = now;
    v.war = &r.war;
    v.pool = r.war.pool[side];
    v.live = r.live[side];
    v.cap = r.config.cap;
    v.squadCost = r.config.squads[0].cost;
    v.squadSize = static_cast<int>(r.config.squads[0].classes[side].size());
    const size_t n = r.war.defs.size();
    v.knownEnemy.assign(n, 0.0f);
    v.canSpawn.assign(n, false);
    for (size_t o = 0; o < n; o++)
        v.canSpawn[o] = SpawnEligible(r.war, side, static_cast<int>(o), now, r.tuning);
    // What this side KNOWS: its centre's target database, never the world's unit list.
    if (AICenter* center = GWorld->GetCenter(r.config.engineSide[side]))
        for (int t = 0; t < center->NTargets(); t++)
        {
            const AITargetInfo& info = center->GetTarget(t);
            if (info._destroyed || info._vanished || !center->IsEnemy(info._side))
                continue;
            if (Glob.time - info._time > KnowledgeAgeSeconds)
                continue;
            for (size_t o = 0; o < n; o++)
            {
                const ObjectiveDef& d = r.war.defs[o];
                if (std::hypot(info._pos.X() - d.x, info._pos.Z() - d.y) <= d.radius + KnowledgeReach)
                    v.knownEnemy[o] += 1.0f;
            }
        }
    for (const GroupRecord& rec : r.groups)
    {
        if (rec.side != side)
            continue;
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader())
            continue;
        GroupView g;
        g.id = rec.id;
        const Vector3 p = grp->Leader()->Position();
        g.x = p.X();
        g.y = p.Z();
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (Alive(grp->UnitWithID(id)))
                g.men++;
        g.task = rec.task;
        g.objective = rec.objective;
        g.stalled = rec.stalled;
        g.locked = now < rec.lockedUntil || grp->IsAnyPlayerGroup();
        v.groups.push_back(g);
    }
    return v;
}

// What an AI commander buys at `base`: by the distance to its front, the fastest kind it can
// afford (air over 2.5 km, motorised over 900 m, armour when rich), else a rifle squad.
int ChooseSquadType(const Referee& r, int side, int base)
{
    float front = 1.0e9f;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (CanAttack(r.war, side, static_cast<int>(o)))
            front = std::min(front, std::hypot(r.war.defs[o].x - r.war.defs[base].x, r.war.defs[o].y - r.war.defs[base].y));
    const float pool = r.war.pool[side];
    const auto affordable = [&](int t)
    {
        const SquadDef& d = r.config.squads[t];
        return d.aiBuys && !d.classes[side].empty() && pool >= d.cost &&
               r.live[side] + static_cast<int>(d.classes[side].size()) <= r.config.cap;
    };
    std::vector<const char*> wanted;
    if (front > 2500.0f)
        wanted = {"air", "motorised", "mechanised"};
    else if (front > 900.0f)
        wanted = {"motorised", "mechanised"};
    for (const char* role : wanted)
        for (size_t t = 0; t < r.config.squads.size(); t++)
            if (r.config.squads[t].role == role && affordable(static_cast<int>(t)))
                return static_cast<int>(t);
    for (size_t t = 0; t < r.config.squads.size(); t++)
        if (r.config.squads[t].role == "mechanised" && pool >= 2.0f * r.config.squads[t].cost && affordable(static_cast<int>(t)))
            return static_cast<int>(t);
    return affordable(0) ? 0 : -1;
}

void Plan(Referee& r, int side, float now)
{
    const SideView view = BuildView(r, side, now);
    const std::vector<Intent> intents = PlanStandard(view, r.commander);
    for (const Intent& intent : intents)
    {
        if (intent.kind == Intent::Kind::BuySquad)
        {
            // While the player commands this side, its supplies are theirs to spend.
            if (side == r.playerSide && r.humanCommander)
                continue;
            std::string why;
            if (!SpawnEligible(r.war, side, intent.objective, now, r.tuning, &why))
            {
                WarLog(Name(side) + " purchase at " + r.war.defs[intent.objective].name + " refused: " + why);
                continue;
            }
            const int type = ChooseSquadType(r, side, intent.objective);
            if (type < 0)
                continue;
            if (SpawnSquad(r, side, intent.objective, false, type))
                WarLog(Name(side) + " bought a " + r.config.squads[type].name + " at " +
                       r.war.defs[intent.objective].name + " (" + intent.why + ")");
            continue;
        }
        for (GroupRecord& rec : r.groups)
        {
            if (rec.id != intent.group)
                continue;
            if (now < rec.lockedUntil || (rec.group && rec.group->IsAnyPlayerGroup()))
                break; // the human commander's order stands, or the player leads this squad
            rec.task = intent.task;
            rec.objective = intent.objective;
            rec.stalled = false;
            rec.bestDistance = 1.0e9f;
            rec.bestAt = now;
            if (intent.objective >= 0)
            {
                Order(rec, r.war.defs[intent.objective], now);
                WarLog(Name(side) + " group " + std::to_string(rec.id) + ": " + TaskName(intent.task) + " " +
                    r.war.defs[intent.objective].name + " (" + intent.why + ")");
            }
            break;
        }
    }
    r.lastPlan[side] = now;
    r.replan[side] = false;
}

void Status(Referee& r)
{
    LOG_INFO(Mission, "Conquest status: {}", StatusText());
    // POSEIDON_CONQUEST_DIAG=1: what each group's FSMs are doing, for "why does it not move".
    static const bool diag = std::getenv("POSEIDON_CONQUEST_DIAG") != nullptr;
    if (!diag)
        return;
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader())
            continue;
        const auto* gcur = grp->GetCurrent();
        AISubgroup* sub = grp->MainSubgroup();
        const auto* scur = sub ? sub->GetCurrent() : nullptr;
        const Vector3 p = grp->Leader()->Position();
        LOG_INFO(Mission,
                 "Conquest diag: group {} {} at {:.0f},{:.0f} subgroups {} group fsm '{}' subgroup task {} fsm '{}' "
                 "dest {:.0f},{:.0f} leader in main {} | leader plan {} state {} mode {} disabled {} path {} wanted "
                 "{:.0f},{:.0f} away {}",
                 rec.id, Name(rec.side), p.X(), p.Z(), grp->NSubgroups(),
                 gcur && gcur->_fsm ? gcur->_fsm->GetStateName() : "-", scur && scur->_task ? int(scur->_task->_message) : -1,
                 scur && scur->_fsm ? scur->_fsm->GetStateName() : "-", rec.destination.X(), rec.destination.Z(),
                 grp->Leader()->GetSubgroup() == sub, int(grp->Leader()->GetPlanningMode()),
                 int(grp->Leader()->GetState()), int(grp->Leader()->GetMode()), grp->Leader()->GetAIDisabled(),
                 grp->Leader()->GetPath().Size(), grp->Leader()->GetWantedPosition().X(),
                 grp->Leader()->GetWantedPosition().Z(), grp->Leader()->IsAway());
    }
}

void Refresh(Referee& r, float now)
{
    Snapshot& s = r.snapshot;
    s.active = r.active;
    s.now = now;
    s.war = r.war;
    s.tuning = r.tuning;
    s.sideNames.clear();
    for (int i = 0; i < r.war.sideCount; i++)
        s.sideNames.emplace_back(SideLabel(r.config.engineSide[i]));
    s.live = r.live;
    s.losses = r.losses;
    s.bought = r.bought;
    s.cap = r.config.cap;
    s.groups.clear();
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader())
            continue;
        GroupSummary g;
        g.id = rec.id;
        g.side = rec.side;
        g.men = grp->NUnits();
        g.task = rec.task;
        g.objective = rec.objective;
        g.stalled = rec.stalled;
        g.x = grp->Leader()->Position().X();
        g.y = grp->Leader()->Position().Z();
        s.groups.push_back(g);
    }
}

void Tick(Referee& r, float dt, float now)
{
    if (!r.started)
    {
        r.started = true;
        for (int s = 0; s < r.war.sideCount; s++)
            for (size_t o = 0; o < r.war.defs.size(); o++)
                if (r.war.states[o].owner == s && r.war.defs[o].kind == ObjectiveKind::HQ)
                    for (int type : r.config.startSquads)
                        SpawnSquad(r, s, static_cast<int>(o), true, type);
        if (Person* player = GWorld->GetRealPlayer())
            for (int s = 0; s < r.war.sideCount; s++)
                if (r.config.engineSide[s] == player->GetTargetSide())
                    r.playerSide = s;
        WarLog("the war begins");
        for (int s = 0; s < r.war.sideCount; s++)
            r.replan[s] = true;
    }

    std::vector<Presence> presence;
    Measure(r, presence);
    TrackUnits(r, now);
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const int before = r.war.states[o].owner;
        const CaptureResult result = StepCapture(r.war, static_cast<int>(o), presence[o], dt, now, r.tuning);
        if (result.event == CaptureEvent::None)
            continue;
        const std::string& name = r.war.defs[o].name;
        if (result.event == CaptureEvent::Captured)
            WarLog(Name(result.side) + " captured " + name);
        else
            WarLog(Name(result.side) + " neutralised " + name + " (was " + Name(before) + ")");
        for (int s = 0; s < r.war.sideCount; s++)
            r.replan[s] = true;
    }
    const int winner = StepEconomy(r.war, dt, r.tuning);
    if (winner != NoSide && !r.ended)
    {
        r.ended = true;
        WarLog(Name(winner) + " wins the war");
        Status(r);
    }
    TrackGroups(r, now);
    if (!r.ended)
        for (int s = 0; s < r.war.sideCount; s++)
            if (!r.war.defeated[s] && (r.replan[s] || now - r.lastPlan[s] >= r.config.planSeconds))
                Plan(r, s, now);
    UpdateMarkers(r);
    UpdateGroupMarkers(r);
    if (now - r.lastStatus >= StatusSeconds)
    {
        r.lastStatus = now;
        Status(r);
    }
}
} // namespace

void Simulate()
{
    Referee& r = R();
    if (!GWorld || GWorld->GetMode() != GModeArcade || !GWorld->GetGameState() || !GLandscape)
        return;
    const std::string mission(static_cast<const char*>(GetMissionDirectory()));
    if (mission != r.mission)
    {
        const std::string previous = r.mission;
        r = Referee();
        r.mission = mission;
        if (const ParamEntry* cfg = ExtParsMission.FindEntry("CfgConquest"); cfg && cfg->IsClass())
            r.active = LoadConfig(r, *cfg);
        r.snapshot.active = r.active;
    }
    if (!r.active)
        return;
    const float now = Now();
    if (r.lastTick < 0.0f)
        r.lastTick = now;
    if (now < r.lastTick - 1.0f)
    {
        // A loaded save or a restart: the war's state is not saved yet (Slice 1), so stop rather
        // than run it against a world that no longer matches.
        WarLog("game time went backwards (save loaded?); Conquest stops until the mission restarts");
        r.active = false;
        r.snapshot.active = false;
        return;
    }
    if (now - r.lastTick < TickSeconds && r.started)
        return;
    const float dt = std::clamp(now - r.lastTick, 0.0f, 5.0f);
    r.lastTick = now;
    const auto t0 = std::chrono::steady_clock::now();
    Tick(r, dt, now);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Refresh(r, now);
    r.snapshot.lastTickMs = ms;
    r.snapshot.maxTickMs = std::max(r.snapshot.maxTickMs, ms);
}

const Snapshot& GetSnapshot()
{
    return R().snapshot;
}

namespace
{
void Hint(const std::string& text)
{
    if (GameState* gs = GWorld ? GWorld->GetGameState() : nullptr)
    {
        std::string code = "hint \"";
        for (char c : text)
            code += c == '"' ? '\'' : c;
        code += "\"";
        gs->Execute(code.c_str());
    }
}

std::string Describe(const Referee& r, const GroupRecord& rec)
{
    AIGroup* grp = rec.group;
    int men = 0;
    for (int id = 1; grp && id <= MAX_UNITS_PER_GROUP; id++)
        if (Alive(grp->UnitWithID(id)))
            men++;
    std::string text = Name(rec.side).substr(0, 1) + std::to_string(rec.id) + " (" + std::to_string(men) + ")";
    if (rec.task != TaskKind::None && rec.objective >= 0)
        text += " " + std::string(TaskName(rec.task)) + " " + r.war.defs[rec.objective].name;
    return text;
}
} // namespace

namespace
{
// The human commander's order for these squads: attack or defend the objective at (x, y), or
// move to the point. Locks them against the AI planner. Returns what was ordered, for the hint.
std::string OrderRecords(Referee& r, const std::vector<GroupRecord*>& records, float x, float y)
{
    const float now = Now();
    int target = -1;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (std::hypot(r.war.defs[o].x - x, r.war.defs[o].y - y) <= r.war.defs[o].radius * 1.2f)
            target = static_cast<int>(o);
    std::string text;
    for (GroupRecord* rec : records)
    {
        rec->lockedUntil = now + HumanLockSeconds;
        rec->stalled = false;
        rec->bestDistance = 1.0e9f;
        rec->bestAt = now;
        if (target >= 0)
        {
            rec->task = r.war.states[target].owner == r.playerSide ? TaskKind::Defend : TaskKind::Attack;
            rec->objective = target;
            Order(*rec, r.war.defs[target], now);
        }
        else
        {
            rec->task = TaskKind::Move;
            rec->objective = -1;
            OrderPoint(*rec, Ground(x, y), now);
        }
        rec->selected = false;
        text += " " + Describe(r, *rec);
        WarLog("commander: " + Describe(r, *rec) + (target >= 0 ? "" : " move"));
    }
    if (target >= 0 && r.war.states[target].owner != r.playerSide && !CanAttack(r.war, r.playerSide, target))
        text += ". Not on the front yet: it cannot be captured until a neighbouring objective is ours";
    r.replan[r.playerSide] = true;
    return text;
}
} // namespace

bool MapClick(float x, float y, bool shift, bool alt)
{
    Referee& r = R();
    if (!r.active || r.playerSide == NoSide)
        return false;

    // A click on one of our squads selects it (Shift adds to the selection); Alt always orders.
    GroupRecord* picked = nullptr;
    float best = PickRadius;
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != r.playerSide || !grp || !grp->Leader())
            continue;
        const Vector3 p = grp->Leader()->Position();
        const float d = std::hypot(p.X() - x, p.Z() - y);
        if (d < best)
        {
            best = d;
            picked = &rec;
        }
    }
    if (picked && !alt)
    {
        if (!shift)
            for (GroupRecord& rec : r.groups)
                rec.selected = false;
        picked->selected = !(shift && picked->selected);
        std::string text = "Selected:";
        for (const GroupRecord& rec : r.groups)
            if (rec.selected)
                text += " " + Describe(r, rec) + ";";
        Hint(text + " Click an objective to attack or defend it, anywhere else to move. Shift adds a squad.");
        return true;
    }

    std::vector<GroupRecord*> selected;
    for (GroupRecord& rec : r.groups)
        if (rec.selected && rec.group)
            selected.push_back(&rec);
    if (selected.empty())
    {
        Hint("Commander: click one of your squads on the map first.");
        return true;
    }
    Hint("Orders sent:" + OrderRecords(r, selected, x, y) + ". The AI leaves these squads to you for 10 minutes.");
    return true;
}

namespace
{
// Hand the player's control and camera to `person`, the way the engine's own player switch does.
void TakeBody(Person* person)
{
    AIUnit* brain = person ? person->Brain() : nullptr;
    if (!brain || !GWorld)
        return;
    GWorld->SwitchCameraTo(brain->GetVehicle(), CamInternal);
    GWorld->SetPlayerManual(true);
    GWorld->SwitchPlayerTo(person);
    GWorld->SetRealPlayer(person);
    if (GWorld->UI())
        GWorld->UI()->ResetVehicle(brain->GetVehicle());
}
} // namespace

bool PossessionAllowed()
{
    return R().active && R().config.possession;
}

void SetPossessionAllowed(bool allowed)
{
    R().config.possession = allowed;
}

bool Possessing()
{
    const Referee& r = R();
    return r.active && r.commanderBody && GWorld && GWorld->GetRealPlayer() != r.commanderBody.GetLink();
}

bool Possess(int groupId, std::string* reason)
{
    Referee& r = R();
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (!r.active || r.playerSide == NoSide || !GWorld)
        return refuse("no war");
    if (!r.config.possession)
        return refuse("taking control of soldiers is switched off");
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.id != groupId || rec.side != r.playerSide || !grp || !grp->Leader())
            continue;
        Person* person = grp->Leader()->GetPerson();
        if (!person || grp->Leader()->GetLifeState() != AIUnit::LSAlive)
            return refuse("its leader is down");
        if (!Possessing())
            r.commanderBody = GWorld->GetRealPlayer();
        TakeBody(person);
        WarLog("commander: took control of " + Describe(r, rec));
        return true;
    }
    return refuse("no such squad");
}

bool ReturnToCommander()
{
    Referee& r = R();
    Person* body = r.commanderBody;
    if (!body || !body->Brain() || body->Brain()->GetLifeState() != AIUnit::LSAlive)
        return false;
    if (GWorld->GetRealPlayer() != body)
        TakeBody(body);
    return true;
}

bool OnRealPlayerDead()
{
    Referee& r = R();
    if (!r.active || !r.commanderBody || !Possessing())
        return false;
    if (!ReturnToCommander())
        return false;
    WarLog("commander: the soldier you controlled was killed; back in command");
    r.reopenView = true;
    return true;
}

bool TakeReopenViewRequest()
{
    const bool want = R().reopenView;
    R().reopenView = false;
    return want;
}

void SetHumanCommander(bool on)
{
    R().humanCommander = on;
}

int PlayerSide()
{
    return R().active ? R().playerSide : NoSide;
}

void ForEachGroup(const std::function<void(const LiveGroup&)>& fn)
{
    Referee& r = R();
    if (!r.active)
        return;
    const float now = Now();
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader())
            continue;
        LiveGroup g;
        g.id = rec.id;
        g.side = rec.side;
        g.group = grp;
        g.task = rec.task;
        g.objective = rec.objective;
        g.locked = now < rec.lockedUntil;
        g.stalled = rec.stalled;
        g.destX = rec.destination.X();
        g.destY = rec.destination.Z();
        if (rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size()))
        {
            const SquadDef& def = r.config.squads[rec.type];
            g.typeName = def.name;
            g.role = def.role;
            g.size = static_cast<int>(def.classes[rec.side].size());
        }
        g.playerLed = grp->IsAnyPlayerGroup();
        fn(g);
    }
}

int OrderGroups(const std::vector<int>& ids, float x, float y)
{
    Referee& r = R();
    if (!r.active || r.playerSide == NoSide)
        return 0;
    std::vector<GroupRecord*> records;
    for (GroupRecord& rec : r.groups)
        if (rec.side == r.playerSide && rec.group && std::find(ids.begin(), ids.end(), rec.id) != ids.end())
            records.push_back(&rec);
    if (records.empty())
        return 0;
    OrderRecords(r, records, x, y);
    return static_cast<int>(records.size());
}

std::vector<SquadInfo> SquadTypes()
{
    std::vector<SquadInfo> out;
    const Referee& r = R();
    if (!r.active || r.playerSide == NoSide)
        return out;
    for (const SquadDef& def : r.config.squads)
        out.push_back({def.name, def.cost, static_cast<int>(def.classes[r.playerSide].size())});
    return out;
}

bool CanBuy(int base, int type, std::string* reason)
{
    const Referee& r = R();
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (!r.active || r.playerSide == NoSide)
        return refuse("no war");
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
        return refuse("no such squad");
    const SquadDef& def = r.config.squads[type];
    if (!SpawnEligible(r.war, r.playerSide, base, Now(), r.tuning, reason))
        return false;
    if (r.war.pool[r.playerSide] < def.cost)
        return refuse("not enough supplies");
    if (r.live[r.playerSide] + static_cast<int>(def.classes[r.playerSide].size()) > r.config.cap)
        return refuse("manpower cap reached");
    return true;
}

int BuySquad(int base, int type, std::string* reason)
{
    Referee& r = R();
    if (!CanBuy(base, type, reason))
        return 0;
    const int id = SpawnSquad(r, r.playerSide, base, false, type);
    if (!id)
    {
        if (reason)
            *reason = "the squad could not be created";
        return 0;
    }
    // A squad the commander bought waits at its base for orders, like a factory's output.
    for (GroupRecord& rec : r.groups)
        if (rec.id == id)
        {
            rec.task = TaskKind::Defend;
            rec.objective = base;
            rec.lockedUntil = Now() + HumanLockSeconds;
        }
    WarLog("commander: bought a " + r.config.squads[type].name + " at " + r.war.defs[base].name);
    return id;
}

int SquadTypeIndex(const std::string& id)
{
    const Referee& r = R();
    for (size_t t = 0; t < r.config.squads.size(); t++)
        if (_stricmp(r.config.squads[t].id.c_str(), id.c_str()) == 0 ||
            _stricmp(r.config.squads[t].name.c_str(), id.c_str()) == 0)
            return static_cast<int>(t);
    return -1;
}

std::vector<Contact> KnownEnemies(int side)
{
    std::vector<Contact> out;
    const Referee& r = R();
    if (!r.active || side == NoSide || !GWorld)
        return out;
    AICenter* center = GWorld->GetCenter(r.config.engineSide[side]);
    if (!center)
        return out;
    for (int t = 0; t < center->NTargets(); t++)
    {
        const AITargetInfo& info = center->GetTarget(t);
        if (info._destroyed || info._vanished || !center->IsEnemy(info._side))
            continue;
        const float age = Glob.time - info._time;
        if (age > KnowledgeAgeSeconds)
            continue;
        Contact c;
        c.x = info._pos.X();
        c.y = info._pos.Z();
        c.height = info._pos.Y();
        c.age = age;
        c.vehicle = info._type && !info._type->IsKindOf(GWorld->Preloaded(VTypeMan));
        out.push_back(c);
    }
    return out;
}

std::string StatusText()
{
    const Referee& r = R();
    if (!r.active)
        return "inactive";
    std::string out;
    char buf[160];
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const ObjectiveState& s = r.war.states[o];
        std::snprintf(buf, sizeof(buf), "%s=%s%s; ", r.war.defs[o].name.c_str(), Name(s.owner).c_str(),
                      s.contested ? "!" : InProgress(s) ? "~" : "");
        out += buf;
    }
    for (int s = 0; s < r.war.sideCount; s++)
    {
        std::snprintf(buf, sizeof(buf), "| %s pool %.0f score %.0f live %d lost %d bought %d%s ", Name(s).c_str(),
                      r.war.pool[s], r.war.score[s], r.live[s], r.losses[s], r.bought[s],
                      r.war.defeated[s] ? " DEFEATED" : "");
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "| groups %d | tick %.2f ms (max %.2f)", static_cast<int>(r.groups.size()),
                  r.snapshot.lastTickMs, r.snapshot.maxTickMs);
    out += buf;
    return out;
}
} // namespace Poseidon::Conquest

static GameValue ConquestStateNular(const GameState*)
{
    return GameValue(RString(Poseidon::Conquest::StatusText().c_str()));
}

// conquestMapClick [_pos, _shift, _alt]: the commander's map click (onMapSingleClick); true when
// Conquest used the click, so the map does not also move the player's own group.
static GameValue ConquestMapClickUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 3 || a[0].GetType() != GameArray)
        return GameValue(false);
    const GameArrayType& pos = a[0];
    if (pos.Size() < 2)
        return GameValue(false);
    const bool shift = a[1].GetType() == GameBool && static_cast<bool>(a[1]);
    const bool alt = a[2].GetType() == GameBool && static_cast<bool>(a[2]);
    return GameValue(Poseidon::Conquest::MapClick(static_cast<float>(pos[0]), static_cast<float>(pos[1]), shift, alt));
}

// conquestBuy ["Morton", "Motorised"]: the player's side buys that squad type (config class name)
// at that objective (its name). Returns the new squad's id, or the reason it was refused.
static GameValue ConquestBuyUnary(const GameState*, GameValuePar oper1)
{
    using namespace Poseidon::Conquest;
    const GameArrayType& a = oper1;
    if (a.Size() < 2 || a[0].GetType() != GameString || a[1].GetType() != GameString)
        return GameValue(RString("usage: conquestBuy [objective, squad type]"));
    const std::string base = static_cast<const char*>(RString(a[0]));
    const std::string type = static_cast<const char*>(RString(a[1]));
    const Snapshot& snap = GetSnapshot();
    int baseIndex = -1;
    for (size_t o = 0; o < snap.war.defs.size(); o++)
        if (_stricmp(snap.war.defs[o].name.c_str(), base.c_str()) == 0)
            baseIndex = static_cast<int>(o);
    const int typeIndex = SquadTypeIndex(type);
    if (baseIndex < 0 || typeIndex < 0)
        return GameValue(RString("unknown objective or squad type"));
    std::string why;
    const int id = BuySquad(baseIndex, typeIndex, &why);
    return id ? GameValue(static_cast<float>(id)) : GameValue(RString(why.c_str()));
}

// conquestTakeControl id: the player takes over the leader of their side's squad `id`
// (conquestCommanderView true brings them back). True, or the reason it was refused.
static GameValue ConquestTakeControlUnary(const GameState*, GameValuePar oper1)
{
    std::string why;
    if (Poseidon::Conquest::Possess(static_cast<int>(static_cast<float>(oper1)), &why))
    {
        Poseidon::Conquest::View::SetActive(false); // as the HUD's "Take control" does: the soldier's eyes
        return GameValue(true);
    }
    return GameValue(RString(why.c_str()));
}

INIT_MODULE(ConquestReferee, 3)
{
    GGameState.NewFunction(GameFunction(GameAny, "conquestTakeControl", ConquestTakeControlUnary, GameScalar));
    GGameState.NewFunction(GameFunction(GameAny, "conquestBuy", ConquestBuyUnary, GameArray));
    GGameState.NewNularOp(GameNular(GameString, "conquestState", ConquestStateNular));
    GGameState.NewFunction(GameFunction(GameBool, "conquestMapClick", ConquestMapClickUnary, GameArray));
};
