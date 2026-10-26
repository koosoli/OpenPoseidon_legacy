// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
class ControlsContainer;
class Display;
namespace Poseidon
{
inline constexpr int LocalMapsMenuId = 62000;
Display* CreateLocalMapsDisplay(ControlsContainer* parent);

/// Main menu "ISLAND EDITOR": the standalone .wrp editor (apps/tools/IslandEditor, Windows only).
inline constexpr int IslandEditorMenuId = 62200;
/// Starts PoseidonIslandEditor.exe from beside the running executable, pointed at the game folder.
/// The game keeps running. Returns false with `error` set when it cannot.
bool LaunchIslandEditor(std::string& error);

/// Main menu "CONQUEST": the island chooser, then that island's war from the @OP_Conquest pack
/// (Missions\OpenPoseidon).
inline constexpr int ConquestMenuId = 62300;
inline constexpr int ConquestIslandIdd = 62310;
Display* CreateConquestIslandDisplay(ControlsContainer* parent);
/// The mission the island chooser closed on ("conquest-malden.Abel"), "" when it was left with Back.
std::string ConquestIslandChoice();
} // namespace Poseidon
