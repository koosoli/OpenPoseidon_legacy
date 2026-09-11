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
    };
};
// One explicit SP seat. Other Jeep cargo, drivers and multiplayer retain
// the original behaviour. Version 2 enters the authored idle on boarding.
class CfgOPVehicleActions
{
    class Jeep
    {
        class OPCargoWeapons
        {
            version = 2;
            class Seats
            {
                class FrontPassenger
                {
                    cargoIndex = 0;
                    weapon = "M16";
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
