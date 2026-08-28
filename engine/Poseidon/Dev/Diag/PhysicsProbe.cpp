#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Poseidon::Dev
{
namespace
{

PhysicsProbeSettings s_settings;

// Line pool, same shape as the ballistics trail view: reuse one Object per
// segment rather than allocating per frame, and stop at a budget so a runaway
// probe count cannot stall the frame.
std::vector<Ref<LODShapeWithShadow>> s_lineShapes;
std::vector<Ref<Object>>             s_lineObjects;
int                                  s_poolUsed = 0;
constexpr int                        SegmentBudget = 4096;

/// Segments per great circle. Twelve reads as a circle at the ranges a dropped
/// ball is watched from and keeps a dozen probes inside the budget.
constexpr int CircleSegments = 12;

void SubmitLine(Vector3Par from, Vector3Par to, PackedColor color)
{
    if (s_poolUsed >= SegmentBudget || !GScene)
    {
        return;
    }
    if (s_poolUsed >= static_cast<int>(s_lineObjects.size()))
    {
        Ref<LODShapeWithShadow> shape = ObjectLine::CreateShape();
        Ref<Object>             obj = new ObjectLineDiag(shape);
        s_lineShapes.push_back(shape);
        s_lineObjects.push_back(obj);
    }

    LODShapeWithShadow* shape = s_lineShapes[static_cast<std::size_t>(s_poolUsed)];
    Object*             obj = s_lineObjects[static_cast<std::size_t>(s_poolUsed)];
    ++s_poolUsed;

    obj->SetConstantColor(color);
    obj->SetPosition(from);
    ObjectLine::SetPos(shape, VZero, to - from);
    GScene->ObjectForDrawing(obj);
}

/// One great circle in the plane spanned by `a` and `b`.
void SubmitCircle(Vector3Par centre, float radius, Vector3Par a, Vector3Par b, PackedColor color)
{
    Vector3 previous = centre + a * radius;
    for (int i = 1; i <= CircleSegments; ++i)
    {
        const float angle = 6.28318530718f * static_cast<float>(i) / static_cast<float>(CircleSegments);
        const Vector3 point = centre + (a * std::cos(angle) + b * std::sin(angle)) * radius;
        SubmitLine(previous, point, color);
        previous = point;
    }
}

} // namespace

PhysicsProbeSettings& ProbeSettings() { return s_settings; }

void SpawnPhysicsProbe(float throwSpeed)
{
    if (!GWorld || !GScene)
    {
        return;
    }
    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
    if (!world.Create())
    {
        return;
    }

    // Spawned in FRONT of the camera, not at it: a sphere born inside the near
    // plane is invisible for the one moment that matters, and one born inside the
    // player's own collider starts by being pushed out of it.
    // Position()/Direction() on the camera, NOT GetInvTransform().Position():
    // that is the world-to-camera matrix and its translation is not a world
    // position at all. The first run spawned every probe kilometres away over the
    // sea floor, which read as "physics ignores the terrain" until the ground
    // height under the spawn point came back as -72 m with the player visibly
    // standing on grass.
    const Vector3 forward = GScene->GetCamera()->Direction();
    const Vector3 origin = GScene->GetCamera()->Position() + forward * 2.0f;

    Physics::SphereProbeDef def;
    def.position = origin;
    def.velocity = throwSpeed > 0.0f ? forward * throwSpeed : VZero;
    def.radius = s_settings.radius;
    def.mass = s_settings.mass;
    def.friction = s_settings.friction;
    def.restitution = s_settings.restitution;

    const Physics::BodyId id = world.SpawnSphereProbe(def);
    LOG_WARN(World, "PHYSPROBE: spawn r={} m={} kg at ({:.2f} {:.2f} {:.2f}) v={:.1f} -> {}", def.radius, def.mass,
             def.position[0], def.position[1], def.position[2], throwSpeed, id.IsValid() ? "ok" : "REFUSED");
}

void ClearPhysicsProbes()
{
    if (Physics::PhysicsWorld* world = Physics::GetPhysicsWorld())
    {
        world->ClearProbes();
    }
}

void UpdatePhysicsProbeWind()
{
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }

    Physics::WindSettings wind;
    wind.enabled = s_settings.applyWind;
    wind.drag = s_settings.windDrag;
    wind.lift = 0.0f;
    wind.maxSpeed = 100.0f;
    // The world's own wind, not an invented one -- the point of the check is
    // whether Poseidon's weather can move a Box3D body, so inventing a vector
    // here would prove nothing.
    wind.velocity = GLandscape ? GLandscape->GetWind() * s_settings.windScale : VZero;
    world->SetWind(wind);
}

void AutoDropPhysicsProbes()
{
    // POSEIDON_PHYSICS_PROBE=<n> drops n spheres a few seconds after the world
    // settles. Exists so PHY-020 can be VERIFIED without a mouse: the acceptance
    // check is a screenshot of a ball resting on Everon, and a dev button cannot
    // be pressed from a capture harness.
    static const int wanted = []
    {
        const char* v = std::getenv("POSEIDON_PHYSICS_PROBE");
        return v ? std::atoi(v) : 0;
    }();
    if (wanted <= 0)
    {
        return;
    }

    // Delayed, not immediate: the corpus registration runs on the first simulated
    // frame and a ball dropped in the same frame would land before the colliders
    // it is meant to land on exist.
    static int  frames = 0;
    static bool dropped = false;
    if (dropped || ++frames < 120)
    {
        return;
    }
    dropped = true;
    LOG_WARN(World, "PHYSPROBE: auto-drop {} probe(s)", wanted);

    // Spread across the view so at least one is likely to meet an object rather
    // than all of them stacking on the same patch of ground.
    for (int i = 0; i < wanted; ++i)
    {
        SpawnPhysicsProbe(i == 0 ? 0.0f : static_cast<float>(i) * 3.0f);
    }
}

void DrawPhysicsProbes()
{
    s_poolUsed = 0;
    if (!s_settings.draw || !GScene)
    {
        return;
    }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }

    static std::vector<Physics::ProbeSample> samples;
    world->GetProbeSamples(samples);

    // Periodic height trace. The screenshot is the nice-to-have; THIS is the
    // evidence that a body fell and came to rest on real terrain, and it survives
    // the ball being 20 cm across and hidden in waist-high grass.
    static int traceFrame = 0;
    if (!samples.empty() && (++traceFrame % 60) == 0)
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const Vector3& p = samples[i].position;
            const float ground = GLandscape ? GLandscape->SurfaceY(p[0], p[2]) : 0.0f;
            LOG_WARN(World, "PHYSPROBE: probe {} at ({:.2f} {:.2f} {:.2f}) ground {:.2f} clearance {:.3f}", i, p[0],
                     p[1], p[2], ground, p[1] - ground);
        }
    }

    // Three axis-aligned great circles. A silhouette circle facing the camera
    // would read more cleanly, but axis-aligned rings also show ROTATION once the
    // ball starts rolling, which is exactly what this check is looking for.
    const PackedColor color = PackedColor(0xff40e0ff);
    for (const Physics::ProbeSample& sample : samples)
    {
        SubmitCircle(sample.position, sample.radius, VForward, VAside, color);
        SubmitCircle(sample.position, sample.radius, VForward, VUp, color);
        SubmitCircle(sample.position, sample.radius, VAside, VUp, color);
    }
}

} // namespace Poseidon::Dev
