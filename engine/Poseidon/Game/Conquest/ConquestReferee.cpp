// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Conquest/ConquestReferee.hpp>
#include <Poseidon/Game/Conquest/ConquestView.hpp>
#include <Poseidon/Game/Conquest/ConquestLlm.hpp>
#include <Poseidon/Dev/NpcChat/NpcChat.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Modules/Modules.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Game/Chat.hpp>
#include <Poseidon/UI/Inventory/InventoryModel.hpp>
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
#include <Poseidon/World/Entities/Vehicles/Ground/Tank.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cjson/cJSON.h>
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
constexpr float NudgeSeconds = 90.0f; // no progress this long (and no fight): stuck, try another way
constexpr float BodySeconds = 300.0f;
constexpr float BodyHideDistance = 300.0f;
constexpr size_t LogLines = 200;
constexpr float HumanLockSeconds = 600.0f; // a commander's order outranks the AI planner this long
constexpr float BoughtWaitSeconds = 120.0f; // a bought squad waits this long for the commander's order
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
    bool holding = false;        // awaiting orders: told once to stand where it is
    bool landing = false;        // its helicopter was told to land for the passengers
    float nudgedAt = -1.0e9f;    // when it was last un-stuck
    bool parkedHeli = false;     // its helicopter has set down for good after unloading
    bool onFoot = false;         // its trucks got stuck: it left them where they are and walks from then on
    bool detouring = false;      // backing off from where its vehicle got stuck ...
    float detourUntil = 0.0f;    // ... until then; no other order meanwhile
    int tries = 0;               // attempts to get it unstuck since it last made progress
    int supplyState = 0;         // a supply squad: 0 at the HQ, 1 loaded and on the way, 2 coming back
    int supplyTarget = -1;       // ... the base it brings supplies to
    float supplyLoad = 0.0f;     // ... and what it carries
    int supplyPin = -1;          // ... the base the commander sent it to (else the neediest)
    bool supplyDuty = false;     // a fighting squad the commander put on supply runs
    bool truckless = false;      // a supply squad whose truck is gone: it fights on foot
    std::string engageName;      // the building or vehicle the commander told it to attack
    std::string netDoing;        // a client: what the server says it is doing
    int nudges = 0;
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
    int cap = 80;
    int capThreeSides = 55; // per side when the Resistance takes part: the total the AI budget was measured at
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
    // The weapons stocked into it, class and count: the game's ammunition boxes carry magazines only,
    // and an arsenal is also where a soldier changes his weapon.
    std::array<std::vector<std::pair<std::string, int>>, MaxSides> arsenalWeapons;
    float arsenalRestock = 600.0f;                    // seconds until a post's box is replaced by a full one
    float arsenalWeaponCost = 25.0f;                  // supplies a weapon taken from an arsenal costs its side
    float arsenalMagazineCost = 3.0f;                 // ... and each magazine
    float barrageRange = 4000.0f;                     // a side fires only this close to an objective it holds
    float supportDelay = 300.0f;                      // no mortars or air strikes this long into the war
    float airCost = 400.0f;                           // air support: one plane, a stick of bombs on one point
    float airCooldown = 300.0f;
    int airBombs = 4;
    float airSpread = 12.0f;                          // the stick, along the plane's track
    std::string airAmmo = "LaserGuidedBomb";
    std::array<std::string, MaxSides> airPlane;       // A10, Su25
    std::array<std::string, MaxSides> airPilot;
    std::array<std::string, MaxSides> personality;    // the AI commander's own posture: balanced, aggressive, cautious
    bool optionalLast = false;                        // the last of three sides joins only when the setup says so
};

// A purchase the player's side could not make yet (no supplies, enemies at the base): it is made
// as soon as it can be, or dropped after QueueSeconds.
struct QueuedBuy
{
    int side = NoSide;
    int base = -1;
    int type = 0;
    float until = 0.0f;
};
constexpr float QueueSeconds = 120.0f;

// An air strike on its way: the plane, its pilot's group, the point, and where it leaves by.
struct Strike
{
    int side = NoSide;
    OLink<Transport> plane;
    OLink<AIGroup> group;
    float x = 0.0f, y = 0.0f;
    float startedAt = 0.0f;
    float closest = 1.0e9f;
    bool dropped = false;
    float droppedAt = 0.0f;
    Vector3 exit;
};
constexpr size_t QueueMax = 4;

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
    std::vector<int> parked;      // per objective: vehicles parked there so far (ParkingSlot)
    std::vector<uint8_t> explored; // the shroud, shroudSize x shroudSize cells
    int shroudSize = 0;
    std::array<float, MaxSides> lastBarrage{-1.0e9f, -1.0e9f, -1.0e9f};
    std::vector<PendingShell> shells;
    std::vector<QueuedBuy> buyQueue;
    std::vector<Strike> strikes;
    float warStart = -1.0f; // game time the war began (the time limit counts from it)
    float netTimeLeft = -1.0f;
    std::array<float, MaxSides> lastAir{-1.0e9f, -1.0e9f, -1.0e9f};
    std::array<float, MaxSides> netAirCooldown{};
    std::array<bool, MaxSides> netAirInbound{};
    // multiplayer command: one human per side, and the side's vote to remove them
    std::array<OLink<Person>, MaxSides> seat;
    struct Vote
    {
        bool active = false;
        float until = 0.0f;
        OLink<Person> target;
        std::string targetName, callerName;
        std::vector<OLink<Person>> yes, no;
    };
    std::array<Vote, MaxSides> vote;
    std::vector<std::pair<OLink<Person>, float>> barred; // voted out: no command again until then
    std::array<OLink<Person>, MaxSides> netSeat;         // a client's mirror of the seats and votes
    std::array<std::string, MaxSides> netSeatName;
    std::array<CommandSeat, MaxSides> netVote;
    int myVote = 0;
    float commandSince = -1.0e9f;
    std::string commandLoss; // why the player lost command (the view closes and says it)
    int winner = NoSide;
    std::string winReason;
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
    // the LLM commander (Hybrid) and the player's requests to HQ: steers for the Standard commander
    std::array<std::vector<float>, MaxSides> llmPriority; // per objective
    std::array<std::string, MaxSides> posture;           // "attack", "balanced", "defend"
    std::array<std::string, MaxSides> llmBuy;            // the next purchase the LLM asked for
    std::array<int, MaxSides> llmBarrage{-1, -1, -1};    // the objective the LLM wants shelled, -1: none
    std::array<float, MaxSides> lastSitrep{};             // HQ's last situation report on the radio
    std::array<std::string, MaxSides> llmReason;
    std::array<std::deque<std::string>, MaxSides> llmMemory; // the commander's own earlier decisions, oldest first
    std::array<float, MaxSides> lastLlmAsk{-1.0e9f, -1.0e9f, -1.0e9f};
    std::array<float, MaxSides> lastLlmFailure{-1.0e9f, -1.0e9f, -1.0e9f};
    std::array<int, MaxSides> llmEpoch{};
    int captureEpoch = 0; // grows with every change of hands: an answer about an older war is dropped
    std::array<int, MaxSides> humanPriority{-1, -1, -1}; // the objective the player asked HQ to push
    std::array<float, MaxSides> humanPriorityUntil{-1.0e9f, -1.0e9f, -1.0e9f};
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

