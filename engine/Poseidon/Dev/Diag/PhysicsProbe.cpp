#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>

#include <Poseidon/Dev/Diag/ProbeMeshes.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <cstring>
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

/// One dynamic model copy: the physics body, the shape it was cloned from, and a
/// draw-only Object placed at the body's transform every frame. The Object is NOT
/// in the world's object grid and no entity references it, so nothing in the
/// mission can see or save it.
struct ModelProbe
{
    Physics::BodyId         id;
    Ref<LODShapeWithShadow> shape;
    Ref<Object>             draw;
    /// The temporary entity the shape came from, kept ALIVE. Holding only the
    /// shape was not enough: the jeep drew with white untextured panels because
    /// its textures are referenced by the entity, and letting that die released
    /// them. The lighthouse model taken straight from the landscape drew
    /// correctly, which is what narrowed it to the config path.
    Ref<Entity> source;
};
std::vector<ModelProbe> s_modelProbes;

// Solid probes reuse one Object per drawn body, like the line pool: an Object
// carries a transform, so it cannot be shared between two bodies in one frame.
std::vector<Ref<Object>> s_solidObjects;
int                      s_solidUsed = 0;

/// Submits a unit mesh scaled and rotated into place. Object has no scale of its
/// own, but the orientation is three column vectors -- scaling them scales the
/// model, which is why one unit mesh serves every size.
void SubmitSolid(LODShapeWithShadow* mesh, Vector3Par position, Vector3Par x, Vector3Par y, Vector3Par z,
                 PackedColor color)
{
    if (!mesh || !GScene || s_solidUsed >= 512)
    {
        return;
    }
    if (s_solidUsed >= static_cast<int>(s_solidObjects.size()))
    {
        s_solidObjects.push_back(new ObjectColored(mesh, -1));
    }
    Object* obj = s_solidObjects[static_cast<std::size_t>(s_solidUsed)];
    ++s_solidUsed;

    // The pooled object may have been built around a different mesh last frame.
    obj->SetShape(mesh);
    Matrix3 orientation;
    orientation.SetDirectionAside(x);
    orientation.SetDirectionUp(y);
    orientation.SetDirection(z);
    obj->SetOrientation(orientation);
    obj->SetPosition(position);
    obj->SetConstantColor(color);
    GScene->ObjectForDrawing(obj);
}

} // namespace

PhysicsProbeSettings& ProbeSettings() { return s_settings; }

