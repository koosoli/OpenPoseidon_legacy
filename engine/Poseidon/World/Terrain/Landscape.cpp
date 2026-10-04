#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyState.hpp>
#include <Poseidon/World/Scene/MapThings.hpp>
#include <Poseidon/World/Scene/Thing.hpp>
#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/Game/OperMap.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <Poseidon/Dev/Diag/DiagModes.hpp>

#include <Poseidon/World/Terrain/LandscapeShared.hpp>
#include <Poseidon/World/Terrain/TerrainProfile.hpp>
#include <Poseidon/World/Terrain/WorldObjectPlacementFrame.hpp>

#include <Poseidon/Core/TaskPool.hpp>

#include <vector>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <unordered_set>
#include <typeinfo>
#include <utility>
#include <Poseidon/Foundation/Containers/Array2D.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
void MemoryCleanUp();

namespace Poseidon
{

#define LOG_SEG 0

bool Landscape::s_enfusionRoadClutterClear = true;

void Landscape::Init()
{
    // A staged converted shape still owns section Texture references. Retire it before the
    // reused world and its banks are cleared, not on the next residency draw.
    _modernPendingOwnerPrimaryTextures.reset();
    ResetModernSimulationResidency();
    int i, x, z;
    // RFG-062: GLandscape is a reused singleton, so a world loaded after a native
    // Reforger one would otherwise inherit its surface names and grow Everon's clutter
    // on Malden. The native loader refills this immediately after calling Init().
    _enfusionSurfaces.Resize(0);
    _enfusionRoadCells.clear(); // same hazard: Everon's roads would clear Malden's clutter
    const ParamEntry& sky = Remaster >> "CfgLandscapeSky";
    RStringB skyName = sky >> "sky" >> "model";
    RStringB starsName = sky >> "stars" >> "model";
    RStringB horizontName = sky >> "horizont" >> "model";
    RStringB sunHaloName = sky >> "sunHalo" >> "model";
    RStringB sunName = sky >> "sun" >> "model";
    RStringB moonName = sky >> "moon" >> "model";
    auto loadSkyShape = [](const RStringB& name, const char* slot) -> Ref<LODShapeWithShadow>
    {
        if (name.GetLength() == 0)
        {
            LOG_WARN(World, "Landscape sky slot '{}' has no model configured", slot);
            return nullptr;
        }

        Ref<LODShapeWithShadow> shape = Shapes.New(name, false, false);
        if (!shape || !shape->LevelOpaque(0))
        {
            LOG_WARN(World, "Landscape sky slot '{}' could not load opaque geometry from '{}'", slot,
                     (const char*)name);
            return nullptr;
        }

        return shape;
    };
    // empty texture list
    ClipFlags clipSky = ClipAll & ~ClipBack | ClipUser0;
    {
        // load sky background shape
        Ref<LODShapeWithShadow> lShape = loadSkyShape(skyName, "sky");
        if (lShape)
        {
            // Ref<LODShapeWithShadow> lShape = GScene->Preloaded(SkySphere);
            if ((lShape->Special() & IsAlphaFog) == 0)
            {
                Shape* shape = lShape->LevelOpaque(0);
                for (i = 0; i < shape->NPos(); i++)
                {
                    shape->SetClip(i, ClipLightSky | clipSky | ClipFogDisable);
                }
                for (Offset f = shape->BeginFaces(); f < shape->EndFaces(); shape->NextFace(f))
                {
                    Poly& face = shape->Face(f);
                    face.OrSpecial(BestMipmap);
                    // SetMipmapLevel(0,0);
                }
                vecAlign Matrix4 scale(MScale, 100);
                lShape->InternalTransform(scale);
                lShape->SetSpecial(NoZBuf | NoZWrite | ClampV | NoShadow | IsAlphaFog | FogDisabled);
                shape->CalculateHints();
                lShape->CalculateHints();
                lShape->AllowAnimation();
            }
            _skyObject = new ObjectPlain(lShape, -1);
        }
    }
    {
        // load star background shape
        Ref<LODShapeWithShadow> lShape = loadSkyShape(starsName, "stars");
        if (lShape)
        { // this should be done only once
            if ((lShape->Special() & IsAlphaFog) == 0)
            {
                lShape->SetSpecial(NoZBuf | IsAlphaFog | IsAlpha | NoClamp);
                Shape* shape = lShape->LevelOpaque(0);
                for (i = 0; i < shape->NPos(); i++)
                {
                    ClipFlags hints = shape->Clip(i) & ClipUserMask;
                    shape->SetClip(i, ClipLightStars | clipSky | ClipFogDisable | hints);
                }
                // lShape->InternalTransform(Matrix4(MScale,100000));
                lShape->InternalTransform(Matrix4(MScale, 1000));
                shape->CalculateHints();
                lShape->CalculateHints();
                lShape->SetAutoCenter(false);
                lShape->CalculateBoundingSphere();
                lShape->AllowAnimation();
            }
            _starsObject = new ObjectPlain(lShape, -1);
        }
    }
    {
        // load blended horizont shape
        ClipFlags clipHor = ClipAll & ~ClipBack | ClipUser0;
        // Ref<LODShapeWithShadow> lShape = GScene->Preloaded(HorizontObject);
        Ref<LODShapeWithShadow> lShape = loadSkyShape(horizontName, "horizont");
        if (lShape)
        {
            if ((lShape->Special() & NoZWrite) == 0)
            {
                lShape->SetSpecial(ClampV | NoShadow | NoZWrite | IsAlpha);
                Shape* shape = lShape->LevelOpaque(0);
                int i;
                for (i = 0; i < shape->NPos(); i++)
                {
                    // const ClipFlags ambFlags = ClipLightAmbient|(ClipUserStep*MSInShadow);
                    const ClipFlags ambFlags = ClipUserStep * static_cast<uint32_t>(MSInShadow);
                    shape->SetClip(i, ambFlags | clipHor);
                }
                // horizontal range of horizont.p3d is -100..100
                // we need to enlarge it to BACKG_Z
                // shape->SortVertices();
                shape->CalculateHints();
                lShape->CalculateHints();
                // lShape->InternalTransform(Matrix4(MScale,BACKG_Z*(1.0/120)));
                lShape->AllowAnimation();
            }
            _horizontObject = new ObjectPlain(lShape, -1);
        }
    }
    {
        // load sun and moon shape
        const int sunSpec =
            (IsAlpha | NoZBuf | NoZWrite | ClampV | ClampU | SpecLighting | NoShadow | IsAlphaFog | FogDisabled);
        Ref<LODShapeWithShadow> haloShape = loadSkyShape(sunHaloName, "sunHalo");
        Shape* haloShape0 = haloShape ? haloShape->LevelOpaque(0) : nullptr;
        int i;
        {
            // load sun shape
            Ref<LODShapeWithShadow> sunShape = loadSkyShape(sunName, "sun");
            if (sunShape)
            {
                Shape* sunShape0 = sunShape->LevelOpaque(0);
                for (i = 0; i < sunShape0->NPos(); i++)
                {
                    sunShape0->SetClip(i, ClipLightSun | ClipFogDisable | ClipDecalNormal | clipSky);
                }
                if (haloShape0)
                {
                    for (i = 0; i < haloShape0->NPos(); i++)
                    {
                        haloShape0->SetClip(i, ClipLightSunHalo | ClipFogDisable | ClipDecalNormal | clipSky);
                    }
                    // halo behind the sun
                    Ref<LODShapeWithShadow> sunWithHalo = new LODShapeWithShadow(*haloShape);
                    sunWithHalo->LevelOpaque(0)->Merge(sunShape0, MIdentity);
                    sunWithHalo->LevelOpaque(0)->CalculateHints();
                    sunWithHalo->CalculateHints();
                    sunWithHalo->SetSpecial(sunSpec);
                    sunWithHalo->AllowAnimation();
                    _sunObject = new ObjectPlain(sunWithHalo, -1);
                }
                else
                {
                    sunShape->SetSpecial(sunSpec);
                    sunShape->AllowAnimation();
                    _sunObject = new ObjectPlain(sunShape, -1);
                }
            }
        }
        {
            // load moon shape
            Ref<LODShapeWithShadow> lShape = loadSkyShape(moonName, "moon");
            if (lShape)
            {
                Shape* moonShape0 = lShape->LevelOpaque(0);
                if ((lShape->Special() & IsAlphaFog) == 0)
                {
                    lShape->OrSpecial(IsAlphaFog);
                    for (i = 0; i < moonShape0->NPos(); i++)
                    {
                        moonShape0->SetClip(i, ClipLightMoon | ClipFogDisable | clipSky);
                    }
                    lShape->InternalTransform(Matrix4(MScale, 0.25));
                }
                if (haloShape0)
                {
                    for (i = 0; i < haloShape0->NPos(); i++)
                    {
                        haloShape0->SetClip(i, ClipLightMoonHalo | ClipFogDisable | clipSky);
                    }
                    // halo behind the moon
                    Ref<LODShapeWithShadow> moonWithHalo = new LODShapeWithShadow(*haloShape);
                    moonWithHalo->LevelOpaque(0)->Merge(moonShape0, MIdentity);
                    moonWithHalo->LevelOpaque(0)->CalculateHints();
                    moonWithHalo->CalculateHints();
                    moonWithHalo->SetSpecial(sunSpec | IsLight);
                    moonWithHalo->AllowAnimation();
                    _moonObject = new ObjectPlain(moonWithHalo, -1);
                }
                else
                {
                    lShape->SetSpecial(sunSpec | IsLight);
                    lShape->AllowAnimation();
                    _moonObject = new ObjectPlain(lShape, -1);
                }
            }
        }
    }
    _weather.Init(); // reinit weather

    _texture.Clear();
    _a3TerrainMaterials.Clear();
    _rvmatCellMaterials = false;
    SetTexture(0, "landtext\\mo.pac");
    // empty landscape
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            SetTex(x, z, 0);
            SetData(x, z, 0);
        }
    }
    // empty object list
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            _objects(x, z).Clear();
        }
    }
    ClearNativeFarResidency();
    _modernRequiredObjects.clear();
    _modernLogicalOnlyObjects.clear();
    _modernObjectPlacements.clear();
    _modernObjectCells.clear();
    _modernObjectModels.clear();
    _modernObjectCellActive.clear();
    _modernObjectCellDesired.clear();
    _modernObjectCellCursor.clear();
    _modernObjectCandidates.clear();
    _modernObjectCandidatesValid = false;
    _modernObjectCandidateBudget = 0;
    _modernObjectDesiredBudget = 0;
    _modernObjectDesiredCount = 0;
    _modernObjectEffectiveBudget = 0;
    _modernObjectBudgetLogged = 0;
    _modernObjectCandidateCenterX = -0x3fffffff;
    _modernObjectCandidateCenterZ = -0x3fffffff;
    _modernObjectCandidateRadius = 0;
    _modernObjectStreaming = false;
    _modernObjectResidencyPending = false;
    _modernObjectCenterX = -0x3fffffff;
    _modernObjectCenterZ = -0x3fffffff;
    _modernResidentObjectCount = 0;
    _modernObjectTestUpdate = 0;
    _modernObjectTestPhase = -1;
    _modernObjectLightOwners.clear();

    const ParamEntry& entry = Pars >> "CfgSurfaces" >> "Water";
    _waterSurface._name = entry >> "files";
    _waterSurface._roughness = entry >> "rough";
    _waterSurface._dustness = entry >> "dust";
    _waterSurface._soundEnv = entry >> "soundEnviron";

    _mountains.Clear();

    // good time to clean up allocator
    // now there should quite a lot of free memory
    MemoryCleanUp();
    _objectId = -1;
    // reenter all vehicles
    if (GWorld)
    {
        int v;
        for (v = 0; v < GWorld->NVehicles(); v++)
        {
            AddObject(GWorld->GetVehicle(v));
        }
        for (v = 0; v < GWorld->NFastVehicles(); v++)
        {
            AddObject(GWorld->GetFastVehicle(v));
        }
    }
}

namespace
{
// I-23 / B-023: the "load asset, keep last-known-good on failure"
// pattern centralized.  `loader` is a callable that returns
// `Ref<Texture>`; if the call returns null, `slot` is left
// unchanged so the existing texture remains bound to the sky
// sphere.  Callers cannot bypass the null-guard by accident —
// every Weather::SetSky overload routes through this helper.
//
// Making `_sky = nullptr` impossible in principle would require a
// non-null-Ref<> wrapper type; the helper is the structural
// chokepoint short of that, and the audit asserts both overloads
// flow through it.
template <typename Loader>
void TryReplaceSky(Ref<Texture>& slot, Landscape* land, Loader&& load)
{
    Ref<Texture> loaded = load();
    if (!loaded)
        return;
    slot = loaded;
    slot->SetMaxSize(1024); // no limit
    slot->ASetNMipmaps(1);  // use only the finest mipmap
    if (land)
        land->SetSkyTexture(slot);
}
} // namespace

void Weather::SetSky(Landscape* land, RStringB name)
{
    TryReplaceSky(_sky, land, [&] { return GlobLoadTexture(name); });
}

void Weather::SetSky(Landscape* land, RStringB n1, RStringB n2, float factor)
{
    TryReplaceSky(_sky, land, [&] { return GlobLoadTextureInterpolated(n1, n2, factor); });
}

class ThunderBolt : public Vehicle
{
    SoundPars _soundPars;
    float _size;
    bool _soundDone{false};
    float _phase{0};
    Ref<LightPoint> _light;

  public:
    ThunderBolt(LODShapeWithShadow* shape, float size, const SoundPars& pars);
    void Simulate(float deltaT, SimulationImportance prec) override;
    bool AnimateTexture(int level, bool shadow);
    bool DeanimateTexture(int level, bool shadow);
};

ThunderBolt::ThunderBolt(LODShapeWithShadow* shape, float size, const SoundPars& pars)
    : Vehicle(shape, VehicleTypes.New("ThunderBolt"), -1), _size(size), _soundPars(pars)
{
}

static const Color ThunderBoltColor(1, 1, 2);
static const Color ThunderBoltAmbient(0.5, 0.5, 1);

void ThunderBolt::Simulate(float deltaT, SimulationImportance prec)
{
    bool lightVisible = (toIntFloor(_phase * 4) & 1) != 0;
    if (!_light && lightVisible)
    {
        _light = new LightPoint(ThunderBoltColor, ThunderBoltAmbient);
        _light->SetBrightness(1000);
        _light->SetPosition(Position());
        GLOB_SCENE->AddLight(_light);
    }
    if (_light)
    {
        const int ThunderBoltPhases = 3;
        float animation = _phase * ThunderBoltPhases;
        // int phase=toIntFloor(animation);
        float frac = animation - toIntFloor(animation);
        float intensity = (0.5 - fabs(frac - 0.5)) * 2.0;
        float useAverage = deltaT * ThunderBoltPhases;
        saturateMin(useAverage, 1);
        intensity = 0.5 * useAverage + intensity * (1 - useAverage);
        saturateMax(intensity, 0);
        // low down intensity with time
        intensity *= 1 - _phase;
        _light->SetDiffuse(ThunderBoltColor * intensity);
        _light->SetAmbient(ThunderBoltAmbient * intensity);
    }
    if (!_soundDone)
    {
        _soundDone = true;
        // sound of explosion
        float rndFreq = GRandGen.RandomValue() * 0.1 + 0.95;
        IWave* sound =
            GSoundScene->OpenAndPlayOnce(_soundPars.name, Position(), VZero, _soundPars.vol, _soundPars.freq * rndFreq);
        if (sound)
        {
            GSoundScene->SimulateSpeedOfSound(sound);
            GSoundScene->AddSound(sound);
        }
    }
    _phase += deltaT * 3;
    if (_phase >= 1)
    {
        SetDelete();
    }
}

