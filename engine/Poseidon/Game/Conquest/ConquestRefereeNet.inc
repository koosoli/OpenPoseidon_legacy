// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest in multiplayer, included by ConquestReferee.cpp inside its anonymous namespace.
//
// The server runs the war: spawning, planning, capturing, the economy. Clients mirror it. Once a
// second the server publishes the war as the global `conquestNet` (publicVariable; groups and
// objects travel as network references, so a client resolves them to its own copies) and every
// client rebuilds its referee from it: the soldier's HUD, the commander view, the map markers and
// the shroud then read the mirror exactly as they read the war in single player. What a client's
// player does in the commander view (orders, purchases, barrages, holding command of the side)
// goes to the server as `[player, kind, args] remoteExec ["conquestRequest", 2]`, and the server
// acts for the side of the unit that asked.
//
// Multiplayer leaves out taking control of other soldiers: a player is their own slot and the
// mission's respawn brings them back.

constexpr float NetPublishSeconds = 1.0f;
constexpr float CommandHoldSeconds = 30.0f; // a client's "I command this side", renewed every 10 s
constexpr size_t NetRadioLines = 16;
constexpr size_t NetLogLines = 30;
constexpr size_t NetContactsPerSide = 80;

bool InMultiplayer()
{
    return GWorld && GWorld->GetMode() == GModeNetware;
}

// The server, or single player: the machine that runs the war.
bool IsAuthority()
{
    return !InMultiplayer() || GetNetworkManager().IsServer();
}

bool IsMirror()
{
    return !IsAuthority();
}

int SideOfUnit(const Referee& r, const EntityAI* unit)
{
    if (!unit)
        return NoSide;
    const TargetSide engine = unit->GetTargetSide();
    for (int s = 0; s < r.war.sideCount; s++)
        if (r.config.engineSide[s] == engine)
            return s;
    return NoSide;
}

int LocalSide(const Referee& r)
{
    return GWorld ? SideOfUnit(r, GWorld->GetRealPlayer()) : NoSide;
}

GameValue NetArray()
{
    return GWorld->GetGameState()->CreateGameValue(GameArray);
}

float Num(const GameArrayType& a, int i, float fallback = 0.0f)
{
    return i < a.Size() && a[i].GetType() == GameScalar ? static_cast<float>(a[i]) : fallback;
}

bool Flag(const GameArrayType& a, int i)
{
    return i < a.Size() && a[i].GetType() == GameBool && static_cast<bool>(a[i]);
}

std::string Str(const GameArrayType& a, int i)
{
    if (i >= a.Size() || a[i].GetType() != GameString)
        return std::string();
    const GameStringType value = a[i];
    return std::string(static_cast<const char*>(value));
}

// ---- the server: what it publishes ----

