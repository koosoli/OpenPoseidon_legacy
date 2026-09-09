// RenderSnapshot slice S1: capture of the environment block. See the header for why
// this exists; this file is the ONE place the render path's environment inputs touch
// live global state.

#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <Poseidon/Dev/Diag/SnapshotDiag.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <vector>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

namespace Poseidon::render::frame
{

// --- The ring ----------------------------------------------------------------------
//
// kPresentationSnapshotRingSize slots plus an index saying which one is published. A
// publish fills the OTHER slot and then flips the index, so the storage a consumer is
// reading is never the storage the producer is writing. See the header for why two.
//
// The index is atomic and the flip is a release store even though everything here runs
// on the main thread today: the flip is the exact point that will become the
// publication edge when step 6 moves the consumer to its own thread, and writing it as
// a relaxed int now would mean finding this line again later by memory. It costs
// nothing single-threaded.
static std::array<PresentationSnapshot, kPresentationSnapshotRingSize>& GRing()
{
    static std::array<PresentationSnapshot, kPresentationSnapshotRingSize> ring;
    return ring;
}

static std::atomic<int>& GCurrentSlot()
{
    static std::atomic<int> slot{0};
    return slot;
}

// Monotonic publish counter and the resource-lifetime epoch stamped onto each publish.
static uint64_t GGeneration = 0;
static uint32_t GResourceEpoch = 0;

const PresentationSnapshot& GPresentationSnapshot()
{
    return GRing()[GCurrentSlot().load(std::memory_order_acquire)];
}

int PresentationSnapshotSlotIndex()
{
    return GCurrentSlot().load(std::memory_order_acquire);
}

const PresentationSnapshot& PresentationSnapshotSlot(int index)
{
    return GRing()[index % kPresentationSnapshotRingSize];
}

void BumpPresentationResourceEpoch()
{
    ++GResourceEpoch;
}

// The one place live global state is read. Shared by the per-frame capture and by the
// validation re-read below, so the two can never diverge in WHAT they read.
static void FillPresentationSnapshot(PresentationSnapshot& snap)
{

    if (GScene)
    {
        snap.hasScene = true;
        snap.fogMinRange = GScene->GetFogMinRange();
        snap.fogMaxRange = GScene->GetFogMaxRange();
        snap.objectDrawDistance = GScene->GetObjectDrawDistance();
        snap.lodInvWidth = GScene->GetLodInvWidth();
        if (Camera* camera = GScene->GetCamera())
        {
            snap.hasCamera = true;
            snap.cameraPos = camera->Position();
            snap.cameraDir = camera->Direction();
        }
        if (LightSun* sun = GScene->MainLight())
        {
            snap.hasMainLight = true;
            snap.sunDiffuse = sun->Diffuse();
            snap.sunAmbient = sun->Ambient();
            snap.sunTravelDir = sun->Direction();
            snap.sunDirection = sun->SunDirection();
            snap.nightEffect = sun->NightEffect();
            snap.moonDirection = sun->MoonDirection();
            snap.moonLightAmount = sun->MoonLightAmount();
            snap.moonPhase = sun->MoonPhase();
            snap.moonIllumination = sun->MoonIllumination();
            snap.moonAngularRadius = sun->MoonAngularRadius();
            snap.moonBrightness = sun->MoonBrightness();
            snap.moonSunDirection = sun->MoonSunDirection();
        }
    }

    // S3: the local-light selection. Mirrors the walk the renderer ran in NextFrame
    // verbatim -- the per-LIGHT night gate (a lamp dims with NightEffect and vanishes by
    // day; a daylight-visible light such as fire or a muzzle flash passes at full
    // strength), the range-normalised score that keeps a light whose attenuation volume
    // contains the camera, and the sort. Skipped entirely by day when no daylight-visible
    // light exists, exactly as before.
    if (GScene && snap.hasMainLight)
    {
        const float night = snap.nightEffect;
        bool anyDaylight = false;
        {
            // LGT-010 cross-check, at the SOURCE of the light list. The renderer-side gauge
            // reported "0 candidates, nightEffect 0.000, sceneLights 0" on a frame that
            // renders as a lamp-lit night village -- so one of the two is reading something
            // other than what it names, and this says which.
            static float nextSay = 0.0f;
            const float now = Glob.time.toFloat();
            if (now >= nextSay)
            {
                nextSay = now + 3.0f;
                LOG_INFO(Graphics, "LGT-010 snapshot: scene lights {} nightEffect {:.3f} hasMainLight {}",
                         GScene != nullptr ? GScene->NLights() : -1, night, snap.hasMainLight);
            }
        }
        for (int i = 0, n = GScene->NLights(); i < n && !anyDaylight; ++i)
        {
            const Light* light = GScene->GetLight(i);
            anyDaylight = light != nullptr && light->IsOn() && light->IsDaylightVisible();
        }
        if (night > 0.0f || anyDaylight)
        {
            struct Candidate
            {
                float score;
                PresentationSnapshot::LocalLight light;
            };
            std::vector<Candidate> candidates;
            const Vector3 cameraPos = snap.hasCamera ? snap.cameraPos : VZero;
            const int n = GScene->NLights();
            candidates.reserve(n);
            for (int i = 0; i < n; i++)
            {
                Light* light = GScene->GetLight(i);
                if (!light || !light->IsOn())
                    continue;
                LightDescription desc;
                light->GetDescription(desc);
                const bool isSpot = desc.type == LTSpotLight;
                if (desc.type != LTPoint && !isSpot)
                    continue; // point + spot only; the sun is the directional main light
                const float gate = light->IsDaylightVisible() ? 1.0f : night;
                if (gate <= 0.0f)
                    continue;
                PresentationSnapshot::LocalLight entry;
                entry.pos = desc.pos;
                entry.startAtten = desc.startAtten;
                entry.diffuse = desc.diffuse * gate;
                entry.ambient = desc.ambient * gate;
                entry.dir = desc.dir;
                entry.dir.Normalize();
                entry.spot = isSpot;
                entry.coneOuter = isSpot ? desc.phi : 0.0f;   // LGT-010
                entry.coneInner = isSpot ? desc.theta : 0.0f; // LGT-011
                entry.endAttenScale = desc.endAttenScale; // LGT-014
                const float range = std::max(desc.startAtten * desc.endAttenScale, 1.0f);
                const float score = desc.pos.Distance2(cameraPos) / (range * range);
                candidates.push_back({score, entry});
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
            snap.lightCandidates = static_cast<uint32_t>(candidates.size());
            snap.lightCount =
                static_cast<int>(std::min(candidates.size(), size_t(PresentationSnapshot::kMaxLights)));
            for (int i = 0; i < snap.lightCount; ++i)
                snap.lights[i] = candidates[i].light;
        }
    }

    snap.timeSeconds = Glob.time.toFloat();
    snap.timeOfDay = Glob.clock.GetTimeOfDay();

    if (GLandscape)
    {
        snap.hasLandscape = true;
        snap.overcast = GSnow().RenderOvercast(GLandscape->GetOvercast());
        snap.weatherFog = GLandscape->GetFog();
        snap.seaLevel = GLandscape->GetSeaLevel();
        snap.landscapeRainDensity = GLandscape->GetRainDensity();
    }

    snap.rainEffectiveDensity = GRain.EffectiveDensity();
    snap.seaRippleGain = GRain.Params().seaRippleGain;

    // S2: interactors. ONE walk of the vehicle list replaces the two per-frame RTTI
    // walks the grass and water renderers used to run themselves, and including the
    // occupied vehicle here removes the "just dismounted, briefly absent from the
    // distributed list" weak-link special case both of them carried.
    if (GWorld)
    {
        const Object* cameraOn = GWorld->CameraOn();
        if (cameraOn)
        {
            snap.hasInteractor = true;
            snap.interactorPos = cameraOn->Position();
            snap.interactorVisibleSize = cameraOn->VisibleSize();
        }
        auto addRotor = [&](const Helicopter* helicopter)
        {
            if (!helicopter)
                return;
            const float rotorSpeed = std::clamp(helicopter->RotorSpeed(), 0.0f, 1.0f);
            if (rotorSpeed <= 0.02f)
                return;
            const Vector3 pos = helicopter->Position();
            PresentationSnapshot::RotorSource source;
            source.x = pos.X();
            source.y = pos.Y();
            source.z = pos.Z();
            source.rotorSpeed = rotorSpeed;
            source.groundY = GLandscape ? GLandscape->SurfaceY(pos.X(), pos.Z()) : 0.0f;
            if (snap.rotorSourceCount < PresentationSnapshot::kMaxRotorSources)
            {
                snap.rotorSources[snap.rotorSourceCount++] = source;
                return;
            }
            // Full: keep the nearest to the interactor/camera.
            const Vector3 reference = snap.hasInteractor ? snap.interactorPos : snap.cameraPos;
            int farthest = 0;
            float farthestD2 = -1.0f;
            for (int i = 0; i < snap.rotorSourceCount; ++i)
            {
                const float dx = snap.rotorSources[i].x - reference.X();
                const float dz = snap.rotorSources[i].z - reference.Z();
                const float d2 = dx * dx + dz * dz;
                if (d2 > farthestD2)
                {
                    farthestD2 = d2;
                    farthest = i;
                }
            }
            const float dx = source.x - reference.X();
            const float dz = source.z - reference.Z();
            if (dx * dx + dz * dz < farthestD2)
                snap.rotorSources[farthest] = source;
        };
        const Helicopter* cameraHeli = dynamic_cast<const Helicopter*>(cameraOn);
        if (cameraHeli)
        {
            addRotor(cameraHeli);
            snap.interactorRotor = snap.rotorSourceCount > 0;
            snap.interactorRotorSpeed = std::clamp(cameraHeli->RotorSpeed(), 0.0f, 1.0f);
        }
        for (int i = 0; i < GWorld->NVehicles(); ++i)
        {
            const Helicopter* helicopter = dynamic_cast<const Helicopter*>(GWorld->GetVehicle(i));
            if (helicopter && helicopter != cameraHeli)
                addRotor(helicopter);
        }
    }

    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        snap.windActive = true;
        snap.windSpeed = wind.speed;
        snap.windDirectionRad = wind.directionRad;
        snap.windGustFraction = wind.gustFraction;
        snap.windMeanSpeed = wind.meanSpeed;
        snap.windMeanDirectionRad = wind.meanDirectionRad;
    }

    snap.valid = true;
}

// True between a capture and the frame's RetirePresentationSnapshot(). Lets the
// renderer's InitDraw fallback tell "the world already published this frame's
// snapshot" from "nobody has" without a frame counter. Main thread only.
static bool GSnapshotFresh = false;

void PublishPresentationSnapshot(const PresentationSnapshot& snap)
{
    Dev::SnapshotCounters& diag = Dev::GSnapshotCounters();

    // 5.3's bounded-queue rule, enforced in structure. The ring cannot grow, so a
    // producer that outran the consumer does not queue -- it DROPS the un-retired
    // snapshot by overwriting the slot behind it. That drop is the event worth counting,
    // and it is the first thing that will move if step 6 gets the handoff wrong.
    //
    // IT IS NOT ZERO. The comment here used to say "zero by construction while producer
    // and consumer are the same thread", and the first capture that displayed the
    // counter said 1. Measured on perf_abel, two runs, 2026-08-31:
    //
    //     run 1: fallback 391, first drop at generation 392
    //     run 2: fallback 402, first drop at generation 403
    //
    // firstDropGeneration == fallback + 1 exactly, both times. So the one drop is the
    // HANDOVER FRAME: for the whole menu and loading screen the renderer publishes its
    // own snapshot (CapturePresentationSnapshotIfStale, hence the fallback count), and
    // the first frame in which the world publishes finds that frame's fallback snapshot
    // already published and not yet retired. One publish per world load, structural,
    // harmless -- the dropped snapshot is a menu snapshot nothing was going to draw.
    //
    // Which means the counter has a known-good value rather than an assumed one: it
    // should equal the number of world loads. Anything above that is the real event.
    // Counted rather than asserted because this legitimate case must not crash a
    // release build.
    if (GSnapshotFresh)
    {
        ++diag.publishedWithoutConsume;
        if (diag.firstDropGeneration == 0)
            diag.firstDropGeneration = GGeneration + 1; // the generation this publish is about to take
    }

    // Write the slot that is NOT current, then flip. Reset to defaults first: the slot
    // holds an older frame's values, and FillPresentationSnapshot only assigns the
    // fields whose source is present (the has* pattern), so a stale light array or
    // rotor list would otherwise survive into a frame that has none.
    const int next = (GCurrentSlot().load(std::memory_order_relaxed) + 1) % kPresentationSnapshotRingSize;
    PresentationSnapshot& slot = GRing()[next];
    slot = snap;
    slot.generation = ++GGeneration;
    slot.resourceEpoch = GResourceEpoch;
    GCurrentSlot().store(next, std::memory_order_release);
    GSnapshotFresh = true;

    ++diag.published;
    diag.ringSize = static_cast<uint32_t>(kPresentationSnapshotRingSize);
    diag.resourceEpoch = GResourceEpoch;
}

void CapturePresentationSnapshot()
{
    PresentationSnapshot snap; // defaults = the values the consuming sites used with no world up
    FillPresentationSnapshot(snap);
    PublishPresentationSnapshot(snap);
}

void CapturePresentationSnapshotIfStale()
{
    if (!GSnapshotFresh)
    {
        CapturePresentationSnapshot();
        ++Dev::GSnapshotCounters().fallback;
    }
}

void RetirePresentationSnapshot()
{
    if (GSnapshotFresh)
    {
        Dev::SnapshotCounters& diag = Dev::GSnapshotCounters();
        ++diag.consumed;
        // Age, in publishes, of what was consumed. Always 0 today (one publish, one
        // retire, same thread); nonzero after step 6 means presentation is that many
        // frames behind simulation, which is 5.3's "snapshot age" gauge.
        diag.consumedAge = GGeneration - GPresentationSnapshot().generation;
    }
    GSnapshotFresh = false;
}

void ResetPresentationSnapshotRingForTest()
{
    for (PresentationSnapshot& slot : GRing())
        slot = PresentationSnapshot{};
    GCurrentSlot().store(0, std::memory_order_release);
    GGeneration = 0;
    GResourceEpoch = 0;
    GSnapshotFresh = false;
    Dev::GSnapshotCounters() = Dev::SnapshotCounters{};
}

// POSEIDON_SNAPSHOT_VALIDATE=1 — the correctness oracle the design doc's validation
// note asks for (render-snapshot-design.md §3): at end of draw, re-read every live
// value the snapshot captured at InitDraw and log any field that changed WITHIN the
// frame. A hit means some draw-side code mutated environment state after capture —
// exactly the class of bug the snapshot exists to make impossible to miss. Off by
// default; one env probe when off.
//
// Measured baseline (60 s Everon traverse, 2026-08-30): during world/mission LOAD the
// whole block legitimately swaps (menu world -> test world, hour applied, camera
// teleport). In steady state the ONLY intra-frame mutator is lodInvWidth — Pass1's
// framerate governor adjusts it mid-draw by design, which is why PushSceneCamera
// consumes the snapshot value. Any OTHER field appearing here is a new bug.
void ValidatePresentationSnapshot()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("POSEIDON_SNAPSHOT_VALIDATE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (!enabled)
        return;
    const PresentationSnapshot& cap = GPresentationSnapshot();
    if (!cap.valid)
        return;
    PresentationSnapshot now;
    FillPresentationSnapshot(now);

    static int budget = 200; // total mismatch lines per session; enough to name the culprits
    if (budget <= 0)
        return;
    auto report = [&](const char* field, float was, float is)
    {
        if (budget <= 0)
            return;
        --budget;
        LOG_INFO(Graphics, "Snapshot validate: {} changed mid-frame: captured={:.6f} live={:.6f}", field, was, is);
    };
    auto checkF = [&](const char* field, float a, float b)
    {
        if (a != b)
            report(field, a, b);
    };
    auto checkV = [&](const char* field, const Vector3& a, const Vector3& b)
    {
        if (a.X() != b.X() || a.Y() != b.Y() || a.Z() != b.Z())
            report(field, a.Distance(b), 0.0f);
    };

    checkF("fogMinRange", cap.fogMinRange, now.fogMinRange);
    checkF("fogMaxRange", cap.fogMaxRange, now.fogMaxRange);
    checkF("objectDrawDistance", cap.objectDrawDistance, now.objectDrawDistance);
    checkF("lodInvWidth", cap.lodInvWidth, now.lodInvWidth);
    checkV("cameraPos", cap.cameraPos, now.cameraPos);
    checkV("cameraDir", cap.cameraDir, now.cameraDir);
    checkF("nightEffect", cap.nightEffect, now.nightEffect);
    checkV("sunTravelDir", cap.sunTravelDir, now.sunTravelDir);
    checkF("moonLightAmount", cap.moonLightAmount, now.moonLightAmount);
    checkF("timeSeconds", cap.timeSeconds, now.timeSeconds);
    checkF("timeOfDay", cap.timeOfDay, now.timeOfDay);
    checkF("overcast", cap.overcast, now.overcast);
    checkF("weatherFog", cap.weatherFog, now.weatherFog);
    checkF("seaLevel", cap.seaLevel, now.seaLevel);
    checkF("landscapeRainDensity", cap.landscapeRainDensity, now.landscapeRainDensity);
    checkF("rainEffectiveDensity", cap.rainEffectiveDensity, now.rainEffectiveDensity);
    checkF("seaRippleGain", cap.seaRippleGain, now.seaRippleGain);
    checkF("windSpeed", cap.windSpeed, now.windSpeed);
    checkF("windDirectionRad", cap.windDirectionRad, now.windDirectionRad);
    checkF("windGustFraction", cap.windGustFraction, now.windGustFraction);
    checkF("lightCount", float(cap.lightCount), float(now.lightCount));
    checkF("lightCandidates", float(cap.lightCandidates), float(now.lightCandidates));
    checkF("rotorSourceCount", float(cap.rotorSourceCount), float(now.rotorSourceCount));
    checkV("interactorPos", cap.interactorPos, now.interactorPos);
}

} // namespace Poseidon::render::frame
