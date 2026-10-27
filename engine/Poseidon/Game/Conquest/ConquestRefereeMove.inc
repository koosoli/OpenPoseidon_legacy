// SPDX-License-Identifier: GPL-3.0-or-later
// Getting squads out of vehicles and unstuck (Dismount, InGroundVehicle, Detour), helicopters home, parking. Included by
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

// Someone of the squad is on his way into its vehicle (ordered in, assigned one, still on foot): boarding
// takes a while, and a squad doing it is busy, not stuck. Ordering it again restarted the boarding (Kolgujev:
// the tanks and the helicopter at EAST's HQ were "stuck" from the first minute, never having got in).
bool Boarding(AIGroup* grp)
{
    for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
        if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetLifeState() == AIUnit::LSAlive && unit->IsFreeSoldier() &&
                                               unit->VehicleAssigned() && unit->IsGetInOrdered() &&
                                               unit->VehicleAssigned()->IsAbleToMove())
            return true;
    return false;
}

// Boarding counts as progress for 3 minutes from when it began (re-orders do not restart that clock): a
// squad that cannot get in at all is stuck after all.
bool BoardingGrace(GroupRecord& rec, float now)
{
    if (!Boarding(rec.group))
    {
        rec.boardingSince = -1.0f;
        return false;
    }
    if (rec.boardingSince < 0.0f)
        rec.boardingSince = now;
    return now - rec.boardingSince < 180.0f;
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
    // Set down: still, low, near home. Its origin stands over the ground by the model's own height (a UH-60
    // 1.9 m, a Mi-17 4.4 m), so a helicopter that has not moved for 5 s under 8 m has landed.
    const float above = at.Y() - GLandscape->SurfaceYAboveWater(at.X(), at.Z());
    const int hq = HqOf(r, rec.side);
    const float toHq = hq >= 0 ? std::hypot(at.X() - r.war.defs[hq].x, at.Z() - r.war.defs[hq].y) : 1.0e9f;
    // On the ground near home, landed by its order or on its own: the AI pilot set the Mi-17 down at the
    // Airfield 200 m short of its slot and waited there for good (soak probe, 2026-10-26).
    // ... or hanging still over home for 20 s: over Goisse (Malden) the AI pilot would not set down at all and
    // hovered 47 m up for good.
    if (heli->Speed().Size() >= 1.0f)
        rec.bestAt = now; // moving: "still since" starts again
    const float still = now - rec.bestAt;
    if ((rec.landing || std::min(toGo, toHq) < 500.0f) && now - rec.orderedAt > 10.0f &&
        ((above < 8.0f && still > 5.0f) || still > 20.0f))
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

Vector3 ParkingSlot(Referee& r, int side, int o, bool big)
{
    if (r.parked.size() != r.war.defs.size())
        r.parked.assign(r.war.defs.size(), 0);
    float fx, fy;
    FrontDir(r, side, o, fx, fy);
    const ObjectiveDef& d = r.war.defs[o];
    const float baseHeight = GLandscape ? GLandscape->SurfaceY(d.x, d.y) : 0.0f;
    // A slot a vehicle can drive away from: dry land at least 3 m above the sea (a helicopter parked in the
    // surf behind Morton counted as unusable), level (no steeper than 0.15 over 12 m), and no more than 8 m
    // below or above the base -- behind Morton the ground drops as a cliff to the beach, and the trucks parked
    // at its foot never climbed out (soak, 2026-10-27). Nor one a vehicle still stands in: the slots used to be
    // handed out in turn, and a new truck appeared on top of one that had not left. Behind the base first, then
    // beside it; with nothing suitable, beside the base's spawn point as it used to.
    const auto suitable = [&](float x, float y)
    {
        if (!GLandscape)
            return true;
        const float h = GLandscape->SurfaceY(x, y);
        const float sx = GLandscape->SurfaceY(x + 6.0f, y) - GLandscape->SurfaceY(x - 6.0f, y);
        const float sy = GLandscape->SurfaceY(x, y + 6.0f) - GLandscape->SurfaceY(x, y - 6.0f);
        if (h < 3.0f || std::abs(h - baseHeight) > 8.0f || std::max(std::abs(sx), std::abs(sy)) > 0.15f * 12.0f)
            return false;
        const float clear = big ? 20.0f : 12.0f;
        return std::none_of(r.units.begin(), r.units.end(),
                            [&](const UnitRecord& u)
                            {
                                const EntityAI* v = u.unit;
                                return u.vehicle && v && !v->IsDammageDestroyed() &&
                                       std::hypot(v->Position().X() - x, v->Position().Z() - y) < clear;
                            });
    };
    // Behind the base (away from the front), then to its left and its right.
    const float dirs[3][2] = {{-fx, -fy}, {-fy, fx}, {fy, -fx}};
    for (const auto& dir : dirs)
    {
        const float bx = dir[0], by = dir[1];
        const float px = -by, py = bx; // across that direction
        for (int tries = 0; tries < 30; tries++)
        {
            const int slot = r.parked[o]++ % 30;
            const int row = slot / 6, col = slot % 6;
            const float lateral = (col % 2 ? 1.0f : -1.0f) * (static_cast<float>(col / 2) + 0.5f) * (big ? 45.0f : 28.0f);
            const float out = d.radius * 0.6f + 30.0f + static_cast<float>(row) * 35.0f;
            const float x = d.x + bx * out + px * lateral, y = d.y + by * out + py * lateral;
            if (suitable(x, y))
                return Ground(x, y);
        }
    }
    const Vector3 at = SpawnPoint(r, side, o);
    const float off = 14.0f * static_cast<float>(r.parked[o] % 6 + 1);
    return Ground(at.X() + off, at.Z() + off * 0.5f);
}
