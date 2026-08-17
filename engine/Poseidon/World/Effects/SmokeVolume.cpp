#include <Poseidon/World/Effects/SmokeVolume.hpp>

#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{

/// The volume's own entity type. Shares the name the legacy smoke uses, so
/// anything that filters the world by type name treats the two identically.
VehicleNonAIType* SmokeVolumeType()
{
    static Ref<VehicleNonAIType> type;
    if (!type)
    {
        type = VehicleTypes.New("smoke");
    }
    return type;
}

/// Blend flags a cloudlet billboard needs. Identical to the set
/// Smokes.cpp::ApplyCloudletShapeSpecials applies, and applied to the same
/// shared preloaded shape, so the new smoke and the legacy smoke composite the
/// same way. That equality is the point: an A/B comparison in which the two
/// systems also blend differently would prove nothing.
constexpr int kCloudletSpecials =
    ClampU | ClampV | NoZWrite | IsAlpha | IsAlphaFog | NoShadow | IsColored | IsAlphaOrdered;

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

} // namespace

SmokeVolume::SmokeVolume(LODShapeWithShadow* shape, const SmokeParams& params)
    : Entity(shape, SmokeVolumeType(), -1), _params(params)
{
    // TypeTempVehicle keeps the volume out of every collision query in the
    // engine, including SmokeWorldQuery's own sweeps. Plumes therefore cannot
    // collide with each other or with themselves — correct, and free.
    Object::_type = TypeTempVehicle;

    if (shape != nullptr)
    {
        shape->OrSpecial(kCloudletSpecials);
    }

    // The whole plume ticks as one entity, so it must tick at a sensible rate
    // rather than at the "far cloudlet" rate the legacy per-puff entities use.
    SetSimulationPrecision(1.0f / 30.0f);

    _particles.reserve(static_cast<std::size_t>(std::max(_params.maxParticles, 1)));
    _drawOrder.reserve(_particles.capacity());
}

SmokeVolume::~SmokeVolume() = default;

SimulationImportance SmokeVolume::WorstImportance() const
{
    return SimulateVisibleFar;
}

SimulationImportance SmokeVolume::BestImportance() const
{
    return SimulateVisibleNear;
}

