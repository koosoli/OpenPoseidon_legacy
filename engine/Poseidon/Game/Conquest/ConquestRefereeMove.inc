// SPDX-License-Identifier: GPL-3.0-or-later
// Getting squads out of vehicles and unstuck: Dismount, InGroundVehicle, Detour. Included by
// ConquestReferee.cpp inside its anonymous namespace, before TrackGroups, which uses them.

// Out of the vehicles: everyone of a truck or APC squad; from a helicopter only the passengers, and the
// helicopter lands for them first. (Ordering its pilot out too left it hovering a metre up for ever:
// a pilot cannot get out of a flying helicopter, and the passengers waited for it to land.)
void Dismount(GroupRecord& rec, Transport* heli)
{
    AIGroup* grp = rec.group;
    if (!grp)
        return;
    if (heli && !rec.landing)
    {
        heli->LandStarted(Transport::LMGetOut);
        rec.landing = true;
    }
    bool mounted = false;
    for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
        if (AIUnit* unit = grp->UnitWithID(id); unit && !unit->IsFreeSoldier())
        {
            if (heli && unit->GetVehicleIn() == heli && heli->Driver() == unit->GetPerson())
                continue; // the pilot stays at the controls; door gunners and passengers get out
            unit->OrderGetIn(false);
            mounted = true;
        }
    if (mounted)
        grp->GetInVehicles();
}

// Someone of the squad sits in a car, truck, APC or tank (not a helicopter).
bool InGroundVehicle(AIGroup* grp)
{
    for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
        if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetVehicleIn() && !dynamic_cast<Helicopter*>(unit->GetVehicleIn()))
            return true;
    return false;
}

// A vehicle that has not moved on backs off: 30-75 m back and to one side of the way it was going, the
// side alternating. Ordering the same point again plans the same blocked path from the same spot; from
// somewhere else the driver's pathfinding gets a new start. The task's destination stays, and TrackGroups
// orders it again once the detour is driven (or after 40 s, if that is no better).
void Detour(GroupRecord& rec, float now)
{
    AIGroup* grp = rec.group;
    if (!grp || !grp->Leader() || !grp->MainSubgroup())
        return;
    const Vector3 p = grp->Leader()->Position();
    const float dx = rec.destination.X() - p.X(), dz = rec.destination.Z() - p.Z();
    const float d = std::max(1.0f, std::hypot(dx, dz));
    const float ux = dx / d, uz = dz / d;
    const float side = (rec.tries % 2) ? 1.0f : -1.0f;
    const float back = 20.0f + 10.0f * static_cast<float>(std::min(rec.tries, 3));
    const float aside = 25.0f + 15.0f * static_cast<float>(std::min(rec.tries, 3));
    const Vector3 go = Ground(p.X() - ux * back - uz * aside * side, p.Z() - uz * back + ux * aside * side);
    grp->Move(grp->MainSubgroup(), go, Command::Undefined);
    rec.detouring = true;
    rec.detourUntil = now + 40.0f;
    rec.tries++;
}
