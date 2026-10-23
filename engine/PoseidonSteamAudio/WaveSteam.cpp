#include <algorithm>
#include <Poseidon/Foundation/Common/Global.hpp>
#include <PoseidonSteamAudio/WaveSteam.hpp>
#include <PoseidonSteamAudio/SoundSystemSteam.hpp>
#include <PoseidonSteamAudio/Mixer.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/Audio/Core/WaveLogic.hpp>
#include <Poseidon/Audio/Shared/AudioMath.hpp>
#include <Poseidon/Audio/Shared/DX8Reference.hpp>
#include <Poseidon/Audio/SoundTestLog.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace Poseidon
{
// OGG (music, long speech) streams in chunks; WSS/WAV decode whole. Open Malprave also
// streams FLAC and MP3 (its patch 0002); OpenPoseidon has no decoder for them.
static bool IsStreamedSoundName(const char* name)
{
    const size_t n = name ? strlen(name) : 0;
    return n >= 4 && _stricmp(name + n - 4, ".ogg") == 0;
}
using SteamAudio::Mixer;
using SteamAudio::MixVoice;
using SteamAudio::VoiceFormat;

namespace
{
VoiceFormat ToVoiceFormat(const WAVEFORMATEX& fmt)
{
    VoiceFormat vf;
    vf.channels = fmt.nChannels > 0 ? fmt.nChannels : 1;
    vf.bitsPerSample = fmt.wBitsPerSample == 8 ? 8 : 16;
    vf.sampleRate = fmt.nSamplesPerSec > 0 ? static_cast<int>(fmt.nSamplesPerSec) : 22050;
    return vf;
}

} // namespace

WaveSteam::WaveSteam(SoundSystemSteam* sys, RString name, bool is3D)
    : IWave(name), _sys(sys), _is3D(is3D), _createdAs3D(is3D)
{
    // DX8 WaveBuffers8::Reset(): start from the current mixer adjust.
    _volumeAdjust = _sys->_volumeAdjustEffect;

    const auto perfOpenStart = ::Poseidon::Dev::Perf::Now();

    const char* nameStr = static_cast<const char*>(name);
    if (!nameStr || !nameStr[0])
    {
        // Empty sound name — a config slot with no sound assigned. Not an error.
        Audio::SoundTestLog::NoteRequest(nameStr, false);
        _loadError = true;
        return;
    }
    _stream = SoundLoadFile(nameStr);
    // The test run's sound census (SoundTestLog) counts this backend's waves too.
    Audio::SoundTestLog::NoteRequest(nameStr, _stream != nullptr);
    if (!_stream)
    {
        _loadError = true;
        LOG_ERROR(Audio, "Load failed: '{}' (file not found or unsupported format)", nameStr);
        return;
    }
    LoadHeader();

    // OGG (music, long speech) streams in chunks; WSS/WAV decode whole — they
    // are small and need tight start latency (radio chatter, SFX).
    _isStreamed = IsStreamedSoundName(nameStr);

    _sys->RegisterWave(this);

    const double perfOpenMs = ::Poseidon::Dev::Perf::ElapsedMs(perfOpenStart);
    if (perfOpenMs >= 1.0)
        LOG_DEBUG(Audio, "PERF: WaveSteam::Open {} took {:.2f}ms", nameStr, perfOpenMs);
    ::Poseidon::Dev::Perf::EmitTraceEventAsset(Poseidon::Foundation::LogCategory::Audio, "WaveSteam::Open",
                                               perfOpenStart, nameStr);
}

WaveSteam::WaveSteam(SoundSystemSteam* sys, float delay) : IWave(GetPauseName()), _sys(sys)
{
    _volumeAdjust = _sys->_volumeAdjustEffect;

    // Empty wave — delay placeholder (1.99 Wave8(sys, delay)): a negative
    // _curPosition in 1 kHz / 1-byte units counts the delay down in Skip.
    _state.frequency = 1000;
    _state.sSize = 1;
    _state.size = 0;
    if (delay > 0.f)
        _state.curPosition = static_cast<int>(delay * -1000.f);
    _sys->RegisterWave(this);
}

WaveSteam::~WaveSteam()
{
    // The streaming pump decodes lock-free without an owning ref.  It latches
    // decodeInFlight only under _audioMutex after checking `aborted`, so with
    // the lock held no new decode can start; wait out one already running
    // (the decode itself never takes the lock).
    _streaming.aborted.store(true, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    while (_streaming.decodeInFlight.load(std::memory_order_acquire))
        std::this_thread::yield();
    _sys->UnregisterWave(this);

    if (_voice)
    {
        // A streamed voice reads _streaming — wait out a render in flight.
        if (Mixer* mixer = _sys->GetMixer())
            mixer->Remove(_voice.get(), _isStreamed);
        _voice.reset();
    }
}

int WaveSteam::FrameBytes() const
{
    return _state.nChannel * _state.sSize;
}

void WaveSteam::LoadHeader()
{
    if (!_stream || _loadError)
        return;

    WAVEFORMATEX fmt;
    _stream->GetFormat(fmt);

    const int size = _stream->GetUncompressedSize();
    if (size <= 0)
    {
        _loadError = true;
        LOG_ERROR(Audio, "Header FAIL: {} (size={})", static_cast<const char*>(Name()), size);
        return;
    }

    _state.size = static_cast<unsigned int>(size);
    _state.frequency = static_cast<long>(fmt.nSamplesPerSec);
    _state.nChannel = fmt.nChannels;
    _state.sSize = fmt.wBitsPerSample / 8;

    if (fmt.nAvgBytesPerSec > 0)
        _state.maxVolume = 1.f;
}

void WaveSteam::DecodePcm()
{
    // Worker-thread safe: stream reads into _pendingPcm only.
    if (!_stream || _loaded || _loadError)
        return;

    const auto perfDecodeStart = ::Poseidon::Dev::Perf::Now();
    _loadState.store(static_cast<int>(LoadState::DecodePending), std::memory_order_release);

    // PCM cache: a preloaded / previously decoded wave skips the decode.  The
    // cache is keyed by file name, so in-memory waves (empty name) bypass it.
    const bool cacheable = !_isStreamed && _sys && Name().GetLength() > 0;
    if (cacheable)
    {
        if (auto hit = _sys->_pcmCache.Find(static_cast<const char*>(Name())))
        {
            _pendingPcm.assign(hit->pcm.begin(), hit->pcm.end());
            _loadState.store(static_cast<int>(LoadState::UploadPending), std::memory_order_release);
            ::Poseidon::Dev::Perf::EmitTraceEventAssetNum(Poseidon::Foundation::LogCategory::Audio,
                                                          "WaveSteam::DecodeCacheHit", perfDecodeStart,
                                                          static_cast<const char*>(Name()), "bytes",
                                                          static_cast<long long>(_state.size));
            return;
        }
    }

    _pendingPcm.assign(static_cast<size_t>(_state.size), 0);
    _stream->GetData(_pendingPcm.data(), 0, static_cast<int>(_state.size));

    if (cacheable && _state.size <= Audio::PcmCache::kMaxEntryBytes)
    {
        auto entry = std::make_shared<Audio::PcmCache::Entry>();
        _stream->GetFormat(entry->fmt);
        entry->uncompressedSize = _state.size;
        entry->pcm = _pendingPcm;
        _sys->_pcmCache.Insert(static_cast<const char*>(Name()), std::move(entry));
    }

    _loadState.store(static_cast<int>(LoadState::UploadPending), std::memory_order_release);
    ::Poseidon::Dev::Perf::EmitTraceEventAssetNum(Poseidon::Foundation::LogCategory::Audio, "WaveSteam::Decode",
                                                  perfDecodeStart, static_cast<const char*>(Name()), "bytes",
                                                  static_cast<long long>(_state.size));
}

void WaveSteam::CreateResidentVoice()
{
    // Main thread: hand the decoded PCM to a mixer voice.
    if (_loaded || _loadError)
        return;
    if (_loadState.load(std::memory_order_acquire) != static_cast<int>(LoadState::UploadPending))
        return;

    WAVEFORMATEX fmt;
    _stream->GetFormat(fmt);

    auto pcm = std::make_shared<std::vector<uint8_t>>(std::move(_pendingPcm));
    _pendingPcm = {};
    if (pcm->empty())
    {
        _loadError = true;
        _loadState.store(static_cast<int>(LoadState::Failed), std::memory_order_release);
        ++_sys->_allocFailures;
        LOG_ERROR(Audio, "Buffer error: {} (no PCM)", static_cast<const char*>(Name()));
        return;
    }

    _voice = std::make_shared<MixVoice>();
    _voice->SetResident(std::move(pcm), ToVoiceFormat(fmt));

    Apply3D();
    ApplyVolume();
    ApplyPitch();
    _loaded = true;
    _loadState.store(static_cast<int>(LoadState::Resident), std::memory_order_release);

    LOG_DEBUG(Audio, "Loaded: {} ({}Hz {}ch {}s)", static_cast<const char*>(Name()), static_cast<int>(_state.frequency),
              _state.nChannel, GetLength());
}

void WaveSteam::OpenStreaming()
{
    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    if (!_stream || _loadError)
        return;

    _stream->GetFormat(_streamFormat);
    const int sampleStride = _streamFormat.nChannels * (_streamFormat.wBitsPerSample / 8);
    if (sampleStride <= 0 || _streamFormat.nSamplesPerSec <= 0)
    {
        _loadError = true;
        return;
    }
    const int bytesPerSec = static_cast<int>(_streamFormat.nSamplesPerSec) * sampleStride;
    int chunkBytes = (bytesPerSec * ::Poseidon::Audio::Steam::StreamingBuffers::kChunkMs) / 1000;
    chunkBytes -= (chunkBytes % sampleStride); // end chunks on a frame boundary
    if (chunkBytes <= 0)
        chunkBytes = sampleStride;

    // A decode latched before a Stop may still be running on the pump: wait
    // for it (it never takes the lock) and invalidate anything it produces.
    while (_streaming.decodeInFlight.load(std::memory_order_acquire))
        std::this_thread::yield();
    _streaming.generation.fetch_add(1, std::memory_order_acq_rel);
    _streaming.chunkBytes = chunkBytes;
    // Start at the requested position (e.g. Skip before the first Play).
    int64_t initialOffset = std::max(_state.curPosition, 0);
    initialOffset -= initialOffset % sampleStride;
    _streamBaseFrames = initialOffset / sampleStride;
    _streaming.decodeOffsetBytes.store(initialOffset, std::memory_order_release);
    _streaming.eofReached.store(false, std::memory_order_release);
    _streaming.aborted.store(false, std::memory_order_release);
    _streaming.queue.Clear();

    if (!_voice)
    {
        _voice = std::make_shared<MixVoice>();
        _voice->SetStreamed(&_streaming, ToVoiceFormat(_streamFormat));
    }
}

bool WaveSteam::PumpDrainUpload()
{
    return PumpDrainUpload(true);
}

bool WaveSteam::PumpDrainUpload(bool kickDecode)
{
    // Pump thread, under _sys->_audioMutex.  Decide whether another chunk is
    // needed and latch decodeInFlight so ~WaveSteam waits for it.
    if (!_isStreamed || !_voice || _loadError || !kickDecode)
        return false;
    if (_streaming.aborted.load(std::memory_order_acquire))
        return false;
    const bool needMore = !_streaming.eofReached.load(std::memory_order_acquire) &&
                          static_cast<int>(_streaming.queue.Size()) < ::Poseidon::Audio::Steam::StreamingBuffers::kNumBuffers;
    if (!needMore)
        return false;
    bool expected = false;
    return _streaming.decodeInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

void WaveSteam::DecodeQueuedChunk()
{
    const auto perfDecodeStart = ::Poseidon::Dev::Perf::Now();
    DecodeOneChunk();
    // Emit before releasing the flag: once it is clear the wave may be freed.
    ::Poseidon::Dev::Perf::EmitTraceEventAsset(Poseidon::Foundation::LogCategory::Audio, "WaveSteam::StreamChunk",
                                               perfDecodeStart, NameCStr());
    _streaming.decodeInFlight.store(false, std::memory_order_release);
}

void WaveSteam::DecodeOneChunk()
{
    // Decode one chunkBytes slice and queue it for the mixer.  Looping waves
    // wrap the decode offset at EOF (sample-accurate loop, no restart).
    if (_streaming.aborted.load(std::memory_order_acquire))
        return;
    if (!_stream || _loadError || _streaming.eofReached.load(std::memory_order_acquire))
        return;
    if (_streaming.chunkBytes <= 0)
        return;

    const uint32_t generation = _streaming.generation.load(std::memory_order_acquire);
    int64_t offset = _streaming.decodeOffsetBytes.load(std::memory_order_acquire);
    const int totalSize = static_cast<int>(_state.size);

    if (totalSize > 0 && offset >= totalSize)
    {
        if (_state.looping)
        {
            offset = 0;
            _streaming.decodeOffsetBytes.store(0, std::memory_order_release);
        }
        else
        {
            _streaming.eofReached.store(true, std::memory_order_release);
            return;
        }
    }

    int wantBytes = _streaming.chunkBytes;
    bool isLast = false;
    if (totalSize > 0 && offset + wantBytes >= totalSize)
    {
        wantBytes = static_cast<int>(totalSize - offset);
        if (!_state.looping)
            isLast = true;
    }

    ::Poseidon::Audio::Steam::DecodedChunk chunk;
    chunk.pcm.assign(static_cast<size_t>(wantBytes), 0);
    chunk.streamOffset = offset;
    const int gotBytes = _stream->GetData(chunk.pcm.data(), static_cast<int>(offset), wantBytes);
    if (gotBytes <= 0)
    {
        if (_state.looping)
            _streaming.decodeOffsetBytes.store(0, std::memory_order_release);
        else
            _streaming.eofReached.store(true, std::memory_order_release);
        return;
    }
    if (gotBytes < wantBytes && !_state.looping)
    {
        chunk.pcm.resize(static_cast<size_t>(gotBytes));
        isLast = true;
    }
    chunk.isLastChunk = isLast;
    chunk.generation = generation;

    // A DoStop may rewind concurrently (it bumps the generation, then resets
    // the offset and EOF flag); no lock is taken here because a wave can be
    // destroyed under the audio lock while it waits for this decode.  Each
    // update is undone if a rewind slipped in, and the mixer drops chunks of
    // an old generation.
    auto stale = [&] { return _streaming.generation.load(std::memory_order_acquire) != generation; };
    if (stale())
        return;
    int64_t expected = offset;
    if (!_streaming.decodeOffsetBytes.compare_exchange_strong(expected, offset + gotBytes, std::memory_order_acq_rel))
        return; // rewound meanwhile
    if (stale())
    {
        int64_t advanced = offset + gotBytes;
        _streaming.decodeOffsetBytes.compare_exchange_strong(advanced, 0, std::memory_order_acq_rel);
        return;
    }
    _streaming.queue.Push(std::move(chunk));
    if (isLast)
    {
        _streaming.eofReached.store(true, std::memory_order_release);
        if (stale())
            _streaming.eofReached.store(false, std::memory_order_release);
    }
}

void WaveSteam::KickAsyncLoad()
{
    // Large assets (>= 1 MB PCM, music-sized) decode on a TaskPool worker;
    // small ones decode inline so SFX / radio start without a frame delay.
    constexpr unsigned int kAsyncThresholdBytes = 1u * 1024u * 1024u;

    if (_loaded || _loadError)
        return;

    int expected = static_cast<int>(LoadState::NotStarted);
    if (!_loadState.compare_exchange_strong(expected, static_cast<int>(LoadState::DecodePending),
                                            std::memory_order_acq_rel))
        return;

    auto* pool = ::Poseidon::GetGlobalTaskPool();
    if (_state.size < kAsyncThresholdBytes || !pool)
    {
        DecodePcm();
        return;
    }

    AddRef(); // keep the wave alive across the worker decode
    pool->Submit(
        [this]()
        {
            DecodePcm();
            Release();
        });
}

void WaveSteam::Load()
{
    if (!_stream || _loaded || _loadError)
        return;
    DecodePcm();
    if (_loadError)
        return;
    CreateResidentVoice();
}

void WaveSteam::Unload()
{
    if (!_loaded)
        return;

    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    if (_isStreamed)
    {
        // Same ordering as ~WaveSteam: no decode can be latched while we hold
        // the lock, so this waits for at most the one already running.
        _streaming.aborted.store(true, std::memory_order_release);
        while (_streaming.decodeInFlight.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    if (_voice)
    {
        if (Mixer* mixer = _sys->GetMixer())
            mixer->Remove(_voice.get(), _isStreamed);
        _voice.reset();
    }
    if (_isStreamed)
    {
        _streaming.queue.Clear();
        _streaming.decodeOffsetBytes.store(0, std::memory_order_release);
        _streaming.eofReached.store(false, std::memory_order_release);
    }
    _loaded = false;
    _loadState.store(static_cast<int>(LoadState::NotStarted), std::memory_order_release);
}

void WaveSteam::ApplyVolume()
{
    if (!_voice)
        return;

    float gain;
    if (_previewMute)
    {
        // Audio screen ducked this wave; keep it playing silently.
        gain = 0.f;
    }
    else if (_is3D)
    {
        // DX8 path 3: volume lives in MinDistance; buffer gain is the adjust.
        gain = AudioMath::Gain3D(_volumeAdjust);
    }
    else if (_createdAs3D)
    {
        // DX8 path 2: 3D buffer heard as 2D (inside a vehicle) — 100x boost,
        // clamped at 0 dB.
        const float accom = _accomEnabled ? _accommodation : 1.f;
        gain = _volume * accom * _volumeAdjust * 100.f;
        if (gain > 1.f)
            gain = 1.f;
    }
    else
    {
        // DX8 path 1: pure 2D — vol * accom * adjust.
        gain = AudioMath::Gain2D(_volume, _accommodation, _accomEnabled, _volumeAdjust);
    }

    if (_playing && std::fabs(gain - _gainSet) > 1e-2f * gain)
    {
        LOG_DEBUG(Audio, "audio gain sound={} gain={:.4f} db={:.0f} 3d={} kind={} vol={:.3f} adj={:.3f}",
                  static_cast<const char*>(Name()), gain, gain > 0.f ? 2000.f * std::log10(gain) : -10000.f,
                  _is3D ? 1 : 0, static_cast<int>(_kind), _volume, _volumeAdjust);
    }
    const bool unmuffled = _unmuffled || _kind != WaveEffect;
    _voice->ModifyParams([gain, unmuffled](MixVoice::Params& p)
                         {
                             p.gain = gain;
                             p.unmuffled = unmuffled;
                         });
    _gainSet = gain;
}

void WaveSteam::Apply3D()
{
    if (!_voice)
        return;
    if (_is3D)
    {
        const float accom = _accomEnabled ? _accommodation : 1.f;
        const float refDist = DX8::referenceDistance(_volume, accom);
        const float maxDist = DX8::maxDistance(refDist);
        const Vector3 pos = _position;
        const Vector3 vel = _velocity;
        _voice->ModifyParams(
            [&](MixVoice::Params& p)
            {
                p.is3D = true;
                for (int b = 0; b < 3; b++)
                    p.occlusion[b] = _occlBands[b];
                p.pos[0] = pos[0];
                p.pos[1] = pos[1];
                p.pos[2] = pos[2];
                p.vel[0] = vel[0];
                p.vel[1] = vel[1];
                p.vel[2] = vel[2];
                p.refDist = refDist;
                p.maxDist = maxDist;
                p.rolloff = 1.f;
            });
    }
    else
    {
        _voice->ModifyParams(
            [](MixVoice::Params& p)
            {
                p.is3D = false;
                for (int i = 0; i < 3; i++)
                {
                    p.pos[i] = p.vel[i] = 0.f;
                    p.occlusion[i] = 1.f;
                }
            });
    }
    ApplyReverbSend();
}

void WaveSteam::ApplyReverbSend()
{
    // DX8: every hardware 3D buffer received the listener reverb.
    if (!_voice)
        return;
    const bool send = _is3D;
    _voice->ModifyParams([send](MixVoice::Params& p) { p.reverbSend = send; });
}

void WaveSteam::ApplyPitch()
{
    // DirectSound absolute-rate clamp [100, 100000] Hz against the native rate
    // (1.99 Wave8::SetFrequency) — keeps the deep low-RPM engine range.
    if (_voice && _state.frequency > 0 && _pitchRatio > 0.f)
    {
        const float pitch = DX8::dsoundPitchRatio(_pitchRatio, _state.frequency);
        _voice->ModifyParams([pitch](MixVoice::Params& p) { p.pitch = pitch; });
    }
}

void WaveSteam::Play()
{
    if (_queued)
        return; // waiting for a predecessor in the Queue() chain
    if (_state.terminated)
        return;
    if (_playing)
        return; // already playing — no-op (DX8)
    _wantPlaying = true;
    _state.wantPlaying = true;
    _everPlayed = true;
}

void WaveSteam::Queue(IWave* predecessorBase, int /*repeat*/)
{
    WaveSteam* predecessor = dynamic_cast<WaveSteam*>(predecessorBase);
    if (!predecessor || predecessor == this)
        return;
    // Walk to the chain tail.  If `this` is already in the chain just re-arm
    // the gate (also defeats crossed-Queue cycles).
    for (WaveSteam* node = predecessor; node; node = node->_queueNext.GetRef())
    {
        if (node == this)
        {
            _queued = true;
            return;
        }
        if (!node->_queueNext)
        {
            node->_queueNext = this;
            _queued = true;
            return;
        }
    }
}

void WaveSteam::DoPlay()
{
    std::unique_lock<std::recursive_mutex> lk(_sys->_audioMutex);
    _startDeferred = false;
    if (_state.terminated || _playing)
        return;
    if (_is3D && !_state.posValid)
        return; // 3D sounds need a position first
    if (_state.curPosition < 0)
        return; // delayed start
    if (_loadError)
        return;

    Mixer* mixer = _sys->GetMixer();
    if (!mixer)
        return;

    if (!_loaded)
    {
        if (_isStreamed)
        {
            OpenStreaming();
            if (_loadError || !_voice)
                return;
            _loaded = true;
            _loadState.store(static_cast<int>(LoadState::Resident), std::memory_order_release);
            // Speech starts mid-frame: decode the first chunk inline so the
            // line isn't clipped by a pump tick.
            if (_kind == WaveSpeech && _streaming.queue.Size() == 0 &&
                !_streaming.decodeInFlight.load(std::memory_order_acquire))
            {
                _streaming.decodeInFlight.store(true, std::memory_order_release);
                lk.unlock();
                DecodeOneChunk();
                lk.lock();
                _streaming.decodeInFlight.store(false, std::memory_order_release);
            }
        }
        else
        {
            KickAsyncLoad();
            if (GetLoadState() == LoadState::UploadPending)
                CreateResidentVoice();
            if (!_loaded)
            {
                _startDeferred = true; // still decoding on a worker; Commit retries
                return;
            }
        }
    }

    if (!_voice)
        return;

    if (_isStreamed)
    {
        Apply3D();
        ApplyVolume();
        ApplyPitch();
        _voice->SetLooping(false); // the decoder loops streamed sources
        if (_resumeInPlace)
        {
            _voice->SetPaused(false);
        }
        else
        {
            if (_pendingSeekSeconds > 0.f)
            {
                // Start mid-stream (device preview carries its offset across
                // a device switch): restart decoding at the frame boundary.
                const int block = _streamFormat.nBlockAlign;
                const float len = GetLength();
                const float seek = (len > 0.f) ? std::fmod(_pendingSeekSeconds, len) : 0.f;
                int64_t bytes = static_cast<int64_t>(seek * static_cast<float>(_streamFormat.nAvgBytesPerSec));
                if (block > 0)
                    bytes -= bytes % block;
                _streaming.generation.fetch_add(1, std::memory_order_acq_rel);
                _streaming.queue.Clear();
                _streaming.decodeOffsetBytes.store(bytes, std::memory_order_release);
                _streaming.eofReached.store(false, std::memory_order_release);
                if (block > 0)
                    _streamBaseFrames = bytes / block;
                _pendingSeekSeconds = 0.f;
            }
            _voice->Rewind(0);
            _voice->SetPaused(false);
            mixer->Add(_voice);
        }
        _resumeInPlace = false;
        _paused = false;
        _playing = true;
        _state.playing = true;
        _everPlayed = true;
        _state.terminated = false;
        Audio::SoundTestLog::NotePlay(static_cast<const char*>(Name()));
        OnPlayOnce();
        return;
    }

    // Termination / wrap of the engine-side cursor.
    if (_state.curPosition >= static_cast<int>(_state.size) && _state.size > 0)
    {
        if (WaveLogic::ShouldTerminate(_state.curPosition, _state.size, _state.looping))
        {
            _state.terminated = true;
            _state.curPosition = _state.size;
            OnTerminateOnce();
            return;
        }
        _state.curPosition = WaveLogic::WrapPosition(_state.curPosition, _state.size, _state.looping);
    }

    Apply3D(); // DX8: DoSetPosition in DoPlay
    ApplyVolume();
    ApplyPitch(); // start at the right pitch (no startup click)
    _voice->SetLooping(_state.looping);

    // The engine owns the resume point (DX8 SetCurrentPosition before Play).
    int64_t startFrame = 0;
    const int fb = FrameBytes();
    if (_state.curPosition > 0 && fb > 0)
        startFrame = _state.curPosition / fb;
    if (_pendingSeekSeconds > 0.f)
    {
        const float len = GetLength();
        const float seek = (len > 0.f) ? std::fmod(_pendingSeekSeconds, len) : 0.f;
        if (seek > 0.f && _state.frequency > 0)
            startFrame = static_cast<int64_t>(seek * static_cast<float>(_state.frequency));
        _pendingSeekSeconds = 0.f;
    }
    // A paused voice is still registered: unregister it (waiting out a render
    // in flight) before rewinding its cursor.
    mixer->Remove(_voice.get(), true);
    _resumeInPlace = false;
    _voice->Rewind(startFrame);
    _voice->SetPaused(false);
    mixer->Add(_voice);

    _paused = false;
    _playing = true;
    _state.playing = true;
    _state.terminated = false;

    LOG_DEBUG(Audio, "Play: {} (vol={} adj={} {})", static_cast<const char*>(Name()), _volume, _volumeAdjust,
              _state.looping ? "loop" : "once");
    OnPlayOnce();
}

void WaveSteam::DoStop()
{
    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    _wantPlaying = false;
    _state.wantPlaying = false;
    // DoStop always lands StoppedReplayable — clear _paused before the early-out.
    // A resumed-but-not-yet-committed voice is still registered too.
    const bool wasPaused = _paused || _resumeInPlace;
    _paused = false;
    _resumeInPlace = false;

    if (!_playing && !wasPaused)
        return;

    if (_voice)
    {
        if (!_isStreamed)
            StoreCurrentOffset(); // DX8 Wave8::DoStop: StoreCurrentPosition before Stop
        if (Mixer* mixer = _sys->GetMixer())
            mixer->Remove(_voice.get(), _isStreamed);
        if (_isStreamed)
        {
            // Restart from the top on the next Play; a decode still in
            // flight sees the new generation and drops its chunk.
            _streaming.generation.fetch_add(1, std::memory_order_acq_rel);
            _streaming.queue.Clear();
            _streaming.decodeOffsetBytes.store(0, std::memory_order_release);
            _streaming.eofReached.store(false, std::memory_order_release);
        }
    }

    _playing = false;
    _state.playing = false;
}

void WaveSteam::Stop()
{
    DoStop();
    Unload();
}

void WaveSteam::Resume()
{
    // A-01: Paused -> WaitingDeferred -> Playing on the next Commit.  _paused
    // clears now; _resumeInPlace tells DoPlay the voice keeps its position.
    if (_state.terminated || _loadError || _playing)
        return;
    if (_paused)
    {
        _paused = false;
        _resumeInPlace = true;
    }
    _wantPlaying = true;
    _state.wantPlaying = true;
}

void WaveSteam::Pause()
{
    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    if (!_playing)
        return;

    if (_voice)
    {
        // The voice keeps its cursor while paused; the engine-side offset is
        // still captured so Stop/Play resumes from the same point (DX8).
        if (!_isStreamed)
            StoreCurrentOffset();
        _voice->SetPaused(true);
    }

    _playing = false;
    _paused = true;
    _wantPlaying = false;
    _state.wantPlaying = false;
    _state.playing = false;
}

void WaveSteam::StoreCurrentOffset()
{
    if (!_voice || _isStreamed)
        return;
    const int fb = FrameBytes();
    if (fb <= 0)
        return;
    int64_t frame = _voice->PositionFrames();
    if (_state.size > 0)
    {
        const int64_t total = static_cast<int64_t>(_state.size) / fb;
        if (total > 0)
            frame = _state.looping ? frame % total : std::min(frame, total);
    }
    _state.curPosition = static_cast<int>(frame * fb);
}

float WaveSteam::GetCurrentOffsetSeconds() const
{
    std::lock_guard<std::recursive_mutex> lk(_sys->_audioMutex);
    if (_voice)
    {
        const int rate = _isStreamed ? static_cast<int>(_streamFormat.nSamplesPerSec) : static_cast<int>(_state.frequency);
        const int64_t base = _isStreamed ? _streamBaseFrames : 0;
        if (rate > 0 && (_playing || _paused || _resumeInPlace))
            return static_cast<float>(base + _voice->PositionFrames()) / static_cast<float>(rate);
        if (rate > 0 && _isStreamed && _loaded)
            return static_cast<float>(base) / static_cast<float>(rate);
    }
    if (_state.frequency <= 0 || _state.nChannel <= 0 || _state.sSize <= 0)
        return 0.f;
    const int bytesPerSec = static_cast<int>(_state.frequency) * _state.nChannel * _state.sSize;
    return bytesPerSec > 0 ? static_cast<float>(_state.curPosition) / static_cast<float>(bytesPerSec) : 0.f;
}

void WaveSteam::Repeat(int repeat)
{
    _state.looping = repeat >= 2;
    // Streamed sources loop in the decoder; the voice never loops them.
    if (_voice && !_isStreamed)
        _voice->SetLooping(_state.looping);
}

void WaveSteam::LastLoop()
{
    if (!_state.looping)
        return;
    _state.looping = false;
    if (_voice && !_isStreamed)
        _voice->SetLooping(false);
}

void WaveSteam::PlayUntilStopValue(float time)
{
    _stopThreshold = time;
    _state.looping = true;
    if (_voice && !_isStreamed)
        _voice->SetLooping(true);
}

void WaveSteam::SetStopValue(float time)
{
    if (time > _stopThreshold)
    {
        // normal end of a looped burst (machine guns): not a warning
        LOG_DEBUG(Audio, "SetStopValue terminate: {} (time={} > threshold={})", static_cast<const char*>(Name()), time,
                 _stopThreshold);
        _state.terminated = true;
        Stop();
    }
}

void WaveSteam::Skip(float deltaT)
{
    if (_queued)
        return; // waiting for a predecessor — don't advance the virtual cursor
    if (_state.frequency <= 0 || _state.sSize <= 0)
        return;

    const int skip = WaveLogic::TimeToPosition(deltaT, _state.frequency, _state.sSize * std::max(_state.nChannel, 1));
    _state.curPosition += skip;

    // Delay/pause wave: no PCM, only a countdown to 0 (1.99 Wave8::Skip).
    if (_state.size == 0 && _state.curPosition >= 0)
    {
        Stop();
        _state.terminated = true;
        _state.curPosition = 0;
        return;
    }

    while (_state.curPosition >= static_cast<int>(_state.size))
    {
        if (WaveLogic::ShouldTerminate(_state.curPosition, _state.size, _state.looping))
        {
            Stop();
            _state.terminated = true;
            _state.curPosition = _state.size;
            return;
        }
        _state.curPosition = _state.curPosition - _state.size;
    }
}

void WaveSteam::Advance(float deltaT)
{
    // 1.99 WaveGlobal8::Advance: a queued start at/after 0 must not drift (it
    // would start past the attack); a negative cursor is a pre-start delay.
    if (_playing || (_wantPlaying && _state.curPosition >= 0))
        return;
    Skip(deltaT);
}

float WaveSteam::GetLength() const
{
    if (_state.frequency <= 0 || _state.nChannel <= 0 || _state.sSize <= 0)
        return 0.f;
    const int bytesPerSec = static_cast<int>(_state.frequency) * _state.nChannel * _state.sSize;
    return bytesPerSec > 0 ? static_cast<float>(_state.size) / static_cast<float>(bytesPerSec) : 0.f;
}

bool WaveSteam::IsStopped()
{
    if (_wantPlaying)
        return false; // deferred play pending

    // Natural end of playback.  A streamed voice in an underrun keeps playing
    // silence and is never Finished(), so only a genuine end lands here.
    if (_voice && _playing && _voice->Finished())
    {
        _playing = false;
        _state.playing = false;
        _state.terminated = true;
        if (Mixer* mixer = _sys->GetMixer())
            mixer->Remove(_voice.get());
        OnTerminateOnce();
    }
    return !_playing;
}

bool WaveSteam::IsTerminated()
{
    if (_wantPlaying)
        return false; // about to play

    if (!_state.terminated && _voice && _playing && _voice->Finished())
    {
        _playing = false;
        _state.playing = false;
        _state.terminated = true;
        if (Mixer* mixer = _sys->GetMixer())
            mixer->Remove(_voice.get());
        OnTerminateOnce();
    }

    if (_loadError)
        _state.terminated = true;

    if (!_state.terminated)
        return false;

    // Activate the next wave in the chain (1.99 WaveGlobal8::AdvanceQueue).
    if (_queueNext && _queueNext->_queued)
    {
        _queueNext->_queued = false;
        _queueNext->Play();
    }

    // The chain is only terminated once every successor has finished.
    if (_queueNext)
        return _queueNext->IsTerminated();

    return true;
}

bool WaveSteam::IsWaiting()
{
    return WaveLogic::IsWaiting(_state.curPosition);
}

void WaveSteam::Restart()
{
    Stop();
    _state.terminated = false;
    _state.curPosition = 0;
    _everPlayed = true;
    _pendingSeekSeconds = 0.f;
}

WaveState WaveSteam::State()
{
    // Authoritative FSM query (audio-invariants A-01).
    if (_state.terminated || _loadError)
        return WaveState::Terminal;
    if (_paused)
        return WaveState::Paused;
    if (_playing)
        return WaveState::Playing;
    if (_wantPlaying)
        return WaveState::WaitingDeferred;
    return _everPlayed ? WaveState::StoppedReplayable : WaveState::Created;
}

float WaveSteam::AppliedGainForTest() const
{
    return _voice ? _voice->GetParams().gain : 1.0f;
}

float WaveSteam::AppliedPitchForTest() const
{
    return _voice ? _voice->GetParams().pitch : 1.0f;
}

void WaveSteam::SetVolume(float v, float freq, bool /*immediate*/)
{
    _volume = v;
    if (freq > 0.f)
        _pitchRatio = freq; // re-applied on (re)start
    ApplyPitch();
    ApplyVolume();
    if (_is3D)
        Apply3D(); // reference distance follows the volume
}

void WaveSteam::SetVolumeAdjust(float volEffect, float volSpeech, float volMusic)
{
    float vol = volEffect;
    if (_kind == WaveMusic)
        vol = volMusic;
    else if (_kind == WaveSpeech)
        vol = volSpeech;
    _volumeAdjust = vol;
    ApplyVolume();
}

void WaveSteam::SetKind(WaveKind k)
{
    _kind = k;
    if (_sys)
    {
        float vol = _sys->_volumeAdjustEffect;
        if (_kind == WaveMusic)
            vol = _sys->_volumeAdjustMusic;
        else if (_kind == WaveSpeech)
            vol = _sys->_volumeAdjustSpeech;
        _volumeAdjust = vol;
    }
    ApplyVolume();
}

void WaveSteam::SetPosition(Vector3Par pos, Vector3Par vel, bool /*immediate*/)
{
    _position = pos;
    _velocity = vel;
    _state.posValid = true;
    Apply3D();
}

} // namespace Poseidon
