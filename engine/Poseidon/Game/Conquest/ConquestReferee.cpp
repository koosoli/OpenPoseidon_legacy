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
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/AI/AIRadio.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
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
    int type = 0;                // index into Config::squads; -1: a player squad (multiplayer slots)
    int hqObjective = -1;        // a player squad: the objective HQ last sent it to
    std::string hqText;          // ... and the order as the soldier's HUD shows it
    float hqX = 0.0f, hqY = 0.0f;
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
    bool mounted = false; // the crew lives in its vehicle (tanks, gunships): never gets out
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
    bool setup = true;        // the mission opens with the side and options screen (setup = 0: config defaults)
    Options defaults;         // the setup screen's starting choices (startAs, possession, fog ...)
    std::string postClass = "Camp";        // each objective's command post: what squads must stand by
    std::string flagClass = "FlagCarrier"; // beside it, flying the owner's flag
    std::array<std::string, MaxSides> flagTexture;
    float barrageCost = 150.0f;
    float barrageCooldown = 120.0f;
    int barrageShells = 8;
    float barrageSpread = 35.0f;
    std::string barrageAmmo = "Shell120";
    std::array<std::string, MaxSides> captureVehicle; // the free troop transport a captured base gives
    std::array<std::string, MaxSides> arsenalClass;   // the owner's weapons and ammunition box at each post
    float arsenalRestock = 600.0f;                    // seconds until a post's box is replaced by a full one
};

// A line of HQ's radio, kept for the clients in multiplayer.
struct NetRadio
{
    int seq = 0;
    int side = NoSide;
    std::string text;
};

// An objective's command post: a tent at its capture centre and the owner's flag beside it.
struct Post
{
    OLink<EntityAI> tent;
    OLink<EntityAI> flag;
    int flown = -2; // the side whose flag flies (-2: not set yet)
    OLink<EntityAI> arsenal; // the owner's box: the inventory's vicinity pane takes and leaves gear in it
    int arsenalSide = -2;
    float arsenalAt = -1.0e9f; // when it was (re)stocked
};

struct PendingShell
{
    float at = 0.0f; // game time it comes down
    float x = 0.0f, y = 0.0f;
};

// The shroud: which 100 m cells a soldier of the player's side has been near.
constexpr float ShroudCell = 100.0f;
constexpr float ShroudReveal = 350.0f;

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
    std::array<float, MaxSides> humanUntil{-1.0e9f, -1.0e9f, -1.0e9f}; // a human holds command of the side until then
    bool localCommand = false;    // a multiplayer client: this player has the commander view open
    float lastCommandBeat = -1.0e9f;
    OLink<Person> commanderBody;  // the player's own body while they are inside a soldier
    bool setupPending = false;    // waiting for the setup screen's Start
    Options options;
    bool viewRequest = false;     // the commander view should open (start in command, a death)
    std::string viewNotice;
    std::string soldierNotice;
    std::vector<Post> posts;      // per objective
    std::vector<uint8_t> explored; // the shroud, shroudSize x shroudSize cells
    int shroudSize = 0;
    std::array<float, MaxSides> lastBarrage{-1.0e9f, -1.0e9f, -1.0e9f};
    std::vector<PendingShell> shells;
    // multiplayer: what the server publishes, and what a client mirrors of it
    int netSeq = 0;
    float lastPublish = -1.0e9f;
    std::vector<NetRadio> radioLog;
    int radioSeq = 0;
    int radioSeen = 0;
    std::vector<std::array<std::string, MaxSides>> spawnWhy; // per objective: why each side may not deploy
    std::array<float, MaxSides> netCooldown{};
    std::array<bool, MaxSides> netHuman{};
    std::array<std::vector<Contact>, MaxSides> netContacts;
    float mirrorSince = 0.0f;
    bool netTestSent = false;
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

// The war as a loaded save holds it, until Simulate has the mission's CfgConquest to continue it in.
struct Saved
{
    bool loaded = false; // a save was loaded since the last Simulate
    bool valid = false;  // ... and it held a war
    std::string mission;
    bool started = false;
    bool ended = false;
    std::vector<ObjectiveState> states;
    std::array<float, MaxSides> pool{};
    std::array<float, MaxSides> score{};
    std::array<bool, MaxSides> defeated{};
    std::array<float, MaxSides> lastPlan{};
    std::array<int, MaxSides> live{};
    std::array<int, MaxSides> losses{};
    std::array<int, MaxSides> bought{};
    std::vector<GroupRecord> groups;
    std::vector<UnitRecord> units;
    int nextGroupId = 1;
    int playerSide = NoSide;
    OLink<Person> commanderBody;
    Options options;
    bool setupPending = false;
    bool viewOpen = false;
    std::vector<Post> posts;
    std::vector<float> postX, postY; // where each command post stands: the capture centre
    std::string explored;            // the shroud, one '0'/'1' per cell
    int shroudSize = 0;
    std::array<float, MaxSides> lastBarrage{-1.0e9f, -1.0e9f, -1.0e9f};
    float lastTick = -1.0f;
    float lastStatus = -1.0e9f;
    std::deque<std::string> log;
};

Saved& Pending()
{
    static Saved s;
    return s;
}

void ContinueSavedWar(Referee& r, const std::string& mission);
bool EnterSoldier(Referee& r, Person* person);
const GroupRecord* RecordOf(const Referee& r, const AIGroup* grp);
void Radio(Referee& r, int side, const std::string& text);

float Now();