bool ThunderBolt::AnimateTexture(int level, bool shadow)
{
    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return false;
    }
    shape->Face(shape->BeginFaces()).AnimateTexture(_phase);
    return true;
}

bool ThunderBolt::DeanimateTexture(int level, bool shadow)
{
    return true;
}

void Weather::SetClouds(float alpha, float brightness, float speed, float through)
{
    saturate(alpha, 0, 1);
    saturate(brightness, 0, 1);
    saturate(through, 0, 1);
    _skyThrough = through;
    _cloudsAlpha = alpha;
    _cloudsBrightness = brightness;
    _cloudsSpeed = speed;
}

struct WeatherBasic
{
    float overcast;
    RStringB sky;
    float alpha, bright, speed, through;
    WeatherBasic(float overcastI, RStringB skyI, float alphaI, float brightI, float speedI, float throughI)
    {
        overcast = overcastI;
        sky = skyI;
        alpha = alphaI, bright = brightI, speed = speedI, through = throughI;
    }
};

static const WeatherBasic* GetWeatherBasics(int& count)
{
    static const WeatherBasic WeatherBasics[] = {//        overcast, sky,            alphaI, brightI, speedI, through
                                                 WeatherBasic(-0.08, "data\\jasno.pac", 0, 1.0, 0.2, 1.0),
                                                 WeatherBasic(0.50, "data\\oblacno.pac", 0.9, 1.0, 0.35, 0.7),
                                                 WeatherBasic(0.66, "data\\zatazeno.pac", 0.9, 0.7, 0.6, 0.1),
                                                 WeatherBasic(1.0, "data\\zatazeno.pac", 1.0, 0.5, 1.0, 0.0)};
    count = sizeof(WeatherBasics) / sizeof(*WeatherBasics);
    return WeatherBasics;
}

void Weather::Init()
{
    _thunderBoltTime = Glob.time - 1;
    _overcastSetSky = -1.0;
    _overcastSetClouds = -1.0;
    _fogSet = 0.0f;
    _cloudsPos = 0;
    _cloudsAlpha = 0.0f;
    _cloudsBrightness = 1.0f;
    _cloudsSpeed = 0.2f;
    _skyThrough = 1.0f;
    _rainDensity = 0;
    _rainDensityWanted = 0;
    _rainDensitySpeed = 1;
    _windSpeed = VZero;
    _lastWindSpeedChange = Glob.time;
    _gust = VZero;
    _gustUntil = Glob.time;
}

Weather::Weather()
{
    Init();
}

void Weather::SetRain(float density, float time)
{
    saturate(density, 0, 1);
    saturateMax(time, 0.001);
    _rainDensityWanted = density;
    _rainDensitySpeed = fabs(_rainDensityWanted - _rainDensity) / time;
}

void Weather::SetOvercast(Landscape* land, float overcast)
{
    // separate calculations for sky texture / clouds layer

    int NWeathers;
    const WeatherBasic* WeatherBasics = GetWeatherBasics(NWeathers);

    saturate(overcast, 0, 1);
    if (fabs(_overcastSetClouds - overcast) > 0.001)
    {
        // LOG_DEBUG(World, "overcastClouds {:.3f}->{:.3f}",_overcastSetClouds,overcast);
        //  find nearest before and nearest after
        _overcastSetClouds = overcast;
        int index;
        for (index = 0; index < NWeathers; index++)
        {
            const WeatherBasic& basic = WeatherBasics[index];
            if (basic.overcast > overcast)
            {
                break;
            }
        }
        if (index <= 0)
        {
            // use basic (clear)
            const WeatherBasic& basic = WeatherBasics[0];
            SetClouds(basic.alpha, basic.bright, basic.speed, basic.through);
            return;
        }
        if (index >= NWeathers)
        {
            const WeatherBasic& basic = WeatherBasics[NWeathers - 1];
            SetClouds(basic.alpha, basic.bright, basic.speed, basic.through);
            return;
        }
        // use combination of basic weathers index-1 and index
        const WeatherBasic& basicMin = WeatherBasics[index - 1];
        const WeatherBasic& basicMax = WeatherBasics[index];
        float interpol = (overcast - basicMin.overcast) / (basicMax.overcast - basicMin.overcast);
        SetClouds((basicMax.alpha - basicMin.alpha) * interpol + basicMin.alpha,
                  (basicMax.bright - basicMin.bright) * interpol + basicMin.bright,
                  (basicMax.speed - basicMin.speed) * interpol + basicMin.speed,
                  (basicMax.through - basicMin.through) * interpol + basicMin.through);

        // LOG_DEBUG(World, "bright {:.3f}",(basicMax.bright-basicMin.bright)*interpol+basicMin.bright);
    }
    if (fabs(_overcastSetSky - overcast) > 0.01)
    {
        // LOG_DEBUG(World, "overcastSky {:.3f}->{:.3f}",_overcastSetSky,overcast);
        //  find nearest before and nearest after
        _overcastSetSky = overcast;
        int index;
        for (index = 0; index < NWeathers; index++)
        {
            const WeatherBasic& basic = WeatherBasics[index];
            if (basic.overcast > overcast)
            {
                break;
            }
        }
        if (index <= 0)
        {
            // use basic (clear)
            const WeatherBasic& basic = WeatherBasics[0];
            SetSky(land, basic.sky);
            return;
        }
        if (index >= NWeathers)
        {
            const WeatherBasic& basic = WeatherBasics[NWeathers - 1];
            SetSky(land, basic.sky);
            return;
        }
        // use combination of basic weathers index-1 and index
        const WeatherBasic& basicMin = WeatherBasics[index - 1];
        const WeatherBasic& basicMax = WeatherBasics[index];
        float interpol = (overcast - basicMin.overcast) / (basicMax.overcast - basicMin.overcast);
        SetSky(land, basicMin.sky, basicMax.sky, interpol);
    }
}

void Weather::SetFog(Landscape* land, float fog)
{
    saturate(fog, 0, 1);
    if (fabs(_fogSet - fog) < 0.001)
    {
        return;
    }
    _fogSet = fog;
}

void Weather::MoveClouds(float deltaT)
{
    _cloudsPos += _cloudsSpeed * deltaT;

    // simulate rain
    float maxRain = _overcastSetClouds * 1.5 - 1;
    if (maxRain > 0.01)
    {
        if (fabs(_rainDensityWanted - _rainDensity) < 1e-6)
        {
            saturate(maxRain, 0, 1);
            _rainDensityWanted = _rainDensity + (GRandGen.RandomValue() - 0.5) * 0.5 * maxRain;
            saturate(_rainDensityWanted, 0, maxRain);
            _rainDensitySpeed = (GRandGen.RandomValue() + 0.1) * 0.01;
        }
        float delta = _rainDensityWanted - _rainDensity;
        Limit(delta, -_rainDensitySpeed * deltaT, +_rainDensitySpeed * deltaT);
        _rainDensity += delta;
        saturate(_rainDensity, 0, 1);
        const float thold = 0.4;
        if (maxRain >= thold)
        {
            // there is a chance of thunderbolt
            float thunderBoltDensity = (maxRain - thold) * (1 / (1 - thold));
            if (Glob.time > _thunderBoltTime)
            {
                // do thunderbolt effect
                const ParamEntry* cfg = nullptr;
                if (GRandGen.RandomValue() < thunderBoltDensity)
                {
                    cfg = &(Pars >> "CfgEffects" >> "ThunderboltHeavy");
                }
                else
                {
                    cfg = &(Pars >> "CfgEffects" >> "ThunderboltNorm");
                }
                Ref<LODShapeWithShadow> shape = Shapes.New(GetShapeName((*cfg) >> "model"), false, false);

                Shape* shape0 = shape->LevelOpaque(0);
                for (int v = 0; v < shape0->NPos(); v++)
                {
                    shape0->SetClip(v, ClipFogSky | (ClipAll & ~ClipBack));
                }
                shape0->CalculateHints();

                // random bolt position
                // select the highest place of some random points
                Vector3Val cPos = GLOB_SCENE->GetCamera()->Position();
                Vector3 best(VZero);
                for (int c = 10; --c >= 0;)
                {
                    Vector3 pos;
                    pos.Init();
                    pos[0] = (GRandGen.RandomValue() - 0.5) * (60 * LandGrid) + cPos.X();
                    pos[2] = (GRandGen.RandomValue() - 0.5) * (60 * LandGrid) + cPos.Z();
                    pos[1] = GLandscape->SurfaceY(pos[0], pos[2]);
                    if (pos[1] > best[1])
                    {
                        best = pos;
                    }
                }
                // use difefent sound for near/far sounds
                float size = 5;
                float dist2 = best.Distance2(cPos);
                SoundPars pars;
                const float minDist = 300;
                if (dist2 < Square(minDist))
                {
                    Vector3 norm = (best - cPos);
                    best = norm.Normalized() * minDist + cPos;
                    best[1] = GLandscape->SurfaceY(best[0], best[2]);
                    dist2 = best.Distance2(cPos);
                }
                if (dist2 > Square(1000))
                {
                    GetValue(pars, (*cfg) >> "soundFar");
                }
                else
                {
                    GetValue(pars, (*cfg) >> "soundNear");
                }
                ThunderBolt* bolt = new ThunderBolt(shape, size, pars);
                bolt->SetScale(size);
                bolt->SetPosition(best + shape->BoundingCenter() * size);
                GWorld->AddCloudlet(bolt);
                _thunderBoltTime = Glob.time + GRandGen.RandomValue() * 5 / (thunderBoltDensity + 0.001);
            }
        }
    }
    else
    {
        _rainDensityWanted = 0;
        _rainDensity = 0;
        _rainDensitySpeed = 0.01;
    }

    {
        // night and weather visibility
        float rainVisibility = (1 - _rainDensity) * TACTICAL_VISIBILITY + 350 * _rainDensity;
        // night: very limited visibility
        const LightSun* sun = GLOB_SCENE->MainLight();
        float nightVisibility = sun->GetDiffuse().R() * 4;
        float fogVisibility = 1.0f - _fogSet * 0.95f;
        saturate(nightVisibility, 0.75f, 1);

        float noFogVisibility = rainVisibility * nightVisibility;

        float defaultFogDistance = floatMin(900, noFogVisibility) * fogVisibility;
        float currentFogDistance = noFogVisibility * fogVisibility;

        float tacRange = defaultFogDistance;
        const float defFogThold = 0.6f;
        if (fogVisibility > defFogThold)
        {
            float iFactor = (fogVisibility - defFogThold) * (1.0f / (1.0f - defFogThold));
            tacRange = iFactor * currentFogDistance + (1 - iFactor) * defaultFogDistance;
        }

        // when fog is very dense
        // (corresponding to <300 m with 900 m viewdistance, daytime and no rain)
        // it should be related to default visibility (900 m)
        // when there is no fog, it should be related to user-selected visibility

        saturate(tacRange, 0.1, 1e6);
        GLOB_SCENE->SetTacticalVisibility(tacRange, tacRange);
    }

    if (Glob.time > _lastWindSpeedChange + 5)
    {
        _lastWindSpeedChange = Glob.time;
        _windSpeed[0] += GRandGen.PlusMinus(0, 1);
        _windSpeed[2] += GRandGen.PlusMinus(0, 1);
        float maxWind = _overcastSetClouds * 4 + 1;
        saturate(_windSpeed[0], -maxWind, +maxWind);
        saturate(_windSpeed[2], -maxWind, +maxWind);

        // simulate wind gusts
        float gustTime = GRandGen.PlusMinus(0, 8 * _overcastSetClouds + 2);
        _gustUntil = Glob.time + gustTime;
        _gust = Vector3(GRandGen.PlusMinus(0, 4 * _overcastSetClouds), GRandGen.PlusMinus(0, 1 * _overcastSetClouds),
                        GRandGen.PlusMinus(0, 4 * _overcastSetClouds));
    }
}

void Landscape::SetSkyTexture(Texture* texture)
{
    if (_skyObject)
    {
        Shape* shape = _skyObject->GetShape()->LevelOpaque(0);
        // note: face SetTexture is no longer necessary here
        // texture information is used only from sections
        for (Offset f = shape->BeginFaces(); f < shape->EndFaces(); shape->NextFace(f))
        {
            Poly& face = shape->Face(f);
            face.SetTexture(texture);
        }
        if (shape->NSections() > 0)
        {
            shape->GetSection(0).properties.SetTexture(texture);
        }
    }
    _world->GetScene()->CalculateSkyColor(texture);
}

Texture* Landscape::SkyTexture()
{
    return _weather.SkyTexture();
}

void Landscape::Simulate(float deltaT)
{
    // simulate weather changes
    _weather.MoveClouds(deltaT);
    // simulate tide
    const LightSun* sun = GLOB_SCENE->MainLight();
    // consider sun and moon
    Vector3Val sunDir = sun->SunDirection();
    Vector3Val moonDir = sun->MoonDirection();
    // Vector3 sunTide=(VUp*sunDir)*sunDir;
    // Vector3 moonTide=(VUp*moonDir)*moonDir;
    Vector3 sunTide = sunDir[1] * sunDir;
    Vector3 moonTide = moonDir[1] * moonDir;
    float tide = (sunTide.Y() + moonTide.Y()) * 0.5;
    // consider moon. Foreign (OPRW) worlds have no tide: their sea is authored at 0 and this
    // term put it metres over their harbours -- and moved it when the moon changed (see
    // LoadOprwModern). Log once so a "sea level rose" report can be answered from the log.
    _seaLevel = _tidal ? maxTide * tide : 0.0f;
    static int loggedTidal = -1; // re-log when the world flips between tidal and not
    if (loggedTidal != int(_tidal))
    {
        loggedTidal = int(_tidal);
        LOG_INFO(World, "Sea level: tidal={} tide={:.3f} (sun {:.3f} moon {:.3f}) -> {:.2f} m", _tidal, tide,
                 sunTide.Y(), moonTide.Y(), _seaLevel);
    }
    // simulate waves
    float wave = sin(2 * H_PI * Glob.time.toFloat() * _seaWaveSpeed);
    // float waveSize=(GRandGen.RandomValue()*0.5+0.5)
    _seaLevelWave = _seaLevel + wave * maxWave;
    // simulate wind speed changes
}

Vector3 Landscape::GetWind() const
{
    // Forward to the world's single wind authority. Every legacy consumer -- smoke
    // sources, flag cloth, parachutes, helicopter turbulence -- already funnels through
    // here, so this one forwarder unifies all of them without touching a call site.
    //
    // The legacy random walk in Weather::MoveClouds is deliberately LEFT RUNNING even
    // though nothing reads its result now: it draws five values from the shared GRandGen
    // stream every 5 seconds, and removing those draws would shift every downstream
    // random result in a classic mission. It is inert, not dead.
    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        return Vector3(wind.velocityX, 0.0f, wind.velocityZ);
    }
    // Fallback for a Landscape used outside a World: exact legacy behaviour.
    Vector3 ret = _weather._windSpeed + Vector3(4, 0, 2) * _weather._overcastSetClouds;
    if (Glob.time < _weather._gustUntil)
    {
        ret += _weather._gust;
    }
    return ret;
}

