#include <cstdlib>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <SDL3/SDL_scancode.h>
#include <Random/randomGen.hpp>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Poseidon::Foundation::IsOutOfMemory;
using Poseidon::Foundation::MStorage;
using Poseidon::Foundation::Time;
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/MapTypes.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Scene/SurfaceDrawOrder.hpp>
#include <Poseidon/World/Scene/AlphaSortOrder.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/ClipVert.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>
#include <Poseidon/World/Simulation/FrameInv.hpp>
#include <Poseidon/AI/AI.hpp> // remove dependency
#include <Poseidon/World/World.hpp>
#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <time.h>
#include <Poseidon/Dev/Diag/DiagModes.hpp>

using namespace Poseidon;
namespace Poseidon
{
RString GetUserParams();
}

namespace Poseidon
{
Scene* GScene;
} // namespace Poseidon

#define PASS_VEC(x) x.X(), x.Y(), x.Z()

typedef float FogF(float dist, float start, float end);

#define FogLinear ((FogF*)nullptr)

float FogQuadratic(float dist, float start, float end)
{
    return Square(dist - start) / Square(end - start);
}

#define LN_20 2.9957322736f

float FogExponential(float dist, float start, float end)
{
    // scale so that at start fog is 0
    return 1 - exp(LN_20 * (start - dist) / end);
}

#define FOG_FUNCTION FogExponential

static void AdaptVolumeLight(LODShape* lodShape)
{
    lodShape->OrSpecial(NoShadow | IsAlpha | IsLight | NoZWrite | ClampU | ClampV | IsAlphaFog | IsColored);
}

namespace Poseidon::Foundation
{
template class Ref<RoadType>;
} // namespace Poseidon::Foundation

#if DENSITY_LOD
#define _drawDensity _lodInvWidth
#endif

Scene::Scene()
    : _skyColor(HWhite), _mainLight(nullptr), _camera(new Camera), _tacticalVisibility(TACTICAL_VISIBILITY),
      _rainRange(900), _constantFog(0), _constantColor(HWhite), _objectShadows(false), _vehicleShadows(true),
      _cloudlets(true), _preferredTerrainGrid(ENGINE_CONFIG.enableHWTL ? 12.5 : 25),
      _preferredViewDistance(GetSelectedPreferredViewDistance())
{
    static StaticStorage<ActiveLightPointer> aLightsS;
    _aLights.SetStorage(aLightsS.Init(64));

    LoadConfig();

    _lodInvWidth = (_minLodInvWidth + _maxLodInvWidth) * 0.5;

#if _PIII
    PoseidonAssert(((int)this & 0xf) == 0);
#endif
    ResetFog();
}

static void UpdateFogRange(float& minRange, float& maxRange, float tacVis)
{
    saturateMin(maxRange, tacVis);
    saturateMin(minRange, maxRange * 0.4f);
}

void Scene::ResetFog()
{
    float minRange, maxRange;
    // shadow fog
    minRange = MAX_SHADOWFOG * 0.3f, maxRange = MAX_SHADOWFOG;
    UpdateFogRange(minRange, maxRange, _rainRange);
    _shadowFog.Set(minRange, maxRange, FOG_FUNCTION);
    _shadowFogMinRange = minRange, _shadowFogMaxRange = maxRange;
    // display fog
    // set back clipping to fog range
    ENGINE_CONFIG.horizontZ = floatMin(_rainRange + 20, ENGINE_CONFIG.tacticalZ);
    minRange = MAX_FOG * 0.3f, maxRange = MAX_FOG;
    UpdateFogRange(minRange, maxRange, _rainRange);

    // FAR-001: the DISPLAY fog range, and only that one, may be widened by the
    // altitude policy. Computed into separate locals rather than by mutating
    // `minRange`/`maxRange` on purpose -- `_tacticalFog` below is built from
    // `minRange`, and tactical fog is a gameplay-visibility quantity, not a
    // shading one. Widening it because the camera is in an aircraft would change
    // what the game considers visible, which is exactly the ground-play spillover
    // this whole change exists to avoid.
    float dispMin = minRange, dispMax = maxRange;
    // WGPU disables distance haze when the weather fog setting is zero. In
    // that mode rain/night's tactical visibility must not remove terrain:
    // heavy rain otherwise leaves a ~350m terrain rectangle with unfogged
    // objects visible beyond it. Keep the selected display reach, while the
    // legacy min/max, horizontZ, shadow fog and tactical fog stay unchanged.
    const char* backend = GraphicsEngineFactory::ActiveBackendCode();
    if (backend && std::strcmp(backend, "wgpu") == 0 && _landscape && _landscape->GetFog() <= 0.0f)
    {
        dispMax = std::max(dispMax, ENGINE_CONFIG.tacticalZ - 20.0f);
        dispMin = dispMax * 0.3f;
    }
    _baseFogMaxRange = dispMax;
    if (_aerialFogMaxRange > dispMax)
    {
        dispMax = _aerialFogMaxRange;
        dispMin = dispMax * 0.3f;
    }
    _fog.Set(dispMin, dispMax, FOG_FUNCTION);
    _fogMinRange = dispMin, _fogMaxRange = dispMax;
    // sky fog is constant
    _skyFog.Set(MinSkyFog, MaxSkyFog, FOG_FUNCTION);
    _tacticalFog.Set(minRange, _tacticalVisibility, FOG_FUNCTION);
}

void Scene::SetAerialFogMaxRange(float range)
{
    if (range < 0.0f)
    {
        range = 0.0f;
    }
    // A metre of hysteresis. Without it every frame of a climb rebuilds four
    // 256-entry fog tables for a change too small to see; with it, level flight
    // and all of ground play rebuild nothing at all.
    if (fabs(range - _aerialFogMaxRange) < 1.0f)
    {
        return;
    }
    _aerialFogMaxRange = range;
    ResetFog();
}

