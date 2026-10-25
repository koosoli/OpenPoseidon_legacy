#include <Poseidon/World/Simulation/DynamicSimulation.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cstring>

namespace Poseidon::DynSim
{
namespace
{
struct State
{
    bool enabled = false;
    // Arma 3's defaults, by Kind
    float distance[static_cast<int>(Kind::Count)] = {500.0f, 350.0f, 250.0f, 50.0f};
    float movingCoef = 2.0f;
    float nextUpdate = -1.0f;
};

State& Current()
{
    static State state;
    return state;
}

// The kind an entity falls under, and the point its distance is measured from: a unit's is
// its group leader's, so a group freezes and wakes together.
Kind KindOf(Entity* entity, Vector3& anchor)
{
    anchor = entity->Position();
    if (Person* man = dyn_cast<Person>(entity))
    {
        if (AIUnit* unit = man->Brain())
            if (AIGroup* group = unit->GetGroup())
                if (AIUnit* leader = group->Leader())
                    if (Person* leaderMan = leader->GetPerson())
                        anchor = leaderMan->Position();
        return Kind::Group;
    }
    if (EntityAI* ai = dyn_cast<EntityAI>(entity))
    {
        if (ai->CommanderUnit())
            return Kind::Vehicle;
        if (ai->GetType()->IsKindOf(GWorld->Preloaded(VTypeAllVehicles)))
            return Kind::EmptyVehicle;
    }
    return Kind::Prop;
}
} // namespace

bool KindFromName(const char* name, Kind& kind)
{
    static const char* const names[] = {"group", "vehicle", "emptyvehicle", "prop"};
    for (int i = 0; i < static_cast<int>(Kind::Count); i++)
        if (name && _stricmp(name, names[i]) == 0)
        {
            kind = static_cast<Kind>(i);
            return true;
        }
    return false;
}

void SetSystemEnabled(bool enabled)
{
    Current().enabled = enabled;
    Current().nextUpdate = -1.0f; // takes effect at the next tick
}

bool SystemEnabled()
{
    return Current().enabled;
}

void SetDistance(Kind kind, float metres)
{
    if (kind != Kind::Count)
        Current().distance[static_cast<int>(kind)] = std::max(0.0f, metres);
}

float Distance(Kind kind)
{
    return kind != Kind::Count ? Current().distance[static_cast<int>(kind)] : 0.0f;
}

void SetMovingCoef(float coef)
{
    Current().movingCoef = std::max(1.0f, coef);
}

float MovingCoef()
{
    return Current().movingCoef;
}

void Reset()
{
    Current() = State();
}

void Update(World& world)
{
    State& state = Current();
    const float now = Glob.time.toFloat();
    // a restarted mission's clock starts over: do not wait for the old one
    if (now < state.nextUpdate && state.nextUpdate - now <= 0.5f)
        return;
    state.nextUpdate = now + 0.5f;

    // what wakes: the player, what the camera is on, and the camera itself
    Person* player = world.PlayerOn();
    Object* cameraOn = world.CameraOn();
    Vector3 viewers[3];
    int nViewers = 0;
    if (player)
        viewers[nViewers++] = player->Position();
    if (cameraOn)
        viewers[nViewers++] = cameraOn->Position();
    if (GScene && GScene->GetCamera())
        viewers[nViewers++] = GScene->GetCamera()->Position();
    AIGroup* playerGroup = player && player->Brain() ? player->Brain()->GetGroup() : nullptr;

    const auto visit = [&](Entity* entity)
    {
        if (!entity || !entity->DynamicSimulationEnabled())
            return;
        bool frozen = false;
        if (state.enabled && nViewers > 0 && entity != player && entity != cameraOn)
        {
            const Person* man = dyn_cast<Person>(entity);
            const bool ownSide = man && man->Brain() && playerGroup && man->Brain()->GetGroup() == playerGroup;
            if (!ownSide)
            {
                Vector3 anchor;
                const Kind kind = KindOf(entity, anchor);
                float reach = state.distance[static_cast<int>(kind)];
                if (entity->Speed().SquareSize() > 1.0f)
                    reach *= state.movingCoef;
                float nearest2 = anchor.Distance2(viewers[0]);
                for (int i = 1; i < nViewers; i++)
                    nearest2 = std::min(nearest2, anchor.Distance2(viewers[i]));
                frozen = nearest2 > reach * reach;
            }
        }
        entity->SetDynamicSimulationFrozen(frozen);
    };
    for (int i = 0; i < world.NVehicles(); i++)
        visit(world.GetVehicle(i));
    for (int i = 0; i < world.NBuildings(); i++)
        visit(world.GetBuilding(i));
}
} // namespace Poseidon::DynSim
