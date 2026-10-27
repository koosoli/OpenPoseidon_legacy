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
class EntityAI;
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

/// One order in production at a base: the deploy panel draws its clock.
struct BuildInfo
{
    int base = -1;
    int type = -1;          // a squad type, or a structure (StructureTypes) when `structure`
    bool structure = false;
    bool active = false;    // the first in its base's queue: its clock runs
    float progress = 0.0f;  // 0..1, paid as it goes
    bool paused = false;    // short of supplies or men, or ready and the base not quiet
    bool placing = false;   // a structure built: the commander clicks where it stands
    std::string why;        // what it waits for
};
/// A structure standing at a base (barracks tent, motor pool, helipad).
struct StructureInfo
{
    int base = -1;
    int kind = -1;
    float x = 0.0f, y = 0.0f;
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
    std::string winReason; // "holds 9 villages", "the war score"
    int queuedPurchases = 0; // the player's side: orders in production at its bases
    std::vector<BuildInfo> builds;          // ... those orders, every base's queue in order
    std::vector<StructureInfo> structures;  // what stands at the bases, every side's
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
    float destX = 0.0f, destY = 0.0f; // where it is going
    bool hasDest = false;              // ... known: it has somewhere to go (not standing, awaiting orders)
    std::string typeName;             // "Motorised squad"
    std::string role;                 // foot, motorised, mechanised, air
    int size = 0;                     // soldiers at full strength
    bool playerLed = false;           // the player is inside one of its soldiers
    std::string doing;                // what it is doing, in words: "Supply run to Figari (300)", "Taking Tyrone"
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
/// The commander's contextual orders for selected squads (the command bar's buttons).
enum class SquadCommand
{
    Engage,   // fire at will, combat stance
    HoldFire, // fire only when fired upon
    Stop,     // stand where they are, awaiting orders
    Board,    // get into the squad's own vehicles
    GetOut,   // out of the vehicles (a helicopter lands for its passengers; crews of tanks stay)
    Land,     // a helicopter sets down and waits
    ToBase,   // back to the nearest objective the side holds
    SupplyRuns, // a squad with a truck of its own carries supplies from the HQ to the bases
    Fight,      // ... and back to fighting
};
/// What the selection can be told: the bar shows only the commands that apply.
struct SelectionInfo
{
    bool any = false;
    bool vehicles = false;  // a squad with vehicles of its own
    bool aboard = false;    // someone sits in one as a passenger or driver who could get out
    bool onFoot = false;    // someone with a vehicle to board is on foot
    bool helicopter = false;
    bool airborne = false;  // a helicopter of theirs is in the air
    bool canSupply = false; // a fighting squad with a truck of its own: it could run supplies
    bool onSupply = false;  // a squad on supply duty (not a supply truck squad: those always are)
};
/// Send a supply truck of the player's side to base `base` (the one at the HQ first, else the nearest).
/// Returns its callsign, or "" with the reason.
std::string SendSupplyTo(int base, std::string* reason = nullptr);
SelectionInfo DescribeSelection(const std::vector<int>& ids);
/// Give the selected squads that order; returns how many took it.
int CommandSquads(const std::vector<int>& ids, SquadCommand command);
const char* SquadCommandName(SquadCommand command);
/// Engage a target: the building or vehicle nearest (x, y) (within 30 m) becomes the selected squads'
/// target, and they attack it (a gunship flies in on it). Returns what was targeted, "" when nothing is there.
std::string EngageAt(const std::vector<int>& ids, float x, float y);

/// What the player's side can buy, index = squad type.
std::vector<SquadInfo> SquadTypes();
/// A squad type by its config class name or display name, -1 if none.
int SquadTypeIndex(const std::string& id);
/// May the player buy squad `type` at objective `base` now? `reason` says why not.
bool CanBuy(int base, int type, std::string* reason = nullptr);
/// Buy it: the squad spawns at the base and waits there for orders. Returns its id, 0 if refused.
int BuySquad(int base, int type, std::string* reason = nullptr);

// ---- production (ConquestRefereeBuild.inc): squads and structures are built at a base, one at a time,
// paid as their clock runs; short of supplies the clock waits ----
struct StructureType
{
    std::string name;
    float cost = 0.0f;
    float seconds = 0.0f;
    std::string unlocks; // what it lets the base build
};
/// The structures a base can build: barracks tent, motor pool, helipad (indices are BuildInfo::type).
std::vector<StructureType> StructureTypes();
/// Seconds squad type `type` takes to build.
float SquadBuildSeconds(int type);
/// "" when base `base` has what squad type `type` needs, else what it lacks ("a Barracks tent").
std::string SquadMissing(int base, int type);
/// Queue a squad (or a structure) at `base` for the player's side: "" when queued, else why not.
std::string QueueAt(int base, int type, bool structure);
/// Cancel the last queued one of these at `base`; what it paid goes back to the base.
bool CancelAt(int base, int type, bool structure);
/// Put a built structure up at (x, y) inside its base: "" when it stands, else why not.
std::string PlaceAt(int base, int kind, float x, float y);

struct Contact
{
    float x = 0.0f, y = 0.0f, height = 0.0f; // height: above sea level (the view draws at it)
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
    bool resistance = false;       // a war with a third side: it takes part (off: its base is a neutral village)
    std::array<float, MaxSides> startPool{2000.0f, 2000.0f, 2000.0f}; // each side's supplies when the war begins
    int victoryTowns = 0;          // a side holding this many villages (HQs not counted) wins; 0: off
    int timeLimit = 0;             // minutes of war; then the higher war score wins. 0: off
    bool resistanceAlly = false;   // the Resistance fights with WEST (with resistance on)
    bool enemyMarkers = false;     // the enemy the side knows of, as symbols in the command view and its minimap
    int aiSkill = 1;               // the AI soldiers: 0 recruits, 1 regulars, 2 veterans
    int weather = 0;               // 0 the mission's, 1 clear, 2 cloudy, 3 overcast, 4 rain, 5 fog, 6 changing slowly
    std::array<std::string, MaxSides> llmModel; // an LLM commander's own model ("": the AI & voice panel's)
};

/// The player asks HQ (in the conversation) for mortars or the attack plane on an objective by name: HQ's
/// answer, fired or refused with the reason.
std::string RequestSupport(const std::string& objective, bool air);
/// No mortars or air strikes this long into the war (CfgConquest supportDelay; conquestStart may set it).
void SetSupportDelay(float seconds);
/// Seconds of war left under the time limit, or -1 when there is none.
float TimeLeft();

// ---- who commands (multiplayer: one human commander per side, removable by a vote) ----

struct CommandSeat
{
    bool multiplayer = false;
    std::string holder;      // the player commanding the player's side, "" when the AI does
    bool mine = false;       // ... it is this player
    bool vote = false;       // a vote to remove the commander is running
    float voteLeft = 0.0f;   // seconds
    int yes = 0, no = 0, electorate = 0;
    std::string voteTarget, voteCaller;
    int myVote = 0;          // this player's vote: 1 yes, -1 no, 0 not yet
};
CommandSeat Seat();
/// May the player take command of their side now? False, with why, while another player holds it.
bool CanTakeCommand(std::string* why = nullptr);
/// Ask the side to vote on removing its commander (multiplayer).
bool CallVote(std::string* why = nullptr);
/// This player's vote in the running vote.
bool CastVote(bool yes);
/// True once when the player lost command (voted out, or another player holds it): the view closes.
bool TakeCommandLoss(std::string& notice);
/// The soldier the side's human commander is (multiplayer: the seat holder's own; single player: the
/// one the commander took over), for the discreet mark teammates see over him. nullptr when none.
const Person* CommanderSoldier(int side);
/// A human player (this one, or one across the network): the commander's orders only ask them.
bool IsHumanPlayer(const Person* person);

/// The side the setup screen may leave out (the Resistance), or NoSide.
int OptionalSide();
/// The most villages a victory condition can ask for: every objective but the HQs of the sides
/// taking part (with `resistance` off, the Resistance's base is one more village).
int VictoryTownsMax(bool resistance);
/// After the war: the setup screen again, on a cleared island (single player).
bool NewWar();
/// After the war: the mission ends (back to the menu).
void EndMission();

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
    float cooldown = 0.0f;     // ... out of this many (the reload, or the war's first minutes): the icon's clock
};
BarrageInfo Barrage();
/// A mortar barrage on (x, y) for the player's side. False with a reason when it cannot be fired.
bool OrderBarrage(float x, float y, std::string* reason = nullptr);

