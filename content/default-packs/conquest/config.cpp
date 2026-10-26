// The @OP_Conquest default content pack. Conquest's rules live in the engine (Game/Conquest) and its wars in
// mission description.ext files (class CfgConquest); this addon only names the pack, so the engine's default
// content resolver can mount it and its Missions folder.
class CfgPatches
{
    class op_conquest
    {
        units[] = {};
        weapons[] = {};
        requiredAddons[] = {};
    };
};
