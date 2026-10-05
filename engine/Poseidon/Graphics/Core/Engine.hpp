#ifdef _MSC_VER
#pragma once
#endif

#ifndef __ENGINE_HPP
#define __ENGINE_HPP

#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Graphics/Rendering/Shape/RegistrationCostPolicy.hpp>

#include <cstdlib> // RFG-089: env seed for GrassSettings::enfusionBlades
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>
#include <Poseidon/Graphics/Rendering/RenderFlags.hpp>
#include <Poseidon/Graphics/Rendering/RenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ClipShape.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/IGraphicsEngine.hpp>

#include <Poseidon/Foundation/Containers/Array.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <Poseidon/Graphics/Core/RenderState.hpp> // DrawItem (for GetRecordedDraws())

// Forward-decl for `EmitDraw` — the frame layer's `Draw` value (full param-pack
// for `glDrawElements`).  Keeps the frame-layer header out of the Core
// base; the GL33 override pulls it in `.cpp`.

namespace Poseidon
{
struct WarmTextureRead;
namespace GeometryPages { struct SourceIdentity; }

// Per-frame draw-call counter — incremented at the GL emission seams,
// consumed + reset by World::Simulate's FrameProfiler EndFrame.
extern int gPerfDrawCalls;
namespace render
{
namespace frame
{
struct Draw;
}
} // namespace render
namespace shadow
{
struct CascadeSet;
}

class Counter
{
  private:
    int _count;

  public:
    Counter() { _count = 0; }
    void operator+=(int a) { _count += a; }
    operator int() const { return _count; }
    void Reset() { _count = 0; }
    int Count() const { return _count; }
};

#define PERF_STATS 1

struct TextInfo
{
    int _handle;
    DWORD _hideTime;
    Ref<Font> _font;
    PackedColor _color;
    float _size;      // size - relative to default size
    float _x, _y;     // relative position
    Temp<char> _text; // remmember text

    TextInfo() {}
    TextInfo(int handle, Engine* engine, DWORD hideTime, Font* font, PackedColor color, float size, float x, float y,
             const char* text);
    TextInfo(const TextInfo& src);
    TextInfo& operator=(const TextInfo& src);
};

struct Draw2DPars
{
    MipInfo mip; // which texture
    PackedColor colorTL, colorTR, colorBL, colorBR;
    void SetColor(PackedColor c) { colorTL = colorTR = colorBL = colorBR = c; }
    int spec;                 // which specflags are used
    float uTL, vTL, uTR, vTR; // u,v range
    float uBL, vBL, uBR, vBR; // u,v range
    void SetU(float u0, float u1) { uTL = uBL = u0, uTR = uBR = u1; }
    void SetV(float v0, float v1) { vTL = vTR = v0, vBL = vBR = v1; }
    void Init();
};

} // namespace Poseidon
#include <Poseidon/Foundation/Strings/Mbcs.hpp>
namespace Poseidon
{

class FontCache
{
    // remmember chars to avoid loading/unloading too often
    struct CachedChar
    {
        Ref<Texture> _texture;
        Font* _font; // will be removed when font is destroyed
        RStringB _c;
    };

    AutoArray<CachedChar> _lastChars;
    RefArray<Font> _fonts;

  public:
    Font* Load(FontID id);
    Texture* Load(Font* font, RStringB name);
    void RemoveFont(Font* font);
    void Clear();
    // Re-resolve each cached Font's FreeType renderer pointer against the
    // active mapping table.  Used by the SCROLL LOCK debug toggle so existing
    // Font instances pick up the swapped renderer without dangling.
    void RefreshAllFonts();
};

struct ResolutionInfo
{
    int w, h, bpp;
    bool operator==(const ResolutionInfo& info) const { return w == info.w && h == info.h && bpp == info.bpp; }
};

struct MonitorInfo
{
    int index;    // SDL display ID / index
    RString name; // Friendly name from the OS (e.g. "DELL U2723QE")
    int w, h;     // Native resolution (current desktop mode)
    int refresh;  // Current refresh rate
};

} // namespace Poseidon
#include <Poseidon/Graphics/Shared/WindowMode.hpp>
namespace Poseidon
{

const int DefSpecFlags2D = NoZBuf | IsAlpha | ClampU | ClampV | IsAlphaFog;

struct Char3DContext
{
    Vector3 dir;
    Vector3 up;
    Font* font;
    Object* obj;
    float z2;
    float x1c;
    float x2c;
    float y1c;
    float y2c;
    ClipFlags clip;
    int spec;
};

//! logical viewport (viewport containing usefull information) settings
struct AspectSettings
{
    //@{ wide screen settings (ratio world to screen)
    float leftFOV = 1.0f;
    float topFOV = 0.75f;
    //}@
    //@{ 2D UI region settings (0..1 range)
    float uiTopLeftX = 0.0f, uiTopLeftY = 0.0f;
    float uiBottomRightX = 1.0f, uiBottomRightY = 1.0f;
    //@}
    //@{ 3D world render rect as fractions of the full window.  Default
    // (0,0,1,1) = full window.  A centered sub-rect crops the world
    // (pillarbox / manual noodle); the FOV matches its aspect so objects
    // keep their size, and the periphery is left black.
    float worldLeft = 0.0f;
    float worldTop = 0.0f;
    float worldRight = 1.0f;
    float worldBottom = 1.0f;
    //@}
};

//@{
/*!\name 2D coordinate system
Various systems of 2D coordinates.
*/

//! position of point on screen in pixels (absolute)
/*!
Onscreen range: x = <0,GEngine-Width()), y = <0,GEngine->Height())
*/
struct Point2DAbs
{
    float x, y;
    Point2DAbs() {}
    Point2DAbs(float xx, float yy) : x(xx), y(yy) {}
};

//! 2d rectangle
struct Rect2DAbs
{
    float x, y, w, h; // rectangle
    Rect2DAbs() {}
    Rect2DAbs(float xx, float yy, float ww, float hh) { x = xx, y = yy, w = ww, h = hh; }
    Rect2DAbs(const Point2DAbs& pos, float ww, float hh) { x = pos.x, y = pos.y, w = ww, h = hh; }
};
struct Line2DAbs
{
    Point2DAbs beg, end;
    Line2DAbs() {}
    Line2DAbs(float x0, float y0, float x1, float y1) { beg.x = x0, beg.y = y0, end.x = x1, end.y = y1; }
};
//! uses same coordinate system as Point2DPixel and Rect2DPixel
struct Vertex2DAbs : Point2DAbs
{
    float z, w;        // screen coordinates
    float u, v;        // texture coordinates
    PackedColor color; // color

    Vertex2DAbs() { z = 0.5f, w = 1.0f; }
};

//! default clipping rectangle
extern Rect2DAbs Rect2DClipAbs;

//! position of point in viewport in pixels
/*!
Insideviewport range: x = <0,GEngine->Width2D()), y = <0,GEngine->Height2D())
*/
struct Point2DPixel
{
    float x, y;
    Point2DPixel() {}
    Point2DPixel(float xx, float yy) : x(xx), y(yy) {}
};
//! position of rectangle in viewport in pixels
struct Rect2DPixel
{
    float x, y, w, h; // rectangle
    Rect2DPixel() {}
    Rect2DPixel(float xx, float yy, float ww, float hh) { x = xx, y = yy, w = ww, h = hh; }
};

struct Line2DPixel
{
    Point2DPixel beg, end;
    Line2DPixel() {}
    Line2DPixel(float x0, float y0, float x1, float y1) { beg.x = x0, beg.y = y0, end.x = x1, end.y = y1; }
};
//! uses same coordinate system as Point2DPixel and Rect2DPixel
struct Vertex2DPixel : Point2DPixel
{
    float z, w;        // screen coordinates
    float u, v;        // texture coordinates
    PackedColor color; // color

    Vertex2DPixel() { z = 0.5f, w = 1.0f; }
};

extern Rect2DPixel Rect2DClipPixel;

//! position of point on screen in 2D viewport coordinates
/*!
Insideviewport range: x = <0,1), y = <0,1)
*/
struct Point2DFloat
{
    float x, y;
    Point2DFloat() {}
    Point2DFloat(float xx, float yy) : x(xx), y(yy) {}
};
//! position of rectangle on screen in 2D viewport coordinates
struct Rect2DFloat
{
    float x, y, w, h; // rectangle
    Rect2DFloat() {}
    Rect2DFloat(float xx, float yy, float ww, float hh) { x = xx, y = yy, w = ww, h = hh; }
};

struct Line2DFloat
{
    Point2DFloat beg, end;
    Line2DFloat() {}
    Line2DFloat(float x0, float y0, float x1, float y1) { beg.x = x0, beg.y = y0, end.x = x1, end.y = y1; }
};

//@}

class ITerrainRenderer;
class IWaterRenderer;

// How much of an object the wgpu GPU-driven retained scene draws (§12 of
// docs/gpu-culling-and-depth-plan.md). Only EngineWgpu (WGR_GPU_DRIVEN) ever reports
// anything but None; every other backend leaves objects on the CPU path.
enum class GpuDrawCoverage
{
    None,    // not GPU-driven — the CPU draws the whole object as usual
    Full,    // the GPU owns every drawn section and the shape has no proxies — skip the CPU draw
    Partial, // the GPU owns the opaque sections; the CPU still draws the complement (proxies,
             // blend/decal sections) with GSkipGpuOwnedSections set so it never repaints the owned geometry
};

class Engine : public IGraphicsEngine
{
  protected:
    int _messageHandle;
    int _textHandle;

    Color _fogColor;
    Color _accomodateEye; // color filter
    float _usrBrightness; // user brightness control
    int _shadowFactor;    // alpha values used for full shadows - from 0 to 255

    float _avgBrightness; // average screen brightness
    bool _nightVision;
    bool _multitexturing;
    render::PassKindHint _passKindHint = render::PassKindHint::None; // explicit cockpit pass routing
    // REN-TEMP-001H: identity of the object whose draws are currently being submitted
    // (the scene draw loop brackets each object; see SetDrawObject below). Same
    // non-virtual-hint shape as _passKindHint — no vtable slot, backends that do not
    // care never read it.
    const void* _drawObjectHint = nullptr;
    int _showFps;
    AspectSettings _aspectSettings;

    Ref<Font> _showTextFont; // actual parameters for ShowText and ShowTextF
    PackedColor _showTextColor;
    float _showTextSize;

    AutoArray<TextInfo> _texts;
    FontCache _fonts;

    DWORD _frameTime, _frameTime0; // last frame stats
    DWORD _startTime;
    DWORD _lastFrameDuration;   // duration of last frame (in ms)
    DWORD _startGame;           // time the game started
    uint32_t _frameCounter = 0; // total frames rendered (incremented in FinishDraw)

    enum
    {
        NFrameDurations = 16
    };
    DWORD _frameDurations[NFrameDurations];

  public:
    void ToggleFps(int state) { _showFps = state; }

    // get stats to be able to scale

    DWORD GetLastFrameDuration() const { return _lastFrameDuration; }
    DWORD GetAvgFrameDuration(int nFrames = 8) const;
    DWORD GetTimeStartGame() const { return _startGame; }
    void SetTimeStartGame(DWORD time) { _startGame = time; }
    void ResetFrameDuration();
    uint32_t GetFrameCounter() const { return _frameCounter; }

    void SetNightVision(bool state) { _nightVision = state; }
    bool GetNightVision() const { return _nightVision; }

    bool IsMultitexturing() const { return _multitexturing; }
    void SetMultitexturing(bool set);

    void SetAspectSettings(const AspectSettings& set) { _aspectSettings = set; }
    void GetAspectSettings(AspectSettings& get) const { get = _aspectSettings; }

    virtual bool IsWBuffer() const { return false; }
    virtual bool CanWBuffer() const { return false; }
    virtual void SetWBuffer(bool val) {}

    ColorVal GetAccomodateEye() const { return _accomodateEye; } // color filter

    virtual void EnableNightEye(float night) {}

    int ShowFps() const { return _showFps; }
    void CCALL ShowMessage(int timeMs, const char* fmt, ...);

    void SetFogColor(ColorVal fogColor);
    ColorVal FogColor() { return _fogColor; }

    void SetShadowFactor(int shadowFactor) { _shadowFactor = shadowFactor; }
    int GetShadowFactor() const { return _shadowFactor; }

    /// Day/night strength of the shadow-MAP shadows in [0,1]: 1 in full daylight,
    /// fading through dusk to 0 at night (sun below the horizon casts no sun shadow,
    /// matching the projected path + OFP/ArmA/FP). The Scene computes it each frame
    /// from the sun's NightEffect; the lit shaders fade the shadow darkness by it.
    virtual void SetShadowMapSunFactor(float /*factor01*/) {}

  private:
    Engine(const Engine& src); // no copy
    void operator=(const Engine& src);

  public:
    Engine();
    ~Engine() override;

    virtual bool IsAbleToDraw() { return true; }
    void Clear(bool clearZ = true, bool clear = true, PackedColor color = PackedColor(0)) override = 0;
    void DrawFinishTexts();
    virtual void InitDraw(bool clear = false, PackedColor color = PackedColor(0)); // Begin scene
    virtual void FinishDraw();                                                     // End scene
    virtual void DrawTestPattern(const char* /*name*/) {} // Harness-only: draw named test pattern
    virtual void NextFrame();                             // swap frames - get ready for next frame
    virtual bool InitDrawDone() { return true; }
    void Pause() override = 0;   // stop and prepare everything for GDI
    void Restore() override = 0; // restore after minimized - before app goes to fullscreen
    virtual void StopAll() {}    // stop all background activity - used before termination
    // Drop all GPU resources tied to game content (textures, shaders, buffers)
    // and rebuild the GL infrastructure, keeping the window + device alive. Used
    // by the in-process mod re-mount; default no-op for headless backends.
    virtual void ResetForRemount() {}

    // --- GPU-driven retained scene (docs/gpu-culling-and-depth-plan.md Stage 3b) ---
    // Notifications from the landscape/world so a GPU-driven backend can keep a
    // GPU-resident retained scene (register the shape's model once, stream instance
    // deltas). Default no-op: only EngineWgpu with WGR_GPU_DRIVEN implements them; every
    // other backend and the flag-off wgpu path ignore them and keep the CPU draw path.
    // `SceneObjectCreated` fires when a drawable object enters the world (static clutter
    // load or spawn), `Removed` before it leaves, `Moved` after its transform changes.
    // `GpuDrivenObject`/`GpuDrivenCoverage(obj)` report how much of `obj` the GPU path draws, so
    // the scene draw loop can suppress the CPU colour draw entirely (Full) or only the GPU-owned
    // sections (Partial — the CPU still paints the complement + proxies). Shadow casters stay on
    // the CPU path regardless. NOTE: `GpuDrivenObject` MUST keep its original vtable slot here;
    // the tri-state `GpuDrivenCoverage` + `GpuDrivenProxy` are appended at the class end instead
    // (see the vtable-slot note on SuppressWorldObjects) — inserting them here shifts every
    // later Engine virtual and breaks ccache partial recompiles (3D-UI misdispatch).
    virtual void SceneObjectCreated(Object* /*obj*/) {}
    virtual void SceneObjectRemoved(Object* /*obj*/) {}
    virtual void SceneObjectMoved(Object* /*obj*/) {}
    // Any GPU involvement at all (Full or Partial); delegates to GpuDrivenCoverage (class end).
    virtual bool GpuDrivenObject(const Object* obj) const { return GpuDrivenCoverage(obj) != GpuDrawCoverage::None; }

    void FogColorChanged(ColorVal fogColor) override = 0;

    bool SwitchRes(int w, int h, int bpp) override = 0; // switch to resolution nearest to w,h
    bool SwitchRefreshRate(int refresh) override = 0;   // switch to resolution nearest to w,h
    RString GetDebugName() const override = 0;
    RString GetRendererName() const override = 0;

    void ListResolutions(FindArray<ResolutionInfo>& ret) override = 0;
    void ListRefreshRates(FindArray<int>& ret) override = 0;
    void SetGamma(float g) override = 0;
    float GetGamma() const override = 0;

    void SaveConfig();
    void LoadConfig();

    virtual void SetBrightness(float v) { _usrBrightness = v; }
    virtual float GetBrightness() const { return _usrBrightness; }

    // MSAA alpha-to-coverage on cutout (alpha-test) draws — grades
    // sub-pixel cutout features (fence wire, foliage) across the MSAA
    // samples instead of the hard per-fragment alpha test keeping or
    // killing whole pixels.  No-op on backends without MSAA.
    virtual void SetAlphaToCoverage(bool /*enable*/) {}
    virtual bool GetAlphaToCoverage() const { return false; }

    // Diagnostic: replace object shading with a flat solid colour (red), keeping
    // the alpha-test silhouette + cutout holes.  A highlight that vanishes under
    // flat colour is a shading/texture artifact; one that persists is a
    // geometry/vertex-position artifact.  No-op on backends that don't support it.
    virtual void SetDebugFlatColor(bool /*enable*/) {}
    virtual bool GetDebugFlatColor() const { return false; }

    // SSAA render scale: render the frame at scale x window size into an
    // offscreen target and downsample to the window.  The only general cure
    // for sub-pixel OPAQUE geometry sparkle (fence bars, wires), which
    // alpha-to-coverage cannot touch and MSAA only dampens.  1.0 = off.
    virtual void SetRenderScale(float /*scale*/) {}
    virtual float GetRenderScale() const { return 1.0f; }

    // MSAA sample count on the frame render target (0/1 = no multisampling).
    virtual void SetMsaaSamples(int /*samples*/) {}
    virtual int GetMsaaSamples() const { return 0; }

    // Instanced-run mode: the scene batches a sorted run of
    // identical static shapes; backends that support it draw every TL section
    // once with K instances. Defaults keep unsupporting backends scalar
    // (InstancedRunAdd refusing = the scene never arms a batch).
    virtual void InstancedRunReset() {}
    virtual bool InstancedRunAdd(const Matrix4& /*modelToWorld*/) { return false; }
    virtual void BeginInstancedRunUpload() {}
    virtual bool EndInstancedRun() { return true; }

    // Explicit pass-kind routing.  Producers (Man::DrawProxies for first-person,
    // vehicle cockpit draw, etc.) wrap a draw scope with `SetPassKindHint(Cockpit)`
    // / `ClearPassKindHint()` so the descriptor build picks the cockpit `PassKind`
    // family explicitly rather than inferring it from `NoDropdown` bit
    // propagation.  Defaults to `None`, where the descriptor falls back to
    // `NoDropdown` inference.
    render::PassKindHint GetPassKindHint() const { return _passKindHint; }
    void SetPassKindHint(render::PassKindHint hint) { _passKindHint = hint; }
    void ClearPassKindHint() { _passKindHint = render::PassKindHint::None; }

    // REN-TEMP-001H: per-object draw bracket, set/cleared by the scene draw loop around
    // one complete object (proxies included — Object::Draw re-enters itself on proxy
    // Objects, which are SHAPE-shared across every placement of a model, so the parent's
    // identity must stand for the whole scope). The wgpu backend maps this pointer to a
    // stable small id for motion vectors; other backends never read it.
    const void* GetDrawObject() const { return _drawObjectHint; }
    void SetDrawObject(const void* obj) { _drawObjectHint = obj; }
    void ClearDrawObject() { _drawObjectHint = nullptr; }

    // Typed form — callers split a legacy int via `render::SplitLegacy` at the
    // boundary; backends read whichever category they care about.
    virtual void PrepareTriangleTL(const MipInfo& mip, const render::LegacySpec& spec) {}
    void PrepareTriangle(const MipInfo& mip, int specFlags) override = 0;
    void DrawPolygon(const VertexIndex* i, int n) override = 0;
    void DrawSection(const FaceArray& face, Offset beg, Offset end) override = 0;

    virtual void EnableReorderQueues(bool enableReorded) {}
    virtual void FlushQueues() {}

    // Shadow pipeline.  Wraps the per-caster shadow draw loop in scene.cpp:
    //   BeginShadowPass()   — color writes off, stencil REPLACE 0xFF
    //                          ALWAYS.  Each shadow draw stamps the
    //                          stencil buffer (alpha-cutout discard
    //                          via PSShadow's discard).  Idempotent
    //                          across overlapping casters.
    //   ...per-caster shadow draws...
    //   EndShadowPass()     — color writes on, stencil EQUAL 0xFF +
    //                          KEEP, draw fullscreen quad
    //                          (1-shadowFactor) blend.  Single uniform
    //                          darken regardless of overlap, replaces
    //                          the per-poly INCR/EQUAL-0 dance.
    virtual void BeginShadowPass() {}
    virtual void EndShadowPass() {}

    // integrated transform&lighting
    virtual bool GetTL() const { return false; }
    virtual bool GetTLOnSurface() const { return false; } // can TL path handle OnSurface (roads)?
    virtual bool HasWBuffer() const { return false; }     // far plane important

    // Only the material category is read by `SetMaterial` (currently just
    // `DisableSun`); the rest of the triplet is accepted for symmetry with the
    // other Engine virtuals.
    virtual void SetMaterial(const TLMaterial& mat, const LightList& lights, const render::LegacySpec& spec) {}
    virtual void EnableSunLight(bool enable) {}

    virtual void UpdateFrameCamera() {
    } // re-upload frame UBO with current GScene camera (needed when camera changes mid-frame)
    virtual void UpdateProjection() {} // re-upload only viewProj matrix (for clip range changes without affecting fog)
    virtual void PrepareMeshTL(const LightList& lights, const Matrix4& modelToWorld, const render::LegacySpec& spec) {
    } // prepare internal variables
    virtual void BeginMeshTL(const Shape& sMesh, int spec, bool dynamic = false) {} // convert all mesh vertices
    virtual void EndMeshTL(const Shape& sMesh) {}                                   // forget mesh
    virtual void DrawSectionTL(const Shape& sMesh, int beg, int end) {}

    virtual int HowLongIdle() { return 0; }
    virtual size_t GetDrawItemCount() const { return 0; }
    // Lifetime-of-process count of HIGH-severity GL/driver errors
    // (KHR_debug `GL_DEBUG_SEVERITY_HIGH` etc.).  the frame layer's ValidateFrame
    // reads this each frame; non-zero is a runtime invariant violation
    // Default 0 for backends without
    // driver-level validation.
    virtual unsigned int GetDebugErrorCount() const { return 0; }
    // Most recent HIGH-severity debug message string captured by
    // the engine's debug-callback.  the frame validator includes this in the I-20
    // violation detail so the log line is actionable on its own;
    // empty string for engines without a debug callback.
    virtual std::string GetLastDebugMessage() const { return {}; }
    // Returns the per-frame DrawItem record at the current point in
    // the frame.  Cleared by the engine each frame.  the frame layer's
    // SceneExtractor reads this at end-of-frame to bucket draws into
    // SceneInputs.  Default empty pointer for backends that don't
    // record draws.
    virtual const std::vector<DrawItem>* GetRecordedDraws() const { return nullptr; }
    // Debug-group markers annotating pass boundaries in GPU captures
    // (RenderDoc, Nsight).  EngineGL33 forwards to glPushDebugGroup /
    // glPopDebugGroup when the function pointers are loaded and emits
    // them at the real pass transitions (BeginPass / BeginScreenPass);
    // headless / test engines no-op.  Strings must be null-terminated
    // and live until EndDebugGroup is called.
    virtual void BeginDebugGroup(const char* /*name*/) {}
    virtual void EndDebugGroup() {}

    // Emit a single indexed draw via the backend's GL path.  Called
    // inline at `DrawSectionTL` with a non-zero VAO / index count
    // (the TL path) once the per-draw state has landed.
    // Implementation issues `glBindVertexArray(d.mesh.vao)` +
    // world-matrix upload + TEXTURE0 + TEXTURE1 binds +
    // `glDrawElements`.  Default no-op so headless / test engines
    // link without graphics; the typed `Draw` parameter is taken by
    // reference and only dereferenced inside the override, so the
    // forward-decl above is sufficient at this seam.
    virtual void EmitDraw(const render::frame::Draw& /*d*/) {}

    // Live GL viewport rect (x, y, width, height).  The frame validator reads
    // this to confirm the engine's recorded viewport at extract time matches
    // what `glGetIntegerv(GL_VIEWPORT)` reports at the observation seam.
    // Returns false on backends without a real GL state to query (dummy /
    // headless).
    virtual bool GetGLViewport(int outRect[4]) const
    {
        (void)outRect;
        return false;
    }
    void DrawDecal(Vector3Par pos, float rhw, float sizeX, float sizeY, PackedColor col, const MipInfo& mip,
                   int specFlags) override = 0; // 3D rectangle
    // Optional texture-free emitter halo; caller has already checked source visibility.
    virtual bool DrawLightGlow(Vector3Par, float, float, ColorVal, bool) { return false; }
    void Draw2D(const Draw2DPars& pars, const Rect2DAbs& rect,
                const Rect2DAbs& clip = Rect2DClipAbs) override = 0; // 2D rectangle
    virtual void Draw2D(const Draw2DPars& pars, const Rect2DPixel& rect, const Rect2DPixel& clip = Rect2DClipPixel)
    {
        Rect2DAbs rectA, clipA;
        Convert(rectA, rect);
        Convert(clipA, clip);
        Draw2D(pars, rectA, clipA);
    }

    void DrawPoly(const MipInfo& mip, const Vertex2DAbs* vertices, int nVertices, const Rect2DAbs& clip = Rect2DClipAbs,
                  int specFlags = DefSpecFlags2D) override = 0;
    void DrawPoly(const MipInfo& mip, const Vertex2DPixel* vertices, int nVertices,
                  const Rect2DPixel& clip = Rect2DClipPixel, int specFlags = DefSpecFlags2D) override = 0;
    void DrawLine(const Line2DAbs& rect, PackedColor c0, PackedColor c1,
                  const Rect2DAbs& clip = Rect2DClipAbs) override = 0; // 2D line
    virtual void DrawLine(const Line2DPixel& rect, PackedColor c0, PackedColor c1,
                          const Rect2DPixel& clip = Rect2DClipPixel)
    {
        Line2DAbs rectA;
        Rect2DAbs clipA;
        Convert(rectA, rect);
        Convert(clipA, clip);
        DrawLine(rectA, c0, c1, clipA);
    }
    void DrawLine(int beg, int end) override = 0; // 3D line - width in m
    void Draw2D(const MipInfo& mip, PackedColor color, const Rect2DAbs& rect,
                const Rect2DAbs& clip = Rect2DClipAbs) // wrapper to keep old interface working
    {
        Draw2DPars pars;
        pars.mip = mip;
        pars.SetColor(color);
        pars.Init();
        // call wrapped function
        Draw2D(pars, rect, clip);
    }
    void Draw2D(const MipInfo& mip, PackedColor color, const Rect2DPixel& rect,
                const Rect2DPixel& clip = Rect2DClipPixel) // wrapper to keep old interface working
    {
        Rect2DAbs rectA, clipA;
        Convert(rectA, rect);
        Convert(clipA, clip);
        Draw2DPars pars;
        pars.mip = mip;
        pars.SetColor(color);
        pars.Init();
        // call wrapped function
        Draw2D(pars, rectA, clipA);
    }
    virtual void DrawPoints(int beg, int end) {} // 3D points

