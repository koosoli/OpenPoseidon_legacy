// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest's air support, included by ConquestReferee.cpp inside its anonymous namespace: the side's attack
// plane, flown in by an AI pilot, drops a stick of bombs on the point the commander called. See
// CONQUEST-PLAN.md (status 2026-10-26).

bool AirStrikeFor(Referee& r, int side, float x, float y, std::string* reason)
{
    const auto refuse = [&](const std::string& why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (!r.active || !r.started || side == NoSide)
        return refuse("no war");
    if (r.config.airBombs <= 0 || r.config.airPlane[side].empty())
        return refuse("this war has no air support");
    const float now = Now();
    if (now < r.lastAir[side] + r.config.airCooldown)
        return refuse("the plane is rearming");
    if (const float wait = SupportWait(r, now); wait > 0.0f)
        return refuse("support is ready " + std::to_string(static_cast<int>(wait)) + " s from now");
    for (const Strike& s : r.strikes)
        if (s.side == side)
            return refuse("a plane is already on its way");
    if (r.war.pool[side] < r.config.airCost)
        return refuse("not enough supplies");
    AICenter* center = GWorld->GetCenter(r.config.engineSide[side]);
    GameState* gs = GWorld->GetGameState();
    if (!center || !gs || center->NGroups() >= MaxGroups)
        return refuse("no pilot available");
    // It comes in over the side's own ground: from its nearest objective, through the target.
    float fx = x, fy = y - 1.0f, best = 1.0e9f;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.states[o].owner == side)
            if (const float d = std::hypot(r.war.defs[o].x - x, r.war.defs[o].y - y); d < best && d > 50.0f)
            {
                best = d;
                fx = r.war.defs[o].x;
                fy = r.war.defs[o].y;
            }
    const float len = std::max(1.0f, std::hypot(x - fx, y - fy));
    const float dx = (x - fx) / len, dy = (y - fy) / len;
    const float size = static_cast<float>(LandRange) * LandGrid;
    const float sx = std::clamp(x - dx * 3000.0f, 50.0f, size - 50.0f), sy = std::clamp(y - dy * 3000.0f, 50.0f, size - 50.0f);
    EntityAI* made = CreateVehicleAt(RString(r.config.airPlane[side].c_str()), sx, sy);
    Transport* plane = dyn_cast<Transport>(made);
    if (!plane)
    {
        if (made)
            DeleteVehicle(made);
        return refuse("no " + r.config.airPlane[side] + " in this game's vehicles");
    }
    Ref<AIGroup> group = new AIGroup();
    center->AddGroup(group);
    group->AddFirstWaypoint(Ground(sx, sy));
    Mission mis;
    mis._action = Mission::Arcade;
    center->SendMission(group, mis);
    GetNetworkManager().CreateObject(group);
    GameValue pos = gs->CreateGameValue(GameArray);
    static_cast<GameArrayType&>(pos).Add(GameValue(sx));
    static_cast<GameArrayType&>(pos).Add(GameValue(sy));
    GameValue args = gs->CreateGameValue(GameArray);
    static_cast<GameArrayType&>(args).Add(GameValue(RString(r.config.airPilot[side].c_str())));
    static_cast<GameArrayType&>(args).Add(pos);
    EntityAI* pilot = dyn_cast<EntityAI>(GetObject(A1UnitCreate(gs, GameValueExt(group.GetRef()), args)));
    if (!pilot)
    {
        group->RemoveFromCenter();
        DeleteVehicle(plane);
        return refuse("no pilot available");
    }
    group->AddVehicle(plane);
    // Airborne at 150 m, heading for the target at speed: the AI pilot flies it from there.
    char code[400];
    const float heading = std::atan2(dx, dy) * 57.29578f;
    std::snprintf(code, sizeof(code),
                  "_t moveInDriver _o; _o setPos [%.1f, %.1f, 150]; _o setDir %.1f; _o setVelocity [%.1f, %.1f, 0]; "
                  "_o flyInHeight 120; (group _t) setBehaviour \"CARELESS\"; (group _t) setCombatMode \"BLUE\"",
                  sx, sy, heading, dx * 90.0f, dy * 90.0f);
    GameVarSpace space;
    gs->BeginContext(&space);
    gs->VarSetLocal("_o", GameValueExt(static_cast<EntityAI*>(plane)), true, true);
    gs->VarSetLocal("_t", GameValueExt(pilot), true, true);
    gs->EvaluateMultiple(code);
    gs->EndContext();
    Strike strike;
    strike.side = side;
    strike.plane = plane;
    strike.group = group.GetRef();
    strike.x = x;
    strike.y = y;
    strike.startedAt = now;
    strike.exit = Ground(std::clamp(x + dx * 4000.0f, 50.0f, size - 50.0f), std::clamp(y + dy * 4000.0f, 50.0f, size - 50.0f));
    if (group->MainSubgroup())
        group->Move(group->MainSubgroup(), Ground(x + dx * 600.0f, y + dy * 600.0f), Command::Undefined);
    r.strikes.push_back(strike);
    r.war.pool[side] -= r.config.airCost;
    r.lastAir[side] = now;
    char where[64];
    std::snprintf(where, sizeof(where), "%.0f, %.0f", x, y);
    WarLog(Name(side) + " air strike on " + where + " (" + r.config.airPlane[side] + ")");
    Radio(r, side, "Air support inbound, " + std::to_string(r.config.airBombs) + " bombs. Time on target about forty seconds.");
    return true;
}