void Scene::SetTacticalVisibility(float tv, float rainRange)
{
    _tacticalVisibility = tv;
    if (fabs(rainRange - _rainRange) < 3)
    {
        return;
    }
    _rainRange = rainRange;
    ResetFog();
}

void Scene::Init(Engine* engine, Landscape* landscape)
{
    if (_landscape != landscape)
    {
        _landscape = landscape;
    }
    AbstractTextBank* bank = GEngine->TextBank();
    if (_landscape)
    {
        _landscape->SetOvercast(0.0);

        _skyTexture = _landscape->SkyTexture();
    }
    else
    {
        _skyTexture = bank->Load("data\\zatazeno.pac");
    }

    CalculateSkyColor(_skyTexture);
}

void Scene::CalculateSkyColor(Texture* texture)
{
    if (!texture)
    {
        texture = _skyTexture;
    }
    else
    {
        _skyTexture = texture;
    }
    if (_skyTexture)
    {
        // guarantee that the mipmap is loaded
        MipInfo mip = GEngine->TextBank()->UseMipmap(_skyTexture, 0, 0);
        PoseidonAssert(mip.IsOK());
        if (mip.IsOK())
        {
            _skyColor = _skyTexture->GetPixel(0, 1, 1);
        }
    }
}

void Scene::CleanUp()
{
    for (int i = 0; i < _drawObjects.Size(); i++)
    {
        _drawObjects[i]->object->SetInList(nullptr);
    }
    _drawObjects.Resize(0);
    _drawMergers.Resize(0);
}

Scene::~Scene()
{
    SaveConfig();
    ResetLights();
    _aLights.Clear();
    if (_mainLight)
    {
        delete _mainLight, _mainLight = nullptr;
    }
    if (_camera)
    {
        delete _camera, _camera = nullptr;
    }
    _skyTexture = nullptr;
    _landscape.Free();
    GLandscape = nullptr;
}

void Scene::SetCamera(const Camera& camera)
{
    if (_camera)
    {
        *_camera = camera;
    }
    else
    {
        _camera = new Camera(camera);
    }
}

FogFunction::FogFunction()
{
    _start2 = 0;
    _end2 = 1;
    _divisor = (LEN_FOG_TABLE - 1) / (_end2 - _start2);
}

void FogFunction::Set(float start, float end, float (*function)(float distRel, float start, float end))
{
    _start2 = start * start;
    _end2 = end * end;
    _divisor = (LEN_FOG_TABLE - 1) / (_end2 - _start2);
    float invEmS = 1.0f / (end - start);
    for (int i = 0; i < LEN_FOG_TABLE; i++)
    {
        // index into the table is i, corresponding relative distance is
        float indexRel = i * (1.0f / (LEN_FOG_TABLE - 1));
        // indexRel corresponds to (dist*dist-_start*_start)/(_end*_end-_start*_start)
        // we would like to calculate (dist-_start)/(_end-_start)
        float dist = sqrt(indexRel * (_end2 - _start2) + _start2);
        float fogLF = (dist - start) * invEmS;
        float fogF = function ? function(dist, start, end) : fogLF;
        int fog = toInt(fogF * 256);
        if (fog < 0)
        {
            fog = 0;
        }
        else if (fog > 255)
        {
            fog = 255;
        }
        _fog[i] = fog;
    }
    _fog[LEN_FOG_TABLE - 1] = 0xff; // full fog
}

int FogFunction::operator()(float distSquare) const
{
    // calculate index between 0 and 1
    float indexFlt = (distSquare - _start2) * _divisor;
    int index = toIntFloor(indexFlt);
    if (index <= 0)
    {
        return _fog[0];
    }
    if (index < LEN_FOG_TABLE)
    {
        return _fog[index];
    }
    return _fog[LEN_FOG_TABLE - 1];
}

void Scene::AddLight(Light* light)
{
    int i;
    // if possible insert the light into some empty slot
    for (i = 0; i < _lights.Size(); i++)
    {
        if (!_lights[i])
        {
            _lights[i] = light;
            return;
        }
    }
    _lights.Add(light);
}