bool Landscape::VerifyStructure() const
{
    return _segCache.VerifyStructure();
}

void Landscape::MakeShadows(Scene& scene)
{
    // make shadow shapes for all existing shapes
}

Texture* Landscape::ClippedTexture(int z, int x) const
{
    if (this_InRange(z, x))
    {
        int index = GetTex(x, z);
        return _texture[index];
    }
    return _texture[0];
}

int Landscape::ClippedTextureIndex(int z, int x) const
{
    if (this_InRange(z, x))
    {
        return GetTex(x, z);
    }
    return 0;
}

float Landscape::GetHeight(int x, int z) const
{
    return ClippedData(x, z);
}

int Landscape::GetTexture(int z, int x) const
{
    return ClippedTextureIndex(z, x);
}

const SurfaceInfo& Landscape::SurfaceAt(float x, float z) const
{
    float xRel = x * _invTerrainGrid;
    float zRel = z * _invTerrainGrid;
    int xi = toIntFloor(xRel);
    int zi = toIntFloor(zRel);

    // the texture grid is coarser than the height grid by tLog
    int tLog = _terrainRangeLog - _landRangeLog;
    int xti = xi >> tLog;
    int zti = zi >> tLog;

    const TextureInfo& info = _texture[ClippedTextureIndex(zti, xti)];

    // fractional position within the texture cell; u runs along +X and v along
    // +Z, matching the terrain UV mapping baked for clamped (pre-blended) tiles
    int span = 1 << tLog;
    float u = (xRel - float(xti << tLog)) / span;
    float v = (zRel - float(zti << tLog)) / span;

    // quadrants are ordered TL, TR, BL, BR with the image top-left at (u=0, v=0)
    int quadrant = (v < 0.5f ? 0 : 2) + (u < 0.5f ? 0 : 1);
    const SurfaceInfo* surface = info.quadrants[quadrant];
    if (!surface)
    {
        static const SurfaceInfo empty = {};
        return empty;
    }
    return *surface;
}

bool Landscape::ClippedIsWater(int z, int x) const
{
    if (!this_InRange(z, x) || !this_InRange(z - 1, x - 1))
    {
        return true;
    }
    if (GetTex(x, z) != 0)
    {
        return false;
    }
    if (GetTex(x - 1, z) != 0)
    {
        return false;
    }
    if (GetTex(x, z - 1) != 0)
    {
        return false;
    }
    if (GetTex(x - 1, z - 1) != 0)
    {
        return false;
    }
    return true;
}

void Landscape::ReleaseAllVBuffers()
{
    _segCache._cache.ForEach(
        [](LandCacheSlot& slot)
        {
            if (slot.segment)
            {
                slot.segment->_table.ReleaseVBuffer();
                slot.segment->_wTable.ReleaseVBuffer();
            }
        });

    if (GScene)
    {
        GScene->GetShadowCache().Clear();
        GScene->CleanUp();
    }
}

void Landscape::CreateAllVBuffers()
{
    _segCache._cache.ForEach(
        [](LandCacheSlot& slot)
        {
            if (!slot.segment)
                return;
            LandSegment* segi = slot.segment;
            if (!segi->_onlyWater)
                segi->_table.ConvertToVBuffer(VBBigDiscardable);
            if (segi->_someWater)
                segi->_wTable.ConvertToVBuffer(VBBigDiscardable);
            segi->_needsGPU = false;
        });
}

void Landscape::CreateNearVBuffers(float cx, float cz, float radius)
{
    // Legacy segment VBs are unused under a GPU terrain renderer (see FillCache).
    if (GEngine && GEngine->GetTerrainRenderer())
        return;
    float r2 = radius * radius;
    int created = 0;
    _segCache._cache.ForEach(
        [&](LandCacheSlot& slot)
        {
            if (!slot.segment)
                return;
            LandSegment* segi = slot.segment;
            if (!segi->_needsGPU)
                return;
            float sx = (segi->_rect.xBeg + 0.5f * LandSegmentSize) * _landGrid;
            float sz = (segi->_rect.zBeg + 0.5f * LandSegmentSize) * _landGrid;
            float dx = sx - cx, dz = sz - cz;
            if (dx * dx + dz * dz > r2)
                return;
            if (!segi->_onlyWater)
                segi->_table.ConvertToVBuffer(VBBigDiscardable);
            if (segi->_someWater)
                segi->_wTable.ConvertToVBuffer(VBBigDiscardable);
            segi->_needsGPU = false;
            created++;
        });
    LOG_DEBUG(Core, "LOAD: CreateNearVBuffers: {} segs (radius={})", created, radius);
}

void Landscape::FlushCache()
{
    _operCache = CreateOperCache(this);
    // Craters and editor changes flush caches while units still own LockField
    // pointers. Deleting those fields here leaves AILocker with dangling
    // pointers and makes its later release target a new, empty lock cache.
    // Occupancy is live simulation state, not cached terrain geometry. Retain
    // it until its owners release it; unloaded worlds have an empty cache and
    // still recreate it against their current grid dimensions.
    if (!_lockCache || _lockCache->IsEmpty())
        _lockCache = CreateLockCache(this);
    _segCache.Clear();
    if (GScene)
    {
        GScene->GetShadowCache().Clear();
    }
}

void Landscape::FillCache(const Frame& pos)
{
    // The legacy segment cache is only consumed by the CPU/GL33 terrain path. Under a GPU
    // terrain renderer (wgpu) the ground is drawn from the heightmap directly, so pre-filling
    // it — and recomputing every tile on setTerrainGrid or a big camera jump — is pure
    // overhead. Skip it here so every caller (load, vehicle switch, setTerrainGrid) is covered;
    // grass overlays still generate their segments on demand in DrawGround.
    if (GEngine && GEngine->GetTerrainRenderer())
        return;
    if (ENGINE_CONFIG.noTerrainCache)
        return;
    LOG_DEBUG(World, "Recreate caches {:.1f},{:.1f}", pos.Position().X(), pos.Position().Z());
    _segCache.Fill(this, pos, Glob.time.toFloat(), ENGINE_CONFIG.horizontZ, Poseidon::GetGlobalTaskPool());
}

void Landscape::HeightChange(int x, int z, float y)
{
    PoseidonAssert(this_TerrainInRange(x, z));
    const bool inRange=this_TerrainInRange(x,z);
    const float before=inRange?GetData(x,z):std::numeric_limits<float>::quiet_NaN();
    SetData(x, z, y);
    ++_heightRevision;
    // Publish only actual committed finite vertex changes. Brush/crater callers
    // reach this after their successful physics patch, never before it.
    const float committed=inRange?GetData(x,z):std::numeric_limits<float>::quiet_NaN();
    if(inRange&&std::isfinite(committed)&&before!=committed)
        GRainWater().RecordTerrainVertex(this,_terrainRange,_terrainGrid,x,z,_heightRevision);
    else if(inRange&&std::isfinite(before)&&std::isfinite(committed))
        GRainWater().RecordTerrainUnchanged(this,_terrainRange,_terrainGrid,x,z,_heightRevision);
    // FlushCache();
}

void Landscape::TextureChange(int x, int z, int id)
{
    PoseidonAssert(this_InRange(x, z));
    SetTex(x, z, id);
    // FlushCache();
}

void Landscape::Dim(int x, int z, int rx, int rz, float landGrid)
{
    int xLog = 0;
    int xVal = x;
    while (xVal > 1)
    {
        xLog++, xVal >>= 1;
    }

    int rxLog = 0;
    int rxVal = rx;
    while (rxVal > 1)
    {
        rxLog++, rxVal >>= 1;
    }

    if (x != z)
    {
        ErrorMessage("Landscape dimensions %dx%d not rectangular", x, z);
    }
    if (x != 1 << xLog)
    {
        ErrorMessage("Landscape dimensions %dx%d not power of 2", x, z);
    }
    if (rx != rz)
    {
        ErrorMessage("Terrain dimensions %dx%d not rectangular", rx, rz);
    }
    if (rx != 1 << rxLog)
    {
        ErrorMessage("Terrain dimensions %dx%d not power of 2", rx, rz);
    }
    if (rx < x || (rx % x) != 0)
    {
        ErrorMessage("Terrain dimensions %dx%d must refine land dimensions %dx%d", rx, rz, x, z);
    }

    _landRange = x;
    _invLandRange = 1.0f / x;
    _landRangeMask = x - 1;
    _landRangeLog = xLog;

    _terrainRange = rx;
    _terrainRangeMask = rx - 1;
    _terrainRangeLog = rxLog;

    _geography.Dim(x, z);
    _soundMap.Dim(x, z);
    _tex.Dim(x, z);
    _objects.Dim(x, z);
    // _terrain, not _land
    _random.Dim(x, z);

    _data.Dim(rx, rz);
    PoseidonAssert(rx >= x && rz >= z);
    PoseidonAssert((rx % x) == 0 && (rz % z) == 0);

    SetLandGrid(landGrid);
    // A (re)load replaces every height: anything keyed on HeightRevision (TerrainWgpu::UploadIfNeeded,
    // conform caches) must not mistake the new world -- or the same world reloaded -- for the old one.
    ++_heightRevision;
}

void Landscape::SetLandGrid(float grid)
{
#if _DEBUG
    // no object may be present
    for (int z = 0; z < _landRange; z++)
    {
        for (int x = 0; x < _landRange; x++)
        {
            const ObjectList& ol = _objects(x, z);
            PoseidonAssert(ol.Size() == 0);
        }
    }
#endif
    _landGrid = grid;
    _invLandGrid = 1 / grid;
    int terrainLog = _terrainRangeLog - _landRangeLog;
    float invTerrainCoef = 1.0 / (1 << terrainLog);
    _terrainGrid = grid * invTerrainCoef;
    _invTerrainGrid = 1 / _terrainGrid;
}

