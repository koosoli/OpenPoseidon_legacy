#include <PoseidonSteamAudio/Mixer.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Poseidon
{
namespace SteamAudio
{
namespace
{
Mixer* s_instance = nullptr;

// Engine space is left-handed; Steam Audio is right-handed.
IPLVector3 ToIPL(const float v[3])
{
    return IPLVector3{-v[0], v[1], v[2]};
}

float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void SteamLog(IPLLogLevel level, const char* message)
{
    if (level == IPL_LOGLEVEL_ERROR)
        LOG_ERROR(Audio, "SteamAudio: {}", message);
    else if (level == IPL_LOGLEVEL_WARNING)
        LOG_WARN(Audio, "SteamAudio: {}", message);
    else
        LOG_DEBUG(Audio, "SteamAudio: {}", message);
}

void FreeBuffer(IPLContext ctx, IPLAudioBuffer& buf)
{
    if (ctx && buf.data)
        iplAudioBufferFree(ctx, &buf);
    buf = IPLAudioBuffer{};
}

// Inverse-distance-clamped model, reference distance = DX8 MinDistance:
// gain = ref / (ref + rolloff * (clamp(d) - ref)).
float InverseDistanceClamped(float dist, float ref, float maxDist, float rolloff)
{
    if (ref <= 0.f)
        return 1.f;
    const float d = std::clamp(dist, ref, std::max(ref, maxDist));
    const float denom = ref + rolloff * (d - ref);
    return denom > 0.f ? ref / denom : 1.f;
}

// Listener-space direction of `src` (Steam Audio axes: +x right, +y up,
// -z ahead) — the fallback for iplCalculateRelativeDirection when Steam Audio
// is unavailable.
IPLVector3 RelativeDirection(const IPLVector3& src, const IPLVector3& lis, const IPLVector3& ahead,
                             const IPLVector3& up)
{
    float d[3] = {src.x - lis.x, src.y - lis.y, src.z - lis.z};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6f)
        return IPLVector3{0.f, 0.f, -1.f};
    for (float& c : d)
        c /= len;
    // right = ahead x up
    const float right[3] = {ahead.y * up.z - ahead.z * up.y, ahead.z * up.x - ahead.x * up.z,
                            ahead.x * up.y - ahead.y * up.x};
    const float a[3] = {ahead.x, ahead.y, ahead.z};
    const float u[3] = {up.x, up.y, up.z};
    return IPLVector3{Dot(d, right), Dot(d, u), -Dot(d, a)};
}

// Constant-power stereo law on the horizontal direction (listener space, Steam
// Audio axes): p = -1 hard left .. +1 hard right, L = cos, R = sin of
// (p + 1) * pi / 4.  A source straight above/below pans to the centre.
void StereoPanGains(const IPLVector3& dir, float out[2])
{
    const float len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    const float p = len > 1e-6f ? std::clamp(dir.x / len, -1.f, 1.f) : 0.f;
    const float q = (p + 1.f) * 0.78539816f;
    out[0] = std::cos(q);
    out[1] = std::sin(q);
}
} // namespace

// ===========================================================================
// MixVoice
// ===========================================================================

MixVoice::~MixVoice()
{
    ReleaseEffects();
}

void MixVoice::ReleaseEffects()
{
    if (_direct)
        iplDirectEffectRelease(&_direct);
    if (_binaural)
        iplBinauralEffectRelease(&_binaural);
    FreeBuffer(_ctx, _mono);
    FreeBuffer(_ctx, _monoOut);
    FreeBuffer(_ctx, _stereo);
    _havePan = false;
    if (_ctx)
        iplContextRelease(&_ctx);
}

void MixVoice::SetResident(std::shared_ptr<const std::vector<uint8_t>> pcm, const VoiceFormat& fmt)
{
    _kind = SourceKind::Resident;
    _fmt = fmt;
    _pcm = std::move(pcm);
    const int fb = _fmt.FrameBytes();
    _totalFrames = (_pcm && fb > 0) ? static_cast<int64_t>(_pcm->size() / fb) : 0;
    Rewind(0);
}

void MixVoice::SetStreamed(Audio::Steam::StreamingBuffers* stream, const VoiceFormat& fmt)
{
    _kind = SourceKind::Streamed;
    _fmt = fmt;
    _stream = stream;
    Rewind(0);
}

void MixVoice::SetExternal(int sampleRate)
{
    _kind = SourceKind::External;
    _fmt.channels = 1;
    _fmt.bitsPerSample = 16;
    _fmt.sampleRate = sampleRate;
    Rewind(0);
}

void MixVoice::SetParams(const Params& p)
{
    std::lock_guard<std::mutex> lk(_paramMutex);
    _params = p;
}

MixVoice::Params MixVoice::GetParams() const
{
    std::lock_guard<std::mutex> lk(_paramMutex);
    return _params;
}

void MixVoice::Rewind(int64_t frame)
{
    if (frame < 0)
        frame = 0;
    if (_kind == SourceKind::Resident && _totalFrames > 0 && frame > _totalFrames)
        frame = _totalFrames;
    _cursor = frame;
    _consumed.store(frame, std::memory_order_release);
    _finished.store(false, std::memory_order_release);
    _chunk = Audio::Steam::DecodedChunk{};
    _chunkPos = 0;
    _blockFrames = 0;
    _blockPos = 0;
    _primed = false;
    _sourceEnded = false;
    _frac = 0.0;
    _lastGain = -1.f;
    _lastWet = 0.f;
}

void MixVoice::ExternalPush(const int16_t* samples, int count)
{
    std::lock_guard<std::mutex> lk(_ringMutex);
    _ring.insert(_ring.end(), samples, samples + count);
    // Cap at ~2 s so a stalled consumer can't grow without bound.
    const size_t cap = static_cast<size_t>(_fmt.sampleRate) * 2;
    if (_ring.size() > cap)
        _ring.erase(_ring.begin(), _ring.begin() + static_cast<std::ptrdiff_t>(_ring.size() - cap));
}

int MixVoice::ExternalQueued() const
{
    std::lock_guard<std::mutex> lk(_ringMutex);
    return static_cast<int>(_ring.size());
}

void MixVoice::ExternalClear()
{
    std::lock_guard<std::mutex> lk(_ringMutex);
    _ring.clear();
}

MixVoice::FetchStatus MixVoice::FillBlock()
{
    constexpr int kBlock = 256;
    const int ch = std::max(1, _fmt.channels);
    if (static_cast<int>(_block.size()) < kBlock * ch)
        _block.resize(static_cast<size_t>(kBlock * ch));
    _blockFrames = 0;
    _blockPos = 0;

    auto convert = [&](const uint8_t* src, int frames)
    {
        float* dst = _block.data();
        if (_fmt.bitsPerSample == 8)
        {
            for (int i = 0; i < frames * ch; i++)
                dst[i] = (static_cast<int>(src[i]) - 128) * (1.f / 128.f);
        }
        else
        {
            const int16_t* s = reinterpret_cast<const int16_t*>(src);
            for (int i = 0; i < frames * ch; i++)
                dst[i] = s[i] * (1.f / 32768.f);
        }
    };

    switch (_kind)
    {
        case SourceKind::Resident:
        {
            if (!_pcm || _totalFrames <= 0)
                return FetchStatus::End;
            if (_cursor >= _totalFrames)
            {
                if (!Looping())
                    return FetchStatus::End;
                _cursor = 0;
                _consumed.store(0, std::memory_order_release);
            }
            const int frames = static_cast<int>(std::min<int64_t>(kBlock, _totalFrames - _cursor));
            convert(_pcm->data() + _cursor * _fmt.FrameBytes(), frames);
            _cursor += frames;
            _blockFrames = frames;
            return FetchStatus::Ok;
        }
        case SourceKind::Streamed:
        {
            if (!_stream)
                return FetchStatus::End;
            const int fb = _fmt.FrameBytes();
            while (_chunkPos >= _chunk.pcm.size())
            {
                Audio::Steam::DecodedChunk next;
                if (!_stream->queue.TryPop(next))
                {
                    const bool ended = _stream->eofReached.load(std::memory_order_acquire) &&
                                       !_stream->decodeInFlight.load(std::memory_order_acquire) &&
                                       _stream->queue.Size() == 0;
                    return ended ? FetchStatus::End : FetchStatus::Underrun;
                }
                if (next.generation != _stream->generation.load(std::memory_order_acquire))
                    continue; // decoded before the last Stop
                _chunk = std::move(next);
                _chunkPos = 0;
            }
            const int avail = static_cast<int>((_chunk.pcm.size() - _chunkPos) / fb);
            const int frames = std::min(kBlock, avail);
            if (frames <= 0)
            {
                _chunkPos = _chunk.pcm.size();
                return FetchStatus::Underrun;
            }
            convert(_chunk.pcm.data() + _chunkPos, frames);
            _chunkPos += static_cast<size_t>(frames) * fb;
            _blockFrames = frames;
            return FetchStatus::Ok;
        }
        case SourceKind::External:
        {
            std::lock_guard<std::mutex> lk(_ringMutex);
            const int frames = std::min<int>(kBlock, static_cast<int>(_ring.size()));
            if (frames <= 0)
                return FetchStatus::Underrun;
            for (int i = 0; i < frames; i++)
            {
                _block[i] = _ring.front() * (1.f / 32768.f);
                _ring.pop_front();
            }
            _blockFrames = frames;
            return FetchStatus::Ok;
        }
        case SourceKind::None:
            break;
    }
    return FetchStatus::End;
}

MixVoice::FetchStatus MixVoice::NextFrame(float* frame)
{
    if (_blockPos >= _blockFrames)
    {
        const FetchStatus st = FillBlock();
        if (st != FetchStatus::Ok)
            return st;
    }
    const int ch = std::max(1, _fmt.channels);
    const float* src = _block.data() + _blockPos * ch;
    frame[0] = src[0];
    frame[1] = ch > 1 ? src[1] : src[0];
    ++_blockPos;
    _consumed.fetch_add(1, std::memory_order_acq_rel);
    return FetchStatus::Ok;
}

int MixVoice::Resample(float* out, int n, double step)
{
    const int ch = std::max(1, std::min(2, _fmt.channels));
    int produced = 0;

    if (!_primed)
    {
        float f[2];
        const FetchStatus st = NextFrame(f);
        if (st == FetchStatus::End)
        {
            _finished.store(true, std::memory_order_release);
            std::fill(out, out + n * ch, 0.f);
            return 0;
        }
        if (st == FetchStatus::Underrun)
        {
            std::fill(out, out + n * ch, 0.f);
            return 0;
        }
        _prev[0] = _cur[0] = f[0];
        _prev[1] = _cur[1] = f[1];
        _frac = 0.0;
        _primed = true;
    }

    for (int i = 0; i < n; i++)
    {
        while (_frac >= 1.0)
        {
            if (_sourceEnded)
            {
                // Let the final sample ring out to silence, then finish.
                _prev[0] = _cur[0];
                _prev[1] = _cur[1];
                _cur[0] = _cur[1] = 0.f;
                _frac -= 1.0;
                _finished.store(true, std::memory_order_release);
                continue;
            }
            float f[2];
            const FetchStatus st = NextFrame(f);
            if (st == FetchStatus::Underrun)
            {
                // Hold position; output silence until data arrives.
                std::fill(out + i * ch, out + n * ch, 0.f);
                return produced;
            }
            _prev[0] = _cur[0];
            _prev[1] = _cur[1];
            if (st == FetchStatus::End)
            {
                _sourceEnded = true;
                _cur[0] = _cur[1] = 0.f;
            }
            else
            {
                _cur[0] = f[0];
                _cur[1] = f[1];
            }
            _frac -= 1.0;
        }
        if (_finished.load(std::memory_order_relaxed))
        {
            std::fill(out + i * ch, out + n * ch, 0.f);
            return produced;
        }
        const float t = static_cast<float>(_frac);
        for (int c = 0; c < ch; c++)
            out[i * ch + c] = _prev[c] + (_cur[c] - _prev[c]) * t;
        ++produced;
        _frac += step;
    }
    return produced;
}

bool MixVoice::EnsureEffects(IPLContext ctx, IPLHRTF hrtf, IPLAudioSettings& settings)
{
    if (_direct)
        return true;
    if (!ctx)
        return false;
    _ctx = iplContextRetain(ctx);

    IPLDirectEffectSettings ds{1};
    IPLBinauralEffectSettings bs{hrtf};
    if (iplDirectEffectCreate(_ctx, &settings, &ds, &_direct) != IPL_STATUS_SUCCESS ||
        (hrtf && iplBinauralEffectCreate(_ctx, &settings, &bs, &_binaural) != IPL_STATUS_SUCCESS) ||
        iplAudioBufferAllocate(_ctx, 1, settings.frameSize, &_mono) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(_ctx, 1, settings.frameSize, &_monoOut) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(_ctx, 2, settings.frameSize, &_stereo) != IPL_STATUS_SUCCESS)
    {
        ReleaseEffects();
        return false;
    }
    return true;
}

// ===========================================================================
// Mixer
// ===========================================================================

Mixer* Mixer::Instance()
{
    return s_instance;
}

Mixer::Mixer()
{
    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = &SteamLog;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2; // Steam Audio picks the best the CPU supports, up to this
    if (iplContextCreate(&cs, &_ctx) != IPL_STATUS_SUCCESS)
    {
        LOG_ERROR(Audio, "SteamAudio: iplContextCreate failed");
        _ctx = nullptr;
    }
    else
    {
        IPLHRTFSettings hs{};
        hs.type = IPL_HRTFTYPE_DEFAULT;
        hs.volume = 1.f;
        hs.normType = IPL_HRTFNORMTYPE_NONE;
        if (iplHRTFCreate(_ctx, &_settings, &hs, &_hrtf) != IPL_STATUS_SUCCESS)
        {
            LOG_WARN(Audio, "SteamAudio: default HRTF unavailable — headphone mode falls back to panning");
            _hrtf = nullptr;
        }
    }

    _out.assign(kFrameSize * 2, 0.f);
    _mixL.assign(kFrameSize, 0.f);
    _mixR.assign(kFrameSize, 0.f);
    _send.assign(kFrameSize, 0.f);
    _tmp.assign(kFrameSize * 2, 0.f);
    _monoScratch.assign(kFrameSize, 0.f);
    s_instance = this;
}

Mixer::~Mixer()
{
    Close();
    {
        std::lock_guard<std::mutex> lk(_voiceMutex);
        for (auto& v : _voices)
            v->_registered.store(false, std::memory_order_release);
        _voices.clear();
    }
    _snapshot.clear();
    if (_hrtf)
        iplHRTFRelease(&_hrtf);
    if (_ctx)
        iplContextRelease(&_ctx);
    if (s_instance == this)
        s_instance = nullptr;
}

std::vector<std::string> Mixer::ListDevices()
{
    std::vector<std::string> result;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    if (!ids)
        return result;
    for (int i = 0; i < count; i++)
    {
        if (const char* name = SDL_GetAudioDeviceName(ids[i]))
            result.emplace_back(name);
    }
    SDL_free(ids);
    return result;
}

bool Mixer::Open(const std::string& deviceName)
{
    Close();

    SDL_AudioDeviceID devid = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!deviceName.empty())
    {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        bool found = false;
        for (int i = 0; ids && i < count; i++)
        {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && deviceName == name)
            {
                devid = ids[i];
                found = true;
                break;
            }
        }
        SDL_free(ids);
        if (!found)
        {
            LOG_WARN(Audio, "SteamAudio: output device '{}' not found", deviceName);
            return false;
        }
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = kSampleRate;
    _stream = SDL_OpenAudioDeviceStream(devid, &spec, &Mixer::Callback, this);
    if (!_stream)
    {
        LOG_ERROR(Audio, "SteamAudio: SDL_OpenAudioDeviceStream failed: {}", SDL_GetError());
        return false;
    }
    _deviceName = deviceName;
    SDL_ResumeAudioStreamDevice(_stream);
    return true;
}

