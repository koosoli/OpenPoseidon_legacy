// ugcave (Claude for Dec, 2 Oct 2026): large test cave for the terrain hole patches (terrainHole1-7).
// One model, 4 terrain-hole areas: a 24 x 18 m chamber 5 m under the ground (pillars, boulders, a ledge), an open
// stairwell up to the north, and a 2.4 m tunnel (south, then east) that ends in an open ramp up to the east.
class CfgPatches
{
	class ugcave
	{
		units[] = {"UgCave"};
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
	class UgCave: House
	{
		scope = 2;
		model = "\ugcave\ugcave";
		displayName = "Test cave (terrain hole)";
		vehicleClass = "Objects";
	};
};
