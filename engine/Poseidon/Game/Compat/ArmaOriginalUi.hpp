// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mounted Arma game's own HUD and menus (--arma-original-ui, with --arma1 / --arma2).
//
// Arma's ui.pbo does not define its screens whole: its config patches the base config in
// Dta\bin.pbo, which Bohemia's ALDP data packages do not ship. CWA's resource is the same
// lineage (same class names, same idcs), so it stands in for that base: a private copy of
// CWA's resource and CfgInGameUI gets the Arma UI classes merged over it, the way Arma
// merges ui.pbo over bin.pbo. Only the screens listed in ArmaOriginalUi.cpp are drawn from
// the copy; every other CWA dialog keeps reading Res, untouched. Off by default.

#include <Poseidon/IO/ParamFile/ParamFile.hpp>

namespace Poseidon::ArmaOriginalUi
{
// --arma-original-ui was given and an Arma 1 or Arma 2 install is mounted.
bool Enabled();

// The Arma version of resource class `name` (RscInGameUI, RscDisplayMain, ...) when it is
// one of the switched screens and the feature is on; otherwise Res >> name.
const ParamEntry& Resource(const char* name);

// The Arma version of config class `name` (CfgInGameUI) when switched; otherwise Pars >> name.
const ParamEntry& Config(const char* name);
} // namespace Poseidon::ArmaOriginalUi