void Publish(Referee& r, float now)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return;
    GameValue root = NetArray();
    GameArrayType& a = root;
    a.Add(GameValue(static_cast<float>(++r.netSeq)));
    a.Add(GameValue(now));
    a.Add(GameValue(r.started));
    a.Add(GameValue(r.ended));

    GameValue objectives = NetArray();
    GameArrayType& oa = objectives;
    for (size_t o = 0; o < r.war.defs.size(); o++)
    {
        const ObjectiveState& s = r.war.states[o];
        GameValue e = NetArray();
        GameArrayType& ea = e;
        ea.Add(GameValue(static_cast<float>(s.owner)));
        ea.Add(GameValue(static_cast<float>(s.holder)));
        ea.Add(GameValue(s.level));
        ea.Add(GameValue(s.contested));
        ea.Add(GameValue(r.war.defs[o].x));
        ea.Add(GameValue(r.war.defs[o].y));
        ea.Add(GameValueExt(static_cast<Object*>(r.posts[o].flag.GetLink())));
        // Why each side may not deploy here now ("" when it may): the client's deploy panel.
        GameValue why = NetArray();
        GameArrayType& wa = why;
        for (int side = 0; side < r.war.sideCount; side++)
        {
            std::string reason;
            SpawnEligible(r.war, side, static_cast<int>(o), now, r.tuning, &reason);
            wa.Add(GameValue(RString(reason.c_str())));
        }
        ea.Add(why);
        oa.Add(e);
    }
    a.Add(objectives);

    GameValue sides = NetArray();
    GameArrayType& sa = sides;
    for (int s = 0; s < r.war.sideCount; s++)
    {
        GameValue e = NetArray();
        GameArrayType& ea = e;
        ea.Add(GameValue(r.war.pool[s]));
        ea.Add(GameValue(r.war.score[s]));
        ea.Add(GameValue(r.war.defeated[s]));
        ea.Add(GameValue(static_cast<float>(r.live[s])));
        ea.Add(GameValue(static_cast<float>(r.losses[s])));
        ea.Add(GameValue(static_cast<float>(r.bought[s])));
        ea.Add(GameValue(std::max(0.0f, r.lastBarrage[s] + r.config.barrageCooldown - now)));
        ea.Add(GameValue(now < r.humanUntil[s]));
        // who commands, and the vote on it
        Person* holder = now < r.humanUntil[s] ? r.seat[s].GetLink() : nullptr;
        ea.Add(GameValueExt(static_cast<Object*>(holder)));
        ea.Add(GameValue(RString(holder ? PlayerName(holder).c_str() : "")));
        const Referee::Vote& v = r.vote[s];
        GameValue vote = NetArray();
        GameArrayType& va = vote;
        va.Add(GameValue(v.active));
        va.Add(GameValue(std::max(0.0f, v.until - now)));
        va.Add(GameValue(static_cast<float>(v.yes.size())));
        va.Add(GameValue(static_cast<float>(v.no.size())));
        va.Add(GameValue(static_cast<float>(v.active ? Electorate(r, s) : 0)));
        va.Add(GameValue(RString(v.targetName.c_str())));
        va.Add(GameValue(RString(v.callerName.c_str())));
        ea.Add(vote);
        ea.Add(GameValue(std::max(0.0f, r.lastAir[s] + r.config.airCooldown - now)));
        ea.Add(GameValue(std::any_of(r.strikes.begin(), r.strikes.end(), [s](const Strike& k) { return k.side == s; })));
        sa.Add(e);
    }
    a.Add(sides);

    GameValue groups = NetArray();
    GameArrayType& ga = groups;
    for (const GroupRecord& rec : r.groups)
    {
        AIGroup* grp = rec.group;
        if (!grp)
            continue;
        GameValue e = NetArray();
        GameArrayType& ea = e;
        ea.Add(GameValue(static_cast<float>(rec.id)));
        ea.Add(GameValue(static_cast<float>(rec.side)));
        ea.Add(GameValueExt(grp));
        ea.Add(GameValue(static_cast<float>(static_cast<int>(rec.task))));
        ea.Add(GameValue(static_cast<float>(rec.objective)));
        ea.Add(GameValue(rec.destination.X()));
        ea.Add(GameValue(rec.destination.Z()));
        ea.Add(GameValue(static_cast<float>(rec.type)));
        ea.Add(GameValue(now < rec.lockedUntil));
        ea.Add(GameValue(rec.stalled));
        ea.Add(GameValue(RString(rec.hqText.c_str())));
        ea.Add(GameValue(rec.hqX));
        ea.Add(GameValue(rec.hqY));
        ga.Add(e);
    }
    a.Add(groups);

    // What each side knows of its enemies: the clients' AI centres do not keep the target database.
    GameValue contacts = NetArray();
    GameArrayType& ca = contacts;
    for (int s = 0; s < r.war.sideCount; s++)
    {
        GameValue list = NetArray();
        GameArrayType& la = list;
        for (const Contact& c : KnownEnemies(s))
        {
            if (static_cast<size_t>(la.Size()) >= NetContactsPerSide)
                break;
            GameValue e = NetArray();
            GameArrayType& ea = e;
            ea.Add(GameValue(c.x));
            ea.Add(GameValue(c.y));
            ea.Add(GameValue(c.height));
            ea.Add(GameValue(c.age));
            ea.Add(GameValue(c.vehicle));
            la.Add(e);
        }
        ca.Add(list);
    }
    a.Add(contacts);

    GameValue radio = NetArray();
    GameArrayType& ra = radio;
    for (const NetRadio& line : r.radioLog)
    {
        GameValue e = NetArray();
        GameArrayType& ea = e;
        ea.Add(GameValue(static_cast<float>(line.seq)));
        ea.Add(GameValue(static_cast<float>(line.side)));
        ea.Add(GameValue(RString(line.text.c_str())));
        ra.Add(e);
    }
    a.Add(radio);

    GameValue log = NetArray();
    GameArrayType& la = log;
    const size_t first = r.snapshot.log.size() > NetLogLines ? r.snapshot.log.size() - NetLogLines : 0;
    for (size_t i = first; i < r.snapshot.log.size(); i++)
        la.Add(GameValue(RString(r.snapshot.log[i].c_str())));
    a.Add(log);

    a.Add(GameValue(static_cast<float>(r.winner)));
    a.Add(GameValue(RString(r.winReason.c_str())));

    gs->VarSet("conquestNet", root);
    GetNetworkManager().PublicVariable("conquestNet");
}

