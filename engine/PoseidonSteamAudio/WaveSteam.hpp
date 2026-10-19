#pragma once

#include <Poseidon/Foundation/Common/Global.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/Core/AudioState.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <PoseidonSteamAudio/StreamingChunks.hpp>
#include <atomic>
#include <memory>
#include <vector>

namespace Poseidon
{
class SoundSystemSteam;

namespace SteamAudio
{
class MixVoice;
}

// One playable sound on the Steam Audio backend.  Lifecycle, queue chaining,
// delayed start and DX8 volume/pitch semantics follow the original DX8
// Wave8; playback runs on a SteamAudio::MixVoice ("playing" == voice
// registered with the mixer).
class WaveSteam : public IWave
{
    friend class SoundSystemSteam;

  public:
    // Async-load state machine (Decode on a worker for large assets, voice
    // creation on the main thread).
    enum class LoadState : int
    {
        NotStarted = 0,
        DecodePending = 1,
        UploadPending = 2,
        Resident = 3,
        Failed = 4,
    };

  private:
    SoundSystemSteam* _sys;
    AudioState _state;
    Ref<WaveStream> _stream;

    std::shared_ptr<SteamAudio::MixVoice> _voice;

    std::atomic<int> _loadState{static_cast<int>(LoadState::NotStarted)};
    std::vector<uint8_t> _pendingPcm;

    // OGG sources play from chunks decoded by the streaming pump.
    ::Poseidon::Audio::Steam::StreamingBuffers _streaming;
    // Format cached by OpenStreaming so the main thread never queries the
    // (non-thread-safe) Vorbis decoder while the pump is reading it.
    WAVEFORMATEX _streamFormat{};

    bool _is3D = false;
    bool _createdAs3D = false;
    bool _playing = false;
    bool _paused = false;
    bool _resumeInPlace = false; // Resume() of a paused voice, pending Commit
    bool _startDeferred = false; // DoPlay waited on an async decode; retry next Commit
    int64_t _streamBaseFrames = 0; // stream position the current streamed playback started at
    bool _wantPlaying = false;
    bool _everPlayed = false; // FSM: Created vs StoppedReplayable (A-01)
    bool _loaded = false;
    bool _loadError = false;
    bool _isStreamed = false;

    float _volume = 1.0f;
    float _accommodation = 1.0f;
    float _volumeAdjust = 1.0f;
    float _gainSet = 0.0f;
    // Requested pitch ratio, re-applied whenever the voice (re)starts so a
    // deferred start doesn't click from 1.0 to the engine pitch (vehicles).
    float _pitchRatio = 1.0f;
    float _stopThreshold = FLT_MAX;
    bool _accomEnabled = true;
    // Seek applied when DoPlay starts the voice (device-picker preview).
    float _pendingSeekSeconds = 0.0f;
    // Audio-screen ducking: forces gain 0 while set (see SuppressMusicForPreview).
    bool _previewMute = false;
    WaveKind _kind = WaveEffect;
    bool _unmuffled = false; // (OP) past the underwater filter

    Vector3 _position;
    Vector3 _velocity;

    // Geometry occlusion (Occlusion.hpp), refreshed by SoundSystemSteam::UpdateOcclusion and
    // carried into the voice by Apply3D: per-band gain, 1 = clear.
    float _occlBands[3] = {1.f, 1.f, 1.f};
    uint32_t _occlNextMs = 0;
    float _occlTerrain = 0.f;
    bool _occlBuilding = false;

    // Queue chaining (radio callsign + message): the successor stays _queued
    // until this wave's IsTerminated() activates it.
    Ref<WaveSteam> _queueNext;
    bool _queued = false;

    void LoadHeader();
    void Load();
    void DecodePcm();
    void CreateResidentVoice();
    void KickAsyncLoad();
    void DecodeOneChunk();
    void OpenStreaming();
    void Unload();
    void ApplyVolume();
    void ApplyPitch();
    void Apply3D();
    void ApplyReverbSend();
    void DoPlay();
    void DoStop();
    // Stash the voice's playback position into _state.curPosition (bytes) so
    // a later DoPlay resumes from it — the DX8 Stop/Play resume contract.
    void StoreCurrentOffset();
    bool PumpDrainUpload(bool kickDecode);
    int FrameBytes() const;

  public:
    float GetCurrentOffsetSeconds() const override;

    LoadState GetLoadState() const { return static_cast<LoadState>(_loadState.load(std::memory_order_acquire)); }

    ::Poseidon::Audio::Steam::StreamingBuffers& StreamingState() { return _streaming; }
    const ::Poseidon::Audio::Steam::StreamingBuffers& StreamingState() const { return _streaming; }


    bool IsStreamed() const { return _isStreamed; }
    // Streaming pump (SoundSystemSteam::StreamPumpLoop): PumpDrainUpload runs
    // under the audio mutex and latches decodeInFlight when a chunk is needed;
    // DecodeQueuedChunk then decodes it lock-free.
    bool PumpDrainUpload();
    void DecodeQueuedChunk();

    // Test-only: pitch ratio applied to the voice (1.0 if none).
    float AppliedPitchForTest() const;
    float AppliedGainForTest() const;
    void OpenStreamingForTest() { OpenStreaming(); }
    void DecodeOneChunkForTest() { DecodeOneChunk(); }

    WaveSteam(SoundSystemSteam* sys, RString name, bool is3D);
    WaveSteam(SoundSystemSteam* sys, float delay);
    ~WaveSteam() override;

    void SetVolumeAdjust(float volEffect, float volSpeech, float volMusic);

    void Queue(IWave* wave, int repeat) override;
    void Repeat(int repeat) override;
    void Play() override;
    void Stop() override;
    void Pause() override;
    void Resume() override;
    void LastLoop() override;
    void PlayUntilStopValue(float time) override;
    void SetStopValue(float time) override;

    bool IsWaiting() override;
    void Skip(float deltaT) override;
    void Advance(float deltaT) override;
    float GetLength() const override;
    bool IsStopped() override;
    bool IsTerminated() override;
    void Restart() override;
    WaveState State() override;

    bool IsMuted() const override { return _gainSet <= 1e-20f; }
    float GetVolume() const override { return _volume; }
    void SetVolume(float v, float freq, bool immediate) override;
    float GetFileMaxVolume() const override { return _state.maxVolume; }
    float GetFileAvgVolume() const override { return _state.avgVolume; }

    void SetAccommodation(float a) override
    {
        _accommodation = a;
        ApplyVolume();
    }
    float GetAccommodation() const override { return _accommodation; }
    void EnableAccommodation(bool e) override { _accomEnabled = e; }
    bool AccommodationEnabled() const override { return _accomEnabled; }

    void SetKind(WaveKind k) override;
    void SetUnmuffled(bool unmuffled) override
    {
        _unmuffled = unmuffled;
        ApplyVolume();
    }
    WaveKind GetKind() const override { return _kind; }
    float GetVolumeAdjust() const { return _volumeAdjust; }

    void SetPosition(Vector3Par pos, Vector3Par vel, bool) override;
    Vector3 GetPosition() const override { return _position; }
    void Set3D(bool v) override
    {
        _is3D = v;
        Apply3D();
        ApplyVolume(); // DX8: 3D toggle changes the volume path (100x boost for 3D-off)
    }
    bool Get3D() const override { return _is3D; }
    float Distance2D() const override { return 1.0f; }
};

} // namespace Poseidon
