// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Arma 3's own main menu (--arma3 with --arma-original-ui).
//
// Arma 3 does not draw its screens the way CWA, Arma 1 and Arma 2 do. RscDisplayMain is a
// tree of controls groups (type 15) holding shortcut buttons (type 16), structured text
// (type 13) and pictures, sized in pixel-grid expressions ("10 * (pixelW * pixelGrid * 2)"),
// and its behaviour -- the drop-down rows under SINGLEPLAYER / TUTORIALS / OPTIONS, the
// spotlight tiles -- is RscDisplayMain.sqf run through BIS_fnc_initDisplay. This engine has
// none of those control types and does not run that script, so the ArmaOriginalUi merge
// (Arma 1 / 2) cannot draw it.
//
// Here the screen is read straight from Arma 3's config (Arma1Compat::ConfigRoot, never
// merged into Res): groups are flattened into the display at their offset and shown/hidden
// as a unit, and each control is drawn by a small Arma 3 control of this file that reads
// Arma 3's members (animTexture*, colorBackgroundFocused, TextPos, Attributes, ...). What
// RscDisplayMain.sqf does is done natively: a title opens its row on hover, a spotlight
// shows its hover frame. Videos and the Steam info panels are skipped and logged.

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

#include <memory>
#include <vector>

class Control;
class ControlsContainer;

namespace Poseidon::Arma3Menu
{
// --arma-original-ui was given, Arma 3 is mounted and its config has RscDisplayMain.
bool Active();

// What a click on an Arma 3 main-menu entry does here.
enum class Action
{
    None,        // not an Arma 3 entry: DisplayMain's own handling
    Handled,     // the menu used it itself (a row title)
    Campaigns,   // the mounted game's campaigns (ArmaContentMenu)
    Missions,    // its scenarios, showcases and challenges
    Tutorials,   // its tutorials and VR training
    Options,     // CWA's Options screen (Video, Audio, Controls, Game)
    Unavailable, // with a message: needs what this engine does not have
};

// A loaded Arma 3 screen: its controls live in the display's own lists; this keeps how they
// nest in groups, for showing and hiding.
class Screen
{
  public:
    Screen();
    ~Screen();

    // Loads Arma 3's class `name` into `display`, appending to its control lists.
    bool Load(ControlsContainer& display, const char* name, RefArray<Control>& background,
              RefArray<Control>& foreground);

    // RscDisplayMain.sqf's onLoad, natively: rows collapsed, spotlight frames hidden.
    void InitMainMenu();
    // Per frame, with the cursor in screen units: rows open under the title the cursor is on
    // and stay open while it is over them; a spotlight shows its frame under the cursor.
    void UpdateMainMenu(float mouseX, float mouseY);
    // Replaces the text of the control(s) with `idc` (the version panel's label).
    void SetText(int idc, const RString& text);
    // The meaning of a main-menu click; `message` is set for Action::Unavailable.
    Action MainMenuClick(int idc, const char*& message);

  private:
    void AddCentreSpotlight();

    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace Poseidon::Arma3Menu
