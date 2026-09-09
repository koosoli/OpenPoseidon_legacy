#pragma once

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>

#define HORIZONT_Z ENGINE_CONFIG.horizontZ
#define OBJECT_Z ENGINE_CONFIG.objectsZ

#define MIN_FOG (200.0f)
#define MAX_FOG (HORIZONT_Z - 20)

#define MIN_SHADOWFOG (100.0f)
#define MAX_SHADOWFOG (ENGINE_CONFIG.shadowsZ)

#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>

namespace Poseidon
{
const float CloudScale = 0.1;

const float MinSkyFog = 4000.0f * CloudScale;
const float MaxSkyFog = 19000.0f * CloudScale;

#define LEN_FOG_TABLE 256
class FogFunction
{
  private:
    byte _fog[LEN_FOG_TABLE];
    float _start2, _end2;
    float _divisor;

  public:
    FogFunction();
    void Set(float start, float end, float (*function)(float distRel, float start, float end));
    int operator()(float distSquare) const; // avoid partial stall
};

class SortObject : public RefCount
{
  public:
    Ref<Object> object;        // must outlive drawing; Ref guarantees the shape exists
    LODShapeWithShadow* shape; // randomized shape

    signed char drawLOD, shadowLOD;
    signed char passNum;
    signed char forceDrawLOD; // forced draw LOD

    unsigned char orClip; // or clip flags only - andClip would always be 0

    bool notUsed; // not used when list was created - delete it

    float radius; // bounding sphere radius

    float distance2;

    float zCoord; // alpha-pass sort key: camera-space depth; see AlphaSortOrder.hpp

    // Sort keys decorated once per frame in Scene::AdjustComplexity, so the object
    // sorts' comparators (CmpShapeObj, CmpSurfaceObj) read a plain field instead of
    // re-deriving these on every one of the O(n log n) comparisons — the derivation
    // (a Level(0)->NFaces() pointer chase and a virtual PassOrder() call) dominated
    // the sorts once the Ref-swap churn was removed.
    int sortComplexity; // GetShape()->Level(0)->NFaces()  (CmpShapeObj shape tiebreak)
    int sortPassOrder;  // object->PassOrder(drawLOD)       (CmpSurfaceObj primary key)

    USE_FAST_ALLOCATOR
};

typedef RefArray<SortObject> SortObjectList;

class RemmemberShadow : public RefCount, public CLRefLink
{
    friend class ShadowCache;

  private:
    Ref<Shape> _shadow;
    OLink<Object> _object;
    Vector3 _lightDir;
    Matrix4 _objectPos;
    int _level; // which LOD of object is used
    Foundation::Time _lastUsed;
    bool _splitOnly; // shadow cache is also used to contain split surfaces

  public:
    RemmemberShadow();
    // init shadow or split to fit on surface
    void Init(Object* object, Vector3Par lightDir, int level, Matrix4Par pos, bool splitOnly = false);
    bool IsShadow(Object* object, int level) const;
    Object* GetObject() const { return _object; }
    Vector3Val LightDir() const { return _lightDir; }
    Matrix4Val ObjectPos() const { return _objectPos; }

    USE_FAST_ALLOCATOR
};

class ShadowCache : public Foundation::MemoryFreeOnDemandHelper
{
  private:
    CLRefList<RemmemberShadow> _data;

  public:
    ShadowCache();
    ~ShadowCache() override;

    Ref<Shape> Shadow(Object* object, Vector3Par lightDir, int level, Matrix4Par pos, bool splitOnly = false);
    void ShadowChanged(Object* obj);
    void Clear();
    void CleanUp();

    size_t FreeOneItem() override;
    float Priority() override;

    // memory-budget observability (dev panel). ~10 KB per remembered shadow,
    // matching ShadowMemSize in Shadow.cpp.
    const char* DomainName() const override { return "Shadows.Cache"; }
    size_t HeldBytes() const override { return (size_t)_data.Size() * (10 * 1024); }
};

enum PreloadedShape
{
    CobraLight,
    SphereLight,
    HalfLight,
    Marker,
    CraterShell,
    SlopBlood,
    CloudletBasic,
    CloudletFire,
    CloudletWater,
    Cloud1,
    Cloud2,
    Cloud3,
    Cloud4,
    CinemaBorder,

    FootStepL,
    FootStepR,

    ForceArrowModel,
    SphereModel,
    RectangleModel,
    BulletLine,

    MaxPreloadedShape
};

typedef Ref<Light> ActiveLightPointer;

class LightList : public FindArray<ActiveLightPointer, Foundation::MemAllocSS>
{
  public:
    LightList(bool staticStorage = false);
    LightList(const LightList& src);
};

class PreloadedTextures
{
    RefArray<Texture> _data;

