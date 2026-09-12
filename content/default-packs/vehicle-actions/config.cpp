class CfgPatches
{
    class op_jeep_actions
    {
        units[] = {};
        weapons[] = {};
        requiredAddons[] = {};
    };
};
class CfgMovesMC
{
    class Actions
    {
        class CargoActions;
        class OPJeepActions: CargoActions
        {
            reloadMagazine = "OPJeepReload";
        };
        class OPHeliActions: CargoActions { reloadMagazine = "OPHeliReload"; };
    };
    class States
    {
        class JeepCoDriver;
        class OPJeepIdle: JeepCoDriver
        {
            file = "\op_jeep_actions\jeep_idle.rtm";
            actions = "OPJeepActions";
            speed = 1;
            disableWeapons = 1;
            variantsAI[] = {};
            variantsPlayer[] = {};
            equivalentTo = "";
            connectTo[] = {"OPJeepRaise",1};
            interpolateWith[] = {};
        };
        class OPJeepRaise: OPJeepIdle { file = "\op_jeep_actions\jeep_raise.rtm"; speed = 1.4; looped = 0; connectTo[] = {"OPJeepAim",1}; };
        class OPJeepAim: OPJeepIdle
        {
            file = "\op_jeep_actions\jeep_aim.rtm";
            disableWeapons = 0;
            connectTo[] = {"OPJeepLower",1,"OPJeepReload",1};
        };
        class OPJeepRecoil: OPJeepIdle { file = "\op_jeep_actions\jeep_recoil.rtm"; speed = 4; };
        class OPJeepReload: OPJeepIdle { file = "\op_jeep_actions\jeep_reload.rtm"; speed = 0.4; looped = 0; connectTo[] = {"OPJeepAim",1}; };
        class OPJeepLower: OPJeepIdle { file = "\op_jeep_actions\jeep_lower.rtm"; speed = 1.4; looped = 0; connectTo[] = {"OPJeepIdle",1}; };
        class OPHeliIdle: OPJeepIdle { file = "\op_jeep_actions\uh60_idle.rtm"; actions = "OPHeliActions"; connectTo[] = {"OPHeliRaise",1}; };
        class OPHeliRaise: OPHeliIdle { file = "\op_jeep_actions\uh60_raise.rtm"; speed = 1.4; looped = 0; connectTo[] = {"OPHeliAim",1}; };
        class OPHeliAim: OPHeliIdle { file = "\op_jeep_actions\uh60_aim.rtm"; disableWeapons = 0; connectTo[] = {"OPHeliLower",1,"OPHeliReload",1}; };
        class OPHeliRecoil: OPHeliIdle { file = "\op_jeep_actions\uh60_recoil.rtm"; speed = 4; };
        class OPHeliReload: OPHeliIdle { file = "\op_jeep_actions\uh60_reload.rtm"; speed = 0.4; looped = 0; connectTo[] = {"OPHeliAim",1}; };
        class OPHeliLower: OPHeliIdle { file = "\op_jeep_actions\uh60_lower.rtm"; speed = 1.4; looped = 0; connectTo[] = {"OPHeliIdle",1}; };
    };
};
// Version 3 uses the personal primary bullet family with the existing seated actions.
class CfgOPVehicleActions
{
    class UH60
    {
        class OPCargoWeapons
        {
            version = 3;
            class Seats
            {
                class RightDoor
                {
                    cargoIndex = 10;
                    weaponFamily = "rifle";
                    action = "OPHeliAim";
                    idleAction = "OPHeliIdle";
                    raiseAction = "OPHeliRaise";
                    reloadAction = "OPHeliReload";
                    lowerAction = "OPHeliLower";
                    azimuth = 90;
                    halfAngle = 25;
                    minElevation = -45;
                    maxElevation = 20;
                    aiAimSpeed = 45;
                };
            };
        };
    };
    class Jeep
    {
        class OPCargoWeapons
        {
            version = 3;
            class Seats
            {
                class FrontPassenger
                {
                    cargoIndex = 0;
                    weaponFamily = "rifle";
                    action = "OPJeepAim";
                    idleAction = "OPJeepIdle";
                    raiseAction = "OPJeepRaise";
                    reloadAction = "OPJeepReload";
                    lowerAction = "OPJeepLower";
                    azimuth = 90;
                    halfAngle = 25;
                    minElevation = -15;
                    maxElevation = 20;
                    aiAimSpeed = 45;
                };
            };
        };
    };
};