void Landscape::DoConstruct(Engine* engine, World* world)
{
    _engine = engine;
    _world = world;
    _texture.Clear();
    // for( int i=0; i<MAX_NETWORKS; i++ ) _networks[i]=nullptr;
    _operCache = CreateOperCache(this);
    _lockCache = CreateLockCache(this);

    _seaLevel = 0;     // sea level with tide
    _seaLevelWave = 0; // sea level with wave effects
    _seaWaveSpeed = 0.08;
    _lastFindObjectX = -1; // no cached query
    _lastFindObjectZ = -1;
}

} // namespace Poseidon
// Single global storage; keep exactly one. A second copy (a separate
// Poseidon::GLandscape) desyncs from this one and crashes viewer-mode
// landscape init.
namespace Poseidon
{
Landscape* GLandscape;

#pragma warning(disable : 4355)

Landscape::Landscape(Engine* engine, World* world, bool nets) : _nets(nets), _randGen(8799656, 148756)
{
    // Dim(256,256);
    Dim(32, 32, 32, 32, 50);

    DoConstruct(engine, world);
    Init();
}

DEFINE_FAST_ALLOCATOR(LandSegment)

LandSegment::LandSegment()
{
    _lastUsed = Glob.time;
    Clear();
}

LandSegment::~LandSegment() = default;

void LandSegment::Clear()
{
    _table.Clear();
    _wTable.Clear();
    _rect.xBeg = INT_MAX, _rect.xEnd = INT_MIN; // invalid rectangle
    _rect.zBeg = INT_MAX, _rect.zEnd = INT_MIN;
    _valid = false;
    _someWater = false;
    _offset = VZero;
}

bool LandSegment::VerifyStructure() const
{
    return true;
}

bool LandSegment::ValidFor(const LandBegEnd& rect) const
{
    if (!_valid)
    {
        return false;
    }
    if (_rect.xBeg > rect.xBeg)
    {
        return false;
    }
    if (_rect.xEnd < rect.xEnd)
    {
        return false;
    }
    if (_rect.zBeg > rect.zBeg)
    {
        return false;
    }
    if (_rect.zEnd < rect.zEnd)
    {
        return false;
    }
    return true;
}

void LandSegment::CalcBSphere()
{
    // scan all vertices
}

void LandSegment::CalcWBSphere() {}

LandCache::LandCache() : _cache(16)
{
    RegisterMemoryFreeOnDemand(this);
}

void LandCache::Clear()
{
    _cache.Clear();
}

// estimated memory used by of LandSegment

const float LandSegmentMemSize = LandSegmentSize * LandSegmentSize * sizeof(TLVertex);

size_t LandCache::FreeOneItem()
{
    if (_cache.Size() == 0)
        return 0;
    _cache.EvictLRU();
    return (size_t)LandSegmentMemSize;
}

float LandCache::Priority()
{
    // estimated time to create LandSegment (CPU cycles)
    const float itemTime = 100000;
    // estimated time per byte
    return itemTime / LandSegmentMemSize;
}

bool LandCache::VerifyStructure() const
{
    bool valid = true;
    _cache.ForEach(
        [&](const LandCacheSlot& slot)
        {
            if (slot.segment && !slot.segment->VerifyStructure())
                valid = false;
        });
    return valid;
}

Ref<LandSegment> LandCache::Segment(Landscape* land, const LandBegEnd& rect, float currentTime)
{
    // FAR-001: the LOD this segment must be at, given the coarse ring armed for
    // this frame. Always 0 inside the ordinary view distance, and always 0 while
    // the ring is disarmed -- which is every frame of ground play, so this is the
    // literal 0 that used to be hard-coded at the Lookup below.
    const int requiredLod = land->RequiredSegmentLod(rect.xBeg, rect.zBeg);

    // When terrain caching is disabled, regenerate every segment every frame
    if (ENGINE_CONFIG.noTerrainCache)
    {
        auto gt0 = TerrainProfile::Now();
        Ref<LandSegment> seg = land->GenerateSegment(rect, false, requiredLod);
        GTerrainProfile.generateSegCycles += (TerrainProfile::Now() - gt0);
        GTerrainProfile.segmentsCacheMiss++;
        GTerrainProfile.segmentsDrawn++;
        return seg;
    }

    auto result = _cache.Lookup(rect.xBeg, rect.zBeg, requiredLod, currentTime,
                                [&](int, int, int lod, float time)
                                {
                                    auto gt0 = TerrainProfile::Now();
                                    Ref<LandSegment> seg = land->GenerateSegment(rect, false, lod);
                                    GTerrainProfile.generateSegCycles += (TerrainProfile::Now() - gt0);

                                    auto slot = std::make_unique<LandCacheSlot>();
                                    slot->xBeg = rect.xBeg;
                                    slot->zBeg = rect.zBeg;
                                    slot->lodLevel = lod;
                                    slot->lastUsed = time;
                                    slot->segment = std::move(seg);
                                    return slot;
                                });

    if (result.wasHit)
        GTerrainProfile.segmentsCacheHit++;
    else
        GTerrainProfile.segmentsCacheMiss++;
    GTerrainProfile.segmentsDrawn++;
    return result.entry->segment;
}

void LandCache::Init(float viewDistance, float invLandGrid)
{
    _cache.SetMaxN(CalculateCacheSize(viewDistance, invLandGrid));
}

void LandCache::Fill(Landscape* land, const Frame& pos, float currentTime, float viewDistance, Poseidon::TaskPool* pool)
{
    auto tFill = TerrainProfile::Now();
    _cache.SetMaxN(CalculateCacheSize(viewDistance, land->GetInvLandGrid()));

    // Pass 1: collect cache-miss rects
    std::vector<LandBegEnd> missRects;
    // FAR-001: the LOD each miss must be generated at, parallel to missRects.
    std::vector<int> missLods;
    int x = toIntFloor(pos.Position().X() * land->GetInvLandGrid());
    int z = toIntFloor(pos.Position().Z() * land->GetInvLandGrid());
    LOG_DEBUG(Core, "LandCache fill: maxN={} cached={} pos=({},{}) grid={}", _cache.GetMaxN(), (int)_cache.Size(),
              pos.Position().X(), pos.Position().Z(), x);
    int maxRange = 64;
    x &= ~(LandSegmentSize - 1);
    z &= ~(LandSegmentSize - 1);
    for (int range = 0; range < maxRange; range++)
    {
        for (int xx = -range; xx <= range; xx++)
        {
            for (int zz = -range; zz <= range; zz++)
            {
                if (xx == -range || xx == +range || zz == -range || zz == +range)
                {
                    if ((int)_cache.Size() + (int)missRects.size() >= _cache.GetMaxN())
                    {
                        break;
                    }
                    int xxx = x + xx * LandSegmentSize;
                    int zzz = z + zz * LandSegmentSize;
                    // FAR-001. This used to compare against a literal 0, which
                    // made `Fill` the enemy of any coarse tier: it walks outwards
                    // from the camera and EVICTS-AND-REGENERATES every cached
                    // segment that is not LOD 0, so a far ring built by the draw
                    // path would be quietly rebuilt at full detail by the
                    // prefetch path on the next recentre -- costing the
                    // regeneration and delivering none of the saving, with no
                    // symptom but a slow frame. Both paths ask the same policy
                    // now. With the ring disarmed it still yields 0 everywhere
                    // and this is the behaviour it always had.
                    const int wantLod = land->RequiredSegmentLod(xxx, zzz);
                    auto* existing = _cache.Find(xxx, zzz);
                    if (existing)
                    {
                        if (existing->lodLevel != wantLod)
                        {
                            _cache.Remove(xxx, zzz);
                            LandBegEnd rect;
                            rect.xBeg = xxx;
                            rect.zBeg = zzz;
                            rect.xEnd = xxx + LandSegmentSize;
                            rect.zEnd = zzz + LandSegmentSize;
                            missRects.push_back(rect);
                            missLods.push_back(wantLod);
                            continue;
                        }
                        _cache.TouchMRU(xxx, zzz, currentTime);
                        GTerrainProfile.segmentsCacheHit++;
                        GTerrainProfile.segmentsDrawn++;
                        continue;
                    }
                    LandBegEnd rect;
                    rect.xBeg = xxx;
                    rect.zBeg = zzz;
                    rect.xEnd = xxx + LandSegmentSize;
                    rect.zEnd = zzz + LandSegmentSize;
                    missRects.push_back(rect);
                    missLods.push_back(wantLod);
                }
            }
        }
    }

    if (missRects.empty())
    {
        auto fillMs = (TerrainProfile::Now() - tFill) / 3e6;
        LOG_DEBUG(Core, "LandCache fill: {} segs (of {}) in {}ms [0 misses]", (int)_cache.Size(), _cache.GetMaxN(),
                  fillMs);
        return;
    }

    // Pass 2: pre-allocate segments (main thread — FastCAlloc not thread-safe)
    const int nMiss = (int)missRects.size();
    auto t2 = TerrainProfile::Now();
    std::vector<Ref<LandSegment>> newSegs(nMiss);
    for (int i = 0; i < nMiss; i++)
        newSegs[i] = new LandSegment;
    auto t2ms = (TerrainProfile::Now() - t2) / 3e6;

    // Segments are generated at LOD 0 inside the ordinary view distance -- which
    // is where T-junction gaps would be visible, and is everything during ground
    // play -- and at the coarse-ring LOD only outside it (FAR-001).
    // Pass 3: parallel CPU generation via TaskPool (deferred GPU)
    auto gt0 = TerrainProfile::Now();
    if (pool && nMiss > 1)
    {
        pool->ParallelFor(static_cast<uint32_t>(nMiss),
                          [land, &missRects, &missLods, &newSegs](uint32_t start, uint32_t end)
                          {
                              for (uint32_t i = start; i < end; i++)
                              {
                                  land->GenerateSegmentInto(newSegs[i], missRects[i], true, missLods[i]);
                              }
                          });
    }
    else
    {
        for (int i = 0; i < nMiss; i++)
        {
            land->GenerateSegmentInto(newSegs[i], missRects[i], true, missLods[i]);
        }
    }
    auto t3ms = (TerrainProfile::Now() - gt0) / 3e6;
    GTerrainProfile.generateSegCycles += (TerrainProfile::Now() - gt0);
    GTerrainProfile.segmentsCacheMiss += nMiss;
    GTerrainProfile.segmentsDrawn += nMiss;

    // Pass 4: mark for lazy GPU init + insert into cache (main thread)
    for (int i = 0; i < nMiss; i++)
    {
        Ref<LandSegment>& seg = newSegs[i];
        seg->_needsGPU = true;

        while ((int)_cache.Size() >= _cache.GetMaxN())
        {
            if (!_cache.EvictLRU())
                break;
        }

        auto slot = std::make_unique<LandCacheSlot>();
        slot->xBeg = missRects[i].xBeg;
        slot->zBeg = missRects[i].zBeg;
        // Must record the LOD it was actually generated at, not 0: the cache
        // invalidates on a lodLevel mismatch, so a slot that lies about its LOD is
        // regenerated every single frame (FAR-001).
        slot->lodLevel = missLods[i];
        slot->lastUsed = currentTime;
        slot->segment = seg;
        _cache.Insert(std::move(slot));
    }

    auto fillMs = (TerrainProfile::Now() - tFill) / 3e6;
    LOG_DEBUG(Core, "LandCache fill: {} segs in {}ms [{} misses: alloc={}ms gen={}ms]", (int)_cache.Size(), fillMs,
              nMiss, t2ms, t3ms);
}

static int CmpALight(const ActiveLightPointer* l0, const ActiveLightPointer* l1, LightContext* context)
{
    const Light* light0 = *l0;
    const Light* light1 = *l1;
    PoseidonAssert(light0);
    PoseidonAssert(light1);
    PoseidonAssert(light0 != light1);
    return light1->Compare(*light0, *context);
}

void Landscape::RegisterTexture(int id, const char* name)
{
    SetTexture(id, name);
}
void Landscape::RegisterObjectType(const char* name)
{
    // add a shape into the bank
    Ref<LODShapeWithShadow> shape = Shapes.New(name, false, true);
}

void Landscape::Quit() {}

void Landscape::SetTexture(int i, const char* name)
{
    char aName[64];
    snprintf(aName, sizeof(aName), "%s", (const char*)name);
    strlwr(aName);
    // some engines use reflections - use alpha instead of water
    // if( !strcmp(aName,"landtext\\mo.pac") )
    if (i == 0)
    {
        snprintf(aName, sizeof(aName), "%s", (const char*)"data\\more_anim.01.pac");
        AnimatedTexture* aWater = GlobLoadTextureAnimated("data\\more_anim.01.pac");
        _texture.Access(i);
        if (aWater)
        {
            _texture[i].texture = (*aWater)[0];
            _texture[i].offsetUV = false;
            for (int a = 0; a < aWater->Size(); a++)
            {
                Texture* aw = aWater->Get(a);
                if (aw)
                {
                    aw->SetMultitexturing(1); // specular highlihts
                    aw->SetMaxSize(ENGINE_CONFIG.maxLandText);
                }
            }
        }
        else
        {
            _texture[i].texture = nullptr;
            _texture[i].offsetUV = false;
        }
        if (GLOB_ENGINE && GLOB_ENGINE->TextBank())
        {
            GLOB_ENGINE->TextBank()->GetQuadrantSurfaces(aName, _texture[i].quadrants);
        }
        return;
    }
    _texture.Access(i);
    _texture[i].texture = GlobLoadTexture(aName);
    if (_texture[i].texture)
    {
        _texture[i].texture->SetMultitexturing(1); // detail texture
        _texture[i].texture->SetMaxSize(ENGINE_CONFIG.maxLandText);
        bool simple = false;
        {
            // check if it is simple texture
            const char* name = strchr(aName, PATH_SEP);
            if (!name)
                name = strchr(aName, (PATH_SEP == '/') ? '\\' : '/');
            const char* ext = strrchr(aName, '.');
            if (ext == name + 3)
            {
                simple = true;
            }
        }
        _texture[i].offsetUV = simple;
    }
    else
    {
        _texture[i].offsetUV = true;
    }
    if (GLOB_ENGINE && GLOB_ENGINE->TextBank())
    {
        GLOB_ENGINE->TextBank()->GetQuadrantSurfaces(aName, _texture[i].quadrants);
    }
}

Landscape::~Landscape()
{
    _modernPendingOwnerPrimaryTextures.reset();
    ResetModernSimulationResidency();
    ClearNativeFarResidency();
    // clear all objects
    int x, z;
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            _objects(x, z).Clear();
        }
    }
}

float Landscape::CalculateBump(float xC, float zC, Texture* texture, float bumpy) const
{
    // const float bumpScale=1.0/0.3;
    const float bumpScale = 1.0;
    float bumpX = xC * bumpScale;
    float bumpZ = zC * bumpScale;
    int iBumpX = toIntFloor(bumpX);
    int iBumpZ = toIntFloor(bumpZ);
    //
    // RandBegin(iBumpX,iBumpZ);
    float bump00 = _randGen.RandomValue(_randGen.GetSeed(iBumpX, iBumpZ));
    float bump01 = _randGen.RandomValue(_randGen.GetSeed(iBumpX + 1, iBumpZ));
    float bump10 = _randGen.RandomValue(_randGen.GetSeed(iBumpX, iBumpZ + 1));
    float bump11 = _randGen.RandomValue(_randGen.GetSeed(iBumpX + 1, iBumpZ + 1));

#if 1
    bumpX -= iBumpX; // relative in-bump coordinates
    bumpZ -= iBumpZ;
    // bilinear interpolation of bumpY
    float bump0 = bump00 + (bump01 - bump00) * bumpX;
    float bump1 = bump10 + (bump11 - bump10) * bumpX;
    float bump = bump0 + (bump1 - bump0) * bumpZ; // result in range 0 .. 1
    bump -= 0.5;
#else
    const float bump = 0;
#endif
    float tr = 0.05;
    // Note: check where does nullptr texture come from
    if (texture)
    {
        tr = texture->Roughness();
    }
    return bump * tr * bumpy;
}

float Landscape::BumpySurfaceY(float xC, float zC, float& rdX, float& rdY, Texture*& texture, float bumpy,
                               float& bump) const
{
    // fine rectangles are not used - use rough instead
    // calculate surface level on given coordinates
    float xRel = xC * _invTerrainGrid;
    float zRel = zC * _invTerrainGrid;
    int x = toIntFloor(xRel);
    int z = toIntFloor(zRel);
    float xIn = xRel - x; // relative 0..1 in square
    float zIn = zRel - z;

    bump = 0;
#if !USE_SWIZZLED_ARRAYS
    if (!this_TerrainInRange(z, x) || !this_TerrainInRange(z + 1, x + 1))
    {
        return YOutsideMap; // no bump outside landscape
    }
    float y00 = GetData(x, z);
    float y01 = GetData(x + 1, z);
    float y10 = GetData(x, z + 1);
    float y11 = GetData(x + 1, z + 1);
#else
    if (!this_TerrainInRange(z, x) || !this_TerrainInRange(z + 1, x + 1))
        return YOutsideMap; // no bump outside landscape
    RawType y4[2][2];
    _data.GetFour(y4, x, z);
    float y00 = RawToHeight(y4[0][0]);
    float y01 = RawToHeight(y4[0][1]);
    float y10 = RawToHeight(y4[1][0]);
    float y11 = RawToHeight(y4[1][1]);
#endif

    // each face is divided to two triangles
    // determine which triangle contains point
    float y;
    if (xIn <= 1 - zIn)
    { // triangle 00,01,10
        rdX = (y01 - y00) * _invTerrainGrid;
        rdY = (y10 - y00) * _invTerrainGrid;
        y = y00 + (y10 - y00) * zIn + (y01 - y00) * xIn;
    }
    else
    {
        // triangle 01,10,11
        rdX = (y11 - y10) * _invTerrainGrid;
        rdY = (y11 - y01) * _invTerrainGrid;
        y = y11 + (y10 - y11) * (1 - xIn) + (y01 - y11) * (1 - zIn);
    }
    // depending on surface texture use roughness
    int tLog = _terrainRangeLog - _landRangeLog;
    Texture* rTexture = ClippedTexture(z >> tLog, x >> tLog);
    bump = CalculateBump(xC, zC, rTexture, bumpy);
    texture = rTexture;
    auto mudGradient = GMud().HeightGradientAt(xC, zC);
    const auto sandGradient = GSand().HeightGradientAt(xC, zC);
    mudGradient[0] += sandGradient[0]; mudGradient[1] += sandGradient[1];
    rdX += mudGradient[0];
    rdY += mudGradient[1];
    return y + GMud().HeightOffsetAt(xC, zC) + GSand().HeightOffsetAt(xC, zC);
}

void Landscape::SurfacePlane(Plane& plane, float xC, float zC) const
{
    // calculate surface level on given coordinates
    float xRel = xC * _invTerrainGrid;
    float zRel = zC * _invTerrainGrid;
    int x = toIntFloor(xRel);
    int z = toIntFloor(zRel);
    float xIn = xRel - x; // relative 0..1 in square
    float zIn = zRel - z;

#if !USE_SWIZZLED_ARRAYS
    if (!this_TerrainInRange(z, x) || !this_TerrainInRange(z + 1, x + 1))
    {
        plane.SetNormal(VUp, -YOutsideMap);
        return;
    }
    float y00 = GetData(x, z);
    float y01 = GetData(x + 1, z);
    float y10 = GetData(x, z + 1);
    float y11 = GetData(x + 1, z + 1);
#else
    if (!this_TerrainInRange(z, x) || !this_TerrainInRange(z + 1, x + 1))
        return YOutsideMap; // no bump outside landscape
    RawType y4[2][2];
    _data.GetFour(y4, x, z);
    float y00 = RawToHeight(y4[0][0]);
    float y01 = RawToHeight(y4[0][1]);
    float y10 = RawToHeight(y4[1][0]);
    float y11 = RawToHeight(y4[1][1]);
#endif

    // each face is divided to two triangles
    // determine which triangle contains point
    Vector3 normal = VUp;
    if (xIn <= 1 - zIn)
    { // triangle 00,01,10

        normal[0] = (y01 - y00) * -_invTerrainGrid;
        normal[2] = (y10 - y00) * -_invTerrainGrid;
    }
    else
    {
        // triangle 01,10,11
        normal[0] = (y11 - y10) * -_invTerrainGrid;
        normal[2] = (y11 - y01) * -_invTerrainGrid;
    }
    Vector3 point((x + 1) * _terrainGrid, y01, z * _terrainGrid);
    auto mudGradient = GMud().HeightGradientAt(xC, zC);
    const auto sandGradient = GSand().HeightGradientAt(xC, zC);
    mudGradient[0] += sandGradient[0]; mudGradient[1] += sandGradient[1];
    const float mudHeight = GMud().HeightOffsetAt(xC, zC) + GSand().HeightOffsetAt(xC, zC);
    if (mudHeight != 0 || mudGradient[0] != 0 || mudGradient[1] != 0)
    {
        normal[0] -= mudGradient[0];
        normal[2] -= mudGradient[1];
        // The original plane passes through the terrain-cell corner. A local
        // boot depression instead needs its tangent anchored at this query.
        point = Vector3(xC, SurfaceY(xC, zC), zC);
    }
    plane = Plane(normal, point);
}

