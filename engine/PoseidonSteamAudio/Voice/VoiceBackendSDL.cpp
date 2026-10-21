#include <PoseidonSteamAudio/Voice/VoiceBackendSDL.hpp>
#include <PoseidonSteamAudio/Mixer.hpp>

#include <Poseidon/Audio/Voice/VonApp.hpp>
#include <Poseidon/Audio/Voice/VonCapture.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace Poseidon
{
using SteamAudio::Mixer;
using SteamAudio::MixVoice;

// ===========================================================================
// Capture (SDL recording stream, 16-bit mono at the requested rate)
// ===========================================================================

VoNCaptureSDL::~VoNCaptureSDL()
{
    close();
}

bool VoNCaptureSDL::open(const char* deviceName, int sampleRate, int /*bufferSamples*/)
{
    close();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return false;
    _sdlInit = true;

    SDL_AudioDeviceID devid = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (deviceName && *deviceName)
    {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; ids && i < count; i++)
        {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && SDL_strcmp(name, deviceName) == 0)
            {
                devid = ids[i];
                break;
            }
        }
        SDL_free(ids);
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = sampleRate;
    // No callback: the stream buffers captured audio until read() drains it.
    _stream = SDL_OpenAudioDeviceStream(devid, &spec, nullptr, nullptr);
    if (!_stream)
    {
        LOG_WARN(Audio, "VoN capture: SDL_OpenAudioDeviceStream failed: {}", SDL_GetError());
        close();
        return false;
    }
    _sampleRate = sampleRate;
    return true;
}

void VoNCaptureSDL::close()
{
    if (_stream)
    {
        SDL_DestroyAudioStream(_stream);
        _stream = nullptr;
    }
    _capturing = false;
    if (_sdlInit)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        _sdlInit = false;
    }
}

void VoNCaptureSDL::start()
{
    if (!_stream || _capturing)
        return;
    SDL_ClearAudioStream(_stream);
    SDL_ResumeAudioStreamDevice(_stream);
    _capturing = true;
}

void VoNCaptureSDL::stop()
{
    if (!_stream || !_capturing)
        return;
    SDL_PauseAudioStreamDevice(_stream);
    _capturing = false;
}

int VoNCaptureSDL::availableSamples() const
{
    if (!_stream)
        return 0;
    const int bytes = SDL_GetAudioStreamAvailable(_stream);
    return bytes > 0 ? bytes / static_cast<int>(sizeof(int16_t)) : 0;
}

int VoNCaptureSDL::read(int16_t* buffer, int maxSamples)
{
    if (!_stream || maxSamples <= 0)
        return 0;
    const int toRead = std::min(availableSamples(), maxSamples);
    if (toRead <= 0)
        return 0;
    const int bytes = SDL_GetAudioStreamData(_stream, buffer, toRead * static_cast<int>(sizeof(int16_t)));
    const int got = bytes > 0 ? bytes / static_cast<int>(sizeof(int16_t)) : 0;

    // Block peak, normalised to 0..1, for the Audio screen's level meter.
    int maxAbs = 0;
    for (int i = 0; i < got; ++i)
        maxAbs = std::max(maxAbs, std::abs(static_cast<int>(buffer[i])));
    _lastFramePeak = static_cast<float>(maxAbs) / 32767.0f;
    return got;
}

std::vector<std::string> VoNCaptureSDL::ListDevices()
{
    std::vector<std::string> result;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return result;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; ids && i < count; i++)
    {
        if (const char* name = SDL_GetAudioDeviceName(ids[i]))
            result.emplace_back(name);
    }
    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return result;
}

// ===========================================================================
// Remote speaker (external mixer voice, 16 kHz mono)
// ===========================================================================

void VoNSpeakerSDL::init()
{
    _active = false;
    _drainFrames = 0;
    _level = 0.0f;
    if (!Mixer::Instance())
    {
        LOG_WARN(Audio, "VoN# spk NO-MIXER (drain-only fallback, audio inaudible)");
        return;
    }
    _voice = std::make_shared<MixVoice>();
    _voice->SetExternal(kSampleRate);
    ApplySpatialState();
}

void VoNSpeakerSDL::destroy()
{
    if (!_voice)
        return;
    if (Mixer* mixer = Mixer::Instance())
        mixer->Remove(_voice.get());
    _voice.reset();
    _active = false;
    _level = 0.0f;
}

void VoNSpeakerSDL::setChannel(VoNChatChannel channel)
{
    _chatChannel = channel;
    ApplySpatialState();
}

void VoNSpeakerSDL::setPosition(float x, float y, float z)
{
    _position[0] = x;
    _position[1] = y;
    _position[2] = z;
    ApplySpatialState();
}

void VoNSpeakerSDL::ApplySpatialState()
{
    if (!_voice)
        return;
    const bool direct = _chatChannel == VoNChatChannel::Direct;
    const float x = _position[0], y = _position[1], z = _position[2];
    _voice->ModifyParams(
        [&](MixVoice::Params& p)
        {
            p.gain = 1.f;
            p.pitch = 1.f;
            p.is3D = direct; // Direct chat is positional; radio channels are 2D
            p.pos[0] = x;
            p.pos[1] = y;
            p.pos[2] = z;
            p.refDist = 1.f;
            p.maxDist = 20.f;
            p.rolloff = 1.f;
            p.reverbSend = false;
        });
}