// Seconds until the war's support (mortars, air strikes) may be called: none in its first minutes, so a
// freshly deployed side is not shelled at its own base before anyone has moved (owner, 2026-10-26).
float SupportWait(const Referee& r, float now)
{
    if (r.warStart < 0.0f)
        return r.config.supportDelay;
    return std::max(0.0f, r.warStart + r.config.supportDelay - now);
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
    std::array<std::deque<std::string>, MaxSides> llmMemory;
    std::array<float, MaxSides> lastBarrage{-1.0e9f, -1.0e9f, -1.0e9f};
    float lastTick = -1.0f;
    float lastStatus = -1.0e9f;
    std::deque<std::string> log;
    int winner = NoSide;
    std::string winReason;
    float warStart = -1.0f;
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

#include "ConquestRefereeProfile.inc"

// The island's towns: CfgWorlds >> world >> Names, villages and up (hills, bays and woods left out).
// An island whose config names none (Kolgujev) lists them in the mission: CfgConquest >> Places.
std::vector<Place> IslandPlaces(const ParamEntry& conquest)
{
    std::vector<Place> places;
    if (const ParamEntry* own = conquest.FindEntry("Places"); own && own->IsClass())
    {
        for (int i = 0; i < own->GetEntryCount(); i++)
        {
            const ParamEntry& e = own->GetEntry(i);
            const ParamEntry* pos = e.IsClass() ? e.FindEntry("position") : nullptr;
            if (!pos || !pos->IsArray() || pos->GetSize() < 2)
                continue;
            Place p;
            p.name = Text(e, "name", static_cast<const char*>(e.GetName()));
            p.x = static_cast<float>((*pos)[0]);
            p.y = static_cast<float>((*pos)[1]);
            p.rank = static_cast<int>(Number(e, "rank", 0.0f));
            places.push_back(p);
        }
        return places;
    }
    std::string world = Glob.header.worldname;
    const ParamEntry* worlds = Pars.FindEntry("CfgWorlds"); // the game's own config (an Arma 1 world: its config)
    const ParamEntry* cls = worlds && worlds->IsClass() ? worlds->FindEntry(world.c_str()) : nullptr;
    if (!cls || !cls->IsClass())
        cls = Arma1Compat::FindConfigClass("CfgWorlds", world.c_str());
    const ParamEntry* names = cls ? cls->FindEntry("Names") : nullptr;
    for (int i = 0; names && names->IsClass() && i < names->GetEntryCount(); i++)
    {
        const ParamEntry& e = names->GetEntry(i);
        const ParamEntry* pos = e.IsClass() ? e.FindEntry("position") : nullptr;
        if (!pos || !pos->IsArray() || pos->GetSize() < 2)
            continue;
        std::string type = Text(e, "type", "NameVillage");
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        Place p;
        if (type.find("capital") != std::string::npos)
            p.rank = 2;
        else if (type.find("namecity") != std::string::npos)
            p.rank = 1;
        else if (type.find("namevillage") == std::string::npos)
            continue;
        const std::string text = Text(e, "name", static_cast<const char*>(e.GetName()));
        p.name = !text.empty() && (text[0] == '$' || text[0] == '@') ? static_cast<const char*>(LocalizeString(text.c_str() + 1))
                                                                     : text;
        if (p.name.empty())
            p.name = static_cast<const char*>(e.GetName());
        p.x = static_cast<float>((*pos)[0]);
        p.y = static_cast<float>((*pos)[1]);
        places.push_back(p);
    }
    return places;
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
    r.config.capThreeSides = static_cast<int>(Number(cfg, "capThreeSides", static_cast<float>(r.config.cap * 2 / 3)));
    r.config.squadCost = Number(cfg, "squadCost", r.config.squadCost);
    r.config.planSeconds = std::max(5.0f, Number(cfg, "planSeconds", r.config.planSeconds));
    r.config.groupMarkers = Number(cfg, "groupMarkers", 1.0f) != 0.0f;
    r.config.setup = Number(cfg, "setup", 1.0f) != 0.0f;
    r.config.defaults.possession = Number(cfg, "possession", 1.0f) != 0.0f;
    r.config.defaults.startInCommand = Text(cfg, "startAs", "commander") != "soldier";
    r.config.defaults.commanderView = Number(cfg, "commanderView", 1.0f) != 0.0f;
    r.config.defaults.hqRadio = Number(cfg, "hqRadio", 1.0f) != 0.0f;
    r.config.defaults.fog = Text(cfg, "fog", "contacts") == "shroud" ? FogMode::Shroud : FogMode::Contacts;
    // Three sides: the last (the Resistance) is the setup screen's choice, off unless resistance = 1.
    r.config.optionalLast = r.war.sideCount == MaxSides;
    r.config.defaults.resistance = !r.config.optionalLast || Number(cfg, "resistance", 1.0f) != 0.0f;
    r.config.defaults.startPool.fill(r.config.startPool);
    r.config.defaults.victoryTowns = std::max(0, static_cast<int>(Number(cfg, "victoryTowns", 0.0f)));
    r.config.defaults.timeLimit = std::max(0, static_cast<int>(Number(cfg, "timeLimit", 0.0f)));
    r.config.defaults.resistanceAlly = Number(cfg, "resistanceFriendlyWest", 0.0f) != 0.0f;
    r.config.defaults.enemyMarkers = Number(cfg, "enemyMarkers", 0.0f) != 0.0f;
    r.config.defaults.aiSkill = std::clamp(static_cast<int>(Number(cfg, "aiSkill", 1.0f)), 0, 2);
    r.config.barrageRange = Number(cfg, "barrageRange", r.config.barrageRange);
    r.config.supportDelay = std::max(0.0f, Number(cfg, "supportDelay", r.config.supportDelay));
    r.config.airCost = Number(cfg, "airStrikeCost", r.config.airCost);
    r.config.airCooldown = Number(cfg, "airStrikeCooldown", r.config.airCooldown);
    r.config.airBombs = static_cast<int>(Number(cfg, "airStrikeBombs", static_cast<float>(r.config.airBombs)));
    r.config.airSpread = Number(cfg, "airStrikeSpread", r.config.airSpread);
    r.config.airAmmo = Text(cfg, "airStrikeAmmo", r.config.airAmmo.c_str());
    for (int s = 0; s < r.war.sideCount; s++)
        r.config.defaults.llmCommander[s] = Text(cfg, (std::string(SideLabel(r.config.engineSide[s])) + "commander").c_str(),
                                                 "standard") == "llm";
    r.config.postClass = Text(cfg, "postClass", r.config.postClass.c_str());
    r.config.flagClass = Text(cfg, "flagClass", r.config.flagClass.c_str());
    r.config.barrageCost = Number(cfg, "barrageCost", r.config.barrageCost);
    r.config.barrageCooldown = Number(cfg, "barrageCooldown", r.config.barrageCooldown);
    r.config.barrageShells = static_cast<int>(Number(cfg, "barrageShells", static_cast<float>(r.config.barrageShells)));
    r.config.barrageSpread = Number(cfg, "barrageSpread", r.config.barrageSpread);
    r.config.barrageAmmo = Text(cfg, "barrageAmmo", r.config.barrageAmmo.c_str());
    r.config.arsenalRestock = Number(cfg, "arsenalRestock", r.config.arsenalRestock);
    r.config.arsenalWeaponCost = Number(cfg, "arsenalWeaponCost", r.config.arsenalWeaponCost);
    r.config.arsenalMagazineCost = Number(cfg, "arsenalMagazineCost", r.config.arsenalMagazineCost);
    const float captureRadius = Number(cfg, "captureRadius", 30.0f);
    for (int s = 0; s < r.war.sideCount; s++)
    {
        const TargetSide engine = r.config.engineSide[s];
        const std::string label = SideLabel(engine);
        r.config.flagTexture[s] = Text(cfg, (label + "flag").c_str(),
                                       engine == TWest ? "usa_vlajka.pac" : engine == TEast ? "rus_vlajka.pac" : "guba_vlajka.pac");
        r.config.personality[s] = Text(cfg, (label + "personality").c_str(), "balanced");
        r.config.defaults.llmModel[s] = Text(cfg, (label + "llmModel").c_str(), "");
        r.config.airPlane[s] = Text(cfg, (label + "airStrikePlane").c_str(), engine == TWest ? "A10" : "Su25");
        r.config.airPilot[s] = Text(cfg, (label + "airStrikePilot").c_str(),
                                    engine == TWest ? "SoldierWPilot" : engine == TEast ? "SoldierEPilot" : "SoldierGPilot");
        r.config.captureVehicle[s] = Text(cfg, (label + "captureVehicle").c_str(), engine == TEast ? "Ural" : "Truck5tOpen");
        r.config.arsenalClass[s] = Text(cfg, (label + "arsenal").c_str(),
                                        engine == TWest ? "ReammoBoxWest" : engine == TEast ? "ReammoBoxEast" : "ReammoBoxGuer");
        // <SIDE>arsenalWeapons[] = {"M16", 6, "M21", 2, ...}
        r.config.arsenalWeapons[s].clear();
        if (const ParamEntry* list = cfg.FindEntry((label + "arsenalWeapons").c_str()); list && list->IsArray())
        {
            for (int i = 0; i + 1 < list->GetSize(); i += 2)
            {
                const RStringB name = (*list)[i];
                r.config.arsenalWeapons[s].emplace_back(static_cast<const char*>(name), static_cast<int>(static_cast<float>((*list)[i + 1])));
            }
        }
        else if (engine == TWest)
            r.config.arsenalWeapons[s] = {{"M16", 6}, {"M16GrenadeLauncher", 2}, {"M21", 2}, {"M60", 2},
                                          {"LAWLauncher", 3}, {"CarlGustavLauncher", 1}, {"Binocular", 2}};
        else if (engine == TEast)
            r.config.arsenalWeapons[s] = {{"AK74", 6}, {"AK47", 2}, {"SVDDragunov", 2}, {"PK", 2},
                                          {"RPGLauncher", 3}, {"Binocular", 2}};
        else
            r.config.arsenalWeapons[s] = {{"AK47CZ", 4}, {"AK47", 4}, {"Binocular", 2}};
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
    if ((!objectives || !objectives->IsClass()) && Text(cfg, "objectives", "") == "auto")
    {
        // An island nobody wrote a war for: its own town list (CfgWorlds >> world >> Names).
        r.war.defs = AutoObjectives(IslandPlaces(cfg), r.war.sideCount);
        if (r.war.defs.size() < 2)
        {
            LOG_WARN(Mission, "Conquest: the island '{}' names fewer than two towns", Glob.header.worldname);
            return false;
        }
        for (ObjectiveDef& d : r.war.defs)
            d.captureRadius = std::min(captureRadius, d.radius);
        std::string list;
        for (const ObjectiveDef& d : r.war.defs)
            list += (list.empty() ? "" : ", ") + d.name + (d.kind == ObjectiveKind::HQ ? " (HQ " + Name(d.initialOwner) + ")" : "");
        LOG_INFO(Mission, "Conquest: objectives from the island's towns: {}", list);
    }
    else if (!objectives || !objectives->IsClass())
    {
        LOG_WARN(Mission, "Conquest: CfgConquest has no class Objectives (nor objectives = \"auto\")");
        return false;
    }
    std::map<std::string, int> byClass;
    std::vector<std::vector<std::string>> links;
    for (int i = 0; objectives && objectives->IsClass() && i < objectives->GetEntryCount(); i++)
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
    if (r.setupPending && GWorld->GetMode() != GModeNetware)
        LoadSettings(r.options, r.war.sideCount); // the setup screen opens on the last war's settings
    if (GWorld->GetMode() == GModeNetware)
    {
        // Multiplayer: every player has their own slot, side and respawn; no setup screen, no body swapping.
        r.setupPending = false;
        r.options.possession = false;
        r.options.startInCommand = false;
    }
    for (int s = 0; s < MaxSides; s++)
    {
        // The commander's own temper; the LLM commander may change it as the war goes.
        const std::string& p = r.config.personality[s];
        r.posture[s] = p == "aggressive" ? "attack" : p == "cautious" ? "defend" : "balanced";
    }
    r.posts.assign(r.war.defs.size(), Post());
    r.spawnWhy.assign(r.war.defs.size(), std::array<std::string, MaxSides>());
    for (int s = 0; s < MaxSides; s++)
        r.llmPriority[s].assign(r.war.defs.size(), 0.0f);
    Llm::CancelAll(); // answers about the previous war are worthless
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
// The direction from objective `o` to the nearest objective `side` does not hold: where the front is.
void FrontDir(const Referee& r, int side, int o, float& dx, float& dy)
{
    const ObjectiveDef& d = r.war.defs[o];
    float bestDist = 1.0e9f;
    dx = 0.0f;
    dy = 1.0f;
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
}

// Where a new vehicle parks at objective `o`: rows behind the base, away from the front, 28 m apart side
// by side and 35 m deep (a helicopter takes more room). Each vehicle gets the next slot, so a squad's truck,
// the tanks and the helicopter no longer appear on one spot and drive into each other.
Vector3 SpawnPoint(const Referee& r, int side, int o);

Vector3 ParkingSlot(Referee& r, int side, int o, bool big)
{
    if (r.parked.size() != r.war.defs.size())
        r.parked.assign(r.war.defs.size(), 0);
    float fx, fy;
    FrontDir(r, side, o, fx, fy);
    const float px = -fy, py = fx; // across the front
    const ObjectiveDef& d = r.war.defs[o];
    // Dry land only: behind Morton is the beach, and a helicopter parked in the surf counts as unusable --
    // the AI then took it from its squad and could not board it. A slot below 3 m is skipped; with none
    // left, the vehicle parks beside the base's spawn point as it used to.
    for (int tries = 0; tries < 18; tries++)
    {
        const int slot = r.parked[o]++ % 18;
        const int row = slot / 6, col = slot % 6;
        const float lateral = (col % 2 ? 1.0f : -1.0f) * (static_cast<float>(col / 2) + 0.5f) * (big ? 45.0f : 28.0f);
        const float back = d.radius * 0.6f + 30.0f + static_cast<float>(row) * 35.0f;
        const float x = d.x - fx * back + px * lateral, y = d.y - fy * back + py * lateral;
        if (GLandscape && GLandscape->SurfaceY(x, y) >= 3.0f)
            return Ground(x, y);
    }
    const Vector3 at = SpawnPoint(r, side, o);
    const float off = 14.0f * static_cast<float>(r.parked[o] % 6 + 1);
    return Ground(at.X() + off, at.Z() + off * 0.5f);
}

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

// Every squad move goes through here. A squad whose helicopter has set down for good walks: AIGroup::Move
// sends a squad more than 200 m from its destination back into its vehicles, and the parked helicopter is no
// longer theirs (the soak logged the AI's get-in checks failing for its six men). So it goes in hops of 180 m;
// TrackGroups gives the next as soon as one is done. (A squad that left its stuck trucks needs no hops: it gave
// them up, see TrackGroups.)
bool Walks(const GroupRecord& rec)
{
    return rec.parkedHeli;
}

void MoveGroup(GroupRecord& rec, const Vector3& destination)
{
    AIGroup* grp = rec.group;
    Vector3 go = destination;
    rec.detouring = false;
    if (Walks(rec) && grp->Leader())
    {
        const Vector3 p = grp->Leader()->Position();
        const float dx = destination.X() - p.X(), dz = destination.Z() - p.Z();
        const float d = std::hypot(dx, dz);
        if (d > 180.0f)
            go = Ground(p.X() + dx / d * 180.0f, p.Z() + dz / d * 180.0f);
    }
    grp->Move(grp->MainSubgroup(), go, Command::Undefined);
}

void Order(GroupRecord& rec, const ObjectiveDef& d, float now)
{
    AIGroup* grp = rec.group;
    if (!grp || !grp->MainSubgroup())
        return;
    rec.destination = TaskPoint(d, rec.id);
    MoveGroup(rec, rec.destination);
    rec.orderedAt = now;
    rec.holding = false;
    rec.landing = false;
    rec.engageName.clear();
}

void OrderPoint(GroupRecord& rec, const Vector3& destination, float now)
{
    AIGroup* grp = rec.group;
    if (!grp || !grp->MainSubgroup())
        return;
    if (std::hypot(destination.X() - rec.destination.X(), destination.Z() - rec.destination.Z()) > 30.0f)
    {
        rec.bestDistance = 1.0e9f; // somewhere new: progress is measured afresh
        rec.bestAt = now;
        rec.tries = 0;
    }
    rec.destination = destination;
    MoveGroup(rec, rec.destination);
    rec.orderedAt = now;
    rec.holding = false;
    rec.landing = false;
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
// The supply line (ConquestRefereeSupply.inc).
float& Budget(Referee& r, int side, int o);
float BudgetAt(const Referee& r, int side, int o);
bool IsSupply(const Referee& r, const GroupRecord& rec);
int HqOf(const Referee& r, int side);
bool KeepSupplyTrucks(Referee& r, int side, float now);
void LoseTruck(Referee& r, GroupRecord& rec, float now, bool abandoned);
std::vector<std::string> WeaponMagazines(const std::string& weapon);

GameValue RunWith(const char* code, const GameValue& o, const GameValue& p, const RString& t);

// Tanks and aircraft: deployed only at the HQ or an airfield (CONQUEST-PLAN.md 3.4).
bool Heavy(const SquadDef& d)
{
    return d.role == "armour" || d.role == "gunship" || d.role == "air";
}

int SpawnSquad(Referee& r, int side, int base, bool free, int type = 0)
{
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
        return 0;
    const SquadDef& squad = r.config.squads[type];
    AICenter* center = Center(side);
    GameState* gs = GWorld->GetGameState();
    if (!center || !gs || center->NGroups() >= MaxGroups || squad.classes[side].empty())
        return 0;
    // A squad with vehicles of its own deploys beside its first parking slot: its men start at their vehicle
    // (spawned 100 m apart, the AI's get-in lost track of which vehicle was whose and failed its checks).
    const bool big = squad.role == "air" || squad.role == "gunship";
    const bool parks = !squad.vehicles[side].empty();
    const Vector3 firstSpot = parks ? ParkingSlot(r, side, base, big) : VZero;
    const Vector3 at = parks ? Ground(firstSpot.X() + 9.0f, firstSpot.Z() + 9.0f) : SpawnPoint(r, side, base);
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
            static const float skill[3] = {0.35f, 0.6f, 0.85f}; // recruits, regulars, veterans
            RunWith("_o setSkill _p", GameValueExt(unit), GameValue(skill[std::clamp(r.options.aiSkill, 0, 2)]), RString());
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
        const Vector3 spot = parked++ == 0 ? firstSpot : ParkingSlot(r, side, base, big);
        if (EntityAI* veh = CreateVehicleAt(cls, spot.X(), spot.Z()))
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
        Budget(r, side, base) -= squad.cost; // the HQ's pool, or the base's own stock
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
    std::vector<const Transport*> armour; // crewed tanks and APCs already counted
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
                // What a soldier weighs in a capture: a helicopter passing over takes nothing (more
                // than 30 m up), a crewed armoured vehicle adds two men's worth once.
                float weight = 1.0f;
                const Transport* in = unit->GetVehicleIn();
                if (in && p.Y() - Ground(p.X(), p.Z()).Y() > 30.0f)
                    weight = 0.0f;
                else if (in && dynamic_cast<const Tank*>(in) && std::find(armour.begin(), armour.end(), in) == armour.end())
                {
                    armour.push_back(in);
                    weight += 2.0f;
                }
                for (size_t o = 0; o < n; o++)
                {
                    const ObjectiveDef& d = r.war.defs[o];
                    const float dx = p.X() - d.x, dy = p.Z() - d.y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 <= d.captureRadius * d.captureRadius)
                        presence[o].strength[s] += weight;
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
                if (e != s && !Allied(r.war, e, s))
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

#include "ConquestRefereeMove.inc"

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
        Transport* heli = nullptr;
        for (int v = 0; v < grp->NVehicles(); v++)
            if (Transport* t = grp->GetVehicle(v); t && dynamic_cast<Helicopter*>(t) && !t->IsDammageDestroyed())
                heli = t;
        const float toGo = std::hypot(grp->Leader()->Position().X() - rec.destination.X(),
                                      grp->Leader()->Position().Z() - rec.destination.Z());
        if (!crew && !IsSupply(r, rec) && grp->NVehicles() > 0 && rec.task != TaskKind::None &&
            toGo < (heli ? 300.0f : DismountDistance))
            Dismount(rec, heli);
        // Its passengers are out: the helicopter sets down and stays, rather than lifting off to trail the
        // squad at 40 m (the squad boards it again for its next long move).
        if (heli && rec.landing && !rec.parkedHeli)
        {
            bool aboard = false;
            for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
                if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetVehicleIn() == heli && heli->Driver() != unit->GetPerson())
                    aboard = true;
            if (!aboard)
            {
                heli->LandStarted(Transport::LMLand);
                rec.parkedHeli = true;
            }
        }
        // Awaiting orders: the squad stands where it is, in formation round its leader, instead of
        // milling about. Told once; any order clears it.
        if (rec.task == TaskKind::None && !rec.holding && !grp->IsAnyPlayerGroup() && grp->MainSubgroup())
        {
            OrderPoint(rec, grp->Leader()->Position(), now);
            rec.holding = true;
        }
        // A detour is driven (or has had its time): on to the destination again.
        if (rec.detouring)
        {
            const bool done = grp->MainSubgroup() && !grp->MainSubgroup()->HasCommand();
            if (!done && now < rec.detourUntil)
            {
                i++;
                continue;
            }
            MoveGroup(rec, rec.destination); // ends the detour, whether or not it got there
            rec.orderedAt = now;
        }
        if (rec.task == TaskKind::Move)
        {
            const float toDest = std::hypot(grp->Leader()->Position().X() - rec.destination.X(),
                                            grp->Leader()->Position().Z() - rec.destination.Z());
            if (toDest < rec.bestDistance - 25.0f || InContact(r, rec.side, grp->Leader()->Position()))
            {
                rec.bestDistance = std::min(rec.bestDistance, toDest);
                rec.bestAt = now;
                rec.tries = 0;
            }
            if (now >= rec.lockedUntil && !Human(r, rec.side)) // under a human commander his order stands
            {
                rec.task = TaskKind::None; // the commander's point order has run out: back to the AI
                r.replan[rec.side] = true;
            }
            // Stuck on the way to the point (a supply truck, or a squad the commander sent): a vehicle
            // backs off and tries another way; a squad on foot is simply ordered again below. A supply truck
            // waiting at the HQ for a load is not on its way anywhere. One that five detours did not free is
            // left behind (LoseTruck).
            else if (!grp->IsAnyPlayerGroup() && toDest > 60.0f && now - rec.bestAt > NudgeSeconds &&
                     now - rec.nudgedAt > NudgeSeconds && !rec.landing && InGroundVehicle(grp) &&
                     !(IsSupply(r, rec) && rec.supplyState == 0))
            {
                rec.nudgedAt = now;
                rec.nudges++;
                const Vector3 at = grp->Leader()->Position();
                const std::string where = " at " + std::to_string(static_cast<int>(at.X())) + " " +
                                          std::to_string(static_cast<int>(at.Z()));
                if (IsSupply(r, rec) && rec.tries >= 5)
                {
                    WarLog(Name(rec.side) + " supply truck " + Callsign(rec.id) + " stuck" + where);
                    LoseTruck(r, rec, now, true);
                }
                else
                {
                    Detour(rec, now);
                    WarLog(Name(rec.side) + (IsSupply(r, rec) ? " supply truck " + Callsign(rec.id) : " group " + std::to_string(rec.id)) +
                           " stuck" + where + ": backing off to try another way");
                }
            }
            else if (toDest > 30.0f && (now - rec.orderedAt > ReorderSeconds || Walks(rec)) && grp->MainSubgroup() &&
                     !grp->MainSubgroup()->HasCommand())
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
                rec.tries = 0;
            }
            // Stuck on the way (no progress for 90 s, not fighting). A vehicle first backs off and tries
            // another way (Detour). A truck or APC squad still stuck after that gets out and walks for good
            // (vehicles wedge on terrain and in villages); a crew that stays mounted alternates detours with
            // another approach to the town; a squad on foot is ordered again to a point a little to the side,
            // which gives its pathfinding a new start.
            // Only a squad still outside the town is stuck: one in it (a tank at the edge, fighting its way to
            // the post) is where it should be -- the soak nudged one tank 17 times there. A helicopter landing
            // is left alone: a new order lifts it off again.
            if (!grp->IsAnyPlayerGroup() && dist > d.radius && now - rec.bestAt > NudgeSeconds &&
                now - rec.nudgedAt > NudgeSeconds && !rec.landing)
            {
                rec.nudgedAt = now;
                rec.nudges++;
                const bool inGroundVehicle = InGroundVehicle(grp);
                if (inGroundVehicle && (rec.tries < 1 || (crew && rec.tries % 3 != 2)))
                {
                    Detour(rec, now);
                    const Vector3 at = grp->Leader()->Position();
                    WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stuck on the way to " + d.name + " at " +
                           std::to_string(static_cast<int>(at.X())) + " " + std::to_string(static_cast<int>(at.Z())) +
                           ": backing off to try another way");
                }
                else if (inGroundVehicle && !crew)
                {
                    // It leaves the trucks for good (the script command leaveVehicle): the engine's own move
                    // and attack logic sends a squad back to its vehicles on any move over 200-300 m, so a squad
                    // that merely got out was back in the same stuck truck within a minute, and its men, moved
                    // between seats on the way, failed the AI's get-in checks.
                    std::vector<Transport*> trucks;
                    for (int v = 0; v < grp->NVehicles(); v++)
                        if (Transport* t = grp->GetVehicle(v); t && !dynamic_cast<Helicopter*>(t))
                            trucks.push_back(t);
                    Dismount(rec, nullptr);
                    for (Transport* t : trucks)
                        grp->UnassignVehicle(t);
                    grp->GetInVehicles();
                    rec.onFoot = true;
                    WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stuck in its vehicles on the way to " +
                           d.name + ": on foot");
                }
                else if (grp->MainSubgroup())
                {
                    const float a = static_cast<float>(rec.nudges) * 2.39996f + static_cast<float>(rec.id);
                    const float off = 30.0f + 15.0f * static_cast<float>(rec.nudges % 4);
                    rec.destination = Ground(d.x + std::cos(a) * off, d.y + std::sin(a) * off);
                    MoveGroup(rec, rec.destination);
                    rec.orderedAt = now;
                    rec.tries++;
                    WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stuck on the way to " + d.name +
                           ": ordered again by another way");
                }
            }
            if (rec.task == TaskKind::Attack && dist > d.radius && now - rec.bestAt > StallSeconds && !rec.stalled)
            {
                rec.stalled = true;
                r.replan[rec.side] = true;
                WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " stalled on the way to " + d.name);
            }
            // The order can be used up (reached, or replaced by combat); give it again if still far. Not in the
            // tick a detour began: that would cancel it.
            if (!grp->IsAnyPlayerGroup() && dist > d.captureRadius * 1.2f && !rec.detouring &&
                (now - rec.orderedAt > ReorderSeconds || Walks(rec)) && grp->MainSubgroup() &&
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
        if (rec.side != side || IsSupply(r, rec))
            continue; // supply squads run the supply line, not the planner's tasks
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
        if (rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size()))
        {
            const SquadDef& def = r.config.squads[rec.type];
            g.crew = def.mounted;
            g.airborne = def.role == "gunship" || (def.role == "air" && !rec.parkedHeli);
        }
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
    const float pool = BudgetAt(r, side, base); // the HQ's pool, or the base's own stock
    const bool heavyHere = r.war.defs[base].kind == ObjectiveKind::HQ || r.war.defs[base].kind == ObjectiveKind::Airfield;
    const auto affordable = [&](int t)
    {
        const SquadDef& d = r.config.squads[t];
        return d.aiBuys && !d.classes[side].empty() && pool >= d.cost && (heavyHere || !Heavy(d)) &&
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
                if (d.role != role || !d.aiBuys || d.classes[side].empty() || !heavyHere ||
                    r.live[side] + static_cast<int>(d.classes[side].size()) > r.config.cap)
                    continue;
                if (pool >= d.cost)
                    return static_cast<int>(t);
                if (r.live[side] >= r.config.cap / 3)
                    return -1; // save up for it
            }
        }
    }
    if (const std::string& asked = r.llmBuy[side]; !asked.empty())
        for (size_t t = 0; t < r.config.squads.size(); t++)
            if (r.config.squads[t].name == asked && affordable(static_cast<int>(t)))
                return static_cast<int>(t);
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

