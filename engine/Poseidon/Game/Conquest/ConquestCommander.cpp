// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Game/Conquest/ConquestCommander.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Poseidon::Conquest
{
namespace
{
float Distance(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

bool Owns(const War& war, int side, int objective)
{
    return objective >= 0 && objective < static_cast<int>(war.states.size()) && war.states[objective].owner == side;
}

float Known(const SideView& view, int objective)
{
    return objective >= 0 && objective < static_cast<int>(view.knownEnemy.size()) ? view.knownEnemy[objective] : 0.0f;
}

// Own objective in trouble: enemies known nearby, or its bar is not full and ours.
bool Threatened(const SideView& view, int objective)
{
    const ObjectiveState& s = view.war->states[objective];
    return Known(view, objective) > 0.0f || s.contested || s.holder != view.side || s.level < 1.0f;
}

// Own objective with no non-own neighbour: nothing can reach it without passing another first.
bool Rear(const SideView& view, int objective)
{
    for (int n : view.war->defs[objective].neighbours)
        if (!Owns(*view.war, view.side, n))
            return false;
    return true;
}
} // namespace

const char* TaskName(TaskKind task)
{
    switch (task)
    {
        case TaskKind::Attack: return "attack";
        case TaskKind::Defend: return "defend";
        case TaskKind::Move: return "move";
        case TaskKind::None: break;
    }
    return "none";
}

std::vector<std::pair<int, float>> RankTargets(const SideView& view)
{
    std::vector<std::pair<int, float>> out;
    const War& war = *view.war;
    for (int i = 0; i < static_cast<int>(war.defs.size()); i++)
    {
        if (!CanAttack(war, view.side, i))
            continue;
        float nearestOwn = std::numeric_limits<float>::max();
        for (int j = 0; j < static_cast<int>(war.defs.size()); j++)
            if (Owns(war, view.side, j))
                nearestOwn = std::min(nearestOwn, Distance(war.defs[i].x, war.defs[i].y, war.defs[j].x, war.defs[j].y));
        const ObjectiveState& s = war.states[i];
        float score = 20.0f + 2.0f * war.defs[i].value - 2.0f * Known(view, i) - nearestOwn / 250.0f;
        if (s.holder == view.side)
            score += 20.0f * s.level; // finish what was started
        if (s.owner == NoSide)
            score += 5.0f; // a neutral objective needs no draining
        out.emplace_back(i, score);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

std::vector<Intent> PlanStandard(const SideView& view, const CommanderTuning& tuning)
{
    std::vector<Intent> intents;
    if (!view.war || view.side == NoSide)
        return intents;
    const War& war = *view.war;
    const int count = static_cast<int>(war.defs.size());

    struct Slot
    {
        const GroupView* group;
        TaskKind task;
        int objective;
    };
    std::vector<Slot> slots;
    slots.reserve(view.groups.size());
    for (const GroupView& g : view.groups)
        slots.push_back({&g, g.task, g.objective});

    const auto assign = [&](Slot& slot, TaskKind task, int objective, const char* why)
    {
        if (slot.task == task && slot.objective == objective)
            return;
        slot.task = task;
        slot.objective = objective;
        Intent intent;
        intent.kind = Intent::Kind::Assign;
        intent.group = slot.group->id;
        intent.objective = objective;
        intent.task = task;
        intent.why = why;
        intents.push_back(intent);
    };

    // 1. Tasks that no longer make sense.
    for (Slot& slot : slots)
    {
        if (slot.group->locked)
            continue;
        if (slot.group->stalled)
        {
            slot.task = TaskKind::None;
            slot.objective = -1;
            continue;
        }
        if (slot.task == TaskKind::Attack && Owns(war, view.side, slot.objective))
            assign(slot, TaskKind::Defend, slot.objective, "taken, holding it");
        else if (slot.task == TaskKind::Attack && !CanAttack(war, view.side, slot.objective))
        {
            slot.task = TaskKind::None;
            slot.objective = -1;
        }
        else if (slot.task == TaskKind::Defend && !Owns(war, view.side, slot.objective))
        {
            slot.task = TaskKind::None;
            slot.objective = -1;
        }
    }

    std::vector<float> force(count, 0.0f);
    for (const Slot& slot : slots)
        if (slot.task != TaskKind::None && slot.objective >= 0)
            force[slot.objective] += static_cast<float>(slot.group->men);

    const auto nearestFree = [&](int objective, bool allowDepleted) -> Slot*
    {
        Slot* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        for (Slot& slot : slots)
        {
            if (slot.task != TaskKind::None || slot.group->locked)
                continue;
            if (!allowDepleted && slot.group->men <= tuning.depletedMen)
                continue;
            const float d = Distance(slot.group->x, slot.group->y, war.defs[objective].x, war.defs[objective].y);
            if (d < bestDist)
            {
                bestDist = d;
                best = &slot;
            }
        }
        return best;
    };
    const auto fill = [&](int objective, TaskKind task, float need, const char* why)
    {
        while (force[objective] < need)
        {
            Slot* slot = nearestFree(objective, task == TaskKind::Defend);
            if (!slot)
                return;
            assign(*slot, task, objective, why);
            force[objective] += static_cast<float>(slot->group->men);
        }
    };

    // 1b. A quiet objective needs one garrison, not the whole assault that took it: keep the
    // first defenders up to garrisonMen and free the rest for the next target.
    for (int i = 0; i < count; i++)
    {
        if (!Owns(war, view.side, i) || Threatened(view, i))
            continue;
        float kept = 0.0f;
        for (Slot& slot : slots)
        {
            if (slot.task != TaskKind::Defend || slot.objective != i)
                continue;
            if (slot.group->locked)
            {
                kept += static_cast<float>(slot.group->men);
                continue;
            }
            if (kept < tuning.garrisonMen)
            {
                kept += static_cast<float>(slot.group->men);
                continue;
            }
            slot.task = TaskKind::None;
            slot.objective = -1;
            force[i] -= static_cast<float>(slot.group->men);
        }
    }

    // 2. Defend what is threatened, most valuable first.
    std::vector<int> threatened;
    for (int i = 0; i < count; i++)
        if (Owns(war, view.side, i) && Threatened(view, i))
            threatened.push_back(i);
    std::stable_sort(threatened.begin(), threatened.end(),
                     [&](int a, int b) { return war.defs[a].value > war.defs[b].value; });
    for (int i : threatened)
        fill(i, TaskKind::Defend, std::max(tuning.defendMinMen, tuning.forceRatio * Known(view, i)), "under threat");

    // 3. Attack the best target on the front, then the second best.
    const auto targets = RankTargets(view);
    const int primary = targets.empty() ? -1 : targets[0].first;
    const int secondary = targets.size() < 2 ? -1 : targets[1].first;
    const auto attackNeed = [&](int objective)
    { return std::max(tuning.attackMinMen, tuning.forceRatio * Known(view, objective) + 4.0f); };
    if (primary >= 0)
    {
        fill(primary, TaskKind::Attack, attackNeed(primary), "main effort");
        // Not enough: rear garrisons have nothing to guard against.
        if (force[primary] < attackNeed(primary))
            for (Slot& slot : slots)
            {
                if (force[primary] >= attackNeed(primary))
                    break;
                if (slot.task != TaskKind::Defend || slot.group->men <= tuning.depletedMen || slot.group->locked)
                    continue;
                if (!Rear(view, slot.objective) || Threatened(view, slot.objective))
                    continue;
                force[slot.objective] -= static_cast<float>(slot.group->men);
                assign(slot, TaskKind::Attack, primary, "rear garrison to the main effort");
                force[primary] += static_cast<float>(slot.group->men);
            }
    }
    if (secondary >= 0)
        fill(secondary, TaskKind::Attack, attackNeed(secondary), "second effort");

    // 4. Whoever is left: fit groups join the main effort, worn-out ones hold the nearest base.
    for (Slot& slot : slots)
    {
        if (slot.task != TaskKind::None || slot.group->locked)
            continue;
        if (primary >= 0 && slot.group->men > tuning.depletedMen)
        {
            assign(slot, TaskKind::Attack, primary, "reinforcing the main effort");
            continue;
        }
        int nearest = -1;
        float bestDist = std::numeric_limits<float>::max();
        for (int i = 0; i < count; i++)
        {
            if (!Owns(war, view.side, i))
                continue;
            const float d = Distance(slot.group->x, slot.group->y, war.defs[i].x, war.defs[i].y);
            if (d < bestDist)
            {
                bestDist = d;
                nearest = i;
            }
        }
        if (nearest >= 0)
            assign(slot, TaskKind::Defend, nearest, "regrouping");
    }

    // 5. Reinforcements at the eligible base nearest the main effort.
    float pool = view.pool;
    int live = view.live;
    for (int buy = 0; buy < tuning.maxBuysPerPlan; buy++)
    {
        if (pool < view.squadCost || live + view.squadSize > view.cap)
            break;
        int base = -1;
        float bestDist = std::numeric_limits<float>::max();
        for (int i = 0; i < count; i++)
        {
            if (i >= static_cast<int>(view.canSpawn.size()) || !view.canSpawn[i])
                continue;
            const float d = primary >= 0 ? Distance(war.defs[i].x, war.defs[i].y, war.defs[primary].x, war.defs[primary].y)
                                         : static_cast<float>(i);
            if (d < bestDist)
            {
                bestDist = d;
                base = i;
            }
        }
        if (base < 0)
            break;
        Intent intent;
        intent.kind = Intent::Kind::BuySquad;
        intent.objective = base;
        intent.why = primary >= 0 ? "reinforcements for the front" : "reinforcements";
        intents.push_back(intent);
        pool -= view.squadCost;
        live += view.squadSize;
    }
    return intents;
}
} // namespace Poseidon::Conquest
