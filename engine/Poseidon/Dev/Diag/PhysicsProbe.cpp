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
    Vector3       origin = GScene->GetCamera()->Position() + forward * 2.0f;

    if (s_settings.placeOnGround && throwSpeed <= 0.0f)
    {
        // Walk the view ray out to 100 m and stop where it meets the ground, then
        // sit the body on top of it. Deliberately a march against SurfaceY rather
        // than a physics cast: the physics world may hold only terrain, and a
        // domino placed against a collider the player cannot see is worse than one
        // placed on the visible ground.
        const Vector3 eye = GScene->GetCamera()->Position();
        float         hit = -1.0f;
        for (float t = 1.0f; t < 100.0f; t += 0.25f)
        {
            const Vector3 p = eye + forward * t;
            if (GLandscape && p[1] <= GLandscape->SurfaceY(p[0], p[2]))
            {
                hit = t;
                break;
            }
        }
        if (hit > 0.0f)
        {
            const Vector3 p = eye + forward * hit;
            const float   ground = GLandscape->SurfaceY(p[0], p[2]);
            // Half the body's own height above the surface, plus a millimetre, so
            // it starts resting rather than interpenetrating and being shoved out.
            float lift = s_settings.radius;
            if (s_settings.shape == 2)
            {
                lift = s_settings.boxY;
            }
            else if (s_settings.shape == 3)
            {
                lift = s_settings.halfLength;
            }
            origin = Vector3(p[0], ground + lift + 0.001f, p[2]);
        }
    }

    Physics::SphereProbeDef def;
    def.position = origin;
    def.velocity = throwSpeed > 0.0f ? forward * throwSpeed : VZero;
    def.radius = s_settings.radius;
    def.mass = s_settings.mass;
    def.friction = s_settings.friction;
    def.restitution = s_settings.restitution;
    def.rollingResistance = s_settings.rollingResistance;
    def.shape = s_settings.shape == 1 ? Physics::ProbeShape::Capsule : Physics::ProbeShape::Sphere;
    def.halfLength = s_settings.halfLength;
    def.halfExtents = Vector3(s_settings.boxX, s_settings.boxY, s_settings.boxZ);
    if (s_settings.shape == 2)
    {
        def.shape = Physics::ProbeShape::Box;
    }
    else if (s_settings.shape == 3)
    {
        def.shape = Physics::ProbeShape::Cylinder;
    }

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

    // POSEIDON_PHYSICS_PROBE_RADIUS overrides the slider for harness runs. A
    // 0.2 m ball is the realistic default and is also invisible in waist-high
    // grass from ten metres, which is not a useful thing to photograph.
    if (const char* sh = std::getenv("POSEIDON_PHYSICS_PROBE_SHAPE"))
    {
        s_settings.shape = std::atoi(sh) == 1 ? 1 : 0;
    }
    if (const char* r = std::getenv("POSEIDON_PHYSICS_PROBE_RADIUS"))
    {
        const float radius = static_cast<float>(std::atof(r));
        if (radius > 0.0f)
        {
            s_settings.radius = radius;
        }
    }
    LOG_WARN(World, "PHYSPROBE: auto-drop {} probe(s) r={}", wanted, s_settings.radius);

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
        if (sample.shape == Physics::ProbeShape::Box)
        {
            // Twelve edges from the three rotated half-axes. A box has to show its
            // ORIENTATION or a toppled domino looks identical to a standing one.
            const Vector3 hx = sample.axisX * sample.halfExtents[0];
            const Vector3 hy = sample.axisY * sample.halfExtents[1];
            const Vector3 hz = sample.axisZ * sample.halfExtents[2];
            Vector3       corner[8];
            for (int i = 0; i < 8; ++i)
            {
                corner[i] = sample.position + (i & 1 ? hx : -hx) + (i & 2 ? hy : -hy) + (i & 4 ? hz : -hz);
            }
            static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                            {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edge)
            {
                SubmitLine(corner[e[0]], corner[e[1]], color);
            }
            continue;
        }

        if (sample.shape == Physics::ProbeShape::Capsule || sample.shape == Physics::ProbeShape::Cylinder)
        {
            // Capsule lies along local X, cylinder stands along local Y.
            const bool    upright = sample.shape == Physics::ProbeShape::Cylinder;
            const Vector3 axis = upright ? sample.axisY : sample.axisX;
            const Vector3 a = upright ? sample.axisX : sample.axisY;
            const Vector3 b = sample.axisZ;
            const Vector3 half = axis * sample.halfLength;
            const Vector3 e1 = sample.position - half;
            const Vector3 e2 = sample.position + half;
            SubmitCircle(e1, sample.radius, a, b, color);
            SubmitCircle(e2, sample.radius, a, b, color);
            SubmitLine(e1 + a * sample.radius, e2 + a * sample.radius, color);
            SubmitLine(e1 - a * sample.radius, e2 - a * sample.radius, color);
            SubmitLine(e1 + b * sample.radius, e2 + b * sample.radius, color);
            SubmitLine(e1 - b * sample.radius, e2 - b * sample.radius, color);
            continue;
        }

        SubmitCircle(sample.position, sample.radius, sample.axisX, sample.axisZ, color);
        SubmitCircle(sample.position, sample.radius, sample.axisX, sample.axisY, color);
        SubmitCircle(sample.position, sample.radius, sample.axisZ, sample.axisY, color);
    }
}

} // namespace Poseidon::Dev
