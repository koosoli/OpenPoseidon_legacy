#pragma once

// DIAG-001: the diagnostics layer ported from Malprave (Dec's GPL fork of the same Poseidon source,
// engine/Poseidon/Dev/Diag/MwDiag*). Compiled only with POSEIDON_DIAG=1 (CMake option
// POSEIDON_ENABLE_DIAG); every hook in the engine sits in an `#if POSEIDON_DIAG` block and costs one
// bool test when --diag is not given.
//
// Turned on at run time with --diag <folder> (not registered in release builds):
//   <folder>/events.jsonl  one JSON object per line, flushed per line (an ExitProcess loses nothing):
//                          start, boot, addon, script, sound, sound_play, shot, impact, shot_end, hit,
//                          death, corpse, move, frame_slow, perf, ai, stuck, spin, trail, mark, crash, exit
//   <folder>/summary.json  at exit (or crash): frames, frame time stats, counts per event type
// --diag-cats a,b,c     limit the event types (default: all but "moveall")
// --diag-draw           draw shot paths, impacts and watched-unit labels over the 3D view (dev overlay)
// --diag-slow-ms <ms>   frame time that counts as a slow frame (default 40)
// --diag-keep-going     in test mode, log script errors instead of ending the mission
// --diag-seed <n>       seed the engine RNG once the test mission has booted
// --diag-fixed-dt <s>   pin the frame delta (Oli's lockstep pacing: POSEIDON_LOCKSTEP_HZ = 1/s)
// --diag-stuck-sec <s>  seconds without moving before a "stuck" event (default 5)
//
// Harness commands (diag_*, need --harness): OpDiagHarness.cpp and OpDiagCamera.cpp.
// Pause / step drive the existing dev pause (Dev/Diag/DiagPause.hpp) rather than a second one.

#include <Poseidon/Dev/Diag/OpDiagJson.hpp>

#if POSEIDON_DIAG

#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <cstdint>
#include <string>

namespace Poseidon
{
class Object;
class EntityAI;
class Shot;
class AmmoType;
class Man;
} // namespace Poseidon

namespace Poseidon::Dev
{
class HarnessServer;

namespace OpDiag
{

// ---- lifetime ---------------------------------------------------------------------------------
//! from GameApplication once AppConfig is parsed; does nothing without --diag
void Init();
//! the writer alone (Init reads AppConfig and calls this; the unit tests call it directly).
//! false when the folder cannot be written.
bool Start(const std::string& dir, const std::string& cats = std::string(), bool draw = false, bool keepGoing = false,
           float slowMs = 40.0f);
//! writes summary.json and closes events.jsonl; safe to call twice
void Shutdown();
//! --diag given and the log is open
bool Enabled();
//! event type enabled (--diag-cats); false when diag is off
bool On(const char* cat);
bool DrawEnabled();
void SetDraw(bool on);
bool KeepGoingOnScriptError();

// ---- event writer -----------------------------------------------------------------------------
//! One JSON line: { Ev e("hit"); e.Str("ammo", name).Num("delta", d); }  (written by ~Ev).
//! Every line starts with "ev" (type), "t" (mission time, s), "rt" (real time since start, s), "f" (frame).
class Ev
{
  public:
    explicit Ev(const char* type);
    ~Ev();
    Ev(const Ev&) = delete;
    Ev& operator=(const Ev&) = delete;
    Ev& Str(const char* key, const char* value);
    Ev& Num(const char* key, double value);
    Ev& Int(const char* key, long long value);
    Ev& Bool(const char* key, bool value);
    Ev& Vec(const char* key, Vector3Par v);      //!< [x, z, height] like getPos
    Ev& Obj(const char* key, const Object* obj); //!< {"name","cls","pos"} or null
    Ev& Raw(const char* key, const char* json);  //!< an already valid JSON value
    std::string& Buffer() { return _buf; }

  private:
    void Key(const char* key);
    std::string _buf;
    bool _active;
};

//! variable name, else debug name, else class / shape name
std::string ObjName(const Object* obj);
std::string ClassName(const Object* obj);

// ---- hooks (each is a no-op when diag or its event type is off) --------------------------------
//! the --test-mission boot result (WorldImpl.cpp); on failure the problems recorded while loading
void OnBoot(bool ok, const char* mission, int errorLevel);
void OnAddonMissing(const char* entry, const char* owner);
void OnScriptError(const char* position, const char* error);
void OnSoundMissing(const char* what, const char* name);
//! SoundScene::Open: every wave opened ("sound_play", rate-limited per file)
void OnSoundOpen(const char* file, bool is3D, bool ok);
void OnDeath(EntityAI* victim, EntityAI* killer);
//! Man::AdvanceMoveQueue: logs a "move" event when a watched man's primary move changed
void OnMoveTick(Man* man, int primaryMoveId);
//! a projectile simulation step (shells, grenades, missiles): records its path
void OnShotStep(Shot* shot);
//! Landscape::ExplosionDammage: a projectile (or explosion) hit something or the ground
void OnImpact(EntityAI* owner, Shot* shot, Object* directHit, Vector3Par pos, Vector3Par dir, const AmmoType* ammo);
//! a projectile ran out of time / was removed without an impact
void OnShotEnd(Shot* shot, const char* reason);
//! end of World::Simulate: frame timing (phases as Dev::FrameProfiler records them)
void OnFrameEnd(float totalMs, const float* phaseMs, int phaseCount, int drawCalls, float viewDistance);
void Mark(const char* text);
//! --diag-seed: called once the mission has booted
void OnMissionStarted();

//! damage bracket: snapshot in the constructor, "hit" event in the destructor (outermost scope only)
class DamageScope
{
  public:
    DamageScope(Object* target, EntityAI* owner, const char* ammo, Vector3Par worldPos, float val, float valRange);
    ~DamageScope();
    DamageScope(const DamageScope&) = delete;
    DamageScope& operator=(const DamageScope&) = delete;

  private:
    struct Impl;
    Impl* _impl;
};

// ---- watched units ------------------------------------------------------------------------------
bool Watching(const Object* obj);
void Watch(Object* obj, bool on);

// ---- time control (through Dev::SetDiagPause) ----------------------------------------------------
void Pause();
void Resume();
//! run `ticks` fixed simulation ticks (one per frame), then stay paused
void Step(int ticks);
bool Paused();
//! World::Simulate, before the fixed-step loop: true = run exactly one tick this frame through the pause
bool BeginStepTick();
//! after the fixed-step loop: re-engages the pause after a stepped tick
void EndStepTick(bool stepped);

// ---- 3D overlay (drawn by the dev overlay, Dev/Debug/DebugOverlay.cpp) ---------------------------
void DrawLine(Vector3Par a, Vector3Par b, uint32_t rgba, float ttlSec);
void DrawCross(Vector3Par p, float size, uint32_t rgba, float ttlSec);
void DrawLabel(Vector3Par p, const char* text, uint32_t rgba, float ttlSec);
//! inside the ImGui frame, before ImGui::Render: projects and draws everything alive
void RenderOverlay();

// ---- harness ------------------------------------------------------------------------------------
//! every diag_* command (OpDiagHarness.cpp, OpDiagCamera.cpp)
void RegisterHarness(HarnessServer& hs);

} // namespace OpDiag
} // namespace Poseidon::Dev

#endif // POSEIDON_DIAG