void AiBarrage(Referee& r, int side, float now);
bool AirStrikeFor(Referee& r, int side, float x, float y, std::string* reason);

void Plan(Referee& r, int side, float now)
{
    SideView view = BuildView(r, side, now);
    // Steers from above: the LLM commander's priorities and the player's request to HQ.
    view.priority = r.llmPriority[side];
    view.priority.resize(r.war.defs.size(), 0.0f);
    if (r.humanPriority[side] >= 0 && now < r.humanPriorityUntil[side])
        view.priority[r.humanPriority[side]] = 3.0f;
    // The posture tunes how much force an attack needs and how much stays at home.
    CommanderTuning tuning = r.commander;
    if (r.posture[side] == "attack")
    {
        tuning.forceRatio = 1.2f;
        tuning.garrisonMen = 6.0f;
    }
    else if (r.posture[side] == "defend")
    {
        tuning.forceRatio = 2.0f;
        tuning.garrisonMen = 12.0f;
    }
    std::vector<Intent> intents = PlanStandard(view, tuning);
    // The supply line keeps its trucks under a human commander too: it was only the AI's, and a side the
    // player commands (from the first tick, with the commander view on) had no supply runs at all (owner,
    // 2026-10-26). The commander can still buy more, pin them, or put squads on supply runs.
    KeepSupplyTrucks(r, side, now);
    // Fire support before purchases: called after them, it found the pool spent on squads and the AI used
    // its mortars and plane almost never (owner, 2026-10-26: none in an 80-minute soak).
    if (!Human(r, side))
        AiBarrage(r, side, now);
    std::map<std::pair<int, int>, std::vector<int>> radioed; // (task, objective) -> squads newly given it
    for (Intent& intent : intents)
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
            int base = intent.objective;
            int type = ChooseSquadType(r, side, base);
            if (type < 0 && base != HqOf(r, side) && HqOf(r, side) >= 0 &&
                SpawnEligible(r.war, side, HqOf(r, side), now, r.tuning))
            {
                base = HqOf(r, side); // the front base has no supplies yet: from the HQ
                type = ChooseSquadType(r, side, base);
            }
            if (type < 0)
                continue;
            intent.objective = base;
            if (SpawnSquad(r, side, base, false, type))
            {
                WarLog(Name(side) + " bought a " + r.config.squads[type].name + " at " +
                       r.war.defs[intent.objective].name + " (" + intent.why + ")");
                if (r.llmBuy[side] == r.config.squads[type].name)
                    r.llmBuy[side].clear(); // the LLM's purchase is made
            }
            continue;
        }
        for (GroupRecord& rec : r.groups)
        {
            if (rec.id != intent.group)
                continue;
            // No squad idles: one without a task always gets the AI's (attack, take, secure). While a human
            // commands the side the AI only fills in, never re-tasks a squad that has one -- re-tasking busy
            // squads every plan was what sent them running back and forth (owner, 2026-10-26).
            if (now < rec.lockedUntil || (Human(r, side) && rec.task != TaskKind::None) ||
                (rec.group && rec.group->IsAnyPlayerGroup()))
                break; // the human commander's order stands, or the player leads this squad
            // The task it already has: nothing to do. (A worn-down squad in a rear town was freed and given the
            // same "regrouping" task back on every plan; each re-order planned its path anew and reset its
            // progress, so it was never seen to be stuck -- 39 to 145 times a war.) A stalled squad is ordered
            // again: that is a new attempt.
            if (rec.task == intent.task && rec.objective == intent.objective && !rec.stalled)
                break;
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
    s.winner = r.winner;
    s.winReason = r.winReason;
    s.queuedPurchases = 0;
    for (const QueuedBuy& q : r.buyQueue)
        if (q.side == r.playerSide)
            s.queuedPurchases++;
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
        if (EntityAI* stocked = post.arsenal)
            for (const auto& [weapon, count] : r.config.arsenalWeapons[owner])
            {
                const auto add = [&](const char* command, const std::string& item, int n)
                {
                    GameValue cargo = GWorld->GetGameState()->CreateGameValue(GameArray);
                    GameArrayType& c = cargo;
                    c.Add(GameValue(RString(item.c_str())));
                    c.Add(GameValue(static_cast<float>(n)));
                    RunWith(command, GameValueExt(stocked), cargo, RString());
                };
                add("_o addWeaponCargo _p", weapon, count);
                // Its ammunition too: the side's ammo box holds only the common magazines (no M21 or SVD rounds
                // in it, owner 2026-10-26), so a weapon the arsenal offers came without anything to fire.
                for (const std::string& magazine : WeaponMagazines(weapon))
                    add("_o addMagazineCargo _p", magazine, count * 5);
            }
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

#include "ConquestRefereeAir.inc"
#include "ConquestRefereeSupply.inc"

std::string Doing(const Referee& r, const GroupRecord& rec);
std::string PlayerName(Person* p);
int Electorate(const Referee& r, int side);
bool ClaimCommand(Referee& r, int side, Person* who, bool on, std::string* why);
void ReleaseCommand(Referee& r, int side);
void StepVotes(Referee& r, float now);

#include "ConquestRefereeNet.inc"

// ---- the LLM commander (Hybrid) ----

std::vector<std::string> ObjectiveNames(const Referee& r)
{
    std::vector<std::string> out;
    for (const ObjectiveDef& d : r.war.defs)
        out.push_back(d.name);
    return out;
}

std::vector<std::string> SquadTypeNames(const Referee& r)
{
    std::vector<std::string> out;
    for (const SquadDef& d : r.config.squads)
        out.push_back(d.name);
    return out;
}

// What `side` knows of the war, for a model: never ground truth (enemy strength comes from the side's
// own target database through BuildView), and the log only where it is the side's business.
std::string SideReport(Referee& r, int side, float now)
{
    const SideView view = BuildView(r, side, now);
    std::string out;
    char line[512];
    std::string enemies;
    for (int s = 0; s < r.war.sideCount; s++)
        if (s != side)
        {
            std::snprintf(line, sizeof(line), "%s%s %.0f", enemies.empty() ? "" : ", ", Name(s).c_str(), r.war.score[s]);
            enemies += line;
        }
    std::snprintf(line, sizeof(line),
                  "You command %s. War time %d min. Supplies %.0f (+%.0f a minute). Soldiers %d of %d allowed. "
                  "War score: yours %.0f, the enemy's %s (a side at 0 loses).\n",
                  Name(side).c_str(), static_cast<int>(now / 60.0f), r.war.pool[side], IncomePerMinute(r.war, side),
                  r.live[side], r.config.cap, r.war.score[side], enemies.c_str());
    out += line;
    if (r.options.victoryTowns > 0)
    {
        std::snprintf(line, sizeof(line), "Holding %d villages (HQs not counted) wins the war; you hold %d.\n",
                      r.options.victoryTowns, OwnedCount(r.war, side, false));
        out += line;
    }
    if (r.config.barrageShells > 0)
    {
        const float wait = std::max(0.0f, r.lastBarrage[side] + r.config.barrageCooldown - now);
        std::snprintf(line, sizeof(line),
                      "Mortars: %d rounds for %.0f supplies, on targets within %.0f m of an objective you hold; %s. "
                      "Name an objective under \"barrage\" and HQ shells the enemy it knows there (or sends the "
                      "attack plane, %.0f supplies, when the mortars cannot).\n",
                      r.config.barrageShells, r.config.barrageCost, r.config.barrageRange,
                      wait > 0.0f ? "reloading" : "ready", r.config.airCost);
        out += line;
    }
    out += "Objectives:\n";
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const ObjectiveDef& d = r.war.defs[o];
        const ObjectiveState& st = r.war.states[o];
        const std::string owner = st.owner == side ? "yours" : st.owner == NoSide ? "neutral" : "the enemy's";
        std::snprintf(line, sizeof(line), "- %s (%s, income %.0f): %s%s%s; enemy your side has seen near it: %.0f\n",
                      d.name.c_str(), d.kind == ObjectiveKind::HQ ? "HQ" : "town", d.value, owner.c_str(),
                      st.contested ? ", contested" : "",
                      st.owner != side && CanAttack(r.war, side, static_cast<int>(o)) ? ", you may attack it" : "",
                      o < view.knownEnemy.size() ? view.knownEnemy[o] : 0.0f);
        out += line;
    }
    out += "Your squads:\n";
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !grp)
            continue;
        int men = 0;
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (Alive(grp->UnitWithID(id)))
                men++;
        const std::string type = rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size())
                                     ? r.config.squads[rec.type].name
                                     : std::string("player squad");
        const std::string task = rec.task == TaskKind::None || rec.objective < 0
                                     ? std::string("no task")
                                     : std::string(TaskName(rec.task)) + " " + r.war.defs[rec.objective].name;
        std::snprintf(line, sizeof(line), "- %s (%s, %d men): %s\n", Callsign(rec.id).c_str(), type.c_str(), men,
                      task.c_str());
        out += line;
    }
    out += "Squad types you can buy:";
    for (const SquadDef& d : r.config.squads)
    {
        std::snprintf(line, sizeof(line), " %s (%.0f);", d.name.c_str(), d.cost);
        out += line;
    }
    if (!r.llmMemory[side].empty())
    {
        // The commander's memory: what it decided before, so it can follow through or change course
        // on purpose rather than start from nothing every few minutes.
        out += "\nYour earlier decisions (oldest first):\n";
        for (const std::string& m : r.llmMemory[side])
            out += "- " + m + "\n";
    }
    out += "\nRecent events:\n";
    const std::string mine = Name(side) + " ";
    int shown = 0;
    for (auto it = r.snapshot.log.rbegin(); it != r.snapshot.log.rend() && shown < 8; ++it)
    {
        const std::string& l = *it;
        const bool hands = l.find(" captured ") != std::string::npos || l.find(" neutralised ") != std::string::npos;
        if (hands || l.find(mine) != std::string::npos)
        {
            out += "- " + l + "\n";
            shown++;
        }
    }
    return out;
}

