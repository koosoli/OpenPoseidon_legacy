// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Conquest mode: the commander view, a top-down real-time-strategy screen over the live war.
//
// Ctrl+M toggles it while a Conquest war runs. An overhead camera (WASD or the screen edge to pan,
// the wheel to zoom, Q/E or the middle mouse button to turn), left click or drag a box to select
// squads, right click to order them (an objective: attack or defend it; an enemy: go for it; the
// ground: move there), and a HUD with the side's supplies, its squads, and the bases where new
// squads can be bought. Enemy positions come from the side's own knowledge (fog of war).
//
// It draws with the ImGui overlay the engine already composites every frame, and takes input
// through the overlay's event hook (DebugOverlay::ProcessEvent), so it needs no dev panel.

union SDL_Event;

namespace Poseidon::Conquest::View
{
bool Active();
/// On or off; off whenever no Conquest war runs.
void SetActive(bool active);

/// Every SDL event, before the game sees it. True when the view used it.
bool ProcessEvent(const SDL_Event& event);
/// Whether the event ProcessEvent just saw must be kept from the game.
bool WantsMouse();
bool WantsKeyboard();

/// Once per rendered frame inside the overlay's ImGui frame: moves the camera, draws the view
/// (or, while it is closed, the soldier's HUD).
void Frame();

/// Loads the HUD's typefaces; called between frames by the referee while a war runs.
void PrepareFonts();

/// The camera at (x, y) from `distance` metres, `pitch` degrees down, facing `yaw` degrees (0 north).
bool PlaceCamera(float x, float y, float distance, float pitch, float yawDegrees);
/// Select squad `groupId`, its leader, and follow him from `distance` metres, `pitch` degrees down.
bool FollowSquad(int groupId, float distance, float pitch);
} // namespace Poseidon::Conquest::View