float SmokeVolume::RandUnit()
{
    // xorshift32. A private stream on purpose: pulling from GRandGen would make
    // the number of smoke particles alive change every downstream random draw
    // in the mission, which is exactly the trap WindModel.cpp documents.
    _rand ^= _rand << 13;
    _rand ^= _rand >> 17;
    _rand ^= _rand << 5;
    return static_cast<float>(_rand & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

float SmokeVolume::RandSymmetric()
{
    return RandUnit() * 2.0f - 1.0f;
}

void SmokeVolume::SetEmitterPosition(Vector3Par position)
{
    _emitterPosition = position;
}

void SmokeVolume::Emit(float deltaT)
{
    if (!_emitting)
    {
        return;
    }

    if (_duration >= 0.0f)
    {
        _duration -= deltaT;
        if (_duration <= 0.0f)
        {
            _emitting = false;
        }
    }

    _emitAccumulator += _params.rate * deltaT;

    while (_emitAccumulator >= 1.0f)
    {
        _emitAccumulator -= 1.0f;

        if (static_cast<int>(_particles.size()) >= _params.maxParticles)
        {
            // At the cap, stop accumulating rather than letting the debt build:
            // otherwise lowering the particle count in the dev panel is followed
            // by a burst as the backlog drains.
            _emitAccumulator = 0.0f;
            break;
        }

        SmokeParticle particle;
        particle.lifetime = _params.particleLifetime * (0.80f + 0.40f * RandUnit());

        const float r = _params.emitterRadius * std::sqrt(RandUnit());
        const float theta = RandUnit() * 6.2831853f;
        particle.position = _emitterPosition + Vector3(r * std::cos(theta), 0.0f, r * std::sin(theta));

        particle.velocity = Vector3(_params.initialSpeed * _params.spread * RandSymmetric(),
                                    _params.initialSpeed * (0.7f + 0.3f * RandUnit()),
                                    _params.initialSpeed * _params.spread * RandSymmetric());

        particle.sizeJitter = 0.75f + 0.5f * RandUnit();
        particle.alphaJitter = 0.7f + 0.6f * RandUnit();

        // Jittered so the whole plume does not sweep on the same frame. A plume
        // that sweeps in lockstep costs the same on average and produces a
        // periodic hitch instead of a flat cost.
        particle.sweepTimer = _params.sweepInterval * RandUnit();
        particle.shelterTimer = RandUnit() * 0.5f;

        // Born with the shelter state unknown; the first probe runs on the first
        // tick. Starting at "exposed" and easing in would make a grenade thrown
        // into a room visibly get blown sideways before settling.
        particle.exposure = GSmokeWorldQuery().IsSheltered(particle.position, _params.shelterProbeHeight) ? 0.0f : 1.0f;

        _particles.push_back(particle);
    }
}

void SmokeVolume::Integrate(SmokeParticle& particle, float deltaT)
{
    const ISmokeWorldQuery& query = GSmokeWorldQuery();

    // -- shelter ----------------------------------------------------------
    particle.shelterTimer -= deltaT;
    if (particle.shelterTimer <= 0.0f)
    {
        // Half-second cadence with jitter. A roof does not move; what moves is
        // the particle, and at smoke speeds half a second is a fraction of a
        // metre. Probing every frame would double the query cost for nothing.
        particle.shelterTimer = 0.4f + 0.3f * RandUnit();
        const bool sheltered = query.IsSheltered(particle.position, _params.shelterProbeHeight);
        const float target = sheltered ? 0.0f : 1.0f;
        // Eased rather than switched, so a particle drifting out of a doorway
        // accelerates into the wind over a metre or so instead of snapping.
        particle.exposure = Lerp(particle.exposure, target, 0.35f);
    }

    // -- air --------------------------------------------------------------
    // Prevailing wind is gated by shelter; local airflow is gated by it too. A
    // rotor hovering over a roof does not stir the room underneath, for the same
    // reason the wind does not.
    Vector3 air(VZero);
    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        const float scale =
            _params.windResponse * Lerp(_params.shelteredWindScale, 1.0f, std::clamp(particle.exposure, 0.0f, 1.0f));
        air[0] = wind.velocityX * scale;
        air[2] = wind.velocityZ * scale;
    }
    air += GAirflow.SampleLocal(particle.position) * std::clamp(particle.exposure, 0.0f, 1.0f);

    // -- forces -----------------------------------------------------------
    const float lifeFraction = std::clamp(particle.age / std::max(particle.lifetime, 1e-3f), 0.0f, 1.0f);

    // Relative-velocity drag, not "wind added to position". This is what makes
    // the wind controls in the dev panel behave: the particle is accelerated
    // toward the air it is in, so it lags a gust and coasts through a lull.
    Vector3 acceleration = (air - particle.velocity) * _params.drag;

    // Buoyancy dies as the plume cools, which is what forms the flat drifting
    // layer at the top of a real plume.
    const float heat = 1.0f - std::clamp(lifeFraction / std::max(_params.buoyancyDecay, 1e-3f), 0.0f, 1.0f);
    acceleration[1] += _params.buoyancy * heat;

    if (_params.turbulence > 0.0f)
    {
        acceleration[0] += _params.turbulence * RandSymmetric();
        acceleration[1] += _params.turbulence * RandSymmetric() * 0.5f;
        acceleration[2] += _params.turbulence * RandSymmetric();
    }

    particle.velocity += acceleration * deltaT;
}

void SmokeVolume::Collide(SmokeParticle& particle, Vector3Par previousPosition, float deltaT)
{
    const ISmokeWorldQuery& query = GSmokeWorldQuery();

    // -- terrain ----------------------------------------------------------
    // Always, and cheaply: a heightmap lookup, not a geometry query. Smoke
    // sinking into a hillside is the most visible failure of the legacy system
    // and it does not deserve a sphere sweep to fix.
    // FloorHeight, not GroundHeight: inside a house the surface smoke pools on
    // is the floor slab, and the terrain under it may be a metre lower.
    const float ground = query.FloorHeight(particle.position.X(), particle.position.Z());
    const float floorY = ground + 0.05f;
    if (particle.position.Y() < floorY)
    {
        particle.position[1] = floorY;
        if (particle.velocity.Y() < 0.0f)
        {
            particle.velocity[1] *= -_params.restitution;
        }
        // Ground contact drags the horizontal flow, which is what makes smoke
        // pool and creep rather than skate.
        particle.velocity[0] *= (1.0f - _params.friction * deltaT * 4.0f);
        particle.velocity[2] *= (1.0f - _params.friction * deltaT * 4.0f);
    }

    if (!_params.collide)
    {
        particle.planeValid = false;
        return;
    }

    const float sphereRadius =
        std::max(Lerp(_params.startRadius, _params.endRadius,
                      std::clamp(particle.age / std::max(particle.lifetime, 1e-3f), 0.0f, 1.0f)) *
                     _params.collisionRadiusScale,
                 0.05f);

    // -- cached plane -----------------------------------------------------
    // Between real sweeps the last contact plane is still enforced. This is what
    // makes the budget honest: the sweep rate is reduced, the collision is not.
    if (particle.planeValid)
    {
        const float distance = particle.planeNormal.DotProduct(particle.position) - particle.planeOffset;
        if (distance < 0.0f)
        {
            particle.position += particle.planeNormal * (-distance);
            const float into = particle.velocity.DotProduct(particle.planeNormal);
            if (into < 0.0f)
            {
                particle.velocity -= particle.planeNormal * (into * (1.0f + _params.restitution));
                particle.velocity *= (1.0f - _params.friction);
            }
        }
    }

    // -- real sweep -------------------------------------------------------
    particle.sweepTimer -= deltaT;
    if (particle.sweepTimer > 0.0f)
    {
        return;
    }
    particle.sweepTimer = _params.sweepInterval * (0.75f + 0.5f * RandUnit());

    // Sweep past where the particle is now, out to where it will be by the time
    // it next sweeps. Finding the wall before reaching it is what keeps a thin
    // wall from being tunnelled through between sweeps.
    const Vector3 lookahead = particle.position + particle.velocity * _params.sweepInterval;
    const SmokeSurfaceHit hit = query.SweepSphere(previousPosition, lookahead, sphereRadius);
    ++_diagSweeps;

    if (!hit.hit)
    {
        particle.planeValid = false;
        return;
    }
    ++_diagHits;
    if (_diagHitLog < 12)
    {
        ++_diagHitLog;
        LOG_INFO(World, "  hit: p=({:.1f},{:.1f},{:.1f}) v=({:.1f},{:.1f},{:.1f}) n=({:.2f},{:.2f},{:.2f}) at y={:.2f}",
                 particle.position.X(), particle.position.Y(), particle.position.Z(), particle.velocity.X(),
                 particle.velocity.Y(), particle.velocity.Z(), hit.normal.X(), hit.normal.Y(), hit.normal.Z(),
                 hit.position.Y());
    }

    // Cache the contact as a half-space and place the particle just outside it.
    particle.planeNormal = hit.normal;
    particle.planeOffset = hit.normal.DotProduct(hit.position) + sphereRadius * 0.5f;
    particle.planeValid = true;

    const float distance = particle.planeNormal.DotProduct(particle.position) - particle.planeOffset;
    if (distance < 0.0f)
    {
        particle.position += particle.planeNormal * (-distance);
    }

    const float into = particle.velocity.DotProduct(particle.planeNormal);
    if (into < 0.0f)
    {
        // Remove the inward component (plus a little rebound) and shed some of
        // the tangential. What survives is flow ALONG the surface, which is why
        // this gives ceiling pooling and wall-crawling without either being
        // special-cased.
        particle.velocity -= particle.planeNormal * (into * (1.0f + _params.restitution));
        particle.velocity *= (1.0f - _params.friction);
    }
}

void SmokeVolume::UpdateBounds()
{
    if (_particles.empty())
    {
        SetPosition(_emitterPosition);
        SetOrientScaleOnly(std::max(_params.startRadius, 0.5f));
        return;
    }

    // Centroid and enclosing radius, recomputed every frame. The scene culls and
    // sorts on these, so a stale bound shows up as a plume that vanishes when
    // its emitter leaves the frustum.
    Vector3 centre(VZero);
    for (const SmokeParticle& particle : _particles)
    {
        centre += particle.position;
    }
    centre /= static_cast<float>(_particles.size());

    float radius2 = 0.0f;
    for (const SmokeParticle& particle : _particles)
    {
        radius2 = std::max(radius2, (particle.position - centre).SquareSize());
    }

    SetPosition(centre);

    // GetRadius() is BoundingSphere() * Scale(), so the scale must be divided
    // by the shape's own sphere or the culling radius is off by that factor.
    // cl_basic's sphere is not 1.0; using the metres directly under-reported
    // the bound and let the frustum test drop a plume whose edge was on screen.
    const float shapeSphere = _shape != nullptr ? std::max(_shape->BoundingSphere(), 1e-3f) : 1.0f;
    const float radiusMetres = std::sqrt(radius2) + _params.endRadius;
    SetOrientScaleOnly(radiusMetres / shapeSphere);
}

void SmokeVolume::Simulate(float deltaT, SimulationImportance prec)
{
    const auto begin = std::chrono::steady_clock::now();

    if (deltaT > 0.0f)
    {
        // A long frame (loading, alt-tab) integrated in one step throws
        // particles through walls and past their contact planes. Clamp rather
        // than substep: smoke that lags a hitch by a frame is invisible, smoke
        // that has been flung across a room is not.
        deltaT = std::min(deltaT, 0.10f);

        Emit(deltaT);

        std::size_t write = 0;
        for (std::size_t read = 0; read < _particles.size(); ++read)
        {
            SmokeParticle& particle = _particles[read];

            particle.age += deltaT;
            if (particle.age >= particle.lifetime)
            {
                continue; // dropped by not being written back
            }

            const Vector3 previous = particle.position;

            Integrate(particle, deltaT);
            particle.position += particle.velocity * deltaT;
            Collide(particle, previous, deltaT);

            if (write != read)
            {
                _particles[write] = particle;
            }
            ++write;
        }
        _particles.resize(write);

        UpdateBounds();
    }

    // A spent volume removes itself. `_delete` is the same mechanism the legacy
    // Smoke uses, so teardown goes through exactly one path.
    if (!_emitting && _particles.empty())
    {
        _delete = true;
    }

    _lastSimulateUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
}

void SmokeVolume::Draw(int level, ClipFlags clipFlags, const FrameBase& frame)
{
    const auto begin = std::chrono::steady_clock::now();
    _lastDrawUs = 0.0;

    // Every early return below used to be silent, which made "the smoke does
    // nothing" impossible to tell apart from "the smoke is never drawn". Each
    // one now names itself, at most once a second per volume.
    const auto bail = [&](const char* reason)
    {
        if (Glob.time > _nextDiagTime)
        {
            _nextDiagTime = Glob.time + 1.0f;
            LOG_WARN(World, "SmokeVolume draw skipped: {} (level={} particles={})", reason, level, _particles.size());
        }
    };

    if (_particles.empty())
    {
        return; // not an error: an emitter that has not produced anything yet
    }
    if (level == LOD_INVISIBLE || _shape == nullptr || GScene == nullptr || GEngine == nullptr)
    {
        bail("invisible LOD or missing shape/scene/engine");
        return;
    }

    Shape* shape = _shape->LevelOpaque(level);
    if (shape == nullptr)
    {
        bail("LevelOpaque(level) is null");
        return;
    }
    if (shape->NFaces() != 1)
    {
        bail("cloudlet shape does not have exactly one face");
        return;
    }

    Texture* texture = shape->FaceIndexed(0).GetTexture();
    MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
    if (!mip.IsOK())
    {
        bail("UseMipmap failed (no cloudlet texture)");
        return;
    }

    const int special = shape->Special() | GetObjSpecial();
    const render::LegacySpec specT = render::SplitLegacy(special);
    const bool alphaFog = render::Has(specT.backend, render::Backend::IsAlphaFog);

    const Camera& camera = *GScene->GetCamera();
    const Matrix4Val project = camera.Projection();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const float invTop = camera.InvTop();

    // ONE lighting solve for the whole plume.
    //
    // Object::DrawDecal does this per billboard: a full sun material solve plus
    // a pass over every active point light, for every puff. Smoke is a diffuse
    // blob metres across; the difference between lighting it at its own centre
    // and at the plume's centre is not visible, and the difference in cost is
    // the entire reason the legacy system is expensive with a big plume up.
    Color lit;
    {
        TLMaterial material;
        material.ambient = HWhite;
        material.diffuse = HWhite * 0.25f;
        material.emmisive = HBlack;
        material.forcedDiffuse = HWhite * 0.25f;
        material.specFlags = 0;

        LightSun* sun = GScene->MainLight();
        sun->SetMaterial(material);
        lit = sun->FullResult(0.5f);

        const float nightFactor = sun->NightEffect();
        if (nightFactor > 0.01f)
        {
            const LightList& lights = GScene->ActiveLights();
            if (lights.Size() > 0)
            {
                Matrix4 worldToModel(MTranslation, -Position());
                for (int i = 0; i < lights.Size(); ++i)
                {
                    lights[i]->Prepare(worldToModel);
                    lights[i]->SetMaterial(material);
                }
                for (int i = 0; i < lights.Size(); ++i)
                {
                    lit += lights[i]->Apply(VZero, VForward);
                }
            }
        }

        lit = lit * GEngine->GetAccomodateEye();
    }

    lit = lit * Color(_params.red, _params.green, _params.blue, 1.0f);

    // -- back-to-front ordering -------------------------------------------
    // The scene sorts OBJECTS. Inside one object the order is ours, and alpha
    // billboards that are not sorted show their draw order as hard seams where
    // puffs overlap. Sorting indices rather than particles keeps the simulation
    // array stable and costs one pass.
    const Matrix4Val toView = GScene->ScaledInvTransform();

    _drawOrder.clear();
    _drawOrder.reserve(_particles.size());
    for (int i = 0; i < static_cast<int>(_particles.size()); ++i)
    {
        _drawOrder.push_back(i);
    }
    std::sort(_drawOrder.begin(), _drawOrder.end(),
              [&](int a, int b)
              {
                  return (toView * _particles[a].position).Z() > (toView * _particles[b].position).Z();
              });

    const Vector3 cameraPosition = camera.Position();
    int issued = 0;

    for (const int index : _drawOrder)
    {
        const SmokeParticle& particle = _particles[index];

        const float lifeFraction = std::clamp(particle.age / std::max(particle.lifetime, 1e-3f), 0.0f, 1.0f);

        // Fade up out of the nozzle, hold, fade out. Without the fade-in a
        // particle appears at full opacity the instant it is born, which reads
        // as flicker at the source.
        float fade = 1.0f;
        if (lifeFraction < _params.fadeIn && _params.fadeIn > 0.0f)
        {
            fade = lifeFraction / _params.fadeIn;
        }
        else if (lifeFraction > 1.0f - _params.fadeOut && _params.fadeOut > 0.0f)
        {
            fade = (1.0f - lifeFraction) / _params.fadeOut;
        }

        const float alpha = std::clamp(_params.opacity * particle.alphaJitter * fade, 0.0f, 1.0f);
        if (alpha <= 0.004f)
        {
            continue;
        }

        const float size =
            Lerp(_params.startRadius, _params.endRadius, lifeFraction) * particle.sizeJitter;

        Vector3 posp = toView * particle.position;
        if (posp.Z() < nearest)
        {
            continue;
        }

        // Same projection Object::DrawDecal performs for x/y; see Object.cpp.
        const float invW = 1.0f / posp[2];
        posp[0] = project(0, 2) + project(0, 0) * posp[0] * invW;
        posp[1] = project(1, 2) + project(1, 1) * posp[1] * invW;

        // DEPTH IS THE PARTICLE'S TRUE CENTRE, NOT SIZE-BIASED.
        //
        // Object::DrawDecal writes the billboard's depth at (z - size): the puff
        // is pulled toward the camera by its own radius so a big soft blob is
        // not knifed by the ground it is sitting on. Sensible for one puff of
        // dust in the open. Fatal for a plume in a room: a 5 m puff at the BACK
        // wall of an 8 m house reads as 5 m nearer than it is, which puts it in
        // FRONT of the front wall, and the whole plume draws over the building
        // from outside. That was the "smoke goes through walls" everyone saw --
        // it was never in the room's geometry, only in the depth test.
        //
        // A small pull (a fraction of a metre) is still worth having so a puff
        // resting on the floor is not half-clipped by it; capped so it can never
        // exceed the particle's own contact plane distance.
        const float depthPull = std::min(size * 0.10f, 0.35f);
        const float nearPos = floatMax(posp[2] - depthPull, nearest);
        const float invNearPos = 1.0f / nearPos;
        posp[2] = project(2, 2) + project.Position()[2] * invNearPos;

        const float sizeX = +project(0, 0) * size * invW * invLeft * 0.5f;
        const float sizeY = -project(1, 1) * size * invW * invTop * 0.5f;
        if (sizeX * sizeY < 0.5f)
        {
            continue; // sub-pixel; drawing it costs a draw call and shows nothing
        }

        Color colour = lit;
        const float distance2 = (particle.position - cameraPosition).SquareSize();
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        colour.SetA(alphaFog ? (1.0f - fog) * alpha : 1.0f - (1.0f - fog) * alpha);

        GEngine->DrawDecal(posp, invNearPos, sizeX, sizeY, PackedColor(colour), mip, special);
        ++issued;
    }

    // The decisive number: particles alive versus billboards actually submitted.
    // A healthy plume has them close; issued == 0 with particles > 0 means every
    // one was rejected by the projection or size test, which is a completely
    // different bug from never reaching Draw at all.
    //
    // Opt-in, because this is the healthy path: a plume logging once a second is
    // noise during normal play, and eight plumes is eight lines a second. The
    // failure warnings above are NOT gated — those always want saying.
    static const bool verbose = []
    {
        const char* value = std::getenv("POSEIDON_TEST_SMOKE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (verbose && Glob.time > _nextDiagTime)
    {
        _nextDiagTime = Glob.time + 1.0f;
        int sheltered = 0;
        for (const SmokeParticle& p : _particles)
        {
            if (p.exposure < 0.5f)
            {
                ++sheltered;
            }
        }
        LOG_INFO(World,
                 "SmokeVolume: particles={} decals={} centre={:.1f},{:.1f},{:.1f} radius={:.2f} emitting={} "
                 "sweeps={} hits={} sheltered={}",
                 _particles.size(), issued, Position().X(), Position().Y(), Position().Z(), GetRadius(),
                 _emitting ? 1 : 0, _diagSweeps, _diagHits, sheltered);
        _diagSweeps = 0;
        _diagHits = 0;
    }

    _lastDrawUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
}

DEFINE_FAST_ALLOCATOR(SmokeVolume)

} // namespace Poseidon