float Landscape::SurfaceY(float x, float z) const
{
    // code equivalent to
    // return SurfaceY(x,z,nullptr,nullptr,nullptr);
    // as this case is quite common, it is optimized

    // it might be also worth to separate SurfaceY into two parts:
    // preparation and calculation
    // preparation would gather and return yii values
    // fine rectangles are not used - use rough instead
    // calculate surface level on given coordinates
    float xRel = x * _invTerrainGrid;
    float zRel = z * _invTerrainGrid;
    int xi = toIntFloor(xRel);
    int zi = toIntFloor(zRel);
    float xIn = xRel - xi; // relative 0..1 in square
    float zIn = zRel - zi;

#if !USE_SWIZZLED_ARRAYS
    if (!this_TerrainInRange(zi, xi) || !this_TerrainInRange(zi + 1, xi + 1))
    {
        return YOutsideMap; // no bump outside landscape
    }
    float y00 = GetData(xi, zi);
    float y01 = GetData(xi + 1, zi);
    float y10 = GetData(xi, zi + 1);
    float y11 = GetData(xi + 1, zi + 1);
#else
    if (!this_TerrainInRange(zi, xi) || !this_TerrainInRange(zi + 1, xi + 1))
        return YOutsideMap; // no bump outside landscape
    RawType y4[2][2];
    _data.GetFour(y4, xi, zi);
    float y00 = RawToHeight(y4[0][0]);
    float y01 = RawToHeight(y4[0][1]);
    float y10 = RawToHeight(y4[1][0]);
    float y11 = RawToHeight(y4[1][1]);
#endif

    // each face is divided to two triangles
    // determine which triangle contains point
    if (xIn <= 1 - zIn)
    { // triangle 00,01,10
        float d1000 = y10 - y00;
        float d0100 = y01 - y00;
        return y00 + d1000 * zIn + d0100 * xIn + GMud().HeightOffsetAt(x, z) + GSand().HeightOffsetAt(x, z);
    }
    else
    {
        // triangle 01,10,11
        float d1011 = y10 - y11;
        float d0111 = y01 - y11;
        return y10 + d0111 - d1011 * xIn - d0111 * zIn + GMud().HeightOffsetAt(x, z) + GSand().HeightOffsetAt(x, z);
    }
}

float Landscape::SurfaceY(float x, float z, float* rdX, float* rdY, Texture** texture) const
{
    // fine rectangles are not used - use rough instead
    // calculate surface level on given coordinates
    float xRel = x * _invTerrainGrid;
    float zRel = z * _invTerrainGrid;
    int xi = toIntFloor(xRel);
    int zi = toIntFloor(zRel);
    float xIn = xRel - xi; // relative 0..1 in square
    float zIn = zRel - zi;

    if (texture)
    {
        int tLog = _terrainRangeLog - _landRangeLog;
        *texture = ClippedTexture(zi >> tLog, xi >> tLog);
    }

#if !USE_SWIZZLED_ARRAYS
    if (!this_TerrainInRange(zi, xi) || !this_TerrainInRange(zi + 1, xi + 1))
    {
        return YOutsideMap; // no bump outside landscape
    }
    float y00 = GetData(xi, zi);
    float y01 = GetData(xi + 1, zi);
    float y10 = GetData(xi, zi + 1);
    float y11 = GetData(xi + 1, zi + 1);
#else
    if (!this_TerrainInRange(zi, xi) || !this_TerrainInRange(zi + 1, xi + 1))
        return YOutsideMap; // no bump outside landscape
    RawType y4[2][2];
    _data.GetFour(y4, xi, zi);
    float y00 = RawToHeight(y4[0][0]);
    float y01 = RawToHeight(y4[0][1]);
    float y10 = RawToHeight(y4[1][0]);
    float y11 = RawToHeight(y4[1][1]);
#endif

    // each face is divided to two triangles
    // determine which triangle contains point
    if (xIn <= 1 - zIn)
    { // triangle 00,01,10
        float d1000 = y10 - y00;
        float d0100 = y01 - y00;
        if (rdX)
        {
            *rdX = d0100 * _invTerrainGrid;
            *rdY = d1000 * _invTerrainGrid;
            auto mudGradient = GMud().HeightGradientAt(x, z);
            const auto sandGradient = GSand().HeightGradientAt(x, z);
            mudGradient[0] += sandGradient[0]; mudGradient[1] += sandGradient[1];
            *rdX += mudGradient[0]; *rdY += mudGradient[1];
        }
        return y00 + d1000 * zIn + d0100 * xIn + GMud().HeightOffsetAt(x, z) + GSand().HeightOffsetAt(x, z);
    }
    else
    {
        // triangle 01,10,11
        float d1011 = y10 - y11;
        float d0111 = y01 - y11;
        if (rdX)
        {
            *rdX = d1011 * -_invTerrainGrid;
            *rdY = d0111 * -_invTerrainGrid;
            auto mudGradient = GMud().HeightGradientAt(x, z);
            const auto sandGradient = GSand().HeightGradientAt(x, z);
            mudGradient[0] += sandGradient[0]; mudGradient[1] += sandGradient[1];
            *rdX += mudGradient[0]; *rdY += mudGradient[1];
        }
        return y10 + d0111 - d1011 * xIn - d0111 * zIn + GMud().HeightOffsetAt(x, z) + GSand().HeightOffsetAt(x, z);
    }
}

float Landscape::RoadSurfaceY(float xC, float zC, float* dX, float* dZ, Texture** texture) const
{
    Texture* surfTexture = nullptr;
    float sdX, sdZ;
    float landY = SurfaceY(xC, zC, &sdX, &sdZ, &surfTexture);

    // if we are on road, return road surface parameters
    // check all near network object clipping boxes

    int xMin, xMax, zMin, zMax;
    Vector3 pos(xC, 0, zC);
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, 0);
    // PERF-028 ABLATION, off by default: skip the road scan entirely and return the
    // bare terrain height. It removes roads from under everyone's feet, so it is a
    // measurement arm and nothing else -- it answers "what is the whole scan worth"
    // before anyone rebuilds the cell index to make it cheaper.
    static const bool skipRoadScan = []
    {
        const char* v = std::getenv("POSEIDON_ABLATE_ROAD_SURFACE");
        return v != nullptr && v[0] == '1';
    }();
    if (skipRoadScan)
    {
        if (dX) { *dX = sdX; }
        if (dZ) { *dZ = sdZ; }
        if (texture) { *texture = surfTexture; }
        return landY;
    }
    // prepare return variables
    float maxY = -1e10;
    float maxDX = 0, maxDZ = 0;
    Texture* maxTexture = nullptr;
    Point3 ret;
    // scan for roads
    for (int z = zMin; z <= zMax; z++)
    {
        for (int x = xMin; x <= xMax; x++)
        {
            Point3 pos(xC, 0, zC);
            const ObjectList& list = _objects(x, z);
            const ObjectListFull* fullList = list.GetList();
            if (!fullList)
            {
                continue;
            }
            int n = fullList->GetRoadwayObjectCount();
            for (int i = 0; i < n; i++)
            {
                Object* obj = fullList->GetRoadwayObject(i);
                if (!obj)
                {
                    continue; // no collisions with roads
                }
                // check bounding box collision
                LODShape* lShape = obj->GetShape();
                if (!lShape)
                {
                    continue;
                }
                Shape* shape = lShape->RoadwayLevel();
                if (!shape)
                {
                    continue;
                }
                float oRad = lShape->BoundingSphere();
                float distXZ2 = (obj->Position() - pos).SquareSizeXZ();
                if (distXZ2 > oRad * oRad)
                {
                    continue;
                }
                Matrix4Val invTransform = obj->GetInvTransform();
                obj->Animate(lShape->FindRoadwayLevel());

                // roadway ready - check collision
                // has top and sometimes side polygons
                // many polygons can incide if we look from the top
                Vector3 modelPos = invTransform.FastTransform(Point3(xC, 0, zC));
                int fi = 0;
                shape->InitPlanes();
                shape->RecalculateNormalsAsNeeded();
                for (Offset f = shape->BeginFaces(), e = shape->EndFaces(); f < e; shape->NextFace(f), fi++)
                {
                    const Poly& face = shape->Face(f);
                    const Plane& plane = shape->GetPlane(fi);
                    float pdX, pdZ;
                    if (face.InsideFromTop(*shape, plane, modelPos, &modelPos[1], &pdX, &pdZ))
                    {
                        // the face is not sure to be a triangle
                        if (shape->GetOrHints() & ClipLandOn)
                        {
                            if (maxY <= landY)
                            {
                                maxTexture = face.GetTexture();
                                maxY = landY;
                                maxDX = sdX;
                                maxDZ = sdZ;
                                ret = Vector3(xC, landY, zC);
                            }
                        }
                        else if (maxY <= modelPos[1])
                        {
                            maxY = modelPos[1], maxDX = pdX, maxDZ = pdZ;
                            maxTexture = face.GetTexture();
                            ret = obj->PositionModelToWorld(modelPos);
                        }
                    }
                }
                obj->Deanimate(lShape->FindRoadwayLevel());
            }
        }
    }

    // Malprave terrainHole3: in a hole with no roadway under the point, the hole's floor (its lowest roadway point)
    // stands in for the missing ground, so nothing falls through a gap in a floor or under a wall
    float holeFloor = 0;
    bool inHole = false, holeChecked = false;
    if (maxY >= -1e3)
    {
        //_world->GetScene()->DrawCollisionStar(ret);
        // if road is under surface, use normal surface
        // (Malprave terrainHole1: except in a terrain hole, where the ground is cut away and the roadway is the floor)
        if (ret.Y() >= landY || ((holeChecked = true), inHole = InTerrainHole(xC, zC, &holeFloor, landY)))
        {
            if (dX)
            {
                *dX = maxDX, *dZ = maxDZ;
            }
            if (texture)
            {
                *texture = maxTexture;
            }
            return ret.Y();
        }
    }
    if (!holeChecked)
    {
        inHole = InTerrainHole(xC, zC, &holeFloor, landY);
    }
    if (inHole)
    {
        if (texture)
        {
            *texture = surfTexture;
        }
        if (dX)
        {
            *dX = 0, *dZ = 0;
        }
        return holeFloor;
    }
    // otherwise return SurfaceY
    if (texture)
    {
        *texture = surfTexture;
    }
    if (dX)
    {
        *dX = sdX, *dZ = sdZ;
    }
    return landY;
}

float Landscape::RoadSurfaceY(Vector3Par pos, float* dX, float* dZ, Texture** texture, Object** obj) const
{
    Texture* surfTexture = nullptr;
    float sdX, sdZ;
    float landY = SurfaceY(pos.X(), pos.Z(), &sdX, &sdZ, &surfTexture);

    // if we are on road, return road surface parameters
    // check all near network object clipping boxes

    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, 0);
    // prepare return variables
    float maxY = -1e10;
    float maxDX = 0, maxDZ = 0;
    Texture* maxTexture = nullptr;
    Point3 ret;
    Object* dXdZRelToObj = nullptr;
    // scan for roads
    for (int z = zMin; z <= zMax; z++)
    {
        for (int x = xMin; x <= xMax; x++)
        {
            const ObjectList& list = _objects(x, z);
            const ObjectListFull* fullList = list.GetList();
            if (!fullList)
            {
                continue;
            }
            int n = fullList->GetRoadwayObjectCount();
            for (int i = 0; i < n; i++)
            {
                Object* o = fullList->GetRoadwayObject(i);
                if (!o)
                {
                    continue; // no collisions with roads
                }
                // check bounding box collision
                LODShape* lShape = o->GetShape();
                if (!lShape)
                {
                    continue;
                }
                Shape* shape = lShape->RoadwayLevel();
                if (!shape)
                {
                    continue;
                }
                float oRad = lShape->BoundingSphere();
                float distXZ2 = (o->Position() - pos).SquareSizeXZ();
                if (distXZ2 > oRad * oRad)
                {
                    continue;
                }
                Matrix4Val invTransform = o->GetInvTransform();
                // roadway ready - check collision
                // has top and sometimes side polygons
                // many polygons can incide if we look from the top
                Vector3 modelPos = invTransform.FastTransform(pos);
                int fi = 0;
                o->Animate(lShape->FindRoadwayLevel());
                shape->InitPlanes();
                // note: plane may be animated in some cases
                // it may be necessary to recalculate it
                shape->RecalculateNormalsAsNeeded();
                for (Offset f = shape->BeginFaces(), e = shape->EndFaces(); f < e; shape->NextFace(f), fi++)
                {
                    const Poly& face = shape->Face(f);
                    const Plane& plane = shape->GetPlane(fi);
                    float pdX, pdZ;
                    if (face.InsideFromTop(*shape, plane, modelPos, &modelPos[1], &pdX, &pdZ))
                    {
                        // the face is not sure to be a triangle
                        if (shape->GetOrHints() & ClipLandOn)
                        {
                            if (maxY <= landY)
                            {
                                maxTexture = face.GetTexture();
                                maxY = landY;
                                maxDX = sdX;
                                maxDZ = sdZ;
                                ret = Vector3(pos.X(), landY, pos.Z());
                                if (obj)
                                {
                                    *obj = o;
                                }
                            }
                        }
                        else
                        {
                            Vector3 wPos = o->PositionModelToWorld(modelPos);
                            if (wPos[1] <= pos.Y() && maxY <= wPos[1])
                            {
                                // create normal, transform it, calculate dX, dZ as atan
                                maxY = wPos[1], maxDX = pdX, maxDZ = pdZ;
                                maxTexture = face.GetTexture();
                                dXdZRelToObj = o;
                                ret = wPos;
                                if (obj)
                                {
                                    *obj = o;
                                }
                            }
                        }
                    }
                }
                o->Deanimate(lShape->FindRoadwayLevel());
            }
        }
    }

    // Malprave terrainHole3: in a hole with no roadway under the point, the hole's floor (its lowest roadway point)
    // stands in for the missing ground, so nothing falls through a gap in a floor or under a wall
    float holeFloor = 0;
    bool inHole = false, holeChecked = false;
    if (maxY >= -1e3)
    {
        //_world->GetScene()->DrawCollisionStar(ret);
        // if road is under surface, use normal surface
        // (Malprave terrainHole1: except in a terrain hole, where the ground is cut away and the roadway is the floor)
        if (ret.Y() >= landY || ((holeChecked = true), inHole = InTerrainHole(pos.X(), pos.Z(), &holeFloor, pos.Y())))
        {
            if (dX)
            {
                if (dXdZRelToObj)
                {
                    // transform maxDX, maxDZ to world space
                    // create normal
                    Vector3 normal(-maxDX, 1, -maxDZ);
                    normal.Normalize();
                    // transform normal to world space
                    dXdZRelToObj->DirectionModelToWorld(normal, normal);
                    if (fabs(normal.Y()) > 1e-2)
                    {
                        maxDX = -normal.X() / normal.Y();
                        maxDZ = -normal.Z() / normal.Y();
                    }
                    else
                    {
                        maxDX = 0;
                        maxDZ = 0;
                    }
                }
                *dX = maxDX, *dZ = maxDZ;
            }
            if (texture)
            {
                *texture = maxTexture;
            }
            return ret.Y();
        }
    }
    if (!holeChecked)
    {
        inHole = InTerrainHole(pos.X(), pos.Z(), &holeFloor, pos.Y());
    }
    if (inHole)
    {
        if (texture)
        {
            *texture = surfTexture;
        }
        if (dX)
        {
            *dX = 0, *dZ = 0;
        }
        if (obj)
        {
            *obj = nullptr;
        }
        return holeFloor;
    }
    // otherwise return SurfaceY
    if (texture)
    {
        *texture = surfTexture;
    }
    if (dX)
    {
        *dX = sdX, *dZ = sdZ;
    }
    if (obj)
    {
        *obj = nullptr;
    }
    return landY;
}

