#include <Poseidon/World/Effects/SmokeVolume.hpp>

#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>

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

/// Fully-saturated hue to RGB. Balloons only; smoke never needs it. Kept simple
/// (the classic six-sector form) because the input is already a random number --
/// perceptual uniformity would buy nothing a party balloon cares about.
Color HueToRgb(float h)
{
    h = h - std::floor(h);
    const float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    const float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    const float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    return Color(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), 1.0f);
}

} // namespace

SmokeVolume::SmokeVolume(LODShapeWithShadow* shape, LODShapeWithShadow* fireShape, const SmokeParams& params)
    : Entity(shape, SmokeVolumeType(), -1), _params(params), _fireShape(fireShape)
{
    if (fireShape != nullptr)
    {
        // Additive, unfogged, no depth write: the legacy engine's own light-
        // sprite flags (Backend::IsLight -> BlendMode::Additive). A flame is an
        // emitter, not a surface; fog on it would grey it out.
        fireShape->OrSpecial(ClampU | ClampV | NoZWrite | IsAlpha | IsLight | NoShadow | IsColored);
    }
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

SmokeVolume::~SmokeVolume()
{
    // Ref<> drops our hold; the scene's light list holds the other. Zero the
    // colour first so a light the scene has not yet pruned goes dark at once
    // rather than lingering at last-frame brightness for a frame.
    if (!_fireLight.IsNull())
    {
        _fireLight->SetDiffuse(HBlack);
        _fireLight->SetAmbient(HBlack);
        _fireLight.Free();
    }
}

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
    if (_params.fire && !_fireShape.IsNull())
    {
        // With fire on, the flames ARE the smoke source: each flame that burns
        // out becomes a smoke particle where it died (see SimulateFire). The
        // nozzle stops emitting so the smoke visibly originates from the fire,
        // not from a point beneath it. The duration timer still runs here.
        if (_duration >= 0.0f)
        {
            _duration -= deltaT;
            if (_duration <= 0.0f)
            {
                _emitting = false;
            }
        }
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

        const float r = _params.emitterRadius * std::sqrt(RandUnit());
        const float theta = RandUnit() * 6.2831853f;
        const Vector3 position = _emitterPosition + Vector3(r * std::cos(theta), 0.0f, r * std::sin(theta));
        const Vector3 velocity(_params.initialSpeed * _params.spread * RandSymmetric(),
                               _params.initialSpeed * (0.7f + 0.3f * RandUnit()),
                               _params.initialSpeed * _params.spread * RandSymmetric());
        SpawnSmoke(position, velocity, 1.0f);
    }
}

void SmokeVolume::SpawnSmoke(Vector3Par position, Vector3Par velocity, float sizeScale)
{
    if (static_cast<int>(_particles.size()) >= _params.maxParticles)
    {
        return;
    }

    SmokeParticle particle;
    particle.lifetime = _params.particleLifetime * (0.80f + 0.40f * RandUnit());
    particle.position = position;
    particle.velocity = velocity;
    particle.sizeJitter = (0.75f + 0.5f * RandUnit()) * sizeScale;
    particle.alphaJitter = 0.7f + 0.6f * RandUnit();
    particle.hue = RandUnit();
    particle.swayPhase = RandUnit() * 6.2831853f;

    // First sweep on the FIRST tick (timer 0), so a particle born against a
    // wall -- a grenade rolled into a corner, a fire against a building --
    // finds its contact plane before it has moved at all. Subsequent sweeps
    // are jittered so the plume does not sweep in lockstep.
    particle.sweepTimer = 0.0f;
    particle.shelterTimer = RandUnit() * 0.5f;

    // Room tracking: a particle born inside a building room is contained
    // proactively from birth (roomId >= 0); the default -1 keeps the legacy
    // reactive path.
    GSmokeWorldQuery().IndoorRoomAt(position, particle.roomId);

    // Born with the shelter state unknown; the first probe runs on the first
    // tick. Starting at "exposed" and easing in would make a grenade thrown
    // into a room visibly get blown sideways before settling.
    particle.exposure = GSmokeWorldQuery().IsSheltered(particle.position, _params.shelterProbeHeight) ? 0.0f : 1.0f;

    _particles.push_back(particle);
}

