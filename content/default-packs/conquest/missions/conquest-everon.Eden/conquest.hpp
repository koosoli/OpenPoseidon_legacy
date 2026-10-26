// Conquest: the war's rules, shared by every Conquest mission (docs/roadmap/conquest/CONQUEST-PLAN.md).
// The referee (engine/Poseidon/Game/Conquest) reads this class; without it the engine does nothing.
// Positions are CfgWorlds >> Eden >> Names (map metres, x east, y north); the airfield is its ilsPosition.

class CfgConquest
{
    // GUER (the Resistance, from Le Moule) takes part only when the setup screen (or resistance = 1)
    // says so; otherwise its base is one more neutral village.
    sides[] = {"WEST", "EAST", "GUER"};
    resistance = 0;
    startPool = 500;   // each side, on top of its starting squads (the setup screen sets each side's)
    // Free, at each HQ when the war begins: three squads, a tank and a helicopter with its squad.
    startSquads[] = {"Motorised", "Rifle", "Recon", "Armour", "AirAssault"};
    cap = 60;          // living soldiers per side
    planSeconds = 20;  // the commanders replan this often, and at once on a capture
    victoryTowns = 0;  // villages a side must hold to win (0: the war score or the enemy's last base decides)
    // The opening screen offers the side and these choices; setup = 0 starts at once with them.
    setup = 1;
    startAs = "soldier"; // lead the first squad ("commander": start in the top-down view)
    possession = 1;      // allow taking control of any friendly soldier
    commanderView = 1;   // the top-down command view (Ctrl+M)
    fog = "contacts";    // or "shroud": unexplored ground stays black in the command view
    hqRadio = 1;         // the AI commander radios its orders while the player is not in command
    // A side's commander personality: "balanced", "aggressive" or "cautious" (the AI's posture).
    WESTpersonality = "balanced";
    EASTpersonality = "aggressive";
    GUERpersonality = "cautious";

    // Each objective's command post: a tent near its centre with the owner's flag beside it. Only
    // soldiers within captureRadius of it count towards taking the objective.
    captureRadius = 30;
    postClass = "Camp";
    flagClass = "FlagCarrier";

    // The arsenal box at each owned post: what taking gear from it costs the side.
    arsenalWeaponCost = 25;
    arsenalMagazineCost = 3;

    // Mortar support (the command view's SUPPORT panel; the AI commanders fire it too).
    barrageCost = 150;
    barrageCooldown = 120;
    barrageShells = 8;
    barrageSpread = 35;
    barrageAmmo = "Shell120"; // a 120 mm mortar round
    barrageRange = 4000;      // a side fires only on targets this close to a base it holds

    // Air support (SUPPORT panel; the AI commanders call it on armour and crowds the mortars cannot
    // reach): the side's attack plane flies in from its own ground and drops a tight stick of bombs.
    airStrikeCost = 400;
    airStrikeCooldown = 300;
    airStrikeBombs = 4;
    airStrikeSpread = 12;          // metres between bombs along the plane's track
    airStrikeAmmo = "LaserGuidedBomb";
    WESTairStrikePlane = "A10";
    EASTairStrikePlane = "Su25";
    GUERairStrikePlane = "Su25";