// A human holds command of `side` (the commander view is open): its purchases are theirs, HQ is quiet.
bool Human(const Referee& r, int side)
{
    return side >= 0 && side < MaxSides && Now() < r.humanUntil[side];
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
    r.config.setup = Number(cfg, "setup", 1.0f) != 0.0f;
    r.config.defaults.possession = Number(cfg, "possession", 1.0f) != 0.0f;
    r.config.defaults.startInCommand = Text(cfg, "startAs", "commander") != "soldier";
    r.config.defaults.commanderView = Number(cfg, "commanderView", 1.0f) != 0.0f;
    r.config.defaults.hqRadio = Number(cfg, "hqRadio", 1.0f) != 0.0f;
    r.config.defaults.fog = Text(cfg, "fog", "contacts") == "shroud" ? FogMode::Shroud : FogMode::Contacts;
    r.config.postClass = Text(cfg, "postClass", r.config.postClass.c_str());
    r.config.flagClass = Text(cfg, "flagClass", r.config.flagClass.c_str());
    r.config.barrageCost = Number(cfg, "barrageCost", r.config.barrageCost);
    r.config.barrageCooldown = Number(cfg, "barrageCooldown", r.config.barrageCooldown);
    r.config.barrageShells = static_cast<int>(Number(cfg, "barrageShells", static_cast<float>(r.config.barrageShells)));
    r.config.barrageSpread = Number(cfg, "barrageSpread", r.config.barrageSpread);
    r.config.barrageAmmo = Text(cfg, "barrageAmmo", r.config.barrageAmmo.c_str());
    r.config.arsenalRestock = Number(cfg, "arsenalRestock", r.config.arsenalRestock);
    const float captureRadius = Number(cfg, "captureRadius", 30.0f);
    for (int s = 0; s < r.war.sideCount; s++)
    {
        const TargetSide engine = r.config.engineSide[s];
        const std::string label = SideLabel(engine);
        r.config.flagTexture[s] = Text(cfg, (label + "flag").c_str(),
                                       engine == TWest ? "usa_vlajka.pac" : engine == TEast ? "rus_vlajka.pac" : "");
        r.config.captureVehicle[s] = Text(cfg, (label + "captureVehicle").c_str(), engine == TEast ? "Ural" : "Truck5tOpen");
        r.config.arsenalClass[s] = Text(cfg, (label + "arsenal").c_str(),
                                        engine == TWest ? "ReammoBoxWest" : engine == TEast ? "ReammoBoxEast" : "ReammoBoxGuer");
    }
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
            def.mounted = Number(e, "mounted", def.role == "armour" || def.role == "gunship" ? 1.0f : 0.0f) != 0.0f;
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
        d.captureRadius = std::min(Number(o, "captureRadius", captureRadius), d.radius);
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
    r.options = r.config.defaults;
    if (Person* player = GWorld->GetRealPlayer())
        for (int s = 0; s < r.war.sideCount; s++)
            if (r.config.engineSide[s] == player->GetTargetSide())
                r.options.side = s;
    r.setupPending = r.config.setup;
    if (GWorld->GetMode() == GModeNetware)
    {
        // Multiplayer: every player has their own slot, side and respawn; no setup screen, no body swapping.
        r.setupPending = false;
        r.options.possession = false;
        r.options.startInCommand = false;
    }
    r.posts.assign(r.war.defs.size(), Post());
    r.spawnWhy.assign(r.war.defs.size(), std::array<std::string, MaxSides>());
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

// Where a group goes for its task: a spread of points round the command post, inside its capture
// circle, so squads do not stack and every one of them counts.
Vector3 TaskPoint(const ObjectiveDef& d, int groupId)
{
    const float angle = static_cast<float>(groupId) * 2.39996f; // golden angle
    const float reach = d.captureRadius * 0.5f;
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
    {
        group->AssignVehicles();
        if (squad.mounted)
            group->GetInVehicles(); // a tank crew starts in its tank
    }
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
                    if (d2 <= d.captureRadius * d.captureRadius)
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

// Does `side` know of an enemy, seen in the last minute, within 400 m of `pos`?
bool InContact(const Referee& r, int side, const Vector3& pos)
{
    AICenter* center = GWorld->GetCenter(r.config.engineSide[side]);
    if (!center)
        return false;
    for (int t = 0; t < center->NTargets(); t++)
    {
        const AITargetInfo& info = center->GetTarget(t);
        if (info._destroyed || info._vanished || !center->IsEnemy(info._side) || Glob.time - info._time > 60.0f)
            continue;
        if (std::hypot(info._pos.X() - pos.X(), info._pos.Z() - pos.Z()) < 400.0f)
            return true;
    }
    return false;
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
        // Mounted squads get out once they are close: a truck is not a place to fight from. Tank and
        // gunship crews fight from their vehicle and stay in it.
        const bool crew = rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size()) && r.config.squads[rec.type].mounted;
        if (!crew && grp->NVehicles() > 0 && rec.task != TaskKind::None &&
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
            // A squad trading fire on the way is busy, not stuck: soaks showed squads "stalled"
            // in front of a held town, re-tasked, and sent straight back into the same fight.
            if (dist < rec.bestDistance - 25.0f || InContact(r, rec.side, grp->Leader()->Position()))
            {
                rec.bestDistance = std::min(rec.bestDistance, dist);
                rec.bestAt = now;
            }
            if (rec.task == TaskKind::Attack && dist > d.radius && now - rec.bestAt > StallSeconds && !rec.stalled)
            {
                rec.stalled = true;
                r.replan[rec.side] = true;
                WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stalled on the way to " + d.name);
            }
            // The order can be used up (reached, or replaced by combat); give it again if still far.
            if (!grp->IsAnyPlayerGroup() && dist > d.captureRadius * 1.2f && now - rec.orderedAt > ReorderSeconds && grp->MainSubgroup() &&
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
    // Every fourth purchase is heavy: a tank platoon or a gunship, alternately. They cost a lot, so
    // the commander saves up for one unless the side is short of men.
    if (r.bought[side] % 4 == 3)
    {
        const char* heavy[2] = {"armour", "gunship"};
        const int first = (r.bought[side] / 4) % 2;
        for (int k = 0; k < 2; k++)
        {
            const char* role = heavy[(first + k) % 2];
            for (size_t t = 0; t < r.config.squads.size(); t++)
            {
                const SquadDef& d = r.config.squads[t];
                if (d.role != role || !d.aiBuys || d.classes[side].empty() ||
                    r.live[side] + static_cast<int>(d.classes[side].size()) > r.config.cap)
                    continue;
                if (pool >= d.cost)
                    return static_cast<int>(t);
                if (r.live[side] >= r.config.cap / 3)
                    return -1; // save up for it
            }
        }
    }
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
    std::map<std::pair<int, int>, std::vector<int>> radioed; // (task, objective) -> squads newly given it
    for (const Intent& intent : intents)
    {
        if (intent.kind == Intent::Kind::BuySquad)
        {
            // While the player commands this side, its supplies are theirs to spend.
            if (Human(r, side))
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
            if (rec.task != intent.task || rec.objective != intent.objective)
                radioed[{static_cast<int>(intent.task), intent.objective}].push_back(rec.id);
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
    if (!Human(r, side))
        for (const auto& [what, ids] : radioed)
        {
            if (what.second < 0)
                continue;
            std::string who;
            for (size_t i = 0; i < ids.size(); i++)
                who += (i == 0 ? "" : i + 1 == ids.size() ? " and " : ", ") + Callsign(ids[i]);
            const char* verb = static_cast<TaskKind>(what.first) == TaskKind::Defend ? "hold" : "attack";
            Radio(r, side, who + ", " + verb + " " + r.war.defs[what.second].name + ".");
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

// ---- command posts, the shroud, HQ's radio, mortars --------------------------------------

GameValue RunWith(const char* code, const GameValue& o, const GameValue& p, const RString& t = RString())
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return GameValue();
    GameVarSpace space;
    gs->BeginContext(&space);
    gs->VarSetLocal("_o", o, true, true);
    gs->VarSetLocal("_p", p, true, true);
    gs->VarSetLocal("_t", GameValue(t), true, true);
    const GameValue result = gs->EvaluateMultiple(code);
    gs->EndContext();
    return result;
}

GameValue Position(float x, float y, float z)
{
    GameValue pos = GWorld->GetGameState()->CreateGameValue(GameArray);
    GameArrayType& p = pos;
    p.Add(GameValue(x));
    p.Add(GameValue(y));
    p.Add(GameValue(z));
    return pos;
}

void FlyFlag(Referee& r, size_t o)
{
    Post& post = r.posts[o];
    const int owner = r.war.states[o].owner;
    if (post.flown == owner)
        return;
    post.flown = owner;
    if (EntityAI* flag = post.flag)
        flag->SetFlagTexture(owner == NoSide ? RString() : RString(r.config.flagTexture[owner].c_str()));
}

// Each objective gets its command post when the war begins: a tent on free ground near the centre,
// which becomes the objective's capture centre, and a flag of the owner beside it.
void PlacePosts(Referee& r)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        ObjectiveDef& d = r.war.defs[o];
        Post& post = r.posts[o];
        if (!r.config.postClass.empty())
            if (EntityAI* tent = CreateVehicleAt(RString(r.config.postClass.c_str()), d.x, d.y))
            {
                Vector3 at(d.x, Ground(d.x, d.y).Y(), d.y);
                if (AIUnit::FindNearestEmpty(at, false, tent))
                {
                    RunWith("_o setPos _p", GameValueExt(tent), Position(at.X(), at.Z(), 0.0f));
                    d.x = at.X();
                    d.y = at.Z();
                }
                post.tent = tent;
            }
        if (!r.config.flagClass.empty())
            post.flag = CreateVehicleAt(RString(r.config.flagClass.c_str()), d.x + 7.0f, d.y + 3.0f);
        FlyFlag(r, o);
    }
}

void UpdateShroud(Referee& r)
{
    if (r.playerSide == NoSide || !GLandscape)
        return;
    if (r.shroudSize == 0)
    {
        const float size = (GLandscape->GetTerrainRange() - 1) * GLandscape->GetTerrainGrid();
        r.shroudSize = std::max(1, static_cast<int>(std::ceil(size / ShroudCell)));
        r.explored.assign(static_cast<size_t>(r.shroudSize) * r.shroudSize, 0);
    }
    AICenter* center = GWorld->GetCenter(r.config.engineSide[r.playerSide]);
    if (!center)
        return;
    const int reach = static_cast<int>(std::ceil(ShroudReveal / ShroudCell));
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
            const Vector3 p = unit->Position();
            const int cx = static_cast<int>(p.X() / ShroudCell), cy = static_cast<int>(p.Z() / ShroudCell);
            for (int y = cy - reach; y <= cy + reach; y++)
                for (int x = cx - reach; x <= cx + reach; x++)
                {
                    if (x < 0 || y < 0 || x >= r.shroudSize || y >= r.shroudSize)
                        continue;
                    const float ex = (static_cast<float>(x) + 0.5f) * ShroudCell - p.X();
                    const float ey = (static_cast<float>(y) + 0.5f) * ShroudCell - p.Z();
                    if (ex * ex + ey * ey <= ShroudReveal * ShroudReveal)
                        r.explored[static_cast<size_t>(y) * r.shroudSize + x] = 1;
                }
        }
    }
}

// HQ on the side radio: the AI commander's orders, heard while the player does not command. The
// local player hears their side's lines; in multiplayer the server also keeps them for the clients.
void Radio(Referee& r, int side, const std::string& text)
{
    if (side == r.playerSide && r.options.hqRadio)
        GChatList.Add(CCSide, RString("HQ"), RString(text.c_str()), false, true);
    if (GWorld && GWorld->GetMode() == GModeNetware)
    {
        r.radioLog.push_back({++r.radioSeq, side, text});
        if (r.radioLog.size() > 16)
            r.radioLog.erase(r.radioLog.begin());
    }
}

// Player squads get their orders from HQ too: the nearest objective they may attack, else the
// nearest of their own to hold. The planner leaves player squads alone; this tells them.
void HqOrders(Referee& r, float now)
{
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || !grp->Leader() || !grp->IsAnyPlayerGroup() || Human(r, rec.side))
            continue;
        // Still good: an attack on a place not yet ours, or a hold of a place still ours.
        if (rec.hqObjective >= 0)
        {
            const int owner = r.war.states[rec.hqObjective].owner;
            const bool attack = rec.task == TaskKind::Attack;
            if ((attack && owner != rec.side && CanAttack(r.war, rec.side, rec.hqObjective)) || (!attack && owner == rec.side))
                continue;
        }
        const Vector3 me = grp->Leader()->Position();
        int best = -1;
        float bestDist = 1.0e9f;
        bool attack = true;
        for (int pass = 0; pass < 2 && best < 0; pass++)
            for (size_t o = 0; o < r.war.defs.size(); o++)
            {
                const bool ok = pass == 0 ? CanAttack(r.war, rec.side, static_cast<int>(o)) : r.war.states[o].owner == rec.side;
                const float dist = std::hypot(r.war.defs[o].x - me.X(), r.war.defs[o].y - me.Z());
                if (ok && dist < bestDist)
                {
                    bestDist = dist;
                    best = static_cast<int>(o);
                    attack = pass == 0;
                }
            }
        if (best < 0)
            continue;
        const ObjectiveDef& d = r.war.defs[best];
        rec.task = attack ? TaskKind::Attack : TaskKind::Defend;
        rec.objective = best;
        rec.destination = TaskPoint(d, rec.id);
        rec.orderedAt = now;
        rec.hqObjective = best;
        rec.hqText = (attack ? "Take the command post at " : "Hold the command post at ") + d.name;
        rec.hqX = d.x;
        rec.hqY = d.y;
        Radio(r, rec.side, Callsign(rec.id) + ", this is HQ. " + rec.hqText + ". Out.");
        WarLog("HQ to the player squad " + Callsign(rec.id) + ": " + rec.hqText);
    }
}

