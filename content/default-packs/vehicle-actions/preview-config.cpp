class CfgPatches
{
    class op_jeep_animation_preview
    {
        units[] = {};
        weapons[] = {};
        requiredAddons[] = {};
    };
};
class CfgMovesMC
{
    class States
    {
        class JeepCoDriver;
        class OPJeepIdle: JeepCoDriver
        {
            file = "\op_jeep_animation_preview\jeep_idle.rtm";
            speed = 1;
            disableWeapons = 1;
            variantsAI[] = {};
            variantsPlayer[] = {};
            equivalentTo = "";
            connectTo[] = {};
            interpolateWith[] = {};
        };
        class OPJeepRaise: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_raise.rtm"; speed = 1.4; };
        class OPJeepAim: OPJeepIdle
        {
            file = "\op_jeep_animation_preview\jeep_aim.rtm";
            disableWeapons = 0;
        };
        class OPJeepRecoil: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_recoil.rtm"; speed = 4; };
        class OPJeepReload: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_reload.rtm"; speed = 0.4; };
        class OPJeepLower: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_lower.rtm"; speed = 1.4; };
    };
};
// Development-only explicit integration seat. No other Jeep cargo or vehicle
// is changed, and the runtime currently requires the authored aim state.
class CfgVehicles
{
    class Car;
    class Jeep: Car
    {
        class OPCargoWeapons
        {
            version = 1;
            class Seats
            {
                class FrontPassenger
                {
                    cargoIndex = 0;
                    weapon = "M16";
                    action = "OPJeepAim";
                    azimuth = 90;
                    halfAngle = 25;
                    minElevation = -15;
                    maxElevation = 20;
                };
            };
        };
    };
};
