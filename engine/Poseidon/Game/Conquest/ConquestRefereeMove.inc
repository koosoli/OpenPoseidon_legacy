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

// An AI side's air assault helicopter, its passengers out, does not stay by the town (owner, 2026-10-26: a
// helicopter never holds a place; parked there it only waited to be shot): its pilot leaves the squad with
// it and flies it back to the HQ. False when that cannot be done (no HQ, the pilot not the squad's): it then
// sets down where it is, as before. `ferry` is the pilot's new record, added by the caller after its loop.
bool SendHelicopterHome(Referee& r, GroupRecord& rec, Transport* heli, float now, GroupRecord& ferry)
{
    AIGroup* grp = rec.group;
    Person* pilot = heli ? heli->Driver() : nullptr;
    AIUnit* brain = pilot ? pilot->Brain() : nullptr;
    const int hq = HqOf(r, rec.side);
    // The pilot may lead the squad (he is made first): leaving, the group picks its next leader (UnitRemoved).
    if (!grp || !brain || brain->GetGroup() != grp || hq < 0 || grp->NUnits() < 2)
        return false;
    grp->UnassignVehicle(heli); // the squad goes on without it (leaveVehicle)
    RunWith("[_o] join grpNull", GameValueExt(pilot), GameValue(), RString());
    AIGroup* own = brain->GetGroup();
    if (!own || own == grp || !own->MainSubgroup())
        return false;
    own->AddVehicle(heli);
    brain->AssignAsDriver(heli);
    ferry = GroupRecord();
    ferry.id = r.nextGroupId++;
    ferry.side = rec.side;
    ferry.type = rec.type;
    ferry.group = own;
    ferry.ferry = true;
    ferry.task = TaskKind::Move;
    ferry.lockedUntil = 1.0e9f; // its own orders: the planner leaves it alone
    ferry.destination = ParkingSlot(r, rec.side, hq, true);
    ferry.orderedAt = now;
    ferry.bestAt = now;
    own->Move(own->MainSubgroup(), ferry.destination, Command::Undefined);
    rec.landing = false;
    rec.parkedHeli = false;
    if (rec.objective >= 0 && rec.objective < static_cast<int>(r.war.defs.size()) && grp->Leader())
        Order(rec, r.war.defs[rec.objective], now); // under its new leader, on foot
    WarLog(Name(rec.side) + " group " + std::to_string(rec.id) + " unloaded; its helicopter flies back to " +
           r.war.defs[hq].name);
    return true;
}

// The helicopter flying home: it lands at the HQ, and there it is stood down -- pilot and helicopter leave
// the war, and part of the squad's price comes back to the side's supplies (no idle pilot, no parked
// helicopter: the next air assault brings its own).
void TrackFerry(Referee& r, GroupRecord& rec, float now)
{
    AIGroup* grp = rec.group;
    Transport* heli = nullptr;
    for (int v = 0; v < grp->NVehicles(); v++)
        if (Transport* t = grp->GetVehicle(v); t && !t->IsDammageDestroyed())
            heli = t;
    if (!heli)
        return; // shot down: the pilot, if he got out, waits for the war's end where he is
    const Vector3 at = heli->Position();
    const float toGo = std::hypot(at.X() - rec.destination.X(), at.Z() - rec.destination.Z());
    if (!rec.landing && toGo < 120.0f)
    {
        heli->LandStarted(Transport::LMLand);
        rec.landing = true;
    }
    // Set down: its origin stands about 1.9 m over the ground (the feature test's "landed" reads 1.88).
    const float above = at.Y() - GLandscape->SurfaceYAboveWater(at.X(), at.Z());
    if (rec.landing && above < 3.0f && heli->Speed().Size() < 1.5f)
    {
        float refund = 0.0f;
        if (rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size()))
            refund = std::round(r.config.squads[rec.type].cost * 0.3f);
        r.war.pool[rec.side] += refund;
        WarLog(Name(rec.side) + " helicopter back at the HQ, stood down: " + std::to_string(static_cast<int>(refund)) +
               " supplies returned");
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetPerson())
                RunWith("deleteVehicle _o", GameValueExt(unit->GetPerson()), GameValue(), RString());
        RunWith("deleteVehicle _o", GameValueExt(heli), GameValue(), RString());
        rec.ferry = false; // done; the empty group goes next tick
        return;
    }
    if (!rec.landing && now - rec.orderedAt > 30.0f && grp->MainSubgroup() && !grp->MainSubgroup()->HasCommand())
    {
        grp->Move(grp->MainSubgroup(), rec.destination, Command::Undefined);
        rec.orderedAt = now;
    }
}
