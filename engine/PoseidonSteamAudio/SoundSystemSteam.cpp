#include <Poseidon/Foundation/Common/Global.hpp>
#include <PoseidonSteamAudio/SoundSystemSteam.hpp>
#include <PoseidonSteamAudio/WaveSteam.hpp>
#include <PoseidonSteamAudio/Mixer.hpp>
#include <PoseidonSteamAudio/EFXPresets.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/Audio/Shared/DX8Reference.hpp>
#include <Poseidon/Audio/Core/WaveLifecycle.hpp>
#include <Poseidon/Audio/AudioFactory.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/UI/Settings/AudioConfig.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Audio/Voice/VonCapture.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string.h>

// Global config singleton (defined in Foundation/Platform/Globals.cpp).
extern Poseidon::ParamFile Pars;

namespace Poseidon
{
// OGG (music, long speech) streams in chunks; WSS/WAV decode whole. Open Malprave also
// streams FLAC and MP3 (its patch 0002); OpenPoseidon has no decoder for them.
static bool IsStreamedSoundName(const char* name)
{
    const size_t n = name ? strlen(name) : 0;
    return n >= 4 && _stricmp(name + n - 4, ".ogg") == 0;
}

using Poseidon::Foundation::IsOutOfMemory;
using SteamAudio::Mixer;
const ParamEntry* FindMusic(RString name, SoundPars& pars);

namespace
{
const ParamEntry* PreviewConfig()
{
    const ParamEntry* sfx = Pars.FindEntry("CfgSFX");
    return sfx ? sfx->FindEntry("Preview") : nullptr;
}

bool ResolvePreviewMusicTrack(const char* entryName, SoundPars& pars)
{
    const ParamEntry* preview = PreviewConfig();
    const ParamEntry* entry = preview ? preview->FindEntry(entryName) : nullptr;
    if (!entry)
        return false;

    RString trackName = *entry;
    return trackName.GetLength() > 0 && FindMusic(trackName, pars) != nullptr && pars.name.GetLength() > 0;
}

bool ResolvePreviewSoundArray(const char* entryName, SoundPars& pars)
{
    const ParamEntry* preview = PreviewConfig();
    const ParamEntry* entry = preview ? preview->FindEntry(entryName) : nullptr;
    if (!entry || !GetValue(pars, *entry) || pars.name.GetLength() <= 0)
        return false;

    RString raw = (*entry)[0];
    const char* rawPath = raw;
    while (*rawPath == '\\' || *rawPath == '/')
        ++rawPath;

    // CfgSFX preview arrays are global-package samples.  Keep "voice\..." and
    // "music\..." rooted at their own banks instead of letting GetSoundName()
    // rewrite them under "sound\".
    if (_strnicmp(rawPath, "voice\\", 6) == 0 || _strnicmp(rawPath, "voice/", 6) == 0 ||
        _strnicmp(rawPath, "music\\", 6) == 0 || _strnicmp(rawPath, "music/", 6) == 0)
    {
        char normalized[256];
        strncpy(normalized, rawPath, sizeof(normalized) - 1);
        normalized[sizeof(normalized) - 1] = 0;
        for (char* c = normalized; *c; ++c)
            if (*c == '/')
                *c = '\\';
        strlwr(normalized);
        pars.name = normalized;
    }
    return pars.name.GetLength() > 0;
}

// DX8 DoSetEAXEnvironment mapping (forest intentionally uses "city").
const char* PresetForEnvironment(SoundEnvironmentType type)
{
    switch (type)
    {
        case SEPlain:
            return "plain";
        case SEForest:
        case SECity:
            return "city";
        case SEMountains:
            return "mountains";
        case SERoom:
            return "room";
    }
    return "generic";
}
} // namespace

SoundSystemSteam::SoundSystemSteam()
{
    // Decoded-PCM cache in the memory-budget panel; evictable under pressure.
    _pcmProbe.Register(
        "Audio.PcmCache", 0.05f, [this] { return _pcmCache.GetStats().bytes; },
        [] { return Audio::PcmCache::kMaxTotalBytes; }, [this] { return _pcmCache.GetStats().entries; },
        [this](size_t bytes) { return _pcmCache.Evict(bytes); });
}

SoundSystemSteam::~SoundSystemSteam()
{
    StopStreamThread();

    _soundReady = false;
    TerminatePreview();

    for (auto* wave : _waves)
    {
        if (wave)
            wave->DoStop();
    }
    _waves.clear();

    _mixer.reset();
    if (_sdlAudioInit)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    LOG_INFO(Audio, "System shutdown");
}

bool SoundSystemSteam::Init()
{
    return InitImpl(true);
}

bool SoundSystemSteam::InitOfflineForTest()
{
    return InitImpl(false);
}

bool SoundSystemSteam::InitImpl(bool openDevice)
{
    _offline = !openDevice;
    if (openDevice)
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            LOG_ERROR(Audio, "SDL audio init failed: {}", SDL_GetError());
            return false;
        }
        _sdlAudioInit = true;
    }

    _mixer = std::make_unique<Mixer>();
    if (openDevice && !_mixer->Open(std::string()))
    {
        _mixer.reset();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        _sdlAudioInit = false;
        return false;
    }

    // Default volumes (soundDX8.cpp:2000-2002).
    SetSpeechVolume(5.f);
    SetWaveVolume(5.f);
    SetCDVolume(5.f);

    LoadConfig(); // applies the EAX (reverb) setting, and with it the default environment's preset

    LOG_INFO(Audio, "Init OK: Steam Audio {}.{}.{} on '{}' ({} Hz, {} output)", STEAMAUDIO_VERSION_MAJOR,
             STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH,
             _mixer->DeviceName().empty() ? "<default>" : _mixer->DeviceName().c_str(), SteamAudio::kSampleRate,
             _headphones ? "headphones/HRTF" : "speakers");

    _soundReady = true;
    _doPreview = false;

    StartStreamThread();
    return true;
}