void Mixer::Close()
{
    if (_stream)
    {
        SDL_DestroyAudioStream(_stream); // stops the callback before returning
        _stream = nullptr;
    }
}

void Mixer::SetDevicePaused(bool paused)
{
    if (!_stream)
        return;
    if (paused)
        SDL_PauseAudioStreamDevice(_stream);
    else
        SDL_ResumeAudioStreamDevice(_stream);
}

void Mixer::Add(const std::shared_ptr<MixVoice>& voice)
{
    if (!voice)
        return;
    std::lock_guard<std::mutex> lk(_voiceMutex);
    if (voice->_registered.exchange(true, std::memory_order_acq_rel))
        return;
    _voices.push_back(voice);
}

void Mixer::Remove(MixVoice* voice, bool waitForRender)
{
    if (!voice)
        return;
    {
        std::lock_guard<std::mutex> lk(_voiceMutex);
        voice->_registered.store(false, std::memory_order_release);
        _voices.erase(
            std::remove_if(_voices.begin(), _voices.end(), [voice](const auto& v) { return v.get() == voice; }),
            _voices.end());
    }
    if (waitForRender)
        std::lock_guard<std::mutex> wait(_renderMutex);
}

int Mixer::ActiveVoices() const
{
    std::lock_guard<std::mutex> lk(_voiceMutex);
    return static_cast<int>(_voices.size());
}

