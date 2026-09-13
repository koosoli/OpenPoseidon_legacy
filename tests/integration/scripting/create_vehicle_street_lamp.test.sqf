// Regression for upstream PR #310: script creation resolves the type's shape.
triSimUntil { time >= 2 }
_p = getpos player
opLampWood = "StreetLampWood" createVehicle [(_p select 0) + 5, (_p select 1) + 5, 0]
opLampMetal = "StreetLampMetal" createVehicle [(_p select 0) - 5, (_p select 1) + 5, 0]
opLampAbstract = "StreetLamp" createVehicle [(_p select 0), (_p select 1) + 8, 0]
triWaitFrames 8
if (isNull opLampWood) exitWith { "FAIL:StreetLampWood returned objNull" }
if (isNull opLampMetal) exitWith { "FAIL:StreetLampMetal returned objNull" }
if (!(isNull opLampAbstract)) exitWith { "FAIL:abstract StreetLamp was not refused" }
deleteVehicle opLampWood
deleteVehicle opLampMetal
triWaitFrames 8
if (!(isNull opLampWood)) exitWith { "FAIL:wood lamp deletion failed" }
if (!(isNull opLampMetal)) exitWith { "FAIL:metal lamp deletion failed" }
triEndTest
