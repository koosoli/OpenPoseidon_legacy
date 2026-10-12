
#include <Poseidon/Audio/AudioFactory.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <fstream>
#include <string>
#include <stddef.h>
#include <algorithm>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{

namespace
{
struct RegisteredAudioBackend
{
    AudioBackendDescriptor descriptor;
    size_t registrationOrder;
};

std::vector<RegisteredAudioBackend>& AudioBackendRegistry()
{
    static std::vector<RegisteredAudioBackend> registry;
    return registry;
}

bool MatchesCode(const std::string& lhs, const char* rhs)
{
    return _stricmp(lhs.c_str(), rhs) == 0;
}

bool MatchesCode(const char* lhs, const char* rhs)
{
    return _stricmp(lhs, rhs) == 0;
}

bool IsDescriptorAvailable(const AudioBackendDescriptor& descriptor)
{
    return descriptor.isAvailable == nullptr || descriptor.isAvailable();
}

AudioBackendInfo MakeInfo(const AudioBackendDescriptor& descriptor)
{
    return AudioBackendInfo{
        descriptor.codeName,
        descriptor.displayName,
        descriptor.priority,
        IsDescriptorAvailable(descriptor),
    };
}

bool RegisterBackendImpl(const AudioBackendDescriptor& descriptor)
{
    auto& registry = AudioBackendRegistry();
    const auto existing = std::find_if(registry.begin(), registry.end(), [&](const RegisteredAudioBackend& backend)
                                       { return MatchesCode(descriptor.codeName, backend.descriptor.codeName); });
    if (existing != registry.end())
        return false;

    registry.push_back(RegisteredAudioBackend{descriptor, registry.size()});
    return true;
}

std::vector<const RegisteredAudioBackend*> PreferredBackends()
{
    std::vector<const RegisteredAudioBackend*> ordered;
    for (const auto& backend : AudioBackendRegistry())
        ordered.push_back(&backend);

    std::sort(ordered.begin(), ordered.end(),
              [](const RegisteredAudioBackend* lhs, const RegisteredAudioBackend* rhs)
              {
                  if (lhs->descriptor.priority != rhs->descriptor.priority)
                      return lhs->descriptor.priority > rhs->descriptor.priority;
                  return lhs->registrationOrder < rhs->registrationOrder;
              });
    return ordered;
}

const RegisteredAudioBackend* FindBackendByCode(const std::string& codeName)
{
    const auto& registry = AudioBackendRegistry();
    const auto it = std::find_if(registry.begin(), registry.end(), [&](const RegisteredAudioBackend& backend)
                                 { return MatchesCode(codeName, backend.descriptor.codeName); });
    return it == registry.end() ? nullptr : &(*it);
}

AudioCreateResult TryCreate(const RegisteredAudioBackend& backend)
{
    AudioCreateResult result;
    result.backend = MakeInfo(backend.descriptor);
    if (!result.backend.isAvailable)
    {
        result.status = AudioCreateStatus::Unavailable;
        return result;
    }

    result.system = backend.descriptor.create ? backend.descriptor.create() : nullptr;
    result.status = result.system ? AudioCreateStatus::Created : AudioCreateStatus::CreateFailed;
    return result;
}

std::string JoinBackendCodes(const std::vector<AudioBackendInfo>& backends)
{
    std::string joined;
    for (size_t i = 0; i < backends.size(); ++i)
    {
        if (i > 0)
            joined += ", ";
        joined += backends[i].codeName;
    }
    return joined;
}
} // namespace

bool AudioFactory::Register(const AudioBackendDescriptor& descriptor)
{
    return RegisterBackendImpl(descriptor);
}

namespace
{
std::string g_runningAudioBackend;
AudioCreateResult NoteRunning(AudioCreateResult created)
{
    if (created.status == AudioCreateStatus::Created && created.backend.codeName)
        g_runningAudioBackend = created.backend.codeName;
    return created;
}
} // namespace

std::string RunningAudioBackend()
{
    return g_runningAudioBackend;
}

AudioCreateResult AudioFactory::Create(const std::string& requestedBackend, bool noSound)
{
    const bool autoSelect = requestedBackend.empty() || MatchesCode(requestedBackend, "auto");
    const std::string effectiveRequest = (noSound && autoSelect) ? "dummy" : requestedBackend;

    if (effectiveRequest.empty() || MatchesCode(effectiveRequest, "auto"))
    {
        for (const RegisteredAudioBackend* backend : PreferredBackends())
        {
            AudioCreateResult created = TryCreate(*backend);
            if (created.status == AudioCreateStatus::Created)
                return NoteRunning(created);
        }

        return {};
    }

    const RegisteredAudioBackend* backend = FindBackendByCode(effectiveRequest);
    if (backend == nullptr)
    {
        AudioCreateResult result;
        result.status = AudioCreateStatus::UnknownBackend;
        return result;
    }

    return NoteRunning(TryCreate(*backend));
}

std::vector<AudioBackendInfo> AudioFactory::EnumerateRegistered()
{
    std::vector<AudioBackendInfo> backends;
    for (const RegisteredAudioBackend* backend : PreferredBackends())
        backends.push_back(MakeInfo(backend->descriptor));
    return backends;
}

std::vector<AudioBackendInfo> AudioFactory::EnumerateAvailable()
{
    std::vector<AudioBackendInfo> backends;
    for (const AudioBackendInfo& backend : EnumerateRegistered())
    {
        if (backend.isAvailable)
            backends.push_back(backend);
    }
    return backends;
}

std::string AudioFactory::DescribeRegisteredCodes()
{
    return JoinBackendCodes(EnumerateRegistered());
}

std::string AudioFactory::DescribeAvailableCodes()
{
    return JoinBackendCodes(EnumerateAvailable());
}

} // namespace Poseidon

namespace Poseidon
{
namespace
{
std::string PreferredAudioBackendPath()
{
    return GamePaths::Instance().UserDir() + "audioEngine.cfg"; // GamePaths ends it with a slash
}
} // namespace

std::string LoadPreferredAudioBackend()
{
    std::ifstream in(PreferredAudioBackendPath());
    std::string code;
    if (!in || !std::getline(in, code))
        return {};
    while (!code.empty() && (code.back() == '\r' || code.back() == ' '))
        code.pop_back();
    return code;
}

bool SavePreferredAudioBackend(const std::string& code)
{
    std::ofstream out(PreferredAudioBackendPath(), std::ios::trunc);
    if (!out)
        return false;
    out << code << std::endl;
    return static_cast<bool>(out);
}
} // namespace Poseidon