  public:
    PreloadedTextures();
    ~PreloadedTextures();

    void Preload(bool all);
    void Clear();

    Texture* New(RStringB name);                   // make texture permanent
    Texture* New(::Poseidon::PreloadedTexture id); // predefined texture ids
};

extern PreloadedTextures GPreloadedTextures;

// Scene graph management: LOD and light management.
class Scene
{
  private:
    Color _constantColor;
    Color _skyColor;
    float _constantFog;

    Ref<Texture> _skyTexture;

    Ref<LODShapeWithShadow> _preloaded[MaxPreloadedShape];

    Camera* _camera;
    LightSun* _mainLight;

    FindArray<Link<Light>> _lights;
    LightList _aLights;

    Ref<Object> _collisionStar;

    // fog functions for different types of objects
    FogFunction _fog, _skyFog, _shadowFog, _tacticalFog;
    float _tacticalVisibility;                    // AI sensors
    float _rainRange;                             // display boundaries main control
    float _fogMaxRange, _fogMinRange;             // display boundaries
    // FAR-001: the view-distance-derived display fog range, kept alongside the
    // possibly-overridden one so the far-plane policy has a stable input.
    float _baseFogMaxRange{0.0f};
    // FAR-001: altitude-derived display fog range; 0 means no override.
    float _aerialFogMaxRange{0.0f};
    float _shadowFogMaxRange, _shadowFogMinRange; // display boundaries

    mutable float _lodInvWidth;

    // POSEIDON_LOD_TRACE state (see Scene.cpp). Nearest matching shape seen during the
    // current frame's object walk, flushed and logged ONCE by AdjustComplexity().
    LODShapeWithShadow* _lodTraceShape = nullptr;
    float _lodTraceDist2 = 0.0f;
    float _lodTraceScale = 1.0f;
    Vector3 _lodTraceDir{0.0f, 0.0f, 1.0f};
    int _lodTraceFrames = 0;
    bool _lodTraceDone = false;

    float _frameRateSettings;
    float _qualitySettings;

    mutable Foundation::UITime _lastScaleBetterTime; // avoid oscillation
    mutable Foundation::UITime _lastScaleWorseTime;
    mutable float _maxTargetFrameDuration;
    mutable float _minTargetFrameDuration;

    mutable float _minLodInvWidth; // visual quality limits
    mutable float _maxLodInvWidth;

    enum
    {
        NStoreComplexities = 4
    };

    mutable int _lastComplexity[NStoreComplexities]; // complexity history

    mutable SortObjectList _drawObjects;
    mutable SortObjectList _drawMergers;

    mutable ShadowCache _shadowCache;
    SRef<Landscape> _landscape; // only pointer to landscape

    bool _objectShadows, _vehicleShadows, _cloudlets;
    float _objectLODBias = 1.0f;
    float _preferredTerrainGrid;
    float _preferredViewDistance;

  public:
    Scene();
    void Init(Engine* engine, Landscape* landscape);
    void ResetFog();
    void CleanUp();
    ~Scene();

    bool GetObjectShadows() const { return _objectShadows; }
    bool GetVehicleShadows() const { return _vehicleShadows; }
    bool GetCloudlets() const { return _cloudlets; }

    float GetMinimalTerrainGrid() const;
    float GetPreferredTerrainGrid() const { return _preferredTerrainGrid; }
    float GetPreferredViewDistance() const { return _preferredViewDistance; }

    void SetPreferredTerrainGrid(float x);
    void SetPreferredViewDistance(float x);

    void SetObjectShadows(bool set = true);
    void SetVehicleShadows(bool set = true);
    void SetCloudlets(bool set = true);

    // Object LOD bias driving entity-LOD selection — multiplier on the
    // entity's projected screen size before LOD lookup.  >1 picks a
    // higher LOD (more detail) at a given distance; <1 picks lower.
    // Range clamped 0.25..4.0 in setter.  Read by RenderShape /
    // LODSelect path; default 1.0 (no bias).
    void SetObjectLODBias(float bias);
    float GetObjectLODBias() const { return _objectLODBias; }

    Camera* GetCamera() { return _camera; }
    const Camera* GetCamera() const { return _camera; }
    void SetCamera(const Camera& camera);

    void SetConstantColor(ColorVal color) { _constantColor = color; }
    ColorVal GetConstantColor() const { return _constantColor; }

    void SetConstantFog(float fog) { _constantFog = fog; }
    float GetConstantFog() const { return _constantFog; }

    const Matrix4& ScaledInvTransform() const;
    const Matrix3& CamNormalTrans() const;
    const Matrix4& CamInvTrans() const;

    Texture* SkyTexture() const { return _skyTexture; }

    void ResetLights();
    void AddLight(Light* light);

