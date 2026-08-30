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

#include <algorithm>
#include <vector>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

namespace Poseidon::render::frame
{

PresentationSnapshot& GPresentationSnapshot()
{
    static PresentationSnapshot snapshot;
    return snapshot;
}

void CapturePresentationSnapshot()
{
    PresentationSnapshot snap; // defaults = the values the consuming sites used with no world up

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
                const float range = std::max(desc.startAtten * 10.0f, 1.0f);
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
        snap.overcast = GLandscape->GetOvercast();
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
    GPresentationSnapshot() = snap;
}

} // namespace Poseidon::render::frame
