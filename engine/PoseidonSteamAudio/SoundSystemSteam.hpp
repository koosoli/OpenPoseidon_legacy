#pragma once

#include <Poseidon/Foundation/Common/Global.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/Shared/PcmCache.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <PoseidonSteamAudio/Occlusion.hpp>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Poseidon
{
class WaveSteam;

namespace SteamAudio
{
class Mixer;
}

// Audio system on Valve's Steam Audio (spatialisation) + SDL3 (device I/O),
// with an in-engine software mixer.  Volume, preview, config and lifecycle
// behaviour follow the original DX8 sound system.
class SoundSystemSteam : public IAudioSystem
{
    friend class WaveSteam;

    std::unique_ptr<SteamAudio::Mixer> _mixer;
    bool _soundReady = false;
    bool _outputActive = true;
    bool _simulationRunning = true;
    bool _deviceSuspended = false;
    bool _sdlAudioInit = false; // this instance holds an SDL audio subsystem ref
    bool _offline = false;      // InitOfflineForTest: mixer without an output device

    std::vector<WaveSteam*> _waves;

    // Streaming pump: refills streamed (OGG) waves every ~10 ms independent of
    // the render frame.  _audioMutex serialises the pump's walk of _waves and
    // each wave's streaming state against main-thread register/unregister,
    // DoPlay/DoStop/Pause and the wave destructor.
    mutable std::recursive_mutex _audioMutex;
    std::thread _streamThread;
    std::mutex _streamWakeMutex;
    std::condition_variable _streamWake;
    bool _streamStop = false;
    void StreamPumpLoop();
    void StartStreamThread();
    void StopStreamThread();

    float _waveVolume = 5.0f;
    float _speechVolume = 5.0f;
    float _cdVolume = 5.0f;

    // Listener reverb, always on (DX8 parity: environment type + size -> preset).
    SoundEnvironment _env{SEPlain, 100.f, 0.5f};

    bool _headphones = false;
    HrtfMode _hrtfMode = HrtfMode::Auto;
    bool _eaxEnabled = false;

    float _volumeAdjustEffect = 1.0f;
    float _volumeAdjustSpeech = 1.0f;
    float _volumeAdjustMusic = 1.0f;
    float _listenerGain = 0.0f;

    Audio::PcmCache _pcmCache;
    Foundation::MemoryDomainProbe _pcmProbe;

    Vector3 _listenerPos;
    Vector3 _listenerVel;
    Vector3 _listenerDir;
    Vector3 _listenerUp;

    // Settings-screen previews.
    bool _doPreview = false;
    uint32_t _previewTime = 0;
    Ref<IWave> _previewSpeech;
    Ref<IWave> _previewEffect;
    Ref<IWave> _previewMusic;
    Ref<IWave> _previewCategory;
    Ref<IWave> _previewDevice;
    uint32_t _devicePreviewAnchorMs = 0;
    float _devicePreviewLengthSec = 0.0f;
    Ref<IWave> _previewSpatial;
    std::vector<WaveSteam*> _suppressedMusic;

    int _allocFailures = 0;
    int _categoryPreviewRestartCount = 0;
    int _evictedThisFrame = 0;
    int _pausedThisFrame = 0;

    std::string _selectedDevice;
    std::string _selectedInputDevice;

    void UpdateMixer();
    // Refresh the geometry occlusion of playing / starting 3D waves (Occlusion.hpp): each at
    // ~10 Hz, at most kOcclusionPerCommit queries per Commit, main-thread cost logged every 10 s.
    void UpdateOcclusion();
    double _occlCostMs = 0.0;
    double _occlMaxMs = 0.0;
    int _occlQueries = 0;
    int _occlCommits = 0;
    uint32_t _occlReportMs = 0;
    int _occlCacheHits = 0;
    double _occlSlowestMs = 0.0;
    float _occlSlowestDist = 0.f;
    struct OcclusionCacheEntry
    {
        int32_t k[6] = {};
        uint32_t timeMs = 0;
        SteamAudio::OcclusionResult result;
        bool valid = false;
    };
    OcclusionCacheEntry _occlCache[32];
    unsigned _occlCacheNext = 0;
    void ApplyListenerGain();
    void UpdateDeviceState();
    void ProcessPreview();
    void ApplyReverb();
    bool ApplyPresetByName(const char* presetName, float size);
    bool InitImpl(bool openDevice);

  public:
    SoundSystemSteam();
    ~SoundSystemSteam() override;

    bool Init();
    // Test seam: initialise like Init() but never touch SDL or open an output
    // device.  The mixer is rendered by hand through Mixer::RenderOffline and
    // output-device switching is disabled.
    bool InitOfflineForTest();
    SteamAudio::Mixer* GetMixer() const { return _mixer.get(); }

    void RegisterWave(WaveSteam* wave);
    void UnregisterWave(WaveSteam* wave);

    float GetWaveDuration(const char* filename) override;
    IWave* CreateEmptyWave(float duration) override;
    IWave* CreateWave(const char* filename, bool is3D = true, bool prealloc = false) override;
    IWave* CreateWaveFromMemory(const void* data, size_t size, const char* ext, bool is3D) override;

    void SetListener(Vector3Par pos, Vector3Par vel, Vector3Par dir, Vector3Par up) override;
    Vector3 GetListenerPosition() const override { return _listenerPos; }
    void Commit() override;
    void Activate(bool active) override;
    void SetSimulationRunning(bool running) override;
    void SetEnvironment(const SoundEnvironment& env) override;
    float ListenerGainForTest() const { return _listenerGain; }
    bool DeviceSuspendedForTest() const { return _deviceSuspended; }

    float GetCDVolume() const override { return _cdVolume; }
    void SetCDVolume(float v) override;
    void SetWaveVolume(float v) override;
    float GetWaveVolume() override { return _waveVolume; }
    void SetSpeechVolume(float v) override;
    float GetSpeechVolume() override { return _speechVolume; }

    void StartPreview() override;
    void TerminatePreview() override;
    void StartCategoryPreview(WaveKind kind) override;
    void PreloadWave(const char* name) override;
    int WaveCacheEntries() const override { return static_cast<int>(_pcmCache.GetStats().entries); }
    int WaveCacheHits() const override { return static_cast<int>(_pcmCache.GetStats().hits); }
    void StartDevicePreview() override;
    void StartEAXPreview() override { StartSpatialPreview(); }
    void StartSpatialPreview();
    void SuppressMusicForPreview() override;
    void ResumeMusicForPreview() override;
    int CountSuppressedMusicWaves() const override { return static_cast<int>(_suppressedMusic.size()); }
    int CountCategoryPreviewRestarts() const override { return _categoryPreviewRestartCount; }
    AudioCounters GetCounters() const override;
    void SetVoiceBudgetCounters(int evictedThisFrame, int pausedThisFrame) override;
    void FlushBank(QFBank*) override {}

    // OpenPoseidon's IAudioSystem names (the port's README): the EFX preset by name is our
    // ApplyReverbPreset; reverb is always on here (an EFX-preset FDN), and this is a software mixer.
    bool ApplyEFXByName(const char* presetName, float size) override { return ApplyReverbPreset(presetName, size); }
    bool ApplyReverbPreset(const char* presetName, float size);
    // The environment reverb follows the EAX setting (Options > Audio), OFF by default as on
    // OpenAL: Open Malprave ran it always on, which over OFP's open landscapes read as a huge echo.
    bool EnableEAX(bool val) override;
    bool GetEAX() const override { return _eaxEnabled; }
    bool EnableHWAccel(bool) override { return false; }
    bool GetHWAccel() const override { return false; }

    // HRTF is Steam Audio's binaural renderer for headphones. Auto has no meaning here (SDL does
    // not report headphones); it is taken as speakers, the Open Malprave default.
    bool SetHrtfMode(HrtfMode mode) override { _hrtfMode = mode; SetHeadphones(mode == HrtfMode::On); return true; }
    HrtfMode GetHrtfMode() const override { return _hrtfMode; }
    HrtfStatus GetHrtfStatus() const override;
    std::vector<std::string> ListHrtfSpecifiers() const override { return {"Steam Audio default HRTF"}; }

    void SetHeadphones(bool headphones);
    bool GetHeadphones() const { return _headphones; }

    void LoadConfig() override;
    void SaveConfig() override;

    std::vector<std::string> ListOutputDevices() const override;
    std::string GetCurrentOutputDevice() const override;
    bool SwitchOutputDevice(const char* name) override;

    const std::string& GetSelectedInputDevice() const override { return _selectedInputDevice; }
    void SetSelectedInputDevice(const std::string& name) override { _selectedInputDevice = name; }
};

IAudioSystem* CreateSoundSystemSteam();

} // namespace Poseidon
