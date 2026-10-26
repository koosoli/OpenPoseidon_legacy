// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest's supply lines, included by ConquestReferee.cpp inside its anonymous namespace.
//
// A side's HQ spends the side's pool (the towns' income goes there). Every other base it holds keeps its
// own stock: deploying a squad there costs that stock, and so does gear from its arsenal. A base's stock
// barely grows by itself (SupplyTrickle a minute) and is lost when the base changes hands, so supplies
// reach the front by truck: supply squads (role "supply": a crew in an ammunition truck) load at the HQ,
// drive to the base that needs them most, unload and come back. A truck destroyed on the way loses its
// load. Every side keeps one truck for every three bases (at most three), under the AI or a human
// commander, and the trucks run by themselves.

constexpr float SupplyLoad = 300.0f;     // what one truck carries
constexpr float SupplyReserve = 150.0f;  // the HQ keeps this much for itself when loading
constexpr float SupplyTrickle = 2.0f;    // a base's own supplies a minute
constexpr float SupplyArrive = 60.0f;    // metres from the post: unloaded
constexpr int MaxSupplyTrucks = 3;

bool IsSupply(const Referee& r, const GroupRecord& rec)
{
    if (rec.truckless)
        return false; // its truck is gone: the crew fights on foot
    return rec.supplyDuty ||
           (rec.type >= 0 && rec.type < static_cast<int>(r.config.squads.size()) && r.config.squads[rec.type].role == "supply");
}

// A base the commander picked for this truck (its tent's card, or a right click on the base): valid while
// the side holds it and it is not the HQ.
bool PinValid(const Referee& r, const GroupRecord& rec)
{
    const int o = rec.supplyPin;
    return o >= 0 && o < static_cast<int>(r.war.defs.size()) && r.war.states[o].owner == rec.side &&
           r.war.defs[o].kind != ObjectiveKind::HQ;
}

// The side's HQ it still holds (its first), or -1.
int HqOf(const Referee& r, int side)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.defs[o].kind == ObjectiveKind::HQ && r.war.states[o].owner == side)
            return static_cast<int>(o);
    return -1;
}

// What can be spent at base `o`: the side's pool at its HQ, the base's own stock elsewhere.
float& Budget(Referee& r, int side, int o)
{
    if (o < 0 || o >= static_cast<int>(r.war.defs.size()) || r.war.defs[o].kind == ObjectiveKind::HQ)
        return r.war.pool[side];
    return r.war.states[o].stock;
}

float BudgetAt(const Referee& r, int side, int o)
{
    if (o < 0 || o >= static_cast<int>(r.war.defs.size()) || r.war.defs[o].kind == ObjectiveKind::HQ)
        return r.war.pool[side];
    return r.war.states[o].stock;
}

int SupplyType(const Referee& r)
{
    for (size_t t = 0; t < r.config.squads.size(); t++)
        if (r.config.squads[t].role == "supply")
            return static_cast<int>(t);
    return -1;
}

// The base a loaded truck should go to: the one that needs it most -- little stock, near the front --
// and that no other truck of the side is already bringing supplies to.
int NeediestBase(const Referee& r, int side, int hq, const GroupRecord* self)
{
    int best = -1;
    float bestScore = 1.0e9f;
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const ObjectiveDef& d = r.war.defs[o];
        if (static_cast<int>(o) == hq || d.kind == ObjectiveKind::HQ || r.war.states[o].owner != side)
            continue;
        bool taken = false;
        for (const GroupRecord& other : r.groups)
            if (&other != self && other.side == side && other.supplyState == 1 && other.supplyTarget == static_cast<int>(o))
                taken = true;
        if (taken)
            continue;
        bool front = false;
        for (int n : d.neighbours)
            if (r.war.states[n].owner != side)
                front = true;
        const float far = hq >= 0 ? std::hypot(d.x - r.war.defs[hq].x, d.y - r.war.defs[hq].y) : 0.0f;
        const float score = r.war.states[o].stock + (front ? 0.0f : 200.0f) + far / 40.0f;
        if (score < bestScore)
        {
            bestScore = score;
            best = static_cast<int>(o);
        }
    }
    return best;
}