float Landscape::RoadSurfaceYAboveWater(Vector3Par pos, float* dX, float* dZ, Texture** texture) const
{
    float y = RoadSurfaceY(pos, dX, dZ, texture);
    float minY = GetSeaLevel();
    if (y < minY)
    {
        y = minY;
        if (dX)
        {
            *dX = *dZ = 0;
        }
        if (texture)
        {
            *texture = _texture[0];
        }
    }
    return y;
}

float Landscape::SurfaceYAboveWater(float x, float z) const
{
    float y = SurfaceY(x, z, nullptr, nullptr, nullptr);
    float minY = GetSeaLevel();
    if (y < minY)
    {
        y = minY;
    }
    return y;
}

float Landscape::SurfaceYAboveWater(float x, float z, float* rdX, float* rdZ, Texture** texture) const
{
    float y = SurfaceY(x, z, rdX, rdZ, texture);
    float minY = GetSeaLevel();
    if (y < minY)
    {
        y = minY;
        if (rdX)
        {
            *rdX = *rdZ = 0;
        }
        if (texture)
        {
            *texture = _texture[0];
        }
    }
    return y;
}

float Landscape::RoadSurfaceYAboveWater(float xC, float zC, float* dX, float* dZ, Texture** texture) const
{
    float y = RoadSurfaceY(xC, zC, dX, dZ, texture);
    float minY = GetSeaLevel();
    if (y < minY)
    {
        y = minY;
        if (dX)
        {
            *dX = *dZ = 0;
        }
        if (texture)
        {
            *texture = _texture[0];
        }
    }
    return y;
}

float Landscape::WaterDepth(float xC, float zC) const
{
    return 0;
}

Point3 Landscape::PointOnSurface(float x, float y, float z) const
{
    float surfaceY = SurfaceYAboveWater(x, z);
    return Point3(x, surfaceY + y, z);
}

float Landscape::AboveSurface(Vector3Val pos) const
{
    float surfaceY = SurfaceY(pos.X(), pos.Z());
    return pos.Y() - surfaceY;
}

float Landscape::AboveSurfaceOrWater(Vector3Val pos) const
{
    float surfaceY = SurfaceYAboveWater(pos.X(), pos.Z());
    return pos.Y() - surfaceY;
}

AutoArray<int> Landscape::GetObjectIDList() const
{
    AutoArray<int> ret;
    for (const auto& entry : _objectIds)
    {
        Object* obj = entry.second;
        if (obj)
        {
            ret.Add(obj->ID());
        }
    }
    return ret;
}

// #define LOG_OBJ_SHAPE "handgrenade"

inline bool IsShape(Object* obj, const char* name)
{
    if (!obj->GetShape())
    {
        return false;
    }
    return strstr(obj->GetShape()->Name(), name) != nullptr;
}

void Landscape::AddObject(Object* obj, int* xr, int* zr, bool avoidRecalculation, bool registerVisual)
{
    int x, z;
    SelectObjectList(x, z, obj->Position().X(), obj->Position().Z());
    if (xr)
    {
        *xr = x;
    }
    if (zr)
    {
        *zr = z;
    }
    // add into corresponding list
    ObjectList& list = _objects(x, z);
    Verify(list.Add(obj, x, z, avoidRecalculation) >= 0); // <0  -> error
    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3b): let a GPU-driven
    // backend register this object into its retained scene. No-op unless WGR_GPU_DRIVEN is on.
    if (GEngine && registerVisual)
    {
        GEngine->SceneObjectCreated(obj);
    }
#ifdef LOG_OBJ_SHAPE
    if (IsShape(obj, LOG_OBJ_SHAPE))
    {
        LOG_DEBUG(World, "add obj {:x}:{} at {:.2f},{:.2f} ({},{})", (uintptr_t)obj, obj->GetShape()->Name(),
                  obj->Position().X(), obj->Position().Z(), x, z);
    }
#endif
}

void Landscape::Recalculate()
{
    for (int zz = 0; zz < _landRange; zz++)
    {
        for (int xx = 0; xx < _landRange; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            list.Recalculate();
        }
    }
}

#define ShapeName(s) ((s) ? (const char*)(s)->Name() : "<null>")

void Landscape::RemoveObject(Object* obj)
{
    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3b): drop this object
    // from a GPU-driven backend's retained scene before it leaves. No-op unless enabled.
    if (GEngine)
    {
        GEngine->SceneObjectRemoved(obj);
    }
    int x, z;
    SelectObjectList(x, z, obj->Position().X(), obj->Position().Z());
    ObjectList& list = _objects(x, z);
    for (int i = 0; i < list.Size(); i++)
    {
        if (list[i] == obj)
        {
#ifdef LOG_OBJ_SHAPE
            if (IsShape(obj, LOG_OBJ_SHAPE))
            {
                LOG_DEBUG(World, "del obj {:x}:{} at {:.2f},{:.2f} ({},{})", (uintptr_t)obj, obj->GetShape()->Name(),
                          obj->Position().X(), obj->Position().Z(), x, z);
            }
#endif
            list.Delete(i);
            return;
        }
    }
#ifdef _MSC_VER
    const type_info& type = typeid(*obj);
    LOG_DEBUG(World, "Removed object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), type.name(), obj->Position().X(), obj->Position().Z(), x, z);
#else
    LOG_DEBUG(World, "Removed object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), typeid(*obj).name(), obj->Position().X(), obj->Position().Z(), x, z);
#endif
    // try to patch it: first search in near slots
    int xMin = x - 1, xMax = x + 1;
    int zMin = z - 1, zMax = z + 1;
    saturateMax(xMin, 0), saturateMin(xMax, _landRange - 1);
    saturateMax(zMin, 0), saturateMin(zMax, _landRange - 1);
    for (int zz = zMin; zz <= zMax; zz++)
    {
        for (int xx = xMin; xx <= xMax; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    list.Delete(i);
                    LOG_DEBUG(World, "  found in ({},{}).", xx, zz);
                    return;
                }
            }
        }
    }
    // try to patch it: search for the object in all slots
    for (int zz = 0; zz < _landRange; zz++)
    {
        for (int xx = 0; xx < _landRange; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    list.Delete(i);
                    LOG_DEBUG(World, "  found in ({},{}).", xx, zz);
                    return;
                }
            }
        }
    }
}

void Landscape::MoveObject(Object* obj, const Matrix4& transform)
{
    int xl, zl;
    SelectObjectList(xl, zl, obj->Position().X(), obj->Position().Z());
    ObjectList& list = _objects(xl, zl);

    int xm, zm;
    SelectObjectList(xm, zm, transform.Position().X(), transform.Position().Z());
    ObjectList& move = _objects(xm, zm);

#ifdef LOG_OBJ_SHAPE
    if (IsShape(obj, LOG_OBJ_SHAPE))
    {
        LOG_DEBUG(World, "move trn obj {:x}:{} {:.2f},{:.2f} ({},{})->{:.2f},{:.2f} ({},{})", (uintptr_t)obj,
                  obj->GetShape()->Name(), obj->Position().X(), obj->Position().Z(), xl, zl, transform.Position().X(),
                  transform.Position().Z(), xm, zm);
    }
#endif
    obj->SetTransform(transform);
    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3b): patch this object's
    // retained instance to the new transform. No-op unless WGR_GPU_DRIVEN is on.
    if (GEngine)
    {
        GEngine->SceneObjectMoved(obj);
    }
    if (&list == &move)
    {
        // recalc bsphere
        // this should happen only in buldozer
        if (obj->Static())
        {
            move->StaticChanged();
        }
        return;
    }
    Verify(move.Add(obj, xm, zm) >= 0); // <0  -> error
    for (int i = 0; i < list.Size(); i++)
    {
        if (list[i] == obj)
        {
            list.Delete(i);
            return;
        }
    }
#ifdef _MSC_VER
    const type_info& type = typeid(*obj);
    LOG_ERROR(World, "Moved object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), type.name(), obj->Position().X(), obj->Position().Z(), xl, zl);
#else
    LOG_ERROR(World, "Moved object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), typeid(*obj).name(), obj->Position().X(), obj->Position().Z(), xl, zl);
#endif
    // try to patch it: first search in near slots
    int xMin = xl - 1, xMax = xl + 1;
    int zMin = zl - 1, zMax = zl + 1;
    saturateMax(xMin, 0), saturateMin(xMax, _landRange - 1);
    saturateMax(zMin, 0), saturateMin(zMax, _landRange - 1);
    for (int zz = zMin; zz <= zMax; zz++)
    {
        for (int xx = xMin; xx <= xMax; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    list.Delete(i);
                    RptF("  found in (%d,%d).", xx, zz);
                    return;
                }
            }
        }
    }

    // try to patch it: search for the object in all slots
    for (int zz = 0; zz < _landRange; zz++)
    {
        for (int xx = 0; xx < _landRange; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    list.Delete(i);
                    RptF("  found in (%d,%d).", xx, zz);
                    return;
                }
            }
        }
    }
}

void Landscape::MoveObject(Object* obj, Vector3Par pos)
{
    int xl, zl;
    SelectObjectList(xl, zl, obj->Position().X(), obj->Position().Z());
    ObjectList& list = _objects(xl, zl);

    int xm, zm;
    SelectObjectList(xm, zm, pos.X(), pos.Z());
    ObjectList& move = _objects(xm, zm);
    // const type_info &type = typeid(*obj);
    float oldX = obj->Position().X();
    float oldZ = obj->Position().Z();

#ifdef LOG_OBJ_SHAPE
    if (IsShape(obj, LOG_OBJ_SHAPE))
    {
        LOG_DEBUG(World, "move pos obj {:x}:{} {:.2f},{:.2f} ({},{})->{:.2f},{:.2f} ({},{})", (uintptr_t)obj,
                  obj->GetShape()->Name(), obj->Position().X(), obj->Position().Z(), xl, zl, pos.X(), pos.Z(), xm, zm);
    }
#endif
    obj->SetPosition(pos);
    if (&list == &move)
    {
        // recalc bsphere
        // this should happen only in buldozer
        if (obj->Static())
        {
            move->StaticChanged();
        }
        return;
    }

    for (int i = 0; i < list.Size(); i++)
    {
        if (list[i] == obj)
        {
            Verify(move.Add(obj, xm, zm) >= 0); // <0  -> error
            list.Delete(i);
            return;
        }
    }

#ifdef _MSC_VER
    const type_info& type = typeid(*obj);
    LOG_DEBUG(World, "Moved object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), type.name(), oldX, oldZ, xl, zl);
#else
    LOG_DEBUG(World, "Moved object {:x}:{} ({}) not in landscape {:.2f},{:.2f} ({},{}).", (uintptr_t)obj,
              ShapeName(obj->GetShape()), typeid(*obj).name(), oldX, oldZ, xl, zl);
#endif
    // try to patch it: first search in near slots
    int xMin = xl - 1, xMax = xl + 1;
    int zMin = zl - 1, zMax = zl + 1;
    saturateMax(xMin, 0), saturateMin(xMax, _landRange - 1);
    saturateMax(zMin, 0), saturateMin(zMax, _landRange - 1);
    for (int zz = zMin; zz <= zMax; zz++)
    {
        for (int xx = xMin; xx <= xMax; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    Verify(move.Add(obj, xm, zm) >= 0); // <0  -> error
                    list.Delete(i);
                    LOG_DEBUG(World, "  found in ({},{}), moving to {:.2f},{:.2f} ({},{})", xx, zz, pos.X(), pos.Z(),
                              xm, zm);
                    return;
                }
            }
        }
    }

    // try to patch it: search for the object in all slots
    for (int zz = 0; zz < _landRange; zz++)
    {
        for (int xx = 0; xx < _landRange; xx++)
        {
            ObjectList& list = _objects(xx, zz);
            for (int i = 0; i < list.Size(); i++)
            {
                if (list[i] == obj)
                {
                    Verify(move.Add(obj, xm, zm) >= 0); // <0  -> error
                    list.Delete(i);
                    LOG_DEBUG(World, "  found in ({},{}), moving to {:.2f},{:.2f} ({},{})", xx, zz, pos.X(), pos.Z(),
                              xm, zm);
                    return;
                }
            }
        }
    }
}

