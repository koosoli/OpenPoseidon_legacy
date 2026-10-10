// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class ControlsContainer;
class Display;
#include <string>
namespace Poseidon
{
inline constexpr int WholeWorldMenuId = 62100;
// Main-menu "Whole world": pick a place and the imagery/elevation providers, then restart the engine
// in whole-world (Earth streaming) mode there.
Display* CreateWholeWorldDisplay(ControlsContainer* parent);
// From inside whole-world mode (the mission editor's world map): restart the engine in the editor with the
// mission area moved to latitude/longitude, keeping this process's providers and settings. The caller saves
// the mission to WholeWorldEditorMission() first. False with `error` when the restart could not start.
bool RelaunchWholeWorldEditor(double latitude, double longitude, std::string& error);
std::string WholeWorldEditorMission();
} // namespace Poseidon