// A supply squad without its truck (destroyed, or stuck for good and left behind): the load is lost, and the
// crew joins the fight on foot rather than standing at the HQ for the rest of the war. The commander buys
// another truck (KeepSupplyTrucks counts only squads that still have one).
void LoseTruck(Referee& r, GroupRecord& rec, float now, bool abandoned)
{
    AIGroup* grp = rec.group;
    if (abandoned && grp)
    {
        std::vector<Transport*> trucks;
        for (int v = 0; v < grp->NVehicles(); v++)
            if (Transport* t = grp->GetVehicle(v))
                trucks.push_back(t);
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (AIUnit* unit = grp->UnitWithID(id); unit && !unit->IsFreeSoldier())
                unit->OrderGetIn(false);
        for (Transport* t : trucks)
            grp->UnassignVehicle(t); // leaveVehicle: see TrackGroups
        grp->GetInVehicles();
    }
    const std::string load = rec.supplyLoad > 0.0f
                                 ? " with " + std::to_string(static_cast<int>(rec.supplyLoad)) + " supplies"
                                 : std::string();
    WarLog(Name(rec.side) + " supply truck " + Callsign(rec.id) + (abandoned ? " stuck for good, left behind" : " lost") +
           load + ": the crew fights on foot");
    if (!load.empty())
        Radio(r, rec.side, Callsign(rec.id) + (abandoned ? ": the truck is stuck, we leave it" : " is down") +
                               ": the supplies are lost.");
    rec.supplyLoad = 0.0f;
    rec.supplyState = 0;
    rec.supplyTarget = -1;
    rec.supplyPin = -1;
    rec.supplyDuty = false;
    rec.truckless = true;
    rec.task = TaskKind::None;
    rec.objective = -1;
    rec.lockedUntil = -1.0e9f; // SupplyMove held it for the supply line; the planner has it now
    rec.holding = false;
    rec.onFoot = true;
    r.replan[rec.side] = true;
    (void)now;
}

void SupplyMove(GroupRecord& rec, const ObjectiveDef& d, float now)
{
    rec.task = TaskKind::Move;
    rec.objective = -1;
    rec.lockedUntil = 1.0e9f; // its own orders: the planner leaves supply squads alone
    OrderPoint(rec, Ground(d.x + 12.0f, d.y - 12.0f), now);
}

// Once a tick: the trickle, and every supply squad's run (0 at the HQ, 1 loaded and on the way, 2 back).
void StepSupply(Referee& r, float dt, float now)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.defs[o].kind != ObjectiveKind::HQ && r.war.states[o].owner != NoSide)
            r.war.states[o].stock += SupplyTrickle * dt / 60.0f;
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!IsSupply(r, rec) || !grp || !grp->Leader())
            continue;
        // The truck: lost (and its load with it) when it is destroyed.
        Transport* truck = nullptr;
        for (int v = 0; v < grp->NVehicles(); v++)
            if (Transport* t = grp->GetVehicle(v); t && !t->IsDammageDestroyed())
                truck = t;
        if (!truck)
        {
            LoseTruck(r, rec, now, false);
            continue;
        }
        const int hq = HqOf(r, rec.side);
        const Vector3 at = truck->Position();
        const auto near = [&](int o)
        { return o >= 0 && std::hypot(at.X() - r.war.defs[o].x, at.Z() - r.war.defs[o].y) < r.war.defs[o].radius; };
        if (rec.supplyState == 1 && PinValid(r, rec) && rec.supplyPin != rec.supplyTarget)
        {
            rec.supplyTarget = rec.supplyPin; // the commander sent it elsewhere
            SupplyMove(rec, r.war.defs[rec.supplyTarget], now);
        }
        if (rec.supplyState == 1)
        {
            const int o = rec.supplyTarget;
            if (o < 0 || r.war.states[o].owner != rec.side)
            {
                rec.supplyState = 2; // lost before it got there: back with the load
                if (hq >= 0)
                    SupplyMove(rec, r.war.defs[hq], now);
                continue;
            }
            if (std::hypot(at.X() - r.war.defs[o].x, at.Z() - r.war.defs[o].y) < SupplyArrive + 40.0f)
            {
                r.war.states[o].stock += rec.supplyLoad;
                WarLog(Name(rec.side) + " supply truck " + Callsign(rec.id) + " delivered " +
                       std::to_string(static_cast<int>(rec.supplyLoad)) + " to " + r.war.defs[o].name);
                Radio(r, rec.side, Callsign(rec.id) + ": supplies delivered to " + r.war.defs[o].name + ".");
                rec.supplyLoad = 0.0f;
                rec.supplyState = 2;
                if (rec.supplyPin == o)
                    rec.supplyPin = -1;
                if (hq >= 0)
                    SupplyMove(rec, r.war.defs[hq], now);
            }
            continue;
        }
        if (hq < 0)
            continue;
        if (rec.supplyState == 2 && !near(hq))
            continue; // on the way back
        rec.supplyState = 0;
        // At the HQ: load what it carries back first, else from the pool, and go where it is needed.
        if (rec.supplyLoad <= 0.0f)
        {
            const float load = std::min(SupplyLoad, r.war.pool[rec.side] - SupplyReserve);
            if (load < 50.0f)
                continue; // the HQ is short itself: wait
            const int target = PinValid(r, rec) ? rec.supplyPin : NeediestBase(r, rec.side, hq, &rec);
            if (target < 0)
                continue; // nothing to supply
            r.war.pool[rec.side] -= load;
            rec.supplyLoad = load;
            rec.supplyTarget = target;
        }
        else
            rec.supplyTarget = PinValid(r, rec) ? rec.supplyPin : NeediestBase(r, rec.side, hq, &rec);
        if (rec.supplyTarget < 0)
            continue;
        rec.supplyState = 1;
        SupplyMove(rec, r.war.defs[rec.supplyTarget], now);
        WarLog(Name(rec.side) + " supply truck " + Callsign(rec.id) + " takes " +
               std::to_string(static_cast<int>(rec.supplyLoad)) + " to " + r.war.defs[rec.supplyTarget].name);
    }
}

