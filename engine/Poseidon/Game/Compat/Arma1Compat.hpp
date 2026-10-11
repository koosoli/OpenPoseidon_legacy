// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Arma 1 (ArmA Gold) compatibility profile.
//
// Plays an original Arma 1 mission straight out of an installed Arma 1 folder:
// the folder's archives are mounted read-only (nothing is copied into the CWA
// install or anywhere else), Arma 1 terrains get generated CfgWorlds classes that
// inherit CWA's Eden, and Arma 1 unit/vehicle classes that CWA does not define are
// represented by their nearest CWA counterpart. Arma 1 configs are NOT parsed:
// gameplay, physics and weapons stay CWA's. Everything here is inert unless the
// game was started with --arma1.

#include <Poseidon/Foundation/Containers/RStringArray.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <string>

namespace Poseidon::Arma1Compat
{
// True when --arma1 named a usable Arma 1 install.
bool Active();

// Mounts <root>\AddOns, <root>\DBE1\AddOns and the campaign archives, data-only.
// Called from Globals::Init while the file banks are (re)built.
void MountArchives();

// Generates CfgWorlds classes for the Arma 1 terrains (Sara, SaraLite, Sara_dbe1,
// Porto) after the CWA config has been loaded. Existing CWA classes are never edited.
void RegisterWorldClasses();

// Returns the class to spawn for a mission/script class name. Classes CWA defines
// are returned unchanged; Arma 1-only classes map to a CWA counterpart.
RString ResolveVehicleClass(const RString& className);
RString ResolveWeaponClass(const RString& className);
RString ResolveMagazineClass(const RString& className);

// Drops Arma 1 addon names (CACharacters, CAWheeled, ...) from a mission's
// addOns[] so the CWA addon check does not refuse an Arma 1 mission.
void FilterMissionAddons(FindArrayRStringCI& addOns);

// Arma 1 campaign keys (isKeyActive / activateKey), persisted in the user folder.
bool IsKeyActive(const char* key);
void ActivateKey(const char* key);

// Resolves --arma1-mission to a virtual mission.sqm path inside the mounted banks
// ("campaigns\ca\missions\m09convoyattack.sara\mission.sqm"). Empty on failure,
// with the reason in `error`.
std::string ResolveMissionFile(const std::string& spec, std::string& error);

// For a virtual campaign mission path, the campaign base directory
// ("campaigns/ca/") so the campaign stringtable/description apply. Empty otherwise.
std::string CampaignBaseForMission(const std::string& missionFile);
} // namespace Poseidon::Arma1Compat
