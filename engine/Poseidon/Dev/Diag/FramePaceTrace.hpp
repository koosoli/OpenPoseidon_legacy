#pragma once

// PACE-001: per-frame pacing trace for the frame-limiter / render-thread / present
// investigation (community report: stutter at a 120 fps cap on a GPU-bound scene).
//
// Off unless POSEIDON_FRAME_TRACE=<path prefix> is set. Three CSV files are written at
// process exit (<prefix>.main.csv, <prefix>.producer.csv, <prefix>.worker.csv), one row per
// frame per side. Recording is a struct copy into a pre-reserved vector: no allocation, no
// logging and no I/O on the frame path, so the trace does not disturb what it measures. All
// three sides stamp the same steady_clock, so rows align on wall time.
//
// This is a diagnostic. Nothing in the engine reads it back.

#include <cstdint>

namespace Poseidon::Dev
{

/// Main thread: one row per RenderFrame (simulation + draw traversal + pacer).
struct FramePaceMainRow
{
    std::uint32_t frame = 0;         ///< Engine frame counter after this frame's FinishDraw.
    double        tStartMs = 0.0;    ///< Frame start (top of RenderFrame), ms on the shared clock.
    double        tPaceMs = 0.0;     ///< When the pacer began (after Simulate/draw).
    double        tEndMs = 0.0;      ///< After the pacer's sleep (end of RenderFrame).
    float         deltaTMs = 0.0f;   ///< The deltaT the simulation was given (integer-ms source).
    std::uint32_t steps = 0;         ///< Fixed-step ticks run this frame.
    float         alpha = 0.0f;      ///< Accumulator remainder after the ticks (0..1).
    std::uint64_t totalTicks = 0;    ///< Cumulative ticks: the simulation state this frame shows.
    float         setupMs = 0.0f;    ///< FrameProfiler phases (elapsed, waits included).
    float         simMs = 0.0f;
    float         drawInitMs = 0.0f; ///< drw:init -- under overlap this holds the wait for the worker.
    float         drawMs = 0.0f;     ///< drw:prep .. drw:post
    float         hudMs = 0.0f;
    float         soundMs = 0.0f;
    float         swapMs = 0.0f;     ///< NextFrame (publish + slot wait, or the lockstep wait).
    int           capFps = 0;        ///< The cap the pacer used (0 = none).
    float         pacePeriodMs = 0.0f;   ///< 1000 / cap.
    float         paceElapsedMs = 0.0f;  ///< Frame time seen by the pacer when it decided.
    float         paceSleepReqMs = 0.0f; ///< What it asked SleepUs for (0 = frame was already late).
    float         paceSleepActMs = 0.0f; ///< What the sleep actually took.
    float         camDxM = 0.0f;         ///< Camera displacement since the previous frame (m): the motion the player sees.
    float         interpAlpha = 0.0f;    ///< Render interpolation alpha used for this frame's draw.
    float         camDyawDeg = 0.0f;     ///< Angle between this and the previous frame's camera direction (deg).
};

/// Producer side of the render handoff (EngineWgpu::NextFrame), one row per published frame.
struct FramePaceProducerRow
{
    std::uint32_t frame = 0;          ///< Engine frame counter (same key as the main row).
    double        tPublishMs = 0.0;   ///< When the frame was handed to the worker (or drawn inline).
    float         preDrawWaitMs = 0.0f; ///< InitDraw's wait for the worker to go idle (REN-THR-012).
    float         lazyWaitMs = 0.0f;    ///< Waits opened late by a renderer-touching call (REN-THR-013).
    float         slotWaitMs = 0.0f;    ///< Wait for a free ring slot at publish (overlap).
    float         lockstepWaitMs = 0.0f; ///< Wait for the whole block (render thread without overlap).
    std::uint8_t  renderThread = 0;
    std::uint8_t  overlap = 0;
};

/// Worker side (RunConsumerBlock), one row per rendered frame.
struct FramePaceWorkerRow
{
    std::uint32_t seq = 0;          ///< Worker block sequence number.
    double        tStartMs = 0.0;   ///< Block start on the shared clock.
    double        tEndMs = 0.0;     ///< Block end (after present + stats).
    float         idleBeforeMs = 0.0f; ///< How long the worker waited for this frame.
    float         acquireMs = 0.0f;    ///< surface.get_current_texture(): the swapchain back-pressure.
    float         submitMs = 0.0f;     ///< queue.submit
    float         presentMs = 0.0f;    ///< frame.present()
    float         renderMs = 0.0f;     ///< wgr_render_frame wall time (acquire .. present).
    float         gpuFrameMs = 0.0f;   ///< Latest harvested GPU frame total (-1 = none yet).
};

bool FramePaceTraceEnabled();
void FramePaceTraceMain(const FramePaceMainRow& row);
void FramePaceTraceProducer(const FramePaceProducerRow& row);
void FramePaceTraceWorker(const FramePaceWorkerRow& row);

/// Milliseconds on the shared steady clock (0 = first call in the process).
double FramePaceNowMs();

/// Write the CSV files now (also happens automatically at exit). Safe to call more than once;
/// rows written by an earlier flush are not repeated.
void FramePaceTraceFlush();

} // namespace Poseidon::Dev