    void PrepareMesh(const render::LegacySpec& spec) override = 0;                    // prepare internal variables
    void BeginMesh(TLVertexTable& mesh, const render::LegacySpec& spec) override = 0; // convert all mesh vertices
    void EndMesh(TLVertexTable& mesh) override = 0;                                   // forget mesh

    AbstractTextBank* TextBank() override = 0; // texture management

    virtual VertexBuffer* CreateVertexBuffer(const Shape& src, VBType type) { return nullptr; }
    virtual int CompareBuffers(const Shape& s1, const Shape& s2) { return 0; }

    // shadow related functions
    float ZShadowEpsilon() const override = 0; // bias used for shadows
    float ZRoadEpsilon() const override = 0;   // bias used for roads
    float ObjMipmapCoef() const override = 0;  // pixel size multiplier
    void GetZCoefs(float& zAdd, float& zMult) override = 0;
    int GetBias() override = 0;
    void SetBias(int value) override = 0;

    virtual void SetGrassParams(float a1, float a2, float a3 = 0, float a4 = 0) {}
    // Sinkhole W1 (from Malprave terrainHole2): terrain pixels inside these world X/Z convex areas are not drawn
    // (the ground is cut away there by a hole-cutting object). edges: nEdges x {nx, nz, d, last}; inside when
    // nx*x + nz*z + d >= 0 for every edge of one area, last = 1 on an area's final edge. nEdges = 0 switches it off.
    // Called with the set before the terrain draws and with nullptr after them; a backend whose hole state is
    // bound only to its terrain passes (wgpu) may ignore the nullptr reset.
    virtual void SetTerrainHoles(const float* /*edges*/, int /*nEdges*/) {}
    /// Stamp a local, recovering bend into procedural grass (for explosions and impacts).
    virtual void AddGrassImpact(Vector3Par /*position*/, float /*radius*/) {}
    virtual bool CanGrass() const { return false; }

    bool CanZBias() const override = 0;
    bool ZBiasExclusion() const override = 0;

    //@{ 2D viewport dimensions
    int Width2D() const;
    int Height2D() const;
    int Top2D() const;
    int Left2D() const;
    //@}

    //@{ 2D viewport conversions
    void Convert(Point2DAbs& to, const Point2DPixel& from);
    void Convert(Point2DAbs& to, const Point2DFloat& from);
    void Convert(Point2DPixel& to, const Point2DAbs& from);
    void Convert(Point2DFloat& to, const Point2DAbs& from);

    void Convert(Rect2DAbs& to, const Rect2DPixel& from);
    void Convert(Rect2DAbs& to, const Rect2DFloat& from);
    void Convert(Rect2DPixel& to, const Rect2DAbs& from);
    void Convert(Rect2DFloat& to, const Rect2DAbs& from);

    void Convert(Line2DAbs& to, const Line2DPixel& from);
    void Convert(Line2DAbs& to, const Line2DFloat& from);
    void Convert(Line2DPixel& to, const Line2DAbs& from);
    void Convert(Line2DFloat& to, const Line2DAbs& from);
    //@}

    void PixelAlignXY(Point2DAbs& pos);
    void PixelAlignX(Point2DAbs& pos);
    void PixelAlignY(Point2DAbs& pos);
    void PixelAlignXY(Point2DPixel& pos);
    void PixelAlignX(Point2DPixel& pos);
    void PixelAlignY(Point2DPixel& pos);

    float PixelAlignedX(float x);
    float PixelAlignedY(float x);

    // general
    int Width() const override = 0;
    int Height() const override = 0;
    int PixelSize() const override = 0; // 16 or 32 bit mode?
    int RefreshRate() const override = 0;
    bool CanBeWindowed() const override = 0;
    bool IsWindowed() const override = 0;
    bool IsResizable() const override = 0;

    virtual int MinGuardX() const { return 0; } // used for guard band clipping
    virtual int MaxGuardX() const { return Width(); }
    virtual int MinGuardY() const { return 0; }
    virtual int MaxGuardY() const { return Height(); }

    virtual int MinSatX() const { return 0; } // used for saturation
    virtual int MaxSatX() const { return Width(); }
    virtual int MinSatY() const { return 0; }
    virtual int MaxSatY() const { return Height(); }

    int AFrameTime() const override = 0;

    void FontDestroyed(Font* font);

#ifndef ACCESS_ONLY
    void TextureDestroyed(Texture* tex) override = 0;

    // 3D texture drawing
    void Draw3D(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, PackedColor color, int spec,
                Texture* tex, float x1c = 0, float y1c = 0, float x2c = 1, float y2c = 1);
    void DrawLine3D(Vector3Par start, Vector3Par end, PackedColor color, int spec);

    // text drawing
    Font* LoadFont(FontID id);
    void RefreshAllFonts() { _fonts.RefreshAllFonts(); }
    // Release every Ref<Texture> the FontCache holds.  Called by derived
    // backends (EngineGL33::ShutdownGuard) *before* the texture bank is
    // destroyed so the FontCache's per-glyph texture refs don't dangle.
    void ClearFontCache() { _fonts.Clear(); }
    void DrawText3D(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, Font* font, PackedColor color,
                    int spec, const char* text, float x1c = 0, float y1c = 0, float x2c = 1e6, float y2c = 1);
    void CCALL DrawText3DF(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, Font* font, PackedColor color,
                           int spec, const char* text, ...);
    Vector3 GetText3DWidth(Vector3Par dir, Font* font, const char* text);
    Vector3 CCALL GetText3DWidthF(Vector3Par dir, Font* font, const char* text, ...);
    void DrawText(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text);
    void DrawText(const Point2DAbs& pos, float size, Font* font, PackedColor color, const char* text);
    void DrawText(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font, PackedColor color,
                  const char* text);
    void DrawText(const Point2DAbs& pos, float size, const Rect2DAbs& clip, Font* font, PackedColor color,
                  const char* text);
    void DrawTextVertical(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text);
    void DrawTextVertical(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font, PackedColor color,
                          const char* text);
    float GetTextWidth(float size, Font* font, const char* text);
    int GetTextPosition(float x, float size, Font* font, const char* text);

    void CCALL DrawTextF(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text, ...);
    void CCALL DrawTextF(const Point2DAbs& pos, float size, Font* font, PackedColor color, const char* text, ...);
    void CCALL DrawTextF(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font, PackedColor color,
                         const char* text, ...);
    void CCALL DrawTextVerticalF(const Point2DFloat& pos, float size, Font* font, PackedColor color, const char* text,
                                 ...);
    void CCALL DrawTextVerticalF(const Point2DFloat& pos, float size, const Rect2DFloat& clip, Font* font,
                                 PackedColor color, const char* text, ...);
    float CCALL GetTextWidthF(float size, Font* font, const char* text, ...);
#endif

    void ShowFont(Font* font, PackedColor color = PackedColor(0xff000000), float size = 1.0);
    void RemoveText(int handle);
    int ShowText(DWORD timeToLive, int x, int y, const char* text);
    int CCALL ShowTextF(DWORD timeToLive, int x, int y, const char* text, ...);

    void ReinitCounters();

    // give opportunity to react to window changes
    virtual void Activate() {}
    virtual void Deactivate() {}
    virtual void Resize(int x, int y, int w, int h) {}

    virtual void Screenshot(RString filename) {}
    virtual void FlushPendingScreenshot() {}

    /// Read back a small sample of pixels from the back buffer.
    /// Returns the number of non-black pixels found in the sample.
    /// Default implementation returns -1 (not supported).
    virtual int SampleBackBufferNonBlack() { return -1; }

    /// Read back a single pixel from the back buffer at integer coords (top-left origin).
    /// Writes R, G, B into outRGB[0..2]. Returns true on success, false if not supported
    /// or out of range. Used by the trident harness for visual regression checks.
    virtual bool SamplePixel(int /*x*/, int /*y*/, uint8_t* /*outRGB*/) { return false; }

    /// Render `vertCount` triangle vertices (3 floats each, GL_TRIANGLES) from the
    /// light into an offscreen depth FBO at `res`x`res`, given a column-major light
    /// view-projection (16 floats), and read the depth back into `outDepth`
    /// (`res*res` floats, [0,1], row 0 = bottom). Returns false if unsupported.
    /// Validates the GL shadow-depth path against the CPU reference (shadow-maps Phase C).
    virtual bool ShadowDepthProbe(const float* /*lightVP16*/, const float* /*triXYZ*/, int /*vertCount*/, int /*res*/,
                                  float* /*outDepth*/)
    {
        return false;
    }

    /// Self-test: run a one-cascade shadow-map depth pass and report whether it
    /// invalidated the pipeline pass-dedup cache, so a later lit draw re-applies
    /// its own cull instead of inheriting the depth pass's cull::Front. Returns
    /// true on backends without the cache (nothing to leak).
    virtual bool ShadowMapCacheSelfTest() { return true; }

    /// Runtime-tunable knobs for the cascaded-shadow path. The dev panel / tri
    /// verbs drive these so the look can be tuned by eye without a rebuild, and
    /// each maps 1:1 to a kernel input. `darkness` multiplies the lit colour
    /// where shadowed (lower = darker). `cascadeCount` is the number of view-
    /// frustum slices (1..4). `distanceCoef` sets the shadow far distance as a
    /// fraction of the view distance (shadowFar = near + coef·(far−near));
    /// `shadowDistance` overrides that `far` with an explicit metre reach
    /// decoupled from the 250 m `shadowsZ` clamp (0 = use `shadowsZ`).
    /// `splitCoef` is the PSSM log/uniform blend (0 = uniform, 1 = logarithmic).
    /// `biasBase` is the per-cascade depth bias base (applied base·(i+1)²).
    /// `fadeRange` is the far-edge fade width in metres (distant shadows dissolve
    /// instead of cutting off). `resolution` is the per-cascade depth-map size.
    struct ShadowMapTuning
    {
        bool enabled = true;
        float darkness = 0.35f;
        int cascadeCount = 4;
        float distanceCoef = 1.00f; // shadows reach the full view distance (frustum tiers)
        // Explicit cascade far distance in metres, decoupled from the 1..250 m
        // `shadowsZ` serialize clamp (the legacy view-distance ceiling). 0 = fall
        // back to `ENGINE_CONFIG.shadowsZ` (legacy behaviour); > 0 overrides it so
        // the shadow path can push object shadows past 250 m without touching the
        // saved game menu slider. Default 400 m — a moderate reach for the current
        // 4 cascades; the 8-cascade rework is what makes ~1 km affordable.
        // `distanceCoef` still scales this reach.
        // Overridable from the environment so a shadow-reach A/B is one run and not
        // one rebuild; the dev panel's own slider (0..1500 m) still wins at runtime.
        static float DefaultShadowDistance()
        {
            static const float value = []
            {
                const char* v = std::getenv("POSEIDON_SHADOW_DISTANCE");
                const float parsed = (v != nullptr && *v != 0) ? static_cast<float>(std::atof(v)) : 0.0f;
                // Raised from 400 on 2026-09-04 after an A/B on Everon: at 400 a forested
                // ridge 600 m out is flat green -- the trees are drawn, they simply stop
                // casting, and the middle distance loses all of its depth. This is an
                // engine-wide default, not a Reforger one; every world with objects past
                // 400 m was losing the same thing. The cost is honest: 4 cascades now span
                // three times the depth, so near texels are coarser and contact shadows
                // soften, and the 8-cascade rework is what makes this free rather than a
                // trade. The dev panel slider and this variable both override it.
                return (parsed > 0.0f && parsed <= 4000.0f) ? parsed : 1200.0f;
            }();
            return value;
        }
        float shadowDistance = DefaultShadowDistance();
        float splitCoef = 0.80f;
        float biasBase = 0.00002f; // small — front-face culling does the acne work
        float fadeRange = 40.0f;
        int resolution = 2048;
        // Leading tiers as camera-centred spheres (omniCoef* radii as a fraction
        // of the shadow range). Mode 0 uses pure frustum slices: a sphere spends
        // most of its area on directions the camera can't see (only in-frustum
        // receivers matter; upsun casters are covered by the fit's depth pad),
        // so slices put several times more texels on visible shadows. The default
        // uses one omni tier for near all-direction caster coverage.
        int omniCount = 1;
        float omniCoef0 = 0.08f;
        float omniCoef1 = 0.20f;
        // Casters re-select their LOD as if this many times farther than they
        // are. 1.0 = cast exactly the drawn LOD — the default, because a
        // coarser caster's simplified surfaces sit slightly off the visible
        // ones and paint false self-shadows at grazing sun angles. Raise to
        // trade that accuracy for depth-pass throughput.
        float casterLodBias = 1.0f;
        // Receiver normal-offset scale (multiplies the ~2-world-texel
        // ShadowBias push toward the light). wgpu path only.
        float normalOffset = 1.0f;
        // PCF spread in texels: < 0.5 = single hardware bilinear tap (crisp),
        // >= 0.5 = 4 taps spread by this many texels (soft). wgpu path only.
        float pcf = 1.0f;
        // Shared WGPU CSM receiver experiment. 0=fixed, 1=PCSS, 2=budget PCSS.
        int contactShadows = 2;
        float sunAngularRadius = 0.266f; // physical solar radius in degrees
        // Long-distance terrain sun-shadow (heightfield self-shadow) — a compute
        // sweep ray-marches the heightmap toward the sun into a world-aligned mask
        // the terrain samples, giving terrain-on-terrain occlusion at any range
        // (the cascade maps never cast terrain). Complements CSM by max(). wgpu
        // path only. `terrainShadowStrength` scales the occlusion (0 = off, 1 =
        // physical, >1 = exaggerated); `terrainShadowScale` supersamples the mask
        // over the heightmap grid (sharper edges); `terrainShadowSteps` caps the
        // march range (steps * terrain_grid); `terrainShadowPenumbra` is the
        // soft-edge half-width in degrees.
        bool terrainShadowEnabled = true;
        float terrainShadowStrength = 1.0f;
        int terrainShadowScale = 2;
        int terrainShadowSteps = 512;
        float terrainShadowPenumbra = 1.0f;

        // Terrain sky-visibility (sky-view factor) ambient occlusion — the AO complement to the
        // sun-shadow above: it darkens the AMBIENT (sky) term in valleys/gorges/cove-water/cliff-
        // bases, where little sky is visible, on terrain + objects + water. Orthogonal to the sun-
        // shadow (which removes the DIRECT sun). wgpu path only. `Strength` scales the effect
        // (0 = off), `Floor` keeps a minimum ambient in fully-occluded columns; `Radius` is the
        // horizon-scan reach (m) and `Azimuths` its direction count — changing either re-runs the
        // (cheap, cached) CPU scan. `Debug` shows the raw factor as greyscale. Default OFF pending
        // look validation. See docs/sky-visibility-ambient-plan.md.
        bool terrainSkyVisEnabled = true;
        float terrainSkyVisStrength = 0.70f;
        float terrainSkyVisContrast = 6.5f;
        float terrainSkyVisFloor = 0.30f;
        float terrainSkyVisRadius = 600.0f;
        int terrainSkyVisAzimuths = 12;
        int terrainSkyVisDownsample = 2;
        bool terrainSkyVisDebug = false;
    };

    /// Shadow-map (depth-buffer) shadows — durable replacement for the projected
    /// path. Default OFF; enabling it makes the
    /// scene render a depth pass from the sun and the lit shaders sample it.
    virtual void SetShadowMapsEnabled(bool /*enabled*/) {}
    virtual bool ShadowMapsEnabled() const { return false; }

    /// Read / replace the full shadow-map tuning set (see ShadowMapTuning).
    /// Default base returns an all-default set; only the GL33 backend stores it.
    virtual ShadowMapTuning GetShadowMapTuning() const { return {}; }
    virtual void SetShadowMapTuning(const ShadowMapTuning& /*tuning*/) {}

    /// Foliage lighting — emulated leaf subsurface scattering for alpha-tested vegetation, so
    /// the low-poly cards don't split into a hard lit/dark pair at harsh sun angles. wgpu path
    /// only. See docs/foliage-translucency-plan.md. Defaults are modest and ON so the effect is
    /// visible for tuning; zero the strengths to disable.
    struct FoliageSettings
    {
        // Defaults dialled in by eye against the scene (2026-07-12); tune live on the Foliage tab.
        float transScale = 0.54f;  // transmission strength — the dark-side / backlit lift (0 = off)
        float distortion = 0.49f;  // transmission light-dir bend toward the normal (0..1)
        float transPower = 5.1f;   // transmission lobe tightness (higher = tighter backlit glow)
        float wrap = 0.5f;         // front terminator-wrap fill (0 = hard Lambert; lit side unchanged)
        // Sky-irradiance multiplier for foliage (1 = off), distance-faded.
        //
        // 2.5 until 2026-08-29, which was tuned against CWA's small trees and washed a dense
        // Arma-2/DayZ-era crown flat: with 2.5x sky on every leaf card the canopy has no lit
        // side left to lose. The owner's call after looking at both: on DayZ a boost of 0
        // reads BEST -- those crowns carry their own depth and want no lift at all -- but 0
        // is worse on OFP's sparse trees, which do need the fill. 1.6 is the compromise
        // that keeps OFP whole, and it is a compromise, not an optimum for either.
        //
        // If a per-world or per-generation split ever lands, DayZ-like content should take
        // something near 0 and OFP content the old lift. LANDED for native Enfusion worlds:
        // EngineWgpu pushes 1.0 there (measured on Everon: 1.6 flattens dense crowns, 1.0
        // restores their modeling); legacy worlds keep this configured value.
        float ambientBoost = 1.6f;
        float normalBend = 0.8f;   // BUSH spherical-normal blend (0 = geometric, 1 = full radial)
        float crownYOffset = 0.27f; // BUSH crown-centre Y lift (lifts the crown up into the canopy)
        float fillFadeEnd = 500.0f; // distance (m) by which the SSS fill + ambient boost fade (0 = never)
        // Cheap GI: scale foliage ambient by the terrain's light level (1 - terrain shadow) so
        // shadowed foliage stops glowing. 0 = off; residual at full shadow is (1 - giStrength).
        float giStrength = 0.44f;
        // Spherical normals for TREES (leaf sections only; the solid trunk keeps its normal). Trees
        // pick their own knobs — the bounding-sphere centre already sits up in the canopy, so the
        // crown lift ends up slightly negative (tuned by eye).
        float treeBend = 0.7f;     // TREE spherical-normal blend (0 = geometric)
        float treeCrownY = -0.52f; // TREE crown-centre Y lift
        // -- VEG-SWAY: geometric wind on vegetation MODELS (wgpu only) ---------------
        // Until this existed the wgpu renderer animated GRASS and nothing else: neither
        // object vertex shader carried a time-varying term, so a forest stood dead still in
        // a gale while the grass under it rippled. These drive both object paths.
        //
        // The wind VECTOR is not authored here — it comes from the one authority
        // (World/Weather/WindModel, the same sample the grass field and cloud deck read).
        // What is authored is the response curve, exactly as the design note in
        // WindModel.hpp intends ("This model replaces the source; consumers keep their own
        // response curves").
        //
        // Amplitude is quoted in METRES OF TIP TRAVEL at a 12 m reference canopy height
        // (VEG_SWAY_HEIGHT_REF in frame.wgsl) at the reference wind speed below, so one
        // number reads sensibly on a 1 m bush and a 25 m spruce.
        float swayStrength = 0.35f;  // 0 = OFF (and the whole term is branched out)
        float swaySpeed = 1.0f;      // rate multiplier on the trunk oscillation
        float swayLeafFlutter = 1.0f; // extra fast flutter on cutout (leaf) sections only
        // Height exponent. 1 = a linear lean (the whole trunk slides), >1 keeps the lower
        // trunk planted so the tree bends about its base instead of sliding sideways.
        float swayStiffness = 1.6f;
        // Wind speed (m/s) at which `swayStrength` is the literal tip travel. The live
        // sample scales the amplitude by speed/this, clamped, so a calm day barely stirs.
        float swayWindReference = 6.0f;
        bool swayUseLiveWind = true; // false = hold the authored direction and a steady breeze
    };

    /// Read / replace the foliage lighting knobs (see FoliageSettings). Default base returns
    /// an all-default set; only the wgpu backend stores + pushes it.
    virtual FoliageSettings GetFoliageSettings() const { return {}; }
    virtual void SetFoliageSettings(const FoliageSettings& /*s*/) {}

    /// Screen-space ambient occlusion (GTAO) — the SHORT-RANGE complement to the terrain
    /// sky-visibility AO above. Sky-vis is baked, positional and km-scale: it darkens gorges and
    /// cliff-bases and structurally cannot see a rock, a wheel or a doorway. GTAO works from the
    /// depth+normal prepass, so it resolves exactly that band — local folds and the contact
    /// between objects and the ground. The two occlude independently and are multiplied.
    ///
    /// Applied to the AMBIENT term only, on terrain + opaque objects; water is untouched.
    /// wgpu path only. Default ON since 2026-08-05 (owner call after smoke testing alongside
    /// LIT-020); its GPU cost is now measured — see the Amb. Occlusion tab's timer rows, which
    /// were added in the same change specifically so a default-on feature is not also an
    /// unmeasured one. See docs/screen-space-ao-plan.md.
    // Debug-only controls for the incremental Arma RVMAT path. This canonical
    // state is shared by Dev Tools and automation; it never mutates imported data.
    struct MaterialDebugSettings
    {
        // These values are uploaded as integers to both object shaders. Keep the order stable:
        // changing it silently repoints an existing capture or automation setting.
        enum class View : int { FullMaterial = 0, BaseColorOnly, NormalMap, AmbientShadow, SpecularGloss, UvSource,
                                // Shade with a WHITE albedo: the output IS the lighting multiplier the
                                // surface is being handed. Pairs with BaseColorOnly -- between them,
                                // albedo and lighting are separated, which is the one measurement that
                                // says whether an over-bright surface is a texture or a light.
                                LightingOnly };
        View view = View::FullMaterial;
        bool disableNormalMap = false;
        // Diagnostic only: compare the two tangent-space Y conventions under
        // fixed lighting; this never redefines source material data.
        bool invertNormalY = false;
        // Suppress the SMDI / specular-gloss map (RVMAT Stage2 or its family equivalent):
        // the section shades with its flat specular constant alone, which is what it did
        // before the map was bound. Read per fragment from the frame UBO like the others.
        // There is no detail, macro or ambient-shadow switch because the renderer binds no
        // such stage; three checkboxes that once promised them are gone rather than dormant.
        bool disableSpecularGloss = false;
        bool disableFresnelEnvironment = false;
        // MAT-039: the Multi family's three further surface layers. On by default --
        // it costs one mask fetch plus three texture fetches on layered sections only,
        // and without it those surfaces render as their first layer alone. Off is the
        // A/B that shows what the composition contributes.
        bool composeMultiLayers = true;
        // MAT-LEGACY: the optional `<texture>_nohq.paa` enhancement for original OFP/CWA assets,
        // which have no RVMAT of their own. Off turns every legacy section back to its 2001
        // shading, which is the only way to see the enhancement's actual contribution A/B.
        bool legacyEnhancement = true;
        // MAT-NET-001: a Blend-classified texture whose histogram is really a punch-through
        // COVERAGE MASK with antialiased edges (netting, chain-link, barbed wire, camo net)
        // is drawn as a depth-WRITING alpha test instead of a translucent blend. On by
        // default; off restores the blend and, with it, the missing depth -- which is the
        // A/B for "things behind transparent netting look strange", because the temporal
        // upscaler's velocity and the screen-space AO both come from that depth buffer.
        // Per-draw and CPU-side, so it takes effect on the next frame with no reload.
        // OFF, and the reason is the owner's: "Zaun hat eh nie probleme gemacht, nur das
        // kamu netz". The rule was measured on chain-link, which was never the complaint,
        // and it has NOT been verified on a camo net -- a very different histogram (27%
        // fully opaque texels against a fence's ~1%), where a hard 0.5 discard could
        // plausibly look worse. Everon carries no camo net at all; the only ones in the
        // game folder are the Arma 3 Stratis shelters. Until it is looked at through one,
        // this ships off rather than changing something that worked.
        // 2026-10-03, looked at: an M2 inside a Fortress1 on Everon, shot through its camo net
        // (maskovaci_sit_new) from all four sides. Off, the net renders as scattered flecks and
        // the gun and anyone behind it read as unobstructed -- the owner's "the AI soldier on
        // the other side is transparent". On, it is a full camo cover with the gun visible only
        // through the real gaps. Default ON; POSEIDON_NET_DEPTH=0 restores the old path.
        bool coverageMaskDepth = true;
        // RFG-047: hand Enfusion's BC7/BC5/BC4 textures to the GPU COMPRESSED instead of
        // decoding them to 32-bit. On by default when the adapter supports it (every
        // D3D12-class GPU does); off restores the RFG-016 decode.
        //
        // This one is NOT a per-fragment switch like the rest of this struct. It changes
        // what is UPLOADED, so it applies to textures loaded after the flip -- and to the
        // section flag that tells the shader which channels a compressed normal map uses,
        // which is baked when a model registers. Reload the world (or relaunch, with the
        // value persisted to graphics.cfg) for a clean A/B; the panel says so.
        bool compressedEnfusionTextures = true;
        // RFG-070: rescale Reforger's SHARED one-metre layer tiles to the per-layer
        // `Color_N` the material authored, instead of binding the raw tile. 436 distinct
        // tiles serve 2,889 of Everon's structure materials, which is why unrelated walls
        // came out the same pale grey; 90.3% of those materials say what colour they
        // should be and the native path threw it away. On by default.
        //
        // Like the switch above and unlike the rest of this struct, this is NOT a
        // per-fragment switch. It decides the texture NAME the world loader hands the
        // renderer, so it applies to what is loaded after the flip: reload the world (or
        // relaunch, with the value persisted to graphics.cfg) for a clean A/B. Inert on
        // every non-Enfusion world -- nothing else builds these names.
        bool enfusionLayerTint = true;
        // RFG-071: and WHAT that colour means. On, `Color_N` multiplies the tile in linear
        // space, which is what a tint physically is and what Enfusion's own baked albedos
        // measure as (median 1.29x the authored mean over 35 paired materials, against
        // 1.82x for the rule below). Off restores RFG-070's rescale-the-mean, which is
        // the A/B. Load-time in the same way, and for a stronger reason: it decides the
        // TEXELS a tinted tile is uploaded with, so relaunch (the value persists to
        // graphics.cfg) rather than trusting a reload to re-decode a cached texture.
        bool enfusionTintMultiply = true;
        // RFG-072: feed the four-layer blend shader from a NATIVE MatPBRMulti as the material
        // describes itself -- mask on the `.xob`'s second UV set, each layer's tile worn in
        // its own `Color_N` -- instead of one flattened image. Registration-time like the
        // two above (the material record is baked when a shape registers): reload the
        // world for a clean A/B. Inert on every non-Enfusion world.
        bool enfusionMultiLayers = true;
    };
    struct MaterialDebugInfo
    {
        bool active = false;
        bool normalResolved = false;
        RString rvmatPath;
        RString shaderFamily;
        RString normalTexturePath;
        RString normalUvSource;
        RString normalStatus;
    };
    virtual bool SupportsMaterialDebug() const { return false; }
    virtual MaterialDebugSettings GetMaterialDebugSettings() const { return {}; }
    virtual void SetMaterialDebugSettings(const MaterialDebugSettings& /*s*/) {}
    virtual MaterialDebugInfo GetMaterialDebugInfo() const { return {}; }