void VoNSpeakerSDL::stopStream()
{
    if (!_voice)
        return;
    if (Mixer* mixer = Mixer::Instance())
        mixer->Remove(_voice.get(), true); // wait out a render before rewinding
    _voice->ExternalClear();
    _voice->Rewind(0);
    _active = false;
    _drainFrames = 0;
}

bool VoNSpeakerSDL::feed(VoNClient* client, uint32_t channel)
{
    if (!_voice || !Mixer::Instance())
        return DrainDecodedOnly(client, channel);

    // Pull jitter-buffered frames while the voice has room.
    int16_t decoded[kFrameSamples];
    int queued = 0;
    while (_voice->ExternalQueued() < kMaxQueuedFrames * kFrameSamples &&
           client->pullSpeaker(channel, decoded, kFrameSamples) == kFrameSamples)
    {
        _voice->ExternalPush(decoded, kFrameSamples);
        UpdateLevel(decoded, kFrameSamples);
        ++queued;
    }
    const bool gotData = queued > 0;

    if (!_active)
    {
        // Buffer ~100 ms before starting so the first syllables don't stutter.
        if (_voice->ExternalQueued() >= kStartThreshold * kFrameSamples)
        {
            Mixer::Instance()->Add(_voice);
            _active = true;
            _drainFrames = 0;
            LOG_DEBUG(Audio, "VoN# spk start ch={} queued={}", channel, _voice->ExternalQueued() / kFrameSamples);
        }
        return false;
    }

    // Underruns just play silence until more frames arrive (the mixer voice
    // holds position); close the stream after ~500 ms without data.
    if (gotData)
    {
        _drainFrames = 0;
    }
    else if (++_drainFrames > kDrainLimit)
    {
        LOG_DEBUG(Audio, "VoN# spk stop ch={} (drain timeout)", channel);
        stopStream();
        _level = 0.0f;
    }
    return gotData;
}

bool VoNSpeakerSDL::DrainDecodedOnly(VoNClient* client, uint32_t channel)
{
    if (!client)
        return false;
    int16_t decoded[kFrameSamples];
    bool gotData = false;
    while (client->pullSpeaker(channel, decoded, kFrameSamples) == kFrameSamples)
    {
        gotData = true;
        UpdateLevel(decoded, kFrameSamples);
    }
    if (gotData)
    {
        _active = true;
        _drainFrames = 0;
        return true;
    }
    if (_active && ++_drainFrames > kDrainLimit)
    {
        _active = false;
        _level = 0.0f;
        _drainFrames = 0;
    }
    return false;
}

void VoNSpeakerSDL::UpdateLevel(const int16_t* pcm, int n)
{
    double sum = 0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double>(pcm[i]) * pcm[i];
    _level = static_cast<float>(std::sqrt(sum / n) / 32768.0);
}

// ===========================================================================
// Mic-test sidetone (capture -> external 2D voice)
// ===========================================================================

bool MicLoopbackSDL::open(int sampleRate)
{
    if (_voice)
        return true;
    Mixer* mixer = Mixer::Instance();
    if (!mixer)
    {
        LOG_ERROR(Audio, "MicLoopback: no audio mixer");
        return false;
    }
    _voice = std::make_shared<MixVoice>();
    _voice->SetExternal(sampleRate);
    // Full level, unspatialised: a diagnostic for hearing your own mic.
    _voice->ModifyParams([](MixVoice::Params& p) { p.gain = 1.f; });
    mixer->Add(_voice);
    return true;
}

void MicLoopbackSDL::close()
{
    if (!_voice)
        return;
    if (Mixer* mixer = Mixer::Instance())
        mixer->Remove(_voice.get());
    _voice.reset();
}

void MicLoopbackSDL::tick(VoNCapture& capture)
{
    if (!_voice || !capture.isCapturing())
        return;
    // Keep latency low: at most ~250 ms queued, drain the capture into it.
    constexpr int kChunk = 1024;
    int16_t scratch[kChunk];
    const int maxQueued = capture.sampleRate() / 4;
    int avail = capture.availableSamples();
    while (avail > 0 && _voice->ExternalQueued() < maxQueued)
    {
        const int got = capture.read(scratch, std::min(avail, kChunk));
        if (got <= 0)
            break;
        _voice->ExternalPush(scratch, got);
        avail -= got;
    }
}

// ===========================================================================
// Registration
// ===========================================================================

namespace
{
bool IsSDLVoiceAvailable()
{
    return true; // SDL is always linked; open() reports missing devices
}

std::unique_ptr<IVoiceCaptureBackend> CreateSDLVoiceCapture()
{
    return std::make_unique<VoNCaptureSDL>();
}

std::unique_ptr<IVoiceSpeakerBackend> CreateSDLVoiceSpeaker()
{
    return std::make_unique<VoNSpeakerSDL>();
}

std::unique_ptr<IVoiceLoopbackBackend> CreateSDLVoiceLoopback()
{
    return std::make_unique<MicLoopbackSDL>();
}

std::vector<std::string> ListSDLVoiceDevices()
{
    return VoNCaptureSDL::ListDevices();
}
} // namespace

void RegisterSDLVoiceBackend()
{
    static const bool registered = RegisterVoiceBackend({
        .codeName = "sdl",
        .priority = 1,
        .isAvailable = &IsSDLVoiceAvailable,
        .createCapture = &CreateSDLVoiceCapture,
        .createSpeaker = &CreateSDLVoiceSpeaker,
        .createLoopback = &CreateSDLVoiceLoopback,
        .listDevices = &ListSDLVoiceDevices,
    });
    (void)registered;
}

} // namespace Poseidon
