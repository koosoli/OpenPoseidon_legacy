// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tidewater's boat model on OFP's ships. Ported from dgreenheck/tidewater src/player/BoatController.js
// (MIT, (c) DRG Software Solutions LLC; see CREDITS.md): a rigid body integrated at a fixed 120 Hz,
// hydrostatics from waterplane patches (rho g A depth each), heave damping against the water's own
// vertical motion, added mass and inertia that scale with how much hull is wet, the calm-water
// resistance curve with its wave-making hump, propeller thrust from a spooling engine, the rudder as
// a lifting surface in the propeller race, hull lift and cross-flow drag along the keel, running trim,
// grounding. Tidewater tuned it for one 8.2 m, 3.2 t lobster boat; here every term is scaled from
// that hull to each OFP boat's own mass, length and beam, and OFP's left/right thrust controls drive
// the throttle and the rudder. The water heights come from the renderer's queries of the drawn sea at
// the hull samples (DrawnWaterQuery, 1-3 frames late, as Tidewater's WaterQuery), else from the CPU
// sea as drawn near the camera (SwimWaterSurfaceY), which servers and headless runs integrate.
//
// What OP changes: the read-back latency is led and extrapolated for the queried heights only, the
// mooring lines and pier capsules are OFP's object collision instead, and the hull samples are a
// 7 x 3 grid over each model's footprint rather than an authored hull.

#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float Rho = 1025.0f;
constexpr float Grav = 9.81f;
constexpr float Deg = 3.14159265f / 180.0f;
// Tidewater's reference hull.
constexpr float RefMass = 3200.0f, RefLength = 8.2f;

float Smooth(float x, float a, float b)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float WaterAt(float x, float z)
{
    return SwimWaterSurfaceY(x, z);
}
} // namespace

float ShipDesignDraft(float length)
{
    return std::clamp(0.06f * length, 0.3f, 2.5f);
}

