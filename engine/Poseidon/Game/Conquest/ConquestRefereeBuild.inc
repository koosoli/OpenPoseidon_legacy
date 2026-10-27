// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest's production, included by ConquestReferee.cpp inside its anonymous namespace (owner, 2026-10-27:
// "a cool down period ... like a loading clock on the item icon", "a build mode system").
//
// Squads and a base's structures are built, not bought: an order joins its base's queue, and only the first
// in each base's queue is worked on. The price is paid a little at a time as the clock runs (Command &
// Conquer's way); when the base runs short of supplies -- or the side of men -- the clock stops, and it goes
// on by itself when there is enough again. A squad deploys when its clock is full and the base is quiet; a
// structure, once built, waits for the commander to click where it stands inside the base (the AI places its
// own). What a base can build depends on what stands there: soldiers need a barracks tent, vehicles a motor
// pool, aircraft a helipad. Every HQ starts with all three; any other base builds its own.

struct StructureDef
{
    const char* id;
    const char* name;
    const char* cls; // the CfgVehicles class put up
    float cost;
    float seconds;
    const char* unlocks;
};

constexpr StructureDef StructureDefs[] = {
    {"barracks", "Barracks tent", "Camp", 300.0f, 45.0f, "soldiers: rifle, recon, motorised, mechanised and air assault squads"},
    {"motorpool", "Motor pool", "Shed", 500.0f, 60.0f, "vehicles: motorised and mechanised squads, tanks"},
    {"helipad", "Helipad", "HeliH", 400.0f, 50.0f, "aircraft: air assault squads and gunships"},
};
constexpr int StructureCount = static_cast<int>(sizeof(StructureDefs) / sizeof(StructureDefs[0]));
constexpr int Barracks = 0, MotorPool = 1, Helipad = 2;
constexpr int BuildQueueMax = 5;           // orders a base holds, its clock's one included
constexpr float BuildSecondsPerSupply = 0.12f; // a rifle squad (160) 19 s, a tank (650) 78 s, a gunship (750) 90 s

// The structures squad type `def` needs at the base it is built at, as bits of StructureDefs.
unsigned Needs(const SquadDef& def)
{
    const std::string& role = def.role;
    if (role == "foot" || role == "recon")
        return 1u << Barracks;
    if (role == "motorised" || role == "mechanised")
        return (1u << Barracks) | (1u << MotorPool);
    if (role == "armour")
        return 1u << MotorPool;
    if (role == "air")
        return (1u << Barracks) | (1u << Helipad);
    if (role == "gunship")
        return 1u << Helipad;
    return 0; // supply trucks: loaded at the HQ
}

bool HasStructure(const Referee& r, int base, int kind)
{
    return std::any_of(r.structures.begin(), r.structures.end(),
                       [&](const Structure& s)
                       {
                           const EntityAI* o = s.object;
                           return s.base == base && s.kind == kind && o && !o->IsDammageDestroyed();
                       });
}

// "" when base `o` has what squad type `type` needs, else what it lacks ("a Barracks tent").
std::string MissingFor(const Referee& r, int o, int type)
{
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
        return std::string();
    const unsigned needs = Needs(r.config.squads[type]);
    std::string missing;
    for (int k = 0; k < StructureCount; k++)
        if ((needs & (1u << k)) && !HasStructure(r, o, k))
            missing += (missing.empty() ? "a " : " and a ") + std::string(StructureDefs[k].name);
    return missing;
}

float BuildSeconds(const Referee& r, const Build& b)
{
    if (b.structure)
        return StructureDefs[b.type].seconds;
    return std::max(8.0f, r.config.squads[b.type].cost * BuildSecondsPerSupply);
}

float BuildCost(const Referee& r, const Build& b)
{
    return b.structure ? StructureDefs[b.type].cost : r.config.squads[b.type].cost;
}

// Put a structure of `kind` up at (x, y) for base `o` (its kind's object, facing away from the front).
bool PutStructure(Referee& r, int o, int kind, float x, float y)
{
    EntityAI* object = CreateVehicleAt(RString(StructureDefs[kind].cls), x, y);
    if (!object)
    {
        LOG_WARN(Mission, "Conquest: the structure class {} could not be created", StructureDefs[kind].cls);
        return false;
    }
    r.structures.push_back({o, kind, object});
    return true;
}