    // What a side can buy. <SIDE>[] are the soldiers, <SIDE>vehicles[] spawn empty beside them and
    // belong to the squad: it mounts for moves over 200 m and gets out near its destination.
    // role tells the AI commanders when to buy it (foot, motorised, mechanised, air). The first
    // class is the AI's default rifle squad.
    class Squads
    {
        class Rifle
        {
            name = "Rifle squad";
            cost = 160;
            role = "foot";
            WEST[] = {"OfficerW", "SoldierWB", "SoldierWMG", "SoldierWB", "SoldierWLAW", "SoldierWB", "SoldierWMedic", "SoldierWB"};
            EAST[] = {"OfficerE", "SoldierEB", "SoldierEMG", "SoldierEB", "SoldierELAW", "SoldierEB", "SoldierEMedic", "SoldierEB"};
            GUER[] = {"OfficerG", "SoldierGB", "SoldierGMG", "SoldierGB", "SoldierGLAW", "SoldierGB", "SoldierGMedic", "SoldierGB"};
        };
        class Motorised
        {
            name = "Motorised squad";
            cost = 260;
            role = "motorised";
            WEST[] = {"OfficerW", "SoldierWB", "SoldierWMG", "SoldierWB", "SoldierWLAW", "SoldierWB", "SoldierWMedic", "SoldierWB"};
            EAST[] = {"OfficerE", "SoldierEB", "SoldierEMG", "SoldierEB", "SoldierELAW", "SoldierEB", "SoldierEMedic", "SoldierEB"};
            GUER[] = {"OfficerG", "SoldierGB", "SoldierGMG", "SoldierGB", "SoldierGLAW", "SoldierGB", "SoldierGMedic", "SoldierGB"};
            WESTvehicles[] = {"Truck5tOpen"};
            EASTvehicles[] = {"Ural"};
            GUERvehicles[] = {"Ural"};
        };
        class Recon
        {
            name = "Recon jeep";
            cost = 140;
            role = "recon";
            ai = 0; // four men in a jeep: the player's scouting tool, poor value as an AI reinforcement
            WEST[] = {"SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB"};
            EAST[] = {"SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB"};
            GUER[] = {"SoldierGB", "SoldierGMG", "SoldierGLAW", "SoldierGB"};
            WESTvehicles[] = {"Jeep"};
            EASTvehicles[] = {"UAZ"};
            GUERvehicles[] = {"UAZ"};
        };
        class Mechanised
        {
            name = "Mechanised squad";
            cost = 440;
            role = "mechanised";
            WEST[] = {"SoldierWCrew", "SoldierWCrew", "SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB", "SoldierWB", "SoldierWMedic"};
            EAST[] = {"SoldierECrew", "SoldierECrew", "SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB", "SoldierEB", "SoldierEMedic"};
            GUER[] = {"SoldierGCrew", "SoldierGCrew", "SoldierGB", "SoldierGMG", "SoldierGLAW", "SoldierGB", "SoldierGB", "SoldierGMedic"};
            WESTvehicles[] = {"M113"};
            EASTvehicles[] = {"BMP"};
            GUERvehicles[] = {"BMP"};
        };
        class AirAssault
        {
            name = "Air assault squad";
            cost = 520;
            role = "air";
            WEST[] = {"SoldierWPilot", "OfficerW", "SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB", "SoldierWB", "SoldierWMedic"};
            EAST[] = {"SoldierEPilot", "OfficerE", "SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB", "SoldierEB", "SoldierEMedic"};
            GUER[] = {"SoldierGPilot", "OfficerG", "SoldierGB", "SoldierGMG", "SoldierGLAW", "SoldierGB", "SoldierGB", "SoldierGMedic"};
            WESTvehicles[] = {"UH60"};
            EASTvehicles[] = {"Mi17"};
            GUERvehicles[] = {"Mi17"};
        };
        // Heavy: the AI saves up for one in every four purchases. Their crews stay mounted.
        class Armour
        {
            name = "Tank";
            cost = 650;
            role = "armour";
            WEST[] = {"SoldierWCrew", "SoldierWCrew", "SoldierWCrew"};
            EAST[] = {"SoldierECrew", "SoldierECrew", "SoldierECrew"};
            GUER[] = {"SoldierGCrew", "SoldierGCrew", "SoldierGCrew"};
            WESTvehicles[] = {"M60"};
            EASTvehicles[] = {"T72"};
            GUERvehicles[] = {"T55"};
        };
        class Gunship
        {
            name = "Gunship";
            cost = 750;
            role = "gunship";
            WEST[] = {"SoldierWPilot", "SoldierWPilot"};
            EAST[] = {"SoldierEPilot", "SoldierEPilot"};
            GUER[] = {"SoldierGPilot", "SoldierGPilot"};
            WESTvehicles[] = {"Cobra"};
            EASTvehicles[] = {"Mi24"};
            GUERvehicles[] = {"Mi24"};
        };
    };

    // The objectives: the island's own file (objectives.hpp) beside this one. Everon's are written
    // by hand; an island without one says objectives = "auto" and the war is made from its town list.
#include "objectives.hpp"
};