    Light* GetLight(int i) const { return _lights[i]; }
    int NLights() const { return _lights.Size(); }

    void SelectActiveLights(Object* dimmed);
    void SetActiveLights(const LightList& lights);
    const LightList& ActiveLights() const { return _aLights; }

    // Select light affecting given position
    const LightList& SelectLights(Vector3Par pos, float radius, LightList& work); // may return work or something else

    // Select light affecting given object
    const LightList& SelectLights(Matrix4Par objPos, const Object* object, int level, LightList& work);

    LightSun* MainLight() const { return _mainLight; }
    void SetMainLight(LightSun* light) { _mainLight = light; }
    void MainLightChanged(); // fog/light color has been changed

    void SetTacticalVisibility(float tacVis, float rainRange);
    float GetTacticalVisibility() const { return _tacticalVisibility; }

    float GetLodInvWidth() const { return _lodInvWidth; }
    // The governor's last decision, for the capture JSON (REN-VEG-004): what it measured, what
    // it estimated, and the complexity it estimated from. Diagnostic only; nothing reads them.
    struct GovernorTrace
    {
        float frameMs = 0.0f;     // GetAvgFrameDuration(4) as the loop saw it
        float estimatedMs = 0.0f; // estDuration after the maxDuration clamp
        int   complexity = 0;     // the sum the estimate was made from (CPU + shadow + retained + land)
        int   retained = 0;       // the retained term inside that sum
        float targetMinMs = 0.0f, targetMaxMs = 0.0f;
        float lodMin = 0.0f, lodMax = 0.0f;
        float avgComplexity = 0.0f; // the four-frame history mean the estimate divides by
        int   history[4] = {};
        int   iterations = 0;       // loop iterations spent this frame
    };
    const GovernorTrace& LastGovernorTrace() const { return _governorTrace; }

  private:
    GovernorTrace _governorTrace;
    int _governorRetainedLast = 0; // last VALID retained triangle count (the readback skips frames)
    float _governorWhatIfBase = 0.0f;    // the _lodInvWidth the last what-if ladder was measured at
    float _governorWhatIfScale[4] = {};  // ascending scales of the ladder (0.5, 0.71, 1.41, 2.0)
    int   _governorWhatIfTris[4] = {};   // retained triangles at those scales

  public:
    float GetSmokeGeneralization() const;

    float GetFrameRateSettings() const { return _frameRateSettings; }
    void SetFrameRateSettings(float val);
    RString GetFrameRateText() const;

    float GetQualitySettings() const { return _qualitySettings; }
    void SetQualitySettings(float val);
    RString GetQualityText() const;

    void LoadConfig();
    void SaveConfig() const;

    float GetFogMaxRange() const { return _fogMaxRange; }
    float GetFogMinRange() const { return _fogMinRange; }

    /// FAR-001. The display fog range the view-distance slider alone implies --
    /// i.e. what `GetFogMaxRange()` returned before the aerial override existed,
    /// and what it still returns at ground level.
    ///
    /// The far-plane policy needs the BASE value, not the current one: it feeds
    /// its own previous answer back in otherwise, and a range that is used to
    /// compute the next range ratchets upwards frame by frame until it hits a
    /// clamp. Anything asking "how far does the player's setting see" wants this;
    /// anything shading a pixel wants `GetFogMaxRange()`.
    float GetBaseFogMaxRange() const { return _baseFogMaxRange; }

    /// FAR-001. Override the display fog range with an altitude-derived one, or
    /// pass 0 to release the override and fall back to the view-distance-derived
    /// value.
    ///
    /// Only ever RAISES the range (`ResetFog` ignores a value below the base), so
    /// this cannot be used to shorten a player's view. Rebuilding the fog tables
    /// is not free, so a call that does not move the range by more than a metre
    /// is dropped -- which also means a ground-level frame, where the argument is
    /// a steady 0, never rebuilds anything.
    void SetAerialFogMaxRange(float range);
    float GetShadowFogMaxRange() const { return _shadowFogMaxRange; }
    float GetShadowFogMinRange() const { return _shadowFogMinRange; }

    int TacticalFog8(float distSquare) const { return _tacticalFog(distSquare); }
    int Fog8(float distSquare) const { return _fog(distSquare); }
    int ShadowFog8(float distSquare) const { return _shadowFog(distSquare); }
    int SkyFog8(float distSquare) const { return _skyFog(distSquare); }

    void CalculateSkyColor(Texture* texture);

    Landscape* GetLandscape() const { return _landscape; }
    ShadowCache& GetShadowCache() const { return _shadowCache; }

