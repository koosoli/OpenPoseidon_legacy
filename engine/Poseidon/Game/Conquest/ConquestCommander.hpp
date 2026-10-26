// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conquest mode: the Standard commander, a pure planner.
//
// It sees only a SideView: the objectives' owners (public knowledge, like the map in Reforger),
// what ITS side knows about the enemy near each objective (built by the referee from the side's
// AICenter target database, never from the world's unit list), its own groups, its pool and its
// cap. It answers with intents; the referee validates and executes them. Because it is pure, the
// unit tests can put it in any situation and check what it decides.

#include <Poseidon/Game/Conquest/ConquestRules.hpp>

#include <string>
#include <vector>

namespace Poseidon::Conquest
{
enum class TaskKind
{
    None,
    Attack,
    Defend,
    Move // a human commander's order to a point (no objective)
};

const char* TaskName(TaskKind task);

struct GroupView
{
    int id = 0;
    float x = 0.0f;
    float y = 0.0f;
    int men = 0;
    TaskKind task = TaskKind::None;
    int objective = -1;
    bool stalled = false; // the referee saw no progress toward the task for too long
    bool locked = false;  // ordered by a human commander: the planner counts it but never re-tasks it
    bool crew = false;     // a tank's or gunship's crew: a full vehicle in a few men, never "worn out"
    bool airborne = false; // a helicopter squad still in the air (a gunship, or air assault not yet set down):
                           // it attacks or carries, never holds a place (owner, 2026-10-26)
};

struct SideView
{
    int side = NoSide;
    float now = 0.0f;
    const War* war = nullptr;
    std::vector<float> knownEnemy; // per objective: estimated enemy strength this side knows of nearby
    std::vector<bool> canSpawn;    // per objective: SpawnEligible for this side
    std::vector<GroupView> groups;
    float pool = 0.0f;
    int live = 0; // living soldiers of this side
    int cap = 0;
    float squadCost = 80.0f;
    int squadSize = 8;
    // Per objective, a steer from above: the LLM commander's priorities or the player's request to HQ
    // (-2 leave it alone .. 3 the main effort). Empty or 0: the Standard commander's own judgement.
    std::vector<float> priority;
};

struct Intent
{
    enum class Kind
    {
        Assign,
        BuySquad
    };
    Kind kind = Kind::Assign;
    int group = 0;      // Assign
    int objective = -1; // Assign: target; BuySquad: the base
    TaskKind task = TaskKind::None;
    std::string why; // for the war log
};

struct CommanderTuning
{
    float attackMinMen = 8.0f;      // never send fewer than this at a target
    float forceRatio = 1.5f;        // wanted strength over the known enemy
    float defendMinMen = 4.0f;
    float garrisonMen = 8.0f;       // a quiet objective keeps this many defenders, the rest move on
    int depletedMen = 2;            // a group this small only defends
    int maxBuysPerPlan = 2;
};

/// The front: objectives `side` may attack now, best first, with the score of each.
std::vector<std::pair<int, float>> RankTargets(const SideView& view);

/// One planning pass. Returns the intents to apply; groups not mentioned keep their task.
std::vector<Intent> PlanStandard(const SideView& view, const CommanderTuning& tuning);
} // namespace Poseidon::Conquest
