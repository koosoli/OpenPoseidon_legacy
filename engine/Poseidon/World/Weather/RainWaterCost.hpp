#pragma once
// Producer-thread diagnostic only. No solver reads these accumulators.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Import only the two API contracts used below. windows.h imports GDI's
// GetObject macro and rewrites Landscape::GetObject in shared-header consumers.
// SDK API contracts require their architecture macro when windows.h has not
// initialized it already (including standalone native policy tests).
#if defined(_M_AMD64) && !defined(_AMD64_)
#define _AMD64_
#elif defined(_M_IX86) && !defined(_X86_)
#define _X86_
#elif defined(_M_ARM64) && !defined(_ARM64_)
#define _ARM64_
#endif
#include <processthreadsapi.h>
#include <processenv.h>
#endif

namespace Poseidon::RainWaterCost
{
inline bool Enabled() {
    static const bool on=[] {
#ifdef _WIN32
        char value[2]{};
        return GetEnvironmentVariableA("POSEIDON_RAIN_WATER_COST",value,sizeof(value))==1 && value[0]=='1';
#else
        const char* v=std::getenv("POSEIDON_RAIN_WATER_COST");
        return v && std::strcmp(v,"1")==0;
#endif
    }();
    return on;
}
inline uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct CpuTime { uint64_t ns=0; bool valid=false; };
inline CpuTime ThreadCpuTime() {
#ifdef _WIN32
    FILETIME created{},exited{},kernel{},user{};
    if (!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) return {};
    const auto ticks=[](FILETIME t) { return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
    return {(ticks(kernel)+ticks(user))*100,true};
#else
    return {}; // Unavailable never means zero CPU cost.
#endif
}
// Preserve the historical five indices; fused transfer/flow has its own scope.
enum class Phase : unsigned { SourceSink, Clear, RawTransfer, LimitedFlux, FinalCells, FusedFlux, Count };
inline constexpr size_t PhaseCount=static_cast<size_t>(Phase::Count);
inline const char* Name(Phase p) {
    constexpr const char* names[] = {"source-sink","clear","raw-transfer","limited-flux","final-cells","fused-flux"};
    return static_cast<size_t>(p)<PhaseCount ? names[static_cast<size_t>(p)] : "invalid";
}
struct Accum {
    uint64_t advances=0,steps=0,emptySteps=0,noStepAdvances=0,multiStepAdvances=0;
    uint64_t cpuSamples=0,slowAdvances=0;
    double wallMs=0,maxWallMs=0,cpuMs=0;
    double cpuAtMaxWallMs=-1;
    unsigned stepsAtMaxWall=0;
    std::array<uint64_t,PhaseCount> phaseCalls{};
    std::array<double,PhaseCount> phaseMs{},phaseMaxMs{};
};
inline Accum& Costs() { static Accum a; return a; }
inline void Reset() { Costs()=Accum{}; }
inline void RecordPhase(Accum& a,Phase phase,uint64_t ns) {
    const auto i=static_cast<size_t>(phase); if(i>=PhaseCount)return;
    const double ms=double(ns)*1e-6; ++a.phaseCalls[i]; a.phaseMs[i]+=ms;
    if(ms>a.phaseMaxMs[i])a.phaseMaxMs[i]=ms;
}
inline void RecordAdvance(Accum& a,uint64_t wallNs,CpuTime begin,CpuTime end,unsigned steps) {
    const double wallMs=double(wallNs)*1e-6;
    const bool cpuValid=begin.valid && end.valid && end.ns>=begin.ns;
    const double cpuMs=cpuValid ? double(end.ns-begin.ns)*1e-6 : -1;
    ++a.advances;a.steps+=steps;a.noStepAdvances+=steps==0;a.multiStepAdvances+=steps>1;
    a.wallMs+=wallMs;a.slowAdvances+=wallMs>=50;
    if(cpuValid){++a.cpuSamples;a.cpuMs+=cpuMs;}
    if(wallMs>a.maxWallMs){a.maxWallMs=wallMs;a.cpuAtMaxWallMs=cpuMs;a.stepsAtMaxWall=steps;}
}
class AdvanceScope {
public:
    AdvanceScope():_enabled(Enabled()) { if(_enabled){_cpu=ThreadCpuTime();_start=NowNs();} }
    ~AdvanceScope(){if(_enabled){const auto elapsed=NowNs()-_start;
        RecordAdvance(Costs(),elapsed,_cpu,ThreadCpuTime(),_steps);}}
    AdvanceScope(const AdvanceScope&)=delete;
    AdvanceScope& operator=(const AdvanceScope&)=delete;
    bool Active()const{return _enabled;}
    void Step(bool empty){if(_enabled){++_steps;Costs().emptySteps+=empty;}}
private:
    bool _enabled;uint64_t _start=0;CpuTime _cpu;unsigned _steps=0;
};
class PhaseTimer {
public:
    explicit PhaseTimer(bool enabled):_enabled(enabled){if(_enabled)_start=NowNs();}
    void End(Phase phase){if(_enabled){const auto now=NowNs();RecordPhase(Costs(),phase,now-_start);_start=now;}}
private:
    bool _enabled;uint64_t _start=0;
};
}