// Once in a while (and soon after a capture) ask the model; fold its answer in when it is still
// about the war as it stands. Failures are logged and ignored: the Standard commander never stopped.
void LlmStep(Referee& r, float now)
{
    for (int s = 0; s < r.war.sideCount; s++)
    {
        if (!r.options.llmCommander[s] || Human(r, s) || r.war.defeated[s])
            continue;
        Llm::Answer answer;
        if (Llm::TakeAnswer(s, answer))
        {
            // Said once: the model in the settings cannot chat, and the default stands in for it.
            static std::string lastNote;
            if (!answer.note.empty() && answer.note != lastNote)
            {
                lastNote = answer.note;
                WarLog(Name(s) + " LLM commander: " + answer.note + " (AI & voice panel: pick a chat model)");
            }
            if (!answer.ok)
            {
                if (now - r.lastLlmFailure[s] > 300.0f)
                    WarLog(Name(s) + " LLM commander: " + answer.error + "; the standard commander carries on");
                r.lastLlmFailure[s] = now;
            }
            else if (r.llmEpoch[s] != r.captureEpoch)
                WarLog(Name(s) + " LLM commander: the answer came after the front had changed; dropped");
            else
            {
                std::fill(r.llmPriority[s].begin(), r.llmPriority[s].end(), 0.0f);
                for (const auto& [name, weight] : answer.advice.priorities)
                    for (size_t o = 0; o < r.war.defs.size(); o++)
                        if (r.war.defs[o].name == name)
                            r.llmPriority[s][o] = weight;
                r.posture[s] = answer.advice.posture;
                r.llmBuy[s] = answer.advice.buy;
                r.llmReason[s] = answer.advice.reason;
                r.llmBarrage[s] = -1;
                for (size_t o = 0; o < r.war.defs.size(); o++)
                    if (!answer.advice.barrage.empty() && r.war.defs[o].name == answer.advice.barrage)
                        r.llmBarrage[s] = static_cast<int>(o);
                {
                    std::string top;
                    float best = 0.0f;
                    for (const auto& [name, weight] : answer.advice.priorities)
                        if (weight > best)
                        {
                            best = weight;
                            top = name;
                        }
                    char when[48];
                    std::snprintf(when, sizeof(when), "minute %d: ", static_cast<int>(now / 60.0f));
                    std::string m = when + answer.advice.posture + (top.empty() ? std::string() : ", main effort " + top);
                    if (!answer.advice.buy.empty())
                        m += ", buy " + answer.advice.buy;
                    if (!answer.advice.barrage.empty())
                        m += ", mortars on " + answer.advice.barrage;
                    if (!answer.advice.reason.empty())
                        m += " -- \"" + answer.advice.reason + "\"";
                    r.llmMemory[s].push_back(m);
                    while (r.llmMemory[s].size() > 8)
                        r.llmMemory[s].pop_front();
                }
                r.replan[s] = true;
                char took[32];
                std::snprintf(took, sizeof(took), "%.0f s", answer.seconds);
                WarLog(Name(s) + " LLM commander (" + took + ", " + answer.advice.posture + "): " +
                       (answer.advice.reason.empty() ? std::string("new priorities") : answer.advice.reason));
                // The commander keeps its soldiers in the picture: its own situation report, else its intent.
                if (!answer.advice.radio.empty())
                {
                    Radio(r, s, "HQ, situation report: " + answer.advice.radio);
                    WarLog(Name(s) + " HQ situation report: " + answer.advice.radio);
                    r.lastSitrep[s] = now;
                }
                else if (!answer.advice.reason.empty())
                    Radio(r, s, "Commander's intent: " + answer.advice.reason);
            }
        }
        const bool due = now - r.lastLlmAsk[s] > 240.0f || (r.llmEpoch[s] != r.captureEpoch && now - r.lastLlmAsk[s] > 30.0f);
        if (!due || Llm::InFlight(s) || now - r.lastLlmFailure[s] < 60.0f)
            continue;
        std::string why;
        r.lastLlmAsk[s] = now;
        r.llmEpoch[s] = r.captureEpoch;
        if (!Llm::Ask(s, SideReport(r, s, now), ObjectiveNames(r), SquadTypeNames(r), &why, r.options.llmModel[s]))
        {
            if (now - r.lastLlmFailure[s] > 300.0f)
                WarLog(Name(s) + " LLM commander not asked: " + why + "; the standard commander carries on");
            r.lastLlmFailure[s] = now;
        }
    }
}

bool CanBuyFor(const Referee& r, int side, int base, int type, std::string* reason);
int BuyFor(Referee& r, int side, int base, int type, std::string* reason, bool queue = true);
bool BarrageFor(Referee& r, int side, float x, float y, std::string* reason);

// The player's own body (the captive spectator the mission starts with) waits out of the war: on
// land, at least 600 m from every objective, as close to its side's HQ as that allows.
void ParkBody(Referee& r)
{
    Person* body = GWorld->GetRealPlayer();
    if (!body || InMultiplayer() || !GLandscape || (body->Brain() && body->Brain()->GetVehicleIn()))
        return;
    const AIUnit* brain = body->Brain();
    if (!brain || !brain->GetCaptive())
        return; // the mission gave the player a real soldier: leave him
    int hq = -1;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.defs[o].kind == ObjectiveKind::HQ && r.war.defs[o].initialOwner == r.options.side)
            hq = static_cast<int>(o);
    const float size = static_cast<float>(LandRange) * LandGrid;
    float best = -1.0e9f, bx = 0.0f, by = 0.0f;
    for (float x = 100.0f; x < size; x += 150.0f)
        for (float y = 100.0f; y < size; y += 150.0f)
        {
            if (GLandscape->SurfaceY(x, y) < 3.0f)
                continue; // sea or shore
            float nearest = 1.0e9f;
            for (const ObjectiveDef& d : r.war.defs)
                nearest = std::min(nearest, std::hypot(d.x - x, d.y - y));
            if (nearest < 600.0f)
                continue;
            const float score = std::min(nearest, 900.0f) - (hq >= 0 ? 0.1f * std::hypot(r.war.defs[hq].x - x, r.war.defs[hq].y - y) : 0.0f);
            if (score > best)
            {
                best = score;
                bx = x;
                by = y;
            }
        }
    if (best <= -1.0e9f)
        return;
    RunWith("_o setPos _p", GameValueExt(body), Position(bx, by, 0.0f));
    WarLog("the player's body waits at " + std::to_string(static_cast<int>(bx)) + ", " + std::to_string(static_cast<int>(by)));
}

// The Resistance with WEST: allied in the rules (no attacks on each other's towns, one force in a zone).
void ApplyAlliance(Referee& r)
{
    r.war.allied = {};
    if (!r.options.resistanceAlly || r.war.sideCount < 3)
        return;
    int west = NoSide, guer = NoSide;
    for (int s = 0; s < r.war.sideCount; s++)
    {
        if (r.config.engineSide[s] == TWest)
            west = s;
        if (r.config.engineSide[s] == TGuerrila)
            guer = s;
    }
    if (west != NoSide && guer != NoSide)
        r.war.allied[west][guer] = r.war.allied[guer][west] = true;
}

// HQ's situation report every few minutes, for a side whose commander does not write its own (the
// LLM commander's answer carries one): towns held, where the fighting is, what to do.
constexpr float SitrepSeconds = 300.0f;
void Sitreps(Referee& r, float now)
{
    for (int s = 0; s < r.war.sideCount; s++)
    {
        // An LLM commander reports itself; HQ steps in only when it has been silent for twice as long.
        const float every = r.options.llmCommander[s] && !Human(r, s) ? 2.0f * SitrepSeconds : SitrepSeconds;
        if (r.war.defeated[s] || now - r.lastSitrep[s] < every)
            continue;
        if (r.lastSitrep[s] == 0.0f)
        {
            r.lastSitrep[s] = now; // the first one after five minutes of war
            continue;
        }
        r.lastSitrep[s] = now;
        int held = 0, total = 0, contested = -1, threatened = -1, threat = 0;
        for (size_t o = 0; o < r.war.defs.size(); o++)
        {
            total++;
            const ObjectiveState& st = r.war.states[o];
            if (st.owner == s)
                held++;
            if (st.contested && contested < 0)
                contested = static_cast<int>(o);
            if (st.owner == s && st.enemiesNear[s] > threat)
            {
                threat = st.enemiesNear[s];
                threatened = static_cast<int>(o);
            }
        }
        std::string text = "HQ, situation report: we hold " + std::to_string(held) + " of " + std::to_string(total) + ".";
        if (contested >= 0)
            text += " Fighting at " + r.war.defs[contested].name + ".";
        if (threatened >= 0)
            text += " Enemy closing on " + r.war.defs[threatened].name + ", reinforce it.";
        else if (r.humanPriority[s] >= 0 && now < r.humanPriorityUntil[s])
            text += " Main effort " + r.war.defs[r.humanPriority[s]].name + ".";
        Radio(r, s, text);
    }
}

// The war begins: the Resistance in or out, each side's supplies, everybody hostile to everybody.
void StartSides(Referee& r)
{
    if (r.config.optionalLast && !r.options.resistance && r.war.sideCount == MaxSides)
    {
        DropLastSide(r.war, r.tuning, 0.0f);
        WarLog(Name(r.war.sideCount) + " stays out of this war; its base is a neutral village");
    }
    for (int s = 0; s < r.war.sideCount; s++)
        r.war.pool[s] = std::max(0.0f, r.options.startPool[s]);
    if (r.war.sideCount > 2)
        r.config.cap = r.config.capThreeSides; // three armies share the AI budget two had
    if (r.options.side >= r.war.sideCount)
        r.options.side = 0;
    ApplyAlliance(r);
    if (r.war.sideCount > 2)
        if (GameState* gs = GWorld->GetGameState())
            gs->Execute(r.options.resistanceAlly
                            ? "west setFriend [resistance, 1]; resistance setFriend [west, 1]; "
                              "east setFriend [resistance, 0]; resistance setFriend [east, 0]"
                            : "west setFriend [resistance, 0]; resistance setFriend [west, 0]; "
                              "east setFriend [resistance, 0]; resistance setFriend [east, 0]");
    r.warStart = Now();
    if (r.options.resistanceAlly && r.war.sideCount > 2)
        WarLog("the Resistance fights with WEST");
    if (r.options.timeLimit > 0)
        WarLog("time limit: " + std::to_string(r.options.timeLimit) + " minutes");
}

