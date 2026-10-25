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
// dev-missions/conquest-everon.Eden). Single player only for now; not saved yet (Slice 1).

#include <Poseidon/Game/Conquest/ConquestCommander.hpp>
#include <Poseidon/Game/Conquest/ConquestRules.hpp>

#include <array>
#include <deque>
#include <string>
#include <vector>

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

/// The player's map click as their side's commander (x east, y north). A click near one of the
/// side's squads selects it (shift adds); otherwise the selected squads get the order: attack or
/// defend the objective clicked, or move to the point. Returns true when the click was used.
bool MapClick(float x, float y, bool shift, bool alt);

/// One line per objective plus pools and scores (the conquestState script command, the log).
std::string StatusText();
} // namespace Poseidon::Conquest
