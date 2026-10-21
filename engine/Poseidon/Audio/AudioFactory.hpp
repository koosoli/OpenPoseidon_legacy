#ifdef _MSC_VER
#pragma once
#endif


namespace Poseidon
{
#ifndef POSEIDON_AUDIO_AUDIOFACTORY_HPP
#define POSEIDON_AUDIO_AUDIOFACTORY_HPP

// Forward declare to avoid pulling in full headers
class IAudioSystem;

} // namespace Poseidon
#include <string>
#include <vector>
namespace Poseidon
{

using AudioBackendCreateFn = IAudioSystem* (*)();
using AudioBackendAvailableFn = bool (*)();

struct AudioBackendDescriptor
{
    const char* codeName;
    const char* displayName;
    int priority;
    AudioBackendCreateFn create;
    AudioBackendAvailableFn isAvailable;
};

struct AudioBackendInfo
{
    const char* codeName = nullptr;
    const char* displayName = nullptr;
    int priority = 0;
    bool isAvailable = false;
};

enum class AudioCreateStatus
{
    Created,
    UnknownBackend,
    Unavailable,
    CreateFailed
};

struct AudioCreateResult
{
    IAudioSystem* system = nullptr;
    AudioBackendInfo backend;
    AudioCreateStatus status = AudioCreateStatus::CreateFailed;
};

class AudioFactory
{
public:
    static bool Register(const AudioBackendDescriptor& descriptor);

    // requestedBackend: empty/"auto" => preferred backend, otherwise code name.
    // noSound prefers the dummy backend only for auto/default selection.
    static AudioCreateResult Create(const std::string& requestedBackend, bool noSound);

    static std::vector<AudioBackendInfo> EnumerateRegistered();
    static std::vector<AudioBackendInfo> EnumerateAvailable();
    static std::string DescribeRegisteredCodes();
    static std::string DescribeAvailableCodes();

private:
    AudioFactory() = delete;
};

/// The audio engine chosen in the dev panel's Audio tab ("openal", "steam"; empty = default),
/// kept in <user dir>/audioEngine.cfg -- its own file, because each backend rewrites audio.cfg
/// from its own fields. Used at start-up when no --audio is given; a change applies on restart.
std::string LoadPreferredAudioBackend();
bool SavePreferredAudioBackend(const std::string& code);
/// Code name of the backend AudioFactory::Create made last ("steam", "openal", ...), or empty.
std::string RunningAudioBackend();

void RegisterDummyAudioBackend();
void RegisterTextAudioBackend();
void RegisterOpenALAudioBackend();
void RegisterSteamAudioBackend(); // engine/PoseidonSteamAudio (POSEIDON_ENABLE_STEAMAUDIO)

} // namespace Poseidon

#endif // POSEIDON_AUDIO_AUDIOFACTORY_HPP
