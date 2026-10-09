// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class ControlsContainer;
class Display;
namespace Poseidon
{
inline constexpr int WholeWorldMenuId = 62100;
// Main-menu "Whole world": pick a place and the imagery/elevation providers, then restart the engine
// in whole-world (Earth streaming) mode there.
Display* CreateWholeWorldDisplay(ControlsContainer* parent);
} // namespace Poseidon