// A captured base gives its new owner a troop transport, handed to the nearest squad of that side
// that has none (else it waits at the post for whoever comes).
void GiveCaptureVehicle(Referee& r, int side, size_t o)
{
    if (side < 0 || side >= MaxSides || r.config.captureVehicle[side].empty())
        return;
    const ObjectiveDef& d = r.war.defs[o];
    EntityAI* veh = CreateVehicleAt(RString(r.config.captureVehicle[side].c_str()), d.x + 14.0f, d.y - 10.0f);
    if (!veh)
        return;
    UnitRecord u;
    u.unit = veh;
    u.side = side;
    u.vehicle = true;
    r.units.push_back(u);
    GroupRecord* best = nullptr;
    float bestDist = 800.0f;
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !grp || !grp->Leader() || grp->NVehicles() > 0 || grp->IsAnyPlayerGroup())
            continue;
        const float dist = Dist(grp->Leader()->Position(), d);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = &rec;
        }
    }
    if (Transport* transport = dyn_cast<Transport>(veh); transport && best)
    {
        best->group->AddVehicle(transport);
        best->group->AssignVehicles();
        WarLog(Name(side) + " took a truck at " + d.name + " for " + Callsign(best->id));
    }
    Radio(r, side, "A transport is waiting for us at " + d.name + ".");
}

// Every base a side owns has that side's weapons and ammunition box beside its command post. The
// game's inventory (its vicinity pane) restocks and swaps weapons from it. A captured base gets the
// new owner's box; a box is replaced by a full one every arsenalRestock seconds, when nobody is at it.
void UpdateArsenals(Referee& r, float now)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        Post& post = r.posts[o];
        const int owner = r.war.states[o].owner;
        EntityAI* box = post.arsenal;
        bool replace = post.arsenalSide != owner || !box;
        if (!replace && now - post.arsenalAt > r.config.arsenalRestock)
        {
            // Not under someone's hands: no living soldier within 15 m.
            bool busy = false;
            for (const UnitRecord& u : r.units)
                if (EntityAI* unit = u.unit; unit && !u.vehicle && !unit->IsDammageDestroyed() &&
                                             unit->Position().Distance(box->Position()) < 15.0f)
                    busy = true;
            if (Person* player = GWorld->GetRealPlayer(); player && player->Position().Distance(box->Position()) < 15.0f)
                busy = true;
            replace = !busy;
        }
        if (!replace)
            continue;
        if (box)
            DeleteVehicle(box);
        post.arsenal = nullptr;
        post.arsenalSide = owner;
        post.arsenalAt = now;
        if (owner == NoSide || owner >= MaxSides || r.config.arsenalClass[owner].empty())
            continue;
        const ObjectiveDef& d = r.war.defs[o];
        post.arsenal = CreateVehicleAt(RString(r.config.arsenalClass[owner].c_str()), d.x - 6.0f, d.y + 5.0f);
    }
}

void FireShells(Referee& r, float now)
{
    for (size_t i = 0; i < r.shells.size();)
    {
        if (r.shells[i].at > now)
        {
            i++;
            continue;
        }
        const PendingShell s = r.shells[i];
        r.shells.erase(r.shells.begin() + static_cast<std::ptrdiff_t>(i));
        const AmmoType* ammo = dynamic_cast<const AmmoType*>(VehicleTypes.New(RString(r.config.barrageAmmo.c_str())));
        if (!ammo)
        {
            LOG_WARN(Mission, "Conquest: barrage ammunition {} is not a CfgAmmo class", r.config.barrageAmmo);
            continue;
        }
        // Falling steeply, a little off vertical (an orientation needs a direction that is not its up).
        const Vector3 speed(6.0f, -150.0f, 4.0f);
        Shot* shot = NewShot(nullptr, ammo, nullptr);
        if (!shot)
            continue;
        shot->SetOrient(speed.Normalized(), VForward);
        shot->SetSpeed(speed);
        shot->SetPosition(Vector3(s.x - 3.6f, Ground(s.x, s.y).Y() + 90.0f, s.y - 2.4f));
        GWorld->AddFastVehicle(shot);
        if (GWorld->GetMode() == GModeNetware)
            GetNetworkManager().CreateVehicle(shot, VLTFast, "", -1);
    }
}