void Mixer::SetListener(const float pos[3], const float vel[3], const float dir[3], const float up[3])
{
    std::lock_guard<std::mutex> lk(_listenerMutex);
    for (int i = 0; i < 3; i++)
    {
        _listener.pos[i] = pos[i];
        _listener.vel[i] = vel[i];
        _listener.dir[i] = dir[i];
        _listener.up[i] = up[i];
    }
}

void Mixer::SetReverb(const EFXEAXREVERBPROPERTIES* props)
{
    std::lock_guard<std::mutex> lk(_reverbMutex);
    if (props)
    {
        const bool wasOn = _reverbOn.load(std::memory_order_acquire);
        _reverb.Configure(*props);
        if (!wasOn)
            _reverb.Reset();
        _reverbOn.store(true, std::memory_order_release);
    }
    else
    {
        _reverbOn.store(false, std::memory_order_release);
    }
}

void Mixer::Callback(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto* self = static_cast<Mixer*>(userdata);
    constexpr int kChunkFrames = 256;
    float buf[kChunkFrames * 2];
    int framesNeeded = additional / static_cast<int>(sizeof(float) * 2);
    while (framesNeeded > 0)
    {
        const int n = std::min(framesNeeded, kChunkFrames);
        self->Render(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * static_cast<int>(sizeof(float) * 2));
        framesNeeded -= n;
    }
}