    struct AoSettings
    {
        bool enabled = true;
        // Occlusion reach in WORLD metres, projected to pixels per fragment (so AO does not
        // swell as you walk toward a wall). ~1-2 m reads as contact shadow; larger reads as
        // soft global shading.
        float radius = 2.0f;
        // Exponent on visibility: 1 = the physical result, >1 deepens without crushing to black.
        float strength = 1.0f;
        // Per-frame sample budget. There is NO temporal accumulation in this engine (no TAA),
        // so these have to be enough on their own; the bilateral blur below is the only denoise.
        // Raise steps before widening the blur — a too-wide blur washes out the contact
        // darkening that is the whole point.
        int slices = 3;
        int steps = 12;
        // Cost bound: screen radius clamp in pixels. NOT a free knob — whenever it bites it
        // silently shortens `radius` above, so too low a value reads as "AO does nothing" on
        // everything close to the camera (measured: 96 px gave a 0.5 m radius at 3 m, not 1.5)
        // AND makes surfaces BRIGHTEN as you walk toward them, since the shortfall grows with
        // proximity. Raise it before suspecting anything else.
        float maxRadiusPixels = 512.0f;
        // Falloff past the radius. Rejects thin foreground occluders, which otherwise shadow
        // everything behind them out to infinity (GTAO's classic "sky behind a pole goes black").
        float thickness = 1.0f;
        // Bilateral denoise: half-width in taps, plus the depth / normal rejection strengths that
        // stop AO bleeding across silhouettes and creases.
        float blurRadius = 6.0f;
        float blurDepthScale = 24.0f;
        float blurNormalPower = 8.0f;
        // Stage 2: sample the sky irradiance along the bent normal (the average direction light
        // still reaches this pixel from) rather than the surface normal. This is what gives a
        // shaded surface near an occluder some form instead of a flat wash; it changes WHERE
        // light comes from, not just how much. Separate toggle so it can be backed out without
        // losing the scalar AO.
        bool bentNormal = true;
        // Highest depth mip the horizon march may climb. 0 = every tap at full resolution.
        // Default 0 because raising it FLICKERS while the camera moves: which surface wins a
        // coarse min-reduction changes abruptly as geometry enters the block, and there is no
        // temporal filter here to absorb it. Raising it extends reach close to surfaces at that
        // cost. Measured, not assumed — both the snapped and the blended variants were worse.
        int maxMip = 0;
        // Raw buffer view on opaque surfaces: 0 = off, 1 = AO as greyscale, 2 = bent normal as
        // RGB. Tune against this rather than through sun + ambient + fog + tonemap. Mode 2 exists
        // because mode 1 shows only the scalar term, which made the bent normal impossible to
        // inspect — it changed nothing in the debug view and everything in the lit one.
        int debugMode = 0;
    };

    /// Read / replace the screen-space AO knobs (see AoSettings). Default base returns an
    /// all-default set; only the wgpu backend stores + pushes it.
    virtual AoSettings GetAoSettings() const { return {}; }
    virtual void SetAoSettings(const AoSettings& /*s*/) {}

    /// Interior sky visibility (LIT-020) — the LONG-RANGE, geometry-aware complement to the two
    /// AO terms above, and the only one that can tell the renderer it is INDOORS. Terrain
    /// sky-vis knows the heightfield only, so a building is invisible to it; GTAO reaches ~2 m
    /// and cannot see a roof that is off-screen. This renders a top-down orthographic depth map
    /// of the object scene and attenuates the sky AMBIENT under it, toward a floor.
    ///
    /// Direct sun and local lights are untouched: the cascade shadow maps and the terrain
    /// sun-shadow mask already occlude the sun, and the local lights are what keep an interior
    /// readable. wgpu path only. Default OFF. See docs/interior-sky-visibility-plan.md.
    struct InteriorSkySettings
    {
        // Both stages default ON as of 2026-08-05 (owner call). They compose: the per-frame maps
        // cover what a per-model volume structurally cannot see — terrain under a building, one
        // object roofing another, movers inside a room — while the baked volumes give a
        // building's own surfaces edges that follow its geometry.
        bool enabled = true;
        // Depth-map edge in texels, and HALF the world box it covers in metres. Together these
        // set the resolving power: 1024 texels over a 256 m box is 25 cm per texel, which is
        // enough for roofs and walls but NOT for window reveals (that is Stage 2's per-model
        // bake, not something a bigger number here fixes — the box has to follow the camera).
        int resolution = 2048;
        float extent = 64.0f;
        // How far above and below the camera the box reaches. Must clear the tallest roof the
        // player can stand under and the deepest floor they can stand on.
        float height = 300.0f;
        // 0 = inert, 1 = full attenuation.
        float strength = 1.0f;
        // Minimum ambient multiplier in a sealed volume. NOT a nicety: OFP interiors carry very
        // few local lights, so an unfloored version of this is a black box you cannot play in.
        float floorLevel = 0.32f;
        // Softening kernel radius in metres — roughly how far light appears to reach in past an
        // opening. This is what grades a porch instead of drawing a hard line at the doorway.
        float kernel = 1.0f;
        // Depth bias in metres. Stops a surface that is its own highest geometry (open ground, a
        // crate in the street) from occluding itself.
        float bias = 0.25f;
        // How far to steer the sky-irradiance lookup toward the direction light actually arrives
        // from. 0 = uniform dimming (a room just gets darker); 1 = fully along the open
        // direction.
        //
        // DEFAULT 0 after smoke testing (2026-08-05). Steering was meant to make a room read as
        // lit THROUGH its window rather than evenly dimmed, and it does — but with only five
        // sampled directions the steered normal jumps between them across a surface, and that
        // quantisation is the hard shadow patches that got this feature parked. Turning it to 0
        // removed them, which is what identified the cause: the patches were never the depth
        // map's texel grid, they were the direction set.
        //
        // The idea is sound and is not abandoned: the BAKED path samples 41 directions, so
        // storing a direction per voxel there would steer smoothly. That is the place to bring
        // it back, not here.
        float directional = 0.0f;
        // Stage 2: apply the per-model BAKED volumes instead of the per-frame maps. The volumes
        // are produced at load time (WGR_SKY_BAKE_VOLUMES); this is the runtime switch for
        // whether shading reads them, so the two can be compared without a restart.
        bool baked = true;
        // Draw the reach factor as greyscale on opaque surfaces instead of lighting with it.
        // Shipped WITH the effect: judging this through sun + ambient + fog + tonemap is much
        // harder than looking at the buffer.
        bool debug = false;
    };

    /// Read / replace the interior sky-visibility knobs. Default base returns an all-default
    /// set; only the wgpu backend stores + pushes it.
    virtual InteriorSkySettings GetInteriorSkySettings() const { return {}; }
    virtual void SetInteriorSkySettings(const InteriorSkySettings& /*s*/) {}

    /// Measure the interior sky-visibility map WHERE THE CAMERA IS NOW, into the log.
    ///
    /// The renderer already reports the map's occluder coverage -- but exactly once, ~2 s after
    /// the map goes active, which on a streamed world is the loading screen: the object stream
    /// has admitted a fraction of the neighbourhood and the camera is not where the player will
    /// stand. That single line then reads as evidence about a building the measurement never
    /// saw. This asks again, at the pose that matters, and prints the C++ half of the answer
    /// (how many retained instances are inside the box, and which) beside it -- the pair that
    /// separates "nothing retained is in the box" from "the cull or the draw is broken".
    virtual void ProbeInteriorSkyMap() {}

    // REN-GI-001: the irradiance probe volume (hybrid GI, stage 1). Same pattern: stored on
    // the engine, folded into the render-params block by PushRenderParams. WGR_GI=0 turns it
    // off; the dev panel's GI tab and the graphics options own the rest.
    struct GiSettings
    {
        bool enabled = true;
        /// REN-GI-009: probe radiance basis. 0 = six-face ambient cube, 1 = first-order SH
        /// (a measured dead end, REN-GI-008), 2 = second-order SH. Two by default: it was
        /// better on every shaded surface measured and identical on every lit one.
        int basis = 2;
        int rays = 24;
        float weight = 1.0f;
        float interiorMix = 0.25f;
        /// REN-GI-010. A probe's rays were only ever tested against the terrain heightfield and,
        /// for UPWARD directions, the interior-sky dome maps: no lateral ray was ever tested
        /// against a wall, so a probe standing in a room drank full sky radiance from more than
        /// half the sphere and came out nearly as bright as one on the lawn. Shading then blends
        /// that over the analytic ambient at `weight` (1.0), which keeps only `interiorMix` of the
        /// per-pixel interior AO the probes replaced -- so the room's ambient went from x0.32 to
        /// x0.83 and interiors read as bright as outdoors. This attenuates each probe's sky by the
        /// dome maps' reach AT THE PROBE. 0 = the pre-fix integration exactly.
        float indoorSky = 1.0f;
        float spacing = 2.5f;
        float hysteresis = 0.3f;
        float groundAlbedo[3] = {0.22f, 0.25f, 0.16f};
        float groundGain = 1.0f;
        float wallAlbedo = 0.3f;
        float rayLength = 48.0f;
        int rsmSamples = 16;     // REN-GI-002: sun-proxy gather samples per probe (0 = off)
        float rsmRadius = 12.0f; // metres
        float rsmGain = 1.0f;
    };
    virtual GiSettings GetGiSettings() const { return {}; }
    virtual void SetGiSettings(const GiSettings& /*s*/) {}

    /// Procedural terrain grass (wgpu).  Kept separate from foliage: these values control
    /// GPU-generated ground blades, not authored alpha-tested trees or bushes.
    struct GrassSettings
    {
        bool enabled = true;
        // Half the candidate field by default. Values above one add candidates
        // on a finer placement grid rather than saturating a probability.
        float density = 0.5f;
        float spacing = 0.10f;
        // Detail radius: dense near clumps only. Mid reach is independent so a
        // bad far proxy never forces the detailed grass to stop too close.
        float radius = 41.0f;
        // Opaque mid-clump reach. Clamped above the near ring by the WGPU
        // backend, which sizes the mid placement grid from this so its spacing --
        // and therefore mid density -- stays fixed all the way out.
        //
        // The mid candidate grid remains capped at 2440 cells per side. Above
        // that budget, placement spacing preserves this reach and additional
        // requested coverage broadens the clumps instead of shrinking the ring.
        float midRadius = 500.0f;
        // Outer terrain-cover ring. 0 = off (the historical behaviour: grass
        // simply ends at the mid ring). When on it MUST stay above the mid
        // ring's reach or the far LOD's accept band is empty, so the mapping
        // in EngineWgpu floors it -- there is no silently-dead middle ground.
        // Independent terrain-cover ring; does not alter near/mid placement.
        float farRadius = 0.0f;
        // Density noise: breaks the field into thicker and thinner patches so
        // coverage is not uniform. Scale is the noise frequency (1/metres);
        // strength 0 = flat density. 0.55 reproduces the previous hardcoded
        // 0.45..1.35 coverage range.
        float densityNoiseScale = 0.131f;
        float densityNoiseStrength = 0.36f;
        // Species mix as fractions of all placed plants; grass takes whatever
        // these two leave. Chosen per clump, so weeds and flowers appear in
        // drifts rather than sprinkled evenly.
        // Keep the stable ribbon width while the near rendering unit is a blade.
        // Widening it only exposes the flat photo layer; it does not create a
        // grass clump (GRS-030 regression guard).
        float bladeWidth = 0.35f;
        // Albedo saturation about luma. 1.0 = untouched; lower desaturates the
        // whole field. Brightness is preserved, so this only pulls colour out.
        // Muted green with a little brown, matching CWA's subdued terrain.
        float saturation = 0.72f;
        // Sun-bleached patches: fraction of the field that dries toward straw, and
        // the patch size (noise frequency, 1/metres). Its own noise field, so dry
        // ground does not line up with thin ground.
        float dryPatches = 0.08f;
        float dryPatchScale = 0.003f;
        float weedPercent = 0.37f;
        float flowerPercent = 0.08f;
        // Blade silhouette variety. The four grass species shared ONE profile,
        // so a field read as the same blade repeated over and over -- the first
        // thing a tester notices standing in it. 0 reproduces that legacy look
        // exactly, 1 gives eight distinct width/height/taper profiles; the two
        // jitters spread taper and lean per blade on top of whichever is chosen.
        float shapeVariety = 1.0f;
        float taperJitter = 0.36f;
        float bendJitter = 0.26f;
        // Keep the stable procedural surface until a clump-based near path can
        // present photographed detail without turning blades into flat cards.
        // The close field uses individual opaque blades. Give those blades their
        // photographed surface detail by default; unlike tuft cards this keeps
        // a stable geometric silhouette from every viewing angle.
        float bladeTextureStrength = 1.0f;
        // Alpha cut-out cards: take the silhouette from the texture instead of
        // the quad, so one card can carry several blade shapes. This is the
        // Reforger-style approach and it buys shape variety without more
        // geometry -- but it discards, which costs the early-Z the solid blade
        // path relies on, and the widened quad adds overdraw. Off until measured.
        bool alphaCards = false;
        float alphaCutoff = 0.5f;
        float cardWiden = 1.6f;
        // How far a blade arcs over, as a multiple of its own height. Grass blades are not rigid;
        // the stock bend moved a tip 5-19 cm on a ~0.8 m blade, roughly ten degrees, which is why
        // a field of them read as spikes standing to attention. Taller blades arc further, so this
        // scales with height rather than being an absolute distance. 0 = the old rigid look.
        float bladeArch = 1.49f;
        // GRS-030: sparse multi-blade clumps; false keeps the legacy ribbons.
        bool clumpRenderer = true;
        // Local exposure trim for the photographed A3 clump cards only. It
        // deliberately leaves legacy ribbons, procedural mid grass and far LOD
        // untouched so visual tuning cannot change field-wide lighting.
        // 2026-08-27: halved from 1.25 on the owner's call -- the photographed plates were
        // reading far too bright against CWA/A1 terrain. It is a plain albedo multiplier
        // (grass.wgsl:1704, `tex.rgb * PHOTO_TONE_CORRECTION * photo_brightness * tone_gain`),
        // so this darkens the cards and nothing else. The floor of the clamp is 0.5 in BOTH the
        // Rust mirror and the shader; 0 is deliberately NOT reachable, because a zero multiplier
        // paints black clumps rather than removing them -- same geometry, same alpha, same
        // overdraw, same shadow. "No cards" is midPhotoTuft/photoTuftMix, not this.
        float photoTuftBrightness = 0.625f;
        // 0 keeps the primary photographed clump; 1 assigns all local grass
        // atlases in stable world-space patches.
        float photoTuftMix = 0.0f;
        float photoTuftPatchSize = 18.0f;
        // Photographed-card look. These four apply ONLY while midPhotoTuft is on;
        // procedural blades, the procedural mid ring and the far proxy ignore them.
        // Contrast about the plate's own mid-luma. The source photographs are
        // flatly lit, which is exactly why unmodified cards read as paper.
        float photoContrast = 1.35f;
        // Per-texel normal rebuilt from the plate's luma gradient, so stems inside
        // one card catch the sun separately. 0 = one flat normal per plate.
        float photoContour = 1.85f;
        // Grass-on-grass shadowing from the cascade the grass itself writes.
        // Requires castShadows; 0 disables it without disabling grass shadows.
        float photoSelfShadow = 1.0f;
        // Occlusion toward the clump root, which is what gives a card a bottom.
        float photoRootAo = 0.45f;
        // Alpha test for the photo cards only. The local family plates come from
        // JPEG opacity maps, whose compression noise around thin stems is a real
        // flicker source; raising this trims those partial texels away.
        float photoAlphaCutoff = 0.5f;
        // Force one atlas layer: -1 = normal selection, 0..31 = that atlas layer
        // (0 = the primary clump / first map clutter class, 1..8 the local
        // families on a loose-card world, up to 31 map clutter classes). Purely
        // a diagnostic -- it is how you find which plate is responsible for a
        // flicker.
        int photoForceLayer = -1;
        // PHOTO CARD COVERAGE: the fraction of clutter-grid cells (1.11 m,
        // WGR_GRASS_CARD_SPACING) that grow a card. Mean card spacing is
        // grid / sqrt(coverage): 0.24 is one card every ~2.3 m, ~0.19 per m^2.
        // The map's bake decides WHERE cards may grow (mask x surface probability);
        // this decides HOW MANY of those cells actually grow one, on every world.
        // It is not a distrust of the bake: one of our cards is two to three of
        // Arma's clutter plants in plate area (see photoCardScale), so a card per
        // authored clutter cell is two to three times the plant the map asked for.
        //
        // MEASURED (Takistan 6970.40 5700.31 352.57 az 20 el -18, consecutive
        // frames 900/901, fraction of grass pixels changing by more than 48/255 --
        // the popping signature, as distinct from smooth sway):
        //     scene with no grass                 0.01%   (the floor: nothing else moves)
        //     procedural blades                   0.83%
        //     cards at the map's own density      3.68%
        //     cards at coverage 0.25 (2.2 m)      1.66%
        //     cards with alpha-to-coverage off   10.16%   (A2C is carrying this)
        // Density is the flicker: every overlapping alpha edge is another chance
        // for a pixel to pop. 0.24 more than halves it AND is what the owner asked
        // for ("far too dense ... settle for something like 0.24 coverage").
        //
        // photoCoverageAuto restores the previous behaviour -- the map's own
        // density, untouched, wherever the bake answered -- for the A/B.
        float photoCoverage = 0.24f;
        bool photoCoverageAuto = false;
        // Photo card SIZE, 1.0 = the stock card. The stock card is 0.5-2.3 m tall
        // and wider than tall; Arma's own clutter plants are ~0.3-0.9 m, so at the
        // map's own density the stock card is two to three plants' worth of plate
        // over every clutter cell -- most of what reads as a solid mat. Kept at 1.0
        // until measured in a run; ~0.5 is the Arma-sized guess.
        float photoCardScale = 1.0f;
        // Photo-card PLACEMENT GRID, metres. Coverage above is the fraction of these
        // cells that grow a card, so this is the ceiling coverage cannot pass: at 1.11
        // (the A2/A3 worlds' own clutterGrid) even coverage 1.0 is 0.81 cards/m^2, while
        // the procedural mid ring sits on 0.82 m and the near ring far below that. Lower
        // this to let the photographed clumps reach procedural density. Renderer clamps
        // to 0.16..4.0; 0.16 was the original blade grid and is ~90 plates over every
        // point of ground, which is where the flicker came from.
        float photoCardSpacing = 1.11f;
        // ...and the same thing the way a human wants it: a DENSITY MULTIPLIER,
        // 1 = the map's own clutter density, 20 = twenty times as many cards.
        // The panel drives this and derives the spacing above from it
        // (spacing = clutterGrid / sqrt(density)), because a spacing slider is
        // the reciprocal of what everything else in the tab does -- "max it
        // out" on a metres control makes the grass SPARSER, which is exactly
        // the trap it laid the first time. Renderer still consumes metres.
        float photoCardDensity = 1.0f;
        // PROCEDURAL BLADE LOOK (near blades + mid ribbons; cards and the far
        // proxy ignore these). The defaults are ON: the pre-existing all-stock
        // values (0 / 1.0 / 0 / 0.70) rendered the field as a pale uniform wash
        // with no per-blade definition -- the owner's "grass looks flat" report.
        // WGR_GRASS_CONTRAST scales the whole group as one definition strength
        // (0 = the exact legacy flat look, 1 = these defaults; see
        // EngineWgpu's constructor), and each slider still overrides live.
        // Blade-on-blade self shadow: root occlusion inside a tuft plus a sample
        // of the cascade the near grass itself writes (needs castShadows for the
        // cascade half). 0 = off.
        float bladeSelfShadow = 0.55f;
        // Contrast about the palette's own mid-tone. 1.0 = untouched.
        float bladeContrast = 1.30f;
        // Per-patch hue drift (warm straw <-> cool blue-green) on top of the
        // luminance-only colourVariation. 0 = none.
        float bladeHueVariation = 0.025f;
        // Root-to-tip darkening. 0.70 = the long-standing mix(0.30, 1.0, t*t).
        float bladeRootShade = 0.78f;
        // CATCH-ALL WORLDS: a world whose terrain arrives as ONE material (a
        // converted Enfusion world such as Reforger's Everon) has no per-surface
        // clutter classification, so the whole map is grass and the stock
        // density/height read as a knee-high uniform meadow everywhere. On such a
        // world (GetGrassSurfaceCount() == 1) these two scale the master coverage
        // and the blade height; both are 1.0-neutral elsewhere. Owner's number for
        // an unknown clutter density is 0.24. WGR_GRASS_CATCHALL=0 disables.
        float catchAllCoverage = 0.24f;
        float catchAllHeight = 0.55f;
        // Saturation of the photographed cards about their own luma, separate
        // from the field-wide `saturation` so the plates can be matched to the
        // procedural grass rather than moved with it. 0.62 was the constant this
        // replaces, so the default changes nothing.
        float photoSaturation = 0.62f;
        // How long a walked imprint survives, in seconds; it recovers over the
        // last third so a trail thins out rather than blinking away. The record
        // is a ring of 256 stamps consumed by DISTANCE walked, so the practical
        // limit is trail length as much as time.
        float trackLifetime = 600.0f;
        // How far a crushed plant is pressed down, as a fraction of its own
        // height. 0.55 is the long-standing value. Photographed cards scale this
        // up internally: one plate stands for a whole clump nearly two metres
        // across, so it has to rotate much further than a blade to read as
        // walked-on.
        float imprintDepth = 0.55f;
        // Relative selection weights for atlas layers 1..8, i.e. the local
        // families. Zeroing one removes it from the field without touching the
        // others; all-zero falls back to the primary clump.
        float photoLayerWeights[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        // Width in metres of the stochastic dissolve at every LOD join. Each ring
        // thins out across this band as the next thickens, so grass no longer
        // changes representation on a hard circle. 0 = the former abrupt joins.
        float lodBlend = 30.0f;
        // Per-LOD coverage multipliers on top of `density`. The near ring is
        // where photographed-card overdraw is paid: a card is roughly 1.8 m wide
        // and they are placed every 0.16 m, so near coverage is the single most
        // effective performance control on that path.
        float nearDensity = 1.0f;
        // Up to 50x: below one thins candidates, above one refines their grid;
        // excess coverage at the fixed candidate budget broadens mid clumps.
        float midDensity = 4.0f;
        float farDensity = 1.0f;
        // Albedo tints, applied after the existing colour work and before
        // lighting, so the sun still does what it did. White = untinted.
        // Procedural covers near blades, mid ribbons and the far proxy together;
        // the photographed cards have their own so the two can be matched.
        float tintProcedural[3] = {1.0f, 1.0f, 1.0f};
        float tintPhoto[3] = {1.0f, 1.0f, 1.0f};
        // Mid LOD geometry. Off = the procedural crossed ribbons. On = crossed
        // cards carrying a photographed grass clump.
        //
        // Off by default: the close field is individual textured blades. The
        // optional cards are useful for comparing silhouettes, but their large
        // alpha-tested clump plates are not the preferred production look.
        // assets/grass/meadow-grass-clump-alpha-1024.png
        // ships: measured opaque mean (0.525, 0.622, 0.127) -- green on every
        // texel, hard binary alpha, no baked lighting. The legacy PAA fallback
        // (data\trava1_pmp2.pac) is grey-teal, so if only that is present the
        // mid ring looks desaturated and this is worth turning off.
        bool midPhotoTuft = false;
        // WHICH grass is drawn (owner request 2026-08-16: the map's own grass by default, his own
        // implementation selectable, and the panel must SHOW which one is on screen -- the
        // previous auto-enable bypassed the checkbox, so it read unchecked while cards drew).
        //   0 = Auto: the map's own clutter (photo cards from the CfgSurfaces atlas) where the
        //       map supplies one; the procedural blades everywhere else, which is every OFP world.
        //   1 = Procedural blades always ("my own grass implementation").
        //   2 = Photo cards always (the old midPhotoTuft; falls back to the loose primary card
        //       where no map atlas exists).
        // Auto is the default. midPhotoTuft is kept for saved-settings compatibility and reads
        // as source 2 when set.
        int clutterSource = 0;
        // RFG-062: bind a NATIVELY loaded Enfusion world's ground clutter from the map's own
        // ClutterConfig chain in the `.pak`s. Default on, and inert by construction on every
        // world that is not one (Landscape::GetEnfusionSurfaceCount() is 0 there), so this is
        // a switch for the native path only -- it does not touch OFP or Arma worlds.
        bool enfusionClutter = true;
        // RFG-089: the near-ring blade IMAGES from the world's own PlantMat atlas (native
        // Reforger worlds only). POSEIDON_ENFUSION_BLADES=0 seeds it off.
        bool enfusionBlades = []
        {
            const char* v = std::getenv("POSEIDON_ENFUSION_BLADES");
            return !(v != nullptr && *v == '0');
        }();
        // RFG-065: bind a NATIVELY loaded Enfusion world's GROUND MATERIAL from the palette
        // entry's own `.emat` -- its `ScaleUV` tiling and its middle-distance map -- instead
        // of stretching one image over each 12.5 m land cell. Lives beside `enfusionClutter`
        // because both are the same world's own data read out of the same `.pak`s, and both
        // are inert by construction on every world that is not one. It is the GROUND, not
        // the grass; the panel groups them, the shader does not.
        bool enfusionGround = true;
        float densityBoost = 4.0f; // turns base spacing into a denser placement grid
        float height = 1.40f;      // authored blade height multiplier
        // The default follows the weather system that also drives smoke,
        // parachutes and cloth. The two wind sliders remain a multiplier and
        // manual fallback for controlled visual testing.
        bool useLiveWind = true;
        float windStrength = 1.07f;
        float windDirection = 0.0f; // degrees, 0 = +X / east
        // How fast the wind NOISE FIELDS travel, as a multiple of the shipped speeds.
        // 1.0 is the historical look. NOT a strength control: it changes how quickly a
        // gust crosses the field, not how far a blade bends.
        //
        // Worth a knob because the shader's scroll speeds are constants with no relation
        // to the wind speed. At the default 6 m/s mean wind the gust field crosses the
        // world at ~48 m/s and the tip-flutter field at ~101 -- eight and seventeen times
        // the wind supposedly driving them, which is what reads as grass shivering or
        // springing rather than leaning and releasing. ~0.15 puts the gust field at
        // roughly wind speed.
        //
        // In the GAME this is driven from the live wind and never exceeds 1.0 (see
        // windScrollAuto). The manual value reaches 4 so the dev panel can still push it
        // past anything the weather will ever produce.
        float windScroll = 1.0f;
        // On by default: divide live speed by the shader's unscaled gust speed
        // so fronts travel at the measured wind speed, including when the bend
        // strength slider changes. Turn it off to use the manual scroll slider.
        bool windScrollAuto = true;
        // Broad travelling fronts with weaker sheltered patches between them.
        // Field size is along the wind; fronts are three times wider across it.
        bool windGustWaves = true;
        float windGustVariation = 1.0f; // 0 = even sway, 1 = full local contrast
        float windGustSize = 45.0f;     // metres along the wind
        // Reference-style field variation. These affect deterministic GPU
        // hashes, so they do not make blades swim when the camera moves.
        float clumping = 0.35f;
        // Subtle per-blade tone spread; preserve a coherent meadow palette.
        // The "Colour variation" slider still covers 0..1.
        float colorVariation = 0.12f;
        float transmission = 0.10f;
        // These are deliberately grass-only controls: terrain and other
        // world geometry retain the renderer's regular shadow/fog settings.
        bool castShadows = true;
        bool applyFog = true;
        // Developer diagnostic for legacy worlds whose geography flags are
        // invalid or over-broad. Off by default: it deliberately bypasses
        // road/forest/building rejection to prove whether placement works.
        bool ignoreGeographyExclusions = false;
    };
      virtual GrassSettings GetGrassSettings() const { return {}; }
      virtual void SetGrassSettings(const GrassSettings& /*settings*/) {}