void SoundSystemSteam::StartStreamThread()
{
    if (_streamThread.joinable())
        return;
    _streamStop = false;
    _streamThread = std::thread([this] { StreamPumpLoop(); });
}

void SoundSystemSteam::StopStreamThread()
{
    if (!_streamThread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lk(_streamWakeMutex);
        _streamStop = true;
    }
    _streamWake.notify_all();
    _streamThread.join();
}

void SoundSystemSteam::StreamPumpLoop()
{
    // Keep every streamed wave's chunk queue topped up on a 10 ms cadence.
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk(_streamWakeMutex);
            _streamWake.wait_for(lk, std::chrono::milliseconds(10), [this] { return _streamStop; });
            if (_streamStop)
                return;
        }
        // Pass 1 (locked): pick waves that need a chunk; latching their
        // decodeInFlight keeps ~WaveSteam from freeing them mid-decode.
        std::vector<WaveSteam*> toDecode;
        {
            std::lock_guard<std::recursive_mutex> lk(_audioMutex);
            if (!_soundReady)
                continue;
            for (auto* wave : _waves)
            {
                if (wave && wave->IsStreamed() && wave->PumpDrainUpload())
                    toDecode.push_back(wave);
            }
        }
        // Pass 2 (lock-free): the multi-ms codec read.
        for (auto* wave : toDecode)
            wave->DecodeQueuedChunk();
    }
}

void SoundSystemSteam::RegisterWave(WaveSteam* wave)
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    if (wave)
        _waves.push_back(wave);
}

void SoundSystemSteam::UnregisterWave(WaveSteam* wave)
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    auto it = std::find(_waves.begin(), _waves.end(), wave);
    if (it != _waves.end())
        _waves.erase(it);
}

float SoundSystemSteam::GetWaveDuration(const char* filename)
{
    WaveStream* s = SoundLoadFile(filename);
    if (!s)
        return 0.f;
    WAVEFORMATEX fmt;
    s->GetFormat(fmt);
    const float dur = fmt.nAvgBytesPerSec > 0
                          ? static_cast<float>(s->GetUncompressedSize()) / static_cast<float>(fmt.nAvgBytesPerSec)
                          : 0.f;
    s->Release();
    return dur;
}