void Mixer::Render(float* interleaved, int frames)
{
    int done = 0;
    while (done < frames)
    {
        if (_outPos >= kFrameSize)
        {
            RenderFrame();
            _outPos = 0;
        }
        const int n = std::min(frames - done, kFrameSize - _outPos);
        std::memcpy(interleaved + done * 2, _out.data() + _outPos * 2, sizeof(float) * 2 * n);
        _outPos += n;
        done += n;
    }
}

void Mixer::RenderFrame()
{
    std::fill(_mixL.begin(), _mixL.end(), 0.f);
    std::fill(_mixR.begin(), _mixR.end(), 0.f);
    std::fill(_send.begin(), _send.end(), 0.f);

    {
        std::lock_guard<std::mutex> render(_renderMutex);
        {
            std::lock_guard<std::mutex> lk(_voiceMutex);
            _snapshot.assign(_voices.begin(), _voices.end());
        }
        for (auto& v : _snapshot)
        {
            // A voice removed after the snapshot was taken is skipped here, and
            // Remove(waitForRender) blocks on _renderMutex until we're done.
            if (!v->_registered.load(std::memory_order_acquire) || v->Paused() || v->Finished())
                continue;
            MixVoiceFrame(*v, _send.data());
        }
        _snapshot.clear(); // drop refs on the audio thread promptly
    }

    if (_reverbOn.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lk(_reverbMutex);
        _reverb.Process(_send.data(), _mixL.data(), _mixR.data(), kFrameSize);
    }

    const float master = _masterGain.load(std::memory_order_acquire);
    for (int i = 0; i < kFrameSize; i++)
    {
        // Gentle soft clip so stacked explosions saturate instead of wrapping.
        auto clip = [](float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); };
        _out[i * 2 + 0] = clip(_mixL[i] * master);
        _out[i * 2 + 1] = clip(_mixR[i] * master);
    }
}

