// Conquest on Everon (docs/roadmap/conquest/CONQUEST-PLAN.md).
// The referee (engine/Poseidon/Game/Conquest) reads this class; without it the engine does nothing.
// Positions are CfgWorlds >> Eden >> Names (map metres, x east, y north); the airfield is its ilsPosition.

class CfgConquest
{
    sides[] = {"WEST", "EAST"};
    startPool = 500;   // each side, on top of its starting squads (prices doubled 2026-10-26)
    startSquads[] = {"Motorised", "Rifle", "Recon"}; // free, at each HQ when the war begins
    cap = 60;          // living soldiers per side
    planSeconds = 20;  // the commanders replan this often, and at once on a capture
    // The opening screen offers the side and these choices; setup = 0 starts at once with them.
    setup = 1;
    startAs = "soldier"; // lead the first squad ("commander": start in the top-down view)
    possession = 1;      // allow taking control of any friendly soldier
    commanderView = 1;   // the top-down command view (Ctrl+M)
    fog = "contacts";    // or "shroud": unexplored ground stays black in the command view
    hqRadio = 1;         // the AI commander radios its orders while the player is not in command

    // Each objective's command post: a tent near its centre with the owner's flag beside it. Only
    // soldiers within captureRadius of it count towards taking the objective.
    captureRadius = 30;
    postClass = "Camp";
    flagClass = "FlagCarrier";

    // The arsenal box at each owned post: what taking gear from it costs the side.
    arsenalWeaponCost = 25;
    arsenalMagazineCost = 3;

    // Mortar support for the player's side (the command view's SUPPORT panel).
    barrageCost = 150;
    barrageCooldown = 120;
    barrageShells = 8;
    barrageSpread = 35;
    barrageAmmo = "Shell120"; // a 120 mm mortar round

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
        };
        class Motorised
        {
            name = "Motorised squad";
            cost = 260;
            role = "motorised";
            WEST[] = {"OfficerW", "SoldierWB", "SoldierWMG", "SoldierWB", "SoldierWLAW", "SoldierWB", "SoldierWMedic", "SoldierWB"};
            EAST[] = {"OfficerE", "SoldierEB", "SoldierEMG", "SoldierEB", "SoldierELAW", "SoldierEB", "SoldierEMedic", "SoldierEB"};
            WESTvehicles[] = {"Truck5tOpen"};
            EASTvehicles[] = {"Ural"};
        };
        class Recon
        {
            name = "Recon jeep";
            cost = 140;
            role = "recon";
            ai = 0; // four men in a jeep: the player's scouting tool, poor value as an AI reinforcement
            WEST[] = {"SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB"};
            EAST[] = {"SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB"};
            WESTvehicles[] = {"Jeep"};
            EASTvehicles[] = {"UAZ"};
        };
        class Mechanised
        {
            name = "Mechanised squad";
            cost = 440;
            role = "mechanised";
            WEST[] = {"SoldierWCrew", "SoldierWCrew", "SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB", "SoldierWB", "SoldierWMedic"};
            EAST[] = {"SoldierECrew", "SoldierECrew", "SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB", "SoldierEB", "SoldierEMedic"};
            WESTvehicles[] = {"M113"};
            EASTvehicles[] = {"BMP"};
        };
        class AirAssault
        {
            name = "Air assault squad";
            cost = 520;
            role = "air";
            WEST[] = {"SoldierWPilot", "OfficerW", "SoldierWB", "SoldierWMG", "SoldierWLAW", "SoldierWB", "SoldierWB", "SoldierWMedic"};
            EAST[] = {"SoldierEPilot", "OfficerE", "SoldierEB", "SoldierEMG", "SoldierELAW", "SoldierEB", "SoldierEB", "SoldierEMedic"};
            WESTvehicles[] = {"UH60"};
            EASTvehicles[] = {"Mi17"};
        };
        // Heavy: the AI saves up for one in every four purchases. Their crews stay mounted.
        class Armour
        {
            name = "Tank";
            cost = 650;
            role = "armour";
            WEST[] = {"SoldierWCrew", "SoldierWCrew", "SoldierWCrew"};
            EAST[] = {"SoldierECrew", "SoldierECrew", "SoldierECrew"};
            WESTvehicles[] = {"M60"};
            EASTvehicles[] = {"T72"};
        };
        class Gunship
        {
            name = "Gunship";
            cost = 750;
            role = "gunship";
            WEST[] = {"SoldierWPilot", "SoldierWPilot"};
            EAST[] = {"SoldierEPilot", "SoldierEPilot"};
            WESTvehicles[] = {"Cobra"};
            EASTvehicles[] = {"Mi24"};
        };
    };

    class Objectives
    {
        class Morton     { name = "Morton";     position[] = {5134, 3955};  radius = 120; value = 10; kind = "HQ"; owner = "WEST"; neighbours[] = {"Figari"}; };
        class Figari     { name = "Figari";     position[] = {5283, 5375};  radius = 120; value = 5;  neighbours[] = {"Montignac", "EntreDeux"}; };
        class EntreDeux  { name = "Entre Deux"; position[] = {5735, 7059};  radius = 120; value = 5;  neighbours[] = {"Montignac"}; };
        class Montignac  { name = "Montignac";  position[] = {4935, 6994};  radius = 150; value = 10; neighbours[] = {"Gravette", "Tyrone"}; };
        class Gravette   { name = "Gravette";   position[] = {4112, 7772};  radius = 120; value = 5;  neighbours[] = {"Tyrone"}; };
        class Tyrone     { name = "Tyrone";     position[] = {4946, 9056};  radius = 120; value = 5;  neighbours[] = {"Airfield"}; };
        class Airfield   { name = "Airfield";   position[] = {4772, 10923}; radius = 200; value = 10; kind = "HQ"; owner = "EAST"; };
    };
};
