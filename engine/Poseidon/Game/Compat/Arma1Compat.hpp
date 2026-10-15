// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Arma 1 (ArmA Gold) compatibility profile.
//
// Plays an original Arma 1 mission straight out of an installed Arma 1 folder:
// the folder's archives are mounted read-only (nothing is copied into the CWA
// install or anywhere else), Arma 1 terrains get generated CfgWorlds classes that
// inherit CWA's Eden, and Arma 1 unit/vehicle classes that CWA does not define are
// represented by their nearest CWA counterpart, drawn with the Arma 1 model where one
// exists. Arma 1 configs are read into a private tree (Arma1Config.cpp) and never
// merged into Pars: gameplay, physics and weapons stay CWA's. Everything here is
// inert unless the game was started with --arma1.

#include <Poseidon/Foundation/Containers/RStringArray.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
// True when --arma1 or --arma2 named a usable install.
bool Active();

// The mounted generation: 1 for Arma 1 (--arma1), 2 for Arma 2 / OA (--arma2); 0 inactive.
// Everything below serves both; where Arma 2 lays its data out differently (Dta, the
// expansion folders, XML stringtables, binarised animations) the generation decides.
int Generation();

// "Arma 1" / "Arma 2", for log lines.
const char* Label();

// Mounts <root>\AddOns, <root>\DBE1\AddOns and the campaign archives, data-only.
// Called from Globals::Init while the file banks are (re)built.
void MountArchives();

// Generates CfgWorlds classes for the Arma 1 terrains (Sara, SaraLite, Sara_dbe1,
// Porto) after the CWA config has been loaded. Existing CWA classes are never edited.
void RegisterWorldClasses();

// Reads the Arma 1 configs (bin + every addon config.bin) into a private tree and
// the Arma 1 stringtables into a lowest-priority table. Never merged into Pars.
void LoadConfig();

// The root of the Arma 1 config tree (for script config access), or null when not loaded.
const ParamEntry* ConfigRoot();

// Arma 1 config class root >> name (CfgRadio, CfgSounds, CfgSFX, ...), or null.
const ParamEntry* FindConfigClass(const char* root, const char* name);

// A generated CfgVehicles class that keeps `cwaClass`'s simulation but draws the
// Arma 1 model of `missionClass` (soldiers also get the Arma 1 moves). Returns
// `cwaClass` when there is no usable Arma 1 model.
RString Arma1ModelClass(const RString& missionClass, const RString& cwaClass);

// What an Arma 1 proxy model path stands for, from Arma 1's own CfgNonAIVehicles
// ("\\ca\\weapons\\ak_47_v58_proxy" -> ProxyAK_47_v58_Proxy, simulation "proxyweapon";
// "...\\uh_60\\pilot" -> ProxyPilot, "proxycrew" at crew position 0). Lower-case
// simulation; crewPosition 0 driver, 1 gunner, 2 commander, 3 cargo, else -1.
bool ProxyKind(const char* modelName, std::string& simulation, int& crewPosition);

// A CWA class to carry an Arma 1 class CWA does not define and no alias names:
// its nearest Arma 1 ancestor CWA has (same simulation, usable scope), else a CWA
// class of that simulation and side, else any CWA ancestor. Empty when none.
RString FallbackCwaClass(const RString& a1Name);

// An Arma 1 binarised config FILE (mission.sqm, description.ext, ...) as config
// text the engine's parser reads. False when the file is not in Arma 1's layout
// (OFP/CWA's own binarised files keep their reader). Works without --arma1.
bool Arma1RapFileToText(const char* fileName, std::string& text);

// Returns the class to spawn for a mission/script class name. Classes CWA defines
// are returned unchanged; Arma 1-only classes map to a CWA counterpart.
RString ResolveVehicleClass(const RString& className);
RString ResolveWeaponClass(const RString& className);
RString ResolveMagazineClass(const RString& className);

// Arma 2: the CWA weapon (for a weapon) or magazine (for a magazine) that carries an
// Arma 2 weapon/magazine class, chosen by its kind (pistol, rifle, machine gun, sniper
// rifle, launcher, grenade, ...) and the calibre of its ammunition. Empty when the
// name is no Arma 2 weapon or magazine.
RString Arma2WeaponFallback(const RString& name);

// For an Arma 2 weapon carried by the CWA weapon cwaWeapon: a generated A1x_<name> class with
// the Arma 2 model (in the hand, on the ground) on that CWA weapon, which keeps its magazines
// and ballistics. Returns cwaWeapon when there is no usable model. OP_A2_WEAPON_MODELS=0 turns
// it off.
RString Arma2WeaponModelClass(const RString& name, const RString& cwaWeapon);
// Whether an Arma 2 CfgWeapons class is a hand weapon (rifle, machine gun, pistol,
// launcher) -- the kind that carries a model of its own in the soldier's hands.
bool Arma2IsHandWeapon(const RString& name);

// Arma 2: the EventHandlers >> init code of a module (a class derived from Logic, such
// as FunctionsManager or the first-aid modules) that a mission places; empty otherwise.
RString Arma2ModuleInit(const RString& className);

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

// Every mission.sqm inside the mounted archives, as virtual paths ("ca\\missions\\Scenarios\\
// SP_EyeForEye.Chernarus\\mission.sqm"). Templates and campaigns included.
std::vector<std::string> MissionFiles();

// True when the mission directory lies inside a mounted Arma 1 archive.
bool IsArma1MissionDirectory(const char* missionDirectory);

// The CfgWorlds class an Arma 1 mission's world name means: "intro" in an Arma 1
// archive is Arma 1's desert (A1_Intro), not CWA's Intro island.
RString WorldClassForMission(const RString& world, const char* missionDirectory);

// For a virtual campaign mission path, the campaign base directory
// ("campaigns/ca/") so the campaign stringtable/description apply. Empty otherwise.
std::string CampaignBaseForMission(const std::string& missionFile);

// The file of a sound an Arma 1/2 config names, where CWA's rule (GetSoundName: "sound\"
// before a relative name, ".wss" after one without extension) finds none. Arma 2's
// configs name many sounds by their archive path without the leading backslash, and its
// Steam release holds .ogg files where the configs still say .wss. "" when nothing is
// found, or outside --arma1/--arma2.
RString FindArmaSoundFile(const char* configName);

// Logs what every entity of the running mission became: class (A1x_ = the Arma model on a
// CWA base), model, LOD count, crew and weapons, grouped by kind (Arma2Census.cpp).
void LogAssetCensus();
// The player's position and the map models around him (part of the census; logged again
// at the first test screenshot, once streamed objects have arrived).
void LogPlayerSurroundings();
} // namespace Poseidon::Arma1Compat