// Closer than this to the listener a 3D source has no usable direction (see MixVoiceFrame).
constexpr float kAtListener = 1.0f;

void Mixer::MixVoiceFrame(MixVoice& v, float* sendBuf)
{
    const MixVoice::Params p = v.GetParams();
    const int ch = std::max(1, std::min(2, v._fmt.channels));

    Listener lis;
    {
        std::lock_guard<std::mutex> lk(_listenerMutex);
        lis = _listener;
    }

    // Distance + Doppler (3D only).
    float dist = 0.f;
    double pitch = p.pitch > 0.f ? p.pitch : 1.f;
    if (p.is3D)
    {
        float toListener[3] = {lis.pos[0] - p.pos[0], lis.pos[1] - p.pos[1], lis.pos[2] - p.pos[2]};
        dist = std::sqrt(Dot(toListener, toListener));
        if (dist >= kAtListener)
        {
            // Doppler (factor 1): f' = f (c - vLS) / (c - vSS), velocities
            // projected on the source->listener axis, clamped below c.
            const float inv = 1.f / dist;
            const float vls = std::min(Dot(lis.vel, toListener) * inv, kSpeedOfSound * 0.95f);
            const float vss = std::min(Dot(p.vel, toListener) * inv, kSpeedOfSound * 0.95f);
            const double doppler = (kSpeedOfSound - vls) / (kSpeedOfSound - vss);
            pitch *= std::clamp(doppler, 0.5, 2.0);
        }
    }
    const double step = pitch * static_cast<double>(v._fmt.sampleRate) / kSampleRate;

    float* src = _tmp.data();
    v.Resample(src, kFrameSize, step);

    // A source at the listener -- Arma's `_camera say`, a line spoken by the camera itself -- has no
    // direction. HRTF and panning then flip between the ears from block to block (the owner heard
    // the outside lines of the Queen's Gambit intro at a different pitch in each ear, and a recording
    // had the line on the left only), and its Doppler came from the camera's own velocity estimate.
    // Played as 2D, as the DX8 engine effectively did: centred, in the file's own stereo, at the
    // distance gain it has right there.
    const bool atListener = p.is3D && dist < kAtListener;
    if (!p.is3D || atListener)
    {
        // 2D: straight into the mix with a per-frame gain ramp (no zipper).
        const float g1 =
            atListener ? std::min(p.gain, 1.f) * InverseDistanceClamped(dist, p.refDist, p.maxDist, p.rolloff) : p.gain;
        const float g0 = v._lastGain < 0.f ? g1 : v._lastGain;
        const float dg = (g1 - g0) / kFrameSize;
        for (int i = 0; i < kFrameSize; i++)
        {
            const float g = g0 + dg * i;
            const float l = src[i * ch];
            const float r = ch > 1 ? src[i * ch + 1] : l;
            _mixL[i] += l * g;
            _mixR[i] += r * g;
        }
        v._lastGain = g1;
        return;
    }

    // 3D: mono, DX8-model distance attenuation (ramped), air absorption, then
    // panning / HRTF.  Gain is applied before Steam Audio so it ramps smoothly.
    // Without Steam Audio (context or allocation failure) the voice still
    // plays, panned, just without air absorption or HRTF; setup is not
    // retried every block.
    bool effects = false;
    if (!v._effectsFailed)
    {
        effects = v.EnsureEffects(_ctx, _hrtf, _settings);
        v._effectsFailed = !effects;
    }

    const float att = InverseDistanceClamped(dist, p.refDist, p.maxDist, p.rolloff);
    const float g1 = std::min(p.gain, 1.f) * att;
    const float g0 = v._lastGain < 0.f ? g1 : v._lastGain;
    const float dg = (g1 - g0) / kFrameSize;
    float* mono = effects ? v._mono.data[0] : _monoScratch.data();
    for (int i = 0; i < kFrameSize; i++)
    {
        const float s = ch > 1 ? 0.5f * (src[i * 2] + src[i * 2 + 1]) : src[i];
        mono[i] = s * (g0 + dg * i);
    }
    v._lastGain = g1;

    // Reverb send: the source's DISTANCE-ATTENUATED level (OpenAL Soft's auxiliary send applies
    // the source's distance gain too -- AL_AUXILIARY_SEND_FILTER_GAIN_AUTO), then faded by the
    // reverb's own decay over distance: 0.001^(d / (decay * c)). The port sent the full,
    // unattenuated gain, so a shot 300 m away drove the reverb as hard as one beside the
    // listener and everything echoed hugely (owner, 2026-10-12).
    if (p.reverbSend && _reverbOn.load(std::memory_order_acquire))
    {
        const float decay = std::max(_reverb.DecayTime(), 0.1f);
        const float w1 = std::min(p.gain, 1.f) * att * std::pow(0.001f, dist / (decay * kSpeedOfSound));
        const float w0 = v._lastWet;
        const float dw = (w1 - w0) / kFrameSize;
        for (int i = 0; i < kFrameSize; i++)
        {
            const float s = ch > 1 ? 0.5f * (src[i * 2] + src[i * 2 + 1]) : src[i];
            sendBuf[i] += s * (w0 + dw * i);
        }
        v._lastWet = w1;
    }
    else
    {
        v._lastWet = 0.f;
    }

    const IPLVector3 srcPos = ToIPL(p.pos);
    const IPLVector3 lisPos = ToIPL(lis.pos);
    const float* dry = mono;
    if (effects)
    {
        // Air absorption (Steam Audio's default frequency-dependent model).
        IPLDirectEffectParams dp{};
        dp.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
        IPLAirAbsorptionModel airModel{};
        airModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        iplAirAbsorptionCalculate(_ctx, srcPos, lisPos, &airModel, dp.airAbsorption);
        dp.distanceAttenuation = 1.f;
        dp.directivity = 1.f;
        dp.occlusion = 1.f;
        for (float& t : dp.transmission)
            t = 1.f;
        iplDirectEffectApply(v._direct, &dp, &v._mono, &v._monoOut);
        dry = v._monoOut.data[0];
    }

    // Direction in listener space.
    IPLVector3 dir{0.f, 0.f, -1.f};
    if (dist > 1e-3f)
        dir = effects ? iplCalculateRelativeDirection(_ctx, srcPos, lisPos, ToIPL(lis.dir), ToIPL(lis.up))
                      : RelativeDirection(srcPos, lisPos, ToIPL(lis.dir), ToIPL(lis.up));

    if (effects && Headphones() && v._binaural && _hrtf)
    {
        IPLBinauralEffectParams bp{};
        bp.direction = dir;
        bp.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        // Very close sources fold back toward mono so they don't snap sides.
        bp.spatialBlend = std::clamp(dist / std::max(p.refDist * 0.1f, 0.5f), 0.f, 1.f);
        bp.hrtf = _hrtf;
        iplBinauralEffectApply(v._binaural, &bp, &v._monoOut, &v._stereo);
        v._havePan = false;
        const float* l = v._stereo.data[0];
        const float* r = v._stereo.data[1];
        for (int i = 0; i < kFrameSize; i++)
        {
            _mixL[i] += l[i];
            _mixR[i] += r[i];
        }
    }
    else
    {
        // Constant-power stereo panning (Steam Audio's stereo law), with the
        // gains crossfaded per sample from the previous block's.  Not
        // iplPanningEffectApply: in 4.8.1 its crossfade weight uses the channel
        // index instead of the sample index, so the gains step at every block.
        float pan[2];
        StereoPanGains(dir, pan);
        if (!v._havePan)
        {
            v._pan[0] = pan[0];
            v._pan[1] = pan[1];
            v._havePan = true;
        }
        const float inv = 1.f / kFrameSize;
        for (int c = 0; c < 2; c++)
        {
            float* outCh = c == 0 ? _mixL.data() : _mixR.data();
            const float w0 = v._pan[c];
            const float dw = pan[c] - w0;
            for (int i = 0; i < kFrameSize; i++)
                outCh[i] += dry[i] * (w0 + dw * ((i + 1) * inv));
            v._pan[c] = pan[c];
        }
    }
}

} // namespace SteamAudio
} // namespace Poseidon
