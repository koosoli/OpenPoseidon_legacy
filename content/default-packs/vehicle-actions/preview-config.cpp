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
        class OPJeepAim: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_aim.rtm"; };
        class OPJeepRecoil: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_recoil.rtm"; speed = 4; };
        class OPJeepReload: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_reload.rtm"; speed = 0.4; };
        class OPJeepLower: OPJeepIdle { file = "\op_jeep_animation_preview\jeep_lower.rtm"; speed = 1.4; };
    };
};