bool Ship::SimulateTidewater(float deltaT, Frame& moveTrans)
{
    if (!GLandscape || !GetShape() || deltaT <= 0)
    {
        return false;
    }
    const LODShape* shape = GetShape();
    const Vector3 lo = shape->Min(), hi = shape->Max();
    const Vector3 comModel = GetCenterOfMass();
    const float length = std::max(hi.Z() - lo.Z(), 2.0f), beam = std::max(hi.X() - lo.X(), 1.0f),
                height = std::max(hi.Y() - lo.Y(), 1.0f);
    const float m = std::max(GetMass(), 50.0f);
    const float s = m / RefMass;                         // force scale against Tidewater's hull
    const float lScale = std::sqrt(length / RefLength);  // Froude speed scale

    // ---- the hull samples: an effective waterplane 0.79 of the beam and 0.9 of the length (Tidewater's
    // sx / sz), at keel level, centred on the centre of mass; waterplane coefficient 0.75
    constexpr int Along = 7, Across = 3;
    static_assert(Along * Across == TwSamples, "sample count");
    const float halfX = std::min(comModel.X() - lo.X(), hi.X() - comModel.X()) * 0.79f;
    const float halfZ = std::min(comModel.Z() - lo.Z(), hi.Z() - comModel.Z()) * 0.9f;
    // Patch area from the draft a real hull of this length floats at (~6 % of its length: the PBR's
    // 0.57 m), not from the bounding box: OFP models carry no displacement, and a box-derived waterplane
    // floated the PBR on 0.3 m, perched on the water (owner: "flies around").
    const float designDraft = ShipDesignDraft(length);
    const float sampleArea = m / (Rho * designDraft) / float(TwSamples);
    Vector3 sample[TwSamples];
    for (int a = 0; a < Along; ++a)
        for (int c = 0; c < Across; ++c)
            sample[a * Across + c] = Vector3(comModel.X() + halfX * ((c + 0.5f) / Across * 2.0f - 1.0f), lo.Y(),
                                             comModel.Z() + halfZ * ((a + 0.5f) / Along * 2.0f - 1.0f));

    // ---- this frame's water under each sample (where the hull is now), and the water's own vertical
    // velocity: the height change since the last frame minus what the sample's own move across the
    // slope explains (at speed a wave face would otherwise read as fast-rising water)
    const Matrix3 orient0 = Orientation();
    const Vector3 pos0 = Position();
    int overWater = 0;

    // (OP) The drawn sea, anywhere: the renderer evaluates the surface it draws at this hull's samples
    // (asked here for the next frames, a latency ahead along the hull's motion, as Tidewater's
    // queueQueries) and answers 1-3 frames late. Without it a boat outside the 16 m around the camera
    // floated on the CPU predictor, which matches the drawn sea in scale but not crest for crest: in a
    // storm it hung over drawn troughs and sank into crests (owner, 2026-10-13). All samples or none,
    // so a hull never rides two seas; a server or another water mode has no results and keeps the
    // predictor. POSEIDON_BOAT_QUERY=0: the predictor (and the grid near the camera) always.
    static const bool useQuery = []
    {
        const char* v = std::getenv("POSEIDON_BOAT_QUERY");
        return !(v && v[0] == '0');
    }();
    float qh[TwSamples], qgx[TwSamples], qgz[TwSamples];
    uint32_t qVersion = 0;
    double qSeconds = 0;
    bool fromQuery = useQuery;
    if (useQuery)
    {
        float req[TwSamples * 2];
        constexpr float Lead = 0.05f;
        for (int i = 0; i < TwSamples && fromQuery; ++i)
        {
            const Vector3 w = pos0 + orient0 * sample[i];
            req[i * 2] = w.X() + _speed.X() * Lead;
            req[i * 2 + 1] = w.Z() + _speed.Z() * Lead;
            fromQuery = DrawnWaterQuery(w.X(), w.Z(), qh[i], qgx[i], qgz[i], &qVersion, &qSeconds);
        }
        RequestWaterQueries(reinterpret_cast<uintptr_t>(this), req, TwSamples);
    }
    const bool newResults = fromQuery && (!_tw.fromQuery || qVersion != _tw.qVersion);
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const float resultDt = float(qSeconds - _tw.qSeconds);

    for (int i = 0; i < TwSamples; ++i)
    {
        const Vector3 w = pos0 + orient0 * sample[i];
        if (GLandscape->SurfaceY(w.X(), w.Z()) < GLandscape->GetSeaLevel())
            ++overWater;
        if (fromQuery)
        {
            // the water's vertical speed between two result sets (not between frames: a set is reused
            // for 1-3 frames), less the part the sample's own move across the slope explains; the
            // height is carried from the set's age to now along it
            const float h = qh[i] - _sink;
            if (newResults)
            {
                if (_tw.hasWater && _tw.fromQuery && resultDt > 1e-3f)
                {
                    const float dx = w.X() - _tw.qx[i], dz = w.Z() - _tw.qz[i];
                    const float wv = (h - _tw.anchorH[i] - 0.5f * ((qgx[i] + _tw.gx[i]) * dx + (qgz[i] + _tw.gz[i]) * dz)) / resultDt;
                    _tw.waterV[i] += (std::clamp(wv, -3.0f, 3.0f) - _tw.waterV[i]) * 0.5f;
                }
                _tw.anchorH[i] = h;
                _tw.qx[i] = w.X();
                _tw.qz[i] = w.Z();
                _tw.gx[i] = qgx[i];
                _tw.gz[i] = qgz[i];
            }
            _tw.waterH[i] = h + _tw.waterV[i] * std::clamp(float(nowSeconds - qSeconds), 0.0f, 0.12f);
            continue;
        }
        const float h = WaterAt(w.X(), w.Z()) - _sink;
        const float gx = (WaterAt(w.X() + 0.5f, w.Z()) - WaterAt(w.X() - 0.5f, w.Z())) * 1.0f;
        const float gz = (WaterAt(w.X(), w.Z() + 0.5f) - WaterAt(w.X(), w.Z() - 0.5f)) * 1.0f;
        float wv = 0;
        if (_tw.hasWater)
        {
            const float dx = w.X() - _tw.qx[i], dz = w.Z() - _tw.qz[i];
            wv = (h - _tw.waterH[i] - 0.5f * ((gx + _tw.gx[i]) * dx + (gz + _tw.gz[i]) * dz)) / deltaT;
        }
        _tw.waterV[i] += (std::clamp(wv, -3.0f, 3.0f) - _tw.waterV[i]) * 0.5f;
        _tw.waterH[i] = h;
        _tw.qx[i] = w.X();
        _tw.qz[i] = w.Z();
        _tw.gx[i] = gx;
        _tw.gz[i] = gz;
    }
    _tw.fromQuery = fromQuery;
    if (fromQuery)
    {
        _tw.qVersion = qVersion;
        _tw.qSeconds = qSeconds;
    }
    if (overWater == 0)
    {
        _tw.hasWater = false;
        return false; // beached: the legacy land contact handles it
    }
    _tw.hasWater = true;

    // ---- controls: OFP's two engines -> throttle lever and rudder (positive = the yaw the legacy
    // differential thrust gave for R > L)
    const float lever = _engineOff ? 0.0f : 0.5f * (_thrustL + _thrustR);
    const float steerIn = _engineOff ? 0.0f : 0.5f * (_thrustR - _thrustL);
    _tw.throttle += (lever - _tw.throttle) * (1.0f - std::exp(-deltaT * 2.2f));
    _tw.rudder += (std::clamp(steerIn * 2.0f, -1.0f, 1.0f) - _tw.rudder) * (1.0f - std::exp(-deltaT * 3.0f));
    const bool driven = !_engineOff && !_isDead;
    const float rpmTarget = std::max(std::fabs(_tw.throttle), driven ? 0.15f : 0.0f);
    _tw.rpm += (rpmTarget - _tw.rpm) * (1.0f - std::exp(-std::min(deltaT, 0.1f) / (rpmTarget > _tw.rpm ? 0.9f : 1.3f)));

    // OFP's rated top speed is reached at full throttle: the propeller's pitch speed sits above it and
    // the bollard thrust is what balances the resistance there.
    const float vMax = std::max(Type()->GetMaxSpeedMs(), 3.0f);
    const auto resistance = [&](float au, float wetD)
    {
        const float a = au / lScale;
        return (40 * au + 22 * au * au + 3000 * Smooth(a, 2.8f, 5.4f) + 55 * au * au * Smooth(a, 7.0f, 11.0f)) * s * wetD;
    };
    const float pitchSpeed = vMax * 1.25f;
    const float maxThrust = resistance(vMax, 1.0f) / std::max(1.0f - vMax / pitchSpeed, 0.05f);

    // inertia (boat frame: x pitch, y yaw, z roll), a box scaled to Tidewater's ratios
    const Vector3 inertia(m * (length * length + height * height) / 12.0f * 0.75f,
                          m * (length * length + beam * beam) / 12.0f * 0.77f,
                          m * (beam * beam + height * height) / 12.0f * 0.86f * 1.24f);
    const Vector3 addedMass(0.6f, 0.7f, 0.05f);    // sway, heave, surge
    const Vector3 addedInertia(1.0f, 0.4f, 0.2f);  // pitch, yaw, roll
    // lateral stations along the keel (Tidewater's, scaled to this length and draft)
    static const float StationZ[7] = {-3.4f, -2.3f, -1.2f, -0.1f, 1.0f, 2.1f, 3.2f};
    static const float StationA[7] = {0.73f, 0.78f, 0.80f, 0.77f, 0.62f, 0.33f, 0.14f};
    const float zScale = length / RefLength, areaScale = s / zScale;
    float pitchK = 0;
    for (int i = 0; i < TwSamples; ++i)
        pitchK += sampleArea * Square(sample[i].Z() - comModel.Z());
    const float pitchStiffness = Rho * Grav * pitchK;

    // impulses (explosions, collisions) from the engine's own queue
    Vector3 velocity = _speed;
    Vector3 angular = _angVelocity;
    Matrix3 orient = orient0;
    Vector3 com = pos0 + orient0 * comModel;
    {
        velocity += _impulseForce * (1.0f / m);
        const Vector3 tl = orient.InverseRotation() * _impulseTorque;
        angular += orient * Vector3(tl.X() / inertia.X(), tl.Y() / inertia.Y(), tl.Z() / inertia.Z());
        _impulseForce = VZero;
        _impulseTorque = VZero;
    }

    const float h = 1.0f / 120.0f;
    _tw.acc += std::min(deltaT, 0.1f);
    int steps = 0;
    float age = 0, wetFraction = 0, heave = 0;
    bool land = false;
    while (_tw.acc >= h && steps < 12)
    {
        _tw.acc -= h;
        ++steps;
        age = std::min(age + h, 0.12f);
        const Matrix3 inv = orient.InverseRotation();
        const auto toWorld = [&](Vector3Par local) { return com + orient * (local - comModel); };
        Vector3 force(0, -m * Grav, 0), torque(VZero);
        const auto addAt = [&](Vector3Par f, Vector3Par pw)
        {
            force += f;
            torque += (pw - com).CrossProduct(f);
        };

        // ---- buoyancy + heave damping per patch
        float wetArea = 0, totalArea = 0, immersion = 0, heaveSum = 0;
        for (int i = 0; i < TwSamples; ++i)
        {
            const Vector3 pw = toWorld(sample[i]);
            const float hw = _tw.waterH[i] + _tw.waterV[i] * age;
            const float depth = hw - pw.Y();
            totalArea += sampleArea;
            if (depth <= 0)
                continue;
            const float sub = std::min(depth, 1.6f * zScale);
            wetArea += sampleArea * std::min(1.0f, depth / 0.3f);
            immersion += sampleArea * std::min(sub / std::max(0.35f * zScale, 0.05f), 1.5f);
            const Vector3 vp = velocity + angular.CrossProduct(pw - com);
            const float vy = vp.Y() - _tw.waterV[i] * 0.6f;
            heaveSum += vy;
            const float wetK = std::min(1.0f, sub / 0.25f) * sampleArea;
            addAt(Vector3(0, Rho * Grav * sampleArea * sub - (1800 * vy + 900 * vy * std::fabs(vy)) * wetK, 0), pw);
        }
        heave = heaveSum / TwSamples;
        wetFraction = totalArea > 0 ? wetArea / totalArea : 0;
        const float imm = totalArea > 0 ? std::clamp(immersion / totalArea, 0.0f, 1.0f) : 0;
        const float wetD = std::min(1.0f, wetFraction * 1.6f);
        const Vector3 fwd = orient.Direction(), side = orient.DirectionAside(), up = orient.DirectionUp();
        const Vector3 vl = inv * velocity;
        const float u = vl.Z(), vs = vl.X(), au = std::fabs(u);
        const Vector3 aLoc = inv * angular;

        // ---- calm-water resistance at the hull's mid-depth below the centre of mass
        addAt(fwd * (-resistance(au, wetD) * (u >= 0 ? 1.0f : -1.0f)), toWorld(Vector3(comModel.X(), comModel.Y() - 0.2f * zScale, comModel.Z())));
        force += velocity * (-3.3f * s * velocity.Size()); // air drag

        // ---- hull lift and cross-flow drag along the keel
        const float lateralY = lo.Y() + 0.06f * zScale + 0.75f * zScale;
        for (int k = 0; k < 7; ++k)
        {
            const float sz = comModel.Z() + StationZ[k] * zScale;
            const float dz = sz - comModel.Z();
            const float vk = vs + aLoc.Y() * dz - aLoc.Z() * (lateralY - comModel.Y());
            const float Y = -0.5f * Rho * StationA[k] * areaScale * (0.5f * au * vk + 1.1f * vk * std::fabs(vk)) * wetD;
            addAt(side * Y, toWorld(Vector3(comModel.X(), lateralY, sz)));
        }

        // ---- propeller (at the stern, below the waterline) and rudder in its race
        const Vector3 propLocal(comModel.X(), lo.Y() + 0.15f * zScale, lo.Z() + 0.08f * length);
        const Vector3 propW = toWorld(propLocal);
        const float propSub = _tw.waterH[(Along / 2) * Across + 1] * 0 + WaterAt(propW.X(), propW.Z()) - _sink - propW.Y();
        const float propWet = std::clamp(propSub / 0.3f + 0.5f, 0.0f, 1.0f);
        const float n = _tw.rpm * (driven && std::fabs(_tw.throttle) > 0.02f ? 1.0f : 0.0f);
        float thrust = 0;
        if (_tw.throttle > 0.02f)
            thrust = maxThrust * n * n * std::max(0.0f, 1.0f - std::max(u, 0.0f) / (pitchSpeed * std::max(n, 0.2f)));
        else if (_tw.throttle < -0.02f)
            thrust = -0.45f * maxThrust * n * n * std::max(0.0f, 1.0f - std::max(-u, 0.0f) / (pitchSpeed * 0.6f * std::max(n, 0.2f)));
        thrust *= propWet;
        addAt(fwd * thrust, propW);
        const Vector3 rudW = toWorld(Vector3(comModel.X(), lo.Y() + 0.35f * zScale, propLocal.Z()));
        const float race = std::max(thrust, 0.0f) * 2.0f / (Rho * 0.14f * s);
        const float ur2 = u * std::fabs(u) + 0.9f * race;
        const float ur = std::sqrt(std::fabs(ur2)) * (ur2 >= 0 ? 1.0f : -1.0f);
        const float vRud = vs + aLoc.Y() * (propLocal.Z() - comModel.Z());
        const float delta = std::clamp(_tw.rudder * 0.6f + (std::fabs(ur) > 0.3f ? std::atan2(vRud, std::fabs(ur)) * (ur >= 0 ? 1.0f : -1.0f) : 0.0f), -0.7f, 0.7f);
        const float lift = 0.5f * Rho * 0.12f * s * std::fabs(ur2) * 2.8f * std::sin(delta) * std::cos(delta) * propWet * (ur2 >= 0 ? 1.0f : -1.0f);
        addAt(side * (-lift), rudW);
        addAt(fwd * (-std::fabs(lift * std::sin(delta)) * 0.8f * (u >= 0 ? 1.0f : -1.0f)), rudW);

        // ---- running trim and rise with speed
        const float a = u / lScale;
        const float trim = (2.8f * Smooth(a, 2.5f, 4.8f) - 0.8f * Smooth(a, 4.8f, 7.5f) - 0.4f * Smooth(a, 7.5f, 10.5f)) * Deg;
        torque += side * (-pitchStiffness * trim * imm);
        force += up * (m * Grav * 0.15f * Smooth(a, 3.5f, 9.0f) * imm);

        // ---- extra angular damping (appendages, bilge), scaled to this hull's inertia
        const float wd = 0.2f + wetD;
        const Vector3 damp(-aLoc.X() * 25000 * inertia.X() / 14600.0f, -aLoc.Y() * 2000 * inertia.Y() / 15700.0f,
                           -aLoc.Z() * (4500 + 900 * au) * inertia.Z() / (3500.0f * 1.24f));
        torque += orient * (damp * wd);

        // ---- grounding: keel and bilge points on the terrain
        const Vector3 contactPoint[7] = {
            Vector3(comModel.X(), lo.Y(), lo.Z() + 0.1f * length), Vector3(comModel.X(), lo.Y(), comModel.Z()),
            Vector3(comModel.X(), lo.Y(), hi.Z() - 0.1f * length),
            Vector3(lo.X() + 0.15f * beam, lo.Y() + 0.25f * height, comModel.Z() + 0.3f * length),
            Vector3(hi.X() - 0.15f * beam, lo.Y() + 0.25f * height, comModel.Z() + 0.3f * length),
            Vector3(lo.X() + 0.15f * beam, lo.Y() + 0.25f * height, comModel.Z() - 0.3f * length),
            Vector3(hi.X() - 0.15f * beam, lo.Y() + 0.25f * height, comModel.Z() - 0.3f * length)};
        for (const Vector3& lp : contactPoint)
        {
            const Vector3 pw = toWorld(lp);
            const float pen = GLandscape->SurfaceY(pw.X(), pw.Z()) - pw.Y();
            if (pen <= 0)
                continue;
            land = true;
            const Vector3 vp = velocity + angular.CrossProduct(pw - com);
            const float fn = (pen * 400000 - std::min(vp.Y(), 0.0f) * 30000) * s;
            addAt(Vector3(-vp.X() * 6000 * s, std::max(fn, 0.0f), -vp.Z() * 6000 * s), pw);
        }

        // ---- integrate (semi-implicit Euler) with added mass / inertia in the boat frame
        Vector3 fl = inv * force;
        fl = Vector3(fl.X() / (m * (1 + addedMass.X() * wetD)), fl.Y() / (m * (1 + addedMass.Y() * wetD)),
                     fl.Z() / (m * (1 + addedMass.Z() * wetD)));
        velocity += (orient * fl) * h;
        Vector3 tl = inv * torque;
        tl = Vector3(tl.X() / (inertia.X() * (1 + addedInertia.X() * wetD)), tl.Y() / (inertia.Y() * (1 + addedInertia.Y() * wetD)),
                     tl.Z() / (inertia.Z() * (1 + addedInertia.Z() * wetD)));
        angular += (orient * tl) * h;
        com += velocity * h;
        orient += angular.Tilda() * orient * h;
        orient.Orthogonalize();
    }

    const bool finite = std::isfinite(com.X()) && std::isfinite(com.Y()) && std::isfinite(velocity.Y()) &&
                        std::isfinite(angular.X()) && std::isfinite(orient.Direction().X());
    if (!finite)
    {
        LOG_WARN(Physics, "Tidewater boat model diverged; this frame keeps the previous state");
        _tw.hasWater = false;
        return false;
    }
    Matrix4 transform;
    transform.SetOrientation(orient);
    transform.SetPosition(com - orient * comModel);
    moveTrans.SetTransform(transform);
    if (deltaT > 0)
        _acceleration = (velocity - _speed) * (1.0f / deltaT);
    _speed = velocity;
    _angVelocity = angular;
    _lastAngVelocity = angular;
    _waterContact = wetFraction > 0.02f;
    _landContact = land;
    _objectContact = false;
    _tw.heaveSpeed = heave;
    static const bool diag = std::getenv("POSEIDON_BOAT_DIAG") != nullptr;
    if (diag)
    {
        static float next = 0;
        if (Glob.time.toFloat() >= next)
        {
            next = Glob.time.toFloat() + 0.25f;
            LOG_INFO(Physics, "Boat TW: t={:.2f} y={:.2f} keel-water={:.2f} wet={:.2f} vy={:.2f} heave-rel={:.2f} u={:.2f} pitch={:.3f} roll={:.3f} thrust={:.0f} rudder={:.2f} steps={} water={}",
                     Glob.time.toFloat(), com.Y(), _tw.waterH[TwSamples / 2] - (com + orient * (sample[TwSamples / 2] - comModel)).Y(),
                     wetFraction, velocity.Y(), heave, (orient.InverseRotation() * velocity).Z(), orient.Direction().Y(),
                     orient.DirectionAside().Y(), 0.0f, _tw.rudder, steps, _tw.fromQuery ? "drawn" : "predictor");
        }
    }
    return true;
}
} // namespace Poseidon
