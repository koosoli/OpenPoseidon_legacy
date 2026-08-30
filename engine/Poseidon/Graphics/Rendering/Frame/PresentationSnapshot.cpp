// RenderSnapshot slice S1: capture of the environment block. See the header for why
// this exists; this file is the ONE place the render path's environment inputs touch
// live global state.

#include <Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
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
