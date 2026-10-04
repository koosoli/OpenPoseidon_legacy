// ugtest (Claude for Dec, job ug1, 2 Oct 2026): test basement for the terrainHole1 engine patch.
// The model's memory LOD selections "terrain_hole1" (the cellar with its walls) and "terrain_hole2" (the stairwell)
// cut the terrain away; its roadway (floor, stair, roof, top landing) is the ground there.
class CfgPatches
{
	class ugtest
	{
		units[] = {"UgBasement"};
		weapons[] = {};
		requiredVersion = 1.3;
	};
};
class CfgVehicles
{
	class All {};
	class Static: All {};
	class Building: Static {};
	class NonStrategic: Building {};
	class House: NonStrategic {};
	class UgBasement: House
	{
		scope = 2;
		model = "\ugtest\ugbasement";
		displayName = "Test basement (terrain hole)";
		vehicleClass = "Objects";
	};
};