void Scene::ResetLights()
{
    _lights.Clear();
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

static int CmpALight(const ActiveLightPointer* l0, const ActiveLightPointer* l1)
{
    const Light* light0 = *l0;
    const Light* light1 = *l1;
    PoseidonAssert(light0);
    PoseidonAssert(light1);
    PoseidonAssert(light0 != light1);
    return light1->Compare(*light0);
}

const LightList& Scene::SelectLights(Vector3Par pos, float radius, LightList& work)
{
    // select only the nearest lights to make lighting faster
    int i;
    for (i = 0; i < _aLights.Size(); i++)
    {
        // select only light which can add something to object lighting
        Light* light = _aLights[i];
        float dist2 = light->SquareDistance(pos);
        if (Square(radius) > dist2 * Square(0.1))
        {
            float dist = dist2 * InvSqrt(dist2) - radius;
            saturateMax(dist, 0);
            if (light->Brightness() > 0.02 * Square(dist))
            {
                work.Add(light);
            }
        }
        else
        {
            if (light->Brightness() > 0.02 * dist2)
            {
                work.Add(light);
            }
        }
    }
    // try to optimize lights
    if (work.Size() > 1)
    {
        LightContext context;
        context.position = pos;
        QSort(work.Data(), work.Size(), &context, CmpALight);
        const int maxLightsPerObject = 7;
        if (work.Size() > maxLightsPerObject)
        {
            work.Resize(maxLightsPerObject);
        }
    }
    return work;
}

#define DIAG_LIGHT_SELECT

const LightList& Scene::SelectLights(Matrix4Par objPos, const Object* object, int level, LightList& work)
{
    work.Clear();
    // no lights used - full day light
    const int spec = object->GetSpecial();
    const bool sunDisabled = render::Has(render::SplitLegacy(spec).material, render::Material::DisableSun);
    if (sunDisabled)
    {
        return _aLights;
    }

    if (!sunDisabled && MainLight()->NightEffect() < 0.01)
    {
        return work;
    }
    // may return work or something else
    LODShape* lShape = object->GetShape();
    Shape* oShape = lShape->LevelOpaque(level);

    // no light selection for sky, clouds ... etc.
    if (spec & IsShadow)
    {
        return work;
    }
    ClipFlags globalLight = oShape->GetAndHints() & ClipLightMask;
    if (globalLight != (oShape->GetOrHints() & ClipLightMask))
    {
        globalLight = 0;
    }
    switch (globalLight)
    {
        case ClipLightCloud:
        case ClipLightSky:
        case ClipLightStars:
            return work;
    }

    // select only the nearest lights to make lighting faster
    return SelectLights(objPos.Position(), lShape->BoundingSphere(), work);
}

static StaticStorage<ActiveLightPointer> LightStorage;

LightList::LightList(bool staticStorage)
{
    if (staticStorage)
    {
        SetStorage(LightStorage.Init(64));
    }
}

LightList::LightList(const LightList& src)
{
    SetStorage(LightStorage.Init(64));
    Realloc(src.Size());
    Resize(src.Size());
    // note: we assume LightList does not need any destruction
    memcpy(Data(), src.Data(), src.Size() * sizeof(ActiveLightPointer));
}

void Scene::SetActiveLights(const LightList& lights)
{
    _aLights.Resize(0);
    // copy all lights from the list
    for (int i = 0; i < lights.Size(); i++)
    {
        Light* light = lights[i];
        _aLights.Add(light);
    }
}

void Scene::SelectActiveLights(Object* dimmed)
{
    _aLights.Resize(0);

    // enumerate all light objects for drawing
    Vector3Val camPos = GetCamera()->Position();
    for (int i = 0; i < _lights.Size(); i++)
    {
        Light* light = _lights[i];
        if (light && light->IsOn())
        {
            // check if light can be ignored (very far)
            if (light->Position().Distance2(camPos) > Square(ENGINE_CONFIG.horizontZ + 500))
            {
                continue;
            }

            bool invisible = false;
            Object* attach = light->AttachedOn();
            if (attach && attach == dimmed)
            {
                invisible = true;
            }
            light->ToDraw(ClipAll, invisible);
            _aLights.Add(light);
        }
    }

    // we need to know the strongest light source
    QSort(_aLights.Data(), _aLights.Size(), CmpALight);
    if (_aLights.Size() > ENGINE_CONFIG.maxLights)
    {
        // use only strongest lights
        _aLights.Resize(ENGINE_CONFIG.maxLights);
    }
}

void Scene::MainLightChanged()
{
    if (GetLandscape())
    {
        Color sunColor = _mainLight->GetColorFull() * (GetLandscape()->SkyThrough() * 0.8f + 0.2f);
        sunColor.SaturateMinMax();
        _mainLight->SetDiffuse(sunColor);
    }
    // recalculate background color
    // calculate fog color from sky texture
    // we should apply some lighting (to have dark fog in the night)
    Color lighting = _mainLight->SkyColor();
    lighting = lighting * GEngine->GetAccomodateEye();
    lighting.SaturateMinMax();
    Color color = _skyColor * lighting;
    color.SaturateMinMax();
    color.SetA(1);
    GEngine->SetFogColor(color);
}

const Matrix4& Scene::ScaledInvTransform() const
{
    return _camera->_scaledInvTransform;
}
const Matrix3& Scene::CamNormalTrans() const
{
    return _camera->_camNormalTrans;
}
const Matrix4& Scene::CamInvTrans() const
{
    return _camera->_camInvTrans;
}

float Scene::GetMinimalTerrainGrid() const
{
    // All terrain grid options are available; modern hardware handles max
    // settings (VD=5000, TG=3.125) at 333+ FPS.
    return 3.125;
}

void Scene::SetPreferredTerrainGrid(float x)
{
    saturate(x, 0.5, 100);
    _preferredTerrainGrid = x;
    if (GWorld && GWorld->GetMode() != GModeNetware)
    {
        GWorld->AdjustSubdivision(GWorld->GetMode());
    }
}
void Scene::SetPreferredViewDistance(float x)
{
    float cliVD = AppConfig::Instance().GetViewDistanceOverride();
    if (cliVD > 0)
        x = cliVD;
    else
        saturate(x, GameSettingsConfig::kMinViewDistance, GameSettingsConfig::kMaxViewDistance);
    _preferredViewDistance = x;

    if (GWorld && GWorld->GetMode() != GModeNetware)
    {
        GWorld->AdjustSubdivision(GWorld->GetMode());
    }
}

void Scene::SetObjectShadows(bool set)
{
    _objectShadows = set;
}

void Scene::SetVehicleShadows(bool set)
{
    _vehicleShadows = set;
}

void Scene::SetCloudlets(bool set)
{
    _cloudlets = set;
}

void Scene::SetObjectLODBias(float bias)
{
    if (bias < 0.25f)
        bias = 0.25f;
    if (bias > 4.0f)
        bias = 4.0f;
    _objectLODBias = bias;
    // Driven by the new Graphics screen's Object LOD tier row.  Read by
    // the LOD selection path on the next render frame; no extra Reset
    // needed — the bias multiplies the projected screen size before
    // LODSelect compares against the per-LOD pixel-size thresholds.
}

// How far AdjustComplexity is allowed to coarsen detail before it stops, as a
// multiple of the quality floor.
//
// This governs `_lodInvWidth`, which multiplies distance in BOTH the LOD selector
// and the GPU sub-pixel cull (`cull.wgsl`: an object is dropped when its projected
// diameter falls under `pixel_limit * lod_scale * dist * _lodInvWidth`). So the
// range is not a detail knob — it is the range over which an object's *visibility
// distance* moves. The legacy value was 16, which collapses a 4 m bush's horizon
// from ~2.7 km to ~170 m, i.e. to a distance with no fog to hide the transition.
// That is the pop-in.
//
// It is also a feedback loop keyed on WALL-CLOCK frame time while it can only
// control GPU geometry. On the modern imported worlds the frame is ~90% CPU, so
// the governor pins itself to the coarse rail chasing GPU time that was never the
// constraint, and the asymmetric damper in AdjustComplexity makes recovery ~10x
// harder than degradation.
//
// Default 4 bounds that horizon swing to 4x (that same bush: ~680 m, where fog is doing
// real work).
//
// NEITHER 16 NOR 4 SELECTS LOD CORRECTLY, AND THAT IS THE REAL BUG HERE. Measured on
// Everon at a NEAR pose (camera 38 m up), same build, same frame, ~47,800 instances in both:
//
//   range  LOD0   LOD1    LOD2    LOD3    LOD4   triangles   GPU
//     16   9,186 15,631  23,118       0       0     44.2M   28.2 ms
//      4     697  1,689   5,153  18,875  18,245      6.4M   18.1 ms
//
// At 16 NOTHING is ever coarser than LOD 2, even at multi-kilometre range -- that is the
// 44M-triangle waste. At 4 the mass sits at LOD 3-4, INCLUDING GEOMETRY CLOSE TO THE CAMERA,
// which the owner reported as "absolutely low poly even though i am very near to them". The
// instance count barely moves either way. So this lever does not trade near detail against
// far waste; it moves EVERY object together, and both ends are wrong at one distance or the
// other.
//
// 16 is the default because it is the historical behaviour and because I shipped 4 on a
// claim that turned out to be false. I wrote that Everon "draws the SAME 39,670 instances,
// so nothing is lost" -- true of the instance COUNT and irrelevant, because I never looked
// at the LOD distribution. The 56M -> 9M triangle drop I described as free is exactly the
// detail the owner then reported missing. Checking the count and calling it detail-neutral
// was the error.
//
// WGR_LOD_GOVERNOR_RANGE=4 opts into the faster, coarser behaviour from the same binary:
// worth +56% here (28.2 -> 18.1 ms) and ~2x on Arma 3 Stratis once its foliage reaches the
// retained path (WGR_TREEADV_RETAINED). Take it if you would rather have the framerate.
//
// THE ACTUAL FIX is neither value. `_maxLodInvWidth = quality * range` is documented as a
// coarseness BOUND, so shrinking the range ought only ever to refine -- it coarsens because
// the same value also sets the target-line geometry of the framerate feedback loop
// (SceneDraw.cpp lineT/lineL/lineC), so changing the range moves the loop's operating point
// instead of just clamping it. Decouple the clamp from the loop's target and the trade above
// should disappear: fine near, coarse far, at either range.
static float LodGovernorRange()
{
    static const float range = []
    {
        const char* v = std::getenv("WGR_LOD_GOVERNOR_RANGE");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 16.0f;
        return parsed >= 1.0f ? parsed : 16.0f;
    }();
    return range;
}

void Scene::SetQualitySettings(float val)
{
    _qualitySettings = val;

    // allow immediate change
    _lastScaleBetterTime = UITIME_MIN;
    _lastScaleWorseTime = UITIME_MIN;

    _minLodInvWidth = _qualitySettings;
    _maxLodInvWidth = _qualitySettings * LodGovernorRange();

#if !DENSITY_LOD
    saturate(_lodInvWidth, _minLodInvWidth, _maxLodInvWidth);
#endif
}

void Scene::SetFrameRateSettings(float val)
{
    _frameRateSettings = val;

    float minFPS = _frameRateSettings * (10.0 / 15);
    float maxFPS = _frameRateSettings * (20.0 / 15);
    _maxTargetFrameDuration = 1000 / minFPS;
    _minTargetFrameDuration = 1000 / maxFPS;

    // allow immediate change
    _lastScaleBetterTime = UITIME_MIN;
    _lastScaleWorseTime = UITIME_MIN;
}

RString Scene::GetQualityText() const
{
    char buffer[256];
    float minQ = -10 * log10(_minLodInvWidth);
    float maxQ = -10 * log10(_maxLodInvWidth);

    snprintf(buffer, sizeof(buffer), "%.0f..%.0f", maxQ, minQ);

    return buffer;
}
RString Scene::GetFrameRateText() const
{
    char buffer[256];
    float minFPS = 1000 / _maxTargetFrameDuration;
    float maxFPS = 1000 / _minTargetFrameDuration;

    snprintf(buffer, sizeof(buffer), "%.0f..%.0f", minFPS, maxFPS);

    return buffer;
}
void Scene::LoadConfig()
{
    RString name = Poseidon::GetUserParams();

    ParamFile cfg;
    cfg.Parse(name);
    // THE GOVERNOR'S TARGET (REN-VEG-003, 2026-09-02). `frameRate` is the LOD governor's set
    // point: the loop steers _lodInvWidth so the estimated frame lands between
    // 1000/(frameRate*2/3) and 1000/(frameRate*4/3) ms. The 2001 default was 15, i.e. a 10..20
    // fps band -- on a modern machine the loop never sees a slow frame, sits at its finest
    // setting, and cannot do the one job it has when the retained set is GPU-bound
    // (Chernarus+ at 18 fps read as "fine" to it, REN-VEG-002). The default is now 40 (a
    // 27..53 fps band; REN-VEG-003 measured 60 against 40 on two worlds and 60 only coarsened
    // objects on frames whose cost was grass and sky, not objects -- Everon 24.9 vs 24.3 ms
    // with one LOD level less detail), and a stored 15 is treated as the legacy default rather than a
    // choice: every existing UserInfo.cfg carries it because the game wrote it, not because
    // anyone asked for 15 fps. Any other stored value is the player's and is kept.
    // WGR_LOD_TARGET_FPS overrides both for a capture.
    constexpr float kLegacyDefaultFrameRate = 15.0f;
    constexpr float kModernDefaultFrameRate = 40.0f;
    float frameRate = kModernDefaultFrameRate;
    if (cfg.FindEntry("frameRate"))
    {
        const float stored = cfg >> "frameRate";
        if (stored > 0.0f && stored != kLegacyDefaultFrameRate)
            frameRate = stored;
    }
    if (const char* env = std::getenv("WGR_LOD_TARGET_FPS"); env && *env)
    {
        const float parsed = static_cast<float>(std::atof(env));
        if (parsed > 0.0f)
            frameRate = parsed;
    }
    SetFrameRateSettings(frameRate);
    if (cfg.FindEntry("visualQuality"))
    {
        float value = cfg >> "visualQuality";
        SetQualitySettings(value);
    }
    else
    {
        const float qFactor = ENGINE_CONFIG.lodCoef * 2 / GEngine->Width();

        SetQualitySettings(qFactor);
    }

    if (cfg.FindEntry("objectShadows"))
    {
        bool value = cfg >> "objectShadows";
        SetObjectShadows(value);
    }
    if (cfg.FindEntry("vehicleShadows"))
    {
        bool value = cfg >> "vehicleShadows";
        SetVehicleShadows(value);
    }
    if (cfg.FindEntry("cloudlets"))
    {
        bool value = cfg >> "cloudlets";
        SetCloudlets(value);
    }
    if (cfg.FindEntry("viewDistance"))
    {
        float value = cfg >> "viewDistance";
        SetPreferredViewDistance(value);
    }
    if (cfg.FindEntry("terrainGrid"))
    {
        float value = cfg >> "terrainGrid";
        SetPreferredTerrainGrid(value);
    }

    SetPreferredViewDistance(GetSelectedPreferredViewDistance());
}

void Scene::SaveConfig() const
{
    if (!IsOutOfMemory())
    {
        RString name = Poseidon::GetUserParams();

        ParamFile cfg;
        cfg.Parse(name);
        cfg.Add("frameRate", GetFrameRateSettings());
        cfg.Add("visualQuality", GetQualitySettings());
        cfg.Add("objectShadows", GetObjectShadows());
        cfg.Add("vehicleShadows", GetVehicleShadows());
        cfg.Add("cloudlets", GetCloudlets());
        cfg.Add("viewDistance", GetSelectedPreferredViewDistance());
        cfg.Add("terrainGrid", GetPreferredTerrainGrid());

        cfg.Save(name);
    }
}

extern bool ObjViewer;

namespace Poseidon
{
bool EnableObjOcc = true;

// Perf diagnostic (dumped by the PERF-shapes one-shot, SceneDraw.cpp): this frame's coverage split
// of the visible object set, tallied in ObjectForDrawing. gCovFull = diverted onto the GPU-driven
// path (no SortObject, out of the whole Pass1 walk); gCovPartial = GPU owns the opaque geometry but
// the CPU still draws the blend/decal/proxy complement; gCovNone = fully CPU (untouched by the
// opaque GPU path — geometry with no opaque-Default section, dominated by on-surface ROAD/decal
// segments, plus wire fences and glass-only objects; vegetation is alpha-TEST cutout, which IS
// GPU-owned, so it's Full, not here). Shows how much the divert removed vs what still rides Pass1.
int gCovFull = 0;
int gCovPartial = 0;
int gCovNone = 0;
} // namespace Poseidon

#include <Poseidon/World/Terrain/Occlusion.hpp>

namespace Poseidon
{
SRef<Occlusion>& GetOcclusions()
{
    static SRef<Occlusion> Occlusions = new Occlusion(256, 256);
    return Occlusions;
}
} // namespace Poseidon

// PERF-025: the census's frame denominator. Incremented HERE, once per frame -- the census
// itself runs per DRAW, and counting frames there reported "271219 draws in 271219 frames".
int gCovCensusFrames = 0;

void Scene::BeginObjects()
{
    ++gCovCensusFrames;
    gCovFull = 0;
    gCovPartial = 0;
    gCovNone = 0;

    // mark all objects and not in list:
    for (int i = 0; i < _drawObjects.Size(); i++)
    {
        _drawObjects[i]->notUsed = true;
    }

    _shadowCache.CleanUp(); // remove old shadows
    GetOcclusions()->Clear();

#if _ENABLE_CHEATS
#if !DENSITY_LOD
    auto& input = InputSubsystem::Instance();
    const int FRStep = 1;
    if (input.GetCheat1ToDo(SDL_SCANCODE_LEFTBRACKET))
    {
        float val = GetFrameRateSettings() - FRStep;
        saturateMax(val, 5);
        SetFrameRateSettings(val);
        GEngine->ShowMessage(500, "FPS@%s", (const char*)GetFrameRateText());
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_RIGHTBRACKET))
    {
        float val = GetFrameRateSettings() + FRStep;
        saturateMin(val, 100);
        SetFrameRateSettings(val);
        GEngine->ShowMessage(500, "FPS@%s", (const char*)GetFrameRateText());
    }

    if (input.GetCheat2ToDo(SDL_SCANCODE_LEFTBRACKET))
    {
        float val = GetQualitySettings() / 1.2f;
        saturate(val, 0.001, 1000);
        SetQualitySettings(val);
        GEngine->ShowMessage(500, "LOD@%s", (const char*)GetQualityText());
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_RIGHTBRACKET))
    {
        float val = GetQualitySettings() * 1.2f;
        saturate(val, 0.001, 1000);
        SetQualitySettings(val);
        GEngine->ShowMessage(500, "LOD@%s", (const char*)GetQualityText());
    }
