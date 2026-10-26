// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Conquest/ConquestRules.hpp>

#include <algorithm>
#include <cctype>

namespace Poseidon::Conquest
{
void StartWar(War& war, const Tuning& tuning, float startPool)
{
    war.states.assign(war.defs.size(), ObjectiveState());
    for (size_t i = 0; i < war.defs.size(); i++)
    {
        ObjectiveState& s = war.states[i];
        s.owner = war.defs[i].initialOwner;
        s.holder = s.owner;
        s.level = s.owner == NoSide ? 0.0f : 1.0f;
    }
    for (int side = 0; side < MaxSides; side++)
    {
        war.pool[side] = side < war.sideCount ? startPool : 0.0f;
        war.score[side] = side < war.sideCount ? tuning.startScore : 0.0f;
        war.defeated[side] = side >= war.sideCount;
    }
}

bool Allied(const War& war, int a, int b)
{
    return a != b && a >= 0 && b >= 0 && a < MaxSides && b < MaxSides && war.allied[a][b];
}

namespace
{
// Allies in one zone count as one force: with the owner's, for the owner; otherwise for the stronger.
Presence MergeAllies(const War& war, int index, const Presence& in)
{
    Presence p = in;
    const int owner = war.states[index].owner;
    for (int a = 0; a < war.sideCount; a++)
        for (int b = a + 1; b < war.sideCount; b++)
        {
            if (!Allied(war, a, b) || p.strength[a] + p.strength[b] <= 0.0f)
                continue;
            const int keep = owner == a ? a : owner == b ? b : p.strength[a] >= p.strength[b] ? a : b;
            const int fold = keep == a ? b : a;
            p.strength[keep] += p.strength[fold];
            p.strength[fold] = 0.0f;
        }
    return p;
}
} // namespace

int OwnedCount(const War& war, int side, bool countHQs)
{
    int n = 0;
    for (size_t i = 0; i < war.states.size(); i++)
        if (war.states[i].owner == side && (countHQs || war.defs[i].kind != ObjectiveKind::HQ))
            n++;
    return n;
}

bool CanAttack(const War& war, int side, int index)
{
    if (side == NoSide || index < 0 || index >= static_cast<int>(war.defs.size()))
        return false;
    const ObjectiveState& s = war.states[index];
    if (s.owner == side || Allied(war, side, s.owner))
        return false; // its own, or an ally's
    // An HQ falls last: while its owner still holds anything else, it cannot be attacked.
    if (war.defs[index].kind == ObjectiveKind::HQ && s.owner != NoSide && OwnedCount(war, s.owner) > 1)
        return false;
    for (int n : war.defs[index].neighbours)
        if (n >= 0 && n < static_cast<int>(war.states.size()) && war.states[n].owner == side)
            return true;
    return false;
}

float CaptureSeconds(float strength, const Tuning& tuning)
{
    const float factor = std::clamp(strength / 4.0f, 0.5f, MaxCaptureFactor);
    return tuning.baseCaptureSeconds / factor;
}

int DominantSide(const Presence& presence, int sideCount, const Tuning& tuning, bool& contested)
{
    contested = false;
    int best = NoSide;
    int present = 0;
    for (int side = 0; side < sideCount; side++)
    {
        if (presence.strength[side] <= 0.0f)
            continue;
        present++;
        if (best == NoSide || presence.strength[side] > presence.strength[best])
            best = side;
    }
    if (best == NoSide)
        return NoSide;
    for (int side = 0; side < sideCount; side++)
    {
        if (side == best || presence.strength[side] <= 0.0f)
            continue;
        if (presence.strength[best] <= tuning.dominanceRatio * presence.strength[side])
        {
            contested = true;
            return NoSide;
        }
    }
    (void)present;
    return best;
}

float CaptureMargin(const Presence& presence, int sideCount, int side)
{
    float other = 0.0f;
    for (int s = 0; s < sideCount; s++)
        if (s != side)
            other = std::max(other, presence.strength[s]);
    return std::max(0.0f, presence.strength[side] - other);
}

CaptureResult StepCapture(War& war, int index, const Presence& measured, float dt, float now, const Tuning& tuning)
{
    const Presence presence = MergeAllies(war, index, measured);
    CaptureResult result;
    ObjectiveState& s = war.states[index];
    bool contested = false;
    const int side = DominantSide(presence, war.sideCount, tuning, contested);
    s.contested = contested;
    if (contested)
        return result; // frozen

    if (side == NoSide)
    {
        // Nobody there: the bar drifts back to its owner (or empties when nobody owns it).
        const float rate = tuning.decayFactor / tuning.baseCaptureSeconds;
        if (s.owner != NoSide && s.holder == s.owner)
            s.level = std::min(1.0f, s.level + rate * dt);
        else
        {
            s.level -= rate * dt;
            if (s.level <= 0.0f)
            {
                s.level = 0.0f;
                s.holder = s.owner;
            }
        }
        return result;
    }

    // The owner standing on its own objective restores its bar.
    if (side == s.owner)
    {
        const float rate = 1.0f / CaptureSeconds(CaptureMargin(presence, war.sideCount, side), tuning);
        if (s.holder == side)
            s.level = std::min(1.0f, s.level + rate * dt);
        else
        {
            s.level -= rate * dt;
            if (s.level <= 0.0f)
            {
                s.level = 0.0f;
                s.holder = side;
            }
        }
        return result;
    }

    if (!CanAttack(war, side, index))
        return result; // not on this side's front line: no progress

    const float rate = 1.0f / CaptureSeconds(CaptureMargin(presence, war.sideCount, side), tuning);
    if (s.holder == side)
    {
        s.level += rate * dt;
        if (s.level >= 1.0f)
        {
            s.level = 1.0f;
            s.owner = side;
            s.capturedAt = now;
            result.event = CaptureEvent::Captured;
            s.stock = 0.0f; // what the old owner kept here is gone
            result.side = side;
        }
        return result;
    }
    if (s.holder == NoSide || s.level <= 0.0f)
    {
        s.holder = side;
        s.level = std::min(1.0f, rate * dt);
        return result;
    }
    // Someone else's bar: drain it first.
    s.level -= rate * dt;
    if (s.level <= 0.0f)
    {
        s.level = 0.0f;
        s.holder = NoSide;
        if (s.owner != NoSide)
        {
            s.owner = NoSide;
            s.capturedAt = now;
            result.event = CaptureEvent::Neutralised;
            s.stock = 0.0f;
            result.side = side;
        }
    }
    return result;
}

float IncomePerMinute(const War& war, int side)
{
    float income = 0.0f;
    for (size_t i = 0; i < war.states.size(); i++)
        if (war.states[i].owner == side)
            income += war.defs[i].value;
    return income;
}

bool SpawnEligible(const War& war, int side, int index, float now, const Tuning& tuning, std::string* reason)
{
    const auto refuse = [&](const char* why)
    {
        if (reason)
            *reason = why;
        return false;
    };
    if (index < 0 || index >= static_cast<int>(war.states.size()))
        return refuse("no such base");
    const ObjectiveState& s = war.states[index];
    if (s.owner != side)
        return refuse("not ours");
    if (s.contested || (s.holder != side && s.level > 0.0f) || s.level < 1.0f)
        return refuse("under attack");
    if (side >= 0 && side < MaxSides && s.enemiesNear[side] > 0)
        return refuse("enemy near the base");
    if (now - s.capturedAt < tuning.settleSeconds)
        return refuse("just captured");
    if (now - s.lastHitAt < tuning.hitMemorySeconds)
        return refuse("taking fire");
    return true;
}

int StepEconomy(War& war, float dt, const Tuning& tuning)
{
    std::array<int, MaxSides> held{};
    int most = 0;
    for (int side = 0; side < war.sideCount; side++)
    {
        if (war.defeated[side])
            continue;
        war.pool[side] += IncomePerMinute(war, side) * dt / 60.0f;
        held[side] = OwnedCount(war, side, false);
        most = std::max(most, held[side]);
    }
    for (int side = 0; side < war.sideCount; side++)
    {
        if (war.defeated[side])
            continue;
        war.score[side] -= tuning.scoreDrainPerObjective * static_cast<float>(most - held[side]) * dt / 60.0f;
        if (war.score[side] <= 0.0f || OwnedCount(war, side) == 0)
        {
            war.score[side] = std::max(0.0f, war.score[side]);
            war.defeated[side] = true;
        }
    }
    int alive = 0;
    int last = NoSide;
    bool allies = true; // everyone left fights on the same side
    for (int side = 0; side < war.sideCount; side++)
        if (!war.defeated[side])
        {
            if (last != NoSide && !Allied(war, side, last))
                allies = false;
            alive++;
            if (last == NoSide || OwnedCount(war, side) > OwnedCount(war, last))
                last = side;
        }
    return alive == 1 || (alive > 1 && allies) ? last : NoSide;
}

int TownVictor(const War& war, int towns)
{
    if (towns <= 0)
        return NoSide;
    int best = NoSide, most = 0;
    for (int side = 0; side < war.sideCount; side++)
    {
        const int held = war.defeated[side] ? 0 : OwnedCount(war, side, false);
        if (held >= towns && held > most)
        {
            best = side;
            most = held;
        }
    }
    return best;
}

int ScoreLeader(const War& war)
{
    int best = NoSide;
    for (int side = 0; side < war.sideCount; side++)
    {
        if (war.defeated[side])
            continue;
        if (best == NoSide || war.score[side] > war.score[best] ||
            (war.score[side] == war.score[best] && OwnedCount(war, side) > OwnedCount(war, best)))
            best = side;
    }
    return best;
}

int TownCount(const War& war)
{
    int n = 0;
    for (const ObjectiveDef& d : war.defs)
        if (d.kind != ObjectiveKind::HQ)
            n++;
    return n;
}

void DropLastSide(War& war, const Tuning& tuning, float startPool)
{
    if (war.sideCount <= 2)
        return;
    const int gone = --war.sideCount;
    for (ObjectiveDef& d : war.defs)
        if (d.initialOwner == gone)
        {
            d.initialOwner = NoSide;
            if (d.kind == ObjectiveKind::HQ)
                d.kind = ObjectiveKind::Town;
        }
    StartWar(war, tuning, startPool);
}

std::vector<ObjectiveDef> AutoObjectives(const std::vector<Place>& input, int sides, float link)
{
    // The larger places first, so a hamlet beside a town folds into the town.
    std::vector<Place> places = input;
    std::stable_sort(places.begin(), places.end(), [](const Place& a, const Place& b) { return a.rank > b.rank; });
    std::vector<ObjectiveDef> defs;
    const auto dist = [](float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); };
    for (const Place& p : places)
    {
        if (std::any_of(defs.begin(), defs.end(),
                        [&](const ObjectiveDef& d) { return d.name == p.name || dist(d.x, d.y, p.x, p.y) < 200.0f; }))
            continue;
        ObjectiveDef d;
        d.name = p.name;
        d.x = p.x;
        d.y = p.y;
        d.radius = p.rank >= 2 ? 150.0f : p.rank == 1 ? 130.0f : 100.0f;
        d.value = p.rank >= 2 ? 10.0f : p.rank == 1 ? 7.0f : 5.0f;
        defs.push_back(d);
    }
    const int n = static_cast<int>(defs.size());
    if (n < 2)
        return defs;
    // The HQs: the two places farthest apart, then the one farthest from both.
    int a = 0, b = 1;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (dist(defs[i].x, defs[i].y, defs[j].x, defs[j].y) > dist(defs[a].x, defs[a].y, defs[b].x, defs[b].y))
            {
                a = i;
                b = j;
            }
    std::vector<int> hq{a, b};
    if (sides >= 3 && n >= 3)
    {
        int c = -1;
        float best = -1.0f;
        for (int k = 0; k < n; k++)
        {
            if (k == a || k == b)
                continue;
            const float m = std::min(dist(defs[k].x, defs[k].y, defs[a].x, defs[a].y), dist(defs[k].x, defs[k].y, defs[b].x, defs[b].y));
            if (m > best)
            {
                best = m;
                c = k;
            }
        }
        hq.push_back(c);
    }
    for (size_t s = 0; s < hq.size(); s++)
    {
        ObjectiveDef& d = defs[hq[s]];
        d.kind = ObjectiveKind::HQ;
        d.initialOwner = static_cast<int>(s);
        d.value = 10.0f;
        d.radius = std::max(d.radius, 150.0f);
    }
    const auto join = [&](int i, int j)
    {
        if (i == j)
            return;
        if (std::find(defs[i].neighbours.begin(), defs[i].neighbours.end(), j) == defs[i].neighbours.end())
            defs[i].neighbours.push_back(j);
        if (std::find(defs[j].neighbours.begin(), defs[j].neighbours.end(), i) == defs[j].neighbours.end())
            defs[j].neighbours.push_back(i);
    };
    // Each place to its nearest neighbours ...
    for (int i = 0; i < n; i++)
    {
        std::vector<int> order;
        for (int j = 0; j < n; j++)
            if (j != i)
                order.push_back(j);
        std::sort(order.begin(), order.end(), [&](int p, int q)
                  { return dist(defs[i].x, defs[i].y, defs[p].x, defs[p].y) < dist(defs[i].x, defs[i].y, defs[q].x, defs[q].y); });
        for (size_t k = 0; k < order.size() && k < 3; k++)
            if (dist(defs[i].x, defs[i].y, defs[order[k]].x, defs[order[k]].y) <= link)
                join(i, order[k]);
    }
    // ... and the shortest links that keep every place reachable (a minimum spanning tree), so no
    // island in the graph is left out of the war however far it lies.
    std::vector<bool> in(n, false);
    in[0] = true;
    for (int added = 1; added < n; added++)
    {
        int bi = -1, bj = -1;
        float best = 1.0e30f;
        for (int i = 0; i < n; i++)
            if (in[i])
                for (int j = 0; j < n; j++)
                    if (!in[j] && dist(defs[i].x, defs[i].y, defs[j].x, defs[j].y) < best)
                    {
                        best = dist(defs[i].x, defs[i].y, defs[j].x, defs[j].y);
                        bi = i;
                        bj = j;
                    }
        in[bj] = true;
        join(bi, bj);
    }
    return defs;
}

void RecordLoss(War& war, int side, const Tuning& tuning)
{
    if (side >= 0 && side < war.sideCount)
        war.score[side] -= tuning.scorePerManLost;
}

const char* KindName(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::HQ: return "HQ";
        case ObjectiveKind::Airfield: return "airfield";
        case ObjectiveKind::Depot: return "depot";
        case ObjectiveKind::Town: break;
    }
    return "town";
}

bool KindFromName(const std::string& name, ObjectiveKind& kind)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "hq")
        kind = ObjectiveKind::HQ;
    else if (lower == "town")
        kind = ObjectiveKind::Town;
    else if (lower == "airfield")
        kind = ObjectiveKind::Airfield;
    else if (lower == "depot")
        kind = ObjectiveKind::Depot;
    else
        return false;
    return true;
}
} // namespace Poseidon::Conquest