// The player's purchases waiting for supplies or a quiet base.
void StepBuyQueue(Referee& r, float now)
{
    for (size_t i = 0; i < r.buyQueue.size();)
    {
        const QueuedBuy q = r.buyQueue[i];
        std::string why;
        if (now > q.until || r.war.states[q.base].owner != q.side)
        {
            r.buyQueue.erase(r.buyQueue.begin() + static_cast<std::ptrdiff_t>(i));
            Radio(r, q.side, "The " + r.config.squads[q.type].name + " for " + r.war.defs[q.base].name +
                                 " is cancelled: it could not deploy.");
            continue;
        }
        if (CanBuyFor(r, q.side, q.base, q.type, &why))
        {
            r.buyQueue.erase(r.buyQueue.begin() + static_cast<std::ptrdiff_t>(i));
            if (const int id = BuyFor(r, q.side, q.base, q.type, nullptr, false))
                Radio(r, q.side, Callsign(id) + ", " + r.config.squads[q.type].name + ", ready at " + r.war.defs[q.base].name + ".");
            continue;
        }
        i++;
    }
}

// One line per finished war in the profile's conquest-history.txt.
void RecordHistory(const Referee& r, float now)
{
    const std::string path = GamePaths::Instance().UserDir() + "conquest-history.txt";
    if (FILE* f = std::fopen(path.c_str(), "a"))
    {
        const std::time_t t = std::time(nullptr);
        char date[32];
        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", std::localtime(&t));
        std::string line = std::string(date) + "  " + r.mission + "  " + Name(r.winner) + " won (" + r.winReason + ") after " +
                           std::to_string(static_cast<int>(now / 60.0f)) + " min; player " + Name(r.playerSide);
        for (int s = 0; s < r.war.sideCount; s++)
            line += "; " + Name(s) + " held " + std::to_string(OwnedCount(r.war, s)) + ", lost " + std::to_string(r.losses[s]);
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

void Tick(Referee& r, float dt, float now)
{
    if (!r.started)
    {
        if (r.setupPending)
            return; // the setup screen first: side and options
        r.started = true;
        StartSides(r);
        ParkBody(r);
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
    StepSupply(r, dt, now);
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
        r.captureEpoch++;
        if (result.event == CaptureEvent::Captured)
        {
            GiveCaptureVehicle(r, result.side, o);
            Radio(r, result.side, "The arsenal at " + name + " is ours: restock there.");
        }
        FlyFlag(r, o);
        for (int s = 0; s < r.war.sideCount; s++)
            r.replan[s] = true;
    }
    int winner = StepEconomy(r.war, dt, r.tuning);
    std::string reason = "the last side standing";
    if (const int towns = TownVictor(r.war, r.options.victoryTowns); winner == NoSide && towns != NoSide)
    {
        winner = towns;
        reason = "holds " + std::to_string(OwnedCount(r.war, towns, false)) + " villages";
    }
    if (winner == NoSide && r.options.timeLimit > 0 && r.warStart >= 0.0f &&
        now - r.warStart >= static_cast<float>(r.options.timeLimit) * 60.0f)
    {
        winner = ScoreLeader(r.war);
        reason = "time is up, the higher war score";
    }
    else if (winner != NoSide && reason == "the last side standing")
        for (int s = 0; s < r.war.sideCount; s++)
            if (Allied(r.war, s, winner) && !r.war.defeated[s])
                reason = "with its ally, the last forces standing";
    if (winner != NoSide && !r.ended)
    {
        r.ended = true;
        r.winner = winner;
        r.winReason = reason;
        WarLog(Name(winner) + " wins the war (" + reason + ")");
        for (int s = 0; s < r.war.sideCount; s++)
            Radio(r, s, s == winner ? "All units: the war is won." : "All units: the war is lost.");
        RecordHistory(r, now);
        Status(r);
    }
    if (!r.ended)
        StepBuyQueue(r, now);
    TrackGroups(r, now);
    if (!r.ended)
        LlmStep(r, now);
    if (!r.ended)
        for (int s = 0; s < r.war.sideCount; s++)
            if (!r.war.defeated[s] && (r.replan[s] || now - r.lastPlan[s] >= r.config.planSeconds))
                Plan(r, s, now);
    if (InMultiplayer())
    {
        AdoptPlayerGroups(r);
        StepVotes(r, now);
        if (r.localCommand && r.playerSide != NoSide)
        {
            std::string why;
            if (!ClaimCommand(r, r.playerSide, GWorld->GetRealPlayer(), true, &why))
            {
                r.localCommand = false;
                r.commandLoss = why.empty() ? "You no longer command." : why;
            }
        }
    }
    if (!r.ended)
    {
        HqOrders(r, now);
        Sitreps(r, now);
    }
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
        if (r.active && !r.started && IsAuthority())
            ParkBody(r); // off the battlefield before the setup screen (and again once a side is chosen)
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
    StepStrikes(r, now);
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
        PARAM_CHECK(ar.Serialize("resistance", w.options.resistance, 1, false))
        PARAM_CHECK(ar.Serialize("victoryTowns", w.options.victoryTowns, 1, 0))
        PARAM_CHECK(ar.Serialize("timeLimit", w.options.timeLimit, 1, 0))
        PARAM_CHECK(ar.Serialize("resistanceAlly", w.options.resistanceAlly, 1, false))
        PARAM_CHECK(ar.Serialize("enemyMarkers", w.options.enemyMarkers, 1, false))
        PARAM_CHECK(ar.Serialize("aiSkill", w.options.aiSkill, 1, 1))
        PARAM_CHECK(ar.Serialize("warStart", w.warStart, 1, -1.0f))
        PARAM_CHECK(ar.Serialize("winner", w.winner, 1, NoSide))
        PARAM_CHECK(SerializeText(ar, "winReason", w.winReason))
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
        PARAM_CHECK(sub.Serialize("startPool", w.options.startPool[s], 1, 500.0f))
        PARAM_CHECK(SerializeText(sub, "llmModel", w.options.llmModel[s]))
        bool llm = w.options.llmCommander[s];
        PARAM_CHECK(sub.Serialize("llmCommander", llm, 1, false))
        w.options.llmCommander[s] = llm;
        int lines = static_cast<int>(w.llmMemory[s].size());
        PARAM_CHECK(sub.Serialize("llmMemoryLines", lines, 1, 0))
        w.llmMemory[s].resize(static_cast<size_t>(std::clamp(lines, 0, 8)));
        for (size_t i = 0; i < w.llmMemory[s].size(); i++)
            PARAM_CHECK(SerializeText(sub, Key("llmMemory", i).c_str(), w.llmMemory[s][i]))
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
        PARAM_CHECK(sub.Serialize("stock", s.stock, 1, 0.0f))
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
        PARAM_CHECK(sub.Serialize("onFoot", g.onFoot, 1, false))
        PARAM_CHECK(sub.Serialize("truckless", g.truckless, 1, false))
        PARAM_CHECK(sub.Serialize("parkedHeli", g.parkedHeli, 1, false))
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
    if (w.started && r.config.optionalLast && !w.options.resistance)
        DropLastSide(r.war, r.tuning, 0.0f); // the Resistance stayed out of the saved war
    r.winner = w.winner;
    r.winReason = w.winReason;
    r.warStart = w.warStart;
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
    r.llmMemory = w.llmMemory;
    r.lastTick = w.lastTick;
    r.lastStatus = w.lastStatus;
    r.snapshot.log = w.log;
    ApplyAlliance(r);
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
        w.llmMemory = r.llmMemory;
        w.winner = r.winner;
        w.winReason = r.winReason;
        w.warStart = r.warStart;
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
// The squad leader answers on the radio. With NPC chat's voices on, in his own voice and in words: "Bravo
// copies, attacking Figari" (the owner did not hear the game's own confirmation, 2026-10-26); without them,
// the game's radio ("Moving" / "Attacking"). `doing` replaces what the task says (a command from the bar).
void VoiceConfirm(const GroupRecord& rec, bool attack, const std::string& doing = std::string())
{
    AIGroup* grp = rec.group;
    AICenter* center = grp ? grp->GetCenter() : nullptr;
    if (!center || !grp->Leader() || grp->Leader()->GetLifeState() != AIUnit::LSAlive)
        return;
    const Referee& r = R();
    if (rec.side == r.playerSide && !IsMirror())
    {
        const std::string place =
            rec.objective >= 0 && rec.objective < static_cast<int>(r.war.defs.size()) ? r.war.defs[rec.objective].name : "";
        std::string what = doing;
        if (what.empty())
            what = !rec.engageName.empty() && rec.task == TaskKind::None ? "engaging the " + rec.engageName
                   : rec.task == TaskKind::Attack && !place.empty()       ? "attacking " + place
                   : rec.task == TaskKind::Defend && !place.empty()       ? "moving to hold " + place
                   : rec.task == TaskKind::Move                           ? "moving to the marked point"
                   : attack                                               ? "engaging"
                                                                          : "on our way";
        const std::string line = Callsign(rec.id) + " copies, " + what + ".";
        if (NpcChat::SayOnRadio(Callsign(rec.id), rec.id * 7 + rec.side, line))
            return;
    }
    Command cmd;
    cmd._message = attack ? Command::Attack : Command::Move;
    cmd._destination = rec.destination;
    center->GetRadio().Transmit(new RadioMessageCommandConfirm(grp->Leader(), grp, cmd), center->GetLanguage());
}

std::string OrderRecords(Referee& r, int side, const std::vector<GroupRecord*>& records, float x, float y,
                         const Person* by = nullptr)
{
    if (!by && !IsMirror())
        by = GWorld->GetRealPlayer();
    const float now = Now();
    int target = -1;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (std::hypot(r.war.defs[o].x - x, r.war.defs[o].y - y) <= r.war.defs[o].radius * 1.2f)
            target = static_cast<int>(o);
    std::string text;
    for (GroupRecord* rec : records)
    {
        // A supply squad told to one of the side's bases: its supply run goes there.
        if (IsSupply(r, *rec) && target >= 0 && r.war.states[target].owner == side &&
            r.war.defs[target].kind != ObjectiveKind::HQ)
        {
            rec->supplyPin = target;
            rec->selected = false;
            text += " " + Callsign(rec->id) + " supply run to " + r.war.defs[target].name;
            WarLog("commander: " + Callsign(rec->id) + " supply run to " + r.war.defs[target].name);
            continue;
        }
        // A squad a human leads (another player) is never moved by the commander: it gets the order
        // as HQ's request on its card and the radio, and its player decides.
        if (AIGroup* grp = rec->group; grp && grp->Leader() && grp->Leader()->GetPerson() != by &&
                                       IsHumanPlayer(grp->Leader()->GetPerson()))
        {
            const bool own = target >= 0 && r.war.states[target].owner == side;
            rec->hqText = target >= 0 ? std::string(own ? "Commander asks: defend " : "Commander asks: attack ") + r.war.defs[target].name
                                      : std::string("Commander asks: move to the marked point");
            rec->hqX = target >= 0 ? r.war.defs[target].x : x;
            rec->hqY = target >= 0 ? r.war.defs[target].y : y;
            Radio(r, side, Callsign(rec->id) + ", the commander asks you to " +
                               (target >= 0 ? std::string(own ? "defend " : "attack ") + r.war.defs[target].name : std::string("move to the marked point")) + ".");
            rec->selected = false;
            text += " " + Describe(r, *rec) + " (asked: a player leads it)";
            continue;
        }
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

int OrderGroupsFor(Referee& r, int side, const std::vector<int>& ids, float x, float y, const Person* by = nullptr)
{
    if (side == NoSide)
        return 0;
    std::vector<GroupRecord*> records;
    for (GroupRecord& rec : r.groups)
        if (rec.side == side && rec.group && std::find(ids.begin(), ids.end(), rec.id) != ids.end())
            records.push_back(&rec);
    if (records.empty())
        return 0;
    OrderRecords(r, side, records, x, y, by);
    return static_cast<int>(records.size());
}

#include "ConquestRefereeOrders.inc"

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
    if (Heavy(def) && r.war.defs[base].kind != ObjectiveKind::HQ && r.war.defs[base].kind != ObjectiveKind::Airfield)
        return refuse("tanks and aircraft deploy at the HQ (or an airfield)");
    if (def.role == "supply" && r.war.defs[base].kind != ObjectiveKind::HQ)
        return refuse("supply trucks are loaded at the HQ: deploy them there");
    if (BudgetAt(r, side, base) < def.cost)
        return refuse(r.war.defs[base].kind == ObjectiveKind::HQ ? "not enough supplies"
                                                                  : "not enough supplies at this base: bring a supply truck");
    if (r.live[side] + static_cast<int>(def.classes[side].size()) > r.config.cap)
        return refuse("manpower cap reached");
    return true;
}

int BuyFor(Referee& r, int side, int base, int type, std::string* reason, bool queue)
{
    if (!CanBuyFor(r, side, base, type, reason))
    {
        // Short of supplies, or the base is not quiet: the order waits, and is made when it can be.
        if (!queue || IsMirror() || type < 0 || type >= static_cast<int>(r.config.squads.size()) || base < 0 ||
            base >= static_cast<int>(r.war.defs.size()) || r.war.states[base].owner != side)
            return 0;
        int waiting = 0;
        for (const QueuedBuy& q : r.buyQueue)
            if (q.side == side)
                waiting++;
        if (waiting >= static_cast<int>(QueueMax))
            return 0;
        r.buyQueue.push_back({side, base, type, Now() + QueueSeconds});
        WarLog("commander: " + Name(side) + " queued a " + r.config.squads[type].name + " at " + r.war.defs[base].name +
               (reason ? " (" + *reason + ")" : std::string()));
        if (reason)
            *reason = "queued: " + *reason + "; it deploys when it can (2 min)";
        return 0;
    }
    const int id = SpawnSquad(r, side, base, false, type);
    if (!id)
    {
        if (reason)
            *reason = "the squad could not be created";
        return 0;
    }
    // A squad the commander bought waits where it deployed for his orders, like a factory's output --
    // for two minutes; then, still without an order, the AI puts it to work (no squad idles).
    for (GroupRecord& rec : r.groups)
        if (rec.id == id)
        {
            rec.task = TaskKind::None;
            rec.objective = -1;
            rec.lockedUntil = Now() + BoughtWaitSeconds;
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
    if (const float wait = SupportWait(r, now); wait > 0.0f)
    {
        const std::string text = "support is ready " + std::to_string(static_cast<int>(wait)) + " s from now";
        return refuse(text.c_str());
    }
    if (r.war.pool[side] < r.config.barrageCost)
        return refuse("not enough supplies");
    // The mortars stand at the side's objectives: a target far from all of them is out of range.
    bool inRange = false;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.states[o].owner == side &&
            std::hypot(r.war.defs[o].x - x, r.war.defs[o].y - y) <= r.config.barrageRange)
            inRange = true;
    if (!inRange)
        return refuse("out of mortar range");
    r.war.pool[side] -= r.config.barrageCost;
    r.lastBarrage[side] = now;
    // The first round lands after the flight time, the rest a second or so apart, scattered.
    unsigned seed = static_cast<unsigned>(now * 1000.0f) ^ 0x9E3779B9u;
    const auto random = [&]()
    {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    // Accurate with an observer: a soldier of the side within a kilometre of the target walks the rounds in
    // (40 m); without one they land by the map, scattered over 120 m (CONQUEST-PLAN.md 3.8).
    bool observer = false;
    if (AICenter* center = GWorld->GetCenter(r.config.engineSide[side]))
        for (int g = 0; g < center->NGroups() && !observer; g++)
            if (AIGroup* grp = center->GetGroup(g))
                for (int id = 1; id <= MAX_UNITS_PER_GROUP && !observer; id++)
                    if (AIUnit* unit = grp->UnitWithID(id); Alive(unit) &&
                                                         std::hypot(unit->Position().X() - x, unit->Position().Z() - y) < 1000.0f)
                        observer = true;
    const float spread = observer ? r.config.barrageSpread : std::max(r.config.barrageSpread, 120.0f);
    for (int i = 0; i < r.config.barrageShells; i++)
    {
        const float a = random() * 6.2831853f, d = std::sqrt(random()) * spread;
        r.shells.push_back({now + 7.0f + static_cast<float>(i) * 1.1f + random() * 0.6f, x + std::cos(a) * d,
                            y + std::sin(a) * d});
    }
    char where[64];
    std::snprintf(where, sizeof(where), "%.0f, %.0f", x, y);
    WarLog(Name(side) + " mortar barrage on " + where);
    Radio(r, side, "Mortars firing, " + std::to_string(r.config.barrageShells) + " rounds. Splash in seven seconds." +
                       (observer ? "" : " No observer near the target: the rounds will scatter."));
    // Friendly squads near the target hear it is coming.
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !grp || !grp->Leader())
            continue;
        const Vector3 p = grp->Leader()->Position();
        if (std::hypot(p.X() - x, p.Z() - y) < 300.0f)
            Radio(r, side, Callsign(rec.id) + ", danger close: friendly mortars on your position, take cover.");
    }
    return true;
}

#include "ConquestRefereeCommand.inc"

// The server's side of `conquestRequest`: [player, kind, args] from a client's commander view.
void HandleRequest(Referee& r, const GameValue& value)
{
    if (!r.active || !IsAuthority() || value.GetType() != GameArray)
        return;
    const GameArrayType& a = value;
    if (a.Size() < 3 || a[2].GetType() != GameArray)
        return;
    Person* who = dyn_cast<Person>(GetObject(a[0]));
    const int side = SideOfUnit(r, who);
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
        OrderGroupsFor(r, side, ids, Num(args, 1), Num(args, 2), who);
    }
    else if (kind == "buy" && args.Size() >= 2)
    {
        const int id = BuyFor(r, side, static_cast<int>(Num(args, 0, -1)), static_cast<int>(Num(args, 1, -1)), nullptr);
        if (id)
            Radio(r, side, "Commander, " + Callsign(id) + " is ready at " + r.war.defs[static_cast<int>(Num(args, 0))].name + ".");
    }
    else if (kind == "barrage" && args.Size() >= 2)
        BarrageFor(r, side, Num(args, 0), Num(args, 1), nullptr);
    else if (kind == "squad" && args.Size() >= 2 && args[0].GetType() == GameArray)
    {
        std::vector<int> ids;
        const GameArrayType& list = args[0];
        for (int i = 0; i < list.Size(); i++)
            ids.push_back(static_cast<int>(Num(list, i)));
        CommandSquadsFor(r, side, ids, static_cast<SquadCommand>(static_cast<int>(Num(args, 1))));
    }
    else if (kind == "supply" && args.Size() >= 1)
    {
        const int base = static_cast<int>(Num(args, 0, -1));
        const int keep = r.playerSide;
        r.playerSide = side; // SendSupplyTo works for the asking player's side
        SendSupplyTo(base, nullptr);
        r.playerSide = keep;
    }
    else if (kind == "target" && args.Size() >= 3 && args[0].GetType() == GameArray)
    {
        std::vector<int> ids;
        const GameArrayType& list = args[0];
        for (int i = 0; i < list.Size(); i++)
            ids.push_back(static_cast<int>(Num(list, i)));
        EngageFor(r, side, ids, Num(args, 1), Num(args, 2));
    }
    else if (kind == "arsenal" && args.Size() >= 3)
    {
        // A client took gear from an arsenal box (the Gear screen): its side pays, down to nothing.
        if (EntityAI* box = dyn_cast<EntityAI>(GetObject(args[0])))
            for (size_t o = 0; o < r.posts.size(); o++)
                if (r.posts[o].arsenal.GetLink() == box && r.posts[o].arsenalSide >= 0 && r.posts[o].arsenalSide < MaxSides)
                {
                    const int owner = r.posts[o].arsenalSide;
                    const float cost = Flag(args, 1) ? r.config.arsenalWeaponCost
                                                     : r.config.arsenalMagazineCost * std::max(1.0f, Num(args, 2, 1.0f));
                    float& budget = Budget(r, owner, static_cast<int>(o));
                    budget = std::max(0.0f, budget - cost);
                }
    }
    else if (kind == "air" && args.Size() >= 2)
        AirStrikeFor(r, side, Num(args, 0), Num(args, 1), nullptr);
    else if (kind == "command" && args.Size() >= 1)
        ClaimCommand(r, side, who, Flag(args, 0), nullptr);
    else if (kind == "vote" && args.Size() >= 1)
    {
        const std::string what = Str(args, 0);
        if (what == "start")
            StartVote(r, side, who);
        else
            Ballot(r, side, who, what == "yes");
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
    if (person && person != GWorld->GetRealPlayer() && IsHumanPlayer(person))
    {
        if (reason)
            *reason = "a player controls this soldier";
        return false;
    }
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

std::string RequestSupport(const std::string& objective, bool air)
{
    Referee& r = R();
    if (!r.active || !r.started || r.playerSide == NoSide)
        return "HQ: there is no war to support.";
    return SupportRequestFor(r, r.playerSide, objective, air);
}

void SetSupportDelay(float seconds)
{
    R().config.supportDelay = std::max(0.0f, seconds);
}

float TimeLeft()
{
    const Referee& r = R();
    if (IsMirror())
        return r.netTimeLeft;
    if (!r.started || r.options.timeLimit <= 0 || r.warStart < 0.0f)
        return -1.0f;
    return std::max(0.0f, r.warStart + static_cast<float>(r.options.timeLimit) * 60.0f - Now());
}

int OptionalSide()
{
    const Referee& r = R();
    return r.active && !r.started && r.config.optionalLast ? r.war.sideCount - 1 : NoSide;
}

int VictoryTownsMax(bool resistance)
{
    const Referee& r = R();
    int n = TownCount(r.war);
    if (!r.started && r.config.optionalLast && !resistance)
        for (const ObjectiveDef& d : r.war.defs)
            if (d.kind == ObjectiveKind::HQ && d.initialOwner == r.war.sideCount - 1)
                n++;
    return n;
}

bool NewWar()
{
    Referee& r = R();
    if (!r.active || !r.ended || InMultiplayer())
        return false;
    if (Possessing())
        ReturnToCommander();
    Person* me = GWorld->GetRealPlayer();
    // Clear the island: every soldier and vehicle the war made, the posts, the shells in the air.
    for (UnitRecord& u : r.units)
    {
        EntityAI* unit = u.unit;
        if (!unit || unit == me)
            continue;
        if (Transport* veh = dyn_cast<Transport>(unit); veh && me && me->Brain() && me->Brain()->GetVehicleIn() == veh)
            continue; // the player sits in it
        DeleteVehicle(unit);
    }
    for (Strike& strike : r.strikes)
        ClearStrike(strike);
    for (Post& post : r.posts)
        for (EntityAI* e : {post.tent.GetLink(), post.flag.GetLink(), post.arsenal.GetLink()})
            if (e)
                DeleteVehicle(e);
    for (GroupRecord& rec : r.groups)
        if (AIGroup* grp = rec.group; grp && !grp->IsAnyPlayerGroup() && grp->NUnits() == 0)
            grp->RemoveFromCenter();
    if (GameState* gs = GWorld->GetGameState())
        for (const auto& [id, text] : r.groupMarkerText)
            gs->Execute(RString(("deleteMarker \"conquest_grp_" + std::to_string(id) + "\"").c_str()));
    WarLog("a new war: the island is cleared");
    r.mission.clear(); // the next Simulate reads CfgConquest again and opens the setup screen
    return true;
}

void EndMission()
{
    if (GameState* gs = GWorld ? GWorld->GetGameState() : nullptr)
        gs->Execute("endMission \"END1\"");
}

bool BeginWar(const Options& options)
{
    Referee& r = R();
    if (!r.active || r.started || options.side < 0 || options.side >= r.war.sideCount)
        return false;
    if (r.config.optionalLast && !options.resistance && options.side == r.war.sideCount - 1)
        return false; // the Resistance is out of this war
    r.options = options;
    r.setupPending = false;
    SaveSettings(options); // the next war's setup opens on these
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
    const FogMode fog = r.options.fog;
    const bool markers = r.options.enemyMarkers;
    r.options = options;
    if (r.started)
    {
        // Fixed once the war runs: the side, the start, and the fog of war (a mid-game switch off the
        // shroud would show what the side has not seen).
        r.options.side = side;
        r.options.startInCommand = start;
        r.options.fog = fog;
        r.options.enemyMarkers = markers; // like the fog: what the side sees is set before the war
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
                                    : std::max({0.0f, r.lastBarrage[r.playerSide] + r.config.barrageCooldown - Now(),
                                                SupportWait(r, Now())});
    return b;
}

AirStrikeInfo AirStrike()
{
    const Referee& r = R();
    AirStrikeInfo a;
    a.cost = r.config.airCost;
    a.bombs = r.config.airBombs;
    a.spread = r.config.airSpread;
    if (r.playerSide != NoSide)
    {
        a.cooldownLeft = IsMirror() ? r.netAirCooldown[r.playerSide]
                                    : std::max({0.0f, r.lastAir[r.playerSide] + r.config.airCooldown - Now(),
                                                SupportWait(r, Now())});
        a.inbound = IsMirror() ? r.netAirInbound[r.playerSide]
                               : std::any_of(r.strikes.begin(), r.strikes.end(), [&](const Strike& s) { return s.side == r.playerSide; });
    }
    return a;
}

bool OrderAirStrike(float x, float y, std::string* reason)
{
    Referee& r = R();
    if (!IsMirror())
        return AirStrikeFor(r, r.playerSide, x, y, reason);
    const AirStrikeInfo a = AirStrike();
    if (a.cooldownLeft > 0.0f || a.inbound || r.playerSide == NoSide || r.war.pool[r.playerSide] < a.cost)
    {
        if (reason)
            *reason = a.inbound ? "a plane is already on its way" : a.cooldownLeft > 0.0f ? "the plane is rearming" : "not enough supplies";
        return false;
    }
    GameValue args = NetArray();
    static_cast<GameArrayType&>(args).Add(GameValue(x));
    static_cast<GameArrayType&>(args).Add(GameValue(y));
    Request("air", args);
    return true;
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
        r.commandSince = Now();
        GameValue args = NetArray();
        GameArrayType& a = args;
        a.Add(GameValue(on));
        Request("command", args);
        return;
    }
    if (r.playerSide == NoSide)
        return;
    if (!InMultiplayer())
    {
        if (on)
            r.humanUntil[r.playerSide] = 1.0e9f;
        else
            ReleaseCommand(r, r.playerSide);
        return;
    }
    // A hosting player: the seat, renewed by the tick while the view is open.
    r.localCommand = on;
    std::string why;
    if (!ClaimCommand(r, r.playerSide, GWorld->GetRealPlayer(), on, &why) && on)
    {
        r.localCommand = false;
        r.commandLoss = why;
    }
}

CommandSeat Seat()
{
    Referee& r = R();
    CommandSeat c;
    c.multiplayer = InMultiplayer();
    const int side = r.playerSide;
    if (!r.active || !c.multiplayer || side == NoSide)
        return c;
    Person* me = GWorld->GetRealPlayer();
    if (IsMirror())
    {
        c = r.netVote[side];
        c.multiplayer = true;
        c.holder = r.netSeat[side] ? r.netSeatName[side] : std::string();
        c.mine = me && r.netSeat[side].GetLink() == me;
    }
    else
    {
        Person* holder = r.seat[side];
        const bool held = holder && Now() < r.humanUntil[side];
        c.holder = held ? PlayerName(holder) : std::string();
        c.mine = held && holder == me;
        const Referee::Vote& v = r.vote[side];
        c.vote = v.active;
        c.voteLeft = std::max(0.0f, v.until - Now());
        c.yes = static_cast<int>(v.yes.size());
        c.no = static_cast<int>(v.no.size());
        c.electorate = Electorate(r, side);
        c.voteTarget = v.targetName;
        c.voteCaller = v.callerName;
    }
    c.myVote = c.vote ? r.myVote : 0;
    return c;
}

bool CanTakeCommand(std::string* why)
{
    Referee& r = R();
    if (!InMultiplayer() || r.playerSide == NoSide)
        return true;
    const CommandSeat c = Seat();
    if (!c.holder.empty() && !c.mine)
    {
        if (why)
            *why = c.holder + " commands " + Name(r.playerSide) + ". Ctrl+V calls a vote to replace them.";
        return false;
    }
    if (!IsMirror())
    {
        Person* me = GWorld->GetRealPlayer();
        for (const auto& [person, until] : r.barred)
            if (person.GetLink() == me && Now() < until)
            {
                if (why)
                    *why = "Your side voted you out of command: wait " + std::to_string(static_cast<int>(until - Now())) + " s.";
                return false;
            }
    }
    return true;
}

bool CallVote(std::string* why)
{
    Referee& r = R();
    const CommandSeat c = Seat();
    const auto refuse = [&](const char* text)
    {
        if (why)
            *why = text;
        return false;
    };
    if (!c.multiplayer)
        return refuse("there is only one player: you");
    if (c.holder.empty())
        return refuse("nobody commands your side: Ctrl+M takes command");
    if (c.mine)
        return refuse("you are the commander");
    if (c.vote)
        return refuse("a vote is already running: Ctrl+Y or Ctrl+N");
    r.myVote = 1;
    if (IsMirror())
    {
        GameValue args = NetArray();
        static_cast<GameArrayType&>(args).Add(GameValue(RString("start")));
        Request("vote", args);
    }
    else
        StartVote(r, r.playerSide, GWorld->GetRealPlayer());
    return true;
}

bool CastVote(bool yes)
{
    Referee& r = R();
    const CommandSeat c = Seat();
    if (!c.multiplayer || !c.vote)
        return false;
    r.myVote = yes ? 1 : -1;
    if (IsMirror())
    {
        GameValue args = NetArray();
        static_cast<GameArrayType&>(args).Add(GameValue(RString(yes ? "yes" : "no")));
        Request("vote", args);
    }
    else
        Ballot(r, r.playerSide, GWorld->GetRealPlayer(), yes);
    return true;
}

bool TakeCommandLoss(std::string& notice)
{
    Referee& r = R();
    if (r.commandLoss.empty())
        return false;
    notice = r.commandLoss;
    r.commandLoss.clear();
    return true;
}

const Person* CommanderSoldier(int side)
{
    Referee& r = R();
    if (!r.active || side < 0 || side >= r.war.sideCount)
        return nullptr;
    if (!InMultiplayer())
        return side == r.playerSide && Possessing() ? GWorld->GetRealPlayer() : nullptr;
    if (IsMirror())
        return r.netSeat[side];
    return Now() < r.humanUntil[side] ? r.seat[side].GetLink() : nullptr;
}

bool IsHumanPlayer(const Person* person)
{
    return person && person->IsNetworkPlayer();
}

const char* SquadCommandName(SquadCommand command)
{
    switch (command)
    {
        case SquadCommand::Engage: return "engage";
        case SquadCommand::HoldFire: return "hold fire";
        case SquadCommand::Stop: return "stop";
        case SquadCommand::Board: return "board";
        case SquadCommand::GetOut: return "get out";
        case SquadCommand::Land: return "land";
        case SquadCommand::ToBase: return "back to base";
        case SquadCommand::SupplyRuns: return "supply runs";
        case SquadCommand::Fight: return "back to fighting";
    }
    return "";
}

namespace
{
// What a squad is doing, in words, for the command view ("Supply run to Figari (300)").
std::string Doing(const Referee& r, const GroupRecord& rec)
{
    if (IsMirror())
        return rec.netDoing;
    const auto name = [&](int o)
    { return o >= 0 && o < static_cast<int>(r.war.defs.size()) ? r.war.defs[o].name : std::string("?"); };
    std::string text;
    if (IsSupply(r, rec))
    {
        if (rec.supplyState == 1)
            text = "Supply run to " + name(rec.supplyTarget) + " (" + std::to_string(static_cast<int>(rec.supplyLoad)) + ")";
        else if (rec.supplyState == 2)
            text = "Back to the HQ for supplies";
        else
            text = "At the HQ, waiting for a load";
        if (rec.supplyPin >= 0 && rec.supplyPin != rec.supplyTarget)
            text += ", next to " + name(rec.supplyPin);
        return text;
    }
    if (rec.stalled)
        return "Held up, finding another way";
    switch (rec.task)
    {
        case TaskKind::Attack:
            text = (rec.objective >= 0 && r.war.states[rec.objective].owner == NoSide ? "Taking " : "Attacking ") + name(rec.objective);
            break;
        case TaskKind::Defend: text = "Securing " + name(rec.objective); break;
        case TaskKind::Move: text = "Moving to the ordered point"; break;
        case TaskKind::None:
            text = !rec.engageName.empty() ? "Engaging " + rec.engageName
                   : Now() < rec.lockedUntil ? "Holding position, as ordered"
                                             : "Awaiting orders";
            break;
    }
    if (rec.landing && !rec.parkedHeli)
        text += " (landing to unload)";
    else if (rec.detouring)
        text += " (stuck, backing off to try another way)";
    else if (rec.onFoot && rec.task != TaskKind::None)
        text += " (on foot, its trucks got stuck)";
    return text;
}
} // namespace

std::string SendSupplyTo(int base, std::string* reason)
{
    Referee& r = R();
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return std::string();
    };
    const int side = r.playerSide;
    if (side == NoSide || base < 0 || base >= static_cast<int>(r.war.defs.size()))
        return refuse("no such base");
    if (r.war.states[base].owner != side || r.war.defs[base].kind == ObjectiveKind::HQ)
        return refuse("supplies go to the side's other bases");
    if (IsMirror())
    {
        GameValue args = NetArray();
        static_cast<GameArrayType&>(args).Add(GameValue(static_cast<float>(base)));
        Request("supply", args);
        return "a supply truck";
    }
    // The truck at the HQ first (it leaves at once), else the one nearest the HQ.
    const int hq = HqOf(r, side);
    GroupRecord* best = nullptr;
    float bestScore = 1.0e9f;
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !IsSupply(r, rec) || !grp || !grp->Leader())
            continue;
        const Vector3 p = grp->Leader()->Position();
        float score = hq >= 0 ? std::hypot(p.X() - r.war.defs[hq].x, p.Z() - r.war.defs[hq].y) : 0.0f;
        if (rec.supplyState == 0)
            score -= 1.0e6f;
        if (score < bestScore)
        {
            bestScore = score;
            best = &rec;
        }
    }
    if (!best)
        return refuse("no supply truck: deploy one at the HQ");
    best->supplyPin = base;
    WarLog("commander: " + Callsign(best->id) + " supply run to " + r.war.defs[base].name);
    Radio(r, side, Callsign(best->id) + ", next load to " + r.war.defs[base].name + ".");
    return Callsign(best->id);
}

std::string EngageAt(const std::vector<int>& ids, float x, float y)
{
    Referee& r = R();
    if (ids.empty() || r.playerSide == NoSide)
        return std::string();
    if (IsMirror())
    {
        GameValue args = NetArray();
        GameArrayType& a = args;
        GameValue list = NetArray();
        for (int id : ids)
            static_cast<GameArrayType&>(list).Add(GameValue(static_cast<float>(id)));
        a.Add(list);
        a.Add(GameValue(x));
        a.Add(GameValue(y));
        Request("target", args);
        return "the target"; // the server picks it
    }
    return EngageFor(r, r.playerSide, ids, x, y);
}

SelectionInfo DescribeSelection(const std::vector<int>& ids)
{
    SelectionInfo info;
    for (const GroupRecord& rec : R().groups)
    {
        AIGroup* grp = rec.group;
        if (!grp || std::find(ids.begin(), ids.end(), rec.id) == ids.end())
            continue;
        info.any = true;
        if (rec.supplyDuty)
            info.onSupply = true;
        bool groundVehicle = false;
        for (int v = 0; v < grp->NVehicles(); v++)
            if (Transport* t = grp->GetVehicle(v); t && !t->IsDammageDestroyed() && !dynamic_cast<Helicopter*>(t))
                groundVehicle = true;
        if (groundVehicle && !IsSupply(R(), rec))
            info.canSupply = true;
        for (int v = 0; v < grp->NVehicles(); v++)
        {
            Transport* t = grp->GetVehicle(v);
            if (!t || t->IsDammageDestroyed())
                continue;
            info.vehicles = true;
            if (dynamic_cast<Helicopter*>(t))
            {
                info.helicopter = true;
                if (t->Position().Y() - (GLandscape ? GLandscape->SurfaceYAboveWater(t->Position().X(), t->Position().Z()) : 0.0f) > 3.0f)
                    info.airborne = true;
            }
        }
        if (!info.vehicles)
            continue;
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetLifeState() == AIUnit::LSAlive)
            {
                if (unit->IsFreeSoldier())
                    info.onFoot = true;
                else
                    info.aboard = true;
            }
    }
    return info;
}