// Players' own groups (the mission's playable slots) become Conquest squads too: HQ gives them
// orders, the commander view shows them, the planner leaves them alone (they are player groups).
void AdoptPlayerGroups(Referee& r)
{
    for (int s = 0; s < r.war.sideCount; s++)
    {
        AICenter* center = GWorld->GetCenter(r.config.engineSide[s]);
        if (!center)
            continue;
        for (int g = 0; g < center->NGroups(); g++)
        {
            AIGroup* grp = center->GetGroup(g);
            if (!grp || !grp->IsAnyPlayerGroup() || RecordOf(r, grp))
                continue;
            GroupRecord rec;
            rec.id = r.nextGroupId++;
            rec.side = s;
            rec.type = -1;
            rec.group = grp;
            rec.bestAt = Now();
            r.groups.push_back(rec);
            WarLog(Name(s) + " player squad joins the war as " + Callsign(rec.id));
        }
    }
}

// ---- a client: the mirror ----

void ReadNet(Referee& r)
{
    GameState* gs = GWorld->GetGameState();
    if (!gs)
        return;
    const GameValue value = gs->VarGet("conquestNet");
    if (value.GetNil() || value.GetType() != GameArray)
        return;
    const GameArrayType& a = value;
    if (a.Size() < 10)
        return;
    const int seq = static_cast<int>(Num(a, 0));
    if (seq == r.netSeq)
        return;
    r.netSeq = seq;
    r.started = Flag(a, 2);
    r.ended = Flag(a, 3);
    // The server left the Resistance out: so does the mirror.
    if (a[5].GetType() == GameArray)
        while (r.war.sideCount > 2 && static_cast<const GameArrayType&>(a[5]).Size() < r.war.sideCount)
            DropLastSide(r.war, r.tuning, 0.0f);
    if (a.Size() > 11)
    {
        r.winner = static_cast<int>(Num(a, 10, NoSide));
        r.winReason = Str(a, 11);
    }

    const GameArrayType& oa = a[4];
    for (int o = 0; o < oa.Size() && o < static_cast<int>(r.war.defs.size()); o++)
    {
        const GameArrayType& e = oa[o];
        ObjectiveState& s = r.war.states[o];
        s.owner = static_cast<int>(Num(e, 0, NoSide));
        s.holder = static_cast<int>(Num(e, 1, NoSide));
        s.level = Num(e, 2);
        s.contested = Flag(e, 3);
        r.war.defs[o].x = Num(e, 4, r.war.defs[o].x);
        r.war.defs[o].y = Num(e, 5, r.war.defs[o].y);
        if (e.Size() > 6)
            r.posts[o].flag = dyn_cast<EntityAI>(GetObject(e[6]));
        r.spawnWhy[o].fill(std::string());
        if (e.Size() > 7 && e[7].GetType() == GameArray)
        {
            const GameArrayType& wa = e[7];
            for (int side = 0; side < wa.Size() && side < MaxSides; side++)
                r.spawnWhy[o][side] = Str(wa, side);
        }
    }

    const GameArrayType& sa = a[5];
    for (int s = 0; s < sa.Size() && s < r.war.sideCount; s++)
    {
        const GameArrayType& e = sa[s];
        r.war.pool[s] = Num(e, 0);
        r.war.score[s] = Num(e, 1);
        r.war.defeated[s] = Flag(e, 2);
        r.live[s] = static_cast<int>(Num(e, 3));
        r.losses[s] = static_cast<int>(Num(e, 4));
        r.bought[s] = static_cast<int>(Num(e, 5));
        r.netCooldown[s] = Num(e, 6);
        r.netHuman[s] = Flag(e, 7);
        r.netSeat[s] = e.Size() > 8 ? dyn_cast<Person>(GetObject(e[8])) : nullptr;
        r.netSeatName[s] = Str(e, 9);
        CommandSeat& c = r.netVote[s];
        c = CommandSeat();
        if (e.Size() > 10 && e[10].GetType() == GameArray)
        {
            const GameArrayType& va = e[10];
            c.vote = Flag(va, 0);
            c.voteLeft = Num(va, 1);
            c.yes = static_cast<int>(Num(va, 2));
            c.no = static_cast<int>(Num(va, 3));
            c.electorate = static_cast<int>(Num(va, 4));
            c.voteTarget = Str(va, 5);
            c.voteCaller = Str(va, 6);
        }
        if (s == r.playerSide && !c.vote)
            r.myVote = 0;
        r.netAirCooldown[s] = Num(e, 11);
        r.netAirInbound[s] = Flag(e, 12);
    }

    const GameArrayType& ga = a[6];
    r.groups.clear();
    for (int i = 0; i < ga.Size(); i++)
    {
        const GameArrayType& e = ga[i];
        if (e.Size() < 13)
            continue;
        GroupRecord rec;
        rec.id = static_cast<int>(Num(e, 0));
        rec.side = static_cast<int>(Num(e, 1, NoSide));
        rec.group = GetGroup(e[2]);
        rec.task = static_cast<TaskKind>(static_cast<int>(Num(e, 3)));
        rec.objective = static_cast<int>(Num(e, 4, -1));
        rec.destination = Ground(Num(e, 5), Num(e, 6));
        rec.type = static_cast<int>(Num(e, 7, -1));
        rec.lockedUntil = Flag(e, 8) ? 1.0e9f : -1.0e9f;
        rec.stalled = Flag(e, 9);
        rec.hqText = Str(e, 10);
        rec.hqX = Num(e, 11);
        rec.hqY = Num(e, 12);
        if (rec.group)
            r.groups.push_back(rec);
    }

    const GameArrayType& ca = a[7];
    for (int s = 0; s < MaxSides; s++)
        r.netContacts[s].clear();
    for (int s = 0; s < ca.Size() && s < MaxSides; s++)
    {
        const GameArrayType& list = ca[s];
        for (int i = 0; i < list.Size(); i++)
        {
            const GameArrayType& e = list[i];
            Contact c;
            c.x = Num(e, 0);
            c.y = Num(e, 1);
            c.height = Num(e, 2);
            c.age = Num(e, 3);
            c.vehicle = Flag(e, 4);
            r.netContacts[s].push_back(c);
        }
    }

    // HQ's radio for this client's side, each line once.
    const GameArrayType& ra = a[8];
    for (int i = 0; i < ra.Size(); i++)
    {
        const GameArrayType& e = ra[i];
        const int lineSeq = static_cast<int>(Num(e, 0));
        if (lineSeq <= r.radioSeen)
            continue;
        r.radioSeen = lineSeq;
        if (static_cast<int>(Num(e, 1, NoSide)) == r.playerSide && r.options.hqRadio)
            GChatList.Add(CCSide, RString("HQ"), RString(Str(e, 2).c_str()), false, true);
    }

    const GameArrayType& la = a[9];
    r.snapshot.log.clear();
    for (int i = 0; i < la.Size(); i++)
        r.snapshot.log.push_back(Str(la, i));
}

