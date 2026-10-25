// A group created while the world runs (createGroup + createUnit) must obey group orders.
//
// Broken state (2026-10-25, found by Conquest): AIGroup::Leader() falls back to any unit
// when no leader is elected, so createUnit's "if (!group->Leader()) SelectLeader" never
// elected one. The main subgroup's planner (_whoAmI) stayed null, GoPlanned had nobody to
// hand the destination to, and both `move` and waypoints were silently dropped. Per-unit
// doMove still worked, which hid it. Fixed with AIGroup/AISubgroup::HasLeader().
//
// Coordinates are the dry, level 00training.abel placements ai_side_relations uses.

triSimFrames 20

createCenter west

_moveGroup = createGroup west
"SoldierWB" createUnit [[9782.3, 4024.0, 40.26], _moveGroup, "", 1, "PRIVATE"]
"SoldierWB" createUnit [[9785.3, 4024.0, 40.26], _moveGroup, "", 1, "PRIVATE"]

_wpGroup = createGroup west
"SoldierWB" createUnit [[9788.3, 4030.0, 40.26], _wpGroup, "", 1, "PRIVATE"]
"SoldierWB" createUnit [[9791.3, 4030.0, 40.26], _wpGroup, "", 1, "PRIVATE"]

triSimFrames 60

_start1 = getPos leader _moveGroup
_start2 = getPos leader _wpGroup

// 1. `group move position`: the commander's order.
_moveGroup move [9758.0, 3990.0]

// 2. A waypoint added at runtime: what mission scripts do.
_wp = _wpGroup addWaypoint [[9760.0, 3995.0], 0]
[_wpGroup, 1] setWaypointType "MOVE"

triSimFrames 3000

_end1 = getPos leader _moveGroup
_end2 = getPos leader _wpGroup
_moved1 = sqrt (((_end1 select 0) - (_start1 select 0)) ^ 2 + ((_end1 select 1) - (_start1 select 1)) ^ 2)
_moved2 = sqrt (((_end2 select 0) - (_start2 select 0)) ^ 2 + ((_end2 select 1) - (_start2 select 1)) ^ 2)

triAssertGt [_moved1, 20]
triAssertGt [_moved2, 20]

triEndTest