#include "ConquestRefereeNet.inc"

void Tick(Referee& r, float dt, float now)
{
    if (!r.started)
    {
        if (r.setupPending)
            return; // the setup screen first: side and options
        r.started = true;
        r.playerSide = InMultiplayer() ? LocalSide(r) : r.options.side;
        PlacePosts(r);
        for (int s = 0; s < r.war.sideCount; s++)
            for (size_t o = 0; o < r.war.defs.size(); o++)
                if (r.war.states[o].owner == s && r.war.defs[o].kind == ObjectiveKind::HQ)
                    for (int type : r.config.startSquads)
                        SpawnSquad(r, s, static_cast<int>(o), true, type);
        WarLog("the war begins: the player is " + Name(r.playerSide) +
               (r.options.startInCommand && r.options.commanderView ? ", in command" : ", leading a squad"));
        // As a soldier, the player leads the first squad of their side. Killed, they carry on in
        // another soldier of their side (OnRealPlayerDead): that is the respawn.
        if (r.options.startInCommand && r.options.commanderView && r.playerSide != NoSide)
        {
            r.humanUntil[r.playerSide] = 1.0e9f; // the supplies are the player's from the first tick, not after the view opens
            r.viewRequest = true;
            r.viewNotice = "You have command. Select squads with the left mouse button, order them with the right.";
        }
        else if (r.playerSide != NoSide && !InMultiplayer())
            for (const GroupRecord& rec : r.groups)
                if (rec.side == r.playerSide)
                {
                    AIGroup* grp = rec.group;
                    if (!grp || !grp->Leader() || !EnterSoldier(r, grp->Leader()->GetPerson()))
                        LOG_WARN(Mission, "Conquest: could not start as a soldier of {}", Name(r.playerSide));
                    break;
                }
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
        {
            if (result.event == CaptureEvent::Captured)
                Radio(r, s, result.side == s ? "All units, we hold " + name + "." : "All units, the enemy has taken " + name + ".");
            else if (before == s)
                Radio(r, s, "All units, we have lost " + name + ".");
        }
        if (result.event == CaptureEvent::Captured)
        {
            GiveCaptureVehicle(r, result.side, o);
            Radio(r, result.side, "The arsenal at " + name + " is ours: restock there.");
        }
        FlyFlag(r, o);
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
    if (InMultiplayer())
        AdoptPlayerGroups(r);
    if (!r.ended)
        HqOrders(r, now);
    UpdateShroud(r);
    UpdateArsenals(r, now);
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
    if (!GWorld || (GWorld->GetMode() != GModeArcade && GWorld->GetMode() != GModeNetware) || !GWorld->GetGameState() ||
        !GLandscape)
        return;
    const std::string mission(static_cast<const char*>(GetMissionDirectory()));
    if (mission != r.mission || Pending().loaded)
    {
        // A new mission, or a loaded save (whose war, if it had one, goes on from the save).
        r = Referee();
        r.mission = mission;
        if (const ParamEntry* cfg = ExtParsMission.FindEntry("CfgConquest"); cfg && cfg->IsClass())
            r.active = LoadConfig(r, *cfg);
        if (r.active)
            ContinueSavedWar(r, mission);
        Pending() = Saved();
        r.snapshot.active = r.active;
    }
    if (!r.active)
        return;
    View::PrepareFonts(); // between frames: ImGui may not get new fonts inside one
    const float now = Now();
    if (IsMirror())
    {
        MirrorStep(r, now); // a multiplayer client: the server runs the war
        Refresh(r, now);
        return;
    }
    if (r.lastTick < 0.0f)
        r.lastTick = now;
    if (now < r.lastTick - 1.0f)
    {
        // Loads arrive through Pending() above; time running backwards otherwise means a world this
        // war's record does not describe, so stop rather than run it against that.
        WarLog("game time went backwards; Conquest stops until the mission restarts");
        r.active = false;
        r.snapshot.active = false;
        return;
    }
    FireShells(r, now);
    if (now - r.lastTick < TickSeconds && r.started)
        return;
    const float dt = std::clamp(now - r.lastTick, 0.0f, 5.0f);
    r.lastTick = now;
    const auto t0 = std::chrono::steady_clock::now();
    Tick(r, dt, now);
    if (InMultiplayer() && now - r.lastPublish >= NetPublishSeconds)
    {
        r.lastPublish = now;
        r.playerSide = LocalSide(r); // a hosting player's side; NoSide on a dedicated server
        Publish(r, now);
    }
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Refresh(r, now);
    r.snapshot.lastTickMs = ms;
    r.snapshot.maxTickMs = std::max(r.snapshot.maxTickMs, ms);
}

const Snapshot& GetSnapshot()
{
    return R().snapshot;
}

// ---- saved games -------------------------------------------------------------------------

namespace
{
std::string Key(const char* prefix, size_t i)
{
    return prefix + std::to_string(i);
}

// Opens (saving: creates) the subclass `name`; false when a load does not find it.
bool Open(ParamArchive& ar, const std::string& name, ParamArchive& sub)
{
    return ar.OpenSubclass(RStringB(name.c_str()), sub);
}

LSError SerializeText(ParamArchive& ar, const char* name, std::string& value)
{
    RString text(value.c_str());
    PARAM_CHECK(ar.Serialize(name, text, 1, RString()))
    value = static_cast<const char*>(text);
    return LSOK;
}

// Both load passes read everything (references resolve in the second); saving writes it once.
LSError SerializeSaved(ParamArchive& ar, Saved& w)
{
    PARAM_CHECK(SerializeText(ar, "mission", w.mission))
    PARAM_CHECK(ar.Serialize("started", w.started, 1, false))
    PARAM_CHECK(ar.Serialize("ended", w.ended, 1, false))
    PARAM_CHECK(ar.Serialize("nextGroupId", w.nextGroupId, 1, 1))
    PARAM_CHECK(ar.Serialize("playerSide", w.playerSide, 1, NoSide))
    PARAM_CHECK(ar.SerializeRef("commanderBody", w.commanderBody, 1))
    {
        int fog = static_cast<int>(w.options.fog);
        PARAM_CHECK(ar.Serialize("side", w.options.side, 1, 0))
        PARAM_CHECK(ar.Serialize("startInCommand", w.options.startInCommand, 1, false))
        PARAM_CHECK(ar.Serialize("commanderView", w.options.commanderView, 1, true))
        PARAM_CHECK(ar.Serialize("fog", fog, 1, 0))
        PARAM_CHECK(ar.Serialize("possession", w.options.possession, 1, true))
        PARAM_CHECK(ar.Serialize("hqRadio", w.options.hqRadio, 1, true))
        w.options.fog = fog == static_cast<int>(FogMode::Shroud) ? FogMode::Shroud : FogMode::Contacts;
    }
    PARAM_CHECK(ar.Serialize("setupPending", w.setupPending, 1, false))
    PARAM_CHECK(ar.Serialize("viewOpen", w.viewOpen, 1, false))
    PARAM_CHECK(SerializeText(ar, "explored", w.explored))
    PARAM_CHECK(ar.Serialize("shroudSize", w.shroudSize, 1, 0))
    PARAM_CHECK(ar.Serialize("lastTick", w.lastTick, 1, -1.0f))
    PARAM_CHECK(ar.Serialize("lastStatus", w.lastStatus, 1, -1.0e9f))

    for (size_t s = 0; s < MaxSides; s++)
    {
        ParamArchive sub;
        if (!Open(ar, Key("side", s), sub))
            continue;
        bool defeated = w.defeated[s];
        PARAM_CHECK(sub.Serialize("pool", w.pool[s], 1, 0.0f))
        PARAM_CHECK(sub.Serialize("score", w.score[s], 1, 0.0f))
        PARAM_CHECK(sub.Serialize("defeated", defeated, 1, false))
        PARAM_CHECK(sub.Serialize("lastPlan", w.lastPlan[s], 1, 0.0f))
        PARAM_CHECK(sub.Serialize("live", w.live[s], 1, 0))
        PARAM_CHECK(sub.Serialize("losses", w.losses[s], 1, 0))
        PARAM_CHECK(sub.Serialize("bought", w.bought[s], 1, 0))
        PARAM_CHECK(sub.Serialize("lastBarrage", w.lastBarrage[s], 1, -1.0e9f))
        w.defeated[s] = defeated;
        ar.CloseSubclass(sub);
    }

    int count = static_cast<int>(w.states.size());
    PARAM_CHECK(ar.Serialize("objectives", count, 1, 0))
    w.states.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t o = 0; o < w.states.size(); o++)
    {
        ParamArchive sub;
        if (!Open(ar, Key("objective", o), sub))
            continue;
        ObjectiveState& s = w.states[o];
        PARAM_CHECK(sub.Serialize("owner", s.owner, 1, NoSide))
        PARAM_CHECK(sub.Serialize("holder", s.holder, 1, NoSide))
        PARAM_CHECK(sub.Serialize("level", s.level, 1, 0.0f))
        PARAM_CHECK(sub.Serialize("contested", s.contested, 1, false))
        PARAM_CHECK(sub.Serialize("capturedAt", s.capturedAt, 1, -1.0e9f))
        PARAM_CHECK(sub.Serialize("lastHitAt", s.lastHitAt, 1, -1.0e9f))
        w.posts.resize(w.states.size());
        w.postX.resize(w.states.size(), 0.0f);
        w.postY.resize(w.states.size(), 0.0f);
        PARAM_CHECK(sub.SerializeRef("tent", w.posts[o].tent, 1))
        PARAM_CHECK(sub.SerializeRef("flag", w.posts[o].flag, 1))
        PARAM_CHECK(sub.SerializeRef("arsenal", w.posts[o].arsenal, 1))
        PARAM_CHECK(sub.Serialize("arsenalSide", w.posts[o].arsenalSide, 1, -2))
        PARAM_CHECK(sub.Serialize("arsenalAt", w.posts[o].arsenalAt, 1, -1.0e9f))
        PARAM_CHECK(sub.Serialize("postX", w.postX[o], 1, 0.0f))
        PARAM_CHECK(sub.Serialize("postY", w.postY[o], 1, 0.0f))
        ar.CloseSubclass(sub);
    }

    count = static_cast<int>(w.groups.size());
    PARAM_CHECK(ar.Serialize("groups", count, 1, 0))
    w.groups.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t i = 0; i < w.groups.size(); i++)
    {
        ParamArchive sub;
        if (!Open(ar, Key("group", i), sub))
            continue;
        GroupRecord& g = w.groups[i];
        int task = static_cast<int>(g.task);
        PARAM_CHECK(sub.Serialize("id", g.id, 1, 0))
        PARAM_CHECK(sub.Serialize("side", g.side, 1, NoSide))
        PARAM_CHECK(sub.SerializeRef("group", g.group, 1))
        PARAM_CHECK(sub.Serialize("task", task, 1, 0))
        PARAM_CHECK(sub.Serialize("objective", g.objective, 1, -1))
        PARAM_CHECK(sub.Serialize("destination", g.destination, 1, VZero))
        PARAM_CHECK(sub.Serialize("orderedAt", g.orderedAt, 1, -1.0e9f))
        PARAM_CHECK(sub.Serialize("bestDistance", g.bestDistance, 1, 1.0e9f))
        PARAM_CHECK(sub.Serialize("bestAt", g.bestAt, 1, 0.0f))
        PARAM_CHECK(sub.Serialize("stalled", g.stalled, 1, false))
        PARAM_CHECK(sub.Serialize("lockedUntil", g.lockedUntil, 1, -1.0e9f))
        PARAM_CHECK(sub.Serialize("type", g.type, 1, 0))
        g.task = static_cast<TaskKind>(task);
        ar.CloseSubclass(sub);
    }

    count = static_cast<int>(w.units.size());
    PARAM_CHECK(ar.Serialize("units", count, 1, 0))
    w.units.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t i = 0; i < w.units.size(); i++)
    {
        ParamArchive sub;
        if (!Open(ar, Key("unit", i), sub))
            continue;
        UnitRecord& u = w.units[i];
        PARAM_CHECK(sub.SerializeRef("unit", u.unit, 1))
        PARAM_CHECK(sub.Serialize("side", u.side, 1, NoSide))
        PARAM_CHECK(sub.Serialize("lastDamage", u.lastDamage, 1, 0.0f))
        PARAM_CHECK(sub.Serialize("deadAt", u.deadAt, 1, -1.0f))
        PARAM_CHECK(sub.Serialize("vehicle", u.vehicle, 1, false))
        ar.CloseSubclass(sub);
    }

    count = static_cast<int>(w.log.size());
    PARAM_CHECK(ar.Serialize("logLines", count, 1, 0))
    w.log.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t i = 0; i < w.log.size(); i++)
        PARAM_CHECK(SerializeText(ar, Key("log", i).c_str(), w.log[i]))
    return LSOK;
}