IWave* SoundSystemSteam::CreateEmptyWave(float duration)
{
    return new WaveSteam(this, duration);
}

IWave* SoundSystemSteam::CreateWave(const char* filename, bool is3D, bool /*prealloc*/)
{
    if (!_soundReady)
        return nullptr;
    return new WaveSteam(this, RString(filename), is3D);
}

IWave* SoundSystemSteam::CreateWaveFromMemory(const void* data, size_t size, const char* ext, bool is3D)
{
    if (!_soundReady)
        return nullptr;
    Ref<WaveStream> stream = SoundLoadMemory(data, size, ext);
    if (!stream)
        return nullptr;
    auto* wave = new WaveSteam(this, 0.f);
    wave->_is3D = is3D;
    wave->_createdAs3D = is3D;
    wave->_stream = stream;
    wave->LoadHeader();
    return wave;
}

void SoundSystemSteam::SetListener(Vector3Par pos, Vector3Par vel, Vector3Par dir, Vector3Par up)
{
    _listenerPos = pos;
    _listenerVel = vel;
    _listenerDir = dir;
    _listenerUp = up;
    if (_mixer)
    {
        const float p[3] = {pos[0], pos[1], pos[2]};
        const float v[3] = {vel[0], vel[1], vel[2]};
        const float d[3] = {dir[0], dir[1], dir[2]};
        const float u[3] = {up[0], up[1], up[2]};
        _mixer->SetListener(p, v, d, u);
    }
}

void SoundSystemSteam::Commit()
{
    if (!_soundReady)
        return;

    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    for (auto* wave : _waves)
    {
        if (wave && wave->_wantPlaying)
        {
            wave->DoPlay();
            // A start waiting on its async decode stays queued for the next Commit.
            if (!wave->_startDeferred)
                wave->_wantPlaying = false;
        }
    }
    ProcessPreview();
}

void SoundSystemSteam::Activate(bool active)
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    if (_outputActive == active)
        return;
    _outputActive = active;
    ApplyListenerGain();
    UpdateDeviceState();
}

void SoundSystemSteam::SetSimulationRunning(bool running)
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    if (_simulationRunning == running)
        return;
    _simulationRunning = running;
    UpdateDeviceState();
}

void SoundSystemSteam::UpdateDeviceState()
{
    // Suspend the device only when the window is inactive AND the game is
    // paused.
    const bool suspend = !_outputActive && !_simulationRunning;
    if (_deviceSuspended == suspend || !_mixer)
        return;
    _mixer->SetDevicePaused(suspend);
    _deviceSuspended = suspend;
}

// --- Reverb (environment presets) -------------------------------------------

bool SoundSystemSteam::EnableEAX(bool val)
{
    _eaxEnabled = val;
    ApplyReverb();
    LOG_INFO(Audio, "EAX (environment reverb) {}", _eaxEnabled ? "enabled" : "disabled");
    return _eaxEnabled;
}

void SoundSystemSteam::ApplyReverb()
{
    if (!_mixer)
        return;
    if (!_eaxEnabled)
    {
        _mixer->SetReverb(nullptr);
        return;
    }
    const EFX::PresetEntry* entry = EFX::FindPreset(PresetForEnvironment(_env.type));
    if (!entry)
        return;
    const EFXEAXREVERBPROPERTIES props = EFX::ScaledPreset(*entry, _env.size);
    _mixer->SetReverb(&props);
    LOG_DEBUG(Audio, "Reverb: environment type={} size={}", static_cast<int>(_env.type), _env.size);
}

void SoundSystemSteam::SetEnvironment(const SoundEnvironment& env)
{
    // Change detection — 1:1 with DX8 (soundDX8.cpp:2596-2614).
    if (env.type == _env.type && fabs(env.size - _env.size) < 1 && fabs(env.density - _env.density) < 0.05)
        return;
    _env = env;
    ApplyReverb();
}