// What this client's player asks the server to do for their side.
void Request(const char* kind, const GameValue& args)
{
    Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
    if (!player)
        return;
    GameValue payload = NetArray();
    GameArrayType& p = payload;
    p.Add(GameValueExt(static_cast<Object*>(player)));
    p.Add(GameValue(RString(kind)));
    p.Add(args);
    RunWith("_p remoteExec [\"conquestRequest\", 2]", GameValue(), payload);
}

// One mirror step: the latest published war, and the client's own markers, flags and shroud.
void MirrorStep(Referee& r, float now)
{
    r.playerSide = LocalSide(r);
    const int before = r.netSeq;
    ReadNet(r);
    if (before == 0 && r.netSeq != 0)
    {
        r.mirrorSince = now;
        LOG_INFO(Mission, "Conquest: this client mirrors the server's war ({} squads; the player is {})",
                 r.groups.size(), Name(r.playerSide));
    }
    if (r.netSeq != 0 && now - r.lastStatus >= StatusSeconds)
    {
        r.lastStatus = now;
        LOG_INFO(Mission, "Conquest mirror status: {}", StatusText());
    }
    // POSEIDON_CONQUEST_NETTEST=1: once mirroring, the client takes command of its side and orders its
    // side's first AI squad to Figari, through the server -- the multiplayer path, checkable from the
    // two logs where the harness is not allowed (it refuses commands in multiplayer).
    static const bool netTest = std::getenv("POSEIDON_CONQUEST_NETTEST") != nullptr;
    if (netTest && !r.netTestSent && r.netSeq != 0 && now - r.mirrorSince > 20.0f && r.playerSide != NoSide)
        for (const GroupRecord& rec : r.groups)
            if (rec.side == r.playerSide && rec.type >= 0)
            {
                r.netTestSent = true;
                SetHumanCommander(true);
                const int n = OrderGroups({rec.id}, 5283.0f, 5375.0f);
                LOG_INFO(Mission, "Conquest: net test: asked for command and sent {} to Figari ({} squad)", Callsign(rec.id), n);
                break;
            }
    if (r.localCommand && r.playerSide != NoSide && r.netSeq != 0 && now - r.commandSince > 4.0f &&
        r.netSeat[r.playerSide].GetLink() != GWorld->GetRealPlayer())
    {
        r.localCommand = false;
        r.commandLoss = r.netSeatName[r.playerSide].empty()
                            ? "You do not have command (your side voted you out, or command was not free)."
                            : r.netSeatName[r.playerSide] + " commands your side.";
    }
    if (r.localCommand && now - r.lastCommandBeat > 10.0f)
    {
        r.lastCommandBeat = now;
        GameValue args = NetArray();
        GameArrayType& aa = args;
        aa.Add(GameValue(true));
        Request("command", args);
    }
    if (now - r.lastTick < TickSeconds)
        return;
    r.lastTick = now;
    for (size_t o = 0; o < r.war.defs.size(); o++)
        FlyFlag(r, o);
    UpdateShroud(r);
    UpdateMarkers(r);
    UpdateGroupMarkers(r);
}
