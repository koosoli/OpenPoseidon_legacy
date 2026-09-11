class CfgPatches
{
    class op_jeep_user_priority
    {
        units[] = {};
        weapons[] = {};
        requiredAddons[] = {"op_jeep_actions"};
    };
};
// Test only: move the explicit seat to a different cargo index. Index 0 must
// use its original animation and have no personal-weapon capability.
class CfgOPVehicleActions
{
    class Jeep
    {
        class OPCargoWeapons
        {
            class Seats
            {
                class FrontPassenger
                {
                    cargoIndex = 2;
                };
            };
        };
    };
};