    // Render-only object cull distance (metres). NOT ENGINE_CONFIG.objectsZ, which is
    // also the simulation radius: this is the distance the renderer stops DRAWING
    // objects at, anchored to the fog max range so an object crossing the edge is
    // already fully dissolved into the sky. See SceneDraw.cpp for the derivation and
    // the WGR_OBJECT_DISTANCE_SCALE knob. Both the CPU draw path
    // (LevelFromDistance2) and the GPU-driven cull (wgr_set_cull_params) read this,
    // so they cannot drift apart.
    float GetObjectDrawDistance() const;

    int LevelFromDistance2(LODShape* shape, float distance2, float oScale, Vector3Par direction,
                           Vector3Par viewDirection);
    int LevelShadowFromDistance2(LODShape* shape, float distance2, float oScale, Vector3Par direction,
                                 Vector3Par viewDirection);

    // POSEIDON_LOD_TRACE — see Scene.cpp. Observe() is called for EVERY visible object
    // (before the GPU-driven divert, so retained vegetation is included); Flush() logs the
    // camera-nearest match once per process.
    void LodTraceObserve(LODShapeWithShadow* shape, float dist2, float oScale, Vector3Par direction);
    void LodTraceFlush();

    void AdjustComplexity();

    int AdjustComplexity(SortObjectList& objs);
    int AdjustShadowComplexity(SortObjectList& objs);

    void BeginObjects();
    void ObjectForDrawing(Object* obj, int forceLOD, ClipFlags clip);
    void CloudletForDrawing(Object* obj);
    void ObjectForDrawing(Object* obj);

    void EndObjects(); // sort all objects
    void DrawReflections(const WaterLevel& water);
    void DrawObjectsAndShadowsPass1();
    void DrawObjectsAndShadowsPass2();
    // Shadow-map depth pass (off by default): collect the visible casters and render
    // the cascade depth maps from the sun.  Lives in SceneShadowPass.cpp to keep
    // Scene.cpp under the file-size limit.  Called from Pass2 when shadow maps are on.
    void RenderShadowMapDepthPass(int nDraw);
    // GPU-driven variant (Engine::UsesGpuShadowCasters): submits caster meshes +
    // transforms via AddShadowCaster instead of collecting a CPU triangle soup.
    void RenderShadowMapDepthPassGpu(int nDraw);
    void DrawObjectsAndShadowsPass3(); // last draw cockpits
    void ObjectsDrawn();               // release all temporary information

    // light color and position
    // `sizeScale` keeps local emitters visually distinct from the solar disc.
    // The legacy code used the same full-screen flare sprite for both.
    void DrawFlare(ColorVal color, Vector3Par pos, bool secondary = true, float sizeScale = 1.0f,
                   bool emissiveCore = false, bool analytic = false);
    void DrawFlares();

    void DrawRainLevel(float alpha, float yDensity, float xOffset, float yOffset, float z);
    void DrawRain();

    void DrawDiagModel(Vector3Par pos, LODShapeWithShadow* shape, float size = 0.1, PackedColor color = PackedWhite);
    void DrawCollisionStar(Vector3Par pos, float size = 0.1, PackedColor color = PackedWhite);
    //! LGT-018: `minAngular` is the smallest angle (radians) the marker may subtend. A light
    //! marker is a WORLD-SPACE object, so without a floor it shrinks below one pixel and the
    //! lamp simply is not there any more -- which is why flying over a lit village showed
    //! black, and why a truck's headlight read as switched off. `maxWorld` caps the other end.
    void DrawVolumeLight(LODShapeWithShadow* shape, PackedColor color, const Frame& pos, float size,
                         float minAngular = 0.0f, float maxWorld = 0.0f);

    bool ShadowPos(Vector3Par pos, Vector3& aprox, LightSun* light);

    LODShapeWithShadow* ForceArrow() const { return Preloaded(ForceArrowModel); }

    LODShapeWithShadow* Preloaded(PreloadedShape type) const
    {
        PoseidonAssert(type < MaxPreloadedShape);
        return _preloaded[type];
    }
    void SetPreloaded(PreloadedShape type, LODShapeWithShadow* shape)
    {
        PoseidonAssert(type < MaxPreloadedShape);
        _preloaded[type] = shape;
    }
    void SetCollisionStar(Object* obj) { _collisionStar = obj; }
    bool HasPreloaded() const { return _preloaded[CraterShell] != nullptr; }
    Texture* Preloaded(::Poseidon::PreloadedTexture type) const { return GPreloadedTextures.New(type); }
    Texture* Preloaded(RStringB name) const { return GPreloadedTextures.New(name); }

    void DrawExShadow(SortObject* oi); // exact shadow casting
};

extern Scene* GScene;
#define GLOB_SCENE (GScene)

#define IS_SHADOW_VEHICLE (GScene ? GScene->GetVehicleShadows() : true)
#define IS_SHADOW_OBJECT (GScene ? GScene->GetObjectShadows() : true)

}  // namespace Poseidon
