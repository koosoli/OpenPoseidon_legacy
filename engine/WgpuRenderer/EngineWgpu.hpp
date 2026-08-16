#pragma once

#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
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
    // The most recently controlled helicopter, held as a weak link so effects
    // can survive the player leaving the cockpit without risking a stale pointer.
    const Helicopter* LastGrassRotor() const;

    RString GetDebugName() const override;
    RString GetRendererName() const override;

    void HandleEvents() override { _eventWindow.HandleEvents(); }
    bool IsOpen() const override { return _eventWindow.IsOpen(); }
    void SetMouseGrab(bool grab) override { _eventWindow.SetMouseGrab(grab); }
    bool IsMouseGrabbed() const override { return _eventWindow.IsMouseGrabbed(); }

    int Width() const override;
    int Height() const override;

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
    // Smoothed celestial inputs: LightSun::Recalculate refreshes sun/moon direction,
    // night factor and fog colour only every few seconds with no interpolation, which
    // makes the sun disc + horizon haze stutter. PushSkyRuntime eases these toward the
    // live values each frame (snapping on init / large jumps). See procedural-sky-plan §9.
    bool _skyInit = false;
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
    // Fraction of a translated RVMAT's flat specular constant to apply, standing in
    // for a per-texel specular map that is not bound. 1.0 for anything that is not a
    // translated RVMAT, and 1.0 once `specularMap` is non-zero -- the map is what the
    // constant was authored to be scaled by, so the approximation steps aside.
    float MaterialSpecularScale(const ShapeSection& section, uint64_t specularMap);
    // Interior sky visibility (docs/interior-sky-visibility-plan.md), pushed via PushRenderParams.
    InteriorSkySettings _interiorSky;
    GrassSettings _grass;
    // A circular history of terrain contacts.  Kept in engine space so foot
    // and vehicle trails persist even though grass placement is camera-relative.
    std::array<WgrGrassTrack, WGR_GRASS_TRACK_COUNT> _grassTracks{};
    size_t _nextGrassTrack = 0;
    Vector3 _lastGrassTrackPos = VZero;
    float _grassTrackSampleTime = 0.0f;
    bool _haveGrassTrackPos = false;
    // A weak link remains valid while a recently exited helicopter still
    // exists, and becomes null automatically if the vehicle is deleted.
    LLink<Helicopter> _lastGrassRotor;
    float _smSunFactor = 1.0f;
    bool _smEnabledFrame = false;
    shadow::CascadeSet _smCascades;
    int _smCascadeRes = 0;
    bool _smCascadesValid = false;
    std::vector<WgrShadowCaster> _shadowCasters;
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
