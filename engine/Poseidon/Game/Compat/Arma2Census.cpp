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
}
} // namespace Poseidon::Arma1Compat
