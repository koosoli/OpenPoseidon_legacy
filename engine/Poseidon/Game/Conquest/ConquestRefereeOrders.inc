// SPDX-License-Identifier: GPL-3.0-or-later
// The commander's contextual orders for selected squads (the command bar: engage a target, hold fire, stop,
// board, get out / land and unload, land, to base, supply runs, fight). Included by ConquestReferee.cpp inside
// its anonymous namespace.

// The building or vehicle nearest (x, y), within 30 m, that is not the side's own; nullptr if none.
EntityAI* TargetNear(const Referee& r, int side, float x, float y)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return nullptr;
    GameValue pos = gs->CreateGameValue(GameArray);
    static_cast<GameArrayType&>(pos).Add(GameValue(x));
    static_cast<GameArrayType&>(pos).Add(GameValue(y));
    const GameValue near = RunWith("_o nearObjects 30", pos, GameValue());
    if (near.GetType() != GameArray)
        return nullptr;
    const GameArrayType& list = near;
    EntityAI* best = nullptr;
    float bestDist = 1.0e9f;
    for (int i = 0; i < list.Size(); i++)
    {
        EntityAI* e = dyn_cast<EntityAI>(GetObject(list[i]));
        if (!e || dynamic_cast<Person*>(e) || e->IsDammageDestroyed() || dynamic_cast<Shot*>(e))
            continue;
        if (!dynamic_cast<Building*>(e) && !dynamic_cast<Transport*>(e))
            continue; // houses and vehicles: not trees, flags or crates
        if (e->GetTargetSide() == r.config.engineSide[side])
            continue;
        bool post = false; // the war's own furniture: command post tents, flags, arsenal boxes
        for (const Post& p : r.posts)
            if (e == p.tent.GetLink() || e == p.flag.GetLink() || e == p.arsenal.GetLink())
                post = true;
        if (post)
            continue;
        const float d = std::hypot(e->Position().X() - x, e->Position().Z() - y);
        if (d < bestDist)
        {
            bestDist = d;
            best = e;
        }
    }
    return best;
}

std::string EngageFor(Referee& r, int side, const std::vector<int>& ids, float x, float y)
{
    EntityAI* target = TargetNear(r, side, x, y);
    if (!target)
        return std::string();
    const float now = Now();
    int n = 0;
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !grp || !grp->Leader() || !grp->MainSubgroup() ||
            std::find(ids.begin(), ids.end(), rec.id) == ids.end())
            continue;
        if (grp->Leader()->GetPerson() && grp->Leader()->GetPerson() != GWorld->GetRealPlayer() &&
            IsHumanPlayer(grp->Leader()->GetPerson()))
            continue; // a player's squad: he decides
        RunWith("_o setCombatMode \"RED\"; _o setBehaviour \"COMBAT\"", GameValueExt(grp), GameValue());
        grp->AddTarget(target, 1.0f, 1.0f, 0.0f); // they know it is there and what it is
        grp->Attack(grp->MainSubgroup(), target, Command::Undefined);
        rec.engageName = static_cast<const char*>(target->GetDisplayName());
        rec.task = TaskKind::None; // no objective: the attack is the order; afterwards they wait
        rec.objective = -1;
        rec.holding = true;
        rec.destination = target->Position();
        rec.orderedAt = now;
        rec.lockedUntil = now + HumanLockSeconds;
        n++;
    }
    if (!n)
        return std::string();
    const std::string what = static_cast<const char*>(target->GetDisplayName());
    WarLog("commander: " + std::to_string(n) + " squad(s) engage " + (what.empty() ? std::string("a target") : what));
    for (const GroupRecord& rec : r.groups)
        if (rec.id == ids.front())
            VoiceConfirm(rec, true);
    return what.empty() ? std::string("the target") : what;
}

int CommandSquadsFor(Referee& r, int side, const std::vector<int>& ids, SquadCommand command)
{
    int n = 0;
    const float now = Now();
    for (GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (rec.side != side || !grp || !grp->Leader() || std::find(ids.begin(), ids.end(), rec.id) == ids.end())
            continue;
        if (grp->Leader()->GetPerson() && grp->Leader()->GetPerson() != GWorld->GetRealPlayer() &&
            IsHumanPlayer(grp->Leader()->GetPerson()))
            continue; // a player's squad: he decides
        Transport* heli = nullptr;
        for (int v = 0; v < grp->NVehicles(); v++)
            if (Transport* t = grp->GetVehicle(v); t && dynamic_cast<Helicopter*>(t) && !t->IsDammageDestroyed())
                heli = t;
        switch (command)
        {
            case SquadCommand::Engage:
                RunWith("_o setCombatMode \"RED\"; _o setBehaviour \"COMBAT\"", GameValueExt(grp), GameValue());
                break;
            case SquadCommand::HoldFire:
                RunWith("_o setCombatMode \"GREEN\"; _o setBehaviour \"AWARE\"", GameValueExt(grp), GameValue());
                break;
            case SquadCommand::Stop:
                rec.task = TaskKind::None;
                rec.objective = -1;
                rec.holding = false; // TrackGroups tells it to stand where it is
                rec.lockedUntil = now + HumanLockSeconds;
                break;
            case SquadCommand::Board:
                for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
                    if (AIUnit* unit = grp->UnitWithID(id); unit && unit->IsFreeSoldier())
                        unit->OrderGetIn(true);
                grp->GetInVehicles();
                rec.landing = false;
                break;
            case SquadCommand::GetOut:
                Dismount(rec, heli);
                break;
            case SquadCommand::Land:
                if (heli)
                    heli->LandStarted(Transport::LMLand);
                break;
            case SquadCommand::SupplyRuns:
                if (IsSupply(r, rec) || grp->NVehicles() == 0 || heli)
                    continue; // needs a ground vehicle of its own
                rec.supplyDuty = true;
                rec.supplyState = 2; // first back to the HQ to load
                rec.supplyLoad = 0.0f;
                if (const int hq = HqOf(r, side); hq >= 0)
                    SupplyMove(rec, r.war.defs[hq], now);
                break;
            case SquadCommand::Fight:
                if (!rec.supplyDuty)
                    continue;
                rec.supplyDuty = false;
                if (rec.supplyLoad > 0.0f)
                    r.war.pool[side] += rec.supplyLoad; // what it carried goes back to the HQ's books
                rec.supplyLoad = 0.0f;
                rec.task = TaskKind::None;
                rec.lockedUntil = now; // the planner gives it a task
                break;
            case SquadCommand::ToBase:
            {
                int best = -1;
                float bestDist = 1.0e9f;
                for (size_t o = 0; o < r.war.defs.size(); o++)
                    if (r.war.states[o].owner == side)
                        if (const float d = Dist(grp->Leader()->Position(), r.war.defs[o]); d < bestDist)
                        {
                            bestDist = d;
                            best = static_cast<int>(o);
                        }
                if (best < 0)
                    continue;
                rec.task = TaskKind::Defend;
                rec.objective = best;
                rec.lockedUntil = now + HumanLockSeconds;
                Order(rec, r.war.defs[best], now);
                break;
            }
        }
        n++;
        WarLog("commander: " + Callsign(rec.id) + " " + SquadCommandName(command));
    }
    if (n)
        for (const GroupRecord& rec : r.groups)
            if (rec.id == ids.front())
                VoiceConfirm(rec, command == SquadCommand::Engage);
    return n;
}