int CommandSquads(const std::vector<int>& ids, SquadCommand command)
{
    Referee& r = R();
    if (ids.empty() || r.playerSide == NoSide)
        return 0;
    if (IsMirror())
    {
        GameValue args = NetArray();
        GameArrayType& a = args;
        GameValue list = NetArray();
        GameArrayType& l = list;
        for (int id : ids)
            l.Add(GameValue(static_cast<float>(id)));
        a.Add(list);
        a.Add(GameValue(static_cast<float>(static_cast<int>(command))));
        Request("squad", args);
        return static_cast<int>(ids.size());
    }
    return CommandSquadsFor(r, r.playerSide, ids, command);
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
        g.doing = Doing(r, rec);
        // Where it is going: the point it was sent to; for a task given without one yet (a purchase, a
        // plan between two ticks) the task's objective; for a player's squad HQ's request. Standing still
        // awaiting orders: nowhere.
        if (rec.task != TaskKind::None && (rec.destination.X() != 0.0f || rec.destination.Z() != 0.0f))
        {
            g.destX = rec.destination.X();
            g.destY = rec.destination.Z();
            g.hasDest = true;
        }
        else if (rec.task != TaskKind::None && rec.objective >= 0 && rec.objective < static_cast<int>(r.war.defs.size()))
        {
            g.destX = r.war.defs[rec.objective].x;
            g.destY = r.war.defs[rec.objective].y;
            g.hasDest = true;
        }
        else if (grp->IsAnyPlayerGroup() && !rec.hqText.empty())
        {
            g.destX = rec.hqX;
            g.destY = rec.hqY;
            g.hasDest = true;
        }
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

bool ArsenalTake(const EntityAI* container, bool weapon, int items, bool charge, std::string* why)
{
    Referee& r = R();
    if (!r.active || !container)
        return true;
    for (size_t o = 0; o < r.posts.size(); o++)
    {
        if (r.posts[o].arsenal.GetLink() != container)
            continue;
        const int side = r.posts[o].arsenalSide;
        if (side < 0 || side >= MaxSides)
            return true;
        const float cost = weapon ? r.config.arsenalWeaponCost : r.config.arsenalMagazineCost * static_cast<float>(std::max(items, 1));
        float& budget = Budget(r, side, static_cast<int>(o)); // the HQ's pool, or this base's own stock
        if (budget < cost)
        {
            if (why)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "Arsenal: not enough supplies here (this costs %.0f, the base has %.0f).",
                              cost, budget);
                *why = text;
            }
            return false;
        }
        if (charge)
        {
            budget -= cost;
            if (IsMirror())
            {
                // The server keeps the supplies: it is told what was taken from which box.
                GameValue args = NetArray();
                GameArrayType& a = args;
                a.Add(GameValueExt(const_cast<Object*>(static_cast<const Object*>(container))));
                a.Add(GameValue(weapon));
                a.Add(GameValue(static_cast<float>(std::max(items, 1))));
                Request("arsenal", args);
            }
        }
        return true;
    }
    return true; // not an arsenal: free, as any crate or body
}

