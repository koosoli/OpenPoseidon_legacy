#pragma once

// Self-contained ImGui debug/test panel.  Tabbed window — Font tuner,
// Cheats, and future Console/Perf/Memory tabs — designed to extend without
// touching the engine.
//
// Lifecycle (called from the GL33 engine + main loop):
//   DebugOverlay::Init(window, glContext)  — once after GL context exists
//   DebugOverlay::ProcessEvent(event)      — for every SDL event
//   DebugOverlay::NewFrame()               — at the start of each frame
//   DebugOverlay::Render()                 — before SDL_GL_SwapWindow
//   DebugOverlay::Shutdown()               — on engine teardown
//
// Hidden by default — press Ctrl+` (US) / Ctrl+; (CZ) to toggle.  Bound by
// physical scancode (SDL_SCANCODE_GRAVE), so the same key works on any
// layout. Requires --dev (AppConfig::DevMode()). Disabled in multiplayer;
// joining a network session closes the panel and cancels its developer camera/actions.

struct SDL_Window;
union SDL_Event;

namespace Poseidon
{
class CameraVehicle;
}

namespace Poseidon::Dev {
namespace DebugOverlay
{
    void Init(SDL_Window* window, void* glContext);

    // Alternative to Init for engines without a native ImGui backend: ImGui
    // renders through the Engine overlay virtuals (SubmitOverlay /
    // OverlayTexture*) instead of imgui_impl_opengl3. Used by EngineWgpu.
    void InitForEngine(SDL_Window* window);

    void ProcessEvent(const SDL_Event& event);

    // Fire an in-process content re-mount through the exact deferred path the MODS-tab
    // "Reload" button uses: the action is queued and drained inside DebugOverlay::Render()
    // (the GL33 BackToFront swap), mid-frame after Simulate — NOT the between-frames
    // m_remountRequested path triRemount uses. modPath is the semicolon-separated mod set
    // ("" = base game). Exposed so the harness/self-test can reproduce the button's timing.
    void RequestDeferredReload(const char* modPath);
    void NewFrame();
    void Render();
    void Shutdown();

    bool IsVisible();
    void SetVisible(bool visible);
    void ToggleVisible();

    // Force the "Shadows" tab to be the selected one on the next drawn frame.
    // Used by the dev-panel shadow test so a screenshot captures the tab's
    // contents deterministically (ImGui otherwise keeps the last-clicked tab).
    void SelectShadowsTab();

    // Same, for the "Memory" tab (process heap + per-subsystem residency).
    void SelectMemoryTab();

    // Returns true if the focused ImGui widget wants to swallow keyboard /
    // mouse events.  Callers should drop the SDL event before passing it to
    // the engine when these return true — otherwise typing into a slider
    // also moves the player, etc.  Reflect the previous frame's state (the
    // ImGui standard pattern), since events are queried before NewFrame.
    bool WantsKeyboard();
    bool WantsMouse();

    /// Adopt a free-fly camera the panel did not create, so the Game tab's
    /// coordinate readout and the Zeus tab's "Beam player here" work with it.
    /// `--test-world-freefly` builds the same CameraVehicle the Zeus tab does;
    /// without this the panel simply cannot see it, and the developer flying a
    /// world from the command line has neither the coordinates nor the beam.
    /// Takes no ownership: the camera stays owned by whoever created it, and
    /// clearing is the caller's business (pass nullptr).
    void AdoptFreeFlyCamera(CameraVehicle* camera);
    // Harness uses the same enter/teleport path as the Zeus coordinate control.
    bool SetFreeFlyTestPose(const char* coordinates);
} // namespace DebugOverlay
} // namespace Poseidon::Dev