bool SoundSystemSteam::ApplyPresetByName(const char* presetName, float size)
{
    const EFX::PresetEntry* entry = EFX::FindPreset(presetName);
    if (!entry || !_mixer || !_eaxEnabled)
        return false;
    const EFXEAXREVERBPROPERTIES props = EFX::ScaledPreset(*entry, size);
    _mixer->SetReverb(&props);
    return true;
}

IAudioSystem::HrtfStatus SoundSystemSteam::GetHrtfStatus() const
{
    HrtfStatus status;
    status.supported = true;
    status.active = _headphones;
    status.specifier = _headphones ? "Steam Audio default HRTF" : "";
    status.deviceStatus = _headphones ? HrtfDeviceStatus::Enabled : HrtfDeviceStatus::Disabled;
    return status;
}

bool SoundSystemSteam::ApplyReverbPreset(const char* presetName, float size)
{
    const bool ok = ApplyPresetByName(presetName, size);
    if (ok)
        LOG_DEBUG(Audio, "Reverb: applied preset '{}' size={}", presetName, size);
    return ok;
}

void SoundSystemSteam::SetHeadphones(bool headphones)
{
    _headphones = headphones;
    if (_mixer)
        _mixer->SetHeadphones(headphones);
    LOG_INFO(Audio, "3D output: {}", headphones ? "headphones (HRTF)" : "speakers (panning)");
}

// --- Config -------------------------------------------------------------------

namespace
{
// Per-machine audio.cfg directly under the user-data directory.
std::string AudioConfigPath()
{
    return GamePaths::Instance().UserDir() + "audio.cfg";
}

struct LiveAudioEnv : AudioConfig::Environment
{
    const SoundSystemSteam* sys;
    explicit LiveAudioEnv(const SoundSystemSteam* s) : sys(s) {}
    std::vector<std::string> ListOutputDevices() const override
    {
        return sys ? sys->ListOutputDevices() : std::vector<std::string>{};
    }
    std::vector<std::string> ListInputDevices() const override { return VoNCapture::listDevices(); }
};
} // namespace

void SoundSystemSteam::LoadConfig()
{
    const std::string path = AudioConfigPath();

    AudioConfig cfg;
    if (!cfg.Load(path))
    {
        cfg.LoadDefaults();
        cfg.Save(path);
        LOG_INFO(Audio, "LoadConfig: created defaults at '{}'", path);
    }

    // Normalized values stay in memory only — a temporarily unplugged device
    // must not lose its saved selection.
    LiveAudioEnv env(this);
    if (cfg.Normalize(env))
        LOG_INFO(Audio, "LoadConfig: normalized invalid fields (not persisted)");

    // UI scale 0..100 percent, backend 0..10.
    SetCDVolume(cfg.musicVolume / 10.0f);
    SetWaveVolume(cfg.effectsVolume / 10.0f);
    SetSpeechVolume(cfg.speechVolume / 10.0f);
    SetHrtfMode(static_cast<HrtfMode>(std::clamp(cfg.hrtfMode, 0, 2)));
    EnableEAX(cfg.eaxEnabled);

    if (!cfg.outputDevice.empty())
        SwitchOutputDevice(cfg.outputDevice.c_str());
    _selectedInputDevice = cfg.inputDevice;

    LOG_DEBUG(Audio, "LoadConfig: music={}% fx={}% speech={}% headphones={} out='{}' in='{}'",
              cfg.musicVolume, cfg.effectsVolume, cfg.speechVolume, _headphones, cfg.outputDevice,
              cfg.inputDevice);
}

