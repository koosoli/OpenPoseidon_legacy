#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>

#include <Poseidon/World/Scene/Thing.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Dev/Diag/PhysicsRayAudit.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Poseidon::Dev
{
namespace
{

/// How many distinct failing models to name in the log. The list is sorted by
/// instance count, so the cut falls on the long tail -- and the total count is
/// printed beside it so a truncated list never reads as a complete one.
constexpr int MaxNamedFailures = 40;

int s_subdivision = []
{
    const char* v = std::getenv("POSEIDON_PHYSICS_TERRAIN_SUBDIV");
    const int   parsed = v ? std::atoi(v) : 0;
    // 12 by default. 3061x3061 samples, about 37 MB of heights plus Box3D's own
    // copy -- expensive for a renderer, nothing for a dev tool, and it is what
    // stops bodies sinking into hillsides.
    return (parsed >= 1 && parsed <= 16) ? parsed : 12;
}();

struct FailureGroup
{
    std::uint32_t instances = 0;
    Physics::RegisterResult::Outcome outcome = Physics::RegisterResult::Outcome::NoGeometryLod;
    int maxPoints = 0;
};

/// What one DISTINCT model turned out to carry, for the LOD-kind census.
///
/// Per model rather than per instance, and that is the whole point: a map is
/// mostly the same forty props copied thousands of times, so an instance count
/// answers "how much of this map has fire geometry" while a model count answers
/// "how much of this content was authored with it". The second is the one that
/// says whether flagging colliders buys anything.
struct KindGroup
{
    std::uint32_t         instances = 0;
    Physics::ColliderFlags resolved = Physics::ColliderFlags::None;
    Physics::ColliderFlags dedicated = Physics::ColliderFlags::None;
    Physics::ColliderFlags registered = Physics::ColliderFlags::None;
};

const char* OutcomeName(Physics::RegisterResult::Outcome outcome)
{
    switch (outcome)
    {
        case Physics::RegisterResult::Outcome::Registered: return "registered";
        case Physics::RegisterResult::Outcome::NoGeometryLod: return "no-geometry-lod";
        case Physics::RegisterResult::Outcome::AllComponentsDegenerate: return "all-components-degenerate";
        case Physics::RegisterResult::Outcome::BackendRefused: return "backend-refused";
    }
    return "?";
}

} // namespace

bool PhysicsCorpusRequested()
{
    static const bool requested = []
    {
        const char* v = std::getenv("POSEIDON_PHYSICS_CORPUS");
        return v && std::strcmp(v, "0") != 0;
    }();
    return requested;
}

namespace
{
bool s_hasRun = false;
bool s_reportCost = false;
int  s_costFrames = 0;
}

bool PhysicsCorpusHasRun() { return s_hasRun; }

void ReportPhysicsCost()
{
    if (!s_reportCost || ++s_costFrames % 600 != 0)
    {
        return;
    }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }
    const Physics::PhysicsStats stats = world->GetStats();
    LOG_WARN(World, "PHYSCOST: {:.3f} ms mean, {:.3f} ms worst over {} steps; {} bodies {} shapes",
             stats.meanStepMs, stats.worstStepMs, stats.stepsTimed, stats.bodies, stats.shapes);
    if (RayAuditEnabled())
    {
        const RayAuditStats a = GetRayAuditStats();
        LOG_WARN(World, "PHYSRAY: {} segments | agree hit {} miss {} | legacy-only {} | physics-only {}", a.segments,
                 a.agreeHit, a.agreeMiss, a.legacyOnly, a.physicsOnly);
    }
}

int&  TerrainSubdivision() { return s_subdivision; }
float TerrainSampleSpacing() { return LandGrid / static_cast<float>(s_subdivision); }

