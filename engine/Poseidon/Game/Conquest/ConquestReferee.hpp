// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conquest mode: the referee, the only part that touches the world.
//
// Once a second it measures who stands where (ground truth: that is the referee's job), feeds
// the rules in ConquestRules, builds each side's SideView from that side's AICenter knowledge
// only, runs its commander, and executes the intents through the engine's own group, unit and
// move paths. It also counts losses, cleans up old bodies and keeps the map markers current.
//
// Inert unless the mission's description.ext declares `class CfgConquest` (see
// content/default-packs/conquest/missions/conquest-everon.Eden). Single player only for now; a saved game
// carries the war (Serialize).

#include <Poseidon/Game/Conquest/ConquestCommander.hpp>
#include <Poseidon/Game/Conquest/ConquestRules.hpp>
#include <Poseidon/IO/Serialization/SerializeClass.hpp>

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class ParamArchive;
class GameValue;

namespace Poseidon
{
class AIGroup;
class Person;
}

namespace Poseidon::Conquest
{
struct GroupSummary
{
    int id = 0;
    int side = NoSide;
    int men = 0;
    TaskKind task = TaskKind::None;
    int objective = -1;
    bool stalled = false;
    float x = 0.0f;
    float y = 0.0f;
};

/// What the referee shows the outside (dev tab, conquestState): a copy refreshed every tick.
struct Snapshot
{
    bool active = false;
    float now = 0.0f;
    War war;
    Tuning tuning;
    std::vector<std::string> sideNames;
    std::array<int, MaxSides> live{};
    std::array<int, MaxSides> losses{};
    std::array<int, MaxSides> bought{};
    int cap = 0;
    int winner = NoSide;
    std::vector<GroupSummary> groups;
    std::deque<std::string> log; // newest last
    float lastTickMs = 0.0f;
    float maxTickMs = 0.0f;
};

/// Once per simulation step (World::SimulateScripts); does real work once a second.
void Simulate();

const Snapshot& GetSnapshot();

/// The war in a saved game (World::Serialize, after the groups and vehicles it refers to). A load
/// keeps what it reads until the next Simulate, which reads the mission's CfgConquest again and
/// continues the saved war instead of starting a new one. A save without a war loads as none.
LSError Serialize(ParamArchive& ar);

/// The player's map click as their side's commander (x east, y north). A click near one of the
/// side's squads selects it (shift adds); otherwise the selected squads get the order: attack or
/// defend the objective clicked, or move to the point. Returns true when the click was used.
bool MapClick(float x, float y, bool shift, bool alt);

/// One line per objective plus pools and scores (the conquestState script command, the log).
std::string StatusText();

// ---- the commander view's interface (game thread only) ----

/// The side the player commands, NoSide when no war runs.
int PlayerSide();
/// The player holds the commander view: the AI stops buying for the player's side (the squads
/// the player has not ordered still follow the AI plan).
void SetHumanCommander(bool on);

struct LiveGroup
{
    int id = 0;
    int side = NoSide;
    AIGroup* group = nullptr;
    TaskKind task = TaskKind::None;
    int objective = -1;
    bool locked = false; // under the human commander's orders
    bool stalled = false;
    float destX = 0.0f, destY = 0.0f; // where it was last sent
    std::string typeName;             // "Motorised squad"
    std::string role;                 // foot, motorised, mechanised, air
    int size = 0;                     // soldiers at full strength
    bool playerLed = false;           // the player is inside one of its soldiers
};
/// Every Conquest squad, live (positions read through `group`).
void ForEachGroup(const std::function<void(const LiveGroup&)>& fn);

/// The human commander's order for the player's squads `ids`: attack or defend the objective
/// at (x, y), or move there. Returns how many squads took it.
int OrderGroups(const std::vector<int>& ids, float x, float y);

struct SquadInfo
{
    std::string name;
    float cost = 0.0f;
    int size = 0;
};
/// What the player's side can buy, index = squad type.
std::vector<SquadInfo> SquadTypes();
/// A squad type by its config class name or display name, -1 if none.
int SquadTypeIndex(const std::string& id);
/// May the player buy squad `type` at objective `base` now? `reason` says why not.
bool CanBuy(int base, int type, std::string* reason = nullptr);
/// Buy it: the squad spawns at the base and waits there for orders. Returns its id, 0 if refused.
int BuySquad(int base, int type, std::string* reason = nullptr);

struct Contact
{
    float x = 0.0f, y = 0.0f, height = 0.0f;
    float age = 0.0f; // seconds since last seen
    bool vehicle = false;
};
/// What `side` knows of the enemy (its AICenter target database): fog of war for the view.
std::vector<Contact> KnownEnemies(int side);

// ---- taking control of soldiers (optional: CfgConquest possession = 0 turns it off) ----

bool PossessionAllowed();
void SetPossessionAllowed(bool allowed);
/// The player is inside one of their side's soldiers rather than their own body.
bool Possessing();
/// Become the leader of squad `groupId`; the player's own body waits. False (with a reason) if not.
bool Possess(int groupId, std::string* reason = nullptr);
/// Back to the player's own body; the soldier returns to the AI.
bool ReturnToCommander();
/// Become this soldier of the player's side (any soldier, not only a squad leader).
bool PossessUnit(Person* person, std::string* reason = nullptr);
/// The player's soldier died (Man::Killed in single player, before the game's death camera, and
/// the World's end check). True when Conquest carried the player on: back to command, or into
/// another soldier of their side. False only when the side has nobody left: the mission ends.
bool OnRealPlayerDead();
/// True once when the commander view should open (the war began with the player in command, or
/// the soldier they controlled died); `notice` says why, for the view's toast.
bool TakeViewRequest(std::string& notice);
/// The latest thing the soldier's HUD should say once ("You were killed. You now lead Bravo.").
bool TakeSoldierNotice(std::string& notice);

/// "Alpha", "Bravo" ... "Alpha 2": a squad's callsign by its id.
std::string Callsign(int id);

// ---- the war's options and the setup screen ----

enum class FogMode
{
    Contacts, // enemies only as the side knows them (the default)
    Shroud,   // also: ground no friendly has seen is black (classic real-time strategy)
};

struct Options
{
    int side = 0;                  // index into the war's sides
    bool startInCommand = false;   // false: lead a squad as a soldier
    bool commanderView = true;     // the top-down command view (Ctrl+M) is available
    FogMode fog = FogMode::Contacts;
    bool possession = true;        // take control of any friendly soldier
    bool hqRadio = true;           // the AI commander radios its orders while the player does not command
    std::array<bool, MaxSides> llmCommander{}; // the side's commander asks an LLM for its priorities (Hybrid)
};

/// The mission opened with CfgConquest setup = 1 and the player has not chosen yet.
bool SetupPending();
/// The war's sides as the setup screen names them ("WEST", "EAST").
std::vector<std::string> SideNames();
/// Begin the war with these choices (the setup screen's Start, or conquestStart from a script).
bool BeginWar(const Options& options);
Options GetOptions();
/// Change the options during the war (the side and start are fixed by then).
void SetOptions(const Options& options);

/// The shroud: has a soldier of the player's side been near (x, y)?
bool Explored(float x, float y);

// ---- support ----

struct BarrageInfo
{
    float cost = 0.0f;
    int shells = 0;
    float cooldownLeft = 0.0f; // seconds of game time until the mortars are ready again
};
BarrageInfo Barrage();
/// A mortar barrage on (x, y) for the player's side. False with a reason when it cannot be fired.
bool OrderBarrage(float x, float y, std::string* reason = nullptr);

/// What HQ last ordered the player's own squad (the soldier's HUD shows it while it stands).
struct PlayerOrder
{
    bool valid = false;
    std::string text; // "Take the command post at Figari"
    float x = 0.0f, y = 0.0f;
};
PlayerOrder CurrentPlayerOrder();

/// Multiplayer: a client's commander action ([player, kind, args]), run on the server through
/// `remoteExec ["conquestRequest", 2]`.
void HandleNetRequest(const GameValue& value);

// ---- talking to HQ (NPC chat) ----

/// The war as the player's side knows it, with HQ's plan, for NPC chat's situation report. Empty when
/// no single-player war runs.
std::string HqReport();
/// The player asked HQ (by voice or text) to put the weight of the war on `objective`. HQ's answer,
/// accepted or refused with the reason.
std::string RequestPriority(const std::string& objective);

/// Each post's arsenal box as the physical inventory sees it (the conquestArsenals check command).
std::string ArsenalReport();

} // namespace Poseidon::Conquest
