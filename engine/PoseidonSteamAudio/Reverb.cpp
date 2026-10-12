#include <PoseidonSteamAudio/Reverb.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon
{
namespace SteamAudio
{
namespace
{
constexpr float kPi = 3.14159265358979f;

// Mutually prime base lengths (samples at 48 kHz, ~21-62 ms) for the FDN.
constexpr int kBaseLengths[8] = {1031, 1327, 1523, 1871, 2083, 2357, 2633, 2953};

// Early reflection tap times (fraction of a 40 ms window) and gains.
constexpr float kTapTimes[6] = {0.07f, 0.19f, 0.33f, 0.48f, 0.71f, 0.97f};
constexpr float kTapGains[6] = {0.84f, 0.73f, 0.62f, 0.51f, 0.41f, 0.32f};

// One-pole low-pass coefficient for cutoff `hz`.
float OnePoleCoef(float hz, int sampleRate)
{
    if (hz <= 0.f)
        return 0.f;
    const float c = std::exp(-2.f * kPi * hz / static_cast<float>(sampleRate));
    return std::clamp(c, 0.f, 0.9999f);
}
} // namespace

void Reverb::DelayLine::Resize(int len)
{
    if (len < 1)
        len = 1;
    buf.assign(static_cast<size_t>(len), 0.f);
    pos = 0;
}

float Reverb::AllPass::Process(float x)
{
    const float d = line.Read(delay);
    const float y = d - coef * x;
    line.Write(x + coef * y);
    return y;
}

Reverb::Reverb(int sampleRate) : _sampleRate(sampleRate)
{
    // Pre-delay sized for the EFX maxima: reflections 0.3 s + late 0.1 s.
    _pre.Resize(static_cast<int>(0.42f * sampleRate) + 4);
    _diffuse[0].line.Resize(static_cast<int>(0.0049f * sampleRate) + 2);
    _diffuse[1].line.Resize(static_cast<int>(0.0071f * sampleRate) + 2);
    _diffuse[0].delay = static_cast<int>(0.0049f * sampleRate);
    _diffuse[1].delay = static_cast<int>(0.0071f * sampleRate);
    // Longest density scale is 1.4 x the base lengths.
    for (int i = 0; i < kLines; i++)
        _lines[i].Resize(static_cast<int>(kBaseLengths[i] * 1.4f * sampleRate / 48000.f) + 4);
}

void Reverb::Reset()
{
    for (float& v : _pre.buf)
        v = 0.f;
    for (AllPass& ap : _diffuse)
        for (float& v : ap.line.buf)
            v = 0.f;
    for (DelayLine& line : _lines)
        for (float& v : line.buf)
            v = 0.f;
    for (float& s : _lineLp)
        s = 0.f;
    _lpState = 0.f;
}

void Reverb::Configure(const EFXEAXREVERBPROPERTIES& p)
{
    const float fs = static_cast<float>(_sampleRate);
    _decayTime = std::max(p.flDecayTime, 0.1f);
    _decayTimeShared.store(_decayTime, std::memory_order_relaxed);

    // Input: overall gain and the GainHF shelf at HFReference.
    _inputGain = p.flGain;
    _hfMix = std::clamp(p.flGainHF, 0.f, 1.f);
    _lpCoef = OnePoleCoef(p.flHFReference > 0.f ? p.flHFReference : 5000.f, _sampleRate);

    // Delays.
    _reflectionsDelay = std::clamp(static_cast<int>(p.flReflectionsDelay * fs), 0, static_cast<int>(_pre.buf.size()) - 2);
    _lateDelay = std::clamp(_reflectionsDelay + static_cast<int>(p.flLateReverbDelay * fs), 1,
                            static_cast<int>(_pre.buf.size()) - 2);

    // Early reflections spread over a window scaled by density (small rooms
    // reflect sooner and closer together).
    const float window = (0.010f + 0.030f * p.flDensity) * fs;
    for (int i = 0; i < kEarlyTaps; i++)
    {
        _earlyTap[i] = std::clamp(_reflectionsDelay + static_cast<int>(kTapTimes[i] * window), 1,
                                  static_cast<int>(_pre.buf.size()) - 2);
        _earlyGain[i] = kTapGains[i];
    }
    _earlyLevel = std::clamp(p.flReflectionsGain, 0.f, 3.16f) * 0.35f;

    // Diffusion: all-pass strength.
    const float diff = std::clamp(p.flDiffusion, 0.f, 1.f) * 0.7f;
    _diffuse[0].coef = diff;
    _diffuse[1].coef = diff * 0.9f;

    // FDN line lengths follow density (EAX: modal density / room size).
    const float densityScale = 0.6f + 0.8f * std::clamp(p.flDensity, 0.f, 1.f);
    if (!_configured || std::fabs(densityScale - _densityScale) > 0.05f)
    {
        _densityScale = densityScale;
        for (int i = 0; i < kLines; i++)
        {
            _lineLen[i] = std::max(16, static_cast<int>(kBaseLengths[i] * densityScale * fs / 48000.f));
            _lines[i].Resize(static_cast<int>(_lines[i].buf.size()));
        }
        for (float& s : _lineLp)
            s = 0.f;
    }

    // Per-line decay: -60 dB after DecayTime; HF decays over DecayTime * HFRatio.
    const float hfRatio = std::clamp(p.flDecayHFRatio, 0.1f, 2.0f);
    for (int i = 0; i < kLines; i++)
    {
        const float len = static_cast<float>(_lineLen[i]);
        const float g = std::pow(10.f, -3.f * len / (_decayTime * fs));
        const float gHF = std::pow(10.f, -3.f * len / (_decayTime * hfRatio * fs));
        _lineGain[i] = g;
        // One-pole in the loop: DC gain 1, Nyquist gain (1-a)/(1+a) = gHF/g.
        const float r = std::clamp(gHF / std::max(g, 1e-6f), 0.f, 1.f);
        _lineDamp[i] = (1.f - r) / (1.f + r);
    }
    // Normalise the tail so a long decay doesn't explode in level.
    _lateLevel = std::clamp(p.flLateReverbGain, 0.f, 10.f) * 0.25f;

    _configured = true;
}

void Reverb::Process(const float* in, float* outL, float* outR, int n)
{
    if (!_configured)
        return;

    for (int s = 0; s < n; s++)
    {
        // Input gain + GainHF shelf.
        const float x = in[s] * _inputGain;
        _lpState = (1.f - _lpCoef) * x + _lpCoef * _lpState;
        const float shaped = _lpState + _hfMix * (x - _lpState);
        _pre.Write(shaped);

        // Early reflections, alternated left/right.
        float eL = 0.f, eR = 0.f;
        for (int t = 0; t < kEarlyTaps; t++)
        {
            const float v = _pre.Read(_earlyTap[t]) * _earlyGain[t];
            if (t & 1)
                eR += v;
            else
                eL += v;
        }

        // Late reverb input: delayed + diffused.
        float lateIn = _pre.Read(_lateDelay);
        lateIn = _diffuse[0].Process(lateIn);
        lateIn = _diffuse[1].Process(lateIn);

        // FDN: read, damp, Householder mix, write back with input.
        float v[kLines];
        float sum = 0.f;
        for (int i = 0; i < kLines; i++)
        {
            float d = _lines[i].Read(_lineLen[i]) * _lineGain[i];
            _lineLp[i] = (1.f - _lineDamp[i]) * d + _lineDamp[i] * _lineLp[i];
            v[i] = _lineLp[i];
            sum += v[i];
        }
        const float householder = sum * (2.f / kLines);
        float lL = 0.f, lR = 0.f;
        for (int i = 0; i < kLines; i++)
        {
            _lines[i].Write(v[i] - householder + lateIn);
            // Decorrelated stereo taps.
            if (i & 1)
                lR += (i & 2) ? -v[i] : v[i];
            else
                lL += (i & 2) ? -v[i] : v[i];
        }

        outL[s] += eL * _earlyLevel + lL * _lateLevel;
        outR[s] += eR * _earlyLevel + lR * _lateLevel;
    }
}

} // namespace SteamAudio
} // namespace Poseidon