void SoundSystemSteam::SaveConfig()
{
    if (IsOutOfMemory())
        return;

    AudioConfig cfg;
    cfg.musicVolume = (int)(GetCDVolume() * 10.0f + 0.5f);
    cfg.effectsVolume = (int)(GetWaveVolume() * 10.0f + 0.5f);
    cfg.speechVolume = (int)(GetSpeechVolume() * 10.0f + 0.5f);
    cfg.hrtfMode = static_cast<int>(_hrtfMode);
    cfg.eaxEnabled = _eaxEnabled;
    cfg.outputDevice = _selectedDevice;
    cfg.inputDevice = _selectedInputDevice;

    const std::string path = AudioConfigPath();
    if (!cfg.Save(path))
        LOG_WARN(Audio, "SaveConfig: failed to write '{}'", path);
}

// --- Output devices -------------------------------------------------------------

std::vector<std::string> SoundSystemSteam::ListOutputDevices() const
{
    return Mixer::ListDevices();
}

std::string SoundSystemSteam::GetCurrentOutputDevice() const
{
    if (!_selectedDevice.empty())
        return _selectedDevice;
    if (_mixer && !_mixer->DeviceName().empty())
        return _mixer->DeviceName();
    return {};
}

bool SoundSystemSteam::SwitchOutputDevice(const char* name)
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    if (!_mixer || _offline)
        return false;
    const std::string requested = (name && *name) ? std::string(name) : std::string();
    if (requested == _selectedDevice && _mixer->IsOpen())
        return true;

    // Voices live in the mixer, not the device: reopening the output keeps
    // every playing sound (and the device preview) going without a restart.
    if (!_mixer->Open(requested))
    {
        LOG_ERROR(Audio, "SwitchOutputDevice: failed to open '{}'", requested.empty() ? "<default>" : requested.c_str());
        _mixer->Open(_selectedDevice); // fall back to the previous device
        return false;
    }
    _selectedDevice = requested;
    _deviceSuspended = false;
    UpdateDeviceState();
    LOG_INFO(Audio, "SwitchOutputDevice: switched to '{}'", _selectedDevice.empty() ? "<default>" : _selectedDevice.c_str());
    return true;
}

// --- Volumes ---------------------------------------------------------------------

void SoundSystemSteam::SetCDVolume(float v)
{
    _cdVolume = std::clamp(v, 0.f, 10.f);
    UpdateMixer();
}

void SoundSystemSteam::SetWaveVolume(float v)
{
    _waveVolume = std::clamp(v, 0.f, 10.f);
    UpdateMixer();
}

void SoundSystemSteam::SetSpeechVolume(float v)
{
    _speechVolume = std::clamp(v, 0.f, 10.f);
    UpdateMixer();
}

void SoundSystemSteam::SetVoiceBudgetCounters(int evictedThisFrame, int pausedThisFrame)
{
    _evictedThisFrame = evictedThisFrame;
    _pausedThisFrame = pausedThisFrame;
}

AudioCounters SoundSystemSteam::GetCounters() const
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    AudioCounters c;
    c.allocFailures = _allocFailures;
    c.evictedThisFrame = _evictedThisFrame;
    c.pausedThisFrame = _pausedThisFrame;
    for (auto* w : _waves)
    {
        if (!w)
            continue;
        WaveSteam* mw = const_cast<WaveSteam*>(w);
        const WaveState st = mw->State();
        if (st == WaveState::Playing || st == WaveState::WaitingDeferred)
        {
            if (mw->Get3D())
                ++c.active3D;
            else
                ++c.active2D;
        }
        else if (st == WaveState::Paused)
        {
            ++c.paused;
        }
    }
    return c;
}

void SoundSystemSteam::UpdateMixer()
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    const float waveVolExp = DX8::expVol(_waveVolume);
    const float speechVolExp = DX8::expVol(_speechVolume);
    const float musicVolExp = DX8::expVol(_cdVolume);

    // Per-wave adjusts: ratio of channel to max (soundDX8.cpp:2977-2979).
    const float maxVolExp = (std::max)({musicVolExp, waveVolExp, speechVolExp});
    _volumeAdjustEffect = DX8::mixerAdjust(waveVolExp, maxVolExp);
    _volumeAdjustSpeech = DX8::mixerAdjust(speechVolExp, maxVolExp);
    _volumeAdjustMusic = DX8::mixerAdjust(musicVolExp, maxVolExp);

    ApplyListenerGain();

    for (auto* wave : _waves)
    {
        if (wave)
            wave->SetVolumeAdjust(_volumeAdjustEffect, _volumeAdjustSpeech, _volumeAdjustMusic);
    }
}