bool IsArsenal(const EntityAI* container)
{
    for (const Post& post : R().posts)
        if (container && post.arsenal.GetLink() == container)
            return true;
    return false;
}

float ArsenalOwnerPool(const EntityAI* container)
{
    Referee& r = R();
    for (const Post& post : r.posts)
        if (container && post.arsenal.GetLink() == container && post.arsenalSide >= 0 && post.arsenalSide < MaxSides)
            return r.war.pool[post.arsenalSide];
    return 0.0f;
}

void SetArsenalOwnerPoolForTest(const Vector3& near, float pool)
{
    Referee& r = R();
    for (const Post& post : r.posts)
        if (EntityAI* box = post.arsenal; box && post.arsenalSide >= 0 && post.arsenalSide < MaxSides &&
                                          box->Position().Distance(near) < 30.0f)
            r.war.pool[post.arsenalSide] = pool;
}

ArsenalPrices GetArsenalPrices()
{
    const Referee& r = R();
    return {r.config.arsenalWeaponCost, r.config.arsenalMagazineCost};
}

std::string ArsenalReport()
{
    // The same test the physical inventory's vicinity pane applies (InventoryModel::ScanVicinity): a
    // VehicleSupply in the landscape's object lists, able to hold cargo or holding some.
    Referee& r = R();
    std::string out;
    if (!r.active || !GLandscape)
        return "no war";
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        EntityAI* box = r.posts[o].arsenal;
        out += r.war.defs[o].name + ": ";
        if (!box)
        {
            out += r.war.states[o].owner == NoSide ? "none (nobody owns it); " : "MISSING; ";
            continue;
        }
        VehicleSupply* supply = dyn_cast<VehicleSupply>(box);
        const EntityAIType* type = supply ? supply->GetType() : nullptr;
        const bool cap = type && (type->GetMaxMagazinesCargo() > 0 || type->_maxWeaponsCargo > 0);
        bool listed = false;
        const int x = toIntFloor(box->Position().X() / LandGrid), z = toIntFloor(box->Position().Z() / LandGrid);
        const ObjectList& list = GLandscape->GetObjects(z, x);
        for (int i = 0; i < list.Size(); i++)
            listed = listed || list[i] == box;
        char line[200];
        std::snprintf(line, sizeof(line), "%s at %.1f %.1f supply=%d cargoCap=%d weapons=%d magazines=%d listed=%d; ",
                      static_cast<const char*>(box->GetType()->GetName()), box->Position().X(), box->Position().Z(),
                      supply ? 1 : 0, cap ? 1 : 0,
                      supply ? supply->GetWeaponCargoSize() : 0, supply ? supply->GetMagazineCargoSize() : 0,
                      listed ? 1 : 0);
        out += line;
    }
    return out;
}