// The AI's (and every HQ's first) place for a structure: a level, free spot by the base.
bool PlaceStructureAuto(Referee& r, int side, int o, int kind)
{
    const Vector3 at = ParkingSlot(r, side, o, true);
    return PutStructure(r, o, kind, at.X(), at.Z());
}

// Where the commander may put a structure for base `o`: inside the base, on dry, level ground, clear of
// other structures and vehicles. "" when it may, else why not.
std::string PlacementRefusal(const Referee& r, int o, float x, float y)
{
    const ObjectiveDef& d = r.war.defs[o];
    if (std::hypot(x - d.x, y - d.y) > d.radius + 40.0f)
        return "outside " + d.name + ": build inside the base";
    if (!GLandscape)
        return std::string();
    const float h = GLandscape->SurfaceY(x, y);
    const float sx = GLandscape->SurfaceY(x + 6.0f, y) - GLandscape->SurfaceY(x - 6.0f, y);
    const float sy = GLandscape->SurfaceY(x, y + 6.0f) - GLandscape->SurfaceY(x, y - 6.0f);
    if (h < 2.0f)
        return "in the water";
    if (std::max(std::abs(sx), std::abs(sy)) > 0.2f * 12.0f)
        return "too steep here";
    for (const Structure& s : r.structures)
        if (const EntityAI* other = s.object; other && std::hypot(other->Position().X() - x, other->Position().Z() - y) < 14.0f)
            return "another structure stands here";
    for (const UnitRecord& u : r.units)
        if (const EntityAI* v = u.unit; u.vehicle && v && !v->IsDammageDestroyed() &&
                                        std::hypot(v->Position().X() - x, v->Position().Z() - y) < 10.0f)
            return "a vehicle stands here";
    return std::string();
}

// "" when `side` may queue this at base `o` (supplies and men are not asked: the clock waits for them).
std::string QueueRefusal(const Referee& r, int side, int o, int type, bool structure)
{
    if (!r.active || side == NoSide || o < 0 || o >= static_cast<int>(r.war.defs.size()))
        return "no such base";
    if (r.war.states[o].owner != side)
        return "not our base";
    if (structure)
    {
        if (type < 0 || type >= StructureCount)
            return "no such structure";
        if (HasStructure(r, o, type))
            return std::string(StructureDefs[type].name) + " already stands at " + r.war.defs[o].name;
        for (const Build& b : r.builds)
            if (b.side == side && b.base == o && b.structure && b.type == type)
                return std::string(StructureDefs[type].name) + " is already being built here";
    }
    else
    {
        if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
            return "no such squad";
        const SquadDef& def = r.config.squads[type];
        if (Heavy(def) && r.war.defs[o].kind != ObjectiveKind::HQ && r.war.defs[o].kind != ObjectiveKind::Airfield)
            return "tanks and aircraft deploy at the HQ (or an airfield)";
        if (def.role == "supply" && r.war.defs[o].kind != ObjectiveKind::HQ)
            return "supply trucks are loaded at the HQ: build them there";
        if (const std::string missing = MissingFor(r, o, type); !missing.empty())
            return "needs " + missing + " at " + r.war.defs[o].name;
    }
    int queued = 0;
    for (const Build& b : r.builds)
        if (b.side == side && b.base == o)
            queued++;
    if (queued >= BuildQueueMax)
        return "the queue here is full (" + std::to_string(BuildQueueMax) + ")";
    return std::string();
}

std::string QueueBuild(Referee& r, int side, int o, int type, bool structure, bool human)
{
    std::string why = QueueRefusal(r, side, o, type, structure);
    if (!why.empty())
        return why;
    Build b;
    b.side = side;
    b.base = o;
    b.type = type;
    b.structure = structure;
    b.human = human;
    r.builds.push_back(b);
    WarLog(Name(side) + " builds a " + (structure ? std::string(StructureDefs[type].name) : r.config.squads[type].name) +
           " at " + r.war.defs[o].name + (human ? " (commander)" : ""));
    return std::string();
}

