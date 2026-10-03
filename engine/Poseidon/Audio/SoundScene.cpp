#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Dev/Diag/OpDiag.hpp> // DIAG-001 hooks, empty unless POSEIDON_DIAG

#include <Poseidon/Audio/SoundScene.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Audio/Core/VoiceBudget.hpp>
#include <Poseidon/Audio/Speaker.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{
SoundScene* GSoundScene;
IAudioSystem* GSoundsys;

SoundScene::SoundScene()
{
    int i;
    for (i = 0; i < MaxEnvSounds; i++)
    {
        _envSoundRenewed[i] = false;
    }
    // init speakers
    const ParamEntry& cfg = Pars >> "CfgVoice";
    const ParamEntry& list = cfg >> "voices";
    for (i = 0; i < list.GetSize(); i++)
    {
        const ParamEntry& speaker = cfg >> RString(list[i]);
        Ref<BasicSpeaker> basic = new BasicSpeaker(speaker);
        const ParamEntry& variants = speaker >> "variants";
        for (int i = 0; i < variants.GetSize(); i++)
        {
            _speakers.Add(new Speaker(basic, variants[i]));
        }
    }
    {
        RString nameSpeaker = cfg >> "voicePlayer";
        const ParamEntry& speaker = cfg >> nameSpeaker;
        Ref<BasicSpeaker> basic = new BasicSpeaker(speaker);
        const ParamEntry& variants = speaker >> "variants";
        if (variants.GetSize() >= 1)
        {
            _speakerPlayer = new Speaker(basic, variants[0]);
        }
        else
        {
            _speakerPlayer = new Speaker(basic, 1.0f);
        }
    }
    _earAccommodation = 0.5;
}

SoundScene::~SoundScene() = default;

IWave* SoundScene::Open(const char* filename, bool is3D, bool accom)
{
    bool prealloc = !is3D && !accom;
    IWave* wave = GSoundsys->CreateWave(filename, is3D, prealloc);
#if POSEIDON_DIAG
    Dev::OpDiag::OnSoundOpen(filename, is3D, wave != nullptr); // DIAG-001: sound_play
#endif

    if (!wave)
    {
#if POSEIDON_DIAG
        Dev::OpDiag::OnSoundMissing("open", filename);
#endif
        return wave;
    }
    if (is3D)
    {
        _all3DSounds.Add(wave);
        if (strstr(wave->Name(), "m16"))
        {
            LOG_DEBUG(Audio, "M16 added to _all3DSounds, RefCount={}", wave->RefCounter());
        }
    }
    else
    {
        _all2DSounds.Add(wave);
    }
    wave->EnableAccommodation(accom);
    if (accom)
    {
        wave->SetAccommodation(_earAccommodation * _soundVolume);
    }
    return wave;
}
void SoundScene::AddSound(IWave* wave)
{
    if (wave)
    {
        _globalSounds.Add(wave);
    }
}

void SoundScene::DeleteSound(IWave* wave)
{
    if (wave)
    {
        _globalSounds.Delete(wave);
    }
}

class HistoryTracker
{ // Value type should be movable
    struct HistoryItem
    {
        float value;
        float howOld;
    };
    AutoArray<HistoryItem> _history;
    float _maxTime;
    int _maxItems;

  public:
    HistoryTracker(float maxTime, int maxItems);
    void Add(float value, float howOld = 0);
    void Evaluate(float& max, float& min, float& avg, float minOld = 0, float maxOld = FLT_MAX);
    void SetLimits(float maxTime, int maxItems)
    {
        _maxTime = maxTime;
        _maxItems = maxItems;
        GuardLimits();
    }
    void AdvanceTime(float deltaT);
    void GuardLimits();
};

HistoryTracker::HistoryTracker(float maxTime, int maxItems)
{
    _maxTime = maxTime;
    _maxItems = maxItems;
}

void HistoryTracker::GuardLimits()
{
    while (_history.Size() > _maxItems || _history.Size() > 0 && _history[0].howOld > _maxTime)
    {
        _history.Delete(0);
    }
}

