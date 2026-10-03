#pragma once

// DIAG-001: internals shared by OpDiag.cpp (core), OpDiagPollers.cpp, OpDiagHarness.cpp, OpDiagCamera.cpp
// and OpDiagOverlay.cpp. Compiled only with POSEIDON_DIAG=1 (see OpDiag.hpp).

#include <Poseidon/Dev/Diag/OpDiag.hpp>

#if POSEIDON_DIAG

#include <deque>
#include <string>
#include <vector>

namespace Poseidon::Dev
{
class HarnessServer;
}

namespace Poseidon::Dev::OpDiag::Internal
{
// ---- OpDiag.cpp ---------------------------------------------------------------------------------------
double RealTimeNow();    //!< real seconds since the diag log started
double MissionTimeNow(); //!< mission time (s)
long long FrameNo();
void Vec(std::string& out, Vector3Par v); //!< [x, z, height] like getPos
std::vector<Object*> Watched();
//! overlay item: kind 0 line, 1 cross, 2 text
void AddDraw(int kind, Vector3Par a, Vector3Par b, float size, uint32_t rgba, float ttl, const char* text);
void ForEachDrawItem(void (*fn)(int kind, Vector3Par a, Vector3Par b, float size, uint32_t rgba, const char* text,
                                void* ctx),
                     void* ctx);

// ---- OpDiagPollers.cpp -----------------------------------------------------------------------------------
void InitPollers();                    //!< from Init: seed, fixed dt, stuck seconds, crash notify
void ShutdownPollers();                //!< from Shutdown: sound statistics, crash notify off
void TickPollers(double realNow);      //!< every frame (OnFrameEnd)
void OnDeathTracked(EntityAI* victim); //!< start following the corpse
bool Corpse(const Object* obj);        //!< a corpse still followed (its moves are logged)
void SetStuckSeconds(float sec);
float StuckSeconds();
const std::deque<Vector3>* TrailOf(const Object* obj); //!< last 10 s of a watched man, or nullptr
std::string MoveNameOf(const Man* man);                //!< current primary move name ("" when none)

// ---- OpDiagHarness.cpp / OpDiagCamera.cpp ----------------------------------------------------------------
//! evaluate SQF to an object ("player", a variable name, "nearestObject [...]"); err says why not
Object* EvalObject(const char* code, std::string& err);
void RegisterCamera(HarnessServer& hs);
void TickCamera(); //!< diag_camera follow, every frame
} // namespace Poseidon::Dev::OpDiag::Internal

#endif // POSEIDON_DIAG
