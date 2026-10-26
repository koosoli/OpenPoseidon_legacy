// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest in multiplayer: one human commander per side, and the side's vote to replace him. Included by
// ConquestReferee.cpp inside its anonymous namespace (after ConquestRefereeNet.inc, whose helpers it uses).

constexpr float VoteSeconds = 60.0f;
constexpr float BarSeconds = 300.0f; // voted out: no command for five minutes

std::string PlayerName(Person* p)
{
    if (!p)
        return "nobody";
    const GameValue v = RunWith("name _o", GameValueExt(p), GameValue());
    if (v.GetType() == GameString)
        return static_cast<const char*>(static_cast<GameStringType>(v));
    return "a player";
}

// The side's human players: the vote's electorate.
int Electorate(const Referee& r, int side)
{
    int n = 0;
    AICenter* center = GWorld->GetCenter(r.config.engineSide[side]);
    for (int g = 0; center && g < center->NGroups(); g++)
        if (AIGroup* grp = center->GetGroup(g))
            for (int id = 1; id <= MAX_UNITS_PER_GROUP; id++)
                if (AIUnit* unit = grp->UnitWithID(id); unit && unit->GetPerson() && unit->GetPerson()->IsNetworkPlayer())
                    n++;
    return std::max(n, 1);
}

// Nobody commands `side` any more: the AI commander takes over at once. Squads the human ordered were held
// to that order for HumanLockSeconds and the planner waited for its next round, so leaving the command view
// looked like nobody took over (owner, 2026-10-26). Supply runs stay as they are.
void ReleaseCommand(Referee& r, int side)
{
    if (r.humanUntil[side] > Now())
        WarLog(Name(side) + " is back under the AI commander");
    r.humanUntil[side] = -1.0e9f;
    for (GroupRecord& rec : r.groups)
    {
        if (rec.side != side || IsSupply(r, rec) || rec.lockedUntil <= Now())
            continue;
        rec.lockedUntil = -1.0e9f;
        if (rec.task == TaskKind::Move)
            rec.task = TaskKind::None;
        rec.engageName.clear();
    }
    r.replan[side] = true;
}

// `who` takes (on) or gives up (off) command of `side`. In multiplayer one player holds it at a time.
bool ClaimCommand(Referee& r, int side, Person* who, bool on, std::string* why)
{
    if (side < 0 || side >= r.war.sideCount)
        return false;
    if (!InMultiplayer())
    {
        if (on)
            r.humanUntil[side] = 1.0e9f;
        else
            ReleaseCommand(r, side);
        return true;
    }
    const float now = Now();
    Person* holder = r.seat[side];
    const bool held = holder && now < r.humanUntil[side];
    if (!on)
    {
        if (holder == who)
        {
            r.seat[side] = nullptr;
            ReleaseCommand(r, side);
        }
        return true;
    }
    if (held && holder != who)
    {
        if (why)
            *why = PlayerName(holder) + " commands " + Name(side) + ". Ctrl+V calls a vote to replace them.";
        return false;
    }
    for (const auto& [person, until] : r.barred)
        if (person.GetLink() == who && now < until)
        {
            if (why)
                *why = "Your side voted you out of command: wait " + std::to_string(static_cast<int>(until - now)) + " s.";
            return false;
        }
    if (!held)
    {
        WarLog(Name(side) + " is under " + PlayerName(who) + "'s command");
        Radio(r, side, PlayerName(who) + " has taken command.");
    }
    r.seat[side] = who;
    r.humanUntil[side] = now + CommandHoldSeconds;
    return true;
}

void StartVote(Referee& r, int side, Person* caller)
{
    Referee::Vote& v = r.vote[side];
    Person* holder = r.seat[side];
    if (v.active || !holder || !caller || Now() >= r.humanUntil[side] || caller == holder)
        return;
    v = Referee::Vote();
    v.active = true;
    v.until = Now() + VoteSeconds;
    v.target = holder;
    v.targetName = PlayerName(holder);
    v.callerName = PlayerName(caller);
    v.yes.push_back(caller);
    WarLog(Name(side) + ": " + v.callerName + " calls a vote to remove " + v.targetName + " as commander");
    Radio(r, side, v.callerName + " calls a vote: remove " + v.targetName + " as commander? Ctrl+Y yes, Ctrl+N no (60 s).");
}

void Ballot(Referee& r, int side, Person* who, bool yes)
{
    Referee::Vote& v = r.vote[side];
    if (!v.active || !who)
        return;
    for (auto* list : {&v.yes, &v.no})
        list->erase(std::remove_if(list->begin(), list->end(), [who](const OLink<Person>& p) { return p.GetLink() == who; }),
                    list->end());
    (yes ? v.yes : v.no).push_back(who);
}

void StepVotes(Referee& r, float now)
{
    for (int side = 0; side < r.war.sideCount; side++)
    {
        Referee::Vote& v = r.vote[side];
        if (!v.active)
            continue;
        Person* holder = r.seat[side];
        if (!holder || holder != v.target.GetLink() || now >= r.humanUntil[side])
        {
            v.active = false;
            Radio(r, side, "The vote is over: " + v.targetName + " no longer commands.");
            continue;
        }
        const auto count = [](const std::vector<OLink<Person>>& list)
        { return static_cast<int>(std::count_if(list.begin(), list.end(), [](const OLink<Person>& p) { return p.GetLink() != nullptr; })); };
        const int voters = Electorate(r, side), yes = count(v.yes), no = count(v.no);
        const bool remove = yes * 2 > voters || (now >= v.until && yes > no);
        if (!remove && no * 2 < voters && now < v.until)
            continue; // still open
        v.active = false;
        const std::string tally = " (" + std::to_string(yes) + " to " + std::to_string(no) + ")";
        if (remove)
        {
            r.seat[side] = nullptr;
            r.humanUntil[side] = -1.0e9f;
            r.barred.emplace_back(OLink<Person>(holder), now + BarSeconds);
            WarLog(Name(side) + ": " + v.targetName + " is voted out of command" + tally);
            Radio(r, side, v.targetName + " is voted out of command" + tally + " and fights on as a soldier. Command is free.");
        }
        else
            Radio(r, side, v.targetName + " stays in command" + tally + ".");
    }
}
