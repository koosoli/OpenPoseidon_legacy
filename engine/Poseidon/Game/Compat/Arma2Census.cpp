// Asset census for Arma 1/2 missions: what every entity of the running mission became.
//
// An Arma 2 class is either carried by a CWA class (its CWA model, config and behaviour) or
// imported as A1x_<class> with the Arma model on a CWA base. The census lists, once the
// mission runs, each kind of entity with its class, model, LOD count, crew and weapons, so a
// test run says which Arma 2 assets are really in the game and which are stand-ins.
#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Vehicles/Transport.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace Poseidon::Arma1Compat
{
namespace
{
std::string TypeName(const Entity* e)
{
    const EntityType* t = e ? e->GetNonAIType() : nullptr;
    return t ? std::string((const char*)t->_className) : std::string("?");
}

std::string Describe(Entity* e)
{
    const EntityType* t = e->GetNonAIType();
    std::string line = TypeName(e);
    // [arma] = drawn with an Arma model; [cwa] = a CWA model (a CWA class, or an A1x_
    // class that kept its CWA base's model, as soldiers do without OP_A2_SOLDIERS).
    std::string shapeName = t ? std::string((const char*)t->GetShapeName()) : std::string();
    std::transform(shapeName.begin(), shapeName.end(), shapeName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool cwaModel = shapeName.rfind("data3d\\", 0) == 0 || shapeName.rfind("\\data3d\\", 0) == 0 ||
                          shapeName.empty();
    line += cwaModel ? " [cwa]" : " [arma]";
    if (t)
    {
        const LODShapeWithShadow* shape = t->GetShape();
        line += " model=" + std::string((const char*)t->GetShapeName());
        line += " lods=" + std::to_string(shape ? shape->NLevels() : 0);
    }
    if (const auto* ai = dyn_cast<EntityAI>(e))
    {
        if (ai->NWeaponSystems() > 0)
        {
            line += " weapons=";
            for (int i = 0; i < ai->NWeaponSystems(); i++)
            {
                const WeaponType* w = ai->GetWeaponSystem(i);
                if (i)
                    line += ",";
                line += w ? std::string((const char*)w->GetName()) : std::string("?");
            }
        }
    }
    if (const auto* tr = dyn_cast<Transport>(e))
    {
        const auto crew = [&](const char* seat, const Person* p)
        {
            if (p)
                line += std::string(" ") + seat + "=" + TypeName(p);
        };
        crew("driver", tr->Driver());
        crew("gunner", tr->Gunner());
        crew("commander", tr->Commander());
    }
    return line;
}
} // namespace

void LogAssetCensus()
{
    if (!Active() || !GWorld)
        return;
    std::map<std::string, int> kinds;
    int arma = 0, cwa = 0;
    const auto add = [&](Entity* e)
    {
        if (!e)
            return;
        const std::string line = Describe(e);
        (line.find(" [arma]") != std::string::npos ? arma : cwa)++;
        kinds[line]++;
        if (const auto* tr = dyn_cast<Transport>(e))
            for (Person* p : {tr->Driver(), tr->Gunner(), tr->Commander()})
                if (p)
                {
                    const std::string crew = Describe(p) + " (crew)";
                    (crew.find(" [arma]") != std::string::npos ? arma : cwa)++;
                    kinds[crew]++;
                }
    };
    for (int i = 0; i < GWorld->NVehicles(); i++)
        add(GWorld->GetVehicle(i));
    for (int i = 0; i < GWorld->NOutVehicles(); i++)
        add(GWorld->GetOutVehicle(i));
    LOG_INFO(Mission, "{}: asset census: {} entities with the Arma model, {} with a CWA model, {} kinds", Label(), arma,
             cwa, kinds.size());
    for (const auto& [line, count] : kinds)
        LOG_INFO(Mission, "{}: census {}x {}", Label(), count, line);

    // Where the player stands and what the map has around him: a unit on a ship or a
    // roof stands on a map object, and a missing one shows as a soldier on the water.
    Object* on = GWorld->CameraOn();
    if (on && GLandscape)
    {
        const Vector3 p = on->Position();
        const float surface = GLandscape->SurfaceY(p.X(), p.Z());
        LOG_INFO(Mission, "{}: census player {} at {:.1f},{:.1f},{:.1f} (surface {:.1f}, above it {:.1f})", Label(),
                 TypeName(dyn_cast<Entity>(on)), p.X(), p.Y(), p.Z(), surface, p.Y() - surface);
        std::map<std::string, std::pair<int, float>> near; // model -> count, nearest distance
        const int cx = static_cast<int>(p.X() * InvLandGrid), cz = static_cast<int>(p.Z() * InvLandGrid);
        const int reach = static_cast<int>(150.0f * InvLandGrid) + 1;
        for (int z = cz - reach; z <= cz + reach; z++)
            for (int x = cx - reach; x <= cx + reach; x++)
            {
                if (x < 0 || z < 0 || x >= LandRange || z >= LandRange)
                    continue;
                const ObjectList& list = GLandscape->GetObjects(z, x);
                for (int i = 0; i < list.Size(); i++)
                {
                    const Object* o = list[i];
                    if (!o || !o->GetShape() || dyn_cast<const EntityAI>(o))
                        continue;
                    const float d = o->Position().Distance(p);
                    if (d > 150.0f)
                        continue;
                    auto& entry = near[std::string((const char*)o->GetShape()->Name())];
                    if (entry.first == 0 || d < entry.second)
                        entry.second = d;
                    entry.first++;
                }
            }
        int listed = 0;
        for (const auto& [model, info] : near)
            if (listed++ < 40)
                LOG_INFO(Mission, "{}: census near player {}x {} (nearest {:.0f} m)", Label(), info.first, model,
                         info.second);
        LOG_INFO(Mission, "{}: census {} map models within 150 m of the player", Label(), near.size());
    }
}
} // namespace Poseidon::Arma1Compat
