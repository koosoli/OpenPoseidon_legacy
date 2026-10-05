#include <Poseidon/UI/LocalMapEditor.hpp>
#include <Poseidon/Core/Game/GameLoop.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/Config.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/PendingConnect.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Input/CheatCode.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>
#include <Poseidon/Dev/Diag/FixedStepStats.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Dev/Diag/FramePaceTrace.hpp>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>
#include <string>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <Poseidon/Foundation/Framework/PoTime.hpp>
#include <Poseidon/Foundation/Threads/PreciseSleep.hpp>

using namespace Poseidon::Dev;
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Foundation/Common/Win.h>
#ifdef _WIN32
// timeBeginPeriod/timeEndPeriod. WIN32_LEAN_AND_MEAN is defined project-wide
// (top-level CMakeLists.txt), so <windows.h> does NOT pull in <mmsystem.h> and
// these have to be asked for by name.
    #include <timeapi.h>
#endif
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Graphics/Rendering/Draw/Font.hpp>

#include <SDL3/SDL.h>

// External references (defined at global ns in Input subsystem)
extern void ProcessMouse(DWORD timeDelta);
extern void ProcessKeyboard(DWORD sysTime, DWORD timeDelta);
extern void ProcessJoystick();
extern bool IsMouseAcquired();
extern void SDLInput_DispatchUIKeys();