void Landscape::InitObjectVehicles()
{
    // Two costs hide in this walk and only counting separates them: the number of
    // objects visited (cheap per item, enormous in total on a streamed world) and
    // the number whose GetShape() had to resolve. A bare millisecond figure cannot
    // tell a long cheap walk from a short expensive one.
    const DWORD walkStart = Poseidon::Foundation::GlobalTickCount();
    long long visited = 0;
    long long primaries = 0;
    long long withShape = 0;
    RefArray<Object> changed;
    for (int x = 0; x < _landRange; x++)
    {
        for (int z = 0; z < _landRange; z++)
        {
            ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                ++visited;
                if (!obj)
                {
                    continue;
                }
                if (obj->GetType() != Primary)
                {
                    continue;
                }
                ++primaries;
                if (!obj->GetShape())
                {
                    continue;
                }
                ++withShape;
                const char* className = obj->GetShape()->GetPropertyClass();
                if (className && *className)
                {
                    // PoseidonAssert( obj->RefCounter()==1 ); // only one reference to any object in this point
                    changed.Add(obj);
                }
            }
        }
    }
    const DWORD scanMs = Poseidon::Foundation::GlobalTickCount() - walkStart;
    const DWORD promoteStart = Poseidon::Foundation::GlobalTickCount();
    long long promoted = 0;
    for (int i = 0; i < changed.Size(); i++)
    {
        // add all vehicles to vehicle list
        Object* obj = changed[i];
        Vehicle* vehicle = dyn_cast<Vehicle>(obj);
        if (vehicle)
        {
            // vehicle->SetTransform(obj->Transform());
            RemoveObject(obj);
            // GWorld->AddSlowVehicle(vehicle);
            // GWorld->AddVehicle(vehicle);
            GWorld->AddBuilding(vehicle);
            ++promoted;
        }
    }
    const DWORD promoteMs = Poseidon::Foundation::GlobalTickCount() - promoteStart;

    // SECOND promotion, by SHAPE NAME rather than by `class` property: map
    // furniture that a `simulation="thing"` config class names becomes a Thing,
    // which is the only class Box3D reaches. See MapThings.hpp for what was
    // measured first -- on stock worlds the furniture is proxies inside
    // building models, not map objects, so the proxy walk is the one that finds
    // the chairs. Runs after the building walk so a Building's proxies are
    // visited through the same landscape cells its ObjectPlain was in.
    const DWORD thingStart = Poseidon::Foundation::GlobalTickCount();
    MapThings::Promotion things;
    const MapThings::Settings& mt = MapThings::Get();
    MapThings::ResetHiddenProxies();
    if (mt.enabled && GWorld)
    {
        const RString worldName = Glob.header.worldname;
        // Mission restart: CleanUp emptied `_vehicles`, and the ObjectPlains
        // those Things replaced left the landscape on the first run. Put the
        // recorded ones back before the walk, which will then find no candidates.
        MapThings::ReplayReplaced(worldName, things);

        RefArray<Object>            replace;    // ObjectPlain -> Thing
        RefArray<Object>            parents;    // objects with at least one qualifying proxy
        // RFG-095c: a parent is collected ONCE. The cell scan below and the RFG-080 walk over
        // `changed` (buildings the promotion just created) both reach the same Building, and a
        // large building is listed in several land cells besides; every duplicate spawned the
        // whole furniture set again -- 528 Things for 264 proxies at the Everon café, two
        // Things at exactly the same spot, one of which flew when shot while "a copy" stayed.
        std::unordered_set<const Object*> parentSeen;
        const auto addParent = [&](Object* parent)
        {
            if (parentSeen.insert(parent).second)
                parents.Add(parent);
        };
        std::unordered_set<const LODShape*> models;
        for (int x = 0; x < _landRange; x++)
        {
            for (int z = 0; z < _landRange; z++)
            {
                ObjectList& list = _objects(x, z);
                for (int i = 0; i < list.Size(); i++)
                {
                    Object* obj = list[i];
                    if (!obj || obj->GetType() != Primary)
                    {
                        continue;
                    }
                    LODShapeWithShadow* shape = obj->GetShape();
                    if (!shape || dyn_cast<Thing>(obj))
                    {
                        continue;
                    }
                    // Object level: only a plain object is replaced. A Building,
                    // Forest or Road that happens to share a model with a thing
                    // class keeps its own behaviour.
                    if (dynamic_cast<ObjectPlain*>(obj) && MapThings::ThingTypeForShape(shape))
                    {
                        replace.Add(obj);
                        models.insert(shape);
                        continue;
                    }
                    if (!mt.proxies || shape->NLevels() <= 0)
                    {
                        continue;
                    }
                    const Shape* lod0 = shape->LevelOpaque(0);
                    for (int p = 0; p < lod0->NProxies(); p++)
                    {
                        const ProxyObject& proxy = lod0->Proxy(p);
                        const LODShape*    pshape = proxy.obj ? proxy.obj->GetShape() : nullptr;
                        if (MapThings::ThingTypeForShape(pshape))
                        {
                            addParent(obj);
                            break;
                        }
                    }
                }
            }
        }
        // RFG-080: the buildings too. A house-class model was promoted to a Building
        // and REMOVED from the cell lists above, so the cell walk never sees it --
        // and the pub is exactly such a model. Measured at the café at [5093 3996]:
        // `hospoda_mesto.p3d` 7.8 m from the camera, thirteen chair proxies inside
        // it, and "things: 0 from proxies of 0 parents". `changed` still holds the
        // promoted objects, so they get the same proxy test here.
        if (mt.proxies)
        {
            for (int i = 0; i < changed.Size(); i++)
            {
                Object*             obj = changed[i];
                LODShapeWithShadow* shape = obj ? obj->GetShape() : nullptr;
                if (!shape || shape->NLevels() <= 0)
                    continue;
                const Shape* lod0 = shape->LevelOpaque(0);
                if (!lod0)
                    continue;
                for (int p = 0; p < lod0->NProxies(); p++)
                {
                    const ProxyObject& proxy = lod0->Proxy(p);
                    const LODShape*    pshape = proxy.obj ? proxy.obj->GetShape() : nullptr;
                    if (MapThings::ThingTypeForShape(pshape))
                    {
                        addParent(obj);
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < replace.Size(); i++)
        {
            Object*       obj = replace[i];
            EntityAIType* type = MapThings::ThingTypeForShape(obj->GetShape());
            const int     id = obj->ID();
            const Matrix4 transform = obj->Transform();
            if (things.first.GetLength() == 0)
            {
                things.first = obj->GetShape()->Name();
            }
            Thing* thing = MapThings::Spawn(type, transform, id);
            if (!thing)
            {
                ++things.failed;
                continue;
            }
            RemoveObject(obj);
            if (id >= 0)
            {
                _objectIds[id] = thing;
            }
            MapThings::RememberReplaced(worldName, type, transform, id);
            ++things.objectsReplaced;
        }
        for (int i = 0; i < parents.Size(); i++)
        {
            Object*             obj = parents[i];
            LODShapeWithShadow* shape = obj->GetShape();
            const Shape*        lod0 = shape->LevelOpaque(0);
            const Matrix4       parentTransform = obj->Transform();
            bool                any = false;
            for (int p = 0; p < lod0->NProxies(); p++)
            {
                const ProxyObject& proxy = lod0->Proxy(p);
                const LODShape*    pshape = proxy.obj ? proxy.obj->GetShape() : nullptr;
                EntityAIType*      type = MapThings::ThingTypeForShape(pshape);
                if (!type)
                {
                    continue;
                }
                if (things.first.GetLength() == 0)
                {
                    things.first = pshape->Name();
                }
                // Same composition Object::DrawProxies uses to place the proxy.
                if (!MapThings::Spawn(type, parentTransform * proxy.obj->Transform(), -1))
                {
                    ++things.failed;
                    continue;
                }
                models.insert(pshape);
                MapThings::HideProxy(shape, pshape);
                ++things.proxiesPromoted;
                any = true;
            }
            if (any)
            {
                ++things.parentsWithProxies;
                // RFG-095b: the building was registered with the retained renderer before its
                // furniture was hidden; let it re-emit its GPU proxies from the hidden set.
                if (GEngine)
                    GEngine->SceneObjectProxiesChanged(obj);
            }
        }
        things.distinctModels = (long long)models.size();
    }
    LOG_INFO(World,
             "InitObjectVehicles: scanned {} object slots in {}x{} land cells ({} primary, {} with a resolved "
             "shape, {} named a class) in {} ms; promoted {} to buildings in {} ms; things: {} from map objects "
             "(+{} replayed) and {} from proxies of {} parents, {} distinct models, {} refused, first: {} "
             "in {} ms [{}]",
             visited, _landRange, _landRange, primaries, withShape, (long long)changed.Size(), scanMs, promoted,
             promoteMs, things.objectsReplaced, things.objectsReplayed, things.proxiesPromoted,
             things.parentsWithProxies, things.distinctModels, things.failed,
             things.first.GetLength() ? (const char*)things.first : "-",
             Poseidon::Foundation::GlobalTickCount() - thingStart,
             !mt.enabled ? "map things OFF" : (mt.proxies ? "map things on, proxies on" : "map things on, proxies OFF"));
}

Object* Landscape::AddObject(Vector3Par pos, float head, LODShapeWithShadow* shape, void* user)
{
    // Fail("Old positioning used.");
    int id = NewObjectID();
    Object* obj = NewObject(shape, id);
    _objectIds[id] = obj;
    if (head)
    {
        obj->SetTransform(Matrix4(MRotationY, head * (H_PI / 180)));
    }
    Point3 nPos = pos + obj->DirectionModelToWorld(shape->BoundingCenter());
    obj->SetPosition(nPos);
    AddObject(obj);
    return obj;
}

namespace
{

// A world-marker light has no standalone visual sprite. Its model material is the emitter;
// this class supplies only physical illumination. Returning no flare prevents Scene::DrawFlares
// from turning a nearby runway marker back into a sun-sized white billboard.
class StaticMarkerLight final : public LightPoint
{
  public:
    StaticMarkerLight(ColorVal diffuse, ColorVal ambient) : LightPoint(diffuse, ambient) {}
    float FlareIntensity(Vector3Par, Vector3Par) const override { return 0.0f; }
};

bool ContainsNoCase(const char* text, const char* needle)
{
    if (!text || !needle || !*needle)
        return false;
    const size_t needleLen = std::strlen(needle);
    for (const char* at = text; *at; ++at)
    {
        size_t i = 0;
        for (; i < needleLen && at[i]; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(at[i])) !=
                std::tolower(static_cast<unsigned char>(needle[i])))
                break;
        }
        if (i == needleLen)
            return true;
    }
    return false;
}

bool IsA3StaticMarkerLightSource(const char* modelName)
{
    return modelName && ContainsNoCase(modelName, "a3\\");
}

// Imported A3 world objects do not instantiate CfgVehicles' MarkerLights yet. Their models
// nevertheless retain the authored light_*_pos memory points. Turn those into invisible point
// lights so the existing WGPU local-light path illuminates terrain and nearby geometry; the
// model's own emissive cap/flare remains the visible source. This is deliberately limited to
// static A3 placements and point names explicitly ending in a position-like token, so ordinary
// CWA objects and model helper points cannot gain unexpected lighting.
void AddA3StaticMarkerLights(Object* obj, LODShapeWithShadow* shape, const char* modelName,
                             RefArray<LightPoint>& ownedLights)
{
    if (!obj || !shape || !IsA3StaticMarkerLightSource(modelName))
        return;
    Shape* memory = shape->MemoryLevel();
    if (!memory || !GLOB_SCENE)
        return;

    const bool redBeacon = ContainsNoCase(modelName, "tower") || ContainsNoCase(modelName, "beacon");
    const Color diffuse = redBeacon ? Color(0.95f, 0.02f, 0.02f) : Color(1.0f, 0.80f, 0.52f);
    const Color ambient = redBeacon ? Color(0.01f, 0.0f, 0.0f) : Color(0.04f, 0.03f, 0.02f);
    const float brightness = redBeacon ? 0.20f : 0.25f; // start 10/12.5m, smooth tail to 100/125m
    int emitted = 0;
    for (int i = 0; i < memory->NNamedSel() && emitted < 8; ++i)
    {
        const char* pointName = memory->NamedSel(i).Name();
        if (!ContainsNoCase(pointName, "light") ||
            !(ContainsNoCase(pointName, "_pos") || ContainsNoCase(pointName, "position")))
            continue;
        if (!shape->MemoryPointExists(pointName))
            continue;

        Ref<LightPoint> light = new StaticMarkerLight(diffuse, ambient);
        light->SetPosition(obj->Transform().FastTransform(shape->MemoryPoint(pointName)));
        light->SetBrightness(brightness);
        GLOB_SCENE->AddLight(light);
        // Scene::AddLight deliberately stores a non-owning Link. Retain the
        // point light on Landscape so it survives past this creation call.
        ownedLights.Add(light);
        ++emitted;
    }
    if (emitted > 0 && std::getenv("WGR_STATIC_LIGHT_TRACE"))
        LOG_INFO(World, "A3 static marker lights: model={} emitted={}", modelName, emitted);
}

} // namespace

Streaming::InstanceCoverageObservation Landscape::ObserveConstructedObjectCoverage(
    int id, const Streaming::OwnerCoverageObservationScope& scope) const
{
    if (!scope.Active() || id < 0)
        return {};
    const auto found = _objectIds.find(id);
    if (found == _objectIds.end() || !found->second)
        return {};
    return scope.Observe(*static_cast<Object*>(found->second));
}

Streaming::InstanceCoverageObservation Landscape::ValidateConstructedObjectCoverage(
    const Streaming::InstanceCoverageObservation& observation,
    const Streaming::OwnerCoverageObservationScope& scope) const
{
    if (!scope.Active() || observation.ObjectId() < 0)
        return {};
    const auto found = _objectIds.find(observation.ObjectId());
    if (found == _objectIds.end() || !found->second)
        return {};
    return scope.Validate(*static_cast<Object*>(found->second), observation);
}

Object* Landscape::ObjectCreate(int id, const char* name, const Matrix4& transform, int* x, int* z,
                                bool avoidRecalculation, bool preserveAuthoredElevation, bool registerVisual)
{
    const ShapeBank::CpuOnlyLoadScope cpuOnly(!registerVisual);
    // create object based on Class hint in LOD 0
    Ref<LODShapeWithShadow> shape = Shapes.New(name, false, true);
    return ObjectCreateFromShape(id, shape, name, transform, x, z, avoidRecalculation,
                                 preserveAuthoredElevation, registerVisual);
}

Object* Landscape::ObjectCreateModernPlainFromShape(int id, LODShapeWithShadow* shape, const Matrix4& transform)
{
    if (!Foundation::IsMainThread() || !_modernObjectStreaming || id < 0 || !shape ||
        static_cast<const char*>(shape->GetPropertyClass())[0] != '\0' ||
        !Streaming::ValidSimulationFrame(transform) || !Streaming::SimulationShapeAdmissible(*shape))
        return nullptr;
    const auto existing = _objectIds.find(id);
    if (existing != _objectIds.end() && static_cast<Object*>(existing->second)) return nullptr;
    Ref<LODShapeWithShadow> keepShape = shape;
    return ObjectCreateFromShape(id, keepShape, shape->Name(), transform, nullptr, nullptr, false, true, false);
}

Object* Landscape::ObjectCreateFromShape(int id, LODShapeWithShadow* shape, const char* name,
                                       const Matrix4& transform, int* x, int* z, bool avoidRecalculation,
                                       bool preserveAuthoredElevation, bool registerVisual)
{
    const ShapeBank::CpuOnlyLoadScope cpuOnly(!registerVisual);
    Object* obj = NewObject(shape, id);
    obj->SetVisualResident(registerVisual);

    if (obj->GetType() != Primary && obj->GetType() != Network)
    {
        obj->SetType(Primary);
    }

    // object id list cleared after load is finished
    _objectIds[id] = obj;
    PoseidonAssert(obj->ID() == id);
    // check matrix type

    static RStringB forestString("forest");
    Matrix4 nTransform = RepairWorldObjectPlacementFrame(transform, shape->GetPropertyClass() == forestString,
                                                        obj->GetType() == Network,
                                                        (shape->GetOrHints() & ClipLandKeep) != 0);

    obj->SetTransform(nTransform);

#if 1

    // Legacy island transforms were historically normalised so a model's
    // bounding centre rests on the heightfield. OPRW25's Matrix4x3 is already
    // an authored A3 world transform; applying that legacy correction lowers
    // many buildings by the model-centre offset (often an entire storey).
    if (!preserveAuthoredElevation && !ENGINE_CONFIG.landEditor)
    {
        // auto-correct values
        Vector3 oPos = obj->Position();
        Vector3 pos = obj->PositionModelToWorld(-shape->BoundingCenter());

        float above = pos[1] - SurfaceY(pos[0], pos[2]);
        if (above > 0)
        {
            oPos[1] -= above;
            obj->SetPosition(oPos);
        }
    }
#endif

    AddObject(obj, x, z, avoidRecalculation, registerVisual);
    if (!registerVisual)
        return obj;
    if (_modernObjectStreaming)
    {
        // Rejected model families never produce marker lights. Avoid creating
        // and immediately erasing an empty owner node for every such admission.
        if (IsA3StaticMarkerLightSource(name))
        {
            RefArray<LightPoint>& lights = _modernObjectLightOwners[id];
            AddA3StaticMarkerLights(obj, shape, name, lights);
            if (lights.Size() == 0)
                _modernObjectLightOwners.erase(id);
        }
    }
    else
    {
        AddA3StaticMarkerLights(obj, shape, name, _staticMarkerLights);
    }
    return obj;
}

void Landscape::RegisterModernObjectVisual(Object* object)
{
    if (auto* shape = object->GetShape())
    {
        ShapeBank::WorldModelScope worldModel(Shapes);
        Shapes.EnsureVisualRendering(shape, true);
    }
    object->SetVisualResident(true);
    object->PrepareDestroyedVisualRendering();
    if (GEngine)
        GEngine->SceneObjectCreated(object);
    auto* shape = object->GetShape();
    if (shape && IsA3StaticMarkerLightSource(shape->Name()))
    {
        auto& lights = _modernObjectLightOwners[object->ID()];
        AddA3StaticMarkerLights(object, shape, shape->Name(), lights);
        if (lights.Size() == 0)
            _modernObjectLightOwners.erase(object->ID());
    }
}