#endif
#endif

    if (ObjViewer)
    {
        _lodInvWidth = ENGINE_CONFIG.lodCoef * 2 / (GEngine->Width());
    }

    if (_camera)
    {
        _camera->Adjust(GEngine);
    }
    else
    {
        Fail("No camera.");
    }

    // precalculate shadow properties - constant precalculation
    float addLightsFactor = GScene->MainLight()->NightEffect();
    float skyCoef = floatMin(GScene->GetLandscape()->SkyThrough(), 0.6);
    float shadowFactor = skyCoef * 0.3 + 0.1;
    int shadowFactorI = toIntFloor(shadowFactor * (1 - addLightsFactor) * 255);
    GEngine->SetShadowFactor(shadowFactorI);
}

DEFINE_FAST_ALLOCATOR(SortObject)

// POSEIDON_LOD_TRACE — one log line that answers "which LOD is the thing in front of me
// actually drawing, and how much geometry is in that LOD".
//
// This question cost a whole session on 2026-09-05 (Reforger trees looking too coarse at 5 m)
// because nothing in a running frame says it. `WGR_COVERAGE_NONE_CENSUS` names models,
// the governor trace names `_lodInvWidth`, and neither joins them to a distance and a
// chosen level for ONE object. Worse, the object most likely to be asked about --
// vegetation -- is GPU-driven Full coverage, so it never reaches AdjustComplexity() and
// cannot be traced from the draw list at all. Hence the observe call sits in
// ObjectForDrawing BEFORE the divert.
//
// The level printed is Scene::LevelFromDistance2's, i.e. the CPU path. That is also the
// retained path's answer: cull.wgsl:213-231 reimplements FindSqrtLevel over the same
// `detail2 * lod_scale^2` from the same `_lodInvWidth` and `Camera::Left()` pushed by
// wgr_set_cull_params, so a divergence here would be a bug in one of the two, not an
// expected difference.
//
// POSEIDON_LOD_TRACE=1        trace the camera-nearest visible object of any kind
// POSEIDON_LOD_TRACE=betula   ... nearest whose shape name contains "betula" (no case)
static const char* LodTraceFilter()
{
    static const char* const filter = []() -> const char*
    {
        const char* v = std::getenv("POSEIDON_LOD_TRACE");
        if (!v || !*v || std::strcmp(v, "0") == 0)
        {
            return nullptr;
        }
        return std::strcmp(v, "1") == 0 ? "" : v;
    }();
    return filter;
}