// Simulate, with the mission's config read again: the saved war goes on where it was.
void Restore(Referee& r, const Saved& w)
{
    if (w.states.size() != r.war.defs.size())
    {
        WarLog("the saved war does not fit this mission's objectives; Conquest stops");
        r.active = false;
        return;
    }
    r.started = w.started;
    r.ended = w.ended;
    r.war.states = w.states;
    r.war.pool = w.pool;
    r.war.score = w.score;
    r.war.defeated = w.defeated;
    r.lastPlan = w.lastPlan;
    r.live = w.live;
    r.losses = w.losses;
    r.bought = w.bought;
    r.groups = w.groups;
    r.units = w.units;
    r.nextGroupId = w.nextGroupId;
    r.playerSide = w.playerSide;
    r.commanderBody = w.commanderBody;
    r.options = w.options;
    r.setupPending = w.setupPending;
    r.posts = w.posts;
    r.posts.resize(r.war.defs.size());
    for (size_t o = 0; o < r.war.defs.size() && o < w.postX.size(); o++)
        if (w.postX[o] != 0.0f || w.postY[o] != 0.0f)
        {
            r.war.defs[o].x = w.postX[o];
            r.war.defs[o].y = w.postY[o];
        }
    r.shroudSize = w.shroudSize;
    r.explored.assign(w.explored.begin(), w.explored.end());
    for (uint8_t& c : r.explored)
        c = c == '1' ? 1 : 0;
    if (r.explored.size() != static_cast<size_t>(r.shroudSize) * r.shroudSize)
    {
        r.explored.clear();
        r.shroudSize = 0;
    }
    r.lastBarrage = w.lastBarrage;
    r.lastTick = w.lastTick;
    r.lastStatus = w.lastStatus;
    r.snapshot.log = w.log;
    for (int s = 0; s < r.war.sideCount; s++)
        r.replan[s] = true;
    WarLog("the saved war goes on");
    Status(r); // the war as restored, before it moves again
}