void Landscape::ObjectTypeChange(int id, const char* name)
{
    Object* obj = GetObject(id);
    Matrix4Val transform = obj->Transform();
    RemoveObject(obj); // remove object from the old position
    // obj no longer valid
    LODShapeWithShadow* shape = Shapes.New(name, false, true);
    obj = new ObjectPlain(shape, id);
    obj->SetTransform(transform);
    AddObject(obj); // insert on new position
}

class TempVehicle : public Vehicle
{
  public:
    TempVehicle(LODShapeWithShadow* shape) : Vehicle(shape, VehicleTypes.New("temp"), -1) {}
    void Simulate(float deltaT, SimulationImportance prec) override;
};

class SelArrow : public TempVehicle
{
  public:
    SelArrow();
};

SelArrow::SelArrow() : TempVehicle(Shapes.New("data3d\\force.p3d", false, true)) {}

void TempVehicle::Simulate(float deltaT, SimulationImportance prec)
{
    _delete = true; // delete immediatelly
}

void Landscape::ShowArrow(Vector3Par pos)
{
    // create a temporary object for drawing
    // use object min-max box to determine arrow position
    SelArrow* sel = new SelArrow();
    LODShape* arrow = sel->GetShape();
    // float topLevelShape=shape->Max().Y();
    //  arrow will be rotated, maxZ will become minY
    float bottomLevelArrow = arrow->Max().Z();
    sel->SetOrientation(Matrix3(MRotationX, H_PI / 2));
    sel->SetPosition(pos + Vector3(0, bottomLevelArrow, 0));
    _arrows.Add(sel);
}

void Landscape::ShowObject(Object* obj)
{
    // create a temporary object for drawing
    _arrows.Add(obj);
}

void Landscape::SetSelection(const AutoArray<int>& sel)
{
    GameState* gstate = GWorld->GetGameState();
    GameValue list = gstate->CreateGameValue(GameArray);
    GameArrayType& array = list;
    array.Realloc(sel.Size());

    for (int index = 0; index < sel.Size(); index++)
    {
        int id = sel[index];
        Object* obj = GetObject(id);
        // create a temporary object for drawing
        // use object min-max box to determine arrow position
        if (!obj)
        {
            continue;
        }
        LODShape* shape = obj->GetShape();
        SelArrow* sel = new SelArrow();
        LODShape* arrow = sel->GetShape();
        float topLevelShape = shape->Max().Y();
        // arrow will be rotated, maxZ will become minY
        float bottomLevelArrow = arrow->Max().Z();
        sel->SetOrientation(Matrix3(MRotationX, H_PI / 2));
        sel->SetPosition(obj->Position() + Vector3(0, topLevelShape + bottomLevelArrow, 0));
        _arrows.Add(sel);
        // GWorld->AddVehicle(sel);

        GameValue CreateGameObject(Object * obj);
        array.Add(CreateGameObject(obj));
    }

    gstate->VarSet("bis_buldozer_selection", list, true);
}

void Landscape::SetSelRectangle(Vector3Par min, Vector3Par max) {}

bool Landscape::Magnetize(bool points, bool planes, bool lockY, const AutoArray<int>& sel)
{
    return false;
}

void Landscape::ObjectMove(int id, const Matrix4& transform)
{
    Ref<Object> obj = GetObject(id);
    RemoveObject(obj); // remove object from the old position
    obj->SetTransform(transform);
    AddObject(obj); // insert on new position
}

void Landscape::ObjectDestroy(int id)
{
    Object* obj = GetObject(id);
    PoseidonAssert(obj);
    if (obj)
    {
        RemoveObject(obj); // remove object from the old position
    }
}

DEFINE_FAST_ALLOCATOR(ObjectListFull)

int ObjectListFull::CountNonStatic() const
{
    int ret = 0;
    int n = Size();
    for (int i = 0; i < n; i++)
    {
        Object* obj = Get(i);
        if (obj->Static())
        {
            continue;
        }
        ret++;
    }
    return ret;
}

int ObjectListFull::CountTypeVehicle() const
{
    int ret = 0;
    const int n = Size();
    for (int i = 0; i < n; i++)
    {
        if (Get(i)->GetType() == TypeVehicle)
        {
            ret++;
        }
    }
    return ret;
}

void ObjectListFull::ChangeTypeVehicleCount(int val)
{
    _nTypeVehicle += val;
    // Same debug-only self-check as the non-static counter below, and for the same
    // reason: the invariant this leans on is "an object's type does not change while
    // it is in a cell". That holds today -- the one site that would break it,
    // `World::MoveOutAndDelete`'s convert-to-object branch, now removes and re-adds
    // around the change -- and this assert is what says so on every debug run.
    PoseidonAssert(CountTypeVehicle() == _nTypeVehicle);
    PoseidonAssert(_typeVehicles.Size() == _nTypeVehicle);
}

void ObjectListFull::ChangeNonStaticCount(int val)
{
    _nNonStatic += val;
    // Verifying the counter costs a full O(n) rescan of the cell on every add and every
    // delete. It is a self-check on the line above, so it belongs in debug builds only -
    // PoseidonAssert does not evaluate its argument once NDEBUG is defined.
    PoseidonAssert(CountNonStatic() == _nNonStatic);
}

void ObjectListFull::StaticChanged() // recalculate what is neccessary
{
    // recalculate bounding sphere
    _bRadius = 50;
    for (int i = 0; i < Size(); i++)
    {
        Object* obj = Get(i);
        if (!obj->Static())
        {
            continue;
        }
        float dist = obj->Position().Distance(_bCenter) + obj->GetRadius();
        saturateMax(_bRadius, dist);
    }
    // assume center is not changed
}

// One static object's contribution to the bounding radius - the term StaticChanged()
// folds in per object.
static inline float StaticBSphereTerm(const Object* obj, Vector3Par center)
{
    return obj->Position().Distance(center) + obj->GetRadius();
}

void ObjectListFull::SetBSphere(Vector3Par center, float radius)
{
    _bCenter = center;
    _bRadius = radius;
}

// default values for landscape grid x,z
void ObjectListFull::SetBSphere(int x, int z)
{
    _bCenter.Init();
    _bCenter[0] = x * LandGrid + LandGrid * 0.5;
    _bCenter[2] = z * LandGrid + LandGrid * 0.5;
    if (GLandscape)
    {
        _bCenter[1] = GLandscape->SurfaceY(_bCenter[0], _bCenter[2]);
    }
    else
    {
        _bCenter[1] = 50; // some estimation
    }
    _bRadius = 100; // default value
}

int ObjectListFull::Add(Object* object, bool avoidRecalculation)
{
    int index = base::Add(object);
    if (object->GetType() != TypeTempVehicle) _visibilityObjects.Add(object);
    LODShape* shape = object->GetShape();
    if (shape && shape->FindRoadwayLevel() >= 0)
    {
        _roadwayObjects.Add(object);
    }
    if (object->Static())
    {
        if (!avoidRecalculation)
        {
            // A bounding sphere only ever grows on insertion, so the full O(n) rescan
            // StaticChanged() does here is redundant: it would return exactly
            // max(_bRadius, term(new object)). Folding the one new object in makes
            // filling a cell O(k) instead of O(k^2).
            //
            // The first static object restarts from the 50 m floor, exactly as
            // StaticChanged() does, so an empty cell's 100 m default (SetBSphere(x,z))
            // is not carried over and streaming a cell in reproduces the old radius
            // bit for bit. Beyond that first object the invariant maintained is
            // "_bRadius >= the value StaticChanged() would compute", which is the safe
            // direction: _bRadius is only read as a conservative culling sphere
            // (World.cpp: Camera::IsClipped / MayBeClipped).
            if (Size() - _nNonStatic <= 1)
            {
                _bRadius = 50;
            }
            float dist = StaticBSphereTerm(object, _bCenter);
            saturateMax(_bRadius, dist);
        }
    }
    else
    {
        ChangeNonStaticCount(+1);
    }
    if (object->GetType() == TypeVehicle)
    {
        _typeVehicles.Add(object);
        ChangeTypeVehicleCount(+1);
    }
    return index;
}

void ObjectListFull::Delete(int index)
{
    Object* obj = Get(index);
    bool isStatic = obj->Static();
    const bool isVehicle = obj->GetType() == TypeVehicle;
    const bool isRoadway = obj->GetShape() && obj->GetShape()->FindRoadwayLevel() >= 0;
    // Removing a static object can only shrink the sphere, and only if that object is
    // the one holding the current bound up. Anything strictly inside leaves
    // max(50, max term) untouched, so the rescan can be skipped outright.
    float dist = isStatic ? StaticBSphereTerm(obj, _bCenter) : 0.0f;
    if (isVehicle)
    {
        int vehicleIndex = -1;
        for (int i = 0; i < _typeVehicles.Size(); ++i)
        {
            if (_typeVehicles[i] == obj)
            {
                vehicleIndex = i;
                break;
            }
        }
        PoseidonAssert(vehicleIndex >= 0);
        if (vehicleIndex >= 0)
        {
            _typeVehicles.Delete(vehicleIndex);
        }
    }
    if (isRoadway)
    {
        int roadwayIndex = -1;
        for (int i = 0; i < _roadwayObjects.Size(); ++i)
        {
            if (_roadwayObjects[i] == obj)
            {
                roadwayIndex = i;
                break;
            }
        }
        PoseidonAssert(roadwayIndex >= 0);
        if (roadwayIndex >= 0)
        {
            _roadwayObjects.Delete(roadwayIndex);
        }
    }
    if (obj->GetType() != TypeTempVehicle)
    {
        for (int i = 0; i < _visibilityObjects.Size(); ++i)
        {
            if (_visibilityObjects[i] == obj) { _visibilityObjects.Delete(i); break; }
        }
    }
    // Release the owning cell reference only after its non-owning mirrors no
    // longer contain the pointer. A static map object can be owned solely by
    // the landscape index at teardown.
    base::Delete(index);
    if (isVehicle)
    {
        ChangeTypeVehicleCount(-1);
    }
    if (isStatic)
    {
        if (dist >= _bRadius)
        {
            StaticChanged();
        }
    }
    else
    {
        ChangeNonStaticCount(-1);
    }
}

void ObjectListFull::Clear()
{
    _visibilityObjects.Clear();
    base::Clear();
    _typeVehicles.Clear();
    _roadwayObjects.Clear();
    _nNonStatic = 0;
    _nTypeVehicle = 0;
    // Same rescan as ChangeNonStaticCount - debug-only self-check. (After base::Clear()
    // the list is empty, so the scan can only ever agree.)
    PoseidonAssert(CountNonStatic() == _nNonStatic);
}

ObjectListFull::ObjectListFull(int x, int z) : _bRadius(1e10), _nNonStatic(0), _nTypeVehicle(0)
{
    SetBSphere(x, z);
}

ObjectListFull::~ObjectListFull()
{
    Clear();
}

Object* Landscape::AddObject(float x, float y, float z, float head, const char* name)
{
    LODShapeWithShadow* shape = Shapes.New(name, false, true);
    return AddObject(Vector3(x, y, z), head, shape, nullptr);
}

Object* Landscape::NearestObject(Vector3Par pos, float limit, ObjectType type, Object* ignore)
{
    // limit==0 means no limit, search whole landscape
    int x, z;
    int xMin, xMax, zMin, zMax;
    bool defLimit = false;
    if (limit < 0)
    {
        defLimit = true, limit = 200;
    }
    if (limit == 0)
    {
        xMin = 0, xMax = _landRangeMask;
        zMin = 0, zMax = _landRangeMask;
        limit = 1e10;
    }
    else
    {
        xMin = toIntFloor((pos.X() - limit) * _invLandGrid);
        xMax = toIntCeil((pos.X() + limit) * _invLandGrid);
        zMin = toIntFloor((pos.Z() - limit) * _invLandGrid);
        zMax = toIntCeil((pos.Z() + limit) * _invLandGrid);
        if (xMin < 0)
        {
            xMin = 0;
        }
        if (xMin > _landRangeMask)
        {
            xMin = _landRangeMask;
        }
        if (xMax < 0)
        {
            xMax = 0;
        }
        if (xMax > _landRangeMask)
        {
            xMax = _landRangeMask;
        }
        if (zMin < 0)
        {
            zMin = 0;
        }
        if (zMin > _landRangeMask)
        {
            zMin = _landRangeMask;
        }
        if (zMax < 0)
        {
            xMax = 0;
        }
        if (zMax > _landRangeMask)
        {
            zMax = _landRangeMask;
        }
    }
    Object* ret = nullptr;
    double minDist2 = limit * limit;
    for (z = zMin; z <= zMax; z++)
    {
        for (x = xMin; x <= xMax; x++)
        {
            ObjectList& list = _objects(x, z);
            int n = list.Size();
            for (int i = 0; i < n; i++)
            {
                Object* obj = list[i];
                if (!obj || !(obj->GetType() & type))
                {
                    continue;
                }
                double dist2 = obj->Position().Distance2(pos);
                // default behaviour: consider only objects
                // that include pos in their bounding sphere
                if (defLimit && dist2 > Square(obj->GetRadius()))
                {
                    continue;
                }
                if (obj == ignore)
                {
                    continue;
                }
                if (minDist2 > dist2)
                {
                    minDist2 = dist2, ret = obj;
                }
            }
        }
    }
    return ret;
}

} // namespace Poseidon

static LinkArray<LODShapeWithShadow> VBShapes;

void RegisterVBShape(LODShapeWithShadow* shape)
{
    int free = VBShapes.Find(nullptr);
    if (free >= 0)
    {
        VBShapes[free] = shape;
        return;
    }
    VBShapes.Add(shape);
}
void UnregisterVBShape(LODShapeWithShadow* shape)
{
    VBShapes.Delete(shape);
}

void ReleaseVBuffers()
{
    Shapes.ReleaseAllVBuffers();
    ::GLandscape->ReleaseAllVBuffers();
    // note: dynamic shapes (on some vehicles) may have vertex buffers assigned to them
    // such shapes should be registered someplace
    for (int i = 0; i < VBShapes.Size(); i++)
    {
        LODShape* lShape = VBShapes[i];
        if (!lShape)
        {
            continue;
        }
        for (int l = 0; l < lShape->NLevels(); l++)
        {
            Shape* shape = lShape->Level(l);
            shape->ReleaseVBuffer();
        }
    }
}

void RestoreVBuffers()
{
    Shapes.OptimizeAll();
    ::GLandscape->CreateAllVBuffers();
    for (int i = 0; i < VBShapes.Size(); i++)
    {
        LODShape* lShape = VBShapes[i];
        if (!lShape)
        {
            continue;
        }
        for (int l = 0; l < lShape->NLevels(); l++)
        {
            Shape* shape = lShape->Level(l);
            shape->ConvertToVBuffer(VBSmallDiscardable);
        }
    }
}

namespace Poseidon
{

void Landscape::SetSound(int x, int z, int index)
{
    if (this_InRange(x, z))
    {
        _soundMap(x, z) = index + 1;
    }
}

int Landscape::GetSound(int x, int z) const
{
    return this_InRange(x, z) ? int(_soundMap(x, z)) - 1 : 0;
}

} // namespace Poseidon
namespace Poseidon::Foundation
{
template class Link<TexMaterial>;
} // namespace Poseidon::Foundation