void SmokeVolume::Integrate(SmokeParticle& particle, float deltaT)
{
    particle.renderRadius = -1.0f;
    const ISmokeWorldQuery& query = GSmokeWorldQuery();

    // -- shelter ----------------------------------------------------------
    particle.shelterTimer -= deltaT;
    if (particle.shelterTimer <= 0.0f)
    {
        // Half-second cadence with jitter. A roof does not move; what moves is
        // the particle, and at smoke speeds half a second is a fraction of a
        // metre. Probing every frame would double the query cost for nothing.
        particle.shelterTimer = 0.4f + 0.3f * RandUnit();
        bool sheltered;
        if (particle.roomId >= 0)
        {
            // In a tracked room: sheltered by definition, no roof probe at all.
            sheltered = true;
        }
        else if (query.IndoorRoomAt(particle.position, particle.roomId))
        {
            // Drifted into a room since the last tick -- same answer, still
            // without spending a probe on it.
            sheltered = true;
        }
        else
        {
            sheltered = query.IsSheltered(particle.position, _params.shelterProbeHeight);
        }
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

    if (_params.balloons)
    {
        // A balloon is a small solid object with fixed lift, not a parcel of hot
        // gas: it climbs at a steady rate rather than decelerating as it cools,
        // and it bobs because it is light enough for the air to push around.
        // Everything below (buoyancy decay, turbulence, diffusion) is smoke
        // behaviour and is skipped.
        const float t = Glob.time.toFloat() + particle.swayPhase;
        acceleration[1] += (_params.balloonRise - particle.velocity.Y()) * 1.5f;
        acceleration[0] += std::cos(t * 1.7f) * _params.balloonSway;
        acceleration[2] += std::sin(t * 1.3f) * _params.balloonSway;
        particle.velocity += acceleration * deltaT;
        return;
    }

    // Buoyancy dies as the plume cools, which is what forms the flat drifting
    // layer at the top of a real plume.
    //
    // ...but it cools much SLOWER in still air. Wind shears a plume apart and
    // mixes it with cold air, which is why a windy-day column flattens within
    // tens of metres while a calm-day one climbs hundreds. Stretch the decay by
    // how still the air is: at 0 m/s the plume stays hot for calmStillnessBoost
    // times longer, at ~6 m/s and above it cools on the authored schedule.
    float decay = _params.buoyancyDecay;
    {
        const float airSpeed = std::sqrt(air.X() * air.X() + air.Z() * air.Z());
        const float stillness = 1.0f - std::clamp(airSpeed / 6.0f, 0.0f, 1.0f);
        decay *= 1.0f + (_params.calmRiseBoost - 1.0f) * stillness * stillness;
    }
    const float heat = 1.0f - std::clamp(lifeFraction / std::max(decay, 1e-3f), 0.0f, 1.0f);
    acceleration[1] += _params.buoyancy * heat;
    if (_params.fire)
    {
        // Hot air off a fire. Applied with the same cooling curve so the smoke
        // still forms a drifting layer at the top instead of rising forever.
        acceleration[1] += _params.fireSmokeLift * heat;
    }

    if (_params.turbulence > 0.0f)
    {
        acceleration[0] += _params.turbulence * RandSymmetric();
        acceleration[1] += _params.turbulence * RandSymmetric() * 0.5f;
        acceleration[2] += _params.turbulence * RandSymmetric();
    }

    // Turbulent diffusion: a random walk whose step GROWS with age. Fickian
    // spread goes as sqrt(t), so the kick does too -- young puffs stay in the
    // column and old ones wander apart, which is exactly how a plume becomes a
    // cloud instead of a rigid shape that drifts. Horizontal is stronger than
    // vertical: the atmosphere is stably stratified and spreads smoke sideways
    // far more readily than it lifts or sinks it.
    if (_params.diffusion > 0.0f)
    {
        const float spread = _params.diffusion * std::sqrt(std::max(particle.age, 0.0f));
        acceleration[0] += spread * RandSymmetric();
        acceleration[1] += spread * RandSymmetric() * 0.35f;
        acceleration[2] += spread * RandSymmetric();
    }

    particle.velocity += acceleration * deltaT;
}

// ---------------------------------------------------------------------------
// Disturbance
// ---------------------------------------------------------------------------

int SmokeVolume::CollectDisturbers(Disturber* out, int max) const
{
    int count = 0;
    if (GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return 0;
    }

    // Early-out per mover: only things within a couple of plume radii matter,
    // and the test is one distance against the plume bound.
    const float plumeReach = GetRadius() + 8.0f;

    const Vector3 camPos = GScene->GetCamera()->Position();
    if ((camPos - Position()).SquareSize() < plumeReach * plumeReach && count < max)
    {
        // The free-fly camera is a body of its own -- flying it through a plume
        // is the easiest way to SEE this working.
        out[count].position = camPos;
        out[count].radius = 0.9f;
        ++count;
    }

    if (GWorld != nullptr)
    {
        // The entity the view is attached to: the walking player, or the
        // vehicle being driven. In free fly this is the player left standing
        // elsewhere -- its velocity will be ~0 and it drops out on the speed
        // test, which is the correct behaviour for a body at rest.
        if (Object* camOn = GWorld->CameraOn())
        {
            const Vector3 pos = camOn->Position();
            if ((pos - Position()).SquareSize() < plumeReach * plumeReach && count < max)
            {
                out[count].position = pos;
                out[count].radius = std::max(camOn->GetRadius(), 0.8f);
                ++count;
            }
        }
    }
    return count;
}

void SmokeVolume::ApplyDisturbance(float deltaT)
{
    Disturber movers[4];
    const int count = CollectDisturbers(movers, 4);
    if (count <= 0)
    {
        return;
    }

    // Velocity from last frame's position. One frame of latency at plume
    // speeds is a couple of centimetres -- invisible -- and it spares every
    // mover its own velocity plumbing.
    struct MoverVelocity
    {
        Vector3 velocity{VZero};
        float speed = 0.0f;
    };
    MoverVelocity velocities[4];
    for (int m = 0; m < count; ++m)
    {
        const Vector3& prev = m == 0 ? _prevCamPos : _prevCamOnPos;
        const bool valid = m == 0 ? _prevCamValid : _prevCamOnValid;
        if (valid)
        {
            velocities[m].velocity = (movers[m].position - prev) / std::max(deltaT, 1e-3f);
            velocities[m].speed = velocities[m].velocity.Size();
        }
    }
    _prevCamPos = movers[0].position;
    _prevCamValid = true;
    if (count > 1)
    {
        _prevCamOnPos = movers[1].position;
        _prevCamOnValid = true;
    }

    for (int m = 0; m < count; ++m)
    {
        if (velocities[m].speed < _params.disturbMinSpeed)
        {
            continue; // a body standing in the plume is not a gust
        }
        const float reach = movers[m].radius + _params.disturbReach;

        for (SmokeParticle& particle : _particles)
        {
            Vector3 away = particle.position - movers[m].position;
            const float dist = away.Size();
            if (dist >= reach)
            {
                continue;
            }
            // Direction out of the mover, with a degenerate case for a particle
            // sitting exactly on it.
            away = dist > 1e-3f
                       ? away / dist
                       : Vector3(RandSymmetric(), 0.5f + RandUnit(), RandSymmetric()).Normalized();
            // Stronger for faster movers, saturating -- a sprint and a jeep
            // both part the plume; neither should explode it.
            const float scale = std::clamp(velocities[m].speed / 3.0f, 0.2f, 1.5f);
            const float falloff = 1.0f - dist / reach;
            particle.velocity += away * (_params.disturbStrength * scale * falloff * falloff * deltaT);
            // A body also drags air UP with it -- keeps the parted smoke from
            // reading as a hole punched by a forcefield.
            particle.velocity[1] += 0.35f * falloff * scale * deltaT;
        }
    }
}

bool SmokeContactPlaneStillPresent(const SmokeParticle& particle, float radius,
                                   const ISmokeWorldQuery& query)
{
    if (!particle.planeValid) return false;
    const float clearance = particle.planeNormal.DotProduct(particle.position) -
        particle.planeOffset + particle.planePadding;
    if (!std::isfinite(clearance) || clearance < 0.0f || clearance > radius + 0.1f)
        return false;
    Vector3 hit;
    // A tangential sweep can miss a roof after the particle stops rising.
    // Offset sweep rays report their hit on the centre line, up to one sphere
    // radius away from the real surface. Include that offset when confirming.
    // Do not retain an infinite cached plane after leaving the edge of a roof.
    return query.ProbeSegment(particle.position,
        particle.position - particle.planeNormal * (clearance + radius + 0.05f), hit);
}

void SmokeVolume::Collide(SmokeParticle& particle, Vector3Par previousPosition, float deltaT)
{
    const ISmokeWorldQuery& query = GSmokeWorldQuery();

    // -- terrain ----------------------------------------------------------
    // Always, and cheaply: a heightmap lookup, not a geometry query. Smoke
    // sinking into a hillside is the most visible failure of the legacy system
    // and it does not deserve a sphere sweep to fix.
    // FloorHeightBelow, not FloorHeight: the clamp must find the surface UNDER
    // this particle. FloorHeight(x, z) is the TOPMOST surface on the column --
    // in a multi-storey building that is an upper floor, and every ground-floor
    // particle was clamped UP through the slab onto the ceiling of the storey
    // above. Same roadway-lookup cost, correct storey.
    const float ground = query.FloorHeightBelow(particle.position);
    particle.groundY = ground;
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

    // -- proactive indoor containment -------------------------------------
    // A particle with a room id does not sweep at all: BuildingInterior knows
    // every opening out of its room, so crossing a wall is impossible by
    // construction and no ray has to find one first. The contact-plane and
    // velocity response below is the same bookkeeping the reactive path does.
    if (_params.contain && particle.roomId >= 0)
    {
        // Do not assign the room of a predicted future point to a particle
        // still inside. That prematurely dropped its wall/roof render bounds.
        const SmokeContainStep step = query.IndoorContainStep(previousPosition, particle.position, particle.roomId);
        ++_diagSweeps;

        if (!step.allowed)
        {
            particle.position = step.position;
            ++_diagHits;

            particle.planeNormal = step.normal;
            particle.planePadding = sphereRadius * 0.5f;
            particle.planeOffset = step.normal.DotProduct(step.position) + particle.planePadding;
            particle.planeValid = true;

            const float distance = particle.planeNormal.DotProduct(particle.position) - particle.planeOffset;
            if (distance < 0.0f)
            {
                particle.position += particle.planeNormal * (-distance);
            }

            const float into = particle.velocity.DotProduct(particle.planeNormal);
            if (into < 0.0f)
            {
                particle.velocity -= particle.planeNormal * (into * (1.0f + _params.restitution));
                particle.velocity *= (1.0f - _params.friction);
            }
        }
        else if (step.room != particle.roomId)
        {
            // Through a portal into a neighbouring room (or out of the
            // building entirely, step.room == -1): drop the stale plane.
            particle.roomId = step.room;
            particle.planeValid = false;
        }
        return;
    }

    // -- real sweep -------------------------------------------------------
    // Contain mode sweeps EVERY tick from where the particle was to where it
    // is: no interval, no cached-plane gap to tunnel through when a particle
    // turns a corner between sweeps. Costs 7 rays per particle per frame; the
    // budget is the particle cap.
    if (!_params.contain)
    {
        particle.sweepTimer -= deltaT;
        if (particle.sweepTimer > 0.0f)
        {
            return;
        }
        particle.sweepTimer = _params.sweepInterval * (0.75f + 0.5f * RandUnit());
    }

    // Sweep past where the particle is now, out to where it will be by the time
    // it next sweeps. Finding the wall before reaching it is what keeps a thin
    // wall from being tunnelled through between sweeps.
    const float ahead = _params.contain ? deltaT * 2.0f : _params.sweepInterval;
    const Vector3 lookahead = particle.position + particle.velocity * ahead;
    const SmokeSurfaceHit hit = query.SweepSphere(previousPosition, lookahead, sphereRadius);
    ++_diagSweeps;

    if (!hit.hit)
    {
        particle.planeValid = _params.contain && SmokeContactPlaneStillPresent(particle, sphereRadius, query);
        return;
    }

    if (_params.contain)
    {
        // HARD CONTAINMENT: the segment previous -> current crossed a surface.
        // Do not slide from where we are (which may already be on the far
        // side of a thin wall); go back to the previous position -- which was
        // provably on the right side -- and only then apply the surface
        // response. A particle can be stopped, it cannot cross.
        const float sidePrev = hit.normal.DotProduct(previousPosition - hit.position);
        const float sideNow = hit.normal.DotProduct(particle.position - hit.position);
        if (sidePrev >= 0.0f && sideNow < sphereRadius * 0.5f)
        {
            particle.position = previousPosition;
        }
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
    particle.planePadding = sphereRadius * 0.5f;
    particle.planeOffset = hit.normal.DotProduct(hit.position) + particle.planePadding;
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

void SmokeVolume::EmitFire(float deltaT)
{
    if (!_params.fire || !_emitting || _fireShape.IsNull())
    {
        return;
    }

    _fireAccumulator += _params.fireRate * deltaT;
    while (_fireAccumulator >= 1.0f)
    {
        _fireAccumulator -= 1.0f;
        if (static_cast<int>(_flames.size()) >= _params.maxFlames)
        {
            _fireAccumulator = 0.0f;
            break;
        }

        SmokeParticle flame;
        flame.lifetime = _params.fireLifetime * (0.7f + 0.6f * RandUnit());

        // Same nozzle disc as the smoke, so the flames sit exactly where the
        // smoke is born and the smoke visibly rises OUT of them.
        const float r = _params.emitterRadius * std::sqrt(RandUnit());
        const float theta = RandUnit() * 6.2831853f;
        flame.position = _emitterPosition + Vector3(r * std::cos(theta), 0.0f, r * std::sin(theta));

        if (_params.fireColumnHeight > 0.0f)
        {
            // Column mode: born anywhere in the column, biased toward the base
            // (sqrt), so the body is filled continuously rather than a train
            // of separate puffs climbing one after another. Lower flames are
            // hotter (younger); starting some already part-way up with an
            // advanced age keeps the ramp consistent with height.
            const float u = RandUnit();
            const float hFrac = u * u;
            flame.position[1] += hFrac * _params.fireColumnHeight;
            flame.age = hFrac * flame.lifetime * 0.5f;
        }

        // Mostly up, a little lick sideways.
        flame.velocity = Vector3(_params.fireRiseSpeed * 0.25f * RandSymmetric(),
                                 _params.fireRiseSpeed * (0.8f + 0.4f * RandUnit()),
                                 _params.fireRiseSpeed * 0.25f * RandSymmetric());
        flame.sizeJitter = 0.7f + 0.6f * RandUnit();
        flame.alphaJitter = 0.8f + 0.4f * RandUnit();
        _flames.push_back(flame);
    }
}

void SmokeVolume::SimulateFire(float deltaT)
{
    // No collision, no shelter, no wind drag worth the cost: a flame lives
    // under a second and travels a couple of metres. It rises, flickers and
    // dies. Anything more is spent on something nobody sees.
    std::size_t write = 0;
    for (std::size_t read = 0; read < _flames.size(); ++read)
    {
        SmokeParticle& flame = _flames[read];
        flame.age += deltaT;
        if (flame.age >= flame.lifetime)
        {
            // THE FLAME BECOMES SMOKE. This is what makes it read as fire rather
            // than as a smoke grenade with flames painted under it: the smoke is
            // born where combustion ends, carrying the flame's upward velocity,
            // so it visibly rises OUT of the top of the fire. Not every flame
            // makes a puff (fireSmokeYield), or a hot fire chokes itself in
            // smoke; and the puff starts small and hot (sizeScale < 1) so the
            // fire's tips are dark wisps that swell as they cool.
            if (_emitting && RandUnit() < _params.fireSmokeYield)
            {
                Vector3 velocity = flame.velocity;
                velocity[1] = std::max(velocity[1], 0.5f) * 0.6f;
                SpawnSmoke(flame.position, velocity, 0.55f);
            }
            continue;
        }
        // Flicker: a random lateral kick, and a slight deceleration upward so
        // the tips of the flames hang before dying rather than shooting off.
        flame.velocity[0] += 6.0f * RandSymmetric() * deltaT;
        flame.velocity[2] += 6.0f * RandSymmetric() * deltaT;
        flame.velocity[1] -= 1.5f * deltaT;

        if (_params.fireColumnHeight > 0.0f)
        {
            // Column cohesion: pull back toward the axis over the emitter, so
            // the body stays a body. Strength falls with height so the tips
            // are free to lick and lean while the base is tight.
            const float h = std::max(flame.position.Y() - _emitterPosition.Y(), 0.0f);
            const float hFrac = std::clamp(h / _params.fireColumnHeight, 0.0f, 1.0f);
            const float k = _params.fireColumnCohesion * (1.0f - 0.7f * hFrac);
            flame.velocity[0] -= (flame.position.X() - _emitterPosition.X()) * k * deltaT;
            flame.velocity[2] -= (flame.position.Z() - _emitterPosition.Z()) * k * deltaT;
        }

        // Flames lean downwind. Same air the smoke breathes, damped by the
        // lean factor; a fire in a gale is a horizontal fan.
        if (_params.fireWindLean > 0.0f)
        {
            const Vector3 air = GAirflow.Sample(flame.position);
            flame.velocity[0] += (air.X() - flame.velocity.X()) * _params.fireWindLean * 2.0f * deltaT;
            flame.velocity[2] += (air.Z() - flame.velocity.Z()) * _params.fireWindLean * 2.0f * deltaT;
        }

        flame.position += flame.velocity * deltaT;

        if (write != read)
        {
            _flames[write] = flame;
        }
        ++write;
    }
    _flames.resize(write);
}

namespace
{
/// Temperature colour ramp by life fraction: white-hot -> yellow -> orange ->
/// dull red -> out. The values are what a flame billboard multiplied by them
/// LOOKS like on screen, not blackbody physics; the fire texture already
/// carries most of the shape and the ramp only has to carry the cooling.
Color FlameColour(float t)
{
    struct Key
    {
        float t;
        float r, g, b, a;
    };
    static const Key kRamp[] = {
        {0.00f, 1.00f, 0.96f, 0.80f, 0.70f}, // white-hot core, still faint (born small)
        {0.12f, 1.00f, 0.88f, 0.40f, 1.00f}, // yellow, full
        {0.40f, 1.00f, 0.52f, 0.10f, 0.95f}, // orange
        {0.70f, 0.80f, 0.20f, 0.03f, 0.60f}, // red, dimming
        {0.90f, 0.35f, 0.06f, 0.01f, 0.20f}, // sooty ember
        {1.00f, 0.10f, 0.02f, 0.00f, 0.00f}, // gone -- and a smoke puff is born
    };
    if (t <= kRamp[0].t)
    {
        return Color(kRamp[0].r, kRamp[0].g, kRamp[0].b, kRamp[0].a);
    }
    for (int i = 1; i < 6; ++i)
    {
        if (t <= kRamp[i].t)
        {
            const Key& a = kRamp[i - 1];
            const Key& b = kRamp[i];
            const float f = (t - a.t) / std::max(b.t - a.t, 1e-4f);
            return Color(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f);
        }
    }
    return Color(kRamp[5].r, kRamp[5].g, kRamp[5].b, kRamp[5].a);
}
} // namespace

void SmokeVolume::DrawFire(int level)
{
    if (_flames.empty() || _fireShape.IsNull() || GScene == nullptr || GEngine == nullptr)
    {
        return;
    }
    Shape* shape = _fireShape->LevelOpaque(level);
    if (shape == nullptr || shape->NFaces() != 1)
    {
        return;
    }
    Texture* texture = shape->FaceIndexed(0).GetTexture();
    MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
    if (!mip.IsOK())
    {
        return;
    }
    const int special = shape->Special() | GetObjSpecial();

    const Camera& camera = *GScene->GetCamera();
    Matrix4Val project = camera.Projection();
    Matrix4Val toView = GScene->ScaledInvTransform();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const float invTop = camera.InvTop();
    // Per-frame flicker: one value for the whole fire (a fire brightens and dims
    // as a body -- gusts feed it) plus a per-flame phase so individual tongues
    // pulse out of step. Cheap sin() on the mission clock; no state to keep.
    const float clock = Glob.time.toFloat();
    const float bodyFlicker = 0.85f + 0.15f * std::sin(clock * 9.7f) * std::sin(clock * 3.1f + 1.3f);

    // Additive sprites do not need sorting; drawn in emission order.
    for (std::size_t i = 0; i < _flames.size(); ++i)
    {
        const SmokeParticle& flame = _flames[i];
        const float t = std::clamp(flame.age / std::max(flame.lifetime, 1e-3f), 0.0f, 1.0f);
        Color colour = FlameColour(t);
        const float phase = static_cast<float>(i) * 0.61803f;
        const float flicker = bodyFlicker * (0.80f + 0.20f * std::sin(clock * 21.0f + phase * 6.2831853f));
        colour = Color(colour.R() * _params.fireIntensity * flicker, colour.G() * _params.fireIntensity * flicker,
                       colour.B() * _params.fireIntensity * flicker, colour.A() * flame.alphaJitter);
        if (colour.A() <= 0.01f)
        {
            continue;
        }
        const float size = Lerp(_params.fireStartRadius, _params.fireEndRadius, t) * flame.sizeJitter;

        Vector3 posp = toView * flame.position;
        if (posp.Z() < nearest)
        {
            continue;
        }
        const float invW = 1.0f / posp[2];
        posp[0] = project(0, 2) + project(0, 0) * posp[0] * invW;
        posp[1] = project(1, 2) + project(1, 1) * posp[1] * invW;
        const float nearPos = floatMax(posp[2] - std::min(size * 0.10f, 0.35f), nearest);
        const float invNearPos = 1.0f / nearPos;
        posp[2] = project(2, 2) + project.Position()[2] * invNearPos;

        // LICK, NOT BLOB. A flame is taller than it is wide because it is
        // being carried upward: stretch the sprite vertically by its rise
        // speed, most at birth (fast, narrow tongue) and relaxing to round as
        // it slows and dies into a puff of smoke. The round flame billboard is
        // what made the first pass read as cotton wool.
        const float rise = std::max(flame.velocity.Y(), 0.0f);
        const float stretch = 1.0f + std::clamp(rise / std::max(_params.fireRiseSpeed, 0.1f), 0.0f, 1.0f) *
                                          _params.fireStretch * (1.0f - t * 0.6f);
        const float sizeX = +project(0, 0) * size * invW * invLeft * 0.5f / std::sqrt(stretch);
        const float sizeY = -project(1, 1) * size * stretch * invW * invTop * 0.5f / std::sqrt(stretch);
        if (sizeX * sizeY < 0.5f)
        {
            continue;
        }
        // Additive: IsLight bypasses the fog branch in DrawDecal (no IsAlphaFog),
        // so pass the colour straight through with alpha as intensity.
        GEngine->DrawDecal(posp, invNearPos, sizeX, sizeY, PackedColor(colour), mip, special);
    }
}

void SmokeVolume::UpdateFireLight(float deltaT)
{
    const bool wantLight = _params.fire && _params.fireLightIntensity > 0.0f && !_flames.empty();

    if (!wantLight)
    {
        if (!_fireLight.IsNull())
        {
            _fireLight->SetDiffuse(HBlack);
            _fireLight->SetAmbient(HBlack);
            _fireLight.Free();
        }
        return;
    }

    if (_fireLight.IsNull())
    {
        if (GScene == nullptr)
        {
            return;
        }
        // Same construction the legacy explosion uses (Smokes.cpp): black at
        // birth, coloured per frame below. Scene owns it once added.
        _fireLight = new LightPoint(HBlack, HBlack);
        // Not gated on night: a fire is bright enough to read against the sun.
        _fireLight->SetDaylightVisible(true);
        GScene->AddLight(_fireLight);
    }

    // Light sits at the flame centroid, a little above it: the brightest part
    // of a fire is the top of the flame column, not the fuel.
    Vector3 centre(VZero);
    for (const SmokeParticle& flame : _flames)
    {
        centre += flame.position;
    }
    centre /= static_cast<float>(_flames.size());
    centre[1] += 0.4f;

    // Flicker: the same two slow sines the flames breathe on, plus a faster
    // sputter, so light and flame pulse together. Kept well away from 0 -- a
    // fire's light never goes out between flickers, it only dips.
    _fireLightPhase += deltaT;
    const float t = _fireLightPhase;
    const float body = 0.85f + 0.15f * std::sin(t * 9.7f) * std::sin(t * 3.1f + 1.3f);
    const float sputter = 0.92f + 0.08f * std::sin(t * 27.0f + 0.7f) * std::sin(t * 13.0f);
    const float flicker = std::clamp(body * sputter, 0.6f, 1.0f);

    // Scale with how much fire there is: more flames, more light, saturating
    // so a raging preset does not become a searchlight.
    const float fill = std::min(static_cast<float>(_flames.size()) / 40.0f, 1.0f);
    // HDR: in daylight the sun is orders of magnitude above the old LDR "1.0",
    // so a fire light that reads at noon needs real radiance -- but the same
    // radiance after dark, under the night exposure, paints the ground yellow.
    // The renderer does not auto-expose, so scale with the SUN: full HDR boost
    // in daylight, easing to 1x as NightEffect rises. NightEffect is 0 by day
    // and 1 at full night.
    float dayBoost = _params.fireLightHdrScale;
    if (GScene != nullptr && GScene->MainLight() != nullptr)
    {
        const float night = std::clamp(GScene->MainLight()->NightEffect(), 0.0f, 1.0f);
        dayBoost = Lerp(_params.fireLightHdrScale, 1.0f, night);
    }
    const float intensity = _params.fireLightIntensity * flicker * (0.4f + 0.6f * fill) * dayBoost;

    // Warm: fire light is orange, and the ambient term is a dimmer, redder
    // version so the shadowed sides of nearby things pick up a glow rather
    // than staying cold.
    const Color diffuse(1.00f * intensity, 0.62f * intensity, 0.22f * intensity, 1.0f);
    const Color ambient(0.30f * intensity, 0.12f * intensity, 0.04f * intensity, 1.0f);

    _fireLight->SetPosition(centre);
    _fireLight->SetDiffuse(diffuse);
    _fireLight->SetAmbient(ambient);
    _fireLight->SetBrightness(_params.fireLightRadius / 50.0f);
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
        EmitFire(deltaT);
        SimulateFire(deltaT);
        UpdateFireLight(deltaT);

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

        if (_params.disturb)
        {
            ApplyDisturbance(deltaT);
        }

        UpdateBounds();
    }

    // A spent volume removes itself. `_delete` is the same mechanism the legacy
    // Smoke uses, so teardown goes through exactly one path.
    if (!_emitting && _particles.empty() && _flames.empty())
    {
        _delete = true;
    }

    _lastSimulateUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
}

float SmokeParticleOpacity(const SmokeParticle& particle, const SmokeParams& params)
{
    const float life = std::clamp(particle.age / std::max(particle.lifetime, 1e-3f), 0.0f, 1.0f);
    if (life >= 1.0f) return 0.0f;
    float fade = 1.0f;
    if (params.fadeIn > 0.0f && life < params.fadeIn)
        fade = life / params.fadeIn;
    else if (params.fadeOut > 0.0f && life > 1.0f - params.fadeOut)
        fade = (1.0f - life) / params.fadeOut;
    // Dilution follows expansion, not wall clipping. Clipping a puff must not
    // concentrate its optical density or make the volume path opaque again.
    const float radius = std::max(Lerp(params.startRadius, params.endRadius, life) * particle.sizeJitter, 1e-3f);
    const float density = params.balloons ? 1.0f :
        std::pow(std::clamp(params.startRadius / radius, 0.0f, 1.0f), std::max(params.densityFalloff, 0.0f));
    const float opacity = params.balloons ? params.balloonOpacity : params.opacity;
    const float jitter = params.balloons ? Lerp(1.0f, particle.alphaJitter, 0.2f) : particle.alphaJitter;
    return std::clamp(opacity * jitter * fade * density, 0.0f, 1.0f);
}

float SmokeParticleRenderRadius(const SmokeParticle& particle, const SmokeParams& params,
                               const ISmokeWorldQuery& query)
{
    const float life = std::clamp(particle.age / std::max(particle.lifetime, 1e-3f), 0.0f, 1.0f);
    const float authored = Lerp(params.startRadius, params.endRadius, life);
    float radius = std::max((params.balloons ? params.balloonSize : authored) * particle.sizeJitter, 0.0f);
    if (!params.contain || !params.collide)
        return radius;
    if (particle.roomId >= 0)
    {
        const float clearance = query.IndoorClearanceAt(particle.position, particle.roomId, radius);
        radius = std::min(radius, std::max(clearance - 0.025f, 0.0f));
    }
    if (particle.planeValid)
    {
        // planeOffset includes the simulation's half-radius contact padding.
        const float clearance = particle.planeNormal.DotProduct(particle.position) - particle.planeOffset +
                                particle.planePadding;
        radius = std::min(radius, std::max(clearance - 0.025f, 0.0f));
    }
    return radius;
}

void BuildSmokeDrawOrder(const std::vector<SmokeParticle>& particles, const Matrix4& toView,
                         std::vector<float>& depths, std::vector<int>& order)
{
    depths.resize(particles.size());
    order.resize(particles.size());
    for (int i = 0; i < static_cast<int>(particles.size()); ++i)
    {
        depths[i] = (toView * particles[i].position).Z();
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return depths[a] > depths[b]; });
}

float SmokeVolume::RenderRadius(const SmokeParticle& particle) const
{
    if (particle.renderRadius < 0.0f)
        particle.renderRadius = SmokeParticleRenderRadius(particle, _params, GSmokeWorldQuery());
    return particle.renderRadius;
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

    // Room/portal overlay (POSEIDON_ROOM_OVERLAY): draws the interior of the
    // building nearest the emitter. Same env pattern as POSEIDON_TEST_SMOKE
    // below; rate-limited on its own clock so the two never interfere.
    static const bool roomOverlay = []
    {
        const char* value = std::getenv("POSEIDON_ROOM_OVERLAY");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (roomOverlay && GScene != nullptr && Glob.time > _nextOverlayTime)
    {
        _nextOverlayTime = Glob.time + 0.5f;
        DebugDrawInteriorNear(_emitterPosition);
    }

    // Flames first: they sit at the base, and the smoke rising through them
    // then composites over the top of the fire the way real smoke shrouds a
    // blaze rather than sitting under a clean flame layer.
    DrawFire(level);

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
    Matrix4Val project = camera.Projection();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const float invTop = camera.InvTop();

    // VOLUMETRIC LIGHTING, split into terms a lit cloud actually has.
    //
    // The first pass did one flat solve for the whole plume and tinted every
    // billboard with it, which is why a thick column read as a grey sticker:
    // its sun side and its underside were the same colour. A real plume has a
    // bright sun-facing rim, a dark self-shadowed core and underside, cool sky
    // ambient on top, and a little warm bounce from the ground. Six-way-lit
    // smoke (Unity/UE) bakes those directions into six textures; there is no
    // such texture here, so the same result is built per PARTICLE from where
    // it sits in the plume:
    //
    //   sun     * transmittance(particle -> sun through the plume)  [self-shadow]
    //   + sky   * (how much of the sky the particle can see)        [top bright]
    //   + ground bounce * (facing down)                             [warm base]
    //
    // Then the local point lights (fire, flares) at the particle. Costs a short
    // march per particle over a small spatial hash of the plume; see the
    // shadow-density loop below.
    LightSun* sun = GScene->MainLight();
    const Vector3 toSun = -sun->SunDirection(); // SunDirection points FROM the sun
    const Color sunColour = sun->GetDiffuse() * GEngine->GetAccomodateEye();
    const Color skyColour = sun->SkyColor() * GEngine->GetAccomodateEye();
    const Color ambientColour = sun->Ambient() * GEngine->GetAccomodateEye();
    const float nightFactor = sun->NightEffect();
    const Color tint(_params.red, _params.green, _params.blue, 1.0f);

    // Precompute each particle's transmittance to the sun. A coarse spatial hash
    // (cell = one typical puff) makes the neighbour march O(k) per particle.
    _shadowScratch.resize(_particles.size());
    {
        const float cell = std::max(Lerp(_params.startRadius, _params.endRadius, 0.5f), 0.5f);
        const float invCell = 1.0f / cell;
        _hash.Clear(_particles.size() * 2);
        auto key = [](int x, int y, int z) -> uint64_t
        {
            // 21 bits per axis, offset so negatives pack.
            const uint64_t ux = static_cast<uint64_t>(x + (1 << 20)) & 0x1FFFFF;
            const uint64_t uy = static_cast<uint64_t>(y + (1 << 20)) & 0x1FFFFF;
            const uint64_t uz = static_cast<uint64_t>(z + (1 << 20)) & 0x1FFFFF;
            return (ux << 42) | (uy << 21) | uz;
        };
        const auto particleDensity = [&](const SmokeParticle& p)
        {
            return SmokeParticleOpacity(p, _params);
        };
        // Each ray samples a cell total, not individual neighbours. Accumulate
        // once so a dense/downwash-compressed plume cannot turn this quadratic.
        for (int i = 0; i < static_cast<int>(_particles.size()); ++i)
        {
            const Vector3& p = _particles[i].position;
            _hash.Add(key(toIntFloor(p.X() * invCell), toIntFloor(p.Y() * invCell), toIntFloor(p.Z() * invCell)),
                      particleDensity(_particles[i]));
        }

        // March toward the sun in steps of one cell, up to `steps` cells;
        // accumulate the density of the neighbours in each cell. Density per
        // particle is its opacity times how much of its life it has left
        // (a fading puff shadows less). Beer-Lambert -> transmittance.
        const int steps = _params.selfShadowSteps;
        const float stepLength = cell;
        for (int i = 0; i < static_cast<int>(_particles.size()); ++i)
        {
            const SmokeParticle& self = _particles[i];
            const uint64_t selfKey = key(toIntFloor(self.position.X() * invCell),
                                         toIntFloor(self.position.Y() * invCell), toIntFloor(self.position.Z() * invCell));
            const float selfDensity = particleDensity(self);
            float density = 0.0f;
            Vector3 probe = self.position;
            for (int st = 0; st < steps; ++st)
            {
                probe += toSun * stepLength;
                const int cx = toIntFloor(probe.X() * invCell);
                const int cy = toIntFloor(probe.Y() * invCell);
                const int cz = toIntFloor(probe.Z() * invCell);
                const uint64_t probeKey = key(cx, cy, cz);
                density += _hash.SampleExcluding(probeKey, selfKey, selfDensity);
            }
            _shadowScratch[i] = std::exp(-density * _params.selfShadowStrength);
        }
    }

    // -- back-to-front ordering -------------------------------------------
    // The scene sorts OBJECTS. Inside one object the order is ours, and alpha
    // billboards that are not sorted show their draw order as hard seams where
    // puffs overlap. Sorting indices rather than particles keeps the simulation
    // array stable and costs one pass.
    Matrix4Val toView = GScene->ScaledInvTransform();

    BuildSmokeDrawOrder(_particles, toView, _drawDepth, _drawOrder);

    const Vector3 cameraPosition = camera.Position();
    int issued = 0;

    for (const int index : _drawOrder)
    {
        const SmokeParticle& particle = _particles[index];

        const float size = RenderRadius(particle);
        if (size < 0.05f)
            continue;

        const float alpha = SmokeParticleOpacity(particle, _params);
        if (alpha <= 0.004f)
        {
            continue;
        }

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
        const float depthPull = particle.roomId >= 0 ? std::min(size * 0.04f, 0.05f)
                                                     : std::min(size * 0.10f, 0.35f);
        const float nearPos = floatMax(posp[2] - depthPull, nearest);
        const float invNearPos = 1.0f / nearPos;
        posp[2] = project(2, 2) + project.Position()[2] * invNearPos;

        const float sizeX = +project(0, 0) * size * invW * invLeft * 0.5f;
        const float sizeY = -project(1, 1) * size * invW * invTop * 0.5f;
        if (sizeX * sizeY < 0.5f)
        {
            continue; // sub-pixel; drawing it costs a draw call and shows nothing
        }

        // -- per-particle light ---------------------------------------------
        // Where the particle sits relative to the plume centre, as a unit
        // vector: this is the "normal" a six-way lightmap would rotate. A
        // particle on the sun side of the plume faces the sun; one on the far
        // side faces away. Combined with the marched transmittance it gives a
        // lit rim and a dark core that track the sun as it moves.
        Vector3 fromCentre = particle.position - Position();
        const float fcLen = fromCentre.Size();
        if (fcLen > 1e-3f)
        {
            fromCentre = fromCentre / fcLen;
        }
        else
        {
            fromCentre = Vector3(0.0f, 1.0f, 0.0f);
        }
        const float transmittance = _shadowScratch[index];

        // Sun: N.L wrapped (smoke is translucent, light bleeds around it), times
        // self-shadow. The wrap keeps the far side from going pitch black.
        const float ndl = fromCentre.DotProduct(toSun);
        const float sunWrap = std::clamp(ndl * 0.5f + 0.5f, 0.0f, 1.0f);
        const float sunTerm = (0.35f + 0.65f * sunWrap) * transmittance;

        // Sky: how much of the upper hemisphere this particle sees. Top of the
        // plume bright and cool; underside sees the ground instead.
        const float up = fromCentre.Y();
        const float skyTerm = std::clamp(0.5f + 0.5f * up, 0.0f, 1.0f) * (0.6f + 0.4f * transmittance);
        // Ground bounce, warm and dim, on the underside.
        const float groundTerm = std::clamp(0.5f - 0.5f * up, 0.0f, 1.0f) * 0.25f;

        Color colour = Color(sunColour.R() * sunTerm + skyColour.R() * skyTerm * _params.skyAmbient +
                                 ambientColour.R() * groundTerm,
                             sunColour.G() * sunTerm + skyColour.G() * skyTerm * _params.skyAmbient +
                                 ambientColour.G() * groundTerm,
                             sunColour.B() * sunTerm + skyColour.B() * skyTerm * _params.skyAmbient +
                                 ambientColour.B() * groundTerm,
                             1.0f);

        // Local lights (the fire's own glow, flares) at THIS particle, so a
        // plume over a fire is lit orange from below and dark on top -- the
        // single strongest cue that it is a volume and not a decal.
        if (nightFactor > 0.01f || _params.fire)
        {
            const LightList& lights = GScene->ActiveLights();
            for (int li = 0; li < lights.Size(); ++li)
            {
                LightDescription desc;
                lights[li]->GetDescription(desc);
                if (desc.type != LTPoint)
                {
                    continue;
                }
                const Vector3 toLight = desc.pos - particle.position;
                const float d2 = toLight.SquareSize();
                const float reach = std::max(desc.startAtten * 3.0f, 0.5f);
                if (d2 > reach * reach)
                {
                    continue;
                }
                const float d = std::sqrt(d2);
                const float atten = std::clamp(1.0f - d / reach, 0.0f, 1.0f);
                // Wrapped so light bleeds through the puff toward the viewer.
                const float wrap = 0.5f + 0.5f * fromCentre.DotProduct(toLight / std::max(d, 1e-3f));
                const float k = atten * atten * (0.4f + 0.6f * wrap);
                colour = Color(colour.R() + desc.diffuse.R() * k, colour.G() + desc.diffuse.G() * k,
                               colour.B() + desc.diffuse.B() * k, 1.0f);
            }
        }

        if (_params.balloons)
        {
            // A balloon is a small SOLID object, so it gets a shaded-sphere
            // solve instead of the volume solve above: fromCentre doubles as
            // the sphere normal. Neighbours' self-shadow is divided out --
            // one balloon does not darken another.
            // Albedo is the balloon's own hue, blended toward the plume tint by
            // hueSpread so 0 gives a uniform batch and 1 the full wheel.
            const Color hueRgb = HueToRgb(particle.hue);
            const Color albedo(Lerp(tint.R(), hueRgb.R(), _params.balloonHueSpread),
                               Lerp(tint.G(), hueRgb.G(), _params.balloonHueSpread),
                               Lerp(tint.B(), hueRgb.B(), _params.balloonHueSpread), 1.0f);

            const Vector3 viewDir = (cameraPosition - particle.position).Normalized();
            // Limb darkening: the disc's edge turns away from the camera and
            // goes dark like a real balloon's terminator, instead of thinning
            // out by alpha the way a puff does.
            const float facing = std::clamp(fromCentre.DotProduct(viewDir), 0.0f, 1.0f);
            const float limb = Lerp(1.0f - _params.balloonLimbDarken, 1.0f, facing);

            const float unshadowed = sunTerm / std::max(transmittance, 1e-3f);
            const float diffuse = unshadowed * limb;

            // Hand-rolled reflect (Foundation math has none): the additive
            // highlight spot is the single strongest "balloon" cue.
            const Vector3 reflected = toSun - fromCentre * (2.0f * toSun.DotProduct(fromCentre));
            const float specular =
                std::pow(std::clamp(reflected.DotProduct(viewDir), 0.0f, 1.0f), _params.balloonShininess) *
                _params.balloonSpecular;
            // Cool rim on the side away from the sun picks the silhouette out
            // against dark backgrounds.
            const float rim = _params.balloonRimLight * (1.0f - facing);

            colour = Color(albedo.R() *
                                   (sunColour.R() * diffuse + skyColour.R() * skyTerm * _params.skyAmbient +
                                    ambientColour.R() * groundTerm) +
                               sunColour.R() * specular + skyColour.R() * rim,
                           albedo.G() *
                                   (sunColour.G() * diffuse + skyColour.G() * skyTerm * _params.skyAmbient +
                                    ambientColour.G() * groundTerm) +
                               sunColour.G() * specular + skyColour.G() * rim,
                           albedo.B() *
                                   (sunColour.B() * diffuse + skyColour.B() * skyTerm * _params.skyAmbient +
                                    ambientColour.B() * groundTerm) +
                               sunColour.B() * specular + skyColour.B() * rim,
                           1.0f);
        }
        else
        {
            colour = colour * tint;
        }
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
        int indoor = 0;
        for (const SmokeParticle& p : _particles)
        {
            if (p.exposure < 0.5f)
            {
                ++sheltered;
            }
            if (p.roomId >= 0)
            {
                ++indoor;
            }
        }
        LOG_INFO(World,
                 "SmokeVolume: particles={} decals={} centre={:.1f},{:.1f},{:.1f} radius={:.2f} emitting={} "
                 "sweeps={} hits={} sheltered={} indoor={} sim_us={:.0f} rays_counter={}",
                 _particles.size(), issued, Position().X(), Position().Y(), Position().Z(), GetRadius(),
                 _emitting ? 1 : 0, _diagSweeps, _diagHits, sheltered, indoor, _lastSimulateUs, SmokeSweepCount());
        _diagSweeps = 0;
        _diagHits = 0;
    }

    _lastDrawUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
}

DEFINE_FAST_ALLOCATOR(SmokeVolume)

} // namespace Poseidon
