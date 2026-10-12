#pragma once

// Voice-over-net audio on SDL3 + the Steam Audio mixer: microphone capture
// through an SDL recording stream, remote speakers and the mic-test sidetone
// as external voices in the game mixer (so they share the master volume and,
// for Direct chat, 3D placement).

#include <Poseidon/Audio/Voice/VoiceBackend.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace Poseidon
{
namespace SteamAudio
{
class MixVoice;
}

class VoNCaptureSDL : public IVoiceCaptureBackend
{
  public:
    VoNCaptureSDL() = default;
    ~VoNCaptureSDL() override;
    VoNCaptureSDL(const VoNCaptureSDL&) = delete;
    VoNCaptureSDL& operator=(const VoNCaptureSDL&) = delete;

    bool open(const char* deviceName, int sampleRate, int bufferSamples) override;
    void close() override;
    void start() override;
    void stop() override;
    int availableSamples() const override;
    int read(int16_t* buffer, int maxSamples) override;
    float lastFramePeak() const override { return _lastFramePeak; }
    bool isOpen() const override { return _stream != nullptr; }
    bool isCapturing() const override { return _capturing; }
    int sampleRate() const override { return _sampleRate; }

    static std::vector<std::string> ListDevices();

  private:
    SDL_AudioStream* _stream = nullptr;
    int _sampleRate = 0;
    bool _capturing = false;
    float _lastFramePeak = 0.0f;
    bool _sdlInit = false;
};

class VoNSpeakerSDL : public IVoiceSpeakerBackend
{
  public:
    ~VoNSpeakerSDL() override { destroy(); }

    void init() override;
    void destroy() override;
    void setChannel(VoNChatChannel channel) override;
    void setPosition(float x, float y, float z) override;
    void stopStream() override;
    bool feed(VoNClient* client, uint32_t channel) override;
    bool isActive() const override { return _active; }
    float level() const override { return _level; }

    // Test hook: the mixer voice (nullptr when no mixer / drain-only).
    const SteamAudio::MixVoice* VoiceForTests() const { return _voice.get(); }

  private:
    void ApplySpatialState();
    bool DrainDecodedOnly(VoNClient* client, uint32_t channel);
    void UpdateLevel(const int16_t* pcm, int n);

    static constexpr int kFrameSamples = 320; // 20 ms at 16 kHz
    static constexpr int kSampleRate = 16000;
    static constexpr int kStartThreshold = 5; // frames buffered before playback (~100 ms)
    static constexpr int kMaxQueuedFrames = 24;
    static constexpr int kDrainLimit = 30;    // empty feeds before the stream is closed (~500 ms)

    std::shared_ptr<SteamAudio::MixVoice> _voice;
    VoNChatChannel _chatChannel = VoNChatChannel::Global;
    float _position[3] = {};
    bool _active = false;
    int _drainFrames = 0;
    float _level = 0.0f;
};

class MicLoopbackSDL : public IVoiceLoopbackBackend
{
  public:
    ~MicLoopbackSDL() override { close(); }

    bool open(int sampleRate) override;
    void close() override;
    void tick(VoNCapture& capture) override;
    bool isOpen() const override { return _voice != nullptr; }

  private:
    std::shared_ptr<SteamAudio::MixVoice> _voice;
};

} // namespace Poseidon