bool SpawnModelProbe(const char* nameFragment, float throwSpeed)
{
    if (!GWorld || !GScene || !GLandscape || !nameFragment || !*nameFragment)
    {
        return false;
    }
    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
    if (!world.Create())
    {
        return false;
    }

    // Reuse a model the world already loaded rather than naming a file. The dev
    // tool then works on any island without knowing what lives there, and it
    // cannot ask for a path the current data set does not have.
    LODShapeWithShadow* found = nullptr;
    const int           range = GLandscape->GetLandRange();
    for (int z = 0; z < range && !found; ++z)
    {
        for (int x = 0; x < range && !found; ++x)
        {
            const ObjectList& list = GLandscape->GetObjects(z, x);
            for (int i = 0; i < list.Size(); ++i)
            {
                Object* object = list[i];
                LODShapeWithShadow* shape = object ? object->GetShape() : nullptr;
                const char* name = shape ? shape->Name() : nullptr;
                if (name && strstr(name, nameFragment))
                {
                    found = shape;
                    break;
                }
            }
        }
    }
    // Vehicles are NOT in the landscape grid -- that holds buildings, trees and
    // props. A jeep comes from the mission, so the world's vehicle list has to be
    // searched too, or "spawn a jeep" only ever finds scenery.
    if (!found)
    {
        for (int i = 0; i < GWorld->NVehicles(); ++i)
        {
            Entity* vehicle = GWorld->GetVehicle(i);
            LODShapeWithShadow* shape = vehicle ? vehicle->GetShape() : nullptr;
            const char* name = shape ? shape->Name() : nullptr;
            if (name && strstr(name, nameFragment))
            {
                found = shape;
                break;
            }
        }
    }

    // Ask the CONFIG, the way the Zeus tab does. NewNonAIVehicle builds an entity
    // from a class name on any island, so "Jeep" works whether or not one happens
    // to be standing nearby -- which is the whole point, and what the two searches
    // above cannot give. The entity is temporary and never enters the world: only
    // its shape is kept, held by a Ref so it outlives the entity that owned it.
    Ref<Entity>             configEntity;
    Ref<LODShapeWithShadow> loaded;
    if (!found)
    {
        // fullCreate=true: with it false the shape comes back with its levels
        // unresolved and the object draws its GEOMETRY LOD -- the collision hulls
        // render as white boxes over the vehicle.
        configEntity = NewNonAIVehicle(RString(nameFragment), nullptr, true);
        if (configEntity)
        {
            found = configEntity->GetShape();
        }
    }

    // And finally as a literal model path, for something with no config class.
    if (!found)
    {
        loaded = Shapes.New(nameFragment, false, false);
        if (loaded && loaded->NLevels() > 0)
        {
            found = loaded;
        }
    }

    if (!found)
    {
        LOG_WARN(World,
                 "PHYSPROBE: '{}' is not a loaded model, not a config class, and not a model path", nameFragment);
        return false;
    }
    // Keep the shape alive past the temporary entity.
    Ref<LODShapeWithShadow> keep = found;

    const Vector3 forward = GScene->GetCamera()->Direction();
    const Vector3 eye = GScene->GetCamera()->Position();
    Vector3       origin = eye + forward * 8.0f;
    if (GLandscape)
    {
        // Lift by the model's OWN lower extent, not a flat two metres. A model's
        // origin is wherever the artist put it -- often at the base, sometimes at
        // the axle line -- so a fixed offset buries anything whose geometry
        // reaches below the origin. The first test did exactly that and the body
        // spent its first second being violently ejected from the ground, which
        // looks like a physics fault and is a placement one.
        const float bottom = found->Min()[1];
        origin[1] = GLandscape->SurfaceY(origin[0], origin[2]) - bottom + 0.5f;
    }

    Matrix4 transform(MIdentity);
    transform.SetPosition(origin);
    if (throwSpeed > 0.0f)
    {
        // Thrown: start it in the air in front of the camera rather than on the
        // ground, or the first thing it does is scrape.
        origin = eye + forward * 6.0f;
        transform.SetPosition(origin);
    }

    const Physics::BodyId id =
        world.SpawnModelProbe(found, transform, s_settings.modelMass, s_settings.friction, s_settings.restitution,
                              throwSpeed > 0.0f ? forward * throwSpeed : VZero);
    if (!id.IsValid())
    {
        LOG_WARN(World, "PHYSPROBE: model '{}' has no usable Geometry LOD", found->Name());
        return false;
    }

    ModelProbe probe;
    probe.id = id;
    probe.shape = keep;
    probe.source = configEntity;
    probe.draw = new Object(found, -1);
    s_modelProbes.push_back(probe);
    LOG_WARN(World, "PHYSPROBE: model probe '{}' at ({:.1f} {:.1f} {:.1f}) mass {}", found->Name(), origin[0],
             origin[1], origin[2], s_settings.modelMass);
    return true;
}

void PhysicsProbeOnProjectileHit(Vector3Par from, Vector3Par to, float momentum)
{
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }
    Vector3               hit;
    const Physics::BodyId id = world->CastRay(from, to, hit);
    if (!id.IsValid())
    {
        return;
    }
    Vector3 dir = to - from;
    const float len = dir.Size();
    if (len < 1e-4f)
    {
        return;
    }
    dir = dir / len;
    // `momentum` is the projectile's SPEED, several hundred m/s. Multiplying that
    // by a scale of 40 gave a bullet the punch of a truck -- the owner's "objects
    // fly much too far". Treated as a real projectile mass instead: about 4 g,
    // which makes the product genuine newton-seconds, and the scale then means
    // what it says rather than compounding an already-large number.
    constexpr float BulletMassKg = 0.004f;
    world->ApplyImpulse(id, hit, dir * (momentum * BulletMassKg * s_settings.hitImpulseScale));
}

