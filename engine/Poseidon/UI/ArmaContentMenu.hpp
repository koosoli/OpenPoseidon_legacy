// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The main menu's Campaign / Missions / Tutorials entries while an Arma game is mounted
// (--arma1 / --arma2 / --arma3): CWA's own screens list CWA's campaigns and the Missions
// folder, so they showed OFP content, and Arma 1's own Campaign button had no handler at all.
// This lists the mounted game's content instead; DisplayMain starts the choice in-process,
// through the same calls CWA's screens use (StartCampaign, ProcessFullName + DisplayIntro).

#include <string>

class ControlsContainer;
class Display;

namespace Poseidon
{
constexpr int ArmaContentIdd = 62100;

enum class ArmaContentKind
{
    Campaigns,
    Missions,
    Tutorials,
};

// What a main-menu button means while an Arma game is mounted: a list to open (true, with
// `kind`), or false for every button that keeps its usual action.
bool ArmaMenuContentKind(int idc, ArmaContentKind& kind);

// Arma 1 menu buttons with no CWA counterpart that open the Options screen (Options and its
// Video / Audio / Controls / Difficulty shortcuts).
bool ArmaMenuOpensOptions(int idc);

// Arma 1 menu buttons that cannot work here (the Armory is script-built); the message to show.
const char* ArmaMenuUnavailable(int idc);

Display* CreateArmaContentDisplay(ControlsContainer* parent, ArmaContentKind kind);

// The entry chosen in a closed ArmaContentIdd display: a campaign name (campaign = true) or a
// virtual mission.sqm path. False when nothing was chosen.
bool ArmaContentChoice(ControlsContainer* display, bool& campaign, std::string& value);

// POSEIDON_MENU_CLICKS="120,1": a test hook for captures. Called once per frame by the main loop:
// every 2 s, presses the next listed idc on the top-most display (-1: opens the pause menu
// in a running mission, -2: switches the player to the aiming view, -3: runs the
// script line in POSEIDON_MENU_SCRIPT). Does nothing without the variable.
void TickMenuClickHook();
} // namespace Poseidon