void SoundSystemSteam::ApplyListenerGain()
{
    // DX8 waveOutSetVolume(maxSlider * 0.1) — the master output gain.
    const float maxRawVol = (std::max)({_waveVolume, _speechVolume, _cdVolume});
    _listenerGain = _outputActive ? DX8::listenerGain(maxRawVol) : 0.f;
    if (_mixer)
        _mixer->SetMasterGain(_listenerGain);
}

// --- Previews (settings screen) -------------------------------------------------

static bool PreviewAdvance(IWave* wave, float deltaT)
{
    if (!wave)
        return false;
    if (!wave->IsMuted())
    {
        wave->Play();
    }
    else
    {
        wave->Stop();
        wave->Advance(deltaT);
    }
    return wave->IsTerminated();
}

void SoundSystemSteam::StartPreview()
{
    // Restart the speech -> effect -> music chain; the 200 ms delay absorbs
    // slider-drag bursts.
    TerminatePreview();
    _doPreview = true;
    _previewTime = GlobalTickCount() + 200;
}

void SoundSystemSteam::TerminatePreview()
{
    _previewSpeech.Free();
    _previewEffect.Free();
    _previewMusic.Free();
    _previewCategory.Free();
    _previewDevice.Free();
    _devicePreviewAnchorMs = 0;
    _devicePreviewLengthSec = 0.0f;
    _previewSpatial.Free();
    _doPreview = false;
}

void SoundSystemSteam::StartSpatialPreview()
{

    _previewSpeech.Free();
    _previewEffect.Free();
    _previewMusic.Free();
    _previewCategory.Free();
    _previewDevice.Free();
    _previewSpatial.Free();
    _doPreview = false;

    const ParamEntry& cfg = Pars >> "CfgSFX" >> "Preview" >> "effect";
    RStringB name = cfg[0];
    IWave* wave = CreateWave(name, true, false);
    _previewSpatial = wave;
    if (!wave)
    {
        LOG_WARN(Audio, "StartSpatialPreview: failed to create wave '{}'", static_cast<const char*>(name));
        return;
    }
    wave->SetKind(WaveEffect);
    wave->EnableAccommodation(false);
    wave->SetSticky(true);
    wave->SetVolume((float)cfg[1], cfg[2], true);
    // Off to the listener's right so speakers vs headphones (HRTF) is an
    // obvious A/B.
    const Vector3 right = _listenerUp.CrossProduct(_listenerDir);
    wave->SetPosition(_listenerPos + right * 6 + _listenerDir * 2, _listenerVel, true);
    wave->Repeat(1);
    wave->Play();
}

void SoundSystemSteam::SuppressMusicForPreview()
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    // Idempotent: a second Suppress would overwrite nothing useful.
    if (!_suppressedMusic.empty())
        return;
    for (WaveSteam* w : _waves)
    {
        if (!w || w->GetKind() != WaveMusic || !w->_playing)
            continue;
        _suppressedMusic.push_back(w);
        w->_previewMute = true;
        w->ApplyVolume();
    }
    LOG_DEBUG(Audio, "SuppressMusicForPreview: ducked {} music wave(s)", _suppressedMusic.size());
}

void SoundSystemSteam::ResumeMusicForPreview()
{
    std::lock_guard<std::recursive_mutex> lk(_audioMutex);
    if (_suppressedMusic.empty())
        return;
    for (WaveSteam* wave : _suppressedMusic)
    {
        // Skip waves deleted while suppressed (no longer registered).
        if (std::find(_waves.begin(), _waves.end(), wave) == _waves.end())
            continue;
        wave->_previewMute = false;
        wave->ApplyVolume();
    }
    _suppressedMusic.clear();
}