void SpawnPhysicsProbe(float throwSpeed)
{
    if (!GWorld || !GScene)
    {
        return;
    }
    if (s_settings.shape == 4)
    {
        SpawnModelProbe(s_settings.modelFilter, throwSpeed);
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
    // Face where the camera faces, so successive pieces line up along the path
    // being walked rather than all pointing at world north.
    def.yaw = atan2(forward[0], forward[2]);
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

void SpawnBrickTower(int count)
{
    const int wasShape = s_settings.shape;
    const bool wasPlace = s_settings.placeOnGround;
    s_settings.shape = 2;
    s_settings.placeOnGround = false;

    if (!GScene || !GLandscape)
    {
        return;
    }
    const Vector3 forward = GScene->GetCamera()->Direction();
    const Vector3 eye = GScene->GetCamera()->Position();
    Vector3       base = eye + forward * 6.0f;
    base[1] = GLandscape->SurfaceY(base[0], base[2]);

    Physics::PhysicsWorld& world = Physics::EnsurePhysicsWorld();
    if (world.Create())
    {
        const float yaw = atan2(forward[0], forward[2]);
        for (int i = 0; i < count; ++i)
        {
            Physics::SphereProbeDef def;
            def.shape = Physics::ProbeShape::Box;
            def.halfExtents = Vector3(s_settings.boxX, s_settings.boxY, s_settings.boxZ);
            // A hair of clearance per layer. Bricks born touching are born
            // interpenetrating, and the solver answers by launching the tower.
            def.position = base + Vector3(0, s_settings.boxY * (2.0f * i + 1.0f) + 0.002f * i, 0);
            // Quarter turn on alternate courses, as a real wall is laid. Aligned
            // blocks are a set of independent columns and topple at a touch.
            def.yaw = yaw + (i % 2 ? 1.5707963f : 0.0f);
            def.mass = s_settings.mass;
            def.friction = s_settings.friction;
            def.restitution = s_settings.restitution;
            def.rollingResistance = s_settings.rollingResistance;
            world.SpawnSphereProbe(def);
        }
    }

    s_settings.shape = wasShape;
    s_settings.placeOnGround = wasPlace;
}

void PlaceProbeAtCrosshair() { SpawnPhysicsProbe(0.0f); }

void ClearPhysicsProbes()
{
    if (Physics::PhysicsWorld* world = Physics::GetPhysicsWorld())
    {
        world->ClearProbes();
    }
    s_modelProbes.clear();
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

Vector3 s_proxyCentre;
bool    s_proxyLive = false;

void UpdatePhysicsPlayerProxy(Vector3Par position)
{
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }
    // Height 0 removes it, so switching the checkbox off actually takes the
    // capsule out of the world rather than leaving an invisible one standing
    // where the player last was.
    s_proxyCentre = position + Vector3(0, s_settings.playerHeight * 0.5f, 0);
    s_proxyLive = s_settings.playerProxy;
    world->SetKinematicProxy(s_proxyCentre, s_settings.playerRadius,
                             s_settings.playerProxy ? s_settings.playerHeight : 0.0f);
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
    if (const char* nd = std::getenv("POSEIDON_PHYSICS_PROBE_NODRAW"))
    {
        s_settings.draw = std::atoi(nd) == 0;
    }
    if (const char* sh = std::getenv("POSEIDON_PHYSICS_PROBE_SHAPE"))
    {
        // 0..3, not a bool. The first version accepted only 0 or 1, so asking
        // for a box silently got a sphere -- and the resulting screenshot looked
        // like the solid renderer was broken when it was working perfectly.
        const int parsed = std::atoi(sh);
        s_settings.shape = (parsed >= 0 && parsed <= 3) ? parsed : 0;
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
    if (const char* m = std::getenv("POSEIDON_PHYSICS_PROBE_MODEL"))
    {
        // Model probes cannot be reached from a capture run any other way, and
        // "does the jeep work" is not a question to answer by assertion.
        SpawnModelProbe(m);
        return;
    }
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
    const PackedColor solidColor = PackedColor(0xff2f9fd0);
    s_solidUsed = 0;

    if (s_settings.drawSolid)
    {
        for (const Physics::ProbeSample& sample : samples)
        {
            switch (sample.shape)
            {
                case Physics::ProbeShape::Box:
                    SubmitSolid(ProbeBoxMesh(), sample.position, sample.axisX * sample.halfExtents[0],
                                sample.axisY * sample.halfExtents[1], sample.axisZ * sample.halfExtents[2],
                                solidColor);
                    break;
                case Physics::ProbeShape::Sphere:
                    SubmitSolid(ProbeSphereMesh(), sample.position, sample.axisX * sample.radius,
                                sample.axisY * sample.radius, sample.axisZ * sample.radius, solidColor);
                    break;
                case Physics::ProbeShape::Cylinder:
                    SubmitSolid(ProbeCylinderMesh(), sample.position, sample.axisX * sample.radius,
                                sample.axisY * sample.halfLength, sample.axisZ * sample.radius, solidColor);
                    break;
                case Physics::ProbeShape::Capsule:
                    // Drawn as a cylinder of the same extent. The hemispherical
                    // ends are missing, which is honest enough for a probe and
                    // avoids a fourth mesh for a shape nobody stacks.
                    SubmitSolid(ProbeCylinderMesh(), sample.position, sample.axisY * sample.radius,
                                sample.axisX * (sample.halfLength + sample.radius), sample.axisZ * sample.radius,
                                solidColor);
                    break;
                default: break;
            }
        }
        if (!s_settings.drawOutline)
        {
            // Model probes still draw below.
            goto drawModels;
        }
    }

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

drawModels:
    // The player proxy, drawn so a probe that is NOT being pushed can be told
    // apart from a proxy that is in the wrong place. Without seeing it, "I walk
    // through things" and "the capsule is at my chest" look identical.
    if (s_proxyLive && s_settings.draw)
    {
        const PackedColor proxyColor = PackedColor(0xffffc040);
        const float       half = s_settings.playerHeight * 0.5f;
        SubmitCircle(s_proxyCentre - Vector3(0, half, 0), s_settings.playerRadius, VForward, VAside, proxyColor);
        SubmitCircle(s_proxyCentre + Vector3(0, half, 0), s_settings.playerRadius, VForward, VAside, proxyColor);
        SubmitCircle(s_proxyCentre, s_settings.playerRadius, VForward, VAside, proxyColor);
    }

    // Model probes draw as the real model, placed at the body's transform. The
    // Object is submitted straight to the scene and lives nowhere else -- it is
    // not in the object grid, no entity holds it, and the mission cannot save it.
    for (ModelProbe& probe : s_modelProbes)
    {
        if (!probe.draw)
        {
            continue;
        }
        for (const Physics::ProbeSample& sample : samples)
        {
            if (sample.id != probe.id)
            {
                continue;
            }
            Matrix3 orientation;
            orientation.SetDirectionAside(sample.axisX);
            orientation.SetDirectionUp(sample.axisY);
            orientation.SetDirection(sample.axisZ);
            probe.draw->SetOrientation(orientation);
            probe.draw->SetPosition(sample.position);
            GScene->ObjectForDrawing(probe.draw);
            break;
        }
    }
}

} // namespace Poseidon::Dev