      /// Whether the optional photographed clump atlas is actually loaded. When it
      /// is not, `midPhotoTuft` is inert and the renderer keeps procedural grass:
      /// a missing asset is always a fallback, never a failure. The Grass tab
      /// reports this rather than presenting a control that silently does nothing.
      virtual bool HasGrassPhotoClumps() const { return false; }
      /// True while the loaded world supplied its OWN clutter (CfgSurfaces atlas), i.e. what
      /// GrassSettings::clutterSource 0 (Auto) would draw here. False on every OFP world.
      virtual bool HasGrassMapClutter() const { return false; }
      /// True while the loaded world is a natively loaded Enfusion (Arma Reforger) one, i.e.
      /// while GrassSettings::enfusionClutter has anything to act on. False everywhere else,
      /// which is what the Grass tab greys the switch on.
      virtual bool HasEnfusionSurfaces() const { return false; }

      /// The photographed clump families actually discovered on disk, in atlas
      /// layer order (0 = the primary clump). Nothing about them is hardcoded:
      /// the count and the names come from what is installed, so the Grass tab
      /// only ever offers families that exist.
      virtual int GetGrassPhotoFamilyCount() const { return 0; }
      virtual const char* GetGrassPhotoFamilyName(int /*index*/) const { return ""; }

      // The active terrain's material layers.  WGPU exposes these so the dev
      // overlay can explicitly choose which painted surfaces receive blades.
      // GRS-A — grass instance accounting for the Grass tab's benchmark table.
      // Counts come from an async readback of the GPU placement counters, so they
      // lag the displayed frame by a few frames. Returns false on non-wgpu backends.
      struct GrassStatsOut
      {
          unsigned nearInstances = 0, midInstances = 0, farInstances = 0;
          unsigned nearCandidates = 0, midCandidates = 0, farCandidates = 0;
          unsigned nearVertices = 0, midVertices = 0, farVertices = 0;
      };
      virtual bool GetGrassStats(GrassStatsOut& /*out*/) const { return false; }

      // Renderer-owned dynamic GPU residency. Byte totals are portable payload /
      // buffer-capacity lower bounds; backend-driver metadata is not included.
      struct GpuMemoryStatsOut
      {
          uint64_t trackedBytes = 0;
          uint64_t budgetBytes = 0;
          uint64_t objectTextureBytes = 0;
          uint64_t geometryLiveBytes = 0;
          uint64_t geometryCapacityBytes = 0;
          uint64_t geometryRetiredBytes = 0;
          uint64_t objectTextureRetiredBytes = 0;
          uint64_t backendAllocationBytes = 0;
          unsigned objectTextureCount = 0;
          bool overBudget = false;
      };
      virtual bool GetGpuMemoryStats(GpuMemoryStatsOut& /*out*/) const { return false; }

      // Explicit on-demand Stage F diagnostics. No per-asset selected-LOD usage is inferred.
      enum class GeometryReportStatus { Unsupported, Pending, Busy, Ready, Invalid, Failed };
      struct GeometryAllocationRow
      {
          std::string asset;
          uint32_t producerModel = 0, rendererModel = 0, lodIndex = 0;
          float resolution = 0;
          uint64_t vertexBytes = 0, indexBytes = 0;
          uint32_t sections = 0, liveMeshes = 0, missingMeshes = 0;
      };
      struct GeometryAllocationReport
      {
          struct RegisteredMeshRef
          {
              uint64_t mesh = 0, models = 0, modelLods = 0, sections = 0;
              uint32_t flags = 0;
          };
          uint64_t requestId = 0;
          std::vector<GeometryAllocationRow> rows;
          uint64_t uniqueVertexBytes = 0, uniqueIndexBytes = 0;
          uint64_t overlapWithinInspectedMeshes = 0;
          uint64_t overlapWithinInspectedVertexBytes = 0, overlapWithinInspectedIndexBytes = 0;
          uint64_t poolLiveBytes = 0, poolCapacityBytes = 0, poolRetiredBytes = 0;
          uint32_t registryEntriesAtRequest = 0, registryEntriesVisited = 0, ineligibleEntriesSkipped = 0;
          uint32_t modelsRequested = 0, modelsVisited = 0;
          uint32_t lodsInVisitedModels = 0, sectionVisits = 0, uniqueMeshes = 0;
          uint32_t missingModels = 0, missingMeshes = 0, invalidRanges = 0;
          uint32_t unresolvedNames = 0;
          uint32_t maxModels = 0, maxRows = 0, maxSectionVisits = 0;
          bool valid = false, truncated = false;
          // Producer-request parked selection versus renderer-drain facts; different cuts.
          // Logical candidate classes, never reclaim/device-free/all-view ownership proof.
          bool parkedReportEnabled=false,parkedCaptureComplete=false,parkedJoinComplete=false,parkedTruncated=false;
          uint64_t parkedModelsVisited=0,parkedModelsCaptured=0,parkedMetadataVisits=0,parkedInvalidEntries=0;
          uint64_t parkedLinkVisits=0,parkedUniqueAllocations=0,parkedKnownCandidateBytes=0;
          uint64_t parkedCpuBorrowedAllocations=0,parkedCpuBorrowedBytes=0,parkedUnborrowedAllocations=0,parkedUnborrowedBytes=0;
          uint64_t parkedOutsideLinkedAllocations=0,parkedOutsideLinkedBytes=0;
          uint64_t parkedAbsentRetiredAllocations=0,parkedUnknownAllocations=0,parkedUnknownKnownBytes=0;
          uint64_t parkedRequestKnownCapacityBytes=0,parkedLinkKnownCapacityBytes=0;
          bool ownershipCoverageComplete = false; // no complete renderer-wide ownership inventory
          // Optional startup-only persistent mesh-owner attribution. Bytes are
          // producer payload estimates, not physical GPU allocation sizes. Scope
          // excludes in-flight draw/caster references, skin/bake buffers and driver
          // allocations; completeness never establishes safe reclamation.
          bool persistentAttributionEnabled = false, persistentAttributionComplete = false;
          uint64_t persistentAttributionEpoch = 0, persistentIncompleteReasons = 0;
          uint64_t producerEstimatedVertexBytes = 0, producerEstimatedIndexBytes = 0;
          uint64_t persistentCpuReferencedVertexBytes = 0, persistentCpuReferencedIndexBytes = 0;
          uint64_t persistentRetainedSharedCpuVertexBytes = 0, persistentRetainedSharedCpuIndexBytes = 0;
          uint64_t persistentStandaloneCpuVertexBytes = 0, persistentStandaloneCpuIndexBytes = 0;
          // "Live" here means no producer scheduled-retirement fact. CPU uploader
          // retirement has no pool-free acknowledgement in this diagnostic.
          uint64_t persistentTrackedAllocations = 0, persistentTrackedBorrowerRecords = 0;
          uint64_t persistentOwnerEstimatedBytes = 0, persistentZeroOwnerEstimatedBytes = 0;
          uint64_t persistentCpuBorrowers = 0, persistentModelLodLinks = 0;
          uint64_t persistentMailboxPending = 0, persistentMailboxDropped = 0;
          uint64_t persistentStandaloneWitnessLive = 0, persistentStandaloneWitnessKnownBytes = 0;
          uint64_t persistentStandaloneWitnessPeakKnownBytes = 0, persistentStandaloneWitnessRefused = 0;
          uint64_t persistentCancelledCreates = 0, persistentReclaimableBytes = 0;
          // Diagnostic metadata only. Cleanup capacity is the fixed numeric
          // candidate array; borrower bytes exclude map nodes/buckets and RSS.
          uint64_t persistentPrunedAllocations = 0, persistentPrunedBorrowers = 0;
          uint64_t persistentRetirementCandidates = 0, persistentCleanupKnownCapacityBytes = 0;
          uint64_t persistentDetachedHistory = 0, persistentBorrowerKnownRecordBytes = 0;
          uint64_t persistentSelectedDiagnosticPins = 0;
          // On-demand Rust record lookup over copied full-generation handles.
          // Absent proves no current renderer mesh record/range, not GPU work
          // completion or released device memory. Scheduled retirement is separate.
          bool rendererMeshFactsEnabled = false, rendererMeshFactsValid = false;
          bool rendererMeshFactsComplete = false, rendererMeshFactsTruncated = false;
          uint64_t rendererMeshFactCalls = 0, rendererMeshFactEpoch = 0;
          uint64_t rendererMeshFactsRequested = 0, rendererMeshFactsInspected = 0;
          uint64_t rendererMeshFactsPresent = 0, rendererMeshFactsAbsent = 0;
          uint64_t rendererMeshFactsInvalid = 0, rendererMeshFactsDuplicateHandles = 0;
          uint64_t rendererMeshFactsUnknownMappings = 0, rendererMeshFactsScheduledRetired = 0;
          uint64_t rendererMeshFactsPresentScheduledRetired = 0, rendererMeshFactsAbsentScheduledRetired = 0;
          uint64_t rendererMeshFactsAbsentWithPersistentOwners = 0;
          uint64_t rendererMeshFactVertexBytes = 0, rendererMeshFactIndexBytes = 0;
          bool rendererMeshRecordScopeValid = false, rendererMeshRecordClosureComplete = false, rendererMeshPoolResidualValid = false;
          uint64_t rendererMeshLiveRecords = 0, rendererMeshRecordPoolGeneration = 0;
          uint64_t rendererMeshRecordPoolLiveBytes = 0, rendererMeshPoolUnattributedBytes = 0;
          // Bounded registered section census, separate from complete allocation
          // ownership. Zero references never imply safe reclamation.
          bool rendererRegisteredRefsEnabled = false, rendererRegisteredRefsValid = false;
          bool rendererRegisteredRefsComplete = false;
          uint64_t rendererRegisteredRefsCalls = 0, rendererRegisteredRefsRows = 0;
          uint64_t rendererRegisteredModelsVisited = 0, rendererRegisteredLodsVisited = 0;
          uint64_t rendererRegisteredSectionsVisited = 0, rendererRegisteredMissingRecords = 0;
          uint64_t rendererRegisteredReferencedMeshes = 0, rendererRegisteredModelRefs = 0;
          uint64_t rendererRegisteredModelLodRefs = 0, rendererRegisteredSectionOccurrences = 0;
          uint32_t rendererRegisteredRefusalFlags = 0;
          std::vector<RegisteredMeshRef> rendererRegisteredRefSamples; // max8, not the full census
          // Immutable mesh cut + later completion of preceding main queue work.
          // No future cached-reference/device-free/physical reclamation proof.
          bool rendererMeshAckEnabled = false, rendererMeshAckValid = false;
          uint64_t rendererMeshAckTicket = 0, rendererMeshAckEpoch = 0;
          uint64_t rendererMeshAckPoolGeneration = 0, rendererMeshAckSubmission = 0;
          uint64_t rendererMeshAckRequested = 0, rendererMeshAckPresent = 0, rendererMeshAckAbsent = 0;
          uint64_t rendererMeshAckVertexBytes = 0, rendererMeshAckIndexBytes = 0;
          uint64_t rendererMeshAckPoolLiveBytes = 0, rendererMeshAckPoolCapacityBytes = 0, rendererMeshAckPoolRetiredBytes = 0;
          uint32_t rendererMeshAckState = 0;
          // Row/union scope: retained meshes only, excluding direct-only meshes,
          // standalone skin/bake buffers, driver alignment and textures/materials.
          // Global pool counters also include direct meshes. Not reclaimable bytes.
          // Overlap counts each live mesh once if distinct inspected model/LOD
          // pairs reference it; absent overlap does not prove exclusive ownership.
      };
      virtual GeometryReportStatus RequestGeometryAllocationReport(uint32_t, uint32_t, uint32_t,
          uint64_t& requestId) { requestId = 0; return GeometryReportStatus::Unsupported; }
      virtual GeometryReportStatus PollGeometryAllocationReport(uint64_t,
          GeometryAllocationReport&) const { return GeometryReportStatus::Unsupported; }
      struct LodDemandRow { uint32_t rendererModel = 0, lodCount = 0, lodMask = 0, state = 0; };
      struct LodDemandReport
      {
          uint64_t requestId = 0, currentFrame = 0, lastSampleFrame = 0;
          uint32_t samplingStatus = 0, framesRequested = 0, framesAttempted = 0, framesSampled = 0;
          uint32_t droppedFrames = 0, mapFailures = 0, droppedDispatches = 0, dispatchedViews = 0, passMask = 0;
          std::vector<LodDemandRow> rows;
          bool ownershipCoverageComplete = false, cachedViewCoverageComplete = false;
      };
      virtual GeometryReportStatus RequestLodDemandReport(const std::vector<uint32_t>&, uint32_t,
          uint64_t& requestId) { requestId = 0; return GeometryReportStatus::Unsupported; }
      virtual GeometryReportStatus PollLodDemandReport(uint64_t, LodDemandReport&) const { return GeometryReportStatus::Unsupported; }
      virtual GeometryReportStatus CancelLodDemandReport(uint64_t) { return GeometryReportStatus::Unsupported; }

      virtual int GetGrassSurfaceCount() const { return 0; }
      virtual const char* GetGrassLoadedMapName() const { return ""; }
      virtual const char* GetGrassSurfaceName(int /*index*/) const { return ""; }
      virtual bool IsGrassSurfaceEnabled(int /*index*/) const { return false; }
      virtual void SetGrassSurfaceEnabled(int /*index*/, bool /*enabled*/) {}

    /// One alpha-tested shadow-caster batch: a contiguous run of the alpha vertex
    /// buffer sharing one caster texture, whose alpha cuts the cast shadow (so
    /// cutout foliage casts a leaf silhouette). Vertices are xyz+uv (5 floats).
    struct ShadowCasterBatch
    {
        Texture* texture = nullptr;
        int firstVertex = 0;
        int vertexCount = 0;
    };

    /// Casters for one shadow depth pass: opaque triangles rendered solid, plus
    /// alpha-cutout triangles grouped into per-texture batches rendered with a
    /// texture-alpha discard so foliage casts its real silhouette, not a blob.
    struct ShadowCasterSet
    {
        const float* solidXYZ = nullptr; // 3 floats/vertex
        int solidVertexCount = 0;
        const float* alphaXYZUV = nullptr; // 5 floats/vertex (xyz + uv)
        int alphaVertexCount = 0;
        const ShadowCasterBatch* alphaBatches = nullptr;
        int alphaBatchCount = 0;
    };

    /// Render the caster set from the light into the cascade depth array —
    /// `numCascades` column-major light view-projections back-to-back in
    /// `lightVPs`, the per-tier selection distance in `splitViewDist` (a camera
    /// 3D-distance radius for the first `omniCount` omni tiers, a far eye-depth for
    /// the frustum tiers), and the camera forward (`camFwd3`, eye-depth select) —
    /// and keep the array + splits + forward + omniCount for the lit pass.
    virtual void RenderShadowDepthScene(const float* /*lightVPs*/, const float* /*splitViewDist*/,
                                        const float* /*camFwd3*/, int /*numCascades*/, int /*omniCount*/, int /*res*/,
                                        const ShadowCasterSet& /*casters*/)
    {
    }

    /// GPU-driven caster submission, the alternative to the CPU triangle soup
    /// above: the scene hands the backend the cascade set plus per-caster mesh +
    /// transform (SetShadowCascades / AddShadowCaster) and the backend renders
    /// the depth passes itself — skinned casters pose on the GPU instead of
    /// being collected at bind pose. Backends opt in via UsesGpuShadowCasters.
    virtual bool UsesGpuShadowCasters() const { return false; }
    virtual void SetShadowCascades(const shadow::CascadeSet& /*cascades*/, int /*resolution*/) {}
    virtual void AddShadowCaster(const Shape& /*mesh*/, const Matrix4& /*modelToWorld*/) {}

    /// Read the current shadow depth map back and write it as a grayscale PNG
    /// (top-down) for eyeballing. Returns false if unsupported / nothing rendered.
    virtual bool DumpShadowMap(const char* /*path*/) { return false; }

    /// Called by the window system when the window has been resized (e.g. after
    /// a fullscreen transition completes).  Backends that need to resize their
    /// swap chain should override this.
    virtual void OnWindowResized(int /*w*/, int /*h*/) {}

    /// Post-resize hook — fires after OnWindowResized has finished updating
    /// _w/_h.  Apps register a function pointer here at boot to re-run the
    /// aspect policy when the viewport changes (e.g. async fullscreen
    /// transition completes with a different native resolution than the
    /// initial windowed size).  Without this, aspect settings stay stuck
    /// at the boot-time viewport — UI ends up pillarboxed on a viewport
    /// it was never computed for.
    typedef void (*ResizePostHook)(int w, int h);
    void SetResizePostHook(ResizePostHook hook) { _resizePostHook = hook; }
    void FireResizePostHook(int w, int h)
    {
        if (_resizePostHook)
            _resizePostHook(w, h);
    }

    /// Called when SDL confirms the fullscreen state has actually changed.
    /// This is the single source of truth for _windowed — do NOT set it in
    /// SwitchWindowed (the request is async, confirmation comes via events).
    virtual void OnFullscreenChanged(bool /*windowed*/) {}

    // True if this backend does skeletal skinning on the GPU. When set, the
    // animation system additionally hands the bone palette + per-vertex weights
    // to the renderer (VertexBuffer::SetSkinData/SetPalette) for graphical LODs.
    virtual bool UsesGpuSkinning() const { return false; }

    /// Screen-space overlay renderer: indexed, textured, scissored triangles
    /// composited over the finished frame. This is the dev panel's (ImGui)
    /// render backend on engines without a native one — GL33 renders ImGui
    /// through imgui_impl_opengl3 instead and leaves these unimplemented.
    /// Coordinates are framebuffer pixels, top-left origin.
    struct OverlayVertex // layout matches ImDrawVert
    {
        float x, y;
        float u, v;
        uint32_t rgba; // R in the low byte (ImGui packing)
    };
    struct OverlayDrawCmd
    {
        float clip[4]; // x0, y0, x1, y1
        uint64_t texture;
        uint32_t firstIndex;
        uint32_t indexCount;
        uint32_t baseVertex;
    };
    virtual bool SupportsOverlayRenderer() const { return false; }
    /// Create / replace / free an RGBA8 overlay texture. `rgba` is w*h*4 bytes.
    virtual uint64_t OverlayTextureCreate(int /*w*/, int /*h*/, const uint8_t* /*rgba*/) { return 0; }
    virtual void OverlayTextureUpdate(uint64_t /*texture*/, int /*w*/, int /*h*/, const uint8_t* /*rgba*/) {}
    virtual void OverlayTextureDestroy(uint64_t /*texture*/) {}
    /// Replace this frame's overlay draw data (drawn last, over everything).
    virtual void SubmitOverlay(const OverlayVertex* /*verts*/, int /*vertCount*/, const uint16_t* /*indices*/,
                               int /*indexCount*/, const OverlayDrawCmd* /*cmds*/, int /*cmdCount*/)
    {
    }

    virtual ITerrainRenderer* GetTerrainRenderer() { return nullptr; }

    // The GPU water-surface renderer, when the backend has one (wgpu). Null on GL33,
    // which keeps drawing the legacy per-segment water mesh. Mirrors GetTerrainRenderer.
    virtual IWaterRenderer* GetWaterRenderer() { return nullptr; }

    // Live tonemap/look parameters for the HDR path (wgpu). Mirrors WgrTonemap; the
    // ImGui Tonemap tab edits these and the backend pushes them to the renderer. The
    // Hable curve is fixed; the per-time-of-day look is exposure + this grade block.
    /// Picture Mode depth of field. A screenshot aid: OFF by default, and while it is off the
    /// renderer does not create the pass, the target or the shader, so it costs one bool test per
    /// frame and nothing else.
    ///
    /// Distances are metres and blur is pixels, because those are the units someone composing a
    /// shot can judge by eye. An f-number and a sensor size would be more photographic and less
    /// useful -- the game has neither, so they would have to be invented and then explained.
    struct DepthOfFieldSettings
    {
        bool enabled = false;
        bool followPlayer = false;
        bool focusOnClick = false;
        /// Distance the sharp band is centred on.
        float focusDistance = 12.0f;
        /// Half-width of the fully sharp band. Everything inside it is untouched.
        float focusRange = 4.0f;
        /// Widest circle of confusion, in pixels at 1080p; scaled to the real render height so a
        /// 4K screenshot is not silently sharper than the same shot at 1080p.
        float maxBlurPixels = 4.0f;
        /// Behind and in front of the focal plane, separately. Foreground blur is much more
        /// intrusive than background blur and wants its own control rather than sharing one.
        float backgroundScale = 1.0f;
        float foregroundScale = 0.6f;
        /// How quickly blur opens up past the sharp band, in 1/metres. Small = long gentle
        /// falloff, large = a hard cut.
        float transition = 0.05f;
        /// Diagnostic: 0 = normal, 1 = paint the circle of confusion, 2 = paint raw view distance.
        /// There is no way to tell a broken depth mapping from a weak blur by looking at the
        /// composited image, which is exactly how the first attempt at this pass wasted an hour.
        int debugView = 0;
        /// Samples per pixel, 4..128. This is the ENTIRE cost of the pass -- each sample is a
        /// colour fetch and a depth fetch, so 96 samples at 1080p is roughly 200 million texture
        /// reads a frame. Fine for a paused screenshot, not for playing. Too few and the spiral
        /// shows as noise in smooth gradients like sky.
        int sampleCount = 48;
        /// Bokeh. A flat average turns a bright out-of-focus point into a faint smear -- its
        /// energy divided over the whole disc. A real lens does the opposite: the point prints as
        /// a bright disc. Weighting samples above bokehThreshold restores that, and it works only
        /// because the pass runs in linear HDR before the tonemap, where a specular genuinely
        /// carries a value above 1 rather than having been clipped to white.
        float bokehBoost = 3.0f;
        float bokehThreshold = 0.7f;
        /// Aperture blades: 0 = a perfect circle, 5..9 = a polygon, as a real iris prints.
        float apertureBlades = 0.0f;
    };

    virtual bool SupportsDepthOfField() const { return false; }
    virtual DepthOfFieldSettings GetDepthOfFieldSettings() const { return {}; }
    virtual void SetDepthOfFieldSettings(const DepthOfFieldSettings& /*s*/) {}

    // REN-TEMP-001 §6.7 — live temporal/upscaler tuning (dev panel Temporal tab).
    // Defaults mirror the renderer's shipped seeds; the panel pushes only on change, so
    // an env-tuned session is not clobbered by merely opening the tab.
    struct TemporalSettings
    {
        bool temporalOn = true;      // jitter + motion vectors + history
        bool dlssOn = true;          // request; effective only on a WGR_DLSS launch
        int renderScalePct = 67;     // 50..200 (<100 upscaling, 100+DLSS = DLAA, >100 SSAA)
        int jitterPhases = 0;        // 0 = auto (frozen at native, cycling when upscaling)
        bool mipBiasAuto = true;     // auto = half log2(scale)
        float mipBias = -0.3f;       // used when !mipBiasAuto, clamped [-4, 0]
        float reactiveSky = 0.4f;    // history-control strengths (0 = off)
        float reactiveWater = 0.3f;
        bool dlssAutoExposure = true; // owner's A/B winner 2026-08-30
        bool jitterYFlip = true;
        // FSR 1 (EASU+RCAS): the license-clean upscaler that ships in every build;
        // active when DLSS is off/unavailable and render scale < 100 %.
        bool fsrOn = true;
        float fsrSharpness = 0.25f; // RCAS stops: 0 = sharpest, 2 = mildest
        // RCAS sharpen pass over the DLSS output (strength = fsrSharpness).
        // Default ON — the owner's pick: DLSS + 0.25-stop RCAS beats plain DLSS.
        bool dlssSharpen = true;      // convention A/B arms — leave alone unless probing
        bool mvRenderSpace = false;
        bool mvFlip = false;
    };