namespace Poseidon
{

// User-driven FPS cap (Graphics screen → graphics.cfg → fpsCap field).
// 0 = uncapped; nonzero values trigger the same sleep-to-target path
// used by the debug "limit FPS" cheat.  Read by the frame-pacer at the
// bottom of RenderFrame.
int gUserFpsCap = 0;

// Sleep out the remainder of a frame so the loop averages `targetFps`.
//
// The arithmetic runs entirely in microseconds. It used to be `1000 / targetFps` in
// INTEGER milliseconds, which truncates, and truncation on a frame period is a large
// error: a 144 cap became 1000/144 = 6 ms = 166.7 fps (+16%), 90 became 11 ms = 90.9,
// and 60 became 16 ms = 62.5 -- patched over with a fractional-millisecond carry so
// the AVERAGE came out right while individual frames still landed on whole
// milliseconds. In microseconds the same truncation is 1'000'000/144 = 6944 vs
// 6944.44, i.e. under a microsecond per frame (< 0.01% on the cap), which is below
// what any frame-time measurement here can resolve, so no carry is needed and every
// frame gets the same budget.
//
// SleepUs is the piece that makes the precision real: the previous Sleep(ms) could
// only land on scheduler ticks, so the pacer alternated 6/7 ms frames to fake 6.944.
// A waitable-timer sleep hits the fractional deadline directly, which steadies
// frame-to-frame delivery -- the thing a player capping to their refresh rate is
// actually after.
// PACE-001: what the pacer decided on the last frame, for the frame trace. Written by the
// main thread only.
struct PaceDecision
{
    int     capFps = 0;
    int64_t periodUs = 0;
    int64_t elapsedUs = 0;
    int64_t sleepReqUs = 0;
    int64_t sleepActUs = 0;
};
static PaceDecision gLastPace;

// PACE-001 diagnostic: POSEIDON_PACE_LEGACY_SLEEP=1 reproduces the pacer as it was in the
// 2026-08-22 build (integer-millisecond period, ::Sleep, no timeBeginPeriod) INSIDE the
// current process, so the trace can show what that pacer did with every DLL this build
// loads -- a standalone probe cannot know whether something in-process had raised the
// timer resolution. Not a play setting.
static bool LegacySleepPacer()
{
    static const bool v = []
    {
        const char* e = std::getenv("POSEIDON_PACE_LEGACY_SLEEP");
        const bool on = e != nullptr && e[0] != 0 && e[0] != '0';
        if (on)
            LOG_WARN(Core, "Frame pacer: LEGACY Sleep(ms) mode (POSEIDON_PACE_LEGACY_SLEEP) -- diagnostic only");
        return on;
    }();
    return v;
}

static void PaceToFps(int targetFps, Foundation::unsigned64 frameStartUs)
{
    gLastPace = PaceDecision{};
    if (targetFps <= 0)
        return;
    if (LegacySleepPacer())
    {
        const int minMsPerFrame = 1000 / targetFps;
        const int64_t elapsedUs = static_cast<int64_t>(Poseidon::Foundation::getSystemTime() - frameStartUs);
        const int durationMs = static_cast<int>(elapsedUs / 1000);
        const int sleepMillis = minMsPerFrame - durationMs;
        gLastPace.capFps = targetFps;
        gLastPace.periodUs = static_cast<int64_t>(minMsPerFrame) * 1000;
        gLastPace.elapsedUs = elapsedUs;
        if (sleepMillis > 0)
        {
            gLastPace.sleepReqUs = static_cast<int64_t>(sleepMillis) * 1000;
            const Foundation::unsigned64 t0 = Poseidon::Foundation::getSystemTime();
            ::Sleep(static_cast<DWORD>(sleepMillis));
            gLastPace.sleepActUs = static_cast<int64_t>(Poseidon::Foundation::getSystemTime() - t0);
        }
        return;
    }

    const int64_t periodUs = 1'000'000 / targetFps;
    const int64_t elapsedUs = static_cast<int64_t>(Poseidon::Foundation::getSystemTime() - frameStartUs);
    const int64_t sleepUs = periodUs - elapsedUs;
    gLastPace.capFps = targetFps;
    gLastPace.periodUs = periodUs;
    gLastPace.elapsedUs = elapsedUs;
    if (sleepUs > 0)
    {
        gLastPace.sleepReqUs = sleepUs;
        const Foundation::unsigned64 t0 = Poseidon::Foundation::getSystemTime();
        Foundation::SleepUs(sleepUs);
        gLastPace.sleepActUs = static_cast<int64_t>(Poseidon::Foundation::getSystemTime() - t0);
    }
}

// Safety net for the pacer's degraded modes. SleepUs normally waits on a
// high-resolution timer that ignores the process timer resolution entirely, but on
// a kernel without that timer it degrades to waits quantized by the timer interrupt
// -- 15.6 ms by default, which overshoots a frame budget by up to a full tick and
// does so unevenly, exactly the symptom a cap is meant to cure. winmm is already
// linked; ask for 1 ms while a cap is active and give it back when it is not, so an
// uncapped session keeps the default (raising it costs battery on a laptop and
// there is nothing to gain when nothing sleeps).
static void SetFrameCapTimerResolution(bool wanted)
{
#ifdef _WIN32
    if (LegacySleepPacer())
        return; // the 2026-08-22 build never asked for 1 ms; the diagnostic must not either
    static bool granted = false;
    if (wanted == granted)
        return;
    if (wanted)
        granted = (timeBeginPeriod(1) == TIMERR_NOERROR);
    else
    {
        timeEndPeriod(1);
        granted = false;
    }
#else
    (void)wanted;
#endif
}

// Idle heartbeat used by blocking waits (progress screen, debug window,
// triWaitFrames). Tickles the watchdog and pumps OS events so the window
// stays responsive; does not run a full AppIdle — callers that want that
// call AppIdle() directly.
// SIM-815: the one place the lockstep state lives, so subsystems that must swap a real-time
// dependence for their deterministic path can ask (the radio drain does). Function-local
// static, not namespace-scope, so the log line runs after logging is up.
static float LockstepDeltaT()
{
    static const float v = []
    {
        const char* e = std::getenv("POSEIDON_LOCKSTEP_HZ");
        if (!e || !e[0])
            return 0.0f;
        const double hz = std::atof(e);
        // A nonsense value must be inert rather than freezing or exploding the simulation.
        if (!(hz > 0.0) || hz > 1000.0)
        {
            LOG_WARN(Core, "POSEIDON_LOCKSTEP_HZ={} is not a rate in (0, 1000]; ignoring", e);
            return 0.0f;
        }
        LOG_INFO(Core,
                 "Lockstep pacing ON at {} Hz -- deltaT is pinned and no longer measured. "
                 "Frame times reported in this mode are NOT play performance.",
                 hz);
        return static_cast<float>(1.0 / hz);
    }();
    return v;
}

bool LockstepPacingActive()
{
    return LockstepDeltaT() > 0.0f;
}

void ProcessMessagesNoWait()
{
    GDebugger.ProcessAlive();
    if (GEngine)
        GEngine->HandleEvents();
    else
        SDL_PumpEvents();
}

void RenderFrame(float deltaT, bool enableDraw)
{
    if (Glob.exit)
    {
        GApp->m_closeRequest = true;
    }
    if (!GWorld)
    {
        return;
    }

    // Microsecond stamp (QPC-backed) rather than the millisecond tick count: the
    // frame pacer at the bottom sleeps with sub-millisecond precision, and a pacer
    // whose elapsed-time input is quantized to 1 ms would throw away most of that.
    const Foundation::unsigned64 frameStartUs = Poseidon::Foundation::getSystemTime();
    GDebugger.NextAliveExpected(10000);

    GWorld->SetSimulationFocus(enableDraw);
    if (GSoundsys)
        GSoundsys->SetSimulationRunning(GWorld->IsSimulationEnabled());
    GWorld->Simulate(deltaT, enableDraw);

    GApp->m_forceRender = false;

#if _ENABLE_CHEATS
    static int limitFpsCoef = 0;
#define limitFps (limitFpsCoef > 0 ? 40 / limitFpsCoef : 0)
    auto& renderInput = InputSubsystem::Instance();
    if (renderInput.GetCheat2ToDo(SDL_SCANCODE_S))
    {
        limitFpsCoef++;
        if (limitFpsCoef > 4)
            limitFpsCoef = 0;
        GlobalShowMessage(500, "Limit FPS %d", limitFps);
    }
#else
    const bool limitFps = false;
    auto& renderInput = InputSubsystem::Instance();
#endif

    // PAUSE key toggles HW T&L.  All F-keys are taken (F1=help,
    // F2-F11 = units / lang via DisplayMain, F12 = language cycle
    // via displayUI.hpp:1040), and modifier-prefixed keys don't pass
    // through ConsumeKeyPress; PAUSE has no other handler.
    extern bool EnableHWTLState;
    if (renderInput.ConsumeKeyPress(SDL_SCANCODE_PAUSE))
    {
        EnableHWTLState = !EnableHWTLState;
        GlobalShowMessage(500, "DX T&L %s", EnableHWTLState ? "On" : "Off");
        LOG_INFO(Graphics, "DX T&L toggled to {} (PAUSE)", EnableHWTLState ? "On" : "Off");
    }

    if (!enableDraw || GWorld->GetRenderingDisabled() || limitFps)
    {
#if _ENABLE_CHEATS
        int maxFps = limitFpsCoef ? limitFps : 50;
#else
        const int maxFps = 50;
#endif
        SetFrameCapTimerResolution(true);
        PaceToFps(maxFps, frameStartUs);
    }
    else if (gUserFpsCap > 0)
    {
        // User-driven FPS cap from the Graphics screen — same sleep-
        // to-target shape as the unfocused / debug-limit branches
        // above, but applied during normal gameplay rendering.  Soft
        // cap (sleep-based).  The 1 ms timer resolution is still
        // requested for the machines where SleepUs has to degrade to
        // a scheduler-granular wait (no high-resolution timer).
        SetFrameCapTimerResolution(true);
        PaceToFps(gUserFpsCap, frameStartUs);
    }
    else
    {
        SetFrameCapTimerResolution(false);
        gLastPace = PaceDecision{};
    }

    // PACE-001: one row per frame, only when POSEIDON_FRAME_TRACE is set. The profiler record
    // is this frame's (World::Simulate ends it); the pacer fields are what the branch above did.
    if (Dev::FramePaceTraceEnabled())
    {
        const Dev::FrameProfiler& perf = Dev::GFrameProfiler();
        const Dev::FixedStepFrame step = Dev::LastFixedStepFrame();
        Dev::FramePaceMainRow row;
        row.frame = GEngine ? GEngine->GetFrameCounter() : 0u;
        row.tEndMs = Dev::FramePaceNowMs();
        // The frame start was stamped on the QPC clock; express it on the shared clock by
        // subtracting the elapsed time, which is the same interval on both.
        const double elapsedMs =
            static_cast<double>(Poseidon::Foundation::getSystemTime() - frameStartUs) * 0.001;
        row.tStartMs = row.tEndMs - elapsedMs;
        row.tPaceMs = row.tEndMs - static_cast<double>(gLastPace.sleepActUs) * 0.001;
        row.deltaTMs = deltaT * 1000.0f;
        row.steps = step.steps;
        row.alpha = step.alpha;
        row.totalTicks = step.totalTicks;
        if (perf.FrameCount() > 0)
        {
            using P = Dev::FrameProfiler;
            const auto& r = perf.Frame(0);
            row.setupMs = r.ms[P::PhaseSetup];
            row.simMs = r.ms[P::PhaseSimStep];
            row.drawInitMs = r.ms[P::PhaseDrawInit];
            row.drawMs = r.ms[P::PhaseDrawObjPrep] + r.ms[P::PhaseDrawLandGround] + r.ms[P::PhaseDrawLandObjects] +
                         r.ms[P::PhaseDrawLandscape] + r.ms[P::PhaseDrawObjects] + r.ms[P::PhaseDrawPost];
            row.hudMs = r.ms[P::PhaseHud];
            row.soundMs = r.ms[P::PhaseSound];
            row.swapMs = r.ms[P::PhaseSwap];
        }
        row.capFps = gLastPace.capFps;
        row.pacePeriodMs = static_cast<float>(gLastPace.periodUs) * 0.001f;
        row.paceElapsedMs = static_cast<float>(gLastPace.elapsedUs) * 0.001f;
        row.paceSleepReqMs = static_cast<float>(gLastPace.sleepReqUs) * 0.001f;
        row.paceSleepActMs = static_cast<float>(gLastPace.sleepActUs) * 0.001f;
        // The motion the player actually sees: how far the render camera moved this frame.
        {
            static Vector3 lastCam(0, 0, 0);
            static Vector3 lastDir(0, 0, 1);
            static bool haveLast = false;
            const Camera* cam = GScene ? GScene->GetCamera() : nullptr;
            if (cam)
            {
                const Vector3 p = cam->Position();
                const Vector3 d = cam->Direction();
                row.camDxM = haveLast ? (p - lastCam).Size() : 0.0f;
                if (haveLast)
                {
                    float c = d.DotProduct(lastDir);
                    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                    row.camDyawDeg = static_cast<float>(acos(c) * (180.0 / 3.14159265358979));
                }
                lastCam = p;
                lastDir = d;
                haveLast = true;
            }
        }
        row.interpAlpha = Object::RenderInterpAlpha();
        Dev::FramePaceTraceMain(row);
    }
}

bool AppIdle()
{
    // Service a deferred re-mount request between frames (set by triRemount / dev panel),
    // before any simulate/draw touches the world it tears down. The reload itself runs
    // StartIntro, so consume this tick and resume on the rebuilt world next call.
    if (GApp->m_remountRequested)
    {
        GApp->m_remountRequested = false;
        if (GApp->m_remountHasModPath)
            GApp->ReloadGameContentWithMods(GApp->m_remountModPath.c_str());
        else
            GApp->ReloadGameContent();
        return true;
    }

    if (Poseidon::ServiceLocalMapEditor())
        return true;

    // Finish a deferred MP join: after a mod-apply re-mount lands back on the menu,
    // connect to the server stashed before the re-mount.
    // Armed only alongside a re-mount request, so the re-mount tick above always
    // runs first and this never fires on the torn-down world; it fires once the
    // rebuilt menu (GModeIntro + Options) is live.
    if (Poseidon::GPendingConnect().IsArmed() && GWorld != nullptr && GWorld->GetMode() == GModeIntro &&
        GWorld->Options() != nullptr)
    {
        Poseidon::PendingConnect& pc = Poseidon::GPendingConnect();
        // Drive the non-blocking deferred connect one step per frame: it starts host
        // enumeration, then later frames poll + join. Do NOT return here — the rest of
        // the frame (SDL pump / simulate) is what advances the enumeration between
        // steps, so blocking the loop the way WaitForSession does would deadlock it.
        extern bool __cdecl CreateClientDeferred(RString ip, int port, RString password);
        if (CreateClientDeferred(RString(pc.Address().c_str()), pc.Port(), RString(pc.Password().c_str())))
        {
            pc.Disarm();
        }
    }

    // Report a failed mod re-mount on the rolled-back menu, once it is live again
    // (same GModeIntro + Options gate as the deferred MP-join above).
    if (GApp->m_remountFailed && GWorld != nullptr && GWorld->GetMode() == GModeIntro && GWorld->Options() != nullptr)
    {
        GApp->m_remountFailed = false;
        extern void __cdecl ReportRemountFailure();
        ReportRemountFailure();
    }

    // Once-per-frame memory maintenance, off the allocation path: when over the
    // soft watermark it trims caches to their declared budgets, and over the hard
    // ceiling it claws the overflow back with cost-ordered eviction. A handful of
    // atomic loads when under budget (the common case — both limits default off).
    Poseidon::Foundation::MemoryFrameMaintenance();

    bool focused = (GApp->m_keepFocus || GApp->m_appActive) && !GApp->m_appPaused && !GApp->m_appIconic;
    if (GSoundsys)
        GSoundsys->Activate(focused);
    const bool testMissionActive = !AppConfig::Instance().GetTestMissionPath().empty();
    bool enableDraw = (focused || (GApp->m_forceRender || testMissionActive) &&
                                      (ENGINE_CONFIG.landEditor || ENGINE_CONFIG.useWindow));

    // Render interpolation needs the same sub-millisecond clock as the pacer.
    // Millisecond ticks turn smooth 60 Hz motion into alternating 16/17 ms
    // advances, even when the actual presentation interval is steady.
    static Foundation::unsigned64 lastTime;
    Foundation::unsigned64 actTime = Poseidon::Foundation::getSystemTime();
    auto deltaTUs = actTime - lastTime;

    if (!enableDraw)
    {
        if (deltaTUs < 50'000)
        {
            Foundation::SleepUs(static_cast<int64_t>(50'000 - deltaTUs));
            actTime = Poseidon::Foundation::getSystemTime();
            deltaTUs = actTime - lastTime;
        }
    }

    float deltaT = static_cast<float>(deltaTUs * 0.000001);
    lastTime = actTime;

    static DWORD lastSysTime;
    DWORD sysTime = ::GetTickCount();
    DWORD timeDelta = sysTime - lastSysTime;
    lastSysTime = sysTime;

    saturateMin(deltaT, 0.3);

    auto& input = InputSubsystem::Instance();

#if _ENABLE_CHEATS
    static bool fixedSimulation = false;
    if (input.GetCheat2ToDo(SDL_SCANCODE_O))
    {
        fixedSimulation = !fixedSimulation;
        GEngine->ShowMessage(500, "Fix Sim %s", fixedSimulation ? "On" : "Off");
    }
    if (fixedSimulation)
    {
        deltaT = 1.0 / 10;
    }
#endif

    // REN-THR-002: lockstep pacing, built as the prerequisite for proving that a
    // restructured producer publishes the same frames.
    //
    // It was written up once as MEASURED NOT TO HELP -- 24 frames of agreement without it
    // against 19 with, and 59 against 46 in a second pairing. Both readings were mine and
    // both were wrong, for a reason worth leaving here: with deltaT pinned to 1/60 s while
    // a frame really takes ~9.7 ms, each frame advances the world 16.7 ms, so the lockstep
    // arm is nearly twice as far into the world at frame N as the arm it was compared with.
    // Counting FRAMES until two runs diverge does not compare the same thing across arms
    // that cover different amounts of world time per frame. The comparison, not the mode,
    // was the defect -- and the deeper defect was that both windows sat inside the MISSION
    // LOAD, so they were timing the loading screen.
    //
    // Measured properly (REN-THR-005), aligning each run on the first frame that draws the
    // world: this mode makes the fixed-step count identical in 40 frames of 40 across three
    // runs, raises bit-identical published frames from 2/41 to 11/40, and holds the camera
    // and draws3d identical for 12 frames instead of 2. It does what it was built to do.
    //
    // Kept because it is a real capability the search for that cause will want, and because
    // it is the scriptable form of the Ctrl-O "Fix Sim" cheat above -- which does the same
    // thing at 10 Hz but exists only in cheat-enabled builds and only from a keypress, so no
    // capture harness can reach it. Off unless asked for.
    //
    // Read once: the value must not change mid-run, which is the whole point.
    const float lockstepDeltaT = LockstepDeltaT();
    if (lockstepDeltaT > 0.0f)
    {
        deltaT = lockstepDeltaT;
    }
    if (GApp->m_canRender)
    {
        // Dispatch UI key events buffered by SDL input processing
        SDLInput_DispatchUIKeys();
        if (ENGINE_CONFIG.useWindow && !IsMouseAcquired() || !enableDraw)
        {
            RenderFrame(deltaT, enableDraw);
        }
        else
        {
            ProcessMouse(timeDelta);
            ProcessKeyboard(sysTime, timeDelta);

            if (input.CheatActivated() == CheatFreeze)
            {
                input.CheatServed();
                for (;;)
                {
                }
            }

            if (input.IsJoystickEnabled())
            {
                ProcessJoystick();
            }

            input.Update();

            if (input.FreelookChanged())
            {
                if (GWorld)
                {
                    GWorld->FreelookChange(input.IsLookAroundEnabled());
                }
            }

            Object* cam = GWorld->CameraOn();
            if (cam)
            {
                cam->DetectControlMode();
            }

            RenderFrame(deltaT, true);
        }
        return false;
    }
    else
    {
        return true;
    }
}

} // namespace Poseidon
