#pragma once

#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <Poseidon/Graphics/Core/MeshVertex.hpp> // SVertex, for the queued mesh-create payloads (REN-THR-008)
#include <Poseidon/Core/Types.hpp>               // VertexIndex, same
#include <Poseidon/Graphics/Core/TLVertex.hpp> // TLMaterial (per-draw lighting capture)
#include <Poseidon/Graphics/Dummy/EngineDummy.hpp>
#include <Poseidon/Graphics/GraphicsEngineFactory.hpp> // GraphicsEngineParams
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <Poseidon/Graphics/Shared/SDLEventWindow.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>       // Ref<> (MAT-050's untextured stand-in)
#include <Poseidon/Graphics/Textures/TextureBank.hpp>   // Texture, complete for the Ref<> member
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>

#include <wgpu_renderer.hpp>

#include <array>
#include <chrono> // steady_clock, for the opt-in auto-exposure time constant (WGR_AUTO_EXPOSURE_TAU)
#include <list>   // parked GPU model LRU
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Window;

namespace Poseidon
{
class TextureBankWgpu;
class TextureWgpu;
class TerrainWgpu;
class WaterWgpu;
class ITerrainRenderer;
class Object;
class Helicopter;
class LODShapeWithShadow;

enum class Sampler2DFlags : uint32_t
{
    None = 0,
    ClampU = 1,
    ClampV = 2,
    Point = 4,
};

constexpr Sampler2DFlags operator|(Sampler2DFlags a, Sampler2DFlags b)
{
    return static_cast<Sampler2DFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr Sampler2DFlags& operator|=(Sampler2DFlags& a, Sampler2DFlags b)
{
    return a = a | b;
}

// Inherits from EngineDummy, so we don't have to add manual stubs for all the missing virtual functions.
class EngineWgpu : public EngineDummy
{
  public:
    explicit EngineWgpu(const GraphicsEngineParams& params);
    ~EngineWgpu() override;

    // False if the window / wgpu device failed to come up; the factory then drops
    // this engine and falls back.
    bool IsValid() const { return _renderer != nullptr; }
    RString GetDebugName() const override;
    RString GetRendererName() const override;

    void HandleEvents() override { _eventWindow.HandleEvents(); }
    bool IsOpen() const override { return _eventWindow.IsOpen(); }
    void SetMouseGrab(bool grab) override { _eventWindow.SetMouseGrab(grab); }
    bool IsMouseGrabbed() const override { return _eventWindow.IsMouseGrabbed(); }

    int Width() const override;
    int Height() const override;

    void StartTextInput() override;
    void StopTextInput() override;
    bool IsTextInputActive() const override;

    bool SetSwapInterval(int interval) override;
    int GetSwapInterval() const override { return _swapInterval; }

    bool IsWindowed() const override;
    bool CanBeWindowed() const override;

    AbstractTextBank* TextBank() override;

    void InitDraw(bool clear, PackedColor color) override;
    void FinishDraw() override;
    void NextFrame() override;
    void Clear(bool clearZ, bool clearColor, PackedColor color) override;
    void Screenshot(RString filename) override;
    void FlushPendingScreenshot() override;

    void Draw2D(const Draw2DPars& pars, const Rect2DAbs& rect, const Rect2DAbs& clip) override;
    void DrawPoly(const MipInfo& mip, const Vertex2DAbs* vertices, int n, const Rect2DAbs& clip,
                  int specFlags) override;
    void DrawPoly(const MipInfo& mip, const Vertex2DPixel* vertices, int n, const Rect2DPixel& clip,
                  int specFlags) override;
    void DrawLine(const Line2DAbs& line, PackedColor c0, PackedColor c1, const Rect2DAbs& clip) override;

    // Fallback CPU SurfaceSplit paths (projected/blob shadows and the diagnostic
    // WGR_GPU_CONFORM=0 road path) use the same world-space separation as GL33.
    // Production WGPU roads retain their shared authored mesh, conform against the
    // terrain heightmap in the vertex shader, and receive the OnSurface depth bias.
    float ZShadowEpsilon() const override { return 0.01f; }
    float ZRoadEpsilon() const override { return 0.005f; }

    bool GetTL() const override { return true; }
    bool GetTLOnSurface() const override { return true; }
    bool UsesGpuSkinning() const override { return true; }
    VertexBuffer* CreateVertexBuffer(const Shape& src, VBType type) override;
    void UpdateFrameCamera() override;
    // MAT-052: honour mid-frame clip-range changes (the cockpit's zSpace bracket);
    // see the definition for the full story.
    void UpdateProjection() override;
    void PrepareMeshTL(const LightList& lights, const Matrix4& modelToWorld, const render::LegacySpec& spec) override;
    void BeginMeshTL(const Shape& sMesh, int spec, bool dynamic) override;
    void EndMeshTL(const Shape& sMesh) override;
    void DrawSectionTL(const Shape& sMesh, int beg, int end) override;
    // Captures the per-section material so DrawSectionTL can fold it with the sun
    // (GL33 parity: emissive + sun_ambient + sun_diffuse * N.L). Called by
    // ShapeSection::PrepareTL immediately before each DrawSectionTL.
    void SetMaterial(const TLMaterial& mat, const LightList& lights, const render::LegacySpec& spec) override;

    // GPU-driven retained scene hooks (docs/gpu-culling-and-depth-plan.md Stage 3b).
    // Active only with WGR_GPU_DRIVEN: register the object's shape once, then stream its
    // retained instance (add on create, patch on move, drop on remove). GpuDrivenObject
    // reports whether an object is currently drawn by the GPU path, so the scene draw loop
    // suppresses its CPU colour draw while its shadow caster stays on the CPU path.
    void SceneObjectCreated(Object* obj) override;
    void SceneObjectRemoved(Object* obj) override;
    void SceneObjectMoved(Object* obj) override;
    GpuDrawCoverage GpuDrivenCoverage(const Object* obj) const override;
    bool GpuDrivenProxy(const Object* parent, int level, int proxyIndex) const override;
    void SuppressWorldObjects(bool suppress) override;

    // Software-T&L path: 3D-in-UI objects (e.g. the menu laptop) arrive here with
    // CPU-projected screen-space vertices, drawn depth-tested like 2D-with-depth.
    void PrepareMesh(const render::LegacySpec& spec) override;
    void BeginMesh(TLVertexTable& mesh, const render::LegacySpec& spec) override;
    void EndMesh(TLVertexTable& mesh) override;
    void PrepareTriangle(const MipInfo& mip, int specFlags) override;
    void DrawSection(const FaceArray& face, Offset beg, Offset end) override;
    // Screen-space billboard (every cloudlet, plus light halos / flares). Same
    // empty-EngineDummy-body story as DrawLine below: until this override existed,
    // all smoke, dust, exhaust and muzzle clouds drew nothing under wgpu.
    void DrawDecal(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color, const MipInfo& mip,
                   int specFlags) override;
    // Same batch as DrawDecal, but the quad is stretched along an explicit
    // screen-space direction: rain streaks lean with the wind, which an
    // axis-aligned sprite cannot show. Depth-tested, non-writing, clipped --
    // everything DrawDecal does except the rectangle's orientation.
    void DrawDecalRotated(Vector3Par screen, float rhw, float sizeX, float sizeY, PackedColor color,
                          const MipInfo& mip, int specFlags, float dirX, float dirY) override;
    // World-space line / point primitives, fed by the same software-T&L mesh as
    // DrawSection above (Object::DrawLines / Object::DrawPoints). Without these the
    // base EngineDummy body is empty, so every world diagnostic line -- ballistics
    // trails, tracers, AI path debug, the viewer grid -- was computed and discarded.
    void DrawLine(int beg, int end) override;
    void DrawPoints(int beg, int end) override;

    void SetBias(int value) override { _bias = value; }
    int GetBias() override { return _bias; }
    void GetZCoefs(float& zAdd, float& zMult) override;

    // HDR tonemap/look tuning (ImGui Tonemap tab). Only meaningful with the HDR
    // resolve pass; SupportsTonemap gates the tab. SetTonemapSettings pushes to the
    // renderer immediately (takes effect next frame). In auto mode the grade is driven
    // from the per-time-of-day presets each frame (UpdateAutoTonemap in NextFrame).
    bool SupportsTonemap() const override { return _hdrEnabled && _renderer != nullptr; }
    bool SupportsDepthOfField() const override { return true; }
    DepthOfFieldSettings GetDepthOfFieldSettings() const override { return _dof; }
    void SetDepthOfFieldSettings(const DepthOfFieldSettings& s) override;

    // REN-TEMP-001 §6.7 — the dev panel's Temporal tab. Set pushes to the renderer
    // immediately (applies next frame); Get returns the last pushed values, seeded from
    // the shipped defaults, so the panel never clobbers an env-tuned session until the
    // user actually moves a control.
    bool SupportsTemporalTuning() const override { return _renderer != nullptr && _hdrEnabled; }
    // Options-menu graphics knobs that were engine-wide no-ops on wgpu until 2026-08-30.
    // MSAA is startup-fixed in the renderer, so the setter only records the wish — the
    // pre-engine env seed in GameApplication makes it real on the next launch.
    void SetMsaaSamples(int samples) override;
    // renderScale 1.0 = AUTO (the renderer's own default, incl. DLSS Quality). Any other
    // value is an explicit pin, applied live through the temporal tuning path.
    void SetRenderScale(float scale) override;
    TemporalSettings GetTemporalTuning() const override { return _temporalTuning; }
    void SetTemporalTuning(const TemporalSettings& t) override;
    TemporalInfo GetTemporalInfo() override;

    TonemapSettings GetTonemapSettings() const override { return _tonemap; }
    void SetTonemapSettings(const TonemapSettings& s) override;
    bool GetTonemapAuto() const override { return _tonemapAuto; }
    void SetTonemapAuto(bool enable) override { _tonemapAuto = enable; }
    ExposureSettings GetExposureSettings() const override { return _exposure; }
    void SetExposureSettings(const ExposureSettings& s) override;
    float GetAutoExposureScale() const override;
    // Emit the scene->UI resolve marker (WGR_CMD_RESOLVE) into the command stream.
    void ResolveSceneToDisplay() override;

    // Procedural atmospheric sky (ImGui Sky tab). Authored look is edited here and pushed via
    // PushRenderParams; the celestial fields are refreshed every frame from LightSun in
    // PushSkyRuntime (both called from NextFrame). See docs/procedural-sky-plan.md.
    bool SupportsSky() const override { return _renderer != nullptr; }
    SkySettings GetSkySettings() const override { return _sky; }
    void SetSkySettings(const SkySettings& s) override;
    // Second cloud layer (high cirrus at ~7 km): on/off and flat-vs-volumetric. Defined here
    // rather than folded into PushRenderParams because it is its own FFI entry point, not a lane
    // in WgrSkyLook — growing that struct changes a size the ABI handshake checks (same reasoning
    // as wgr_set_cloud_shadow_strength). _sky is updated too so GetSkySettings keeps round-tripping.
    void SetSkyCirrus(bool enabled, bool volumetric) override
    {
        _sky.cirrusEnabled = enabled;
        _sky.cirrusVolumetric = volumetric;
        if (_renderer != nullptr)
            wgr_set_cirrus_mode(_renderer, enabled ? (volumetric ? 2u : 1u) : 0u);
    }
    // Second-layer shape: puffiness + how much it varies from that on its own. Same shape as
    // SetSkyCirrus above (own FFI entry point, _sky updated so GetSkySettings round-trips).
    void SetSkyCirrusPuffiness(float puffiness, float variation) override
    {
        _sky.cirrusPuffiness = puffiness;
        _sky.cirrusPuffVariation = variation;
        if (_renderer != nullptr)
            wgr_set_cirrus_puffiness(_renderer, puffiness, variation);
    }
    // Second-layer AMOUNT + how far it matches the deck below. Same shape again (own FFI entry
    // point, _sky updated so GetSkySettings round-trips and "Reset to defaults" restores it).
    void SetSkyCirrusLook(float amount, float matchDeck) override
    {
        _sky.cirrusAmount = amount;
        _sky.cirrusMatchDeck = matchDeck;
        if (_renderer != nullptr)
            wgr_set_cirrus_look(_renderer, amount, matchDeck);
    }
    // How soft the SECOND layer's cloud edges are. Same shape again.
    void SetSkyCirrusSoftness(float softness) override
    {
        _sky.cirrusSoftness = softness;
        if (_renderer != nullptr)
            wgr_set_cirrus_softness(_renderer, softness);
    }
    // Where the distance fog finishes, as a fraction of the draw distance. Same shape again.
    void SetFogFarClose(float close) override
    {
        _sky.fogFarClose = close;
        if (_renderer != nullptr)
            wgr_set_fog_far_close(_renderer, close);
    }
    // Road / decal per-pixel ground conform: the three knobs that decide whether a road wins
    // its depth test against the terrain it lies on. Its own struct and its own FFI entry
    // point, for the same reason SetFogFarClose has one -- pushed live, never baked.
    RoadSettings GetRoadSettings() const override { return _road; }
    void SetRoadSettings(const RoadSettings& road) override
    {
        _road = road;
        if (_renderer != nullptr)
            wgr_set_road_conform(_renderer, _road.liftFlat, _road.liftPerMetre, _road.liftMaxFrac);
    }
    // God rays. Same shape again (own FFI entry point, _sky updated so GetSkySettings round-trips
    // and "Reset to defaults" restores it). The renderer clamps every value, so a garbage push
    // cannot make the march expensive.
    void SetGodRays(bool enabled, float intensity, float density, float distance, float g, float cloudInfluence,
                    int steps, int resDiv) override
    {
        _sky.godRays = enabled;
        _sky.godRayIntensity = intensity;
        _sky.godRayDensity = density;
        _sky.godRayDistance = distance;
        _sky.godRayG = g;
        _sky.godRayCloudInfluence = cloudInfluence;
        _sky.godRaySteps = steps;
        _sky.godRayResDiv = resDiv;
        if (_renderer != nullptr)
            wgr_set_god_rays(_renderer, enabled ? 1u : 0u, intensity, density, distance, g, cloudInfluence,
                             static_cast<uint32_t>(steps < 0 ? 0 : steps),
                             static_cast<uint32_t>(resDiv < 0 ? 0 : resDiv));
    }
    // Suppress the legacy skydome on wgpu while the procedural sky is drawing.
    bool ProceduralSkyActive() const override { return _renderer != nullptr && _sky.enabled; }

    // GPU water look (ImGui Water tab). Edited here and read live by WaterWgpu, which
    // pushes them into the water UBO each frame. Gated on the water renderer existing.
    bool SupportsWater() const override { return _renderer != nullptr && _water != nullptr; }
    WaterSettings GetWaterSettings() const override { return _waterLook; }
    void SetWaterSettings(const WaterSettings& s) override;
    // Legacy terrain grass layers call these while submitting their GrassTexture
    // overlays.  The procedural system uses that exact hook as its eligibility
    // signal instead of drawing over every opaque terrain cell.
    void SetGrassParams(float a1, float a2, float a3 = 0, float a4 = 0) override;
    void AddGrassImpact(Vector3Par position, float radius) override;
    bool CanGrass() const override { return _renderer != nullptr; }
    GrassSettings GetGrassSettings() const override { return _grass; }
    void SetGrassSettings(const GrassSettings& settings) override;
    bool HasGrassPhotoClumps() const override;
    bool HasGrassMapClutter() const override;
    int GetGrassPhotoFamilyCount() const override;
    const char* GetGrassPhotoFamilyName(int index) const override;
    int GetGrassSurfaceCount() const override;
    const char* GetGrassLoadedMapName() const override;
    const char* GetGrassSurfaceName(int index) const override;
    bool IsGrassSurfaceEnabled(int index) const override;
    void SetGrassSurfaceEnabled(int index, bool enabled) override;
    // Live look, read by WaterWgpu::DrawWater when building the per-frame water UBO.
    const WaterSettings& WaterLook() const { return _waterLook; }

    // WTR-002 — GPU water-pipeline pass timings, read back from wgr_get_gpu_timings
    // (non-blocking; the Rust side harvests asynchronously). Names follow the
    // WgrGpuTimerRegion index contract.
    int GetWaterGpuTimings(float* outMs, int maxCount) const override;
    const char* GetWaterGpuTimingName(int region) const override;
    // PERF-005 — per-region CPU encode ms (same indices) and this frame's object accounting.
    int GetCpuTimings(float* outMs, int maxCount) const override;
    // Rate-limited periodic object-cost log row. On the unconditional frame path, NOT beside
    // the water rows in WaterWgpu.cpp — those are gated on the water pipeline running, and
    // object cost is exactly what needs measuring on worlds with no water.
    void LogObjectPerfRow();
    bool GetObjectStats(ObjectStatsOut& out) const override;
    uint32_t GetRuntimeCapabilityFlags() const override;

    // FAR INSTANCE TIER — the whole proxy set for a world, pushed once at load from the
    // authored placement rows (Landscape::LoadOprwModern). Everything past the object
    // residency window is invisible without it; nothing here ever becomes an Object.
    void SetFarProxyInstances(const FarProxyInstance* data, size_t count) override;

    // GRS-A — grass instance counts, read back from wgr_get_grass_stats.
    bool GetGrassStats(GrassStatsOut& out) const override;
    bool GetGpuMemoryStats(GpuMemoryStatsOut& out) const override;

    // GPU-driven cull DEBUG (ImGui Culling tab): only meaningful when GPU-driven is on.
    bool SupportsCullDebug() const override { return _renderer != nullptr && _gpuDriven; }
    CullDebugSettings GetCullDebugSettings() const override { return _cullDebug; }
    void SetCullDebugSettings(const CullDebugSettings& s) override;

    // Cascaded shadow maps, GPU-driven caster submission (SceneShadowPass).
    void SetShadowMapsEnabled(bool enabled) override { _smTuning.enabled = enabled; }
    bool ShadowMapsEnabled() const override { return _smTuning.enabled && _renderer != nullptr; }
    ShadowMapTuning GetShadowMapTuning() const override { return _smTuning; }
    void SetShadowMapTuning(const ShadowMapTuning& tuning) override
    {
        _smTuning = tuning;
        // The terrain sun-shadow + sky-visibility knobs ride the consolidated render-params
        // block (assembled + clamped in PushRenderParams). The renderer diffs them, so the
        // sweep realloc / scan rebuild only happens on an actual change.
        PushRenderParams();
    }
    // Foliage lighting knobs (docs/foliage-translucency-plan.md) — stored here, folded into the
    // consolidated render-params block by PushRenderParams and read by the object shader.
    FoliageSettings GetFoliageSettings() const override { return _foliage; }
    void SetFoliageSettings(const FoliageSettings& s) override
    {
        _foliage = s;
        PushRenderParams();
    }
    // Screen-space AO knobs (docs/screen-space-ao-plan.md) — stored here, folded into the
    // consolidated render-params block by PushRenderParams.
    AoSettings GetAoSettings() const override { return _ao; }
    void SetAoSettings(const AoSettings& s) override
    {
        _ao = s;
        PushRenderParams();
    }
    bool SupportsMaterialDebug() const override { return _renderer != nullptr; }
    MaterialDebugSettings GetMaterialDebugSettings() const override { return _materialDebug; }
    void SetMaterialDebugSettings(const MaterialDebugSettings& s) override
    {
        _materialDebug = s;
        PushMaterialDebug();
    }

    // Push the Materials tab's state to the retained path, where it is read per
    // fragment out of the frame UBO.
    //
    // This used to try to rebuild instead: it dropped `_gpuModels` so each shape
    // would re-register with the new answer baked into its material record. That
    // could never work on a loaded world. `_gpuInstances` was untouched, so
    // SceneObjectCreated early-returns for every live object; and the hook only
    // fires from Landscape::AddObject, which is load/spawn-only and never runs
    // again. The cache cleared, nothing re-registered, and the toggle did nothing
    // -- which is exactly what was reported. Baking presentation state into
    // retained data was the mistake; these are switches, so they are switches.
    void PushMaterialDebug()
    {
        if (_renderer == nullptr)
        {
            return;
        }
        uint32_t view = 0;
        switch (_materialDebug.view)
        {
            case MaterialDebugSettings::View::BaseColorOnly: view = 1; break;
            case MaterialDebugSettings::View::NormalMap: view = 2; break;
            case MaterialDebugSettings::View::LightingOnly: view = 3; break;
            default: view = 0; break; // every other view stays on the per-draw path
        }
        uint32_t flags = 0;
        flags |= _materialDebug.disableNormalMap ? (1u << 0) : 0u;
        flags |= _materialDebug.invertNormalY ? (1u << 1) : 0u;
        flags |= _materialDebug.composeMultiLayers ? (1u << 2) : 0u;
        // DZ-007 -- bit 3, the dev panel's "Disable Fresnel / Environment" checkbox. It
        // suppresses the WHOLE water treatment DZ-003 and DZ-005 added: the Fresnel sky
        // reflection AND the world-space ripple that shapes its normal. One switch that
        // returns a water surface to its pre-DZ-003 look is what a human wants; splitting
        // them would leave a rippling near-black quad, which is not a state anyone is trying
        // to look at. `WGR_WATER_RIPPLE` remains the separate ripple-only control, because a
        // capture A/B needs one term to move and this one moves two.
        //
        // A switch, not baked state: read per fragment out of the frame UBO, for exactly the
        // reason recorded above this function -- presentation state baked into a retained
        // material record cannot be changed on a world that is already loaded.
        flags |= _materialDebug.disableFresnelEnvironment ? (1u << 3) : 0u;
        wgr_set_material_debug(_renderer, view, flags);
    }
    MaterialDebugInfo GetMaterialDebugInfo() const override { return _materialDebugInfo; }
    // Interior sky visibility (docs/interior-sky-visibility-plan.md) — same pattern: stored
    // here, folded into the consolidated render-params block by PushRenderParams.
    InteriorSkySettings GetInteriorSkySettings() const override { return _interiorSky; }
    void SetInteriorSkySettings(const InteriorSkySettings& s) override
    {
        _interiorSky = s;
        PushRenderParams();
    }
    void SetShadowMapSunFactor(float factor01) override { _smSunFactor = factor01; }
    bool UsesGpuShadowCasters() const override { return true; }
    void SetShadowCascades(const shadow::CascadeSet& cascades, int resolution) override;
    void AddShadowCaster(const Shape& mesh, const Matrix4& modelToWorld) override;
    bool DumpShadowMap(const char* path) override;
    bool ShadowDepthProbe(const float* lightVP16, const float* triXYZ, int vertCount, int res,
                          float* outDepth) override;

    bool SupportsOverlayRenderer() const override { return _renderer != nullptr; }
    uint64_t OverlayTextureCreate(int w, int h, const uint8_t* rgba) override;
    void OverlayTextureUpdate(uint64_t texture, int w, int h, const uint8_t* rgba) override;
    void OverlayTextureDestroy(uint64_t texture) override;
    void SubmitOverlay(const OverlayVertex* verts, int vertCount, const uint16_t* indices, int indexCount,
                       const OverlayDrawCmd* cmds, int cmdCount) override;

    void OnWindowResized(int w, int h) override;

    // GPU terrain renderer (always active on this backend).
    ITerrainRenderer* GetTerrainRenderer() override;
    // Called by the terrain renderer: append a batch of `nodes` for the current
    // camera and enqueue its draw in submission order.
    void SubmitTerrain(std::span<const WgrTerrainNode> nodes);

    // GPU water renderer (active unless WGR_GPU_WATER=0; null keeps legacy water).
    IWaterRenderer* GetWaterRenderer() override;
    // Called by the water renderer: append a batch of `nodes` for the current camera
    // and enqueue its draw in submission order (after the opaque terrain + 3D).
    void SubmitWater(std::span<const WgrWaterNode> nodes);
    // Called by TerrainWgpu after its terrain batch. The procedural grass system owns
    // all blade placement; C++ only preserves ordering and the source camera.
    void SubmitGrass();

  private:
    // A camera-relative view/projection plus the world-space camera position the
    // per-object world matrices are offset by, and the forward direction (shadow
    // cascade eye-depth select).
    struct CameraEntry
    {
        GfxMatrix proj;
        GfxMatrix view;
        float pos[3];
        float dir[3];
    };

    void ResizeSurface(int w, int h);
    // Append triangles under command `kind` (DRAW_2D or DRAW_SCREEN), merging with
    // the previous batch only when it is the most recent command of the same kind
    // and texture + blend + sampler match.
    void AppendTriangles(uint64_t texture, WgrBlend blend, Sampler2DFlags sampler, WgrDepthMode depth,
                         std::span<const WgrVertex2D> verts);
    // Push a camera entry built from the current scene camera and make it active.
    void PushSceneCamera();
    // Establish a camera for the frame's first 3D draw if none has been pushed yet.
    void EnsureCamera();

    // Assemble the consolidated imgui-tweakable render params (tonemap, exposure, sky look,
    // terrain sun-shadow, sky-visibility) from _tonemap/_exposure/_sky/_smTuning and push them
    // via wgr_set_render_params. Called on every edit and once per frame from NextFrame; the
    // renderer diffs the terrain sub-blocks so the per-frame push is cheap. See
    // docs/render-params-consolidation-plan.md.
    void PushRenderParams();
    // Assemble the per-frame sky runtime (eased celestial values from LightSun + camera
    // altitude / fog range) and push it via wgr_set_sky_runtime. Called each frame.
    void PushSkyRuntime();
    // In auto mode, interpolate the per-ToD preset for the current game time into
    // _tonemap and push it. Called once per frame from NextFrame.
    void UpdateAutoTonemap();
    // In auto mode (_sky.autoToD), interpolate the per-ToD atmosphere preset for the
    // current game time into _sky (preserving the live toggle knobs). Called once per
    // frame from NextFrame, before the render-params push.
    void UpdateAutoSky();
    void SyncWaterLookProfile();
    // Gentle, view-dependent eye accommodation for the visible sun. Kept separate from
    // scene-average auto-exposure, which is a different measurement (frame luminance, not
    // sun visibility) and which -- as of 2026-08-16 -- is itself enabled by default; the
    // stale note here said it "remains disabled to prevent white-outs".
    void UpdateSunGlareExposure();
    // Convert the adaptation TIME CONSTANT (seconds) into the per-frame ease the exposure
    // shader wants, using this frame's measured wall-clock delta. See RND-039: the authored
    // `rate` is per-FRAME and therefore framerate-dependent, which makes any tuning of it
    // specific to one world's fps -- the defect that must not ship with a default-on feature.
    void UpdateAutoExposureRate();
    // The time constant actually in force: the WGR_AUTO_EXPOSURE_TAU override when the env
    // var was set (including a deliberate 0, which selects the legacy per-frame `rate`),
    // otherwise the authored `ExposureSettings::rateTau`. Read every frame, not cached, so a
    // live change on the Tonemap tab's "Adapt time (s)" slider takes effect immediately.
    float EffectiveAutoExposureTau() const;

    SDL_Window* _window = nullptr;
    WgrRenderer* _renderer = nullptr;
    // Registers renderer-owned dynamic VRAM alongside the engine's existing RAM
    // caches, so every compatibility layer uses one budget/debug surface.
    Foundation::MemoryDomainProbe _gpuMemoryProbe;
    // Optional low-frequency residency trace used by unattended compatibility
    // smokes.  It samples the same cross-layer accounting as the Memory tab.
    uint64_t _residencyTraceFrame = 0;
    RString _pendingScreenshotPath;
    // HDR path enabled (mirrors the renderer's WGR_HDR gate) — gates the tonemap tab.
    // Default on, matching the renderer; WGR_HDR=0 forces it off (see the ctor env read).
    bool _hdrEnabled = true;
    // Auto = drive _tonemap from the per-ToD presets; false = manual override (tab).
    bool _tonemapAuto = true;
    Engine::TonemapSettings _tonemap;
    Engine::DepthOfFieldSettings _dof;
    Engine::ExposureSettings _exposure;
    // WGR_AUTO_EXPOSURE_TAU, an OVERRIDE of `_exposure.rateTau` and not the source of it.
    // NEGATIVE = the env var was not set, so the authored `rateTau` decides; >= 0 = the env
    // var was set and wins, with an explicit 0 meaning "use the legacy per-frame rate".
    //
    // The sentinel is negative rather than 0 precisely because 0 is a MEANINGFUL setting here:
    // conflating "unset" with "off" would have made WGR_AUTO_EXPOSURE_TAU=0 indistinguishable
    // from not passing it at all, and therefore useless as the A/B against a default-on
    // feature -- which is the one comparison this variable now exists to make.
    float _autoExposureTau = -1.0f;
    // Per-frame ease derived from _autoExposureTau; 0 = not in tau mode. PushRenderParams
    // prefers this over _exposure.rate when it is positive.
    float _autoExposureRate = 0.0f;
    // Wall-clock stamp of the previous UpdateAutoExposureRate call (tau mode only).
    std::chrono::steady_clock::time_point _autoExposureLast{};
    // Small multiplier applied only while the sun is centred in the player's view.
    // The authored time-of-day tonemap exposure remains unchanged in dev controls.
    float _sunGlareExposure = 1.0f;
    // Live GPU-water look, edited by the Water tab, read by WaterWgpu each frame.
    Engine::WaterSettings _waterLook;
    std::string _waterLookMap;
    bool _waterLookDirty = false;
    // Authored procedural-sky params (atmosphere + look); celestial fields are filled
    // per frame from LightSun in PushSkyRuntime.
    Engine::SkySettings _sky;
    // Road / decal ground conform, pushed live from the dev panel (see SetRoadSettings).
    Engine::RoadSettings _road;
    // Smoothed celestial inputs: LightSun::Recalculate refreshes sun/moon direction,
    // night factor and fog colour only every few seconds with no interpolation, which
    // makes the sun disc + horizon haze stutter. PushSkyRuntime eases these toward the
    // live values each frame (snapping on init / large jumps). See procedural-sky-plan §9.
    bool _skyInit = false;
    // When the sky smoothing last stepped, so its ease can be a TIME constant instead of a
    // per-frame fraction. Zero-initialised time_point = "no previous step", treated as a snap.
    //
    // STEADY CLOCK, not the snapshot's `timeSeconds`, and the reason is precision. That field
    // is `Glob.time.toFloat()` -- seconds since mission start, in a float -- so after four
    // hours of play its representable step is about 1 ms and after eight it is 2 ms, against
    // a 120 fps frame of 8 ms. The dt would quantise to something coarse and wrong LATE in a
    // session and be fine early, which is the worst shape a bug can have. Pausing is not a
    // problem for a real clock here: with the target constant the ease simply converges and
    // stops, and a time skip is already handled by the large-jump snap.
    std::chrono::steady_clock::time_point _skySmoothAt{};
    Vector3 _skySunDir;
    Vector3 _skyMoonDir;
    float _skyMoonPhase = 0.5f;
    float _skyNight = 0.0f;
    float _skyFog[3] = {0.7f, 0.75f, 0.8f};
    // Cloud-deck advection offset in world metres, INTEGRATED rather than computed
    // as velocity*time. With a constant authored cloudWind the closed form was
    // exact, but a weather-driven wind changes speed and heading, and v(t)*t would
    // teleport the whole deck every time it did. Double so the accumulation stays
    // exact over a long session; wrapped to kWindWrap in PushSkyRuntime.
    double _cloudOffsetX = 0.0;
    double _cloudOffsetZ = 0.0;
    double _cloudOffsetClock = 0.0; // last sky clock value, for the dt
    bool _cloudOffsetInit = false;
    TextureBankWgpu* _wbank = nullptr;
    // MAT-050 -- the 1x1 stand-in an OBJECT SECTION gets when nothing resolved its albedo.
    //
    // A section that reaches the renderer with texture_id == 0 samples bindless slot 0, and
    // slot 0 is opaque WHITE (rust/src/textures.rs). White is albedo 1.0, which in a daylight
    // HDR frame is brighter than every authored surface on Stratis (of 4,273 Arma 3 textures
    // only 35 average >= 250, all UI/overlay art), so an unresolved section does not merely
    // look wrong -- it out-shines the sun-lit world and blooms. That is the "bright orbs"
    // report, and, on the far LODs of buildings, the "textures look wrong" one.
    //
    // Slot 0 itself must stay white: it is the multiply identity for every draw that has no
    // texture BY DESIGN and carries its colour in the material (2D fills, lines, procedural
    // constants). So the correction is applied here, at the one place that knows the draw is a
    // 3D object section whose albedo genuinely failed to resolve. Created lazily on first need
    // through the texture bank's dynamic path, so a session that never hits the case allocates
    // nothing. Owned by the bank (Ref keeps it alive for the session).
    //
    // OFF BY DEFAULT (`WGR_UNTEXTURED_ALBEDO`, see UntexturedAlbedoHandle). Nothing about the
    // shipped frame changes until the knob is set; what ships unconditionally is the
    // DIAGNOSTIC -- the counters below and the per-draw UNTEXTURED warning that never existed
    // on the direct path, which is why a lamp mast could sample slot 0 for a whole session
    // without a single line naming it.
    Ref<Texture> _untexturedAlbedo;
    uint64_t _untexturedAlbedoHandle = 0;
    // How much took that fallback, per path, so a run reports the size of the problem rather
    // than only the first offender of each material. The direct one counts DRAWS (a section is
    // re-submitted every frame it is visible); the retained one counts sections, since
    // registration happens once per shape.
    long long _untexturedDirectDraws = 0;
    long long _untexturedRetainedSections = 0;
    SDLEventWindow _eventWindow;
    int _w = 0;
    int _h = 0;
    int _swapInterval = 1;
    bool _windowed = true;

    float _clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<WgrVertex2D> _verts;
    std::vector<WgrDraw2DBatch> _batches;
    std::vector<WgrDraw3D> _draws3d;
    std::vector<WgrCmd> _cmds;
    // Bone-matrix pool for skinned draws (128-matrix blocks; world pre-multiplied in).
    std::vector<WgrMat4> _palette;
    // Frame-global point/spot lights (rebuilt each frame in NextFrame, <= WGR_MAX_LIGHTS).
    std::vector<WgrLight> _lights;

    // --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---
    // On when WGR_GPU_DRIVEN=1 at construction (mirrors the Rust-side gate); the hooks and
    // GpuDrivenObject are inert otherwise, so every other path keeps the CPU draw.
    bool _gpuDriven = false;
    // Cull debug toggles (ImGui Culling tab), pushed to the renderer on change.
    CullDebugSettings _cullDebug;
    // Registered shapes -> model id. WGR_INVALID_MODEL marks a shape scanned and found
    // ineligible (transparent/decal/etc.), so an object using it never re-scans and stays
    // on the CPU path.
    std::unordered_map<const LODShapeWithShadow*, uint32_t> _gpuModels;
    // Per-registered-model coverage (§12): Full = the GPU draws the whole object (skip the CPU
    // draw); Partial = the shape has proxies and/or non-owned (blend/decal) sections, so the CPU
    // still draws the complement with GSkipGpuOwnedSections set. Keyed by shape like _gpuModels.
    std::unordered_map<const LODShapeWithShadow*, GpuDrawCoverage> _gpuModelCoverage;
    // §12d-full: shapes that have a CPU complement (some visible section is NOT GPU-owned —
    // blend/decal), so the object can NEVER be Full even if all its proxies move to the GPU. A
    // shape that is Partial ONLY because of proxies is absent here and becomes Full once every
    // proxy is GPU-driven (SceneObjectCreated). Populated by RegisterGpuModel.
    std::unordered_set<const LODShapeWithShadow*> _gpuModelComplement;
    // Shapes registered as terrain-conform (ClipLand, mode 2): their instances carry the
    // CONFORM_CLIPLAND flag + bcSurfaceY so the GPU-driven VS conforms them to SurfaceY.
    std::unordered_set<const LODShapeWithShadow*> _gpuConformShapes;
    struct GpuInstance
    {
        uint32_t model;
        uint32_t slot;
        const LODShapeWithShadow* shape = nullptr;
        // Debug bookkeeping for the Culling tab's nearby-instance dump: the position + conform
        // mode CAPTURED AT REGISTRATION (what the retained buffer holds), compared against the
        // object's live Position() to expose stale transforms.
        Vector3 pos = VZero;
        int mode = 0;
        // How much of this object the GPU draws — read by GpuDrivenCoverage to tell the scene
        // draw loop whether to suppress the whole CPU draw (Full) or just the owned sections
        // (Partial). Cached from _gpuModelCoverage at add time.
        GpuDrawCoverage coverage = GpuDrawCoverage::Full;
    };
    // Objects handed to the GPU path -> their model + retained instance slot.
    std::unordered_map<const Object*, GpuInstance> _gpuInstances;
    // REN-TEMP-001H / ID-1: draw-object identity for motion vectors. The scene draw
    // loop brackets each object via Engine::SetDrawObject; the identity itself lives ON
    // the Object (Object::RenderId, render-snapshot design §4) and is folded to the
    // 16-bit wire lane (bits 16-31 of WgrDraw3D::misc; 0 = none) per draw.
    TemporalSettings _temporalTuning;
    int _msaaRequested = -1;
    uint32_t CurrentDrawObjectId();
    // §12d: interior furniture proxies moved to the GPU as CHILD instances. A proxy is a shared
    // ProxyObject on the parent shape at a reference LOD; its world = parentTransform *
    // proxyLocalTransform, a static composite for a static parent. Only proxies whose shape is
    // Full coverage (self-contained: no complement sections, no nested proxies) are taken; the
    // rest stay on the CPU (Object::DrawProxies). Keyed by the PARENT object.
    struct GpuProxyChild
    {
        int proxyIndex; // index into the reference LOD's Proxy() list (for the DrawProxies skip)
        uint32_t slot;  // retained-instance slot (for update on move / remove)
        uint32_t model; // the proxy shape's model (for re-composing the transform on move)
        const LODShapeWithShadow* shape = nullptr;
    };
    struct GpuProxySet
    {
        int refLevel = -1; // the parent LOD whose proxy list we registered (finest with proxies)
        std::vector<GpuProxyChild> children;
    };
    std::unordered_map<const Object*, GpuProxySet> _gpuProxies;
    // Register the parent's eligible interior proxies as GPU child instances (§12d); records them
    // in _gpuProxies so DrawProxies skips them and Moved/Removed maintain them. Returns true iff
    // EVERY proxy was moved onto the GPU (or there are none) — i.e. no proxy remains for the CPU,
    // which (with no complement) lets the parent become Full (§12d-full).
    bool EmitGpuProxies(Object* parent, LODShapeWithShadow* shape);
    // Drop a parent's proxy child instances (on remove / destruction / shape swap).
    void RemoveGpuProxies(const Object* parent);
    // Dedicated pool meshes the retained scene OWNS (one per registered model LOD), created
    // directly from shape geometry so they survive ShapeBank::OptimizeAll's release of the
    // shapes' own vertex buffers. Destroyed in the dtor. (Map-change re-registration is a TODO:
    // the shape-keyed _gpuModels map would otherwise go stale when ShapeBank clears.)
    std::unordered_set<uint64_t> _gpuMeshes;
    // Per-model residency. Model/LOD metadata in Rust is append-only and cheap;
    // the expensive pool meshes and textures are released when the last streamed
    // instance leaves the working set, then rebuilt if that shape returns.
    std::unordered_map<const LODShapeWithShadow*, std::vector<uint64_t>> _gpuModelMeshes;
    std::unordered_map<const LODShapeWithShadow*, std::vector<TextureWgpu*>> _gpuModelTextures;
    std::unordered_map<const LODShapeWithShadow*, uint32_t> _gpuModelRefs;
    void RetainGpuModel(const LODShapeWithShadow* shape);
    void ReleaseGpuModel(const LODShapeWithShadow* shape);
    // PARKED registrations (WGR_GPU_MODEL_PARK, EngineWgpu.cpp). When the last instance of a
    // shape leaves, its meshes/materials are kept -- not destroyed -- in a bounded LRU keyed by
    // the shape, as long as the shape itself is alive (the streamer's evict cache holds it). A
    // re-admission then finds the model in _gpuModels and skips the whole rebuild. The texture
    // handles baked into the model's materials are recorded so an unpark can prove they are
    // still the live ones (a TrimResidency eviction in between changes them); if not, the
    // parked entry is destroyed and the model rebuilt. Shape death (LODShape::SetDestroyListener
    // -> OnShapeDestroyed) destroys the entry, so a key can never dangle.
    std::unordered_map<const LODShapeWithShadow*, std::vector<uint64_t>> _gpuModelTextureHandles;
    std::list<const LODShapeWithShadow*> _gpuModelParked; // front = oldest
    std::unordered_map<const LODShapeWithShadow*, std::list<const LODShapeWithShadow*>::iterator> _gpuModelParkedIndex;
    void DestroyGpuModel(const LODShapeWithShadow* shape); // meshes + every shape-keyed map entry
    //! Drop every registered model whose baked bindless slots no longer match the textures
    //! they were baked from, so the next admit rebuilds them. Returns how many went.
    //!
    //! `wgr_model_register` bakes a material's slot index permanently. A texture evicted by
    //! TextureBankWgpu::TrimResidency therefore leaves its models on the white fallback FOREVER
    //! -- re-uploading gives the texture a NEW slot the baked material never learns, and worse,
    //! hands the old slot to some other upload, so the model can come back wearing a stranger's
    //! texture. The parked path has always revalidated handles for exactly this reason; a LIVE
    //! model had no such check, which is the difference between a surface that recovers and one
    //! that is white until the world reloads.
    //!
    //! Since the liveness sweep landed this also does the REFILL half: a mismatch on a model
    //! the sweep says is in range is first met with EnsureUploaded() on the missing textures,
    //! and only a model whose re-upload failed (or whose texture never held a slot lease, so
    //! its baked index really has moved) is destroyed. That ordering is what lets
    //! TrimResidency evict a pinned texture at all -- eviction becomes a round trip rather
    //! than a one-way door.
    uint32_t InvalidateStaleGpuModels();

    // --- Retained-path texture liveness (REN-RES-001 "Liveness") -------------------------
    //! Refresh `_lastUsedFrame` on the textures of every registered model that still has an
    //! instance near the camera, so TextureBankWgpu's LRU can age retained textures the same
    //! way it ages direct-draw ones.
    //!
    //! WHY THIS SHAPE, and what it is not. The honest signal would be "which models survived
    //! the GPU cull", and the renderer does read cull results back -- but only as AGGREGATE
    //! counters (wgr_get_object_stats), never per model, and making them per-model is a Rust
    //! change. The CPU's own visible set is no good either: Scene::ObjectForDrawing diverts a
    //! FULL-coverage object out of the draw list BEFORE the frustum test, so the CPU never
    //! learns whether the very objects most at risk (whole buildings) were visible.
    //!
    //! So this sweeps the retained instance table itself and asks a cheaper question --
    //! "is any instance of this model within the object draw distance" -- which is a strict
    //! SUPERSET of what drew (no frustum, no size cull, no occlusion). Erring toward marking
    //! is the safe direction: a texture wrongly kept costs VRAM, a texture wrongly evicted
    //! costs a white building.
    //!
    //! Granularity: PER MODEL, not per instance. Lag: up to one sweep pass -- the table is
    //! walked a slice of buckets per frame (kRetainedLivenessSweepFrames) so a 200k-instance
    //! world costs a bounded fraction of a frame rather than one hitch, and a pass therefore
    //! completes well inside TrimResidency's 120-frame grace.
    void MarkRetainedModelLiveness();
    // Rolling bucket cursor into _gpuInstances. unordered_map iteration is only stable while
    // the bucket count is, so a rehash restarts the pass rather than silently skipping part of
    // the table -- restarting costs latency, skipping would cost correctness.
    size_t _livenessCursor = 0;
    size_t _livenessBuckets = 0;
    // Completed sweeps. Also the dedupe key: a shape marked in this pass is not walked again
    // until the next one, so a model with 5,000 instances costs one texture walk, not 5,000.
    uint64_t _livenessPass = 0;
    // Residency frame at which the last pass COMPLETED. The valve in MarkRetainedModelLiveness
    // withdraws the pin relaxation when this falls too far behind.
    uint64_t _livenessPassFrame = 0;
    std::unordered_map<const LODShapeWithShadow*, uint64_t> _livenessMarked;
    //! True iff `shape` was marked in the current or the immediately preceding pass. The
    //! previous pass counts because the current one is only partly walked at any moment, so
    //! testing against it alone would call every not-yet-visited model dead.
    bool RetainedModelIsLive(const LODShapeWithShadow* shape) const;
    uint32_t DestroyGpuModelMeshes(const LODShapeWithShadow* shape); // just the pool meshes
    void UnparkGpuModel(const LODShapeWithShadow* shape);  // no-op if not parked
    void OnShapeDestroyed(const LODShape* shape);
    // Register `shape` (all graphical LODs + per-section geometry/material) if not already,
    // returning its model id or WGR_INVALID_MODEL if it is ineligible for the GPU path.
    uint32_t RegisterGpuModel(LODShapeWithShadow* shape);

    std::vector<CameraEntry> _cameras;
    uint32_t _currentCamera = 0;
    bool _haveCamera = false;
    GfxMatrix _world{}; // camera-relative world for the current mesh
    Matrix4 _worldM{};  // same, as an engine Matrix4 (for pre-multiplying into skin palettes)
    // Object-level spec from BeginMeshTL (IsShadow / OnSurface / z-bias / fog);
    // combined with each section's material spec in DrawSectionTL.
    int _meshSpec = 0;
    // Material captured by SetMaterial for the section about to be drawn. The
    // default (diffuse/ambient white, emissive/forcedDiffuse black) leaves an
    // unlit-by-material fallback if a draw ever reaches DrawSectionTL without a
    // preceding SetMaterial. Folded with the sun per section in DrawSectionTL.
    TLMaterial _curMaterial;
    // Current z-bias level (engine sets it via SetBias before each draw): decals
    // 0x10, ZBias overlay faces level*5, shadows 0x10/0x20. 0 = no bias.
    int _bias = 0;
    // Palette slot for the current skinned mesh, pre-multiplied once in BeginMeshTL
    // and shared by all its sections; WGR_NO_PALETTE when the mesh isn't skinned.
    uint32_t _currentPaletteSlot = WGR_NO_PALETTE;

    TLVertexTable* _swMesh = nullptr;
    uint64_t _swTexture = 0;
    WgrBlend _swBlend = WGR_BLEND_OPAQUE;
    Sampler2DFlags _swSampler = Sampler2DFlags::None;
    WgrDepthMode _swDepth = WGR_DEPTH_TEST_WRITE;

    // Cascaded-shadow state
    ShadowMapTuning _smTuning;
    // Foliage lighting knobs (docs/foliage-translucency-plan.md), pushed via PushRenderParams.
    FoliageSettings _foliage;
    AoSettings _ao;
    MaterialDebugSettings _materialDebug;
    MaterialDebugInfo _materialDebugInfo;
    // First-slice guard: Material Debug applies only to the explicit test RVMAT
    // identity supplied by automation, never incidentally to the rest of a map.
    std::string _materialDebugRvmatMatch;
    // MAT-LEGACY: optional modern materials for original OFP/CWA assets, which carry no RVMAT at
    // all. On when an enhancement pack may be present; every lookup simply misses without one.
    bool _legacyEnhancement = true;
    // Texture name -> enhancement normal handle, 0 meaning "no enhancement exists". Misses are
    // cached too: stock content misses on every section, every frame.
    std::unordered_map<std::string, uint64_t> _legacyEnhancementCache;
    uint64_t LegacyEnhancementNormal(const char* textureName);
    // Source material semantics are immutable for a loaded mission. Cache them
    // by virtual path so production draws do not reopen and parse an RVMAT for
    // every section on every frame.
    std::unordered_map<std::string, Asset::Material::TranslatedMaterial> _materialBindings;
    // A section's albedo when its RVMAT supplies one, or 0. The per-draw path has
    // always done this inline; the GPU-driven retained set needs the same answer,
    // so the lookup lives here rather than being duplicated.
    // `fromConstant`, when given, reports whether the returned albedo came from a PROCEDURAL
    // CONSTANT stage rather than a real image. The caller needs that to decide precedence
    // against the section's own face texture -- see MAT-045 at the call site.
    uint64_t ResolveMaterialBaseColour(const ShapeSection& section, bool* fromConstant = nullptr);
    // MAT-050. The handle of the 1x1 neutral stand-in described at `_untexturedAlbedo`,
    // creating it on first call, or 0 to leave the section on the white bindless slot 0.
    //
    // 0 is the DEFAULT answer: `WGR_UNTEXTURED_ALBEDO` is unset in a shipped run and the
    // correction is off, so pixels are bit-identical to pre-MAT-050. `sectionHasMaterial`
    // is the scope guard -- true only when the section NAMES an RVMAT that then failed to
    // supply an albedo. A section with no authored material is the original 2001 case
    // (colour in the folded TLMaterial, white texel as the multiply identity) and is never
    // corrected unless `WGR_UNTEXTURED_ALBEDO=all` asks for it, so no shipped CWA pixel can
    // move even when the knob is on.
    //
    // Announces its own state on the first untextured section of a run, whether on or off,
    // so a capture with no visible change can distinguish "nothing was untextured" from
    // "the lever never fired" -- see the announcement's twin in rust/src/textures.rs.
    uint64_t UntexturedAlbedoHandle(bool sectionHasMaterial);
    // A section's per-texel specular map (RVMAT SpecularDetail) when its RVMAT names
    // a loadable one, or 0.
    uint64_t ResolveMaterialSpecularMap(const ShapeSection& section);
    // A section's tangent-space normal map (RVMAT Stage1) when its RVMAT names a
    // loadable one, or 0. Same reason as the two above: the per-draw path bound it
    // and the retained set, which owns nearly every world section, did not.
    uint64_t ResolveMaterialNormalMap(const ShapeSection& section);
    // A section's environment (reflection) map when its material names a loadable one,
    // or 0. DZ-003: DayZ water is authored as a near-black constant plus a reflection,
    // so a non-zero answer here is what marks a surface reflective for the shader.
    uint64_t ResolveMaterialEnvironmentMap(const ShapeSection& section);
    // Whether a section carries a Fresnel-weighted environment reflection: its material's
    // shader family is CalmWater (the DayZ water stub, which ships with no `.emat` at all
    // on Chernarus) and nothing else. See the definition for why "names an EnvironmentMap"
    // was measured and rejected as a trigger.
    bool MaterialIsReflective(const ShapeSection& section);
    // Multi's mask and its three further layer colours, with each layer's own UV
    // transform. Fills nothing and returns false for any other family.
    bool ResolveMaterialLayers(const ShapeSection& section, WgrModelMaterial& material);
    // True when the material's Multi mask stage is authored on uvSource "tex1".
    bool MaterialMaskUsesUv1(const ShapeSection& section);
    // Fraction of a translated RVMAT's flat specular constant to apply, standing in
    // for a per-texel specular map that is not bound. 1.0 for anything that is not a
    // translated RVMAT, and 1.0 once `specularMap` is non-zero -- the map is what the
    // constant was authored to be scaled by, so the approximation steps aside.
    float MaterialSpecularScale(const ShapeSection& section, uint64_t specularMap);
    // Interior sky visibility (docs/interior-sky-visibility-plan.md), pushed via PushRenderParams.
    InteriorSkySettings _interiorSky;
    GrassSettings _grass;
    // Whether the photo-tuft path is live. It decides how far grass casts shadows: the mid ring
    // is submitted to the shadow pass ONLY on that path (Grass::draw_shadow), so without it the
    // reach is the near ring's radius. Read when building frame.shadow.grass_cascade_mask.
    bool _grassPhotoTuftActive = false;
    // A circular history of terrain contacts.  Kept in engine space so foot
    // and vehicle trails persist even though grass placement is camera-relative.
    std::array<WgrGrassTrack, WGR_GRASS_TRACK_COUNT> _grassTracks{};
    size_t _nextGrassTrack = 0;
    Vector3 _lastGrassTrackPos = VZero;
    float _grassTrackSampleTime = 0.0f;
    bool _haveGrassTrackPos = false;
    float _smSunFactor = 1.0f;
    bool _smEnabledFrame = false;
    shadow::CascadeSet _smCascades;
    int _smCascadeRes = 0;
    bool _smCascadesValid = false;
    std::vector<WgrShadowCaster> _shadowCasters;

    // PUSH-FRAME BACKING (Phase 5 step 6, blocker 4).
    //
    // The fifteen vectors above and the `cameras` vector built inside NextFrame are the
    // frame the renderer consumes. `WgrFrame` holds non-owning `WgrSlice`s into them, so
    // while a render thread is reading a frame the producer must not be refilling the same
    // storage -- and `cameras` is worse than the rest, because it is a LOCAL whose lifetime
    // ends when NextFrame returns. That is fine while the call is synchronous and is a
    // dangling pointer the moment it is not.
    //
    // Publishing swaps the producer's vectors into a ring slot and points the frame at the
    // SLOT. The producer then refills the vectors it swapped out -- which still own their
    // capacity, so this costs no allocation churn -- and the consumer reads storage nobody
    // is writing.
    //
    // TWO SLOTS, for the same reason the presentation-snapshot ring has two: two makes the
    // writer's target disjoint from the reader's source, which is the property worth having.
    // A third only pays once the producer may run AHEAD of the consumer, and it would absorb
    // the "published without being consumed" event that 5.3 asks to keep visible. Raise it
    // when the threads split and the counters show the drop, not before.
    struct PushFrameBacking
    {
        std::vector<WgrVertex2D> verts;
        std::vector<WgrDraw2DBatch> batches;
        std::vector<WgrDraw3D> draws3d;
        std::vector<WgrCmd> cmds;
        std::vector<WgrMat4> palette;
        std::vector<WgrLight> lights;
        std::vector<WgrShadowCaster> shadowCasters;
        std::vector<WgrOverlayVertex> overlayVerts;
        std::vector<uint16_t> overlayIndices;
        std::vector<WgrOverlayDraw> overlayDraws;
        std::vector<WgrTerrainNode> terrainNodes;
        std::vector<WgrTerrainBatch> terrainBatches;
        std::vector<WgrWaterNode> waterNodes;
        std::vector<WgrWaterBatch> waterBatches;
        std::vector<WgrGrassBatch> grassBatches;
        std::vector<WgrCamera> cameras;

        // Blocker 3, closed here rather than by locking the map. `NextFrame` needs to know
        // whether the retained set can cast shadows, and asked `_gpuInstances.empty()` --
        // an unordered_map the SIMULATION thread inserts into and erases from. A concurrent
        // `size()` against an `insert` is undefined behaviour, and no per-entry generation
        // touches that. So the producer records the count it already knows at publish time
        // and the consumer never sees the container. The liveness sweep that walks the same
        // map stays with the producer, after publish, because it is residency bookkeeping
        // and not rendering.
        uint32_t retainedInstances = 0;
    };
    static constexpr int kPushFrameRing = 2;
    PushFrameBacking _pushRing[kPushFrameRing];
    int _pushSlot = 0;

    // REN-THR-006 -- the settings half of the producer/consumer handoff (REN-THR-001
    // shape 1). Every wgr_set_* the producer used to call mid-frame now computes its exact
    // FFI payload at the moment the setter runs and parks it here; NextFrame pushes the
    // dirty ones at ONE point, which is where the render thread's frame will begin. Last
    // write wins inside a frame -- byte-for-byte what back-to-back direct calls did,
    // because each call overwrote the previous renderer state anyway.
    //
    // Payloads, not inputs: the computation (clamps, env gates, catch-all scaling) stays
    // in the setter where it can read producer state (GScene, _terrain, Glob) freely.
    // Only the finished bytes cross the drain.
    struct PendingRendererSettings
    {
        bool suppressWorld = false;
        bool suppressWorldDirty = false;

        uint32_t cullDrawSpheres = 0, cullDisableFrustum = 0, cullOcclusion = 0;
        bool cullDebugDirty = false;

        WgrTemporalTuning temporal{};
        bool temporalDirty = false;

        WgrGrassParams grassParams{};
        bool grassParamsDirty = false;
        float grassCardSpacing = 0.0f;
        bool grassSpacingDirty = false;

        // From PushSceneCamera: the object cull's four inputs and the far tier's knobs.
        float cullDrawDistance = 0.0f, cullCamLeft = 0.0f, cullLodInvWidth = 0.0f, cullPixelLimit = 0.0f;
        bool cullParamsDirty = false;
        float farNearCutoff = 0.0f, farDistance = 0.0f, farPixelLimit = 0.0f;
        uint32_t farEnabled = 0;
        bool farParamsDirty = false;
    };
    PendingRendererSettings _pendingSet;
    // The drain. Called once at the top of NextFrame; must tolerate a null _renderer.
    void ApplyPendingRendererSettings();

    // REN-THR-007 -- the instance half of the handoff (REN-THR-001 shape 2). The producer no
    // longer holds renderer slots at all: it holds ITS OWN handles, allocated here, and every
    // add/update/remove is a queued op executed in FIFO order at the drain, where the handle
    // is translated to the renderer slot the queued add obtained.
    //
    // FIFO order is what makes handle reuse sound: Remove(h) frees h at ENQUEUE time, so a
    // later Add in the same frame may get h back -- and at the drain the remove still
    // translates through the OLD slot before the add overwrites the mapping. Reordering the
    // queue would break exactly this, which is why there is one vector and not three.
    //
    // The renderer's own slot generations (ID-3) stay untouched underneath: a bug here shows
    // up as the stale_instance_ops ANOMALY line, not as silent corruption.
    struct InstanceOp
    {
        enum Kind : uint8_t
        {
            Add,
            Update,
            Remove
        };
        Kind kind;
        uint32_t handle;
        WgrInstance inst; // Add / Update payload; unused for Remove
    };
    std::vector<InstanceOp> _instanceOps;
    std::vector<uint32_t> _instSlotOf;       // producer handle -> renderer slot (valid after drain)
    std::vector<uint32_t> _instFreeHandles;  // recycled producer handles
    uint32_t EnqueueInstanceAdd(const WgrInstance& inst);
    void EnqueueInstanceUpdate(uint32_t handle, const WgrInstance& inst);
    void EnqueueInstanceRemove(uint32_t handle);
    void DrainInstanceOps();

    // REN-THR-008 -- shape 3, the RETAINED-model half: mesh creates and destroys, crown
    // centres and model registration cross the frame as queued ops with producer handles,
    // translated at the drain. Two things made this possible (REN-THR-001): Pool::alloc's
    // only failure is empty input, which the producer checks itself, and the crown-centre
    // BASE -- which is baked into vertex data -- is applied at the drain, where the real
    // base exists, via the op's crownComp offsets.
    //
    // Deliberately NOT covered: the CPU-path VertexBufferWgpu meshes (their handles ride
    // inside WgrDraw3D payloads and their per-frame wgr_mesh_update is upload traffic, not
    // bookkeeping -- the right thread answer there is a genuinely Send upload path, not a
    // queue that copies vertex arrays), and the overlay textures (UI, same story).
    struct ResourceOp
    {
        enum Kind : uint8_t
        {
            MeshCreate,
            MeshDestroy,
            ModelRegister
        };
        Kind kind = MeshCreate;
        uint64_t mesh = 0;  // MeshCreate: the producer handle being defined; MeshDestroy: the one dying
        uint32_t model = 0; // ModelRegister: the producer handle being defined
        float sphereRadius = 0.0f;
        std::vector<SVertex> verts;        // MeshCreate (moved in, zero-copy)
        std::vector<VertexIndex> indices;  // MeshCreate
        std::vector<WgrVec4> crownCentres; // MeshCreate, optional: register, then patch
        std::vector<uint32_t> crownComp;   //   verts[i].conform = base + crownComp[i]
        std::vector<WgrModelLod> lods;             // ModelRegister
        std::vector<WgrModelSection> sections;     // ModelRegister; .mesh holds PRODUCER handles
        std::vector<WgrModelMaterial> materials;   // ModelRegister (texture handles baked, see cpp)
        std::string name;
    };
    std::vector<ResourceOp> _resourceOps;
    uint64_t _nextMeshHandle = 1;  // 0 stays "no mesh", matching the FFI's failure value
    uint32_t _nextModelHandle = 0; // WGR_INVALID_MODEL is the sentinel and is never allocated
    std::unordered_map<uint64_t, uint64_t> _meshIdOf;  // producer -> renderer (post-drain)
    std::unordered_map<uint32_t, uint32_t> _modelIdOf; // producer -> renderer (post-drain)
    void DrainResourceOps();

    // REN-THR-001 asked how often the producer mutates renderer state outside NextFrame,
    // and answered "unmeasured" twice, in the two places where the answer decides the
    // design. These count it. A mean alone would hide the shape -- streaming is bursty by
    // nature, and a queue is sized by its worst frame, not its average one -- so the peak
    // is carried alongside and reported with its denominator in frames.
    struct MutationOps
    {
        uint32_t instanceAdd = 0;
        uint32_t instanceUpdate = 0;
        uint32_t instanceRemove = 0;
        uint32_t meshCreate = 0;
        uint32_t meshDestroy = 0;
        uint32_t modelRegister = 0;
        uint32_t textureOps = 0;
        uint32_t settingsPush = 0;

        void Add(const MutationOps& o)
        {
            instanceAdd += o.instanceAdd;
            instanceUpdate += o.instanceUpdate;
            instanceRemove += o.instanceRemove;
            meshCreate += o.meshCreate;
            meshDestroy += o.meshDestroy;
            modelRegister += o.modelRegister;
            textureOps += o.textureOps;
            settingsPush += o.settingsPush;
        }
        void Max(const MutationOps& o)
        {
            instanceAdd = std::max(instanceAdd, o.instanceAdd);
            instanceUpdate = std::max(instanceUpdate, o.instanceUpdate);
            instanceRemove = std::max(instanceRemove, o.instanceRemove);
            meshCreate = std::max(meshCreate, o.meshCreate);
            meshDestroy = std::max(meshDestroy, o.meshDestroy);
            modelRegister = std::max(modelRegister, o.modelRegister);
            textureOps = std::max(textureOps, o.textureOps);
            settingsPush = std::max(settingsPush, o.settingsPush);
        }
    };
    // Phase 5 step 6 wants lockstep equivalence PROVEN before the producer and the renderer
    // are allowed to overlap. This is the oracle for the renderer half: the exact bytes
    // published to wgr_render_frame, hashed at the publish point. Any restructuring of the
    // producer -- the command queue, the instance table moving across the boundary, the
    // handoff itself -- is equivalent if and only if it publishes the same frames.
    //
    // It is only an oracle if the frames are reproducible in the first place, which is a
    // question about the engine, not about this code, and is answered by running it. Enabled
    // by WGR_FRAME_HASH=1 and off otherwise: it walks every published byte.
    static constexpr int kFrameHashSlices = 17; // scalars + the sixteen slices
    void HashPublishedFrame(const WgrFrame& frame);
    bool _frameHashOn = false;
    uint64_t _frameHashRun = 0;   // folded over every frame so far this run
    uint64_t _frameHashLast = 0;  // the last single frame, so a stuck value is visible
    uint32_t _frameHashCount = 0; // frames folded in
    uint32_t _frameHashDistinct = 0;
    uint32_t _frameHashFirst = 0; // WGR_FRAME_HASH_FIRST -- move the window past the load

    MutationOps _ops;      // this frame, so far
    MutationOps _opsTotal; // summed since the last report
    MutationOps _opsPeak;  // worst single frame since the last report
    uint32_t _opsFrames = 0;
    // Camera position the shadow casters were made camera-relative to, and the origin the Rust
    // side reconstructs absolute world xz from (depth-pass terrain conform) and builds every
    // shadow-cascade cull frustum around. Must match the caster world matrices, so it is NOT
    // re-read from _currentCamera at frame end (which may have advanced to a HUD/optics camera
    // during live gameplay).
    //
    // Written in AddShadowCaster (authoritative — same camera as the caster matrices beside it)
    // AND in SetShadowCascades (same camera, same call chain, one step earlier). The second
    // write exists because AddShadowCaster early-returns, so a frame with no CPU casters used to
    // leave this stale from an earlier frame — which only stopped mattering because the old
    // cascade gate skipped those frames entirely. It no longer does; see both sites.
    float _smCamPos[3] = {0.0f, 0.0f, 0.0f};

    // Dev-panel overlay for the current frame
    std::vector<WgrOverlayVertex> _overlayVerts;
    std::vector<uint16_t> _overlayIndices;
    std::vector<WgrOverlayDraw> _overlayDraws;

    // GPU terrain renderer + its per-frame node/batch buffers.
    std::unique_ptr<TerrainWgpu> _terrain;
    std::vector<WgrTerrainNode> _terrainNodes;
    std::vector<WgrTerrainBatch> _terrainBatches;

    // GPU water renderer + its per-frame node/batch buffers (null on WGR_GPU_WATER=0).
    std::unique_ptr<WaterWgpu> _water;
    std::vector<WgrWaterNode> _waterNodes;
    std::vector<WgrWaterBatch> _waterBatches;

    std::vector<WgrGrassBatch> _grassBatches;
    bool _grassSubmitted = false;
};

Engine* CreateEngineWgpu(const GraphicsEngineParams& params);

} // namespace Poseidon