void ContinueSavedWar(Referee& r, const std::string& mission)
{
    Saved& w = Pending();
    if (w.valid && w.mission == mission)
    {
        Restore(r, w);
        // Saved with the commander view open: the save holds its camera, not the view. Open it anew.
        if (r.active && w.viewOpen && !View::Active())
        {
            if (GWorld)
                GWorld->SetCameraEffect(nullptr);
            View::SetActive(true);
        }
    }
}
} // namespace

LSError Serialize(ParamArchive& ar)
{
    if (ar.IsSaving())
    {
        const Referee& r = R();
        if (!r.active)
            return LSOK;
        Saved w;
        w.mission = r.mission;
        w.started = r.started;
        w.ended = r.ended;
        w.states = r.war.states;
        w.pool = r.war.pool;
        w.score = r.war.score;
        w.defeated = r.war.defeated;
        w.lastPlan = r.lastPlan;
        w.live = r.live;
        w.losses = r.losses;
        w.bought = r.bought;
        w.groups = r.groups;
        w.units = r.units;
        w.nextGroupId = r.nextGroupId;
        w.playerSide = r.playerSide;
        w.commanderBody = r.commanderBody;
        w.options = r.options;
        w.setupPending = r.setupPending;
        w.posts = r.posts;
        for (const ObjectiveDef& d : r.war.defs)
        {
            w.postX.push_back(d.x);
            w.postY.push_back(d.y);
        }
        w.shroudSize = r.shroudSize;
        for (uint8_t c : r.explored)
            w.explored.push_back(c ? '1' : '0');
        w.lastBarrage = r.lastBarrage;
        w.viewOpen = View::Active();
        w.lastTick = r.lastTick;
        w.lastStatus = r.lastStatus;
        w.log = r.snapshot.log;
        ParamArchive sub;
        if (!ar.OpenSubclass("Conquest", sub))
        {
            ar.OnError(LSNoEntry, "Conquest");
            return LSNoEntry;
        }
        PARAM_CHECK(SerializeSaved(sub, w))
        ar.CloseSubclass(sub);
        return LSOK;
    }
    Saved& w = Pending();
    if (ar.GetPass() == ParamArchive::PassFirst)
        w = Saved();
    w.loaded = true;
    ParamArchive sub;
    if (!ar.OpenSubclass("Conquest", sub))
        return LSOK; // a save without a war (or from before the war began): the mission starts it afresh
    PARAM_CHECK(SerializeSaved(sub, w))
    ar.CloseSubclass(sub);
    w.valid = ar.GetPass() == ParamArchive::PassSecond;
    return LSOK;
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
// The squad leader answers on the side's radio, in his own voice ("Moving" / "Attacking").
void VoiceConfirm(const GroupRecord& rec, bool attack)
{
    AIGroup* grp = rec.group;
    AICenter* center = grp ? grp->GetCenter() : nullptr;
    if (!center || !grp->Leader() || grp->Leader()->GetLifeState() != AIUnit::LSAlive)
        return;
    Command cmd;
    cmd._message = attack ? Command::Attack : Command::Move;
    cmd._destination = rec.destination;
    center->GetRadio().Transmit(new RadioMessageCommandConfirm(grp->Leader(), grp, cmd), center->GetLanguage());
}

std::string OrderRecords(Referee& r, int side, const std::vector<GroupRecord*>& records, float x, float y)
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
            rec->task = r.war.states[target].owner == side ? TaskKind::Defend : TaskKind::Attack;
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
    // One voice answers (the first squad): several at once would talk over each other.
    if (!records.empty())
        VoiceConfirm(*records.front(), records.front()->task == TaskKind::Attack);
    if (target >= 0 && r.war.states[target].owner != side && !CanAttack(r.war, side, target))
        text += ". Not on the front yet: it cannot be captured until a neighbouring objective is ours";
    r.replan[side] = true;
    return text;
}

int OrderGroupsFor(Referee& r, int side, const std::vector<int>& ids, float x, float y)
{
    if (side == NoSide)
        return 0;
    std::vector<GroupRecord*> records;
    for (GroupRecord& rec : r.groups)
        if (rec.side == side && rec.group && std::find(ids.begin(), ids.end(), rec.id) != ids.end())
            records.push_back(&rec);
    if (records.empty())
        return 0;
    OrderRecords(r, side, records, x, y);
    return static_cast<int>(records.size());
}

bool CanBuyFor(const Referee& r, int side, int base, int type, std::string* reason)
{
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (!r.active || side == NoSide)
        return refuse("no war");
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()) || base < 0 ||
        base >= static_cast<int>(r.war.defs.size()))
        return refuse("no such squad");
    const SquadDef& def = r.config.squads[type];
    if (IsMirror())
    {
        // The server's word, published each second.
        if (r.war.states[base].owner != side)
            return refuse("not our base");
        if (!r.spawnWhy[base][side].empty())
        {
            if (reason)
                *reason = r.spawnWhy[base][side];
            return false;
        }
    }
    else if (!SpawnEligible(r.war, side, base, Now(), r.tuning, reason))
        return false;
    if (r.war.pool[side] < def.cost)
        return refuse("not enough supplies");
    if (r.live[side] + static_cast<int>(def.classes[side].size()) > r.config.cap)
        return refuse("manpower cap reached");
    return true;
}

int BuyFor(Referee& r, int side, int base, int type, std::string* reason)
{
    if (!CanBuyFor(r, side, base, type, reason))
        return 0;
    const int id = SpawnSquad(r, side, base, false, type);
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
    WarLog("commander: " + Name(side) + " bought a " + r.config.squads[type].name + " at " + r.war.defs[base].name);
    return id;
}

bool BarrageFor(Referee& r, int side, float x, float y, std::string* reason)
{
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (!r.active || !r.started || side == NoSide)
        return refuse("no war");
    if (r.config.barrageShells <= 0 || r.config.barrageAmmo.empty())
        return refuse("this war has no mortars");
    const float now = Now();
    if (now < r.lastBarrage[side] + r.config.barrageCooldown)
        return refuse("the mortars are reloading");
    if (r.war.pool[side] < r.config.barrageCost)
        return refuse("not enough supplies");
    r.war.pool[side] -= r.config.barrageCost;
    r.lastBarrage[side] = now;
    // The first round lands after the flight time, the rest a second or so apart, scattered.
    unsigned seed = static_cast<unsigned>(now * 1000.0f) ^ 0x9E3779B9u;
    const auto random = [&]()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int i = 0; i < r.config.barrageShells; i++)
    {
        const float a = random() * 6.2831853f, d = std::sqrt(random()) * r.config.barrageSpread;
        r.shells.push_back({now + 7.0f + static_cast<float>(i) * 1.1f + random() * 0.6f, x + std::cos(a) * d,
                            y + std::sin(a) * d});
    }
    char where[64];
    std::snprintf(where, sizeof(where), "%.0f, %.0f", x, y);
    WarLog(Name(side) + " mortar barrage on " + where);
    Radio(r, side, "Mortars firing, " + std::to_string(r.config.barrageShells) + " rounds. Splash in seven seconds.");
    return true;
}

