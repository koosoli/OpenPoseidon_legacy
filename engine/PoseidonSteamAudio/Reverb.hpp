#pragma once

// Software EAX-style reverb for the Steam Audio backend.
//
// Steam Audio's own reverb needs a simulated or baked scene; the game only has
// coarse environment types (plain / city / forest / mountains / room) mapped to
// EAX presets, so the wet path is a classic algorithmic reverb parameterised
// directly by EFXEAXREVERBPROPERTIES:
//   pre-delay (reflections delay) -> early reflections (tapped delay)
//   -> + late delay -> input diffusion (2 all-passes)
//   -> 8-line feedback delay network (Householder matrix), per-line decay from
//      DecayTime and high-frequency damping from DecayHFRatio
//   -> decorrelated stereo out, scaled by Gain * ReflectionsGain / LateReverbGain.

#include <efx-presets.h>

#include <atomic>
#include <vector>

namespace Poseidon
{
namespace SteamAudio
{

class Reverb
{
  public:
    explicit Reverb(int sampleRate);

    // Apply preset parameters (already size-scaled).  Clears the tail when the
    // delay-line layout changes; otherwise only the gains/decays are updated.
    void Configure(const EFXEAXREVERBPROPERTIES& props);

    // Mono send in, stereo added to outL/outR.  n <= any size.
    void Process(const float* in, float* outL, float* outR, int n);

    void Reset();

    // Decay time of the active preset (s) — used for the wet-path distance
    // falloff, 0.001^(dist / (decay * c)).
    float DecayTime() const { return _decayTimeShared.load(std::memory_order_relaxed); }

  private:
    static constexpr int kLines = 8;
    static constexpr int kEarlyTaps = 6;

    struct DelayLine
    {
        std::vector<float> buf;
        int pos = 0;
        void Resize(int len);
        float Read(int delay) const
        {
            int i = pos - delay;
            if (i < 0)
                i += static_cast<int>(buf.size());
            return buf[i];
        }
        void Write(float v)
        {
            buf[pos] = v;
            if (++pos >= static_cast<int>(buf.size()))
                pos = 0;
        }
    };

    struct AllPass
    {
        DelayLine line;
        int delay = 1;
        float coef = 0.5f;
        float Process(float x);
    };

    int _sampleRate;
    bool _configured = false;

    // Input conditioning
    float _inputGain = 0.f;
    float _hfMix = 1.f;    // GainHF: how much of the content above HFReference passes
    float _lpState = 0.f;  // input low-pass state
    float _lpCoef = 0.f;   // input low-pass coefficient (HFReference)

    // Delays
    DelayLine _pre;        // pre-delay, long enough for reflections + late delay
    int _reflectionsDelay = 0;
    int _lateDelay = 0;
    int _earlyTap[kEarlyTaps] = {};
    float _earlyGain[kEarlyTaps] = {};
    float _earlyLevel = 0.f;

    AllPass _diffuse[2];

    DelayLine _lines[kLines];
    int _lineLen[kLines] = {};
    float _lineGain[kLines] = {};  // broadband decay per pass
    float _lineDamp[kLines] = {};  // one-pole low-pass coefficient (HF decay)
    float _lineLp[kLines] = {};    // low-pass state
    float _lateLevel = 0.f;
    float _densityScale = 1.f;

    float _decayTime = 1.49f;
    std::atomic<float> _decayTimeShared{1.49f}; // read by the audio thread
};

} // namespace SteamAudio
} // namespace Poseidon
