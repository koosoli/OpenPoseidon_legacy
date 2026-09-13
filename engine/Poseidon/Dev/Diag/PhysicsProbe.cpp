#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
#include <Poseidon/Dev/Diag/ShowcaseCastle.hpp>
#include <Poseidon/Dev/Diag/ShowcaseContact.hpp>
#include <Poseidon/Core/Global.hpp>

#include <Poseidon/Dev/Diag/ProbeMeshes.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>
#include <cstring>
#include <Poseidon/World/World.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

namespace Poseidon::Dev
{
namespace
{

PhysicsProbeSettings s_settings;
PhysicsProbeSettings s_beforeShowcase;
bool s_showcaseActive = false;
bool s_showcaseCastle = false;
Ref<LightPoint> s_showcaseRoomLight;
bool s_showcaseRoomLit = true;
struct CraterSample { int x, z; float before, after; };
std::vector<CraterSample> s_craterSamples;

bool SetShowcaseCrater(bool restore)
{
    if (!s_showcaseActive || !GLandscape || restore == s_craterSamples.empty()) return false;
    auto& land = *GLandscape;
    const int range = land.GetTerrainRange();
    const float spacing = land.GetTerrainGrid();
    // One explicitly invoked prototype, bounded memory; never an explosion hot path.
    if (range < 2 || range > 2049 || !(spacing > 0) || spacing > 50) return false;
    if (!restore)
    {
        const float radius = std::max(24.0f, spacing * 3.0f);
        for (int z = std::max(0, static_cast<int>((3600-radius)/spacing));
             z < range && z*spacing <= 3600+radius; ++z)
            for (int x = std::max(0, static_cast<int>((9700-radius)/spacing));
                 x < range && x*spacing <= 9700+radius; ++x)
            {
                const float depth = TerrainCraterDepth(x*spacing-9700, z*spacing-3600, radius, 8.0f);
                if (depth > 0)
                {
                    const float old = land.GetHeight(z, x);
                    s_craterSamples.push_back({x,z,old,old-depth});
                }
            }
        if (s_craterSamples.empty()) return false;
    }
    auto& world = Physics::EnsurePhysicsWorld();
    if (!world.Create()) { if (!restore) s_craterSamples.clear(); return false; }
    const auto begin = std::chrono::steady_clock::now();
    const auto stats = world.GetStats();
    // Corpus/loose-object collision may use a different sampling grid.
    if (!stats.terrainRegistered || stats.terrainWidth != range || stats.terrainHeight != range ||
        stats.terrainCellSize != spacing || stats.terrainOriginX != 0 || stats.terrainOriginZ != 0)
    {
        std::vector<float> heights(static_cast<size_t>(range)*range);
        for (int z = 0; z < range; ++z)
            for (int x = 0; x < range; ++x)
                heights[static_cast<size_t>(z)*range+x] = land.GetHeight(z,x);
        if (!world.SetTerrain(heights.data(),range,range,spacing,0,0))
        {
            if (!restore) s_craterSamples.clear();
            return false;
        }
    }
    int left=range, top=range, right=0, bottom=0;
    for (const auto& p : s_craterSamples)
    {
        left=std::min(left,p.x); top=std::min(top,p.z);
        right=std::max(right,p.x); bottom=std::max(bottom,p.z);
    }
    const int width=right-left+1, height=bottom-top+1;
    if (!CaptureTerrainEditBaseline(left,top,width,height))
    {
        if (!restore) s_craterSamples.clear();
        return false;
    }
    std::vector<float> patch(static_cast<size_t>(width)*height);
    for (int z=0; z<height; ++z)
        for (int x=0; x<width; ++x)
            patch[static_cast<size_t>(z)*width+x]=land.GetHeight(top+z,left+x);
    for (const auto& p : s_craterSamples)
        patch[static_cast<size_t>(p.z-top)*width+p.x-left]=restore ? p.before : p.after;
    if (!world.PatchTerrain(left,top,width,height,patch.data()))
    {
        if (!restore) s_craterSamples.clear();
        return false;
    }
    for (const auto& p : s_craterSamples) land.HeightChange(p.x,p.z,restore ? p.before : p.after);
    land.FlushCache();
    LOG_INFO(World, "SHOWCASE terrain edit: patch={}x{} collision/cache={:.3f}ms", width,height,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
    LOG_INFO(World, "SHOWCASE crater: restore={} samples={} spacing={} surface={}", restore,
             s_craterSamples.size(), spacing, land.SurfaceY(9700,3600));
    if (restore) s_craterSamples.clear();
    return true;
}

// Harness seeds. The probe settings live behind a panel that holds the mouse, so an
// automated capture cannot tick a box -- these are the only way to A/B the probe light
// from a command line. They are read ONCE, at first use.
const bool s_probeEnvSeeded = []
{
    if (const char* v = std::getenv("POSEIDON_PROBE_LIGHT"))
        s_settings.emitLight = !(*v == '0');
    if (const char* v = std::getenv("POSEIDON_PROBE_LIGHT_BRIGHTNESS"))
        s_settings.lightBrightness = std::clamp(static_cast<float>(std::atof(v)), 0.05f, 8.0f);
    if (const char* v = std::getenv("POSEIDON_PROBE_UNLIT"))
        s_settings.selfIllum = !(*v == '0');
    if (const char* v = std::getenv("POSEIDON_PROBE_HOTKEYS"))
        s_settings.hotkeys = !(*v == '0');
    if (const char* v = std::getenv("POSEIDON_PROBE_GLOW"))
        s_settings.lightGlow = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 8.0f);
    if (const char* v = std::getenv("POSEIDON_PROBE_LIGHT_OWN_COLOR"))
        s_settings.lightUsesProbeColor = !(*v == '0');
    if (const char* v = std::getenv("POSEIDON_PROBE_SPOT"))
        s_settings.lightSpot = !(*v == '0');
    if (const char* v = std::getenv("POSEIDON_PROBE_CONE"))
        s_settings.lightConeOuter = std::clamp(static_cast<float>(std::atof(v)), 0.05f, 1.4f);
    if (const char* v = std::getenv("POSEIDON_PROBE_AIM_VIEW"))
        s_settings.lightSpotAimView = !(*v == '0');
    return true;
}();

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
// PROBE-LIGHT: one LightPoint per probe, pooled like the solid objects. The scene owns
// them once added, so the pool is only ever grown; a light that is not wanted this frame
// is turned black rather than removed, which keeps the scene's light list stable.
std::vector<Ref<LightPositionedColored>> s_probeLights;
bool s_probeSpotPool = false;
int s_probeLightsUsed = 0;
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

/// PROBE-LIGHT: place (or create) the pooled light for one probe. Returns quietly when
/// lights are off -- the caller still runs, so the "turn the rest black" pass below can
/// tell "no probes this frame" from "lights disabled".
void SubmitProbeLight(Vector3Par position)
{
    if (!GScene || s_probeLightsUsed >= 64)
    {
        return;
    }
    // A cone and a point are different Light subclasses, so a change of kind has to rebuild
    // the pooled light rather than mutate it.
    if (!s_probeLights.empty() && s_probeSpotPool != s_settings.lightSpot)
    {
        for (auto& l : s_probeLights)
        {
            if (l)
            {
                l->SetDiffuse(HBlack);
                l->SetAmbient(HBlack);
            }
        }
        s_probeLights.clear();
    }
    s_probeSpotPool = s_settings.lightSpot;
    if (s_probeLightsUsed >= static_cast<int>(s_probeLights.size()))
    {
        // LightPointVisible, not LightPoint: it draws a halo of its own, which is the GLOW
        // the owner asked for -- what the lamp looks like, as opposed to what it does to
        // the world. The shape is the engine's own preloaded light sprite.
        Ref<LightPositionedColored> light;
        if (s_settings.lightSpot)
        {
            Ref<LightReflector> spot =
                new LightReflector(GScene->Preloaded(HalfLight), HBlack, HBlack, s_settings.lightConeOuter);
            spot->SetOrient(Vector3(0, -1, 0), Vector3(0, 0, 1)); // a lamp points at the floor
            light = spot;
        }
        else
        {
            light = new LightPointVisible(GScene->Preloaded(HalfLight), HBlack, HBlack);
        }
        GScene->AddLight(light);
        s_probeLights.push_back(light);
    }
    LightPositionedColored* light = s_probeLights[static_cast<std::size_t>(s_probeLightsUsed)];
    ++s_probeLightsUsed;
    // The colour you set on the probe is the colour it shines. Anything else makes the
    // Colour picker a lie: you tint the ball red and it lights the street warm white.
    const float* rgb = s_settings.lightUsesProbeColor ? s_settings.solidColor : s_settings.lightColor;
    const Color diffuse(rgb[0], rgb[1], rgb[2], 1.0f);
    light->SetPosition(position);
    light->SetDiffuse(diffuse);
    light->SetAmbient(diffuse * s_settings.lightAmbient);
    if (auto* pv = dynamic_cast<LightPointVisible*>(light))
    {
        pv->SetBrightness(s_settings.lightBrightness);
        pv->SetSize(s_settings.lightGlow);
    }
    else if (auto* rf = dynamic_cast<LightReflector*>(light))
    {
        rf->SetBrightness(s_settings.lightBrightness);
        rf->SetSize(s_settings.lightGlow);
        rf->SetAngle(s_settings.lightConeOuter);
        Vector3 aim(0, -1, 0);
        if (s_settings.lightSpotAimView && GScene != nullptr && GScene->GetCamera() != nullptr)
        {
            aim = GScene->GetCamera()->Direction();
        }
        const Vector3 up = (std::fabs(aim.Y()) > 0.99f) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
        rf->SetOrient(aim, up);
    }
}

} // namespace

// PHY-021: the world's colliders, on demand.
//
// The physics world starts EMPTY. Terrain and static objects are registered only by
// PhysicsCorpus, which until now ran solely from the tab's "Register world colliders"
// button or POSEIDON_PHYSICS_CORPUS. So the first thing anyone did with this tab -- spawn
// a shape -- dropped it into a world with no ground in it, and it fell for ever. Measured
// three separate times while trying to photograph a probe: "ground 16.33 clearance
// -140.200", the landscape knowing exactly where its surface is while the physics had
// never been told.
//
// A probe that falls through the planet reads as a broken feature, not as a missing setup
// step, so the setup step is no longer optional: the first spawn does it.
void EnsureWorldColliders()
{
    if (PhysicsCorpusHasRun())
    {
        return;
    }
    LOG_WARN(World, "PHY-021: registering world colliders on first probe spawn (the physics world was empty)");
    RunPhysicsCorpus();
}

PhysicsProbeSettings& ProbeSettings() { return s_settings; }

void ResetProbeSettings()
{
    // Assignment from a fresh instance, so the defaults are the ones written in
    // the struct and cannot drift out of step with a second copy of them.
    s_settings = PhysicsProbeSettings{};
}

bool SpawnModelProbe(const char* nameFragment, float throwSpeed)
{
    EnsureWorldColliders(); // PHY-021: a model probe takes its own path to the physics world
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

bool PhysicsShowcaseProjectileContact(Vector3Par from, Vector3Par to, Vector3& hit, Vector3& normal)
{
    auto* world = Physics::GetPhysicsWorld();
    if (!s_showcaseActive || !world || !world->IsCreated()) return false;
    std::vector<Physics::ProbeSample> samples;
    world->GetProbeSamples(samples);
    float fraction = 1.0f;
    bool contact = false;
    for (const auto& sample : samples)
        contact |= SweepShowcaseBlock(sample, from, to, 0, 0, fraction, normal);
    if (contact) hit = from + (to-from)*fraction;
    return contact;
}

void PhysicsShowcaseExplosion(Vector3Par position, float hit, float range)
{
    auto* world = Physics::GetPhysicsWorld();
    if (!s_showcaseActive || !world || !world->IsCreated() || !(hit > 0) || !(range > 0)) return;
    std::vector<Physics::ProbeSample> samples;
    world->GetProbeSamples(samples);
    int affected = 0;
    for (const auto& sample : samples)
    {
        Vector3 impulse = ShowcaseBlastImpulse(sample.position-position, hit, range);
        if (impulse.SquareSize() < 0.0001f) continue;
        // Use the existing scenery visibility, so buildings/hills shield the demo.
        const float visible = GLandscape->Visible(position, sample.position, 0.2f, nullptr, nullptr, ObjIntersectIFire);
        if (visible <= 0) continue;
        world->ApplyImpulse(sample.id, sample.position, impulse*visible);
        ++affected;
    }
    LOG_INFO(Physics, "SHOWCASE blast: hit={} range={} affected={}", hit, range, affected);
}

Vector3 PhysicsShowcasePlayerMove(Vector3Par from, Vector3Par to, float height)
{
    auto* world = Physics::GetPhysicsWorld();
    if (!s_showcaseActive || !world || !world->IsCreated()) return to;
    std::vector<Physics::ProbeSample> samples;
    world->GetProbeSamples(samples);
    const float radius = std::max(0.3f, s_settings.playerRadius);
    const Vector3 lift(0, height*0.5f, 0);
    Vector3 position = from+lift, remaining = to-from;
    for (int slide = 0; slide < 4 && remaining.SquareSize() > 1e-10f; ++slide)
    {
        float fraction = 1;
        Vector3 normal = VZero;
        bool contact = false;
        for (const auto& sample : samples)
            contact |= SweepShowcaseBlock(sample, position, position+remaining, radius,
                                          std::max(0.0f, height*0.5f-radius), fraction, normal);
        if (!contact) { position += remaining; break; }
        position += remaining*fraction + normal*0.002f;
        remaining *= 1-fraction;
        remaining -= normal*std::min(0.0f, remaining.DotProduct(normal));
    }
    return position-lift;
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
    EnsureWorldColliders();
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
        const auto placementHeight = [](Vector3Par p)
        {
            float water = GLandscape->GetSeaLevel();
            if (const WaterBody* body = GetWaterBodies().Find(p[0], p[2]))
                water = body->SurfaceLevelAt(p[0], p[2]);
            return floatMax(GLandscape->SurfaceY(p[0], p[2]), water);
        };
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
            if (GLandscape && p[1] <= placementHeight(p))
            {
                hit = t;
                break;
            }
        }
        if (hit > 0.0f)
        {
            const Vector3 p = eye + forward * hit;
            const float   ground = placementHeight(p);
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

bool PhysicsHotkeysEnabled() { return s_settings.hotkeys; }

void ClearPhysicsProbes()
{
    if (Physics::PhysicsWorld* world = Physics::GetPhysicsWorld())
    {
        world->ClearProbes();
    }
    s_modelProbes.clear();
}

void ForgetShowcaseTerrainCrater() { s_craterSamples.clear(); }

void ShutdownPhysicsProbes()
{
    ClearTerrainEditBaseline();
    ResetTerrainBrush();
    EndPhysicsShowcase();
    ClearPhysicsProbes();
    s_solidObjects.clear();
    s_lineObjects.clear();
    s_lineShapes.clear();
    s_probeLights.clear();
    s_poolUsed = s_solidUsed = s_probeLightsUsed = 0;
    ReleaseProbeMeshes();
    if (auto* world = Physics::GetPhysicsWorld()) world->Destroy();
}

void EndPhysicsShowcase()
{
    s_craterSamples.clear();
    if (s_showcaseRoomLight)
        s_showcaseRoomLight->SetDiffuse(HBlack);
    s_showcaseRoomLight = nullptr;
    if (!s_showcaseActive) return;
    ClearPhysicsProbes();
    for (auto& light : s_probeLights)
    {
        light->SetDiffuse(HBlack);
        light->SetAmbient(HBlack);
    }
    s_probeLights.clear();
    s_probeLightsUsed = 0;
    s_settings = s_beforeShowcase;
    s_showcaseActive = false;
}

bool PhysicsShowcaseCommand(const char* command)
{
    if (!command || !GWorld || !GScene || !GScene->GetCamera() || !GLandscape)
        return false;
    const bool reset = !strcmp(command, "reset");
    if (!reset && !s_showcaseActive) return false;
    if (reset)
    {
        s_showcaseCastle = false;
        s_showcaseRoomLit = true;
        if (!s_showcaseActive)
        {
            s_beforeShowcase = s_settings;
            RunPhysicsCorpus(); // The previous mission may have registered a different island.
        }
        s_showcaseActive = true;
        ClearPhysicsProbes();
        ResetProbeSettings();
        s_settings.hotkeys = true;
        s_settings.mass = 2.0f;
        s_settings.radius = 0.3f;
        EnsureWorldColliders();
        auto& world = Physics::EnsurePhysicsWorld();
        if (!world.Create()) return false;
        // Desert campus: native terrain supports the bodies; the surrounding
        // authored architecture is ordinary mission scenery, not physics authority.
        for (int i = 0; i < 24; ++i)
        {
            Physics::SphereProbeDef def;
            def.shape = Physics::ProbeShape::Box;
            def.halfExtents = Vector3(0.42f, 0.3f, 0.42f);
            const float x = 9494.0f + (i % 4) * 0.86f;
            const float z = 3504.0f + ((i / 4) % 2) * 0.86f;
            def.position = Vector3(x, GLandscape->SurfaceY(x, z) + 0.3f + (i / 8) * 0.605f, z);
            def.mass = 2.0f;
            def.friction = 0.6f;
            def.restitution = 0.05f;
            world.SpawnSphereProbe(def);
        }
        for (int i = 0; i < 18; ++i)
        {
            Physics::SphereProbeDef def;
            def.shape = Physics::ProbeShape::Box;
            def.halfExtents = Vector3(0.12f, 0.65f, 0.35f);
            const float x = 9488.0f + i * 0.55f;
            def.position = Vector3(x, GLandscape->SurfaceY(x, 3511.0f) + 0.652f, 3511.0f);
            def.mass = 1.0f;
            def.friction = 0.6f;
            def.restitution = 0.05f;
            world.SpawnSphereProbe(def);
        }
        return true;
    }
    if (!strcmp(command, "roomlight"))
    {
        s_showcaseRoomLit = !s_showcaseRoomLit;
        return true;
    }
    if (!strcmp(command, "crater")) return SetShowcaseCrater(false);
    if (!strcmp(command, "craterreset")) return SetShowcaseCrater(true);
    if (!strcmp(command, "castle"))
    {
        ClearPhysicsProbes();
        s_settings.emitLight = false;
        s_settings.hotkeys = false;
        s_settings.mass = 8.0f;
        s_settings.radius = 0.5f;
        s_settings.throwSpeed = 35.0f;
        s_showcaseCastle = true;
        auto& world = Physics::EnsurePhysicsWorld();
        if (!world.Create()) return false;
        for (const auto& block : ShowcaseCastleLayout())
        {
            Physics::SphereProbeDef def;
            def.shape = Physics::ProbeShape::Box;
            def.halfExtents = block.side ? Vector3(0.39f, 0.4f, 0.99f) : Vector3(block.halfWidth, 0.4f, 0.39f);
            const float x = 9500.0f + block.x, z = 3555.0f + block.z;
            def.position = Vector3(x, GLandscape->SurfaceY(x, z) + block.height, z);
            def.mass = 8.0f * block.halfWidth / 0.99f; // Uniform-density lightweight demo blocks.
            def.friction = 0.65f;
            def.restitution = 0.02f;
            world.SpawnSphereProbe(def);
        }
        return true;
    }
    if (!strcmp(command, "clear")) { ClearPhysicsProbes(); s_showcaseCastle = false; return true; }
    if (!strcmp(command, "ball")) { s_settings.shape = 0; s_settings.emitLight = false; }
    else if (!strcmp(command, "box")) { s_settings.shape = 2; s_settings.emitLight = false; }
    else if (!strcmp(command, "light"))
    {
        ClearPhysicsProbes(); // A deliberate four-light budget, not 42 shadowed lamps.
        s_settings.shape = 0;
        s_settings.emitLight = true;
        s_settings.placeOnGround = false;
        s_settings.hotkeys = false; // Mission actions enforce the four-light budget.
        s_settings.lightUsesProbeColor = false;
        s_settings.lightBrightness = 0.12f;
        s_settings.lightColor[0] = 1.0f;
        s_settings.lightColor[1] = 0.72f;
        s_settings.lightColor[2] = 0.36f;
    }
    else if (!strcmp(command, "cool"))
    {
        s_settings.lightColor[0] = 0.25f;
        s_settings.lightColor[1] = 0.65f;
        s_settings.lightColor[2] = 1.0f;
        return true;
    }
    else if (!strcmp(command, "throw") || !strcmp(command, "drop"))
    {
        std::vector<Physics::ProbeSample> samples;
        if (auto* world = Physics::GetPhysicsWorld()) world->GetProbeSamples(samples);
        if (samples.size() >= (s_settings.emitLight ? 4u : s_showcaseCastle ? 512u : 128u)) return false;
        SpawnPhysicsProbe(!strcmp(command, "throw") ? s_settings.throwSpeed : 0.0f);
        return true;
    }
    else return false;
    if (!s_settings.emitLight) s_settings.placeOnGround = true;
    return true;
}

void UpdatePhysicsProbeWind()
{
    if (s_showcaseActive && GScene && GLandscape)
    {
        if (!s_showcaseRoomLight)
        {
            s_showcaseRoomLight = new LightPoint(HBlack, HBlack);
            s_showcaseRoomLight->SetBrightness(0.1f);
            s_showcaseRoomLight->SetDaylightVisible(true);
            GScene->AddLight(s_showcaseRoomLight);
        }
        const float phase = Glob.time.toFloat() * 0.65f;
        const float x = 9547.0f + std::cos(phase) * 3.0f;
        const float z = 3504.0f + std::sin(phase) * 3.0f;
        s_showcaseRoomLight->SetPosition(Vector3(x, GLandscape->SurfaceY(x, z) + 2.3f, z));
        s_showcaseRoomLight->SetDiffuse(s_showcaseRoomLit ? Color(1.0f, 0.72f, 0.38f) : Color(HBlack));
    }
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
    const Vector3 proxyCentre = position + Vector3(0, s_settings.playerHeight * 0.5f, 0);
    world->SetKinematicProxy(proxyCentre, s_settings.playerRadius,
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
    if (const char* m = std::getenv("POSEIDON_PHYSICS_PROBE_MASS"))
        s_settings.mass = std::clamp(static_cast<float>(std::atof(m)), 0.01f, 10000.0f);

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
        for (std::size_t i = 0; i < (s_showcaseCastle ? std::min<size_t>(samples.size(), 16) : samples.size()); ++i)
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
    auto channel = [](float v) { return static_cast<unsigned>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
    const PackedColor solidColor =
        PackedColor(0xff000000u | (channel(s_settings.solidColor[0]) << 16) |
                    (channel(s_settings.solidColor[1]) << 8) | channel(s_settings.solidColor[2]));
    s_solidUsed = 0;

    // PROBE-LIGHT: a probe you can throw and roll is the only MOVING light source the
    // engine's test scenes have, which is what makes it useful for checking dynamic
    // lighting and the shadows it casts. Lights are placed for every probe, model probes
    // included, whether or not the solid mesh is being drawn.
    s_probeLightsUsed = 0;
    if (s_settings.emitLight)
    {
        for (const Physics::ProbeSample& sample : samples)
        {
            SubmitProbeLight(sample.position);
        }
    }
    // Every pooled light beyond the ones used this frame goes black. They stay in the
    // scene's list -- removing and re-adding lights per frame is how a light list gets
    // corrupted -- but a black light contributes nothing.
    for (std::size_t i = static_cast<std::size_t>(s_probeLightsUsed); i < s_probeLights.size(); ++i)
    {
        s_probeLights[i]->SetDiffuse(HBlack);
        s_probeLights[i]->SetAmbient(HBlack);
    }
    // PROBE-LIGHT: with the probe lit from inside, its own surface should read as the
    // lamp it now is rather than as a dark ball. IsLight is the engine's own unlit
    // routing (BuildRenderPassDescriptor: LightingMode::Unlit).
    SetProbeMeshSelfIllum(s_settings.emitLight && s_settings.selfIllum);

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