// The server's side of `conquestRequest`: [player, kind, args] from a client's commander view.
void HandleRequest(Referee& r, const GameValue& value)
{
    if (!r.active || !IsAuthority() || value.GetType() != GameArray)
        return;
    const GameArrayType& a = value;
    if (a.Size() < 3 || a[2].GetType() != GameArray)
        return;
    const int side = SideOfUnit(r, dyn_cast<EntityAI>(GetObject(a[0])));
    if (side == NoSide)
        return;
    const std::string kind = Str(a, 1);
    const GameArrayType& args = a[2];
    if (kind == "order" && args.Size() >= 3 && args[0].GetType() == GameArray)
    {
        std::vector<int> ids;
        const GameArrayType& list = args[0];
        for (int i = 0; i < list.Size(); i++)
            ids.push_back(static_cast<int>(Num(list, i)));
        OrderGroupsFor(r, side, ids, Num(args, 1), Num(args, 2));
    }
    else if (kind == "buy" && args.Size() >= 2)
    {
        const int id = BuyFor(r, side, static_cast<int>(Num(args, 0, -1)), static_cast<int>(Num(args, 1, -1)), nullptr);
        if (id)
            Radio(r, side, "Commander, " + Callsign(id) + " is ready at " + r.war.defs[static_cast<int>(Num(args, 0))].name + ".");
    }
    else if (kind == "barrage" && args.Size() >= 2)
        BarrageFor(r, side, Num(args, 0), Num(args, 1), nullptr);
    else if (kind == "command" && args.Size() >= 1)
    {
        const bool was = Human(r, side);
        r.humanUntil[side] = Flag(args, 0) ? Now() + CommandHoldSeconds : -1.0e9f;
        if (was != Human(r, side))
            WarLog(Name(side) + (Human(r, side) ? " is under a player's command" : " is back under the AI commander"));
    }
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
    Hint("Orders sent:" + OrderRecords(r, r.playerSide, selected, x, y) + ". The AI leaves these squads to you for 10 minutes.");
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

bool AliveSoldier(Person* person)
{
    return person && person->Brain() && person->Brain()->GetLifeState() == AIUnit::LSAlive;
}

// The player becomes `person`; their own body (the commander's) waits where it is.
bool EnterSoldier(Referee& r, Person* person)
{
    if (!AliveSoldier(person) || !GWorld)
        return false;
    if (!r.commanderBody || GWorld->GetRealPlayer() == r.commanderBody.GetLink())
        r.commanderBody = GWorld->GetRealPlayer();
    TakeBody(person);
    return true;
}

const GroupRecord* RecordOf(const Referee& r, const AIGroup* grp)
{
    for (const GroupRecord& rec : r.groups)
        if (rec.group.GetLink() == grp)
            return &rec;
    return nullptr;
}

// After the player's soldier died: a living soldier of the player's side to carry on in, first from
// the dead man's own squad, then the squad nearest to where he fell (its leader first).
Person* NextSoldier(const Referee& r, Person* dead)
{
    AIGroup* own = dead && dead->Brain() ? dead->Brain()->GetGroup() : nullptr;
    if (own)
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (AIUnit* unit = own->UnitWithID(id); Alive(unit) && unit->GetPerson() != dead)
                return unit->GetPerson();
    const Vector3 at = dead ? dead->Position() : VZero;
    Person* best = nullptr;
    float bestDist = 1.0e18f;
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != r.playerSide || !grp || grp == own)
            continue;
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
        {
            AIUnit* unit = grp->UnitWithID(id);
            if (!Alive(unit) || unit->GetPerson() == dead)
                continue;
            const float d = (unit->Position() - at).SquareSize() + (unit == grp->Leader() ? 0.0f : 1.0e4f);
            if (d < bestDist)
            {
                bestDist = d;
                best = unit->GetPerson();
            }
        }
    }
    return best;
}
} // namespace

bool PossessionAllowed()
{
    return R().active && R().options.possession && !InMultiplayer();
}

void SetPossessionAllowed(bool allowed)
{
    R().options.possession = allowed;
}

bool Possessing()
{
    const Referee& r = R();
    return r.active && r.commanderBody && GWorld && GWorld->GetRealPlayer() != r.commanderBody.GetLink();
}

bool PossessUnit(Person* person, std::string* reason)
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
    if (!r.options.possession)
        return refuse("taking control of soldiers is switched off");
    if (!AliveSoldier(person))
        return refuse("that soldier is down");
    const GroupRecord* rec = RecordOf(r, person->Brain()->GetGroup());
    if (!rec || rec->side != r.playerSide)
        return refuse("not one of your soldiers");
    EnterSoldier(r, person);
    WarLog("commander: took control of a soldier of " + Describe(r, *rec));
    return true;
}

bool Possess(int groupId, std::string* reason)
{
    Referee& r = R();
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.id == groupId && rec.side == r.playerSide && grp && grp->Leader())
            return PossessUnit(grp->Leader()->GetPerson(), reason);
    }
    if (reason)
        *reason = "no such squad";
    return false;
}

bool ReturnToCommander()
{
    Referee& r = R();
    Person* body = r.commanderBody;
    if (!AliveSoldier(body))
        return false;
    if (GWorld->GetRealPlayer() != body)
        TakeBody(body);
    return true;
}

bool OnRealPlayerDead()
{
    Referee& r = R();
    if (!r.active || !r.started || r.playerSide == NoSide || !GWorld || InMultiplayer())
        return false; // multiplayer: the mission's respawn
    Person* dead = GWorld->GetRealPlayer();
    // With the commander view, the player goes back to command and picks where to go next.
    if (r.options.commanderView && Possessing() && ReturnToCommander())
    {
        WarLog("commander: the soldier you controlled was killed; back in command");
        r.viewRequest = true;
        r.viewNotice = "The soldier you controlled was killed. You are back in command.";
        return true;
    }
    // Without it: straight into the next soldier of the side, as long as the side has one.
    Person* next = NextSoldier(r, dead);
    if (!next || !EnterSoldier(r, next))
        return false;
    std::string squad;
    if (const GroupRecord* rec = RecordOf(r, next->Brain()->GetGroup()))
        squad = Callsign(rec->id);
    r.soldierNotice = squad.empty() ? std::string("You were killed. You fight on as another soldier.")
                                    : "You were killed. You fight on with " + squad + ".";
    WarLog("the player was killed and carries on in " + (squad.empty() ? std::string("another soldier") : squad));
    return true;
}

bool TakeViewRequest(std::string& notice)
{
    Referee& r = R();
    if (!r.viewRequest)
        return false;
    r.viewRequest = false;
    notice = r.viewNotice;
    return true;
}

bool TakeSoldierNotice(std::string& notice)
{
    Referee& r = R();
    if (r.soldierNotice.empty())
        return false;
    notice = r.soldierNotice;
    r.soldierNotice.clear();
    return true;
}