    // Read-only display block for the same tab.
    struct TemporalInfo
    {
        int renderWidth = 0, renderHeight = 0;
        int outputWidth = 0, outputHeight = 0;
        bool temporalActive = false;
        bool dlssRoute = false;  // NGX device route up (decided at launch)
        bool dlssActive = false; // evaluated last frame
        int dlssQuality = -1;    // NGX PerfQuality value
        float jitterX = 0.0f, jitterY = 0.0f;
        float mipBiasEffective = 0.0f;
        bool resetThisFrame = false;
        // Active MSAA sample count of the scene targets (1 = off). Fixed at launch;
        // SetMsaaSamples changes the NEXT launch.
        int msaaSamples = 1;
        // 0 none/native, 1 DLSS, 2 FSR1, 3 bilinear.
        int activeUpscaler = 0;
    };

    struct TonemapSettings
    {
        float exposure = 1.0f;    // linear pre-curve multiplier (main per-ToD lever)
        float temperature = 0.0f; // white balance warm(+)/cool(-)
        float tint = 0.0f;        // white balance magenta(+)/green(-)
        float contrast = 1.0f;    // post-curve contrast (1 = neutral)
        float saturation = 1.0f;  // post-curve saturation (1 = neutral)
        float lift = 0.0f;        // shadow lift (0 = neutral)
        float gain = 1.0f;        // post-curve overall multiply (1 = neutral)
        bool hable = true;        // false = passthrough clamp (debug)
        bool encode = true;       // linear->sRGB encode
        // Bloom (HDR only). A global look setting, not per-time-of-day keyframed, so
        // it is preserved across the auto-preset overwrite. intensity 0 = off.
        float bloomIntensity = 0.04f; // linear weight of the bloom added to the scene
        float bloomThreshold = 1.0f;  // soft-knee centre (scene-referred luminance)
        float bloomKnee = 0.5f;       // soft-knee half-width
        // NV-001 night vision. Whether the goggles are ON is not stored here -- that is
        // Engine::_nightVision, driven by the player -- these are the LOOK of the tube.
        float nvGain = 40.0f;      // linear amplification of scene light before the curve
        float nvNoise = 0.10f;     // sensor grain, scaled by darkness in the shader
        float nvVignette = 0.55f;  // eyepiece edge falloff, 0 = none
    };
    // True only on backends with an HDR resolve pass (wgpu w/ HDR enabled); gates
    // the ImGui Tonemap tab.
    virtual bool SupportsTonemap() const { return false; }
    virtual TonemapSettings GetTonemapSettings() const { return {}; }
    virtual void SetTonemapSettings(const TonemapSettings& /*s*/) {}
    // Auto = drive the grade from the per-time-of-day preset keyframes; override =
    // hold the manual values set via SetTonemapSettings (for tuning a keyframe).
    virtual bool GetTonemapAuto() const { return false; }
    virtual void SetTonemapAuto(bool /*enable*/) {}

    // Marks the scene->UI seam: the 3D world is done, UI/HUD drawing follows. On the
    // HDR path the backend resolves (tonemaps) the offscreen scene to the swapchain
    // here, so subsequent 2D/3D-in-UI composites display-referred (no tonemap). No-op
    // on LDR-direct backends (GL33).
    virtual void ResolveSceneToDisplay() {}

    // Authored parameters for the procedural atmospheric sky (wgpu). The celestial
    // inputs (sun/moon direction, night factor) come live from LightSun each frame;
    // these are the tunable atmosphere + look knobs edited in the ImGui Sky tab and
    // pushed to the renderer. See engine/WgpuRenderer/docs/procedural-sky-plan.md.
    struct SkySettings
    {
        float rayleigh[3] = {5.8e-6f, 13.5e-6f, 33.1e-6f}; // scattering coeff per channel (1/m)
        float rayleighHeight = 8000.0f;                    // Rayleigh density scale height (m)
        float mie = 6.0e-6f;                               // Mie scattering coeff (1/m). Lowered from
                                                           // the Earth clear-day 21e-6: the aerial glare
                                                           // (Mie forward phase x sun radiance) was
                                                           // washing the scene. Tunable in the Sky tab;
                                                           // the froxel LUT + sun-shadowing is the real fix.
        float mieG = 0.76f;                                // Mie anisotropy [0,1)
        float mieHeight = 1200.0f;                         // Mie density scale height (m)
        float turbidity = 1.0f;                            // haze amount
        float ozone = 1.0f;                                // ozone absorption strength (blue-hour knob)
        float ground[3] = {0.1f, 0.1f, 0.1f};              // ground albedo
        float sunAngularRadius = 0.0047f;                  // sun disc half-angle (rad, ~0.27 deg)
        float sunIntensity = 22.0f;                        // sun radiance scale
        float exposure = 1.0f;                             // radiance -> scene-referred scale
        float planetRadius = 6360000.0f;                   // planet radius (m)
        float atmosphereHeight = 60000.0f;                 // atmosphere thickness (m)
        int viewSamples = 16;                              // primary ray march steps
        int lightSamples = 8;                              // light ray march steps
        float horizonHaze = 0.0f;                          // legacy sky->fog-colour horizon blend; 0 now that aerial perspective handles the terrain/sky seam
        float aerialShadow = 1.0f;                          // froxel fog terrain sun-shadowing strength: 0 = off (sun lights fog everywhere), 1 = physical, >1 exaggerated. Pushed via sky.night_horizon.w
        // EDGE SOFTNESS of the SECOND layer. 0 = the layer as it rendered before this existed:
        // the sheet noise is remapped linearly about the coverage threshold into a hard clamp, so
        // a cloud reaches useful density close to its boundary and gets a definite silhouette.
        // High cloud does not look like that — it is ice, its edges sublimate rather than
        // condense, and the bigger the mass the deeper the fringe of thinning crystals round it.
        // 1 holds the density down through a much wider band either side of the threshold, so the
        // fringe is long and the silhouette soft.
        // WEIGHTED BY LOCAL COVERAGE, which is what makes it "the BIGGER ones get fuzzier": a
        // large cloud is where the coverage field is high, so the banks soften and isolated wisps
        // stay defined. The layer's mean density is restored as it rises, so it is a shape control
        // and not a brightness one.
        // Unlike the other second-layer sliders the default here is NOT the old look — 0 is.
        float cirrusSoftness = 0.45f;
        // Fraction of the draw distance at which distance fog reaches FULL. 1.0 = the ramp as
        // it behaved before this existed, saturating only exactly at the far plane -- which is
        // also exactly where the terrain grid stops, where Scene::GetObjectDrawDistance puts the
        // object cull ring, and where the map ends, so anything still visible out there reads as
        // the world being cut off. 0.85 closes the horizon a little inside all three.
        // This is a DIFFERENT question from fogFalloff below: that shapes how fast the ramp
        // climbs, so using it to thicken the far field also thickens the near and mid field.
        float fogFarClose = 0.85f;
        bool layeredFog = true; // Finite ASL volume by default; weather fog zero is unchanged.
        float fogLayerBase[2] = {0.0f, 70.0f};
        float fogLayerTop[2] = {35.0f, 100.0f};
        float fogLayerFeather[2] = {5.0f, 5.0f};
        float fogLayerExtinction[2] = {0.008f, 0.003f}; // 1/metre at weather fog 1
        float fogLayerAlbedo = 0.95f;
        float fogLayerG = 0.6f;
        float fogTerrainFollow = 1.0f; // 0: fixed ASL; 1: layer height above local ground
        float fogTerrainReference = 0.0f;
        float fogValleyStrength = 1.0f;
        float fogValleyRadius = 150.0f;
        float fogPatchStrength = 0.45f;
        float fogPatchHorizontalScale = 250.0f;
        float fogPatchVerticalScale = 70.0f;
        float fogCoverageFeather = 300.0f;
        float fogFalloff = 3.0f;                             // aerial fog distance-ramp exponent (pow(dist/drawDist, k)). High = clear near/mid, fog only at the edge; low (~1) = dense fog throughout, which reveals the volumetric terrain shadowing / god rays. Pushed via camera fog_color.w

        // Authored night-sky floor: a deep-blue radiance blended in by sun elevation so
        // twilight/night settle into blue instead of the physical model's near-black.
        // Colours are normalised (0..1, pickable); nightIntensity scales them to radiance.
        float nightZenith[3] = {0.15f, 0.30f, 0.80f};     // night colour at the zenith
        float nightHorizon[3] = {0.35f, 0.45f, 0.90f};    // night colour at the horizon
        float nightIntensity = 0.035f;                     // scales the night colours to radiance
        // Band chosen to OVERLAP the physical sunset: the model's blue/zenith collapses to
        // near-black by ~-2deg sun elevation, so the floor must be well underway by then to
        // avoid an orange->black->blue gap. Ramps in from +4deg, full night by -6deg.
        float nightStartDeg = 4.0f;                        // sun elevation for full day (night = 0)
        float nightEndDeg = -6.0f;                         // sun elevation for full night (night = 1)
        bool enabled = true;                               // draw the procedural sky
        // Sky-based scene lighting (HDR only): light terrain + objects FROM the atmosphere —
        // sun = sunIntensity*exposure*transmittance(camAlt->sun) (reddens at sunset, -> 0 below
        // horizon), on the same physical radiance scale as the sky/fog. Off = legacy GL33 sun.
        // A toggle for A/B while the look is re-tuned. See lighting.wgsl / EngineWgpu PushFrame.
        bool skyLighting = true;                           // atmosphere-driven surface sun/ambient
        float skyAmbient = 1.35f;                          // scale on the DIRECTIONAL SH sky-irradiance ambient (objects/terrain sample the env map per normal). Physical now, so expect to re-tune this in the planned sky/tonemap pass
        // Drive the atmosphere look (exposure/sunIntensity/rayleigh/mie/ozone/turbidity/
        // sun radius/night intensity) from the per-time-of-day preset table each frame,
        // like the tonemap grade. Off = hold the Sky tab's manually edited values so the
        // atmosphere sliders can be tuned. The toggle knobs above are always live.
        bool autoToD = true;                               // interpolate atmosphere from the ToD presets

        // Volumetric clouds (plan Stage 5): a raymarched cloud shell composited into the
        // procedural sky (so clouds also appear in water reflections + SH ambient). Coverage
        // spans isolated cumulus (low) to a solid overcast deck (high). Coverage also dims the
        // directional sun / lifts ambient on the CPU side (PushFrame), so overcast reads flat.
        // Off by default (coverage 0) so the clear-sky look is unchanged until authored.
        // REN-SKY-004: the planar water reflection marched the SKY's 128-step clouds into a
        // half-resolution buffer that is then reflected off a displaced surface and read back
        // through a roughness mip chain. On, it uses its own low-step pipeline
        // (WGR_CLOUD_STEPS_REFLECTION, 32 by default): 2.55 -> 1.20 ms on an open-sea pose with
        // no visible difference. Off restores the sky's own march for the reflection.
        bool cloudReflectionCheap = true;
        float cloudCoverage = 0.42f;                       // 0 = clear .. 1 = full overcast
        // Drive cloudCoverage from the WORLD's overcast (Landscape::GetOvercast) each frame, so
        // Zeus / the `weather` console command / mission weather all move the sky. Without this
        // the two are unrelated: Zeus reported an overcast that nothing rendered. When on, the
        // Coverage slider below is a read-only display of the driven value; turn this off to
        // author coverage directly.
        bool cloudCoverageFromWeather = true;
        float cloudCoverageClear = 0.05f;                  // coverage at overcast 0
        float cloudCoverageFull = 0.95f;                   // coverage at overcast 1
        // Cloud EVOLUTION: metres per second of drift through the noise volume, perpendicular to
        // the wind. Wind alone only translates the field — the same clouds slide past forever.
        // Drifting the sample position through the third noise axis instead makes them form and
        // dissolve in place. Deliberately slow: the shape tile is ~9 km, so 8 m/s is a full
        // turnover in roughly twenty minutes, which reads as weather rather than animation.
        float cloudEvolve = 8.0f;
        float cloudDensity = 0.06f;                        // extinction (1/m); higher = more opaque
        float cloudBottom = 1200.0f;                       // cloud layer base altitude ASL (m)
        float cloudTop = 3500.0f;                          // cloud layer top altitude ASL (m)
        float cloudWind[2] = {8.0f, 2.0f};                 // horizontal scroll velocity (m/s), manual fallback
        // Drive the deck's scroll velocity from the world's wind authority
        // (World/Weather/WindModel) instead of the constant above, the same way
        // cloudCoverageFromWeather drives coverage from overcast. With this off
        // the sky keeps its authored constant wind exactly, which is also the
        // only way to get the old bit-reproducible velocity*time cloud offset
        // back for frame-comparison work.
        bool cloudWindFromWeather = true;
        // Cloud-base wind is not surface wind. Friction slows the lowest tens of
        // metres; at a ~1200 m base the flow runs appreciably faster and backed a
        // few degrees. 2.2x is the usual rule-of-thumb ratio for a neutral
        // boundary layer and it is what stops a 2 m/s surface breeze reading as a
        // frozen sky.
        float cloudWindAloftScale = 2.2f;
        // Anti-repetition: shape + detail tiles sampled at INCOMMENSURATE world sizes so the visual
        // period is far longer than either; a large-scale weather field drifts coverage across the
        // sky; a domain warp breaks grid regularity. Sizes are world metres (scale = 1/size).
        float cloudShapeSize = 9300.0f;                    // base shape tile (m) — large = less tiling
        float cloudDetailSize = 1700.0f;                   // detail tile (m) — incommensurate with shape
        // 16 km was under four whole periods inside the 60 km march, and the repeats stack up into
        // bands near the horizon where many periods share a few degrees of screen. 26 km puts ~2.3
        // periods in view, and the warp below now displaces coverage as well as shape.
        float cloudWeatherSize = 26000.0f;                 // coverage-drift field size (m)
        float cloudWeatherAmount = 0.4f;                   // how much weather varies local coverage (0 = uniform)
        float cloudWarpSize = 6000.0f;                     // domain-warp field size (m)
        // 2200 m was set when the warp was the only thing fighting repetition, and it cost real
        // noise: measured, it drove the high-frequency residual from 1.355 to 1.768, because a
        // large warp adds structure the march then has to integrate. The slice drift in
        // cloud_coverage/cloud_density now does the anti-repetition work -- it changes the PERIOD
        // rather than displacing within it -- so the warp can go back to breaking up grid
        // regularity without carrying the whole job. 1400 m is still over a tenth of the shape
        // tile, which is what 900 m was short of.
        float cloudWarpAmount = 1400.0f;                   // domain-warp displacement (m)
        float cloudHgG = 0.35f;                            // forward-scatter anisotropy (silver lining)
        float cloudPowder = 1.0f;                          // Beer-Powder dark-edge strength (0..1)
        float cloudAmbient = 1.0f;                         // sky-ambient fill scale on the shadowed sides
        float cloudMaxDist = 60000.0f;                     // march / visibility cap (m); keep <= ~80 km
        // SECOND CLOUD LAYER — high cirrus at ~7 km, above the cumulus deck. It is what stops the
        // sky reading as one populated slab with clear air above it. Off = the upper sky is bare.
        bool cirrusEnabled = true;
        // Flat sheet (one ray/sheet intersection + one field evaluation) vs a thin-shell march.
        // Cirrus IS nearly a sheet, so the flat model is not wrong — but a sheet is pierced, never
        // travelled through, so it has no parallax and no soft edge where a grazing ray leaves it.
        // The shell march is 6 samples through 900 m and the field walks the noise volume's third
        // axis with height, so the top and the bottom of the layer are different cloud.
        // Default ON because it was measured, not assumed: interleaved GPU-frame-total samples on
        // the OFP devtest sky (800x600) read 8.895 ms with the layer off, 8.932 flat, 9.068
        // volumetric -- +0.136 ms over flat, +0.173 ms over no second layer at all. With the
        // camera above the deck and the sky filling the frame the two modes are level (5.865 vs
        // 5.869 ms), because the shell march bounds its path and bails in the gaps.
        bool cirrusVolumetric = true;
        // PUFFINESS of the second layer: 0 = cirrus fibratus (wind shear draws the ice into long
        // parallel fibres — elongated, striated, flat), 1 = cirrocumulus (shallow convection
        // breaks it into a raft of nearly round cells — short, lumpy, with real vertical depth).
        // One slider drives four coupled things in the shader (streak aspect, coarse-vs-fine
        // octave balance, fibre erosion, and the marched shell's thickness), because up there
        // those are all consequences of the same cause: whether the air is being sheared or is
        // convecting. Total opacity is held roughly constant across the range, so it reads as a
        // shape control and not a brightness one. 0.5 = EXACTLY the shipped look (every endpoint
        // pair in sky.wgsl is symmetric about the previous hard-coded constant).
        float cirrusPuffiness = 1.0f;
        // How far the layer wanders from that on its own, as a fraction of the slider — so a
        // still day is sometimes flatter and sometimes lumpier without anyone touching it.
        // Driven from the world's weather-drift clock (the same one the cumulus shapes evolve
        // on), so it is smooth, continuous, tied to sim time, and freezes when the sim does; the
        // full cycle is the better part of an hour at the default evolution speed. 0 = perfectly
        // steady, i.e. exactly the look at the chosen puffiness. Small by default: at 0.15 the
        // extremes are a slightly more drawn-out or slightly lumpier version of the shipped look.
        float cirrusPuffVariation = 1.0f;
        // AMOUNT of the second layer — the "more or less of it" control that the on/off flag above
        // was not. 0 = a handful of separated wisps in an otherwise empty upper sky, 0.5 = EXACTLY
        // the coverage the layer shipped with, 1 = a continuous cirrostratus veil with only thin
        // breaks. Coverage is what moves furthest; the layer's optical depth follows only slightly
        // (0.8x .. 1.25x), because a sky filling with cirrus really does thicken as it fills — a
        // warm front — and holding density flat would make the top of the slider read as "the same
        // thin veil, everywhere". Same three-knot mapping as cirrusPuffiness, so the midpoint is
        // the shipped constant itself rather than the average of two others.
        float cirrusAmount = 0.5f;
        // How much the second layer takes on the FIRST layer's character instead of high ice
        // cloud's. 0 = cirrus at 7 km, which is what this layer has always been. 1 = as close to
        // the cumulus deck as a 6-to-14-step shell march gets: it descends to just above the
        // deck's top, its features shrink to the deck's own shape scale, the 4:1 wind-shear
        // stretch and the fibre striation relax to isotropic cells, the shell deepens toward a
        // fraction of the deck's bottom-to-top span, optical depth rises from ~2.8 to 8, the ice
        // halo phase gives way to the deck's droplet phase, and a cheap vertical self-shading term
        // appears so it reads as solid cloud rather than glowing fog. Every one of those targets
        // is read from the deck's LIVE parameters, so "similar" follows the weather rather than a
        // second set of constants — and the altitude is clamped so the second layer can never sink
        // into or below the first, because then it would stop being a second layer.
        // Default 0: this only ever moves the layer AWAY from what it shipped as, so the bottom of
        // the range is the neutral value, not the middle.
        float cirrusMatchDeck = 1.0f;
        // CLD-020 cloud shadows: the deck dims the direct sun on terrain, objects, grass and
        // water through a world-anchored transmittance map rebuilt each frame around the camera.
        // 0 = off (every surface reads fully lit, and the compute pass early-outs per texel).
        // This is the "approved cheaper fallback" the roadmap allows rather than near/far
        // clipmaps with reprojection: one 512x512 dispatch, no history, no temporal state.
        float cloudShadowStrength = 0.85f;
        // Procedural star field. The night sky had nothing in it at all -- the authored night
        // floor is a flat blue wash, so a clear night read as black. Gated to night by sun
        // altitude in the shader, and added before the cloud composite so a deck covers the stars
        // the way it covers the sky behind them.
        float starIntensity = 1.0f;
        // Experimental procedural lens flare.  The current WGPU implementation can project a
        // sun-strength blob through an imported world's horizon, so keep it opt-in until its
        // sun-direction and depth-occlusion contract is validated.  0 early-outs in the shader;
        // local emitters and the sun's lighting are independent of this setting.
        float lensFlare = 0.0f;

        // ---- God rays (crepuscular rays / light shafts) ----------------------------
        // A world-space volumetric march at a fraction of the screen resolution, occluded by the
        // CLD-020 cloud sun-transmittance map, the terrain shadow ceiling and the cascade shadow
        // map — so the shafts take the shape of whatever is actually between the camera and the
        // sun, and change as a cloud bank crosses it. Not a screen-space radial blur, which is why
        // an off-screen sun produces no edge halo. Composited into the linear HDR scene before
        // bloom/exposure/tonemap, so shafts bloom and roll off like any other light.
        // These defaults MUST match godrays::GodRaySettings::default on the renderer side, or the
        // first Sky-tab edit would visibly change settings nobody touched.
        bool godRays = true;
        float godRayIntensity = 4.0f;      // multiplier on the physical single-scatter estimate
        // Scattering coefficient (1/m); clear-day Mie is 6e-6. The renderer reads it relative to
        // its DENSITY_REF (1.2e-5 = 1.0x), so this default is just above the physical
        // density. The Sky tab shows it in units of 1e-6, i.e. "Air density (x1e-6)" = 13.
        float godRayDensity = 1.3e-5f;
        // March cap. Short on purpose: the cloud map spans 4 km centred on the camera, so a ray
        // leaves it by ~2-2.9 km and reads fully lit past that, and the aerial froxel already owns
        // sun-shafted haze beyond. What this pass adds over the froxel is cloud shaping and
        // resolution, both near/mid-field concerns.
        float godRayDistance = 3000.0f;    // march cap (m)
        float godRayG = 0.60f;             // Henyey-Greenstein anisotropy (beam tightness)
        float godRayCloudInfluence = 1.0f; // how strongly the cloud deck shapes the shafts
        int godRaySteps = 16;              // samples per ray      -- the two cost knobs
        int godRayResDiv = 2;              // screen-res divisor   --

        // ---- Moon / celestial mechanics -------------------------------------------
        // The 2001 engine placed the sun and the moon with a toy orbital model (see the
        // header comment in Graphics/Rendering/Lighting/Ephemeris.hpp): no eccentricity,
        // no lunar perturbations, a 28-day month, and longitude ignored entirely. These
        // switch the bodies onto a real low-precision ephemeris driven by the world's
        // date (Glob.clock) and the world config's latitude/longitude, and expose the
        // handful of overrides a screenshot or an A/B needs.
        //
        // The whole block is CONSUMED BY LightSun::Recalculate, which runs about once
        // per twenty seconds of simulated time — not per frame. Nothing here costs
        // anything at frame rate.
        bool moonRealistic = true;  // ephemeris moon position + phase (off = legacy orbit)
        // The SUN stays on the legacy model by default. A real ephemeris moves daylight:
        // solar transit shifts by up to the +/-16 min equation of time, and the
        // altitude/azimuth track through the day changes shape. Every time-of-day preset
        // in this file was authored against the legacy sun, so flipping this silently
        // would re-time every sunrise in every mission. Turn it on deliberately.
        bool sunRealistic = false;
        // Hand-place the moon (azimuth from north toward east, elevation above the
        // horizon) instead of computing it. For screenshots and for isolating a phase.
        bool moonManual = false;
        float moonManualAzimuthDeg = 135.0f;
        float moonManualElevationDeg = 30.0f;
        // Observer latitude override, GEOGRAPHIC degrees, NORTH POSITIVE. Note this is
        // the OPPOSITE sign to the CfgWorlds `latitude` field. Off = use the world's.
        bool moonLatitudeOverride = false;
        float moonLatitudeDeg = 45.0f;
        // Celestial date override. Affects the EPHEMERIS ONLY — it does not move the
        // simulation clock, so it is local, non-replicated, and safe to scrub while a
        // mission runs. Use it to find a phase; use the `setDate` path to move the world.
        bool moonDateOverride = false;
        int moonDateYear = 1985;
        int moonDateMonth = 6;
        int moonDateDay = 21;
        // Multiplies the TRUE angular radius of the disc. 1 = correct (~0.26 deg, which
        // is genuinely small — the "huge moon" of photographs is a long lens, not the sky).
        float moonSizeScale = 1.0f;
        // Multiplies both the drawn disc and the moonlight. Artistic gain on top of the
        // physical phase falloff, not a replacement for it.
        float moonBrightnessScale = 1.0f;
        // What fraction of the moon disc's night brightness survives in FULL DAYLIGHT.
        //
        // The disc's radiance is its irradiance divided by its solid angle, and that
        // irradiance carries a deliberate ~24,000x artistic inflation: the physical
        // full-moon/sun ratio is ~2.5e-6 and `moonIntensity` uses 0.06, because anything
        // physical renders as black once the tonemap is not scotopic. That inflation is
        // justified only while the eye is adapted to night. Carried into daylight unchanged
        // -- which is what it did until 2026-08-29 -- it draws a disc at 6% of the SUN's
        // radiance into a daylit sky, i.e. a second sun, which is exactly what players
        // reported. The real daytime moon is a pale grey wafer barely above the sky.
        //
        // Ramped by LightSun::NightEffect, so the NIGHT look is bit-identical: the fade only
        // acts where NightEffect is below 1, and the value here is where it lands at noon.
        float moonDaylightScale = 0.03f;
        // Full-moon irradiance as a fraction of the sun's, for the sky-lit scene path.
        // The physical ratio is ~2.5e-6, which renders as black: human night vision is
        // scotopic and the tonemap here is not. What it must beat is the night AMBIENT
        // floor, ~0.023 radiance at the 22:00 preset, and the moon's directional term works
        // out to about intensity * 0.41 -- so the original 1/400 landed at 4% of ambient:
        // present in the buffer and invisible on screen. 0.06 puts it at ~1.1x ambient. The
        // legacy GL33 path sits at 1.7x (0.075 * 0.68 diffuse against 0.030 ambient), so the
        // two backends were a factor of ~40 apart for the same scene. Drives the disc as
        // well as the light: they are one body, so this is one knob by necessity. The phase
        // function scales it on top, making a crescent ~30x dimmer.
        //
        float moonIntensity = 0.06f;
        // Extra gain on the moon's LIGHT only (not the disc), so the ground can catch it
        // without the disc blooming into a lamp. At 1.0 (= moonIntensity alone) Takistan's
        // moon-facing slopes measured (7.4, 6.4, 7.4)/255 under a forced full moon at 45 deg
        // and the owner, flying Stratis under a 25 deg moon over dark grass, said "ground does
        // not seem to catch light from the moon". 4.0 is the answer to that; the disc stays
        // where it was. WGR_MOON_LIGHT_GAIN overrides it without a rebuild.
        float moonLightGain = 4.0f;
        // Moon casts a directional light (and shadows) at night. Off = the pre-existing
        // behaviour, where night is lit by flat ambient only and the moon is scenery.
        bool moonLighting = true;
        // Force the illuminated fraction (0 = new .. 1 = full) instead of computing it.
        // Negative = use the real phase. Only affects the LOOK; the position is unchanged.
        float moonPhaseOverride = -1.0f;
    };