void HistoryTracker::AdvanceTime(float deltaT)
{
    for (int i = 0; i < _history.Size(); i++)
    {
        _history[i].howOld += deltaT;
    }
    GuardLimits();
}

void HistoryTracker::Add(float value, float howOld)
{
    HistoryItem item;
    item.value = value;
    item.howOld = howOld;
    _history.Add(item);
    GuardLimits();
}

void HistoryTracker::Evaluate(float& max, float& min, float& avg, float minOld, float maxOld)
{
    float lMax = FLT_MIN;
    float lMin = FLT_MAX;
    float sum = 0;
    int n = 0;
    for (int i = 0; i < _history.Size(); i++)
    {
        const HistoryItem& item = _history[i];
        if (item.howOld > maxOld || item.howOld < minOld)
        {
            continue;
        }
        float value = item.value;
        sum += value;
        n++;
        saturateMin(lMin, value);
        saturateMax(lMax, value);
    }
    min = lMin;
    max = lMax;
    avg = (n > 0 ? sum / n : avg);
}

void SoundScene::AdvanceAll(float deltaT, bool paused)
{
    int i;
    for (i = 0; i < _globalSounds.Size();)
    { // remove terminated global sounds
        IWave* wave = _globalSounds[i];
        if (!wave || wave->IsTerminated())
        {
            _globalSounds.Delete(i);
        }
        else
        {
            i++;
        }
    }
    _all3DSounds.Compact();
    _all2DSounds.Compact();
    Vector3Val pos = GSoundsys->GetListenerPosition();

    // 3D voice budget — audio-invariants A-07 / A-09 / A-11.
    // totLoudness / nLoud feed the 2D loop and the accommodation
    // history below.
    audio::VoiceBudget3DInput voiceIn;
    voiceIn.listenerPos = pos;
    voiceIn.earAccommodation = _earAccommodation;
    voiceIn.soundVolume = _soundVolume;
    voiceIn.maxAudibleBudget = ENGINE_CONFIG.maxSounds;
    voiceIn.gamePaused = paused;
    voiceIn.deltaT = deltaT;
    audio::VoiceBudget3DResult voice = audio::ApplyVoiceBudget3D(_all3DSounds, voiceIn);
    float totLoudness = voice.totalLoudness;
    int nLoud = voice.nLoud;

    // 2D voice path — audio-invariants A-08.  No budget; uniform
    // Play+Advance vs Pause.  Adds to the running totLoudness.
    audio::Voice2DInput voice2DIn;
    voice2DIn.earAccommodation = _earAccommodation;
    voice2DIn.soundVolume = _soundVolume;
    voice2DIn.gamePaused = paused;
    voice2DIn.deltaT = deltaT;
    audio::Voice2DResult voice2D = audio::ApplyVoice2D(_all2DSounds, voice2DIn);
    totLoudness += voice2D.totalLoudness;

    // Report 3D + 2D pause + eviction counts — audio-invariants A-28.
    GSoundsys->SetVoiceBudgetCounters(voice.evictedThisFrame, voice.pausedThisFrame + voice2D.pausedThisFrame);

    if (!paused || _musicTrack && _musicTrack->GetSticky())
    {
        // musicTrack playback (Play/Stop) is controlled the same way
        // as any other 2D sound (musicTrack is listed in _all2DSounds)
        // simulate fade in/out
        if (Poseidon::Glob.time >= _musicTargetTime)
        {
            _musicVolume = _musicTargetVolume;
        }
        else
        {
            float timeRest = _musicTargetTime - Poseidon::Glob.time;
            float chSpeed = (_musicTargetVolume - _musicVolume) / timeRest;
            _musicVolume += deltaT * chSpeed;
        }
    }
    if (!paused)
    {
        // simulate fade in/out
        if (Poseidon::Glob.time >= _soundTargetTime)
        {
            _soundVolume = _soundTargetVolume;
        }
        else
        {
            float timeRest = _soundTargetTime - Poseidon::Glob.time;
            float chSpeed = (_soundTargetVolume - _soundVolume) / timeRest;
            _soundVolume += deltaT * chSpeed;
        }
    }
    if (_musicTrack)
    {
        _musicTrack->SetVolume(_musicInternalVolume * _musicVolume, _musicInternalFrequency);
    }
#if 1
    // compress dynamics
    // total loudness is calculated
    // adjust volume to accomodate

    // aggregated loudness
    // scan environmental sounds
    for (i = 0; i < MaxEnvSounds; i++)
    {
        IWave* wave = _envSounds[i];
        if (!wave)
        {
            continue;
        }
        float loudness = wave->GetVolume() / Square(wave->Distance2D());
        totLoudness += loudness;
    }

    static HistoryTracker history(2.5f, 500);
    history.Add(totLoudness);

    float maxLoud, minLoud, avgLoud;
    history.AdvanceTime(deltaT);
    history.Evaluate(maxLoud, minLoud, avgLoud);

    float aggLoud = maxLoud;

    float compressedLoudness = floatMax(aggLoud, 1e-20);

    //  very loud volume should be still loud after compression
    //  very soft volume should be still soft after compression
    //  10->2
    //  0.1->0.4

    float cLoudnessWanted = Interpolativ(aggLoud, 0.01, 10, 0.1, 2);

    float accLoudness = compressedLoudness * _earAccommodation * (1.0 / cLoudnessWanted);
    float logAccLoudness = -1000;
    if (accLoudness > 0)
    {
        logAccLoudness = -log(accLoudness);
    }
    saturate(logAccLoudness, -0.5f * deltaT, +0.1f * deltaT);
    float eaWanted = _earAccommodation * exp(logAccLoudness);

#if _ENABLE_CHEATS
    if (CHECK_DIAG(DESound))
    {
        GlobalShowMessage(200, "Accom %.4f, abs vol %.4f, accommodated to %.4f, wanted %.4f", log(_earAccommodation),
                          log(aggLoud), log(aggLoud * _earAccommodation), log(cLoudnessWanted));
    }
#endif

    const float maxEarAccommodation = 4;
    const float minEarAccommodation = 1.0 / 256;
    saturate(eaWanted, minEarAccommodation, maxEarAccommodation);
    _earAccommodation = eaWanted;

#endif
}