std::string HqReport()
{
    Referee& r = R();
    if (!r.active || !r.started || r.playerSide == NoSide || InMultiplayer())
        return std::string();
    const float now = Now();
    const int side = r.playerSide;
    std::string out = "=== CONQUEST: THE WAR AS YOUR SIDE KNOWS IT (HQ speaks from this) ===\n";
    out += SideReport(r, side, now);
    // The plan as it stands: what the commander will push first, and why.
    SideView view = BuildView(r, side, now);
    view.priority = r.llmPriority[side];
    view.priority.resize(r.war.defs.size(), 0.0f);
    if (r.humanPriority[side] >= 0 && now < r.humanPriorityUntil[side])
        view.priority[r.humanPriority[side]] = 3.0f;
    const auto ranked = RankTargets(view);
    if (!ranked.empty())
    {
        out += "HQ's plan: the main effort is " + r.war.defs[ranked.front().first].name;
        if (ranked.size() > 1)
            out += ", then " + r.war.defs[ranked[1].first].name;
        out += ".";
        if (r.humanPriority[side] >= 0 && now < r.humanPriorityUntil[side])
            out += " (The player asked for " + r.war.defs[r.humanPriority[side]].name + ".)";
        if (!r.llmReason[side].empty())
            out += " Commander's intent: " + r.llmReason[side];
        out += "\n";
    }
    if (const PlayerOrder order = CurrentPlayerOrder(); order.valid)
        out += "The player's own squad is ordered to: " + order.text + ".\n";
    out += "When the player asks HQ to put the weight of the war on an objective, answer as HQ and add the order "
           "{\"to\": \"all\", \"order\": \"priority\", \"value\": \"<objective name>\"}. HQ cannot push an objective "
           "the side may not attack yet (only ones next to a held objective). When the player asks for mortars or "
           "an air strike on an objective, answer as HQ and add {\"to\": \"all\", \"order\": \"barrage\", \"value\": "
           "\"<objective name>\"} (or \"air_strike\"); HQ says whether it fires.\n";
    return out;
}

std::string RequestPriority(const std::string& objective)
{
    Referee& r = R();
    if (!r.active || !r.started || r.playerSide == NoSide)
        return "HQ: there is no war to steer.";
    const int side = r.playerSide;
    int found = -1;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (_stricmp(r.war.defs[o].name.c_str(), objective.c_str()) == 0)
            found = static_cast<int>(o);
    if (found < 0)
        return "HQ: no objective called " + objective + ".";
    const std::string& name = r.war.defs[found].name;
    if (r.war.states[found].owner != side && !CanAttack(r.war, side, found))
        return "HQ: negative, " + name + " is not on our front yet. Take a town next to it first.";
    r.humanPriority[side] = found;
    r.humanPriorityUntil[side] = Now() + 600.0f;
    r.replan[side] = true;
    WarLog(Name(side) + " HQ: the player asked for " + name + " as the main effort");
    return r.war.states[found].owner == side ? "HQ: understood, " + name + " will be held. Reinforcing it."
                                             : "HQ: understood, " + name + " is the main effort for the next ten minutes.";
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

// conquestStats: [started, ended, sides, squads, [soldiers alive per side], [objectives held per side]] -- the
// war in numbers for scripted tests (tests/integration/conquest).
static GameValue ConquestStatsNular(const GameState* state)
{
    const Poseidon::Conquest::Snapshot& snap = Poseidon::Conquest::GetSnapshot();
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& a = result;
    a.Add(GameValue(snap.active && Poseidon::Conquest::PlayerSide() != Poseidon::Conquest::NoSide && !Poseidon::Conquest::SetupPending()));
    a.Add(GameValue(snap.winner != Poseidon::Conquest::NoSide));
    a.Add(GameValue(static_cast<float>(snap.war.sideCount)));
    a.Add(GameValue(static_cast<float>(snap.groups.size())));
    GameValue live = state->CreateGameValue(GameArray);
    GameValue owned = state->CreateGameValue(GameArray);
    for (int s = 0; s < snap.war.sideCount; s++)
    {
        static_cast<GameArrayType&>(live).Add(GameValue(static_cast<float>(snap.live[s])));
        static_cast<GameArrayType&>(owned).Add(GameValue(static_cast<float>(Poseidon::Conquest::OwnedCount(snap.war, s))));
    }
    a.Add(live);
    a.Add(owned);
    return result;
}

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

// conquestStart ["EAST", "soldier"|"commander", "contacts"|"shroud", "noview"|"", ["WEST"]]: the setup screen's Start, from a
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
    // [..., ["EAST"]]: the sides whose commander asks an LLM (Hybrid).
    if (a.Size() > 4 && a[4].GetType() == GameArray)
    {
        const GameArrayType& llm = a[4];
        for (int i = 0; i < llm.Size(); i++)
            if (llm[i].GetType() == GameString)
                for (size_t s = 0; s < sides.size(); s++)
                    if (_stricmp(sides[s].c_str(), static_cast<const char*>(static_cast<GameStringType>(llm[i]))) == 0)
                        options.llmCommander[s] = true;
    }
    // [..., resistance (bool), villages to win, [supplies per side]]
    if (a.Size() > 5 && a[5].GetType() == GameBool)
        options.resistance = static_cast<bool>(a[5]);
    if (a.Size() > 6 && a[6].GetType() == GameScalar)
        options.victoryTowns = std::max(0, static_cast<int>(static_cast<float>(a[6])));
    if (a.Size() > 7 && a[7].GetType() == GameArray)
    {
        const GameArrayType& pools = a[7];
        for (int i = 0; i < pools.Size() && i < Poseidon::Conquest::MaxSides; i++)
            if (pools[i].GetType() == GameScalar)
                options.startPool[i] = std::max(0.0f, static_cast<float>(pools[i]));
    }
    // [..., time limit (minutes), the Resistance with WEST (bool), support delay (seconds; tests: 0)]
    if (a.Size() > 8 && a[8].GetType() == GameScalar)
        options.timeLimit = std::max(0, static_cast<int>(static_cast<float>(a[8])));
    if (a.Size() > 9 && a[9].GetType() == GameBool)
        options.resistanceAlly = static_cast<bool>(a[9]);
    if (a.Size() > 10 && a[10].GetType() == GameScalar)
        Poseidon::Conquest::SetSupportDelay(static_cast<float>(a[10]));
    return GameValue(Poseidon::Conquest::BeginWar(options));
}

// conquestSquad [[ids], "engage"|"holdfire"|"stop"|"board"|"getout"|"land"|"tobase"]: the command bar's
// contextual orders. Returns how many squads took it.
static GameValue ConquestSquadUnary(const GameState*, GameValuePar oper1)
{
    using Poseidon::Conquest::SquadCommand;
    const GameArrayType& a = oper1;
    if (a.Size() < 2 || a[0].GetType() != GameArray || a[1].GetType() != GameString)
        return GameValue(0.0f);
    std::vector<int> ids;
    const GameArrayType& list = a[0];
    for (int i = 0; i < list.Size(); i++)
        if (list[i].GetType() == GameScalar)
            ids.push_back(static_cast<int>(static_cast<float>(list[i])));
    const std::string what = static_cast<const char*>(static_cast<GameStringType>(a[1]));
    const std::pair<const char*, SquadCommand> names[] = {
        {"engage", SquadCommand::Engage}, {"holdfire", SquadCommand::HoldFire}, {"stop", SquadCommand::Stop},
        {"board", SquadCommand::Board},   {"getout", SquadCommand::GetOut},     {"land", SquadCommand::Land},
        {"tobase", SquadCommand::ToBase}};
    for (const auto& [name, command] : names)
        if (_stricmp(what.c_str(), name) == 0)
            return GameValue(static_cast<float>(Poseidon::Conquest::CommandSquads(ids, command)));
    return GameValue(0.0f);
}

// conquestEngage [[ids], x, y]: the command bar's Engage and a click on a target. Returns what was
// targeted ("" when no building or vehicle is within 30 m of the point).
static GameValue ConquestEngageUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 3 || a[0].GetType() != GameArray)
        return GameValue(RString());
    std::vector<int> ids;
    const GameArrayType& list = a[0];
    for (int i = 0; i < list.Size(); i++)
        if (list[i].GetType() == GameScalar)
            ids.push_back(static_cast<int>(static_cast<float>(list[i])));
    return GameValue(RString(Poseidon::Conquest::EngageAt(ids, static_cast<float>(a[1]), static_cast<float>(a[2])).c_str()));
}

// conquestAirStrike [x, y]: the player's side calls its plane on that point. true, or why not.
static GameValue ConquestAirStrikeUnary(const GameState*, GameValuePar oper1)
{
    const GameArrayType& a = oper1;
    if (a.Size() < 2)
        return GameValue(RString("conquestAirStrike [x, y]"));
    std::string why;
    if (Poseidon::Conquest::OrderAirStrike(static_cast<float>(a[0]), static_cast<float>(a[1]), &why))
        return GameValue(true);
    return GameValue(RString(why.c_str()));
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

// conquestTestArsenalTake ["weapon"|"magazine", supplies]: the test of the arsenal's price. Runs the
// physical inventory's own transfer (InventoryModel::Transfer, what a drag does) for the first item of
// that kind the player can take from an arsenal in reach, after setting the box owner's supplies when
// `supplies` is given. "result=<InvTransferResult> pool=<before>-><after>", or why nothing was tried.
static GameValue ConquestTestArsenalTakeUnary(const GameState*, GameValuePar oper1)
{
    using namespace Poseidon;
    const GameArrayType& a = oper1;
    if (a.Size() < 1 || a[0].GetType() != GameString)
        return GameValue(RString("usage: conquestTestArsenalTake [\"weapon\"|\"magazine\", supplies]"));
    const bool weapon = _stricmp(static_cast<const char*>(static_cast<GameStringType>(a[0])), "weapon") == 0;
    Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (!player)
        return GameValue(RString("no player"));
    if (a.Size() > 1 && a[1].GetType() == GameScalar)
        Conquest::SetArsenalOwnerPoolForTest(player->Position(), static_cast<float>(a[1]));
    InventoryModel model(player);
    model.Refresh();
    std::string last = "no arsenal item of that kind in reach";
    for (int i = 0; i < model.Vicinity().Size(); i++)
    {
        const InvCell& cell = model.Vicinity()[i];
        if (!cell.container || cell.fromBody || (cell.kind == InvItemKind::Weapon) != weapon ||
            !Conquest::IsArsenal(cell.container))
            continue;
        const float before = Conquest::ArsenalOwnerPool(cell.container);
        const InvTransferResult result = model.Transfer(cell, InvPane::Grid);
        char line[96];
        std::snprintf(line, sizeof(line), "result=%d pool=%.0f->%.0f", static_cast<int>(result), before,
                      Conquest::ArsenalOwnerPool(cell.container));
        last = line;
        if (result == InvTransferResult::Ok || result == InvTransferResult::Unavailable)
            break; // taken, or refused (the price): either way the answer
    }
    return GameValue(RString(last.c_str()));
}

// conquestArsenals: each post's arsenal box as the inventory sees it (class, supply, cargo, listed).
static GameValue ConquestArsenalsNular(const GameState*)
{
    return GameValue(RString(Poseidon::Conquest::ArsenalReport().c_str()));
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
    GGameState.NewNularOp(GameNular(GameString, "conquestArsenals", ConquestArsenalsNular));
    GGameState.NewFunction(GameFunction(GameString, "conquestTestArsenalTake", ConquestTestArsenalTakeUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameBool, "conquestStart", ConquestStartUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameAny, "conquestBarrage", ConquestBarrageUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameAny, "conquestAirStrike", ConquestAirStrikeUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameString, "conquestEngage", ConquestEngageUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameScalar, "conquestSquad", ConquestSquadUnary, GameArray));
    GGameState.NewFunction(GameFunction(GameAny, "conquestTakeControl", ConquestTakeControlUnary, GameScalar));
    GGameState.NewFunction(GameFunction(GameAny, "conquestBuy", ConquestBuyUnary, GameArray));
    GGameState.NewNularOp(GameNular(GameString, "conquestState", ConquestStateNular));
    GGameState.NewNularOp(GameNular(GameArray, "conquestStats", ConquestStatsNular));
    GGameState.NewFunction(GameFunction(GameBool, "conquestMapClick", ConquestMapClickUnary, GameArray));
};
