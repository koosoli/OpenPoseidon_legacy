#pragma once

// Software mixer for the Steam Audio backend.
//
// SDL3 owns the output device; its callback pulls 48 kHz stereo float frames
// from Mixer::Render.  Every playing sound is a MixVoice.  Per 512-sample
// frame each voice is resampled (source rate x pitch x Doppler), then
//   2D: gain-ramped straight into the stereo mix;
//   3D: down-mixed to mono, attenuated with the engine's DX8 distance model
//       (inverse-distance-clamped, reference = MinDistance), filtered by Steam
//       Audio's air absorption (iplDirectEffect) and placed with constant-power
//       stereo panning (speakers) or Steam Audio binaural HRTF (headphones).
//       An optional send feeds the EAX-preset reverb bus (Reverb.hpp).
//
// Threading: main thread creates voices, changes their Params and transport;
// the SDL audio thread renders.  Params are guarded by a per-voice mutex,
// transport state by atomics, the voice list by the mixer mutex.

#include <PoseidonSteamAudio/StreamingChunks.hpp>
#include <PoseidonSteamAudio/Reverb.hpp>

#include <phonon.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace Poseidon
{
namespace SteamAudio
{

constexpr int kSampleRate = 48000;
constexpr int kFrameSize = 512;
constexpr float kSpeedOfSound = 343.3f;

struct VoiceFormat
{
    int channels = 1;
    int bitsPerSample = 16;
    int sampleRate = 22050;
    int FrameBytes() const { return channels * (bitsPerSample / 8); }
};

class MixVoice
{
  public:
    // Main-thread controlled parameters (copied by the mixer each frame).
    struct Params
    {
        float gain = 1.f;    // linear source gain, already category/accommodation scaled
        float pitch = 1.f;   // playback-rate ratio (1 = native)
        bool is3D = false;   // positional (else plain stereo / centred mono)
        float pos[3] = {};   // world position, engine coordinates
        float vel[3] = {};   // world velocity, engine coordinates
        float refDist = 1.f; // full volume inside this distance
        float maxDist = 1e9f;
        float rolloff = 1.f;
        bool reverbSend = false;
    };

    enum class SourceKind
    {
        None,
        Resident, // whole decoded PCM block
        Streamed, // chunk queue filled by the streaming pump
        External  // mono int16 ring fed by the caller (voice chat, mic test)
    };

    MixVoice() = default;
    ~MixVoice();
    MixVoice(const MixVoice&) = delete;
    MixVoice& operator=(const MixVoice&) = delete;

    // --- source setup (main thread, before Mixer::Add) ---------------------
    void SetResident(std::shared_ptr<const std::vector<uint8_t>> pcm, const VoiceFormat& fmt);
    // `stream` must outlive the voice's registration with the mixer.
    void SetStreamed(Audio::Steam::StreamingBuffers* stream, const VoiceFormat& fmt);
    void SetExternal(int sampleRate);
    SourceKind Kind() const { return _kind; }
    const VoiceFormat& Format() const { return _fmt; }

    // --- parameters (any thread) -------------------------------------------
    void SetParams(const Params& p);
    Params GetParams() const;
    template <typename F> void ModifyParams(F&& f)
    {
        std::lock_guard<std::mutex> lk(_paramMutex);
        f(_params);
    }

    // --- transport (main thread) -------------------------------------------
    void SetLooping(bool loop) { _looping.store(loop, std::memory_order_release); }
    bool Looping() const { return _looping.load(std::memory_order_acquire); }
    void SetPaused(bool paused) { _paused.store(paused, std::memory_order_release); }
    bool Paused() const { return _paused.load(std::memory_order_acquire); }
    // True once a non-looping source played to its end (DX8: buffer status no longer PLAYING).
    bool Finished() const { return _finished.load(std::memory_order_acquire); }
    // Rewind to `frame` (resident) and clear the finished/resampler state.
    // Only call while the voice is not registered with the mixer.
    void Rewind(int64_t frame = 0);
    // Source frames consumed so far (playback position).
    int64_t PositionFrames() const { return _consumed.load(std::memory_order_acquire); }

    // --- external ring (voice chat / mic loopback) --------------------------
    void ExternalPush(const int16_t* samples, int count);
    int ExternalQueued() const;
    void ExternalClear();

  private:
    friend class Mixer;

    enum class FetchStatus
    {
        Ok,
        End,      // source genuinely ended
        Underrun  // no data right now, more may come
    };

    // Mixer-thread side.
    FetchStatus FillBlock();
    FetchStatus NextFrame(float* frame);
    // Resample `n` output frames at `step` source frames per output frame
    // into `out` (interleaved, source channel count).  Returns frames that
    // carry signal; the rest are zero-filled.  Sets _finished at the end.
    int Resample(float* out, int n, double step);
    bool EnsureEffects(IPLContext ctx, IPLHRTF hrtf, IPLAudioSettings& settings);
    void ReleaseEffects();

    SourceKind _kind = SourceKind::None;
    VoiceFormat _fmt;

    std::shared_ptr<const std::vector<uint8_t>> _pcm; // Resident
    int64_t _totalFrames = 0;
    int64_t _cursor = 0; // next resident frame to read (mixer thread)

    Audio::Steam::StreamingBuffers* _stream = nullptr; // Streamed
    Audio::Steam::DecodedChunk _chunk;
    size_t _chunkPos = 0;

    mutable std::mutex _ringMutex; // External
    std::deque<int16_t> _ring;

    mutable std::mutex _paramMutex;
    Params _params;

    std::atomic<bool> _looping{false};
    std::atomic<bool> _paused{false};
    std::atomic<bool> _finished{false};
    std::atomic<bool> _registered{false};
    std::atomic<int64_t> _consumed{0};

    // Resampler state (mixer thread).
    std::vector<float> _block; // decoded source frames, interleaved
    int _blockFrames = 0;
    int _blockPos = 0;
    float _prev[2] = {};
    float _cur[2] = {};
    double _frac = 0.0;
    bool _primed = false;
    bool _sourceEnded = false;

    // Gain ramp state (mixer thread).
    float _lastGain = -1.f;
    float _lastWet = 0.f;

    // Steam Audio objects (mixer thread).
    IPLContext _ctx = nullptr;
    IPLDirectEffect _direct = nullptr;
    IPLBinauralEffect _binaural = nullptr;
    IPLAudioBuffer _mono{};
    IPLAudioBuffer _monoOut{};
    IPLAudioBuffer _stereo{};
    // Speaker panning gains of the last block, crossfaded per sample into the
    // next so listener turns stay click-free (the binaural effect interpolates
    // internally).
    float _pan[2] = {};
    bool _havePan = false;
    bool _effectsFailed = false; // Steam Audio setup failed: plain panning only
};

class Mixer
{
  public:
    Mixer();
    ~Mixer();

    // The live mixer (owned by the sound system), or nullptr.
    static Mixer* Instance();

    // Open the named playback device (empty = system default).
    bool Open(const std::string& deviceName);
    void Close();
    bool IsOpen() const { return _stream != nullptr; }
    std::string DeviceName() const { return _deviceName; }
    static std::vector<std::string> ListDevices();

    void Add(const std::shared_ptr<MixVoice>& voice);
    // waitForRender: block until a render frame in flight has finished, so the
    // caller may free anything the voice reads (a streamed wave's chunk queue).
    void Remove(MixVoice* voice, bool waitForRender = false);

    void SetListener(const float pos[3], const float vel[3], const float dir[3], const float up[3]);
    void SetMasterGain(float gain) { _masterGain.store(gain, std::memory_order_release); }
    void SetDevicePaused(bool paused);

    // Reverb: nullptr disables the wet bus.
    void SetReverb(const EFXEAXREVERBPROPERTIES* props);
    bool ReverbEnabled() const { return _reverbOn.load(std::memory_order_acquire); }

    // Headphones: binaural HRTF.  Speakers: amplitude panning.
    void SetHeadphones(bool headphones) { _headphones.store(headphones, std::memory_order_release); }
    bool Headphones() const { return _headphones.load(std::memory_order_acquire); }

    int ActiveVoices() const;

    // Test hook: render `frames` interleaved stereo float frames on the calling
    // thread, exactly as the device callback would.  The mixer needs no open
    // device for this; only call it while IsOpen() is false so the SDL
    // callback never renders concurrently.
    void RenderOffline(float* interleavedStereo, int frames) { Render(interleavedStereo, frames); }

  private:
    static void Callback(void* userdata, SDL_AudioStream* stream, int additional, int total);
    void Render(float* interleaved, int frames);
    void RenderFrame(); // one kFrameSize block into _out
    void MixVoiceFrame(MixVoice& v, float* sendBuf);

    SDL_AudioStream* _stream = nullptr;
    std::string _deviceName;

    mutable std::mutex _voiceMutex;
    std::vector<std::shared_ptr<MixVoice>> _voices;
    std::mutex _renderMutex; // held while voices are rendered

    struct Listener
    {
        float pos[3] = {};
        float vel[3] = {};
        float dir[3] = {0.f, 0.f, 1.f};
        float up[3] = {0.f, 1.f, 0.f};
    };
    mutable std::mutex _listenerMutex;
    Listener _listener;

    std::atomic<float> _masterGain{1.f};
    std::atomic<bool> _headphones{false};

    std::mutex _reverbMutex;
    Reverb _reverb{kSampleRate};
    std::atomic<bool> _reverbOn{false};

    // Steam Audio.
    IPLContext _ctx = nullptr;
    IPLHRTF _hrtf = nullptr;
    IPLAudioSettings _settings{kSampleRate, kFrameSize};

    // Render scratch (audio thread).
    std::vector<float> _out;       // kFrameSize * 2, last rendered frame
    int _outPos = kFrameSize;      // consumed position in _out (frames)
    std::vector<float> _mixL, _mixR, _send, _tmp;
    std::vector<float> _monoScratch; // 3D mono when a voice has no Steam Audio effects
    std::vector<std::shared_ptr<MixVoice>> _snapshot;
};

} // namespace SteamAudio
} // namespace Poseidon
