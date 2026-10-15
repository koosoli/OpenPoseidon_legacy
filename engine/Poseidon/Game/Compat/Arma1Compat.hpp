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
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon::Arma1Compat
{
// True when --arma1, --arma2 or --arma3 named a usable install.
bool Active();

// The mounted generation: 1 for Arma 1 (--arma1), 2 for Arma 2 / OA (--arma2), 3 for
// Arma 3 and its DLCs (--arma3); 0 inactive. Everything below serves all three; where a
// later game lays its data out differently (Dta, the expansion and DLC folders, XML
// stringtables, binarised animations, the Eden mission layout) the generation decides.
// "Arma 2 and later" is written Generation() >= 2.
int Generation();

// "Arma 1" / "Arma 2" / "Arma 3", for log lines.
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

// Arma 1: the CWA class to carry an armed Arma 1 car whose name resolved to `cwaClass`.
// A car whose main gun fires bullets (HMMWV M2, UAZ DShKM, pickups with a PK) on one of
// CWA's unarmed light cars goes to CWA's JeepMG, so its gunner has a seat and an M2.
// Grenade launchers, missiles, trucks and every other case keep `cwaClass`.
// OP_A1_ARMED_CARS=0 turns it off.
RString Arma1ArmedCarCarrier(const RString& a1Name, const RString& cwaClass);

// An Arma 1 binarised config FILE (mission.sqm, description.ext, ...) as config
// text the engine's parser reads. False when the file is not in Arma 1's layout
// (OFP/CWA's own binarised files keep their reader). Works without --arma1.
bool Arma1RapFileToText(const char* fileName, std::string& text);

// Returns the class to spawn for a mission/script class name. Classes CWA defines
// are returned unchanged; Arma 1-only classes map to a CWA counterpart.
RString ResolveVehicleClass(const RString& className);
RString ResolveWeaponClass(const RString& className);
RString ResolveMagazineClass(const RString& className);

// The class name a script sees for an engine class: a class the import generated
// ("A1x_SoldierWB", "A1x_M4AIM") is shown under the mission's own name ("M4AIM").
// Identity for every other name and when no Arma install is active.
RString ScriptClassName(const RString& engineName);

// The magazine name a script sees on a unit of the generated class `unitClass`: the Arma
// magazine of that unit's own loadout that the CWA magazine stands for ("M16" on an
// A1x_SoldierWB -> "30Rnd_556x45_Stanag"). The CWA name for any other unit or magazine.
RString ScriptMagazineName(const RString& unitClass, const RString& cwaMagazine);

// The text the HUD and the action menu show for a CWA magazine that an entity of class
// `ownerClass` carries or holds: the displayName of the Arma magazine it stands for (see
// ScriptMagazineName; for other entities, the Arma magazine first resolved to it). Empty
// when there is none, or without --arma1/--arma2.
RString MagazineDisplayName(const RString& ownerClass, const RString& cwaMagazine);

// Whether a weapon or muzzle name a script gives ("M4AIM") names the engine weapon
// `engineName`: the name itself, the generated A1x_ class of it, or the CWA class it
// resolves to. Exact (case-insensitive) comparison when no Arma install is active.
bool ScriptWeaponMatches(const char* engineName, const RString& scriptName);

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

// Arma 1 soldiers carry their own weapons with the Arma 1 models (OP_A1_WEAPONS=0: off).
bool Arma1OwnWeapons();

// An Arma 1 magazine whose round count differs from the CWA magazine that carries it
// (5Rnd_762x51_M24 on CWA's 20-round M21): a generated A1x_<magazine> class deriving from
// the CWA magazine with the Arma 1 count and name. Every muzzle that takes the CWA magazine
// also takes the generated one. Returns cwaMagazine when the counts agree, the name is
// also an Arma 1 weapon (M136), or without --arma1. OP_A1_MAGAZINES=0 turns it off.
RString Arma1MagazineClass(const RString& name, const RString& cwaMagazine);

// The magazines generated by Arma1MagazineClass on the CWA magazine `cwaMagazine`.
std::vector<RString> GeneratedMagazinesFor(const char* cwaMagazine);

// Whether the generated magazine `magazine` (A1x_5Rnd_762x51_M24) fits a muzzle of the
// weapon class `weaponClass`: for a generated A1x_ weapon, only the magazines its Arma 1
// weapon lists (the M24 takes no M107 rounds although both ride on CWA's M21); any CWA
// weapon that takes the carrier magazine takes it too.
bool GeneratedMagazineFits(const char* weaponClass, const char* magazine);

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
// True when `name` (a file name from an Arma config) starts with the top folder of a mounted
// Arma archive ("a3\anims_f\...", "ca\..."): a path of its own, though written without
// the leading backslash (Arma 3's binarised configs drop it), which must not be put under
// CWA's default folder ("anim\", "data\", "sound\").
bool IsArmaBankPath(const char* name);

// Arma 3 draws a man's head through a placeholder proxy (\a3\characters_f\heads\bysta)
// that it replaces with the head of the unit's identity. Here: the model of CfgHeads
// Default_A3 for that proxy, "" for every other proxy.
RString Arma3HeadForProxy(const char* proxyModel);

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
// A simulation name without Arma 3's PhysX suffixes: carx -> car, tankx -> tank,
// helicopterrtd / helicopterx -> helicopter, airplanex -> airplane, shipx / submarinex ->
// ship. Lower case. Arma 1 / 2 names come back unchanged (lower case).
std::string SimKind(const std::string& simulation);

// Arma 3: the CWA class that carries an Arma 3 vehicle class, chosen by the class's
// ancestry in the Arma 3 config (Tank / APC / anti-air, Wheeled_APC_F, Truck_F, Car,
// Helicopter attack / transport / light, Plane, Ship, StaticWeapon, ParachuteBase) and
// its side. Empty for anything else (men, things) and outside Arma 3. (Arma3Compat.cpp)
RString Arma3VehicleCarrier(const RString& className);

// Arma 3: true for animals (Animal_Base_F), which are left out like Arma 2's.
bool Arma3IsAnimal(const RString& className);

// Arma 3: a mission.sqm as config text the engine's parser reads -- binarised or not,
// Eden layout or Arma 2 layout (Arma3Eden.hpp). False when the file is no Arma 3
// mission.sqm that needs it (outside --arma3, other files, plain text the parser reads
// as it is).
bool Arma3MissionFileToText(const char* fileName, std::string& text);

// Any binarised config ("\0raP") as config text. False when it is not one.
bool RapBytesToText(const std::vector<uint8_t>& bytes, std::string& text);

// Logs what every entity of the running mission became: class (A1x_ = the Arma model on a
// CWA base), model, LOD count, crew and weapons, grouped by kind (Arma2Census.cpp).
void LogAssetCensus();
// The player's position and the map models around him (part of the census; logged again
// at the first test screenshot, once streamed objects have arrived).
void LogPlayerSurroundings();
} // namespace Poseidon::Arma1Compat