// The bombs: released over the target, each aimed at its own point of the stick.
void DropBombs(Referee& r, Strike& s)
{
    Transport* plane = s.plane;
    const AmmoType* ammo = dynamic_cast<const AmmoType*>(VehicleTypes.New(RString(r.config.airAmmo.c_str())));
    if (!ammo)
        ammo = dynamic_cast<const AmmoType*>(VehicleTypes.New(RString(r.config.barrageAmmo.c_str())));
    if (!plane || !ammo)
        return;
    const Vector3 from = plane->Position() - Vector3(0.0f, 4.0f, 0.0f);
    Vector3 track = plane->Speed();
    track[1] = 0.0f;
    if (track.SquareSize() < 1.0f)
        track = Vector3(s.x - from.X(), 0.0f, s.y - from.Z());
    track.Normalize();
    for (int i = 0; i < r.config.airBombs; i++)
    {
        const float along = (static_cast<float>(i) - 0.5f * static_cast<float>(r.config.airBombs - 1)) * r.config.airSpread;
        Vector3 aim = Ground(s.x + track.X() * along, s.y + track.Z() * along);
        const float speed = 120.0f;
        const float t = aim.Distance(from) / speed;
        aim[1] += 0.5f * 9.81f * t * t; // what gravity takes on the way down
        const Vector3 v = (aim - from).Normalized() * speed;
        Shot* shot = NewShot(plane, ammo, nullptr); // the plane's own bombs: hits are its (and its side's)
        if (!shot)
            continue;
        shot->SetOrient(v.Normalized(), VUp);
        shot->SetSpeed(v);
        shot->SetPosition(from + track * (static_cast<float>(i) * 1.5f));
        GWorld->AddFastVehicle(shot);
        if (GWorld->GetMode() == GModeNetware)
            GetNetworkManager().CreateVehicle(shot, VLTFast, "", -1);
    }
    s.dropped = true;
    s.droppedAt = Now();
    WarLog(Name(s.side) + " air strike: bombs away");
    Radio(r, s.side, "Bombs away.");
    if (AIGroup* grp = s.group; grp && grp->MainSubgroup())
        grp->Move(grp->MainSubgroup(), s.exit, Command::Undefined);
}

void ClearStrike(Strike& s)
{
    if (AIGroup* grp = s.group)
    {
        for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
            if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetPerson())
                DeleteVehicle(unit->GetPerson());
    }
    if (Transport* plane = s.plane)
        DeleteVehicle(plane);
}

// Every frame: release over the target, then fly off and vanish.
void StepStrikes(Referee& r, float now)
{
    for (size_t i = 0; i < r.strikes.size();)
    {
        Strike& s = r.strikes[i];
        Transport* plane = s.plane;
        // Only a plane that is still flying, with a living pilot, can drop: shot down (or its pilot
        // killed) before the target, the strike does not happen and the supplies are gone.
        const Person* pilot = plane ? plane->Driver() : nullptr;
        const bool flying = plane && !plane->IsDammageDestroyed() && pilot && !pilot->IsDammageDestroyed();
        if (!flying && (!s.dropped || !plane))
        {
            if (!s.dropped)
            {
                Radio(r, s.side, "Air support lost: the plane is down.");
                WarLog(Name(s.side) + " air strike lost: the plane was shot down before the target");
            }
            r.strikes.erase(r.strikes.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!s.dropped)
        {
            const float d = std::hypot(plane->Position().X() - s.x, plane->Position().Z() - s.y);
            // Over the target, or as close as this pass gets (the distance grows again within 400 m).
            if (d < 140.0f || (s.closest < 400.0f && d > s.closest + 20.0f))
                DropBombs(r, s);
            else if (now - s.startedAt > 120.0f)
            {
                Radio(r, s.side, "Air support could not find the target and turns back.");
                s.dropped = true;
                s.droppedAt = now;
            }
            s.closest = std::min(s.closest, d);
        }
        else if (now - s.droppedAt > 45.0f)
        {
            ClearStrike(s);
            r.strikes.erase(r.strikes.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

void ApplyAlliance(Referee& r);