static bool LodTraceNameMatches(const char* name, const char* needle)
{
    if (!*needle)
    {
        return true;
    }
    if (!name)
    {
        return false;
    }
    std::string haystack(name), lowerNeedle(needle);
    for (char& c : haystack)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lowerNeedle)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return haystack.find(lowerNeedle) != std::string::npos;
}

void Scene::LodTraceObserve(LODShapeWithShadow* shape, float dist2, float oScale, Vector3Par direction)
{
    const char* filter = LodTraceFilter();
    if (!filter || _lodTraceDone || !shape)
    {
        return;
    }
    if (_lodTraceShape && dist2 >= _lodTraceDist2)
    {
        return;
    }
    if (!LodTraceNameMatches(shape->Name(), filter))
    {
        return;
    }
    _lodTraceShape = shape;
    _lodTraceDist2 = dist2;
    _lodTraceScale = oScale;
    _lodTraceDir = direction;
}

void Scene::LodTraceFlush()
{
    LODShapeWithShadow* shape = _lodTraceShape;
    _lodTraceShape = nullptr;
    if (!shape || _lodTraceDone || !_camera)
    {
        return;
    }
    // Skip the first second or so of drawn frames: with POSEIDON_LOD_TRACE=1 the first frame
    // is the intro/loading camera, and a one-shot line spent there answers nothing.
    if (++_lodTraceFrames < 60)
    {
        return;
    }
    _lodTraceDone = true;

    const float dist = sqrt(_lodTraceDist2);
    const int level = LevelFromDistance2(shape, _lodTraceDist2, _lodTraceScale, _lodTraceDir, _camera->Direction());
    const float camLeft = _camera->Left();
    // The value FindSqrtLevel compares the ladder against, un-squared (SceneDraw.cpp:640).
    const float resol = dist * _lodInvWidth * camLeft;

    std::string ladder;
    for (int i = 0; i < shape->NLevels(); i++)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%d:res %g faces %d", i ? " " : "", i, shape->Resolution(i),
                      shape->Level(i) ? shape->Level(i)->NFaces() : -1);
        ladder += buf;
    }
    const int faces = (level >= 0 && level < shape->NLevels() && shape->Level(level)) ? shape->Level(level)->NFaces() : -1;

    LOG_INFO(Graphics,
             "LODTRACE {} dist {:.2f} m -> level {} ({} faces); resol {:.4f} = dist * lodInvWidth {:.5f} * camLeft "
             "{:.3f} (rails {:.5f}..{:.5f}); bsphere {:.2f} m scale {:.2f}; ladder [{}]",
             shape->Name() ? shape->Name() : "<unnamed>", dist, level, faces, resol, _lodInvWidth, camLeft,
             _minLodInvWidth, _maxLodInvWidth, shape->BoundingSphere(), _lodTraceScale, ladder.c_str());
}