IWave* SoundScene::OpenAndPlay(const char* filename, Vector3Par pos, Vector3Par speed, float volume, float frequency)
{
    IWave* wave = Open(filename);
    if (wave)
    {
        wave->SetVolume(volume, frequency);
        wave->SetPosition(pos, speed, true);
        // No Play() here — matches original DX8 behavior.
        // Playback is started by AdvanceAll() based on loudness sorting.
    }
    return wave;
}
IWave* SoundScene::OpenAndPlayOnce(const char* filename, Vector3Par pos, Vector3Par speed, float volume,
                                   float frequency)
{
    LOG_DEBUG(Audio, "SoundScene::OpenAndPlayOnce: {}", filename);
    IWave* handle = Open(filename);
    if (handle)
    {
        handle->SetVolume(volume, frequency);
        handle->SetPosition(pos, speed, true);
        handle->Repeat(1);
        // No Play() here — matches original DX8 behavior.
        // Playback is started by AdvanceAll() based on loudness sorting.
    }
    return handle;
}

} // namespace Poseidon
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
namespace Poseidon
{

void SoundScene::SimulateSpeedOfSound(IWave* sound)
{
    float distance = sound->GetPosition().Distance(GScene->GetCamera()->Position());
    // sound will not start playing between frames
    // it is therefore more accurate to subtract half of expected frame duration
    // from wait time. This way we are effectively rounding to nearest frame instead
    // of rounding to next one
    float halfFrame = GEngine->GetAvgFrameDuration(4) * (0.001f * 0.5f);
    float skip = -distance * InvSpeedOfSound + halfFrame;
    // avoid skipping sound start, only delay is allowed
    if (skip < 0)
    {
        sound->Skip(skip);
    }
}

// FindSound lives in OptionsUI.cpp (mission -> campaign -> global config
// resolution incl. mission-dir prefix and the per-language voice suffix);
// same local-extern pattern DynSound.cpp uses.
const ParamEntry* FindSound(RString name, SoundPars& pars);
RString GetMissionDirectory();

void SoundScene::PreloadMissionAudio()
{
    if (!GSoundsys)
    {
        return;
    }
    const DWORD t0 = Poseidon::Foundation::GlobalTickCount();
    int queued = 0;

    // CfgSounds resolves through FindSound so the preload path and the
    // play path agree byte-for-byte on the file (mission dir, voice-lang
    // suffix, sound\ default prefix).
    if (const ParamEntry* cls = ::ExtParsMission.FindEntry("CfgSounds"))
    {
        for (int i = 0; i < cls->GetEntryCount(); i++)
        {
            const ParamEntry& e = cls->GetEntry(i);
            if (!e.IsClass())
            {
                continue;
            }
            SoundPars pars;
            if (FindSound(e.GetName(), pars) && pars.name.GetLength() > 0)
            {
                GSoundsys->PreloadWave(pars.name);
                queued++;
            }
        }
    }

    // CfgRadio sentence waves are mission-relative like CfgSounds but
    // FindSound only searches CfgSounds — apply the same prefix here.
    if (const ParamEntry* cls = ::ExtParsMission.FindEntry("CfgRadio"))
    {
        for (int i = 0; i < cls->GetEntryCount(); i++)
        {
            const ParamEntry& e = cls->GetEntry(i);
            if (!e.IsClass())
            {
                continue;
            }
            const ParamEntry* sound = e.FindEntry("sound");
            SoundPars pars;
            if (sound && GetValue(pars, *sound) && pars.name.GetLength() > 0)
            {
                GSoundsys->PreloadWave(GetMissionDirectory() + pars.name);
                queued++;
            }
        }
    }

    if (queued > 0)
    {
        LOG_DEBUG(Core, "LOAD: Audio preload queued {} waves in {}ms", queued,
                  Poseidon::Foundation::GlobalTickCount() - t0);
    }
}
int SoundScene::CountActive2DSpeechWaves() const
{
    int count = 0;
    for (int i = 0; i < _all2DSounds.Size(); i++)
    {
        IWave* wave = _all2DSounds[i];
        if (!wave || wave->IsTerminated())
            continue;
        if (wave->GetKind() != WaveSpeech)
            continue;
        count++;
    }
    return count;
}

RString SoundScene::Describe2DSpeechWaves() const
{
    RString out;
    bool first = true;
    for (int i = 0; i < _all2DSounds.Size(); i++)
    {
        IWave* wave = _all2DSounds[i];
        if (!wave)
            continue;
        if (wave->GetKind() != WaveSpeech)
            continue;
        const char* state;
        if (wave->IsTerminated())
            state = "terminated";
        else if (wave->IsStopped())
            state = "stopped"; // includes paused: WaveOAL clears _playing on pause
        else
            state = "playing";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s%s:%s", first ? "" : "|", (const char*)wave->Name(), state);
        out = out + RString(buf);
        first = false;
    }
    return out;
}

float SoundScene::Find2DWaveOffsetSeconds(const char* nameSubstring) const
{
    if (!nameSubstring || !*nameSubstring)
        return -1.f;
    for (int i = 0; i < _all2DSounds.Size(); i++)
    {
        IWave* wave = _all2DSounds[i];
        if (!wave)
            continue;
        // Case-insensitive substring match (DX8/OpenAL names are stored lowercased).
        const char* haystack = (const char*)wave->Name();
        if (!haystack)
            continue;
        if (strstr(haystack, nameSubstring) != nullptr)
            return wave->GetCurrentOffsetSeconds();
    }
    return -1.f;
}

IWave* SoundScene::OpenAndPlayOnce2D(const char* filename, float volume, float frequency, bool accomodate)
{
    IWave* handle = Open(filename, false, accomodate);
    if (handle)
    {
        handle->SetVolume(volume, frequency, true);
        handle->Repeat(1);
    }
    return handle;
}

IWave* SoundScene::OpenAndPlay2D(const char* filename, float volume, float frequency, bool accomodate)
{
    IWave* handle = Open(filename, false, accomodate);
    if (handle)
    {
        handle->SetVolume(volume, frequency, true);
    }
    return handle;
}

void SoundScene::Reset()
{
    _earAccommodation = 0.5;
    for (int i = 0; i < MaxEnvSounds; i++)
    {
        _envSounds[i].Free(), _envSoundRenewed[i] = false;
    }
    _globalSounds.Clear();

    _musicVolume = 0.5; // middle value
    _musicTargetVolume = 0.5;
    _musicTrack.Free(); // remove musical track
    _musicTargetTime = TIME_MIN;

    _soundVolume = 1; // middle value
    _soundTargetVolume = 1;
    _soundTargetTime = TIME_MIN;
}

const float EnvMagicConstant = 30;

float SoundScene::GetMusicVolume() const
{
    return _musicTargetVolume;
}

void SoundScene::SetMusicVolume(float vol, float time)
{
    _musicTargetVolume = vol;
    _musicTargetTime = Poseidon::Glob.time + time;
    if (time <= 0.01)
    {
        _musicVolume = _musicTargetVolume;
    }
}

float SoundScene::GetSoundVolume() const
{
    return _soundTargetVolume;
}

void SoundScene::SetSoundVolume(float vol, float time)
{
    _soundTargetVolume = vol;
    _soundTargetTime = Poseidon::Glob.time + time;
    if (time <= 0.01)
    {
        _soundVolume = _soundTargetVolume;
    }
}

void SoundScene::StartMusicTrack(const SoundPars& pars, float from)
{
    IWave* wave = GSoundScene->OpenAndPlayOnce2D(pars.name, pars.vol, pars.freq, false);
    if (!wave)
    {
        return;
    }
    wave->SetKind(WaveMusic);
    _musicTrack = wave;
    _musicInternalVolume = pars.vol;
    _musicInternalFrequency = pars.freq;
    wave->Skip(from);
}

void SoundScene::StopMusicTrack()
{
    _musicTrack.Free();
}

void SoundScene::SetEnvSound(const char* name, float volume)
{
    // see if sound can be reused
    if (!name || !*name)
    {
        return; // no sound
    }
    char lowName[256];
    snprintf(lowName, sizeof(lowName), "%s", (const char*)name);
    strlwr(lowName);
    int i;
    for (i = 0; i < MaxEnvSounds; i++)
    {
        IWave* snd = _envSounds[i];
        if (snd && !strcmp(snd->Name(), lowName))
        {
            snd->SetVolume(volume * EnvMagicConstant);
            snd->SetAccommodation(_earAccommodation * _soundVolume);
            // call play, this ensures streaming sounds are streaming
            snd->Play();
            _envSoundRenewed[i] = true;
            return;
        }
    }
    // no sound found, we have to set new
    // there must be one free slot
    for (i = 0; i < MaxEnvSounds; i++)
    {
        if (!_envSounds[i])
        {
            IWave* snd = GSoundsys->CreateWave(lowName, false);
            if (snd)
            {
                snd->SetAccommodation(_earAccommodation * _soundVolume);
                snd->SetVolume(volume * EnvMagicConstant);
                snd->Play();
                _envSounds[i] = snd;
                _envSoundRenewed[i] = true;
            }
            break;
        }
    }
}

void SoundScene::AdvanceEnvSounds()
{
    for (int i = 0; i < MaxEnvSounds; i++)
    {
        // sounds that were not renewed die
        if (!_envSoundRenewed[i] && _envSounds[i])
        {
            _envSounds[i].Free();
        }
        _envSoundRenewed[i] = false;
    }
}

IWave* SoundScene::Say(int speaker, RString id)
{
    if (speaker < 0)
    {
        PoseidonAssert(_speakerPlayer);
        return _speakerPlayer->Say(id);
    }
    speaker %= _speakers.Size();
    PoseidonAssert(_speakers[speaker]);
    return _speakers[speaker]->Say(id);
}

IWave* SoundScene::SayPause(float duration)
{
    IWave* wave = GSoundsys->CreateEmptyWave(duration);
    if (wave)
    {
        _all2DSounds.Add(wave);
    }
    return wave;
}

} // namespace Poseidon
