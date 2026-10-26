// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conquest mode: the rules of the war, as pure functions over plain data.
//
// Nothing here touches the world. The referee (ConquestReferee.cpp) measures who stands where and
// feeds that in as a Presence; these functions decide what it means: capture progress, income,
// whether a base may spawn, the war score, who has lost. Keeping them world-free is what lets the
// unit tests (tests/unit/engine/Poseidon/Game/Conquest) pin every rule without booting a mission.
//
// Sides are small indices into the war's side list (0..MaxSides-1), not engine TargetSides; -1 is
// "nobody". See docs/roadmap/conquest/CONQUEST-PLAN.md section 3 for why each rule is what it is.

#include <array>
#include <string>
#include <vector>

namespace Poseidon::Conquest
{
constexpr int MaxSides = 3;
// Capture speeds up with every soldier in the zone (owner, 2026-10-26) up to 24 of them: more
// than that changes nothing, so no blob takes a town instantly.
constexpr float MaxCaptureFactor = 6.0f;
constexpr int NoSide = -1;

enum class ObjectiveKind
{
    HQ,
    Town,
    Airfield,
    Depot
};

struct ObjectiveDef
{
    std::string name;
    float x = 0.0f; // map metres, east
    float y = 0.0f; // map metres, north
    float radius = 120.0f;        // the town: spawning, threat, the map's area ring
    float captureRadius = 30.0f;  // round the command post at (x, y): only soldiers here take it
    float value = 5.0f; // income per minute while owned
    ObjectiveKind kind = ObjectiveKind::Town;
    int initialOwner = NoSide;
    std::vector<int> neighbours; // indices into the war's objective list
};

// The capture bar. `holder` is the side the bar belongs to and `level` how full it is (0..1).
// An objective is OWNED by `owner`; its bar normally reads holder == owner, level == 1. Enemies
// first drain it to zero (the objective is neutralised: owner = NoSide), then fill it for
// themselves (owner changes when it reaches 1).
struct ObjectiveState
{
    int owner = NoSide;
    int holder = NoSide;
    float level = 0.0f;
    bool contested = false;
    float capturedAt = -1.0e9f; // game time of the last change of owner
    float lastHitAt = -1.0e9f;  // game time a friendly of the owner was last hit inside the security radius
    std::array<int, MaxSides> enemiesNear{}; // per side: enemy men of that side within the security radius
};

struct Tuning
{
    float baseCaptureSeconds = 90.0f; // one full bar for a four-man strength
    float decayFactor = 0.25f;        // abandoned progress returns at this fraction of the base rate
    float dominanceRatio = 1.0f;      // a side progresses with more than this multiple of every other side
                                      // (1: a plain majority; the owner, 2026-10-26: "the one with more soldiers captures")
    float settleSeconds = 60.0f;      // no spawning at a freshly captured objective for this long
    float hitMemorySeconds = 60.0f;   // no spawning this long after a friendly was hit there
    float securityRadius = 400.0f;    // enemies inside this block spawning (see ObjectiveState::enemiesNear)
    float startScore = 500.0f;
    float scoreDrainPerObjective = 5.0f; // per minute, per objective of difference (HQs not counted)
    float scorePerManLost = 1.0f;
};

// Strength of each side inside one objective's capture radius, measured by the referee.
struct Presence
{
    std::array<float, MaxSides> strength{};
};

enum class CaptureEvent
{
    None,
    Neutralised, // owner lost it; nobody owns it now
    Captured     // a new owner
};

struct CaptureResult
{
    CaptureEvent event = CaptureEvent::None;
    int side = NoSide; // who neutralised or captured
};

struct War
{
    std::vector<ObjectiveDef> defs;
    std::vector<ObjectiveState> states;
    int sideCount = 2;
    std::array<float, MaxSides> pool{};
    std::array<float, MaxSides> score{};
    std::array<bool, MaxSides> defeated{};
};

/// Starting state: initial owners hold full bars, scores at Tuning::startScore, pools at `startPool`.
void StartWar(War& war, const Tuning& tuning, float startPool);

/// Number of objectives `side` owns (optionally leaving HQs out).
int OwnedCount(const War& war, int side, bool countHQs = true);

/// The front-line rule: `side` may attack objective `index` only if it owns one of its neighbours.
/// An HQ is also protected while its owner still holds any other objective.
bool CanAttack(const War& war, int side, int index);

/// Seconds for a full bar at this strength: baseCaptureSeconds / clamp(strength / 4, 0.5, 6),
/// i.e. 45 s for a squad of eight, 22.5 s for two squads, 15 s from three squads on.
float CaptureSeconds(float strength, const Tuning& tuning);

/// What capturing runs on: `side`'s strength less the strongest other side present (8 against 3
/// takes it at the speed of 5 alone).
float CaptureMargin(const Presence& presence, int sideCount, int side);

/// The side whose strength is more than dominanceRatio times every other side present (with the
/// default 1, simply the side with more soldiers there), or NoSide
/// (nobody present, or contested). `contested` reports two or more sides present without one dominating.
int DominantSide(const Presence& presence, int sideCount, const Tuning& tuning, bool& contested);

/// One step of the capture bar of objective `index` (dt seconds of game time, `now` = game time).
CaptureResult StepCapture(War& war, int index, const Presence& presence, float dt, float now,
                          const Tuning& tuning);

/// Income per minute of `side`: the values of everything it owns.
float IncomePerMinute(const War& war, int side);

/// May `side` spawn at objective `index` now? `reason` says why not.
bool SpawnEligible(const War& war, int side, int index, float now, const Tuning& tuning, std::string* reason = nullptr);

/// Income and score drain for dt seconds; marks defeated sides. Returns the winner once only one
/// side is left undefeated, else NoSide.
int StepEconomy(War& war, float dt, const Tuning& tuning);

/// The victory condition the setup screen sets: the side holding at least `towns` objectives other
/// than HQs has won (towns <= 0: off). Several at once cannot happen while towns is over half of them;
/// below that, the one holding most wins.
int TownVictor(const War& war, int towns);

/// Objectives other than HQs: the most villages a victory condition can ask for.
int TownCount(const War& war);

/// Takes the war's last side out before it begins (the Resistance, when the setup leaves it off):
/// what it would have held, its HQ included, becomes a neutral town. Restarts the war.
void DropLastSide(War& war, const Tuning& tuning, float startPool);

/// A named place of an island (CfgWorlds >> world >> Names): what a war is generated from.
struct Place
{
    std::string name;
    float x = 0.0f, y = 0.0f;
    int rank = 0; // 0 village, 1 town, 2 capital
};

/// A war's objectives for an island nobody wrote one for: every town, the HQs of the `sides` sides
/// as far apart as the island allows (the third as far from both as it can be), each town linked to
/// its nearest (up to three, within `link` metres) and the whole map kept connected. Places closer
/// than 200 m to a larger one are merged into it.
std::vector<ObjectiveDef> AutoObjectives(const std::vector<Place>& places, int sides, float link = 2500.0f);

/// A soldier of `side` died (war score).
void RecordLoss(War& war, int side, const Tuning& tuning);

const char* KindName(ObjectiveKind kind);
bool KindFromName(const std::string& name, ObjectiveKind& kind);
} // namespace Poseidon::Conquest