/// Air support: the side's attack plane flies in and bombs one point, far more precisely than the
/// mortars (CfgConquest airStrike*).
struct AirStrikeInfo
{
    float cost = 0.0f;
    int bombs = 0;
    float spread = 0.0f;
    float cooldownLeft = 0.0f;
    float cooldown = 0.0f; // the whole wait cooldownLeft counts down: the icon's clock
    bool inbound = false; // a plane is on its way
};
AirStrikeInfo AirStrike();
bool OrderAirStrike(float x, float y, std::string* reason = nullptr);

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

/// The physical inventory is about to take `items` (a weapon, or that many magazines) out of
/// `container`. Free unless it is one of Conquest's arsenal boxes; then the box owner's supplies pay,
/// when `charge` (a check only otherwise). False, with the reason, when the side cannot pay.
bool ArsenalTake(const EntityAI* container, bool weapon, int items, bool charge, std::string* why = nullptr);
struct ArsenalPrices
{
    float weapon = 0.0f;
    float magazine = 0.0f;
};
ArsenalPrices GetArsenalPrices();
/// Is `container` one of the arsenal boxes; the supplies of the side that owns it (tests, the HUD).
bool IsArsenal(const EntityAI* container);
float ArsenalOwnerPool(const EntityAI* container);
/// Test hook (conquestTestArsenalTake): the supplies of the side whose box stands within 30 m of `near`.
void SetArsenalOwnerPoolForTest(const Vector3& near, float pool);

} // namespace Poseidon::Conquest
