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
    if (s.owner == side)
        return false;
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
    const float factor = std::clamp(strength / 4.0f, 0.5f, 2.0f);
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
        if (presence.strength[best] < tuning.dominanceRatio * presence.strength[side])
        {
            contested = true;
            return NoSide;
        }
    }
    (void)present;
    return best;
}

CaptureResult StepCapture(War& war, int index, const Presence& presence, float dt, float now, const Tuning& tuning)
{
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
        const float rate = 1.0f / CaptureSeconds(presence.strength[side], tuning);
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

    const float rate = 1.0f / CaptureSeconds(presence.strength[side], tuning);
    if (s.holder == side)
    {
        s.level += rate * dt;
        if (s.level >= 1.0f)
        {
            s.level = 1.0f;
            s.owner = side;
            s.capturedAt = now;
            result.event = CaptureEvent::Captured;
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
    for (int side = 0; side < war.sideCount; side++)
        if (!war.defeated[side])
        {
            alive++;
            last = side;
        }
    return alive == 1 ? last : NoSide;
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