#define MaxDistance2(obj) (Square(ENGINE_CONFIG.objectsZ))

void Scene::ObjectForDrawing(Object* obj, int forceLOD, ClipFlags clip)
{
    static const bool playerTrace = [] {
        const char* v = std::getenv("POSEIDON_PLAYER_GROUND_TRACE");
        return v && v[0] == '1';
    }();
    static float nextPlayerTrace = -1.0f;
    const float now = playerTrace ? Glob.time.toFloat() : 0.0f;
    if (playerTrace && obj && GWorld && obj == GWorld->PlayerOn() &&
        (now >= nextPlayerTrace || now < nextPlayerTrace-1.0f))
    {
        nextPlayerTrace = now + 0.5f;
        const auto* shape = obj->GetShape();
        LOG_INFO(Graphics, "PLAYER_GROUND_QUEUE time={:.3f} camera={} forceLOD={} insideLOD={} resident={} invisible={} coverage={} model={} x={:.3f} y={:.3f} z={:.3f}",
            now, int(GWorld->GetCameraType()), forceLOD, obj->InsideLOD(GWorld->GetCameraType()),
            obj->IsVisualResident(), obj->Invisible(), obj->GetGpuCoverage(), shape ? shape->Name() : "<none>",
            obj->Position().X(), obj->Position().Y(), obj->Position().Z());
    }
    // Camera-independent collision/state must not re-enter through CPU fallback.
    if (!obj->IsVisualResident())
        return;
    LODShapeWithShadow* shape = obj->GetShapeOnPos(obj->Position());

    // some objects may be trivially clipped
    // get rid of them ...
    bool invisible = false;
    Vector3Val pos = obj->Position();
    if (!shape)
    {
        return; // nothing to draw
    }

    // GPU-render divert (docs/gpu-culling-and-depth-plan.md — "CPU-render divert"): a FULL-coverage
    // GPU-driven object is culled, LOD-selected, colour-drawn AND shadow-cast entirely by the
    // compute/indirect path, so it never needs a SortObject. Skipping the add here takes it out of
    // the whole Pass1 walk — the frustum/shadow-visibility tests below, QSort, Occlusion::TestBBox,
    // AdjustComplexity, the SortObject alloc churn, and the DrawSortObject / AddShadowCaster loops
    // — which is the actual per-frame CPU saving the retained scene was built for. PARTIAL objects
    // stay: the GPU owns their opaque geometry but the CPU still paints/casts the complement
    // (blend/decal sections + non-GPU proxies), so they keep a SortObject and DrawSortObject /
    // AddShadowCaster drop the GPU-owned sections via GSkipGpuOwnedSections.
    //
    // Read the coverage cached ON the Object (Object::_gpuCoverage, stamped by EngineWgpu at
    // register/move/remove) rather than the virtual GEngine->GpuDrivenCoverage() — this runs for
    // EVERY visible object (~160 K/frame at 20 km view distance with the whole island in view), so
    // even the virtual dispatch is worth dropping. 0 (None) for GL33 / dynamics / unregistered
    // shapes, so they fall straight through to the CPU path below.
    // POSEIDON_LOD_TRACE, before the divert: a Full-coverage object returns two lines below
    // and never reaches AdjustComplexity, which is exactly the case (vegetation) the trace
    // exists for. Off unless the env var is set — LodTraceObserve's first test is the cached
    // filter pointer.
    if (LodTraceFilter())
    {
        LodTraceObserve(shape, _camera->Position().Distance2Inline(pos), obj->Scale(), obj->Direction());
    }

    // Full retained trees never reach DrawSortObject. Only actual snow invokes
    // the optional bounded owner proof, before that divert; dry worlds retain
    // the field-only coverage read. No registration/all-world ray sweep.
    const auto& treeSnow = GSnow();
    if ((treeSnow.enabled && treeSnow.Depth() > 0.0f) || treeSnow.EffSnowlineEnabled())
    {
        const MapType canopy = shape->GetMapType();
        if (canopy == MapTree || canopy == MapSmallTree)
            GEngine->PrepareVisibleTreeSnow(obj);
    }
    const int gpuCov = obj->GetGpuCoverage();
    if (gpuCov == static_cast<int>(GpuDrawCoverage::Full))
    {
        gCovFull++;
        return;
    }
    (gpuCov == static_cast<int>(GpuDrawCoverage::Partial) ? gCovPartial : gCovNone)++;
    // PERF-025: WHICH models miss the GPU-driven path. PERF-024 measured 6.6 ms/frame spent
    // recording the CPU complement, and the pass-1 line counts it (`coverage none=`) without
    // naming a single model, which is a number nobody can act on. WGR_COVERAGE_NONE_CENSUS=1
    // keeps a per-shape tally and prints the worst offenders once a second, with the
    // denominator, so "none" becomes a list of models to look at.
    if (gpuCov != static_cast<int>(GpuDrawCoverage::Partial))
    {
        static const bool census = std::getenv("WGR_COVERAGE_NONE_CENSUS") != nullptr;
        if (census)
        {
            static std::map<std::string, int> tally;
            static int framesAtLastReport = 0;
            static std::chrono::steady_clock::time_point lastReport{};
            if (shape)
            {
                tally[shape->Name() ? shape->Name() : "<unnamed>"]++;
            }
            const auto now = std::chrono::steady_clock::now();
            if (lastReport.time_since_epoch().count() == 0)
            {
                lastReport = now;
            }
            const int frames = gCovCensusFrames - framesAtLastReport;
            if (std::chrono::duration<double>(now - lastReport).count() > 1.0 && frames > 0)
            {
                std::vector<std::pair<std::string, int>> rows(tally.begin(), tally.end());
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                int total = 0;
                for (const auto& r : rows)
                    total += r.second;
                std::string top;
                for (std::size_t i = 0; i < rows.size() && i < 12; ++i)
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s=%.1f ", rows[i].first.c_str(),
                                  double(rows[i].second) / double(frames ? frames : 1));
                    top += buf;
                }
                LOG_INFO(Graphics,
                         "PERF coverage-none census over {} draws in {} frames ({:.0f}/frame), {} distinct models: {}",
                         total, frames, double(total) / double(frames ? frames : 1), rows.size(), top.c_str());
                tally.clear();
                framesAtLastReport = gCovCensusFrames;
                lastReport = now;
            }
        }
    }

    float radius = obj->GetRadius();
    // get object position in clipping coordinates

    if (obj != GWorld->CameraOn())
    {
        if (_camera->IsClipped(pos, radius, 1))
        {
            // check if shadow is enabled
            if (shape->Special() & NoShadow)
            {
                return;
            }
            // some invisible objects have visible shadows
            // simple test - shadow possible
            if (_camera->IsClipped(pos, radius + 50, 1))
            {
                return;
            }

            Vector3 objTop = obj->Position() + Vector3(0, radius, 0);
            Vector3 shadowPos = objTop;

            // estimate shadow position
            if (!ShadowPos(objTop, shadowPos, _mainLight))
            {
                return;
            }
            if (_camera->IsClipped(shadowPos, 0.5f, 1))
            {
                // shadow of object top is clipped
                // check whole shadow shape
                // estimate shadow radius and shadow center
                float distTopBot = shadowPos.Distance(pos) * 0.5f;
                Vector3 shadowCenter = (shadowPos + pos) * 0.5f;
                float shadowRadius = floatMax(distTopBot, radius);
                if (_camera->IsClipped(shadowCenter, shadowRadius, 1))
                {
                    return;
                }
            }
            invisible = true;
        }
    }

    // calculate distance
    float dist2 = _camera->Position().Distance2Inline(pos);