    /// ROAD / DECAL per-pixel ground conform. Deliberately NOT part of SkySettings: these
    /// describe how ground-conformed geometry wins its depth test, which has nothing to do with
    /// the atmosphere. They lived there for one afternoon because that was the push path that
    /// already existed, and that is not a reason.
    struct RoadSettings
    {
        // An OnSurface fragment re-seats its depth on the terrain height field and is then pulled
        // this far toward the camera ALONG THE RAY so it wins against the ground it lies on.
        float liftFlat = 0.02f;
        // Per metre of distance, and divided by |dir.y| in the shader: the residual it covers is a
        // height error, and a height error becomes error/|dir.y| along the ray -- about 6x looking
        // down a road from standing height. Before that division existed one constant had to serve
        // both the top-down view and the grazing one, and at grazing angles the road broke into a
        // chequer of tiles that won and lost the test alternately (owner, Malden 2026-08-29).
        // Swept on Malden; the owner's value after testing the remaining hole himself.
        float liftPerMetre = 0.0067f;
        // Ceiling as a fraction of distance. Its job is not to keep the lift small but to keep it
        // FINITE as the ray flattens toward the horizon, where the division above runs away.
        // The DEFAULT is bounded at 0.04; the control is not. The owner measured what lies
        // past it: at 0.1263 a road is pulled far enough toward the camera to be drawn OVER
        // HOUSES AND OTHER OBJECTS standing on or near it (the pull is ~8.7 m at 80 m and ~25 m
        // at 200 m, so anything closer than that in front of the road loses). That is the
        // failure this term exists to bound, and 0.04 is where the two requirements meet on
        // Malden -- large enough to beat the conform residual, small enough to lose to a house.
        //
        // Clamped generously rather than at 0.04 on purpose: a shipped default is mine to keep
        // safe, a dev-panel slider is the user's to push. Anyone who raises it will see the
        // artefact immediately, which is a better teacher than a slider that stops.
        float liftMaxFrac = 0.04f;
    };
    virtual RoadSettings GetRoadSettings() const { return {}; }
    virtual void SetRoadSettings(const RoadSettings&) {}

    // True on backends with a procedural sky pass (wgpu); gates the ImGui Sky tab.
    virtual bool SupportsSky() const { return false; }
    virtual SkySettings GetSkySettings() const { return {}; }
    virtual void SetSkySettings(const SkySettings& /*s*/) {}
    // True while the procedural sky owns the background, so the legacy skydome meshes
    // must be suppressed (backend-aware: only the wgpu backend with the sky enabled
    // returns true, so GL33 keeps drawing its dome). See Landscape::DrawSky.
    virtual bool ProceduralSkyActive() const { return false; }

    // Eye adaptation / auto-exposure (HDR only, gated by SupportsTonemap). Off by
    // default so it doesn't fight manual per-time-of-day exposure tuning; when on, the
    // resolve multiplies exposure by a scale eased toward key / scene-average-luminance.
    // NOTE: appended at the class end on purpose — inserting virtuals mid-class shifts
    // every later vtable slot and misdispatches across TUs (see git history / memory).
    struct ExposureSettings
    {
        // ON by default since 2026-08-16, at the owner's explicit request, and measured
        // before flipping. Takistan hour 8 at the worst-clipping pose on record, same
        // binary, only this flag differing:
        //
        //     off   mean luminance 155.1, 23.17% of pixels >= 250
        //     on    mean luminance 123.9,  0.59% of pixels >= 250
        //
        // i.e. clipping falls from a quarter of the frame to under one percent. The sky
        // keeps its cloud structure instead of burning out, and as a side effect the
        // long-standing "Arma 2 roads are white" complaint largely goes with it — that was
        // never a texture bug, it was the palest large surface in an overexposed frame
        // clipping first (see WLD-024).
        //
        // The previous comment here read "Manual filmic exposure is the stable default...
        // its 4x adaptation range can flash the whole scene when a player turns from dark
        // terrain toward a bright sky or water glint." That risk is REAL and is not
        // disproved by a settled capture — a still frame cannot show pumping. Two things
        // reduce it, and a third is the actual mitigation:
        //   1. `rateTau` below makes adaptation framerate-INDEPENDENT, so it no longer runs
        //      twice as fast on a 45 fps world as on a 20 fps one. Tuning against one
        //      world's framerate was the deeper half of the original problem.
        //   2. Narrowing minScale/maxScale is the lever to reach for
        //      FIRST if pumping is reported — see the Tonemap tab.
        //   3. If it does pump in motion, turn it off here, or narrow minScale/maxScale
        //      toward 0.55..1.10, which measured within 0.2% of the full range on the test
        //      pose. Almost all of the benefit is available from a much narrower band.
        bool enabled = true;
        float key = 0.18f;      // target middle-grey luminance (higher = brighter)
        // ASYMMETRIC ON PURPOSE. Darkening is the feature the owner asked for ("looks into the
        // sun, the iris adjusts and it gets a bit darker"). Bright snow also needs enough
        // darkening headroom to retain grain and depression shading. BRIGHTENING is what
        // caused a regression the moment this went default-on:
        // at night the scene is dark, the iris opened toward the old 4.0 ceiling, and that
        // amplified Arma 2 vegetation's AUTHORED emissive (41 OA rvmats literally contain
        // `emmisive[]={2.6,2.3,2.0}`) until trees self-illuminated and trunks went white.
        //
        // Measured, Takistan hour 0, same pose, pixels at luminance >= 200:
        //     auto-exposure off      0
        //     maxScale 4.0        1215   <- the regression
        //     maxScale 1.10          0   <- and mean luminance back to 1.13 vs 1.06 off
        //
        // Costs almost nothing in daylight: a 0.55..1.10 range measured 0.76% of pixels clipped
        // against 0.59% for the full 0.25..4.0 range on the worst-clipping pose on record. Nearly
        // all of the benefit is in the darkening half, which is exactly the half being kept.
        //
        // Raise this only with a NIGHT capture in hand. A bright-sky pose cannot show what it breaks.
        // Actual settled original Noe snow at hour16: the previous0.25 floor
        // clipped56..75% of the sampled snow's red channel;0.10 clips none.
        // Key, adaptation speed and the night-safe brightening ceiling stay fixed.
        float minScale = 0.10f;
        float maxScale = 1.10f;
        // Per-frame ease, kept for the legacy path and for `rateTau <= 0`. Framerate
        // DEPENDENT, which is why it is no longer the default mechanism.
        float rate = 0.03f;
        // Adaptation time constant in SECONDS. When > 0 the backend uses
        // 1 - exp(-dt / rateTau) instead of `rate`, so the same setting adapts at the same
        // real-world speed on every world regardless of framerate. 0 restores `rate`.
        // 0.4 s is a deliberately unhurried iris: fast enough to stop a sky white-out,
        // slow enough that ordinary look-around does not read as the scene breathing.
        float rateTau = 0.4f;
        // Spatial metering: relative weight given to the TOP of the frame (the sky) vs
        // the bottom (the ground), so a bright sky in view doesn't drag the average up
        // and over-darken the ground. 1.0 = uniform; lower biases metering toward the
        // lower screen. See exposure.wgsl fs_lum_first.
        float skyWeight = 0.3f;
    };
    virtual ExposureSettings GetExposureSettings() const { return {}; }
    virtual void SetExposureSettings(const ExposureSettings& /*s*/) {}
    // Debug: the current auto-exposure scale the resolve is applying (1.0 = neutral).
    // Blocking GPU readback on the wgpu backend; call only from the dev panel.
    virtual float GetAutoExposureScale() const { return 1.0f; }

    // GPU water surface look (wgpu only; gated by SupportsWater). Live-tunable via the
    // ImGui Water tab; the backend pushes these into the water UBO each frame. Purely
    // cosmetic — gameplay reads the flat sea plane regardless. See docs/water-rendering-plan.md.
    // NOTE: appended at the class end on purpose (see the vtable-slot note above).
    struct WaterSettings
    {
        bool enabled = true;         // draw the GPU water surface (off = seabed only, for A/B)
        // 1 = Tidewater Native (default); 0 = the legacy Current OP FFT/CDLOD water.
        // Saved explicit profile/UI choices remain valid; an absent backend group means 1.
        // Switching happens at a frame boundary. Exact WGR_WATER_BACKEND=0|1 overrides
        // the profile/UI selection for a session; do not force an environment override.
        int waterBackend = 1;
        // TW-WATER — the Tidewater Native parameter group (read only while waterBackend == 1).
        // Defaults are Tidewater's own (dgreenheck/tidewater @4811ba48). Spectrum
        // wind speed and heading come from the authored sea conditions. Shipped to
        // the renderer in WgrWaterParams::tidewater; see wgpu_renderer.hpp for the lane layout.
        struct TidewaterLook
        {
            // W3g — sea conditions, Tidewater's own preset table (AppUI.js SEA):
            // 0 = Breezy shape with weather-scaled amplitude; 1 Calm, 2 Breezy,
            // 3 Choppy, 4 Storm; 5 custom sliders with authored Breezy wind/fetch.
            int seaConditions = 0;
            float whitecaps = 0.5f;        // custom mode: whitecap amount (AppUI whitecaps)
            // W4 — breaker spray (Breakers `spray` emission gain, Spray sprite `intensity`); 0 = off
            float spray = 1.0f;
            float sprayIntensity = 1.0f;
            // W3j — scale of the light scattered inside the water (1 = Tidewater's formula on OP's
            // light, pale cyan; default 0, Dec's pick 2026-09-28: the reflection-led navy of run 4)
            float waterGlow = 0.0f;
            float amplitude = 1.0f;        // spectrum amplitude scale
            float choppiness = 0.9f;       // horizontal displacement (Tessendorf lambda)
            float swellScale = 0.48f;      // energy of the distant-swell system
            float swellHeadingDeg = 5.0f;  // heading of the distant swell, world degrees
            float foamCoverage = 1.0f;     // whitecap / foam coverage gain
            float foamIntensity = 1.0f;    // foam brightness
            bool ssr = true;               // screen-space reflections of the scene
            float reflectionStrength = 1.0f;
            float cascadeScale = 1.0f;     // x 733 / 157 / 33.3 / 7.1 m cascade tiles
            float roughness = 0.035f;      // base GGX roughness
            float sss = 1.0f;              // backlit-crest subsurface gain
            float backscatter = 0.035f;    // water-volume backscatter fraction
            float gust = 1.0f;             // gust patches (cat's paws)
            float slick = 1.0f;            // surfactant slicks
            float windrow = 0.3f;          // windrow foam lines
            int debugView = 0;             // Tidewater's own material debug views (0 = off)
            // W3 — shoreline waves (ShoreWaves.js ShoreParams defaults)
            bool shoreWaves = true;
            float shorePeriod = 9.0f;      // s
            float shoreAmplitude = 0.34f;  // offshore amplitude H/2 (m), x the swell energy / 0.48
            float shoreVariation = 0.55f;  // per-wave and along-shore height variation
            float shoreGamma = 0.78f;      // breaker index H / depth
            float shoreBreakSpan = 0.13f;  // fraction of the break depth the lip plunges over
            float shoreCurl = 1.0f;        // plunging / bore profile weight
            float shoreRunup = 1.0f;       // swash run-up gain
            float shoreTurbidity = 0.16f;  // surf-zone sediment + bubble scattering (1/m)
            // W3b — shore simulation (ShoreSim.js): carried / stranded foam, wet sand
            bool shoreSim = true;
            float shoreDryTime = 28.0f;    // s for wet sand to dry (ShoreSimParams dryTime)
            // W3d — breaker lip sheets (Breakers.js `sheet`: lip opacity multiplier, 0 = off)
            float shoreLips = 1.0f;
        } tidewater;
        // Neutral multipliers reproduce GodotOceanWaves' authored cascade values.
        // Those physical spectrum coefficients are already metre-scaled; multiplying
        // them by 2.4 after removing the erroneous IFFT normalization was excessive.
        float waveAmp = 1.00f;       // authored reference amplitude (was a calmer 0.40 default)
        float waveChoppy = 1.0f;     // 1 = reference horizontal displacement
        float waveSpeed = 1.0f;      // 1 = reference dispersion time
        float waveScale = 0.65f;     // owner default 2026-09-06 (was 1.0 = reference cascade wavelengths)
        // Distance detail LOD: wave detail flattens between these (metres), killing the
        // far-field moiré / repetition — past fadeEnd the water is a smooth horizon mirror.
        float fadeStart = 589.0f;
        float fadeEnd = 865.0f;
        // De-tiling domain warp (metres). A finite FFT texture is necessarily periodic, so at
        // distance the cascade period reads as a repeating grid on the ocean — the further you
        // see, the more repeats fit on screen and the more obvious it is. This warps the world ->
        // FFT lookup through low-gradient value noise whose hash does not repeat within the
        // playable world, which breaks the period without disturbing the wave shape. 0 reproduces
        // the reference project's exact sampling (and its tiling); a few metres is enough.
        float warpAmp = 5.0f;
        float specPower = 140.0f;    // sun-glint sharpness (owner default 2026-09-06, was 11)
        float specIntensity = 3.82f; // sun-glint brightness (HDR, blooms)
        float alpha = 1.00f;         // base opacity (Fresnel raises it toward 1 at grazing angles)
        // Sun shadow: terrain heightfield + CSM occlusion removes the sun glint and
        // direct-sun sheen where the water is shadowed; shadowDim additionally darkens
        // the whole shadowed surface (0 = physical sun-only removal, 1 = strong artistic).
        float shadowDim = 0.5f;
        // Depth-based colour + soft shoreline (Stage 2, from the opaque-depth prepass). The body
        // tint runs shallowColor -> deepColor with the water column depth (Beer-Lambert-like),
        // and the surface fades to transparent over the last coastFade metres of depth so the
        // coast is a soft wash over the wet beach, not a hard clip line.
        // Gamma-space. "Deep" means a SATURATED dark blue, not black: I drove these to near-zero
        // chasing darker water and the result was a desaturated sea showing nothing but its own
        // reflection. The body radiance is albedo x irradiance, so a near-black albedo renders as
        // black no matter how bright the sun is. Real ocean blue comes from volumetric inscattering,
        // which is far brighter than a surface albedo would suggest — so the body colour has to
        // carry actual brightness in blue while staying dark in red.
        float shallowColor[3] = {0.070f, 0.290f, 0.320f}; // coastal turquoise
        // WRL-002: the deep swatch now also sets the absorption hue (water_optics), so it
        // decides what the water turns into with depth, not just what the far body looks
        // like. Owner direction 2026-09-06: "the sea can also be a darker blue green the
        // deeper it goes" -- pulled from the ultramarine (0.014, 0.105, 0.240) toward a
        // darker teal, so a deepening column drifts blue-green and darker rather than
        // saturating to a brighter navy.
        float deepColor[3] = {0.010f, 0.085f, 0.135f};    // dark blue-green deep ocean
        // 1/m extinction. Applied directly (the old 0.15/m floor saturated every bay to the deep
        // colour by ~20 m, so the turquoise only survived at the waterline); ~0.035 spreads the
        // shallow -> deep transition over ~60 m, which is what a real shelf looks like from above.
        // Raised so the turquoise is confined to genuinely shallow water: at 0.16/m the body is
        // 55% toward the deep colour by 5 m and 96% by 20 m, instead of carrying cyan far out
        // across the shelf.
        float colorExt = 0.160f;
        float coastFade = 0.09f;  // m of column depth over which the shore ramps transparent->opaque
        // Coast foam + swash (Stage 2c): a churning foam band at the waterline, and a gentle
        // oscillation of the near-shore water edge in/out over the wet beach. Cosmetic only.
        float foamWidth = 1.12f;   // m of column depth the foam band spans (peaks ~1/4 in)
        float foamIntensity = 0.10f;// foam brightness / coverage
        // WAVE foam: whitecaps and persistent breaker foam on open water, as opposed to the
        // shoreline band the two values above drive. Separate because they are different
        // phenomena -- water breaking on land versus a crest collapsing under its own steepness --
        // and foamIntensity used to scale both, so there was no way to calm the ocean without
        // also stripping the surf.
        float waveFoamIntensity = 0.62f;
        // How strongly deep water suppresses whitecaps. 0 = waves break the same everywhere;
        // 1 = open ocean stays nearly smooth and breaking is concentrated where it belongs, in
        // shoaling water near the coast.
        float waveFoamDeepFalloff = 1.0f;
        // m the near-shore waterline oscillates in/out. Reduced from 0.47: this shifts the
        // EFFECTIVE column depth, so on a gently sloping beach half a metre of depth translates
        // into several metres of horizontal waterline travel, which reads as the water pulling
        // back off the shore and leaving a gap rather than as a wash.
        float swashAmp = 0.14f;
        float swashSpeed = 0.018f; // swash cycles per second (very slow = long, lazy wash)
        // Terrain-side wet/intertidal band: near-flat ground just above the (swash-moved) sea
        // level reads as damp sand (darker albedo), registering with the water's edge. Shared
        // by the terrain shader via WgrTerrainParams. wetDarken = 1 disables it.
        float wetHeight = 4.0f;    // m above sea level the damp band reaches
        float wetDarken = 0.35f;   // albedo multiplier in the band (1 = no darkening)
        // Master switch for water splash particles: the restrained CPU rifle-impact spray and
        // the GPU whitewater/spray billboard emitter. Enabled by default; activity remains at
        // 0.25 so ordinary impacts and crest spray stay subtle.
        bool rifleImpactSpray = true;
        // Multiplier for the GPU whitewater/splash billboard emitter.
        float waterSplashParticleActivity = 0.25f;

        // WTR-LOOK — surface energy model. The legacy composite capped the Fresnel reflection
        // weight, scaled the sun specular to 0.12x and multiplied the subsurface-scattering term
        // by the (near-black) deep body colour, which together flattened the surface into blue
        // plastic. The physical composite lets Fresnel run uncapped, evaluates the sun lobe at the
        // variance-filtered roughness so glitter stays stable with distance, and gives SSS its own
        // light path. Keep the legacy path selectable for A/B captures.
        bool physicalLook = true;
        // WRL-002 — shared optical model. ON: the surface composites through water_optics
        // (one absorption + scattering extinction derived from the deep swatch and Colour
        // clarity, a single-scatter body glow that is zero at zero path, and the transmitted
        // background composited in-shader at full coverage), and the underwater compositor
        // fogs with the SAME extinction. OFF: the previous physical composite, kept for A/B
        // (separate Beer-Lambert curve, seabed-visibility falloff, shallow->deep lerp, and a
        // framebuffer alpha that blended the background in a second time). Only consulted
        // while physicalLook is on.
        bool sharedOptics = true;
        // WRL-002 — whitecaps (persistent foam injection, crest whitecaps and wind-torn spray)
        // are gated by the latched wind speed: none below ~4 m/s, full at the 12 m/s reference
        // sea. OFF reproduces crest-geometry-only whitecaps for A/B.
        bool whitecapWindGate = true;
        float glitterGain = 1.0f;    // sun-specular gain (1 = the model's own energy)
        float sssGain = 1.0f;        // subsurface / backlit-crest gain
        // 0.22 on the owner's instruction (was 0.7). NOTE for anyone changing this again:
        // reflectionGain is in the profile's `surface` group, so a new default is dead on
        // any install that already has %APPDATA%\CWR\water-look\<map>.cfg -- which is
        // every install, because the Water tab writes the whole subset on any edit. The
        // profile version was bumped to 5 and the reader drops this one field from older
        // files for exactly that reason.
        float reflectionGain = 0.22f; // environment-reflection gain (1 = uncapped physical Fresnel)
        // How much wider the planar reflection renders than the screen's field of view. The
        // reflected camera otherwise inherits the main projection exactly, so a grazing reflection
        // needs directions that were never rendered, the lookup falls off the edge of the target,
        // and the reflected clouds end in a visible line across the water. Padding trades angular
        // resolution for coverage; the planar sample is mip-filtered on purpose, so a little
        // softness costs less here than a hard edge. 1.0 = the old behaviour.
        float reflectionFovPad = 1.35f;
        // Fraction of the reflection target over which the planar reflection hands back to the
        // sky/environment sample. Some water cannot be covered by a planar reflection at all --
        // tilt down and the water beneath you maps outside the mirrored camera's frustum at any
        // field of view -- so this fade is what carries those pixels. At the old 3% the swap read
        // as a line across the sea, because planar has parallax-correct clouds and the environment
        // sample does not. Wider = the reflection loses parallax gradually instead of ending.
        float reflectionEdgeFade = 0.22f;

        // WTR-LOOK — physical sea-state coupling. The amplitude control used to scale the whole
        // variance spectrum uniformly, which raised every wave at its existing wavelength: a
        // rougher sea became short steep chop instead of the long swell a real wind sea grows.
        // Coupled, the amplitude sets a wind speed (and matching cascade domain lengths), so the
        // JONSWAP peak frequency moves with it and taller seas are also longer seas.
        bool seaStateCoupling = true;
        // WEATHER AMPLITUDE — use the world's mean wind to scale water wave height.
        // The authored spectrum, heading, fetch, foam and material stay fixed.
        // Wind speed is latched in coarse buckets before it changes amplitude,
        // limiting spectrum rebuilds (see UpdateSeaWind in WaterWgpu.cpp).
        // Force off for A/B with the environment variable WGR_WATER_WIND=0.
        bool windFromWeather = true;
        // Wave amplitude left at DEAD CALM, as a fraction of the reference sea.
        //
        // The coupling is an intercept-plus-slope anchored so 12 m/s is exactly 1.0 (the
        // sea the spectrum was authored against). This is the intercept. It was 0.55 with a
        // 0.45 slope, which put the calm-weather mean of 1.5 m/s at 0.61 -- a flat day still
        // carried nearly two thirds of a blustery one's wave energy, and it showed.
        //
        // Not zero, and not near it: a literal speed ratio flattens the sea to a mirror,
        // which reads as broken water rather than calm water. There is also always some
        // residual swell on a real sea, generated by wind that is somewhere else.
        //
        // Changing this does NOT move the reference look: the slope is derived
        // from it using 12 m/s for Current OP and 7 m/s for Tidewater.
        float seaCalmScale = 0.30f;
        // Shore breaker gain — the shoaling swell that runs in toward the beach. OFF by default:
        // the current train is an analytic two-harmonic sine, which can shoal and steepen but
        // fundamentally cannot overturn, so it never reads as a wave crashing into itself. A real
        // plunging breaker needs an actual breaking model, not a bigger sine. Left in place and
        // tunable rather than deleted, but it should stay off until that exists.
        float shoreWaveGain = 0.10f;
        // Dev-only performance mode: drops SSR, planar reflection and their scene sampling from
        // the water fragment shader. Off by default.
        bool lowQuality = false;
        // Coast-aware CDLOD geometry density. GodotOceanWaves uses a camera-following clipmap
        // with Low/High/High8K meshes; our terrain-integrated equivalent changes how far each
        // fine CDLOD level remains active while preserving shoreline pruning and horizon coverage.
        // 0 = Performance, 1 = Balanced (default), 2 = Reference High, 3 = Ultra.
        // Balanced preserves the near-water mesh fidelity while avoiding the much
        // larger visible patch set of the reference/screenshot tier.
        int geometryQuality = 1;
        // Live FFT spectral-map resolution. 512 is the optimized gameplay default;
        // 1024 matches GodotOceanWaves' authored map size, while 256 is the low tier.
        // Resource reconstruction happens only when this value changes.
        int fftResolution = 512;

        // Scene-referred underwater compositor. It reconstructs metric view-ray distance, limits
        // extinction to the portion of each ray below the surface, and uses the authored
        // shallow/deep colours.
        //
        // ON by default since 2026-09-06 (owner decision, after WRL-002 gave the volume the
        // same optics as the surface). The Water tab checkbox still switches it off.
        //
        // The flag gates BOTH the fullscreen compositor and the water shader's own underwater
        // tint (they share fft_control.w), so with it off a submerged view is the plain scene
        // seen through the water surface, with no volume and no distance fog either. That is
        // deliberate: half the effect is not better than none of it.
        bool underwaterEffect = true;

        // Underwater tuning, all live from the Water tab so the look can be dialled in without
        // a rebuild. Every one of these is inert while underwaterEffect is off.
        //
        // The two depth thresholds are the hysteresis band around the local (wave-displaced)
        // surface: the effect turns ON once the eye is enterDepth below it and stays on until
        // the eye is exitDepth above it. They are asymmetric on purpose — equal thresholds make
        // the effect flicker when the eye rides exactly on a moving crest.
        float underwaterEnterDepth = 0.03f; // m below the surface before the effect engages
        float underwaterExitDepth = 0.08f;  // m above the surface before it releases
        // How far above sea level the compositor still runs. It has to run slightly dry so a
        // view straddling the surface can be classified per ray; well past the crest height it
        // just pays for froxel and caustic dispatches that produce no water path.
        float underwaterEngageBand = 1.5f;
        // Absorption density multiplier. 1.0 is the tuned default; lower is clearer water.
        float underwaterDensity = 1.0f;
        // 1 = absorption hue derived from the authored deep colour, so the volume is the same
        // substance as the surface. 0 = the fixed (0.280, 0.065, 0.020) curve the effect used
        // before, which had no relation to the water you swam into. Between the two it blends.
        float underwaterColorBias = 1.0f;
        // Gain on the caustic pattern cast onto nearby seabed geometry. 1.0 = tuned default.
        float underwaterCausticGain = 1.0f;

        // WTR-036C / WTR-037 — FFT Cascade Preset (0 = Production Non-Harmonic 4-Cascade, 1 = GodotOceanWaves Reference Style, 2 = Legacy Harmonic 4-Cascade).
        // The GodotOceanWaves-derived TMA/JONSWAP setup is the gameplay default.  The
        // non-harmonic production layout remains available for A/B testing, but should
        // never silently replace the reference look the water system is targeting.
        int cascadePreset = 1;

