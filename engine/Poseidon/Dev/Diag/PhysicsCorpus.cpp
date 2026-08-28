#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
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

struct FailureGroup
{
    std::uint32_t instances = 0;
    Physics::RegisterResult::Outcome outcome = Physics::RegisterResult::Outcome::NoGeometryLod;
    int maxPoints = 0;
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

void RunPhysicsCorpus()
{
    if (!GLandscape)
    {
        LOG_WARN(World, "PHYSCORPUS: no landscape loaded, nothing to census");
        return;
    }

    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
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
        std::vector<float> heights(static_cast<std::size_t>(range) * range);
        for (int z = 0; z < range; ++z)
        {
            for (int x = 0; x < range; ++x)
            {
                heights[static_cast<std::size_t>(z) * range + x] = GLandscape->GetHeight(z, x);
            }
        }
        const bool ok = world.SetTerrain(heights.data(), range, range, LandGrid, 0.0f, 0.0f);
        LOG_INFO(World, "PHYSCORPUS: terrain {}x{} grid {} m -> {}", range, range, LandGrid,
                 ok ? "registered" : "REFUSED");
    }

    // ---- objects -----------------------------------------------------------
    // Grouped by model name, not by instance: PHY-010 asks which DISTINCT models
    // cannot convert. Instance counts ride along so a single bad fence copied a
    // thousand times cannot masquerade as a systematic failure.
    std::map<std::string, FailureGroup> failures;
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
    LOG_WARN(World, "PHYSCORPUS: ---- end ----");
}

} // namespace Poseidon::Dev