// Cancel the last queued order of this kind at base `o`; what it had paid goes back to the base.
bool CancelBuild(Referee& r, int side, int o, int type, bool structure)
{
    for (size_t i = r.builds.size(); i-- > 0;)
    {
        const Build& b = r.builds[i];
        if (b.side != side || b.base != o || b.type != type || b.structure != structure)
            continue;
        Budget(r, side, o) += b.paid;
        WarLog(Name(side) + " cancelled a " + (structure ? std::string(StructureDefs[type].name) : r.config.squads[type].name) +
               " at " + r.war.defs[o].name + " (" + std::to_string(static_cast<int>(b.paid)) + " back)");
        r.builds.erase(r.builds.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

// Is build `i` the first of its base's queue (the one the clock works on)?
bool BuildActive(const Referee& r, size_t i)
{
    for (size_t j = 0; j < i; j++)
        if (r.builds[j].side == r.builds[i].side && r.builds[j].base == r.builds[i].base)
            return false;
    return true;
}

int SpawnSquad(Referee& r, int side, int base, bool free, int type);

// Once a tick: every base's first order runs its clock.
void StepBuilds(Referee& r, float dt, float now)
{
    for (size_t i = 0; i < r.builds.size();)
    {
        Build& b = r.builds[i];
        const ObjectiveDef& d = r.war.defs[b.base];
        if (r.war.states[b.base].owner != b.side)
        {
            const std::string what = b.structure ? std::string(StructureDefs[b.type].name) : r.config.squads[b.type].name;
            Radio(r, b.side, "The " + what + " at " + d.name + " is lost with the base.");
            r.builds.erase(r.builds.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!BuildActive(r, i) || b.placing)
        {
            i++;
            continue;
        }
        const float cost = BuildCost(r, b);
        if (b.paid < cost)
        {
            // The clock: the price a little at a time; short of supplies (or men, for a squad), it waits.
            const float want = std::min(cost * dt / BuildSeconds(r, b), cost - b.paid);
            const int men = b.structure ? 0 : static_cast<int>(r.config.squads[b.type].classes[b.side].size());
            float& budget = Budget(r, b.side, b.base);
            if (!b.structure && r.live[b.side] + men > r.config.cap)
            {
                b.paused = true;
                b.why = "the side has all the men it may field";
            }
            else if (budget < want)
            {
                b.paused = true;
                b.why = d.kind == ObjectiveKind::HQ ? "waiting for supplies" : "waiting for supplies at " + d.name;
            }
            else
            {
                budget -= want;
                b.paid += want;
                b.paused = false;
                b.why.clear();
            }
            i++;
            continue;
        }
        if (b.structure)
        {
            // Built: the AI puts it up itself; the commander's waits for his click (PlaceStructure).
            if (!b.human || !Human(r, b.side))
            {
                PlaceStructureAuto(r, b.side, b.base, b.type);
                WarLog(Name(b.side) + " " + StructureDefs[b.type].name + " stands at " + d.name);
                r.builds.erase(r.builds.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            b.placing = true;
            b.paused = false;
            b.why.clear();
            Radio(r, b.side, std::string(StructureDefs[b.type].name) + " ready at " + d.name + ": choose where it stands.");
            i++;
            continue;
        }
        std::string why;
        if (!SpawnEligible(r.war, b.side, b.base, now, r.tuning, &why))
        {
            b.paused = true;
            b.why = "ready, waiting: " + why;
            i++;
            continue;
        }
        const Build done = b;
        r.builds.erase(r.builds.begin() + static_cast<std::ptrdiff_t>(i));
        const int id = SpawnSquad(r, done.side, done.base, true, done.type);
        if (!id)
            continue;
        r.bought[done.side]++;
        if (done.human)
            for (GroupRecord& rec : r.groups)
                if (rec.id == id)
                {
                    // A squad the commander built waits where it deployed for his orders, like a factory's
                    // output -- for two minutes; then, still without an order, the AI puts it to work.
                    rec.task = TaskKind::None;
                    rec.objective = -1;
                    rec.lockedUntil = now + BoughtWaitSeconds;
                }
        WarLog(Name(done.side) + " " + r.config.squads[done.type].name + " ready at " + d.name + ": " + Callsign(id));
        if (done.human)
            Radio(r, done.side, Callsign(id) + ", " + r.config.squads[done.type].name + ", ready at " + d.name + ".");
    }
}

// The commander's click for a built structure. "" when it stands, else why not.
std::string PlaceBuilt(Referee& r, int side, int o, int kind, float x, float y)
{
    for (size_t i = 0; i < r.builds.size(); i++)
    {
        Build& b = r.builds[i];
        if (b.side != side || b.base != o || !b.structure || b.type != kind || !b.placing)
            continue;
        if (const std::string why = PlacementRefusal(r, o, x, y); !why.empty())
            return why;
        if (!PutStructure(r, o, kind, x, y))
            return "it could not be put up";
        WarLog(Name(side) + " " + StructureDefs[kind].name + " stands at " + r.war.defs[o].name + " (commander)");
        r.builds.erase(r.builds.begin() + static_cast<std::ptrdiff_t>(i));
        return std::string();
    }
    return "nothing is ready to stand here";
}

// The war begins: every HQ has its barracks, motor pool and helipad.
void BuildHqStructures(Referee& r)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
        if (r.war.defs[o].kind == ObjectiveKind::HQ && r.war.states[o].owner != NoSide)
            for (int k = 0; k < StructureCount; k++)
                if (!HasStructure(r, static_cast<int>(o), k))
                    PlaceStructureAuto(r, r.war.states[o].owner, static_cast<int>(o), k);
}

// An AI commander builds a barracks at a front base that has none, once that base holds the supplies.
void AiBuildStructures(Referee& r, int side)
{
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const int base = static_cast<int>(o);
        if (r.war.states[o].owner != side || r.war.defs[o].kind == ObjectiveKind::HQ || HasStructure(r, base, Barracks))
            continue;
        bool front = false;
        for (int n : r.war.defs[o].neighbours)
            front |= r.war.states[n].owner != side;
        if (!front || BudgetAt(r, side, base) < StructureDefs[Barracks].cost)
            continue;
        if (std::any_of(r.builds.begin(), r.builds.end(), [&](const Build& b) { return b.side == side && b.base == base; }))
            continue;
        QueueBuild(r, side, base, Barracks, true, false);
    }
}

// Is anything of `side` in production at base `o`?
bool BaseBusy(const Referee& r, int side, int o)
{
    return std::any_of(r.builds.begin(), r.builds.end(), [&](const Build& b) { return b.side == side && b.base == o; });
}

// The production queues and the structures in a saved game.
LSError SerializeBuilds(ParamArchive& ar, std::vector<Build>& builds, std::vector<Structure>& structures)
{
    int count = static_cast<int>(builds.size());
    PARAM_CHECK(ar.Serialize("builds", count, 1, 0))
    builds.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t i = 0; i < builds.size(); i++)
    {
        ParamArchive sub;
        if (!ar.OpenSubclass(RStringB(("build" + std::to_string(i)).c_str()), sub))
            continue;
        Build& b = builds[i];
        RString why(b.why.c_str());
        PARAM_CHECK(sub.Serialize("side", b.side, 1, NoSide))
        PARAM_CHECK(sub.Serialize("base", b.base, 1, -1))
        PARAM_CHECK(sub.Serialize("type", b.type, 1, -1))
        PARAM_CHECK(sub.Serialize("structure", b.structure, 1, false))
        PARAM_CHECK(sub.Serialize("human", b.human, 1, false))
        PARAM_CHECK(sub.Serialize("paid", b.paid, 1, 0.0f))
        PARAM_CHECK(sub.Serialize("paused", b.paused, 1, false))
        PARAM_CHECK(sub.Serialize("placing", b.placing, 1, false))
        PARAM_CHECK(sub.Serialize("why", why, 1, RString()))
        b.why = static_cast<const char*>(why);
        ar.CloseSubclass(sub);
    }
    count = static_cast<int>(structures.size());
    PARAM_CHECK(ar.Serialize("structures", count, 1, 0))
    structures.resize(static_cast<size_t>(std::max(count, 0)));
    for (size_t i = 0; i < structures.size(); i++)
    {
        ParamArchive sub;
        if (!ar.OpenSubclass(RStringB(("structure" + std::to_string(i)).c_str()), sub))
            continue;
        Structure& st = structures[i];
        PARAM_CHECK(sub.Serialize("base", st.base, 1, -1))
        PARAM_CHECK(sub.Serialize("kind", st.kind, 1, -1))
        PARAM_CHECK(sub.SerializeRef("object", st.object, 1))
        ar.CloseSubclass(sub);
    }
    return LSOK;
}