void SoundSystemSteam::PreloadWave(const char* name)
{
    if (!name || !*name)
        return;
    // OGG / FLAC / MP3 stream in chunks and never enter the PCM cache.
    if (IsStreamedSoundName(name))
        return;
    if (_pcmCache.Contains(name))
        return;

    auto work = [this, path = std::string(name)]()
    {
        Ref<WaveStream> stream = SoundLoadFile(path.c_str());
        if (!stream)
            return;
        const int size = stream->GetUncompressedSize();
        if (size <= 0 || static_cast<size_t>(size) > Audio::PcmCache::kMaxEntryBytes)
            return; // oversized waves decode async at play time instead
        auto entry = std::make_shared<Audio::PcmCache::Entry>();
        stream->GetFormat(entry->fmt);
        entry->uncompressedSize = static_cast<unsigned int>(size);
        entry->pcm.assign(static_cast<size_t>(size), 0);
        stream->GetData(entry->pcm.data(), 0, size);
        _pcmCache.Insert(path.c_str(), std::move(entry));
    };

    // Loading goes through the FileCache, which is main-thread only: from a pool worker it
    // raced the main thread and crashed in FileCache::Maintain (Arma 1 fos_lament, 131-mission
    // run on d3ac4c62) -- the race the OpenAL backend fixed the same way (d8f2e36). The waves
    // preloaded here are short effects, so they are read and decoded right here.
    work();
}

void SoundSystemSteam::StartCategoryPreview(WaveKind kind)
{

    _previewSpeech.Free();
    _previewEffect.Free();
    _previewMusic.Free();
    _previewDevice.Free();
    _devicePreviewAnchorMs = 0;
    _devicePreviewLengthSec = 0.0f;
    _doPreview = false;

    // Restart even for the same kind so a slider drag is heard immediately.
    _previewCategory.Free();

    const char* slot = (kind == WaveSpeech) ? "speech" : (kind == WaveMusic) ? "music" : "effect";
    SoundPars pars{};
    if (kind == WaveMusic)
    {
        if (!ResolvePreviewMusicTrack("musicTrack", pars))
            ResolvePreviewSoundArray(slot, pars);
    }
    else
    {
        ResolvePreviewSoundArray(slot, pars);
    }
    RStringB name = pars.name;
    const bool is3D = (kind == WaveEffect); // only the effect sample is 3D
    IWave* wave = CreateWave(name, is3D, !is3D);
    _previewCategory = wave;
    if (!wave)
    {
        LOG_WARN(Audio, "StartCategoryPreview({}): failed to create wave '{}'", slot, static_cast<const char*>(name));
        return;
    }
    wave->SetKind(kind);
    wave->EnableAccommodation(false);
    wave->SetSticky(true);
    const float baseVol = (kind == WaveMusic) ? 0.5f : 1.0f; // legacy halves music
    wave->SetVolume(pars.vol * baseVol, pars.freq, true);
    if (is3D)
        wave->SetPosition(_listenerPos + _listenerDir * 50, _listenerVel, true);
    wave->Repeat(kind == WaveMusic ? 0 : 1);
    wave->Play();
    ++_categoryPreviewRestartCount; // audio-invariants A-18
}

void SoundSystemSteam::StartDevicePreview()
{
    if (_previewDevice)
        return;
    SoundPars pars{};
    if (!ResolvePreviewMusicTrack("deviceMusicTrack", pars) && !ResolvePreviewMusicTrack("musicTrack", pars))
        ResolvePreviewSoundArray("music", pars);

    RStringB name = pars.name;
    IWave* wave = CreateWave(name, false, true);
    _previewDevice = wave;
    if (!wave)
    {
        LOG_WARN(Audio, "StartDevicePreview: no preview wave found");
        return;
    }
    wave->SetKind(WaveMusic);
    wave->EnableAccommodation(false);
    wave->SetSticky(true);
    wave->SetVolume(pars.vol * 0.5f, pars.freq, true);
    wave->Repeat(0); // loops while the row is focused

    // Wall-clock anchored offset (audio-invariants A-17): re-focusing resumes
    // where the track would be instead of restarting it.
    const uint32_t now = GlobalTickCount();
    if (_devicePreviewAnchorMs == 0)
    {
        _devicePreviewAnchorMs = now;
        _devicePreviewLengthSec = wave->GetLength();
    }
    const float seekTo = audio::DevicePreviewOffsetSeconds(now, _devicePreviewAnchorMs, _devicePreviewLengthSec);
    if (auto* w = dynamic_cast<WaveSteam*>(wave))
        w->_pendingSeekSeconds = seekTo;
    wave->Play();
}

