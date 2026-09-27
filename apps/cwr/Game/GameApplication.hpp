#pragma once

#include "GameBase.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

class GameApplication : public GameBase
{
  public:
    GameApplication() : GameBase(Poseidon::Application::CLIENT) {}
    virtual ~GameApplication() {}

#ifdef _WIN32
    int Run(HINSTANCE hInstance, LPSTR commandLine, int showCmd);
#endif

#ifndef _WIN32
    int Run(int argc, char** argv);
#endif

    virtual int Run(const char* commandLine) override;

    virtual Engine* CreateGraphicsEngine(const GraphicsEngineParams& params) override;

  protected:
    const char* LogDomain() const override { return "app"; }
    virtual bool InitializeSubsystems() override;
    virtual void RunMainLoop() override;
    virtual void ShutdownSubsystems() override;

    // In-process reload of all game content with the current mod set, keeping
    // the window/device alive (dev-panel "Reload game", mod Apply). Returns
    // false if a mission is active or the reload failed.
    bool ReloadGameContent() override;

    // Re-mount with a user-picked mod set (semicolon-separated absolute mod-folder
    // paths; empty = base game). Dev-panel mod picker; same gating as above.
    bool ReloadGameContentWithMods(const char* modPath) override;

  private:
    // Shared joined cleanup for the frame loop and explicit startup-probe refusal.
    void ShutdownRuntime();
    bool CreateAndSetGraphicsEngine();
    int RunAfterArgumentParsing();

    // --strict: if an ERROR-level log has been latched, request a clean shutdown
    // with a non-zero exit code. Polled once per main-loop iteration; no-op when
    // strict is off or already closing.
    void PollStrictAbort();

  protected:
    // Graphics engine bring-up — file banks, Glob_Init, InitMan, AI_InitTables,
    // InitFPU.  Apps that don't drive AI/vehicle simulation (Tetris) override
    // to a thinner sequence.
    virtual bool InitializeGraphicsEngine();

    // The re-runnable subset of the above (everything except the one-time
    // filebank-decryptor registration): banks/addons + config-derived tables.
    // Boot calls it via InitializeGraphicsEngine; a mod re-mount calls it again
    // after UnloadGameData to reload content onto the live window.
    bool InitializeGameContent();

    // GameBase engine-core hooks (client deltas).
    void ConfigureBankMerge() override;
    void OnGlobInitialized() override;
    void OnManagersInit() override;

    // World/landscape construction — CreateWorld, GPreloadedTextures_Preload,
    // CreateLandscape, VehicleTypes_Preload, InitWorld.  Apps without a 3D
    // game world (Tetris) override to a no-op.
    virtual bool InitializeWorld();
    virtual void RegisterGraphicsBackends();

    void RegisterAudioBackends() override;
    virtual bool InitializeSound();

  private:
    // Re-mount internals. Remount() is the seam: guard → loading screen →
    // GPU reset → UnloadGameData(keepEngine) → SetModPath → LoadGameData →
    // StartIntro. LoadGameData() replays the boot data-layer init (sans the
    // persisted platform steps); CanRemount() blocks while a mission runs.
    bool CanRemount() const;
    bool LoadGameData();
    bool Remount(const char* newModPath);

    bool InitializeInput();
    bool InitializeNetwork();
    void ProcessWindowMessages();

    void EnableRendering();
    void VerifySerialKey();
    void StartGameMode();
    void FinalizeInitialization();
    void RegisterGameModules() override;

#ifdef _WIN32
    HINSTANCE m_hInstance = nullptr;
    int m_showCmd = 0;
#endif

    bool m_keyVerified = false;

    // Set once the first content load has armed (or skipped) the startup splash; a mod re-mount
    // re-runs InitializeWorld() but must not replay the splash over the rebuilt menu.
    bool m_startupSplashArmed = false;
};