#if DENSITY_LOD
    // if object is too far, we ignore its shadow
    if (dist2 > Square(ENGINE_CONFIG.objectsZ))
        return;

    // if object is smaller than 1 pixel, do not draw it
    // it would cause aliasing artifacts
    float areaK = _camera->InvLeft() * _camera->InvTop() * GEngine->Width() * GEngine->Height();
    if (Square(radius * 2) * areaK < dist2)
    {
        return;
    }
#endif

    if ((shape->GetAndHints() & ClipFogMask) == ClipFogShadow && (shape->GetOrHints() & ClipFogMask) == ClipFogShadow)
    {
        if (dist2 >= Square(_shadowFogMaxRange + radius))
        {
            return;
        }
    }

    SortObject* sObj = obj->GetInList();
    if (!sObj)
    {
        int index = _drawObjects.Add(new SortObject);
        sObj = _drawObjects[index];

        sObj->object = obj;
        sObj->shape = shape;
        sObj->radius = radius;
        obj->SetInList(sObj); // this object is in list
    }

    if (invisible)
    {
        sObj->forceDrawLOD = LOD_INVISIBLE;
    }
    else
    {
        sObj->forceDrawLOD = forceLOD;
    }

    // alpha-pass sort key: far extent (planar camera-space depth + radius), so the
    // object's depth-writing blend sections draw before interpenetrating dust
    sObj->zCoord = AlphaSort::AlphaObjectDepth((ScaledInvTransform() * pos).Z(), radius);

    // if object is near we use nearest distance instead of center distance
    // this avoid degenerate LODs when beign near
    // if radius is 0.25 of distance, it is considered significant
    if (Square(radius) > dist2 * Square(0.25))
    {
        float dist = dist2 * InvSqrt(dist2);
        float distNear = dist - radius;
        saturateMax(distNear, 0);
        dist2 = Square(distNear);
    }

