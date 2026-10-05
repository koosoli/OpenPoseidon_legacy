// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
class ControlsContainer;
class Display;
namespace Poseidon
{
inline constexpr int LocalMapsMenuId = 62000;
Display* CreateLocalMapsDisplay(ControlsContainer* parent);
} // namespace Poseidon