// An AI commander's supply line: one truck for every three bases it holds beyond the HQ, at most three.
bool KeepSupplyTrucks(Referee& r, int side, float now)
{
    const int type = SupplyType(r);
    const int hq = HqOf(r, side);
    if (type < 0 || hq < 0 || r.config.squads[type].classes[side].empty())
        return false;
    int bases = 0, trucks = 0;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.states[o].owner == side && r.war.defs[o].kind != ObjectiveKind::HQ)
            bases++;
    for (const GroupRecord& rec : r.groups)
        if (rec.side == side && IsSupply(r, rec))
            trucks++;
    const int want = std::min(MaxSupplyTrucks, (bases + 2) / 3);
    if (trucks >= want || r.war.pool[side] < r.config.squads[type].cost + SupplyReserve ||
        !SpawnEligible(r.war, side, hq, now, r.tuning))
        return false;
    if (!SpawnSquad(r, side, hq, false, type))
        return false;
    WarLog(Name(side) + " bought a supply truck at " + r.war.defs[hq].name + " (" + std::to_string(bases) + " bases to supply)");
    return true;
}

// The magazines an arsenal weapon fires: the first of each muzzle's magazines[] (an M16 with its grenade
// launcher brings both kinds; a launcher's own rockets, "this", carry its name). Empty for a weapon without
// ammunition (binoculars).
std::vector<std::string> WeaponMagazines(const std::string& weapon)
{
    std::vector<std::string> out;
    const ParamEntry* w = (Pars >> "CfgWeapons").FindEntry(weapon.c_str());
    if (!w || !w->IsClass())
        return out;
    const auto first = [&](const ParamEntry& e)
    {
        const ParamEntry* list = e.FindEntry("magazines");
        if (!list || !list->IsArray() || list->GetSize() == 0)
            return;
        const RStringB name = (*list)[0];
        // "this": the magazine named after the weapon (or muzzle) itself, as Weapons.cpp reads it.
        const std::string magazine = _stricmp(static_cast<const char*>(name), "this") == 0
                                         ? std::string(static_cast<const char*>(e.GetName()))
                                         : std::string(static_cast<const char*>(name));
        if (!magazine.empty() && std::find(out.begin(), out.end(), magazine) == out.end())
            out.push_back(magazine);
    };
    const ParamEntry* muzzles = w->FindEntry("muzzles");
    if (!muzzles || !muzzles->IsArray() || muzzles->GetSize() == 0)
    {
        first(*w);
        return out;
    }
    for (int i = 0; i < muzzles->GetSize(); i++)
    {
        const RStringB name = (*muzzles)[i];
        const ParamEntry* m = _stricmp(static_cast<const char*>(name), "this") == 0 ? w : w->FindEntry(name);
        if (m && m->IsClass())
            first(*m);
    }
    return out;
}