        // WTR-003 — water debug view selector (dev-only; the Water tab "Debug views" section).
        // 0 = normal shading; any other value is a WgrWaterDebugView index that the wgpu water
        // shader maps to an on-surface diagnostic (FFT/interaction/foam/reflection/refraction).
        // Backend-agnostic: non-wgpu engines ignore it. Written to WgrWaterParams.debug_params.x.
        int debugView = 0;

        // WTR-004 — standard test scene selector (dev-only; 0 = None / Authored, 1..10 = WTR-Test-01..10).
        int testScene = 0;

        // WTR-001 — deterministic water debug controls (dev-only; the Water tab "Debug" section).
        // All freezes are renderer-local: they override the UBO time/dt the shader sees, without
        // touching Glob.time (gameplay / net clock) or any non-water subsystem other than the cloud
        // wind offset + underwater caustic clock (which ride the same water sim clock by design).
        // Use these to make a single frame reproducible across launches for before/after captures
        // and shader-diff work (WTR-002 GPU timestamps and WTR-003 debug views rely on this).
        struct Freeze
        {
            // Master switches (each gate is independent so subsystems can be frozen in combination).
            bool freezeTime = false;          // hold the water-sim clock at fixedTime
            bool freezeFft = false;           // skip Fft::dispatch (the spectrum holds at its last state)
            bool freezeInteraction = false;    // skip Interaction::dispatch (dt forced to 0 beforehand)
            bool freezeFoam = false;          // skip Foam::dispatch
            bool freezeClouds = false;        // hold the cloud wind world offset at fixedTime
            bool freezeWeather = false;       // hold the rain/calmness weather vector sent to the
                                             // interaction solver (no per-frame recomputation today,
                                             // but reserved so future weather threading stays A/B-safe)
            // Fixed sim time (seconds) substituted for Glob.time when any freeze*that uses the clock
            // is enabled. One value drives water waves, interaction now-impulse, cloud wind offset,
            // and the underwater caustic clock, so all four stay coherent for a single test frame.
            float fixedTime = 0.0f;
            // Deterministic FFT random seed override (replaces fft_control[1]; -1 = use the
            // authored 1337 default so the spectrum only re-seeds when the user asks for it).
            // Toggling the value (even back) rewrites h0 on the next Fft::dispatch.
            int fftSeed = -1;
            // Fixed delta time (seconds) for the interaction solver when freezeInteraction is OFF.
            // 0 = use the live frame delta clamped to 1/30 (existing behaviour). Non-zero fixes the
            // simulation step so the ripple solver runs at the same rate regardless of render fps.
            float fixedDelta = 0.0f;
            // Repeatable-camera-path foundation (WTR-001: smallest necessary scaffolding). The full
            // camera-path recorder is a separate work package; here we expose a single integer that,
            // when >= 0, the renderer logs each frame along with the water UBO digest so two runs are
            // comparable frame-by-frame. The actual camera-driver work is WTR-Test-* (WTR-004).
            int cameraPathFrame = -1;
        } freeze;
    };
    // True on backends with a GPU water renderer (wgpu with water enabled); gates the tab.
    virtual bool SupportsWater() const { return false; }
    virtual WaterSettings GetWaterSettings() const { return {}; }
    virtual void SetWaterSettings(const WaterSettings& /*s*/) {}
    // Authored menu sea is transient; it never writes the per-map water profile.
    virtual void SetMenuSeaSceneActive(bool /*active*/) {}

    // GPU-driven cull DEBUG toggles (ImGui Culling tab). drawSpheres = render each retained
    // instance's frustum-cull sphere as a wireframe; disableFrustum = skip the GPU frustum
    // test (discriminator for the "objects vanish at certain pitches" bug).
    struct CullDebugSettings
    {
        bool drawSpheres = false;
        bool disableFrustum = false;
        // GPU Hi-Z occlusion culling (docs/gpu-culling-and-depth-plan.md §5): the color pass
        // draws only the retained instances not hidden by the depth-prepass occluders (terrain +
        // drawn objects). Default on (matches the WGR_GPU_OCCLUSION Rust default); when on, the
        // engine's built-in software occlusion (EnableObjOcc) is forced off — GPU Hi-Z replaces it.
        bool occlusion = true;
        // Momentary (a button, not a state): log every retained instance near the camera —
        // registration-time position vs the object's LIVE Position() vs the terrain surface.
        // Consumed by SetCullDebugSettings; never stored true.
        bool dumpNearby = false;
    };
    // True on the wgpu backend with GPU-driven rendering enabled; gates the Culling tab.
    virtual bool SupportsCullDebug() const { return false; }
    virtual CullDebugSettings GetCullDebugSettings() const { return {}; }
    virtual void SetCullDebugSettings(const CullDebugSettings& /*s*/) {}

    // Per-frame: suppress drawing the retained GPU-driven world set (its objects live in a
    // GPU-resident scene and otherwise draw every frame regardless of the per-frame 3D lists
    // World::Simulate skips). Raised while the world must not be shown (mission editor,
    // loading, shutdown) so those frames clear to black with no clutter leaking behind the 2D
    // UI. Default no-op. NOTE: appended at the class end on purpose (see the vtable-slot note).
    virtual void SuppressWorldObjects(bool /*suppress*/) {}

    // Tri-state GPU-driven coverage (§12) + proxy query (§12d). APPENDED HERE at the class end to
    // keep every existing vtable slot stable (vtable-slot note above) — do NOT move these up next
    // to SceneObject*/GpuDrivenObject, or ccache partial recompiles misdispatch (breaks 3D UI).
    // Only EngineWgpu with WGR_GPU_DRIVEN overrides them; everything else keeps objects on the CPU.
    virtual GpuDrawCoverage GpuDrivenCoverage(const Object* /*obj*/) const { return GpuDrawCoverage::None; }
    // §12d: is proxy `proxyIndex` at parent LOD `level` drawn by the GPU retained scene (as a child
    // instance)? Object::DrawProxies skips it if so, to avoid double-drawing the furniture.
    virtual bool GpuDrivenProxy(const Object* /*parent*/, int /*level*/, int /*proxyIndex*/) const { return false; }
    // RFG-095b: the set of proxies a parent should draw changed after it was registered -- the
    // furniture promotion (MapThings) runs AFTER the buildings were added, so the GPU proxy
    // instances it hides were already emitted. The renderer re-emits from the current hidden set.
    // Appended at the class end for the same vtable-slot reason as the two above.
    virtual void SceneObjectProxiesChanged(Object* /*obj*/) {}
    // RFG-099: a WORLD model (object stream cold load, native loader) finished loading and is
    // about to build its CPU vertex buffers. The retained renderer registers it first, so those
    // buffers can share the retained mesh instead of each becoming a private copy. Measured
    // before this hook under native streaming: 5944 direct-path meshes, 863 MB, none of whose
    // levels belonged to a registered model -- the shapes were converted before any object
    // using them was admitted. Appended at the class end (vtable-slot rule).
    virtual void WorldShapeLoaded(LODShapeWithShadow* /*shape*/) {}

    // WTR-002 — GPU water-pipeline pass timings (Water tab, dev-only). Copies up to maxCount
    // per-region times in milliseconds into outMs (indexed by the renderer's fixed region
    // contract; -1 = pass never ran / reserved slot) and returns the region count, or 0 when
    // the backend has no GPU timers. Names come from GetWaterGpuTimingName so the overlay
    // stays backend-agnostic. APPENDED at the class end (vtable-slot note above).
    virtual int GetWaterGpuTimings(float* /*outMs*/, int /*maxCount*/) const { return 0; }
    virtual const char* GetWaterGpuTimingName(int /*region*/) const { return ""; }
    // Live WGPU adapter/runtime flags for the Profile diagnostic tab. Appended to
    // preserve existing vtable slots; zero means unavailable/non-WGPU.
    virtual uint32_t GetRuntimeCapabilityFlags() const { return 0; }

    // SECOND CLOUD LAYER (high cirrus) on/off + flat-vs-volumetric, from the Sky tab.
    // SkySettings carries the two flags so they round-trip through Get/SetSkySettings and the
    // "Reset to defaults" button; this pushes them to the backend. A separate entry point rather
    // than another lane in the sky look block, because that block's size is part of the renderer's
    // ABI handshake (the same reason cloud-shadow strength, stars and lens flare have their own).
    // APPENDED at the class end (vtable-slot note above).
    virtual void SetSkyCirrus(bool /*enabled*/, bool /*volumetric*/) {}

    // SECOND CLOUD LAYER shape: puffiness (0 fibrous veil .. 1 lumpy cirrocumulus, 0.5 = shipped)
    // and how far it varies from that on its own over world time (0 = steady). Same reasoning as
    // SetSkyCirrus above — SkySettings carries both so they round-trip through Get/SetSkySettings
    // and "Reset to defaults", and this is a separate entry point so no ABI-checked struct grows.
    // APPENDED at the class end (vtable-slot note above).
    virtual void SetSkyCirrusPuffiness(float /*puffiness*/, float /*variation*/) {}

    // GOD RAYS (crepuscular rays / light shafts), from the Sky tab. SkySettings carries the whole
    // block so it round-trips through Get/SetSkySettings and "Reset to defaults"; this pushes it
    // to the backend. Same reasoning as SetSkyCirrus above — a separate entry point, so no
    // ABI-checked struct grows. APPENDED at the class end (vtable-slot note above).
    virtual void SetGodRays(bool /*enabled*/, float /*intensity*/, float /*density*/, float /*distance*/,
                            float /*g*/, float /*cloudInfluence*/, int /*steps*/, int /*resDiv*/)
    {
    }

    // GetWaterGpuTimings returns ONE shared region array covering every timed
    // subsystem; each debug tab slices its own range. Mirrors WgrGpuTimerRegion
    // in wgpu_renderer.hpp (append only, never reorder).
    enum : int
    {
        kWaterGpuRegionBegin = 0,
        kWaterGpuRegionEnd = 19,
        kGrassGpuRegionBegin = 19,
        kGrassGpuRegionEnd = 25,
        kFrameGpuRegionTotal = 25,
        // LIT-020 — sliced by the Interior Sky tab.
        kInteriorSkyGpuRegionBegin = 26,
        kInteriorSkyGpuRegionEnd = 28,
        // LIT-010 — sliced by the Amb. Occlusion tab.
        kGtaoGpuRegionBegin = 28,
        kGtaoGpuRegionEnd = 31,
        // PERF-004 - main terrain, separate from reflected terrain.
        kTerrainGpuRegionBegin = 31,
        kTerrainGpuRegionEnd = 33,
        // PERF-005 - object rendering, sliced by the Objects tab. kObjectGpuContainerBegin
        // marks where the two CONTAINER regions start: they overlap the leaves before them by
        // construction, so any code summing regions to compare against kFrameGpuRegionTotal
        // MUST skip [kObjectGpuContainerBegin, kObjectGpuContainerEnd). Getting this wrong
        // produces a "total" larger than the frame, which is worse than no measurement.
        kObjectGpuRegionBegin = 33,
        kObjectGpuContainerBegin = 40,
        kObjectGpuContainerEnd = 42,
        kShadowCascadeGpuRegionBegin = 42,
        kShadowCascadeGpuRegionEnd = 46,
        kObjectGpuRegionEnd = 46,
        // FAR INSTANCE TIER - the proxy sweep over every authored placement, and the draw of
        // what survived it. Appended past kObjectGpuRegionEnd so the Objects tab's slice keeps
        // meaning exactly what it meant: these proxies are never Objects.
        kFarGpuRegionBegin = 46,
        kFarGpuRegionCull = 46,
        kFarGpuRegionDraw = 47,
        kFarGpuRegionEnd = 48,
        // COLOUR SUB-PASS ATTRIBUTION. The container measured 12.824 ms on perf_abel @1920x1080
        // with every leaf inside it summing to 3.777 -- 9.05 ms, 70% of the pass, named by
        // nothing. These four name it: the cloud march and its composite, and the two replay
        // containers. CPU-replayed direct draws are DERIVED (pre-water minus terrain minus
        // grass) because Plan3dOp::Draw3D is per-draw and a region is one bracket per frame.
        kCloudGpuRegionMarch = 48,
        kCloudGpuRegionComposite = 49,
        kOpsGpuRegionPreWater = 50,
        kOpsGpuRegionPostWater = 51,
        // POST CHAIN + UI. 2.85 ms of a 23.08 ms frame at 1920x1080 (12.3%) belonged to no
        // region once the colour sub-pass was attributed. All outermost.
        kPostGpuRegionHdrResolve = 52,
        kPostGpuRegionGodRays = 53,
        kPostGpuRegionBloom = 54,
        kPostGpuRegionExposure = 55,
        kPostGpuRegionTonemap = 56,
        kPostGpuRegionUi2d = 57,
        // ATMOSPHERE. PERF-010 narrowed a ~3 ms per-frame residual that is independent of
        // scene cost, pass count and RESOLUTION; these five were the resolution-independent
        // per-frame GPU work with no region at all. Every one writes a FIXED-SIZE target --
        // LUT, froxel grid, equirect env map, nine SH coefficients -- which is exactly why
        // the residual did not care how big the window was. All OUTERMOST: they run before
        // the colour sub-pass opens. Appended, never inserted: these are FFI slot numbers.
        kSkyGpuRegionLuts = 58,
        kSkyGpuRegionFroxel = 59,
        kSkyGpuRegionEnv = 60,
        kSkyGpuRegionSh = 61,
        kSkyGpuRegionDraw = 62,
        // LGT-026 - the local-light shadow views, all of them, as one region. The four
        // per-cascade rows above stop at MAX_CASCADES, so the tiled local atlas was untimed.
        kLocalShadowGpuRegion = 77,
        // TW-WATER W2 - the Tidewater Native ocean's compute (spectrum, FFT + mips).
        kTidewaterGpuRegionSpectrum = 78,
        kTidewaterGpuRegionFft = 79,
        kTidewaterGpuRegionShoreSim = 80,
        kTidewaterGpuRegionWake = 81,
        kTidewaterGpuRegionCaustics = 82,
        kTidewaterGpuRegionBreakers = 83,
        kWeatherCoverNearCull = 84,
        kWeatherCoverFarCull = 85,
        kWeatherCoverNearDraw = 86,
        kWeatherCoverFarDraw = 87,
        kLayeredFog = 88,
        kGpuRegionEnd = 89, // WGR_GPU_TIMER_REGION_COUNT: append-only shared timing contract
        // Regions that are physically enclosed by another region (see GpuTimerContainedBy).
        kGrassGpuRegionShadow = 24,
        kTerrainGpuRegionColor = 32,
        kGrassGpuRegionColor = 23,
        kGrassGpuRegionPrepass = 22,
        kNoGpuRegion = -1,
    };

    // Whether a region DELIBERATELY overlaps regions inside it, so a reader knows its
    // milliseconds are an envelope and not an addend. Was an `i >= begin && i < end` range
    // test at the one call site, which silently stopped being true the moment a container was
    // added outside that contiguous block - as the two ops containers are.
    static constexpr bool IsGpuTimerContainer(int region)
    {
        return (region >= kObjectGpuContainerBegin && region < kObjectGpuContainerEnd) ||
               region == kOpsGpuRegionPreWater || region == kOpsGpuRegionPostWater;
    }

    // PERF-005 - region nesting, and the ONLY correct way to total the timing array.
    //
    // Several regions physically enclose others: the two segment containers enclose the terrain,
    // grass and GPU-driven object draws recorded inside their render passes, and each shadow
    // cascade encloses the grass blades drawn into that cascade's depth map. Adding every region
    // up therefore over-counts. The rule is:
    //
    //     frame total ~= sum of every region whose GpuTimerContainedBy() is kNoGpuRegion
    //
    // i.e. sum the OUTERMOST regions only. This is not a rounding concern: on Everon's
    // 00training a naive sum double-counts grass-shadow (0.74 ms) and the entire colour
    // sub-pass (2.13 ms), producing a "total" LARGER than the frame it claims to explain -
    // which reads as a measurement rather than as the bug it is. Mirrors
    // WgrGpuTimerContainedBy in wgpu_renderer.hpp; keep the two in step.
    // Returns the enclosing region index, or kNoGpuRegion when the region is outermost.
    static constexpr int GpuTimerContainedBy(int region)
    {
        // Inside the depth/normal prepass render pass.
        if (region == kTerrainGpuRegionBegin || region == kGrassGpuRegionPrepass ||
            (region >= kObjectGpuRegionBegin + 3 && region <= kObjectGpuRegionBegin + 4))
            return kObjectGpuContainerBegin;
        // Inside the 3D colour sub-pass. The far tier draws there too, immediately after the
        // GPU-driven objects; its CULL is a standalone compute pass and stays outermost.
        // Clouds and the two replay containers are inside it as well; a container may itself
        // be contained, and all the sum rule needs is that neither is outermost.
        if (region == kTerrainGpuRegionColor || region == kGrassGpuRegionColor ||
            region == kFarGpuRegionDraw || region == kCloudGpuRegionMarch ||
            region == kCloudGpuRegionComposite || region == kOpsGpuRegionPreWater ||
            region == kOpsGpuRegionPostWater ||
            // Water draw and god rays were MISSING from this list until 2026-08-31. Unlike
            // the rows above they open their own render pass on the encoder instead of
            // being bracketed inside one, so they read as standalone at the call site --
            // but containment here is about the TIMESTAMPS, and both fall inside the
            // colour segment's bracket. Reported as outermost they were double-counted:
            // on perf_water the sum came to 20.485 ms against a 20.322 ms frame, a
            // NEGATIVE residual, the parts explaining more than the whole. See PERF-010.
            // Region 15 is the water surface pass; the water block (0..18) is indexed
            // numerically here because it has no per-row constants, only Begin/End.
            region == kWaterGpuRegionBegin + 15 || region == kPostGpuRegionGodRays ||
            (region >= kObjectGpuRegionBegin + 5 && region <= kObjectGpuRegionBegin + 6))
            return kObjectGpuContainerBegin + 1;
        // Grass casts into the cascade depth maps from INSIDE each cascade's render pass, so its
        // one region already sits inside the per-cascade brackets. Attributed to cascade 0
        // because a region can name only one parent, and grass shadows are a near-field effect.
        if (region == kGrassGpuRegionShadow)
            return kShadowCascadeGpuRegionBegin;
        // The whole-frame envelope is never part of a sum - it IS the thing summed against.
        if (region == kFrameGpuRegionTotal)
            return kFrameGpuRegionTotal;
        return kNoGpuRegion;
    }

    // PERF-005 - backend-agnostic mirror of WgrObjectStats. Counts come from the GPU cull's
    // atomics via a non-blocking readback (so they lag the displayed frame by ~2-3 frames)
    // except directCalls/directTris, which are counted on the CPU during the colour replay.
    struct ObjectStatsOut
    {
        uint32_t registeredInstances = 0;
        // False until a readback lands. Zeros with valid == false mean "not measured yet",
        // which is a different statement from "nothing drew" - report them differently.
        bool valid = false;

        uint32_t mainInstances = 0, mainRecords = 0, mainDraws = 0, mainTris = 0;
        uint32_t colorInstances = 0, colorRecords = 0, colorDraws = 0, colorTris = 0;
        uint32_t colorDrawsSolid = 0, colorDrawsAlpha = 0;
        uint32_t colorTrisSolid = 0, colorTrisAlpha = 0;
        uint32_t shadowInstances[4] = {}, shadowDraws[4] = {}, shadowTris[4] = {};
        // LOD 0..6 plus a "7 or coarser" bucket, over the MAIN view's survivors.
        uint32_t mainLodHist[8] = {};
        // What the main view would cost in triangles at whatIfScale[i] x the current detail
        // multiplier (REN-VEG-004); the governor's per-step cost prediction for the retained set.
        uint32_t mainWhatIfTris[4] = {};
        float whatIfScale[4] = {};
        // Object fragment census (REN-OBJ-003): colour, colour cutout, prepass, prepass cutout;
        // then REN-ATM-001's atmosphere-by-family words -- [4] opaque with the aerial-perspective
        // term, [5] cutout with it, [6] the vegetation subset of [5], [7] shaded with none.
        uint32_t fragments[8] = {};
        uint32_t directCalls = 0, directIndirectCalls = 0, directInstances = 0, directTris = 0;
        // Instance handles used after their instance was removed, refused and counted since
        // startup. Nonzero is a bug on THIS side of the boundary, not in the renderer.
        uint64_t staleInstanceOps = 0;
    };

    // PERF-005 - per-region CPU ENCODE ms, indexed exactly like GetWaterGpuTimings
    // (-1 = the region was not recorded this frame). Answers a different question from the GPU
    // rows: how long the CPU spent RECORDING the region, not how long the GPU ran it.
    // APPENDED at the class end (vtable-slot note above).
    virtual int GetCpuTimings(float* /*outMs*/, int /*maxCount*/) const { return 0; }
    // PERF-005 - this frame's object accounting. False when the backend has none.
    virtual bool GetObjectStats(ObjectStatsOut& /*out*/) const { return false; }

    // FAR INSTANCE TIER - one cheap proxy per AUTHORED PLACEMENT, drawn straight from the
    // placement rows without ever creating an Object.
    //
    // The object residency window stops far short of the view distance (~900 m on Everon,
    // ~550-711 m on Chernarus). Past it a placement is not a dim, coarse object: it does not
    // exist at all, so the world is bare ground and every asset pops in as the camera closes.
    // This carries the whole island's silhouette for the cost of one GPU sweep.
    //
    // Layout is deliberately the WgrFarInstance ABI record (checked with a static_assert in the
    // backend) so the world loader's array can be handed to the renderer without a repack of a
    // few million rows. Backends that have no such tier ignore it, which is the whole fallback.
    struct FarProxyInstance
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float height = 0.0f;
        float radius = 0.0f;
        uint32_t colour = 0xFFFFFFFFu;
        uint32_t flags = 0; // bit0: 0 = card (vegetation), 1 = prism (structure)
        uint32_t pad = 0;
    };
    // Replace the whole far-proxy set. Called ONCE per world load. Empty releases it.
    virtual void SetFarProxyInstances(const FarProxyInstance* /*data*/, size_t /*count*/) {}

    // SECOND CLOUD LAYER look: how MUCH of it there is (0 wisps .. 0.5 shipped .. 1 full veil) and
    // how far it resembles the FIRST layer (0 = high ice cloud, 1 = a second cumulus-like deck).
    // Same reasoning as SetSkyCirrus above — SkySettings carries both so they round-trip through
    // Get/SetSkySettings and "Reset to defaults", and this is a separate entry point so no
    // ABI-checked struct grows. APPENDED at the class end (vtable-slot note above).
    virtual void SetSkyCirrusLook(float /*amount*/, float /*matchDeck*/) {}

    // Fraction of the draw distance at which distance fog reaches FULL (see SkySettings::
    // fogFarClose). Same reasoning as SetSkyCirrus above for being its own entry point rather
    // than a lane in an ABI-checked struct. APPENDED at the class end (vtable-slot note above).
    virtual void SetFogFarClose(float /*close*/) {}

    // SECOND cloud layer EDGE SOFTNESS (see SkySettings::cirrusSoftness). Same reasoning as
    // SetSkyCirrus above for being its own entry point rather than a lane in an ABI-checked
    // struct. APPENDED at the class end (vtable-slot note above).
    virtual void SetSkyCirrusSoftness(float /*softness*/) {}

    // REN-TEMP-001 §6.7 — live temporal/upscaler tuning. APPENDED at the class end
    // (vtable-slot note above). Wgpu-only; the base is inert so GL33/tools need nothing.
    // REN-THR-015 -- the render thread, as something the dev panel can switch and watch.
    //
    // The three times are measured, not modelled. `workerBusyMs` is wall time inside the
    // consumer block on whichever thread ran it. `mainWaitMs` is how long the producer sat
    // blocked on it. `overlapMs` is the DIFFERENCE -- worker time the producer did NOT wait
    // for -- which is zero by construction in lockstep and is the whole point of the mode
    // that follows it. Deriving overlap rather than timing it separately means the three can
    // never disagree with each other.
    struct RenderThreadInfo
    {
        bool available = false; // backend can run one at all (WGPU yes, GL33 no)
        bool enabled = false;   // one is running right now
        float workerBusyMs = 0.0f;
        float mainWaitMs = 0.0f;
        float overlapMs = 0.0f;
        // Busy fraction over the reporting window, 0..1. `mainBusy` excludes the wait, so a
        // producer that spends its frame blocked reads LOW here and the render thread reads
        // high -- which is the picture lockstep should paint.
        float workerBusyFrac = 0.0f;
        float mainBusyFrac = 0.0f;
        // REN-THR-016. `overlapEnabled` is the mode; `lagFrames` is how far the worker was
        // behind when the producer last published (0 kept up, 1 still on the previous
        // frame); `queueWaitMs` is how long the producer BLOCKED because both ring slots
        // were busy. In lockstep queueWaitMs is the whole render, because the producer waits
        // for all of it; in overlap it is only the part the renderer could not absorb, so it
        // going above zero is the direct reading of "the renderer cannot keep up".
        bool overlapAvailable = false;
        bool overlapEnabled = false;
        float lagFrames = 0.0f;
        float queueWaitMs = 0.0f;
        // REN-THR-013: how often and how long the producer had to open the window itself
        // (a direct renderer call while the worker was busy), and who did it first last frame.
        float lazyWaitMs = 0.0f;
        float lazyWaitsPerFrame = 0.0f;
        const char* lazyOpener = "none";
    };
    virtual RenderThreadInfo GetRenderThreadInfo() const { return {}; }
    /// Switch the render thread at a frame boundary. Takes effect on the next frame; the
    /// engine never starts or stops a worker while one holds the renderer handle.
    virtual void SetRenderThreadEnabled(bool /*on*/) {}
    /// Let the producer run ahead instead of waiting for each frame. Requires the render
    /// thread; switched at the same frame boundary and drained before it is turned off.
    virtual void SetRenderOverlapEnabled(bool /*on*/) {}

    virtual bool SupportsTemporalTuning() const { return false; }
    virtual TemporalSettings GetTemporalTuning() const { return {}; }
    virtual void SetTemporalTuning(const TemporalSettings& /*t*/) {}
    virtual TemporalInfo GetTemporalInfo() { return {}; }
    // WHY DLSS is inactive, as one specific sentence ("nvngx_dlss.dll not found next to
    // the executable", "adapter is Intel UHD Graphics, not NVIDIA", "NGX init failed
    // (0xbad0000b UnableToInitializeFeature)"), or "active". Empty when the backend has
    // no such readout. The dev panel and the capture metrics print it verbatim.
    virtual std::string DlssStatusReason() { return {}; }