void SoundSystemSteam::ProcessPreview()
{
    if (!_doPreview)
        return;
    if (GlobalTickCount() < _previewTime)
        return;

    const float deltaT = 0.1f;

    if (!_previewSpeech)
    {
        SoundPars pars{};
        ResolvePreviewSoundArray("speech", pars);
        RStringB name = pars.name;
        IWave* wave = CreateWave(name, false, true);
        _previewSpeech = wave;
        if (wave)
        {
            wave->SetKind(WaveSpeech);
            wave->EnableAccommodation(false);
            wave->SetSticky(true);
            wave->SetVolume(pars.vol, pars.freq, true);
            wave->Repeat(1);
            wave->Play();
        }
        else
        {
            LOG_WARN(Audio, "Preview: failed to create speech wave '{}'", static_cast<const char*>(name));
            _doPreview = false;
        }
    }
    else if (PreviewAdvance(_previewSpeech, deltaT) && !_previewEffect)
    {
        SoundPars pars{};
        ResolvePreviewSoundArray("effect", pars);
        RStringB name = pars.name;
        IWave* wave = CreateWave(name, true, false);
        _previewEffect = wave;
        if (wave)
        {
            wave->EnableAccommodation(false);
            wave->SetSticky(true);
            wave->SetVolume(pars.vol, pars.freq, true);
            wave->SetPosition(_listenerPos + _listenerDir * 50, _listenerVel, true);
            wave->Repeat(1);
            wave->Play();
        }
    }
    else if (PreviewAdvance(_previewEffect, deltaT) && !_previewMusic)
    {
        SoundPars pars{};
        if (!ResolvePreviewMusicTrack("musicTrack", pars))
            ResolvePreviewSoundArray("music", pars);
        RStringB name = pars.name;
        IWave* wave = CreateWave(name, false, true);
        _previewMusic = wave;
        if (wave)
        {
            wave->SetKind(WaveMusic);
            wave->EnableAccommodation(false);
            wave->SetSticky(true);
            wave->SetVolume(pars.vol * 0.5f, pars.freq, true);
            wave->Repeat(1);
            wave->Play();
        }
    }
    else if (PreviewAdvance(_previewMusic, deltaT))
    {
        _doPreview = false;
        _previewSpeech.Free();
        _previewEffect.Free();
        _previewMusic.Free();
    }
}

// --- Factory ----------------------------------------------------------------------

IAudioSystem* CreateSoundSystemSteam()
{
    auto* sys = new SoundSystemSteam();
    if (!sys->Init())
    {
        delete sys;
        return nullptr;
    }
    return sys;
}

namespace
{
bool IsSteamAudioAvailable()
{
    // SDL audio must initialise and expose at least one playback device.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return false;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return count > 0;
}
} // namespace

void RegisterSteamAudioBackend()
{
    static bool registered = false;
    if (registered)
        return;
    AudioFactory::Register(AudioBackendDescriptor{
        "steam",
        "Steam Audio engine (SDL3 output)",
        // Above OpenAL (1): the default engine since 2026-10-12 (owner). Auto-selection falls back
        // to OpenAL when Steam Audio cannot open an output device; --audio openal or the dev
        // panel's Audio tab picks OpenAL explicitly.
        2,
        &CreateSoundSystemSteam,
        &IsSteamAudioAvailable,
    });
    registered = true;
}

} // namespace Poseidon