void RunPhysicsCorpus()
{
    if (!GLandscape)
    {
        LOG_WARN(World, "PHYSCORPUS: no landscape loaded, nothing to census");
        return;
    }

    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
    // Rebuilt from scratch, so changing the subdivision and pressing the button
    // again actually changes the colliders instead of adding a second copy of the
    // map on top of the first.
    if (world.IsCreated())
    {
        world.Destroy();
    }
    if (!world.Create())
    {
        LOG_WARN(World, "PHYSCORPUS: no physics backend available");
        return;
    }
    LOG_INFO(World, "PHYSCORPUS: backend {}", world.BackendName());

    // ---- terrain -----------------------------------------------------------
    // The landscape stores heights as GetHeight(z, x); the backend wants a
    // row-major grid. Copied rather than aliased because the backend keeps a
    // reference for the lifetime of the shape.
    const int range = GLandscape->GetLandRange();
    if (range > 1)
    {
        // SUBDIVIDED. The landscape grid is 50 m, and between two samples Box3D
        // spans a straight chord while the rendered ground curves above it. On a
        // hillside that chord cuts the corner by metres, so a ball sinks into the
        // slope and never finds the surface you can see -- which is exactly what
        // the owner reported on Desert ("clips through the mountain").
        //
        // SurfaceY interpolates, so sampling finer costs nothing but memory:
        // 4x gives 12.5 m spacing, 1024x1024 samples, 4 MB. Raising it further
        // has a real cost and buys less each time, because past a point the
        // remaining error is the engine's own interpolation rather than ours.
        const int subdiv = s_subdivision;
        const int grid = (range - 1) * subdiv + 1;
        const float spacing = LandGrid / static_cast<float>(subdiv);

        std::vector<float> heights(static_cast<std::size_t>(grid) * grid);
        for (int z = 0; z < grid; ++z)
        {
            for (int x = 0; x < grid; ++x)
            {
                heights[static_cast<std::size_t>(z) * grid + x] =
                    GLandscape->SurfaceY(static_cast<float>(x) * spacing, static_cast<float>(z) * spacing);
            }
        }

        // Cross-check against the engine's own surface query. A height field that
        // is subtly wrong is indistinguishable from a right one until something
        // falls through it.
        {
            const auto  mm = std::minmax_element(heights.begin(), heights.end());
            const int   sx = grid / 2;
            const int   sz = grid / 2;
            const float gridHeight = heights[static_cast<std::size_t>(sz) * grid + sx];
            const float surfaceY = GLandscape->SurfaceY(static_cast<float>(sx) * spacing,
                                                        static_cast<float>(sz) * spacing);
            LOG_WARN(World,
                     "PHYSCORPUS: heights min {:.2f} max {:.2f}; grid[{},{}]={:.2f} SurfaceY={:.2f} spacing {:.2f} m",
                     *mm.first, *mm.second, sx, sz, gridHeight, surfaceY, spacing);
        }

        const bool ok = world.SetTerrain(heights.data(), grid, grid, spacing, 0.0f, 0.0f);
        LOG_INFO(World, "PHYSCORPUS: terrain {}x{} at {:.2f} m (subdiv {}) -> {}", grid, grid, spacing, subdiv,
                 ok ? "registered" : "REFUSED");
    }

    // ---- objects -----------------------------------------------------------
    // Grouped by model name, not by instance: PHY-010 asks which DISTINCT models
    // cannot convert. Instance counts ride along so a single bad fence copied a
    // thousand times cannot masquerade as a systematic failure.
    std::map<std::string, FailureGroup> failures;

    // Every model, not only the failing ones: the LOD-kind question is about what
    // the corpus HAS, and a census that only looks at failures cannot answer it.
    std::map<std::string, KindGroup> kinds;

    // THE LOOSE OBJECTS, counted rather than assumed. The open question before
    // handing anything to physics was whether the barrels and pallets a map
    // actually contains are Things at all -- or terrain decoration with no entity
    // behind them, which physics can neither move nor be asked about. Grouped
    // by config class, with the network id, because a body the physics owns has
    // to belong to exactly one machine and an object with no id belongs to none.
    std::map<std::string, std::uint32_t> things;
    std::uint32_t                        thingsTotal = 0;
    std::uint32_t                        thingsWithNetworkId = 0;
    std::uint32_t                        thingsLocal = 0;

    // THE CONTROL. Zero Things is only meaningful if this grid holds entities at
    // all -- if it is the WRP's static decoration and nothing else, then counting
    // Things here would report zero however many the map has, and the number
    // would be an artefact of where I looked rather than a fact about the map.
    // The mission puts a player on the map, so Entity must not come back zero.
    std::uint32_t entitiesSeen = 0;
    std::uint32_t vehiclesSeen = 0;

    std::uint32_t                       instancesSeen = 0;
    std::uint32_t                       instancesRegistered = 0;
    std::uint32_t                       instancesNoShape = 0;
    int                                 maxPoints = 0;

    for (int z = 0; z < range; ++z)
    {
        for (int x = 0; x < range; ++x)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            for (int i = 0; i < list.Size(); ++i)
            {
                Object* object = list[i];
                if (!object)
                {
                    continue;
                }
                if (dyn_cast<Entity>(object))
                {
                    ++entitiesSeen;
                }
                if (dyn_cast<Vehicle>(object))
                {
                    ++vehiclesSeen;
                }
                if (Thing* thing = dyn_cast<Thing>(object))
                {
                    ++thingsTotal;
                    ++things[static_cast<const char*>(thing->GetName())];
                    const NetworkId id = thing->GetNetworkId();
                    if (id.creator != 0 || id.id != 0)
                    {
                        ++thingsWithNetworkId;
                    }
                    if (thing->IsLocal())
                    {
                        ++thingsLocal;
                    }
                }

                LODShape* shape = object->GetShape();
                if (!shape)
                {
                    ++instancesNoShape;
                    continue;
                }
                ++instancesSeen;

                const Physics::RegisterResult result =
                    world.RegisterStaticDetailed(shape, object->WorldTransform());
                maxPoints = std::max(maxPoints, result.maxComponentPoints);

                {
                    const char* kindName = shape->Name();
                    KindGroup&  kind = kinds[kindName ? kindName : "<unnamed>"];
                    ++kind.instances;
                    kind.resolved |= result.lodKindsResolved;
                    kind.dedicated |= result.lodKindsDedicated;
                    kind.registered |= result.flagsRegistered;
                }

                if (result.outcome == Physics::RegisterResult::Outcome::Registered)
                {
                    ++instancesRegistered;
                    continue;
                }

                const char*  name = shape->Name();
                FailureGroup& group = failures[name ? name : "<unnamed>"];
                ++group.instances;
                group.outcome = result.outcome;
                group.maxPoints = std::max(group.maxPoints, result.maxComponentPoints);
            }
        }
    }

    const Physics::PhysicsStats stats = world.GetStats();

    LOG_WARN(World, "PHYSCORPUS: ---- PHY-010 corpus census ----");
    LOG_WARN(World, "PHYSCORPUS: instances seen {}, registered {}, no shape at all {}", instancesSeen,
             instancesRegistered, instancesNoShape);
    LOG_WARN(World, "PHYSCORPUS: distinct models that failed: {}", failures.size());
    LOG_WARN(World, "PHYSCORPUS: bodies {}, shapes {}", stats.bodies, stats.shapes);
    LOG_WARN(World, "PHYSCORPUS: models without usable geometry {}, degenerate components {}, backend hull refusals {}",
             stats.modelsWithoutGeometry, stats.degenerateComponents, stats.hullFailures);
    // The number that decides whether MaxHullVertices is a guard or a silent cap.
    LOG_WARN(World, "PHYSCORPUS: max ConvexComponent points observed: {}", maxPoints);

    // ---- LOD kinds ---------------------------------------------------------
    // The measurement behind the collider flags. `dedicated` is the load-bearing
    // column: a flag whose LOD always resolves back onto Geometry costs nothing
    // and also distinguishes nothing, so the size of that column is exactly how
    // much the filter is worth on this content.
    {
        std::uint32_t withFire = 0, dedicatedFire = 0;
        std::uint32_t withView = 0, dedicatedView = 0;
        std::uint32_t withRoadway = 0, roadwayRegistered = 0;
        std::uint32_t instFire = 0, instView = 0, instRoadway = 0;
        for (const auto& [modelName, kind] : kinds)
        {
            (void)modelName;
            using Physics::ColliderFlags;
            if (Physics::HasAny(kind.resolved, ColliderFlags::BulletCollision))
            {
                ++withFire;
            }
            if (Physics::HasAny(kind.dedicated, ColliderFlags::BulletCollision))
            {
                ++dedicatedFire;
                instFire += kind.instances;
            }
            if (Physics::HasAny(kind.resolved, ColliderFlags::ViewBlocking))
            {
                ++withView;
            }
            if (Physics::HasAny(kind.dedicated, ColliderFlags::ViewBlocking))
            {
                ++dedicatedView;
                instView += kind.instances;
            }
            if (Physics::HasAny(kind.resolved, ColliderFlags::Roadway))
            {
                ++withRoadway;
                instRoadway += kind.instances;
                if (Physics::HasAny(kind.registered, ColliderFlags::Roadway))
                {
                    ++roadwayRegistered;
                }
            }
        }
        LOG_WARN(World, "PHYSLOD: {} distinct models walked", static_cast<std::uint32_t>(kinds.size()));
        LOG_WARN(World, "PHYSLOD: fire geometry -- {} resolve to one, {} authored their own ({} instances)",
                 withFire, dedicatedFire, instFire);
        LOG_WARN(World, "PHYSLOD: view geometry -- {} resolve to one, {} authored their own ({} instances)",
                 withView, dedicatedView, instView);
        // Separate line because roadway is the one kind that can be PRESENT and
        // still register nothing: it is authored as a surface, not as named
        // ComponentNN volumes, so there is no convex set to hull.
        LOG_WARN(World, "PHYSLOD: roadway -- {} models carry one ({} instances), {} produced a hullable collider",
                 withRoadway, instRoadway, roadwayRegistered);
        LOG_WARN(World, "PHYSLOD: query-only shapes attached: {} of {} total", stats.queryOnlyShapes, stats.shapes);
    }

    std::vector<std::pair<std::string, FailureGroup>> sorted(failures.begin(), failures.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.instances > b.second.instances; });

    const int shown = std::min<int>(MaxNamedFailures, static_cast<int>(sorted.size()));
    for (int i = 0; i < shown; ++i)
    {
        LOG_WARN(World, "PHYSCORPUS:   {} x{} {} (max points {})", sorted[i].first.c_str(),
                 sorted[i].second.instances, OutcomeName(sorted[i].second.outcome), sorted[i].second.maxPoints);
    }
    if (static_cast<int>(sorted.size()) > shown)
    {
        // Never let a truncated list read as a complete one.
        LOG_WARN(World, "PHYSCORPUS:   ... and {} more distinct models not listed",
                 static_cast<int>(sorted.size()) - shown);
    }
    // The loose-object answer, in the log rather than in a design note.
    LOG_WARN(World, "PHYSTHINGS: control -- {} entities, {} vehicles in the same walk", entitiesSeen,
             vehiclesSeen);
    LOG_WARN(World, "PHYSTHINGS: {} loose objects ({} distinct classes); {} carry a network id, {} local",
             thingsTotal, static_cast<std::uint32_t>(things.size()), thingsWithNetworkId, thingsLocal);
    for (const auto& [className, count] : things)
    {
        LOG_WARN(World, "PHYSTHINGS:   {} x{}", className.c_str(), count);
    }

    LOG_WARN(World, "PHYSCORPUS: ---- end ----");
    // Cost, logged periodically after the world is built. Without a number here
    // "the physics is cheap" is an assertion, and this project has a rule about
    // those.
    s_reportCost = true;
    s_hasRun = true;
}

} // namespace Poseidon::Dev