    // Visual-only authored world instances. Append virtuals to preserve existing slots.
    virtual bool SceneFarObjectCreated(uint32_t, LODShapeWithShadow*, const Matrix4&) { return false; }
    virtual void SceneFarObjectRemoved(uint32_t) {}
    virtual void ClearFarObjects() {}

    // Owner-only, bounded scheduling observation; no reads/uploads or coverage promise.
    // Appended to preserve all existing virtual slots. Other renderers retain legacy charging.
    virtual render::RegistrationCost QueryRegistrationCost(const LODShapeWithShadow*) const
    { return render::RegistrationCost::Reusable; }
    // Appended owner observation of already-loaded texture metadata only.
    // Work budget is shared across the recentre. Default-nullptr permits no loading.
    // Optional owner preflight may initialize at most two missing cached-material sources
    // under the explicit warm-material flag; no material resolution or GPU upload.
    virtual void CaptureWarmTextureReads(const LODShapeWithShadow*,
        std::vector<WarmTextureRead>&, size_t&, size_t* = nullptr) const {}


    // Exact opt-in owner diagnostic for one existing real material stage. No source
    // load, texture upload, eviction, store consumption or strong lease escapes.
    enum class WarmPatnikProbeStatus : uint8_t { Disabled, WrongOwner, Unsupported, Ready };
    struct WarmPatnikProbeReport
    {
        WarmPatnikProbeStatus status = WarmPatnikProbeStatus::Disabled;
        bool textureFound = false, targetKeyMatched = false, gpuResident = false;
        bool uploadTried = false, initializedMips = false, initializedBinding = false;
        bool currentBinding = false, canWarmPrepare = false;
        bool storeEntry = false, storeChain = false, storeWarmBound = false;
        bool storePublicationValid = false, storeWithinTtl = false, storeSameMember = false;
        bool storeUploadedMark = false, traceEnabled = false, traceTruncated = false;
        uint32_t traceExactToken = 0, traceSources = 0, traceRows = 0;
        uint64_t storeGeneration = 0;
    };
    // Explicit bounded private-allocation diagnostic; never unregisters a world
    // model. Snapshot reads a copied renderer-cut observation, not live FFI.
    enum class SharedMeshFixtureAction { Begin, DropNewest, ReleaseProducer, DropOldest, Observe, Snapshot, Abort, BeginStandalone };
    struct SharedMeshFixtureReport
    {
        GeometryReportStatus status = GeometryReportStatus::Unsupported;
        uint64_t requestId = 0, observedRequestId = 0, producer = 0, rendererHandle = 0, knownPayloadBytes = 0;
        uint32_t meshState = 0, cpuBorrowers = 0;
        bool producerOwned = false, sourceCopied = false, privateAllocation = false;
        bool ledgerEnabled = false, ledgerRecordObserved = false, ledgerScheduledRetired = false, ledgerProducerOwned = false;
        uint64_t ledgerCpuBorrowers = 0;
        bool standalone = false;
        uint64_t witnessKnownMetadataBytes = 0, witnessPeakKnownMetadataBytes = 0, witnessCapacityRefused = 0;
    };
    virtual SharedMeshFixtureReport ControlSharedMeshLifetimeFixture(SharedMeshFixtureAction, const Shape* = nullptr)
    { return {}; }

    // Ready is a returned-main-frame record cut, not all-pass/pixel or GPU-free proof.
    enum class GeometryPageFixtureAction { Begin, Coarse, Fine, Observe, Snapshot, Abort, BeginClod, Fallback, BeginPaged, RequestFine, PollFine, CancelFine, HoldFine, ReleaseFine, EvictFine, BeginClodPaged, BeginDiskClodPaged, RequestCoarse, PollCoarse, BeginOriginalDiskClodPaged, FollowCamera, StopCamera, PollFinePrefetch, RequestFinePrefetch, FollowProjected, PrivateAutoFine, PrivateAutoCoarse, BeginOriginalHierarchy, StepHierarchy, RootHierarchy, FineHierarchy, IntermediateHierarchy, LocalHierarchy, BeginOriginalHierarchyDisk, ReleaseHierarchyNonroot, ProjectHierarchy, FollowHierarchy, StopHierarchy, ProbeTextureReuseOnly, ProbeRetailOdol7, PollRetailRecords, ReleaseRetailRecords, RefillRetailRecords, AbortRetailRecords, SnapshotRetailRecords, BeginRetailVisible, RootRetailVisible, FineRetailVisible, PollRetailVisible, RemoveRetailVisible, FallbackRetailVisible, ProbeRetailColdWorld, BeginRetailWorldVisible, PollRetailWorldVisible, RootRetailWorldVisible, FineRetailWorldVisible, RestoreRetailWorldVisible, MoveRetailWorldVisible, RemoveRetailWorldVisible, FollowRetailWorldVisible, StopRetailWorldVisible };
    struct GeometryPageSourceIdentity
    {
        std::array<uint8_t,32> sha256{};
        uint64_t geometryOptions=0,materialOptions=0;
        uint32_t producerVersion=0,coarseRepresentation=0,fineRepresentation=0,vertexLayout=0,materialMapping=0;
    };
    // Explicit startup admission probe only; no renderer allocation or disk-frontier consumer.
    enum class GeometryPageOriginalStatus : uint32_t {
        Disabled, Admitted, Unsupported, Invalid, Capacity, WrongOwner, IoFailure, IdentityMismatch, Busy
    };
    struct GeometryPageSurfaceStartupInput {
        std::array<char,1024> manifestPath{},certificatePath{};
        std::array<uint8_t,32> manifestSha256{},certificateSha256{};
    };
    struct GeometryPageOriginalStartupInput {
        std::array<char,1024> originalPath{};
        uint64_t expectedRawBytes=0;
        GeometryPageSourceIdentity expectedOriginal;
        std::shared_ptr<const GeometryPageSurfaceStartupInput> surface; // Optional explicit startup transport only.
    };
    struct GeometryPageOriginalAdmissionReport {
        GeometryPageOriginalStatus status=GeometryPageOriginalStatus::Disabled;
        uint64_t epoch=0, originalSourceBytes=0, knownRetainedBytes=0;
        bool surfaceCertificateValidated=false;
        uint32_t surfaceCertificateStatus=0; // SurfaceAdmission::Status+1; zero means not requested.
        uint64_t surfaceCertificateKnownBytes=0;
        bool snapshotValidated=false, consumed=false; // Single source admission cannot rearm after fixture Abort.
    };
    // Full source-key numbers in decimal: geometry:material:producer:coarse:fine:layout:mapping.
    static bool ParseGeometryPageOriginalStartupInput(const std::string& path, const std::string& sha256,
        uint64_t bytes, const std::string& key, GeometryPageOriginalStartupInput& destination);
    static GeometryPageOriginalStatus ReadGeometryPageOriginalSnapshot(const GeometryPageOriginalStartupInput&,
        std::vector<uint8_t>& destination);
    static bool GeometryPageOriginalIdentityMatches(const GeometryPageSourceIdentity&,
        const GeometryPages::SourceIdentity&);
    virtual GeometryPageOriginalAdmissionReport AdmitGeometryPageOriginalStartup(const GeometryPageOriginalStartupInput&)
        { GeometryPageOriginalAdmissionReport r; r.status=GeometryPageOriginalStatus::Unsupported; return r; }
    virtual GeometryPageOriginalAdmissionReport SnapshotGeometryPageOriginalAdmission() const { return {}; }
    // Explicit private fixture arguments FROM a separately verified offline
    // producer; no Engine file/manifest reads and no retail eligibility authority.
    struct GeometryPageDiskFixtureInput
    {
        std::array<char,1024> path{};
        GeometryPageSourceIdentity original,selected;
        std::array<uint8_t,32> helperSha256{};
        std::array<char,41> libraryRevision{};
        uint64_t layoutKey=0;
        uint64_t sourceAdmissionEpoch=0,originalSourceBytes=0;
        uint32_t originalSubsetVersion=0; // schema2 original-file mode only; schema1 synthetic stays unchanged.
        uint32_t schemaVersion=0,pilotVersion=0,codecSchema=0,vertexBytes=0,clodAdapter=0,ramAdapter=0;
        uint32_t formatVersion=0,algorithmVersion=0,clusterVertices=0,clusterTriangles=0,pageBytes=0;
    };
    struct GeometryPageCameraDemandInput {
        double nearDistance=30,farDistance=50;
        double enterUpper=2,leaveUpper=1; // Experimental pair-discrepancy preparation preference only.
        uint32_t dwellMs=200,retryMs=250;
    };
    struct GeometryPageHierarchyDemandInput {
        std::array<uint8_t,32> packageSha256{};
        uint64_t sourceAdmissionEpoch=0,pageEpoch=0,expectedRequestId=0;
        uint64_t frameGeneration=0;
        double pixelIndicatorAllowance=1;
        bool privateBinaryOwner=false; // Only the serialized owner sets this; not a harness input.
        uint32_t groupCount=0;
        std::array<float,64> groupThresholds{};
    };
    struct GeometryPageHierarchyDiskFixtureInput {
        static constexpr uint64_t MaxMetadataBytes=32768;
        static constexpr uint64_t MaxFileBytes=MaxMetadataBytes+64ull*65536;
        std::array<char,1024> path{};
        GeometryPageSourceIdentity original;
        std::array<uint8_t,32> packageSha256{},metadataSha256{};
        std::vector<uint8_t> metadataBytes;
        uint64_t fileBytes=0,sourceAdmissionEpoch=0,originalSourceBytes=0;
    };
    struct GeometryPageFixtureReport
    {
        GeometryReportStatus status=GeometryReportStatus::Unsupported;
        // One-shot live TextureWgpu binding seam diagnostic; no geometry mutation.
        bool textureReuseProbe=false,textureReusePassed=false,textureReuseHeldDenied=false;
        bool textureReuseResidentStable=false,textureReuseUnexpectedDenied=false,textureReuseRetriesResident=false;
        uint32_t textureReuseAllowed=0,textureReuseDenied=0,textureReuseOwnerCreates=0;
        bool retailSourceProbe=false,retailSourceExported=false,retailSourceOtherPassesRequired=true;
        uint32_t retailSourcePrepareStatus=0,retailSourceExportStatus=0;
        // Explicit source diagnostic records only: no instance or world routing.
        bool retailRecordOnly=false,retailPixelSourceVerified=false,retailRecordDistinctCuts=false;
        bool retailDiagnosticReferenceAllocated=false;
        bool retailVisiblePilot=false,retailVisiblePresent=false,retailVisibleRemoved=false,retailVisibleReturned=false;
        uint64_t retailVisibleBirth=0,retailVisibleRequest=0,retailVisibleFrame=0;
        uint32_t retailVisibleRenderer=UINT32_MAX,retailVisibleSlot=UINT32_MAX,retailVisibleModel=UINT32_MAX;
        std::string retailVisiblePhase;
        // One actual world placement: CPU row pair and returned-camera proof only.
        bool retailWorldVisiblePilot=false,retailWorldOriginalOtherViews=false,retailWorldPagePresent=false;
        bool retailWorldRestored=false,retailWorldReturned=false;
        uint64_t retailWorldRequest=0,retailWorldPageBirth=0,retailWorldCameraGeneration=0;
        uint32_t retailWorldOriginalInstance=UINT32_MAX,retailWorldPageInstance=UINT32_MAX;
        uint32_t retailWorldPageModel=UINT32_MAX,retailWorldPageRenderer=UINT32_MAX;
        std::string retailWorldPhase;
        bool retailWorldAutoEnabled=false,retailWorldSurfaceCertified=false;
        bool retailWorldProjectedBounded=false,retailWorldForcedFine=false;
        uint32_t retailWorldSelectedTriangles=0,retailWorldSafeRootObservations=0;
        uint32_t retailWorldProjectionStatus=0,retailWorldSurfaceStatus=0;
        uint64_t retailWorldStableFrameGeneration=0,retailWorldAutoObservations=0;
        uint64_t retailWorldAutoRefines=0,retailWorldAutoCoarsens=0;
        double retailWorldProjectedUpper=0,retailWorldSurfaceUpper=0;
        bool retailWorldResidencyEnabled=false,retailWorldConventionalFallback=false;
        uint32_t retailWorldResidencyCycles=0;
        uint32_t retailWorldResidencyCycleLimit=0,retailWorldLastRetiredMeshes=0;
        uint32_t retailWorldCompactedMeshes=0,retailWorldRetiredFineModels=0;
        uint64_t retailWorldResidencyReservedBytes=0,retailWorldRetirementAckRequest=0;
        uint64_t retailWorldResidencyTriggerCamera=0,retailWorldRetirementRequest=0,retailWorldRefillRequest=0;
        uint64_t retailWorldFallbackCameraGeneration=0;
        uint64_t retailWorldFallbackRequest=0,retailWorldFallbackPageBirth=0,retailWorldFallbackInstanceEpoch=0;
        uint64_t retailWorldRefillRequestAtFallback=0;
        uint32_t retailWorldFallbackOriginalFlags=UINT32_MAX;
        bool retailWorldFallbackPageAbsent=false;
        std::string retailWorldResidencyPhase;
        uint32_t retailVisibleReferenceSourceLevel=UINT32_MAX,retailVisibleReferenceTriangles=0;
        uint64_t retailVisibleCameraGeneration=0;
        bool retailFineTriangleSetExact=false;
        std::string retailPixelSourceSha256;
        uint32_t retailHierarchyProductStatus=0,retailHierarchyPages=0;
        uint32_t retailRecordMeshesPresent=0,retailRecordMeshesAbsent=0;
        uint32_t retailRecordModelsPresent=0,retailRecordModelsAbsent=0;
        uint64_t retailRecordRequestId=0,retailRecordWorkerReads=0,retailRecordFreshIds=0;
        uint32_t modelAdmissionAccepted=0,modelAdmissionRejected=0,modelAdmissionPending=0;
        uint64_t requestId=0, observedRequestId=0, knownPayloadBytes=0;
        uint32_t producerModel=UINT32_MAX, rendererModel=UINT32_MAX;
        uint32_t producerInstance=UINT32_MAX, rendererInstance=UINT32_MAX;
        uint32_t meshCount=0, meshesPresent=0, meshesAbsent=0;
        uint32_t coarseTriangles=0, fineTriangles=0, coarsePages=0, finePages=0;
        // Private original-source hierarchy: individual RAM page residency and
        // complete selected cuts. No pixel/quality or physical GPU-free claim.
        bool hierarchicalPilot=false;
        bool hierarchicalDiskPilot=false;
        uint32_t hierarchyPages=0,hierarchyRootPages=0,hierarchyResidentPages=0,hierarchyRetainedCutModels=0,hierarchyDiskPageReads=0;
        uint32_t hierarchyReleaseCount=0,hierarchyRefillCount=0;
        uint64_t hierarchyRetiredPageMask=0;
        uint64_t hierarchyProjectedGeneration=0,hierarchyProjectedRequired=0,hierarchyProjectedOutside=0,hierarchyProjectedForcedFine=0;
        double hierarchyProjectedAllowance=0,hierarchyProjectedMaximumIndicator=0;
        bool hierarchySourceVerticesExact=false;
        bool hierarchyAutoEnabled=false,hierarchyAutoPending=false;
        uint64_t hierarchyAutoObservations=0,hierarchyAutoRefines=0,hierarchyAutoCoarsens=0;
        uint32_t hierarchyAutoSafeRootObservations=0;
        bool hierarchyRetirementPending=false;
        uint32_t hierarchySelectedTriangles=0,hierarchyCutModel=UINT32_MAX;
        uint64_t hierarchyRequiredMask=0,hierarchyResidentMask=0,hierarchyMissingMask=0;
        std::vector<uint32_t> hierarchySelectedClusters;
        std::string hierarchyPackageSha256;
        float hierarchyThreshold=0;
        bool hierarchyLocalDemand=false;
        uint32_t hierarchyDemandGroupCount=0;
        std::array<float,64> hierarchyDemandThresholds{};
        std::array<float,64> hierarchyGroupErrors{}; // exact admitted simplified.error, FLT_MAX roots
        bool active=false, fineSelected=false, cpuHelpersUnchanged=false;
        bool mainFrameReturned=false, cascadeConfigured=false, localShadowConfigured=false;
        int32_t renderReturnStatus=INT32_MIN;
        // Optional copied command-recording facts, not fixture survivors/pixels or all-pass proof.
        // Fixed arrays; no new normal-frame capture or allocation. Valid only after successful frame/getter.
        struct PassFacts
        {
            bool valid=false;
            uint32_t version=0, structBytes=0, enabled=0, capabilities=0, required=0, pending=0, recordedThisFrame=0;
            uint64_t frame=0, instanceEpoch=0, cascadeEpoch=0, cascadeFrame=0, giRsmEpoch=0, giRsmFrame=0;
            uint64_t reflectionEpoch=0, reflectionFrame=0;
            uint32_t localCount=0, localDrawMask=0, localValidMask=0, interiorDrawMask=0, interiorValidMask=0;
            uint32_t cascadeDrawMask=0, cascadeCount=0;
            uint64_t localEpochs[24]{}, localFrames[24]{}, interiorEpochs[5]{}, interiorFrames[5]{};
        } passFacts;
        // Optional completed CPU view tuples, separate from COUNT and cached
        // images. Matrices remain renderer-owned; summaries copy only after
        // exact main-packet association on the serialized renderer owner.
        struct DemandViews {
            bool enabled=false,available=false,mainPacketJoined=false;
            uint32_t status=0,viewCount=0,mainCameraIndex=UINT32_MAX,mainCameraSource=0;
            uint64_t generation=0,instanceEpoch=0,required=0,known=0,unknown=0;
            struct Row {
                uint32_t id=0,kind=0,status=0,provenance=0,width=0,height=0,originX=0,originY=0,flags=0;
                uint64_t generation=0;
            };
            std::array<Row,40> rows{};
        } demandViews;
        bool clodPilot=false, fallbackSelected=false;
        bool pagedPilot=false, fineResident=false;
        bool diskPilot=false,coarseResident=false;
        bool originalFilePilot=false,sourceSnapshotValidated=false,sourceAdmissionConsumed=false;
        bool surfaceCertificateAdmitted=false,surfaceSelectedVerified=false;
        uint32_t surfaceCertificateStatus=0;
        uint64_t surfaceCertificateKnownBytes=0;
        double selectedSurfaceUpper=0; // Object-space selected coarse/fine only, never authored fallback/pixels.
        std::array<float,3> selectedUnionMinimum{},selectedUnionMaximum{};
        // Read-only successful returned-frame ideal selected-union projection. Never selects a cut or proves pixels/all-pass quality.
        bool cameraTupleEnabled=false,cameraTupleValid=false,projectedSurfaceIdealBound=false;
        uint32_t cameraTupleStatus=0,cameraTupleIndex=UINT32_MAX,cameraTupleSource=0,viewportWidth=0,viewportHeight=0;
        uint64_t cameraTupleGeneration=0,cameraTupleModelBirth=0,cameraTupleCertificateGeneration=0;
        uint32_t cameraTupleVersion=0,cameraTupleStructBytes=0,cameraTupleOutputWidth=0,cameraTupleOutputHeight=0;
        std::array<float,3> cameraTuplePosition{}; // Copied successful main-frame packet only, not a later live-camera read.
        // Same copied camera packet as Position; positive only for a validated active/Ready tuple.
        float cameraTupleClipNear=0,cameraTupleProjectionXScale=0,cameraTupleProjectionYScale=0;
        uint32_t projectedSurfaceStatus=0; // ProjectedSurface::Status+1, zero not requested.
        // Depth minimum is from the successful selected-union interval bound; zero on refusal.
        double projectedSurfaceDepthMinimum=0,projectedSurfaceXUpper=0,projectedSurfaceYUpper=0,projectedSurfaceEuclideanUpper=0;
        bool cameraDemandActive=false,cameraDesiredFine=false,cameraHasObservation=false,finePrepared=false;
        uint64_t cameraDemandRevision=0,cameraRequestedRevision=0,cameraObservations=0,cameraRequests=0,cameraCancels=0;
        uint32_t cameraAttempts=0,preparedProducerModel=UINT32_MAX;
        double cameraDistance=0;
        bool projectedDemandActive=false,projectedDemandHasBound=false;
        uint64_t projectedDemandGeneration=0;
        uint32_t projectedDemandStatus=0; // Last consumed projection status; preserved across pending Observe.
        double projectedDemandUpper=0; // Last authenticated pair discrepancy, not source error or pixels.
        bool privateAutoFineEnabled=false,privateAutoFineCommitted=false; // Historical one-way fixture action only.
        uint64_t privateAutoFineCameraGeneration=0,privateAutoFineSourceRequest=0; // Completed Observe request, not worker pageRequest.
        bool privateAutoCoarseEnabled=false,privateAutoCoarseCommitted=false; // Separate one-way private return.
        uint32_t privateAutoCoarseObservations=0; // At most 64 Fine-selected cuts.
        uint64_t privateAutoCoarseCameraGeneration=0,privateAutoCoarseSourceRequest=0;
        double privateAutoCoarseCertifiedUpper=0; // Fine-selected source-bound ideal pair upper at commit.
        uint64_t sourceAdmissionEpoch=0,originalSourceBytes=0,sourceAdmissionKnownBytes=0;
        uint32_t helperCount=0; // Original subset has no helper LODs; hashes stay empty.
        float originalOriginRadius=0;
        std::string diskReadStatus,diskRequestedRepresentation;
        // Jobs/known capacity are CPU source+decode scheduling estimates, not RSS.
        // Completed counts returned worker results; fineResident requires renderer validation.
        uint64_t pageEpoch=0,pageRequest=0,pageReservedBytes=0,pageCompleted=0,pageCancelled=0;
        uint32_t pageQueued=0,pageActive=0,pageReady=0,pageLiveJobs=0;
        std::string pageWorkState;
        // Full history: old fine records must be Absent while coarse/refill remain Present.
        // Absence is a SlotMap fact, not physical GPU release or submission completion.
        uint32_t expectedPresentMeshes=0,expectedAbsentMeshes=0,observedExpectedPresentMeshes=0,observedExpectedAbsentMeshes=0;
        uint32_t fineGeneration=0,fineEvictionCount=0,retiredFineFirst=0,retiredFineCount=0;
        bool fineEvictionPending=false;
        uint64_t rendererMeshHandles[64]{};
        uint32_t clodAdapterVersion=0, clodGroups=0, clodClusters=0, fallbackTriangles=0, authoredFineTriangles=0;
        double clodBakeMs=0;
        float coarseThreshold=0, fineThreshold=0;
        std::string clodLibraryRevision;
        uint32_t clodRamAdapterVersion=0;
        uint64_t clodRamKnownSourceBytes=0,pageSourceKnownBytes=0;
        std::string selectedCutSha256; // derived selected-cut package; sourceSha256 below stays raw source
        float x=0,y=0,z=0;
        std::string helperHashBefore, helperHashAfter, sourceSha256;
    };
    virtual GeometryPageFixtureReport ControlGeometryPageFixture(GeometryPageFixtureAction,
        float x=1200,float y=11.5f,float z=1200,const GeometryPageDiskFixtureInput* disk=nullptr,
        const GeometryPageCameraDemandInput* camera=nullptr,
        const GeometryPageHierarchyDemandInput* hierarchyDemand=nullptr,
        const GeometryPageHierarchyDiskFixtureInput* hierarchyDisk=nullptr) { return {}; }
    virtual WarmPatnikProbeReport ProbeWarmPatnikStage() const { return {}; } // append virtual slot
    // Exact opted-in raP candidate only. The renderer may initialize one PAA
    // header on the owner thread and return its current immutable source binding.
    virtual bool CaptureWarmRapStageRead(const char*, WarmTextureRead&) const { return false; }
    // Exact, opt-in DayZ owner staging before a converted shape enters ShapeBank.
    // One call inspects one parent section and may upload one authored NormalMap.
    // A false result means the caller must finish through ordinary registration.
    virtual bool StageWorldShapeParentNormalMap(const LODShapeWithShadow*, int, int, uint64_t,
        bool& attempted, bool& uploaded, uint64_t& chargedBytes, std::string& uploadedName)
    { attempted = uploaded = false; chargedBytes = 0; uploadedName.clear(); return false; }
    // Exact-tenement, default-off partial parent material pre-touch. The list comes
    // from the translated material's declared file stages; it does not certify
    // final bindings, proxy dependencies, or complete model admission.
    virtual bool ListWorldShapeParentMaterialRoles(const LODShapeWithShadow*, int, int,
        std::vector<std::string>& names) const { names.clear(); return false; }
    virtual bool StageWorldShapeParentMaterialRole(const char*, uint64_t,
        bool& attempted, bool& uploaded, uint64_t& chargedBytes)
    { attempted = uploaded = false; chargedBytes = 0; return false; }
    // Proof after complete registration: number of staged parent images actually
    // captured by that model's material bindings (never includes proxy children).
    virtual uint32_t CountWorldShapeCapturedTextures(const LODShapeWithShadow*,
        const std::vector<std::string>&) const { return 0; }
    // Optional bounded weather proof for source-visible individual tree owners.
    // APPENDED to preserve existing vtable slots; other renderers do no work.
    virtual void PrepareVisibleTreeSnow(const Object* /*obj*/) {}
    // Appended owner-only opt-in colour diagnostic. -1 queries, 0..4 sets;
    // -2 unavailable/backend/owner, -3 invalid mode. No public renderer ABI grows.
    virtual int ControlWetSoilDiagnostic(int /*mode*/ = -1) { return -2; }

  protected:
    // Post-hook fires from OnWindowResized so apps can re-run the aspect policy
    // when the viewport changes.
    ResizePostHook _resizePostHook = nullptr;

    void DrawTextFreeType(const Point2DAbs& pos, float size, const Rect2DAbs& clip, Font* font, PackedColor color,
                          const char* text);
    void DrawTextFreeType3D(Vector3Par pos, Vector3Par up, Vector3Par dir, ClipFlags clip, Font* font,
                            PackedColor color, int spec, const char* text, float x1c, float y1c, float x2c, float y2c);
    float GetText3DWidthFreeType(Font* font, const char* text);
};

extern Engine* GEngine;

#define GLOB_ENGINE (GEngine)

} // namespace Poseidon
#endif