std::string Callsign(int id)
{
    static const char* names[] = {"Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
                                  "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
                                  "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
                                  "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};
    if (id <= 0)
        return "Squad";
    const int lap = (id - 1) / 26;
    const std::string name = names[(id - 1) % 26];
    return lap > 0 ? name + " " + std::to_string(lap + 1) : name;
}

bool SetupPending()
{
    return R().active && R().setupPending;
}

std::vector<std::string> SideNames()
{
    std::vector<std::string> out;
    const Referee& r = R();
    for (int s = 0; s < r.war.sideCount; s++)
        out.emplace_back(SideLabel(r.config.engineSide[s]));
    return out;
}

bool BeginWar(const Options& options)
{
    Referee& r = R();
    if (!r.active || r.started || options.side < 0 || options.side >= r.war.sideCount)
        return false;
    r.options = options;
    r.setupPending = false;
    return true;
}

Options GetOptions()
{
    return R().options;
}

void SetOptions(const Options& options)
{
    Referee& r = R();
    const int side = r.options.side;
    const bool start = r.options.startInCommand;
    r.options = options;
    if (r.started)
    {
        r.options.side = side;
        r.options.startInCommand = start;
    }
}

bool Explored(float x, float y)
{
    const Referee& r = R();
    if (r.shroudSize == 0)
        return false;
    const int cx = static_cast<int>(std::floor(x / ShroudCell)), cy = static_cast<int>(std::floor(y / ShroudCell));
    if (cx < 0 || cy < 0 || cx >= r.shroudSize || cy >= r.shroudSize)
        return false;
    return r.explored[static_cast<size_t>(cy) * r.shroudSize + cx] != 0;
}

BarrageInfo Barrage()
{
    const Referee& r = R();
    BarrageInfo b;
    b.cost = r.config.barrageCost;
    b.shells = r.config.barrageShells;
    if (r.playerSide != NoSide)
        b.cooldownLeft = IsMirror() ? r.netCooldown[r.playerSide]
                                    : std::max(0.0f, r.lastBarrage[r.playerSide] + r.config.barrageCooldown - Now());
    return b;
}

bool OrderBarrage(float x, float y, std::string* reason)
{
    Referee& r = R();
    if (!IsMirror())
        return BarrageFor(r, r.playerSide, x, y, reason);
    const BarrageInfo b = Barrage();
    if (b.cooldownLeft > 0.0f || r.playerSide == NoSide || r.war.pool[r.playerSide] < b.cost)
    {
        if (reason)
            *reason = b.cooldownLeft > 0.0f ? "the mortars are reloading" : "not enough supplies";
        return false;
    }
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(GameValue(x));
    a.Add(GameValue(y));
    Request("barrage", args);
    return true;
}

PlayerOrder CurrentPlayerOrder()
{
    const Referee& r = R();
    PlayerOrder order;
    Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (!r.active || !player || !player->Brain())
        return order;
    if (const GroupRecord* rec = RecordOf(r, player->Brain()->GetGroup()); rec && !rec->hqText.empty())
    {
        order.valid = true;
        order.text = rec->hqText;
        order.x = rec->hqX;
        order.y = rec->hqY;
    }
    return order;
}

void SetHumanCommander(bool on)
{
    Referee& r = R();
    if (IsMirror())
    {
        r.localCommand = on;
        r.lastCommandBeat = Now();
        GameValue args = NetArray();
        GameArrayType& a = args;
        a.Add(GameValue(on));
        Request("command", args);
        return;
    }
    if (r.playerSide != NoSide)
        r.humanUntil[r.playerSide] = on ? 1.0e9f : -1.0e9f;
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
        else
        {
            g.typeName = "Player squad";
            g.size = grp->NUnits();
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
    if (!IsMirror())
        return OrderGroupsFor(r, r.playerSide, ids, x, y);
    int mine = 0;
    GameValue list = NetArray();
    GameArrayType& l = list;
    for (const GroupRecord& rec : r.groups)
        if (rec.side == r.playerSide && std::find(ids.begin(), ids.end(), rec.id) != ids.end())
        {
            l.Add(GameValue(static_cast<float>(rec.id)));
            mine++;
        }
    if (mine == 0)
        return 0;
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(list);
    a.Add(GameValue(x));
    a.Add(GameValue(y));
    Request("order", args);
    return mine;
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
    return CanBuyFor(r, r.playerSide, base, type, reason);
}

int BuySquad(int base, int type, std::string* reason)
{
    Referee& r = R();
    if (!IsMirror())
        return BuyFor(r, r.playerSide, base, type, reason);
    if (!CanBuyFor(r, r.playerSide, base, type, reason))
        return 0;
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(GameValue(static_cast<float>(base)));
    a.Add(GameValue(static_cast<float>(type)));
    Request("buy", args);
    return -1; // asked: the server answers on the radio when the squad is ready
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
    if (IsMirror())
        return side < MaxSides ? r.netContacts[side] : out;
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

void HandleNetRequest(const GameValue& value)
{
    HandleRequest(R(), value);
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

// conquestStart ["EAST", "soldier"|"commander", "contacts"|"shroud", "noview"]: the setup screen's Start, from a
// script (the soak and test harnesses). Missing entries keep the mission's defaults.
static GameValue ConquestStartUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    Poseidon::Conquest::Options options = Poseidon::Conquest::GetOptions();
    const auto text = [&](int i) -> std::string
    {
        if (i >= a.Size() || a[i].GetType() != GameString)
            return std::string();
        const GameStringType value = a[i];
        std::string s(static_cast<const char*>(value));
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    };
    const std::vector<std::string> sides = Poseidon::Conquest::SideNames();
    for (size_t s = 0; s < sides.size(); s++)
        if (sides[s] == text(0))
            options.side = static_cast<int>(s);
    if (const std::string start = text(1); !start.empty())
        options.startInCommand = start == "COMMANDER";
    if (const std::string fog = text(2); !fog.empty())
        options.fog = fog == "SHROUD" ? Poseidon::Conquest::FogMode::Shroud : Poseidon::Conquest::FogMode::Contacts;
    if (text(3) == "NOVIEW")
    {
        options.commanderView = false;
        options.startInCommand = false;
    }
    return GameValue(Poseidon::Conquest::BeginWar(options));
}

// conquestBarrage [x, y]: the player's side fires its mortars there. true, or why not.
static GameValue ConquestBarrageUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 2)
        return GameValue(RString("conquestBarrage [x, y]"));
    std::string why;
    if (Poseidon::Conquest::OrderBarrage(static_cast<float>(a[0]), static_cast<float>(a[1]), &why))
        return GameValue(true);
    return GameValue(RString(why.c_str()));
}

// conquestLog: the war log's latest lines, newest last (a multiplayer client's is the server's, mirrored).
static GameValue ConquestLogNular(const GameState*)
{
    const auto& log = Poseidon::Conquest::GetSnapshot().log;
    std::string text;
    const size_t first = log.size() > 40 ? log.size() - 40 : 0;
    for (size_t i = first; i < log.size(); i++)
        text += log[i] + "\n";
    return GameValue(RString(text.c_str()));
}

// conquestOrder [[ids], x, y]: the commander view's right click for the player's squads `ids`, from a
// script. Returns how many squads took it (asked of the server from a multiplayer client).
static GameValue ConquestOrderUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 3 || a[0].GetType() != GameArray)
        return GameValue(0.0f);
    std::vector<int> ids;
    const GameArrayType& list = a[0];
    for (int i = 0; i < list.Size(); i++)
        ids.push_back(static_cast<int>(static_cast<float>(list[i])));
    return GameValue(static_cast<float>(
        Poseidon::Conquest::OrderGroups(ids, static_cast<float>(a[1]), static_cast<float>(a[2]))));
}

// conquestRequest [player, kind, args]: a multiplayer client's commander action, run on the server
// (`_this remoteExec ["conquestRequest", 2]`).
static GameValue ConquestRequestUnary(const GameState*, GameValuePar oper1)
{
    Poseidon::Conquest::HandleNetRequest(oper1);
    return GameValue();
}

INIT_MODULE(ConquestReferee, 3)
{
    GGameState.NewFunction(GameFunction(GameNothing, "conquestRequest", ConquestRequestUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameScalar, "conquestOrder", ConquestOrderUnary, GameArray));
    GGameState.NewNularOp(GameNular(GameString, "conquestLog", ConquestLogNular));
    GGameState.NewFunction(GameFunction(GameBool, "conquestStart", ConquestStartUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameAny, "conquestBarrage", ConquestBarrageUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameAny, "conquestTakeControl", ConquestTakeControlUnary, GameScalar));
    GGameState.NewFunction(GameFunction(GameAny, "conquestBuy", ConquestBuyUnary, GameArray));
    GGameState.NewNularOp(GameNular(GameString, "conquestState", ConquestStateNular));
    GGameState.NewFunction(GameFunction(GameBool, "conquestMapClick", ConquestMapClickUnary, GameArray));
};