#if DENSITY_LOD
    sObj->shadowLOD = -1; // override with autodetection
    sObj->drawLOD = -1;   // override with autodetection
#else
    sObj->shadowLOD = LOD_INVISIBLE; // override with autodetection
    sObj->drawLOD = LOD_INVISIBLE;   // override with autodetection
#endif
    sObj->distance2 = dist2;
    sObj->passNum = -1; // noninit

    sObj->orClip = clip;
    sObj->notUsed = false;
}

#define DRAW_OBJS 1

void Scene::CloudletForDrawing(Object* obj)
{
    if (!GetCloudlets())
    {
        return;
    }
#if DRAW_OBJS
    // some objects may be trivially clipped
    // get rid of them ...
    Vector3Val pos = obj->Position();
    float radius = obj->GetRadius();

    // perform clip test
    if (_camera->IsClipped(pos, radius, 1))
    {
        return;
    }

    Vector3 cPos = GScene->ScaledInvTransform() * pos;

    // camera plane clip test
    // perform more distant clipping than normal
    // in case of real 3d object peform normal clipping
    float nearest = _camera->Near() * obj->CloudletClippingCoef();
    // Test the SPHERE against the near plane, not the centre. For a legacy
    // cloudlet (one puff) the two are the same test. For a volume that owns many
    // particles the centre can sit behind the camera while half the plume is in
    // front of it — you are standing in the smoke — and culling on the centre
    // dropped every particle at once. A coefficient of 0 plus the radius term
    // lets such an object through; each billboard still does its own near test.
    if (cPos.Z() + radius < nearest)
    {
        return;
    }

    float dist2 = _camera->Position().Distance2Inline(pos);

    // estimate area
    float size2 = Square(radius * GEngine->Width() * _camera->InvLeft());
    if (size2 < dist2)
    {
        return;
    }

    SortObject* sObj = obj->GetInList();
    if (!sObj)
    {
        int index = _drawObjects.Add(new SortObject);
        sObj = _drawObjects[index];

        sObj->object = obj;
        sObj->radius = radius;
        sObj->shape = obj->GetShape();
        obj->SetInList(sObj); // this object is in list
                              // new object - never occluded?
    }
    sObj->object = obj;
    sObj->drawLOD = 0;
    sObj->forceDrawLOD = 0;
    sObj->shadowLOD = LOD_INVISIBLE;
    sObj->distance2 = dist2;
    sObj->zCoord = cPos.Z(); // alpha-pass sort key: centre camera-space depth (cPos computed above)
    sObj->radius = radius;
    sObj->passNum = 2; // all cloudlets drawn in alpha pass
    sObj->notUsed = false;
#endif
}

void Scene::ObjectForDrawing(Object* obj)
{
    // used for drawdiags and volume lights
    ObjectForDrawing(obj, -1, ClipAll);
}
