#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>

#include <Poseidon/Graphics/Dummy/GraphicsEngineDummy.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/EngineFactory.hpp>
#include <Poseidon/Core/Application.hpp>
#include <stddef.h>
#include <algorithm>
#include <string>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{

namespace
{
struct RegisteredGraphicsBackend
{
    GraphicsBackendDescriptor descriptor;
    size_t registrationOrder;
};

std::vector<RegisteredGraphicsBackend>& GraphicsBackendRegistry()
{
    static std::vector<RegisteredGraphicsBackend> registry;
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

bool IsDescriptorAvailable(const GraphicsBackendDescriptor& descriptor)
{
    return descriptor.isAvailable == nullptr || descriptor.isAvailable();
}

GraphicsBackendInfo MakeInfo(const GraphicsBackendDescriptor& descriptor)
{
    return GraphicsBackendInfo{
        descriptor.codeName,
        descriptor.displayName,
        descriptor.priority,
        IsDescriptorAvailable(descriptor),
    };
}

const RegisteredGraphicsBackend* FindBackendByCode(const std::string& codeName)
{
    const auto& registry = GraphicsBackendRegistry();
    const auto it = std::find_if(registry.begin(), registry.end(), [&](const RegisteredGraphicsBackend& backend)
                                 { return MatchesCode(codeName, backend.descriptor.codeName); });
    return it == registry.end() ? nullptr : &(*it);
}

std::vector<const RegisteredGraphicsBackend*> PreferredBackends()
{
    std::vector<const RegisteredGraphicsBackend*> ordered;
    for (const auto& backend : GraphicsBackendRegistry())
        ordered.push_back(&backend);

    std::sort(ordered.begin(), ordered.end(),
              [](const RegisteredGraphicsBackend* lhs, const RegisteredGraphicsBackend* rhs)
              {
                  if (lhs->descriptor.priority != rhs->descriptor.priority)
                      return lhs->descriptor.priority > rhs->descriptor.priority;
                  return lhs->registrationOrder < rhs->registrationOrder;
              });
    return ordered;
}

/// FAR-001: which backend actually created the live Engine. Written only on a
/// successful create, so a failed attempt at a preferred backend during Auto
/// selection does not leave a lie behind for the one that succeeds after it.
std::string& ActiveBackendCodeStorage()
{
    static std::string code;
    return code;
}

const char* CodeForBackend(GraphicsBackend backend)
{
    switch (backend)
    {
        case GraphicsBackend::Dummy:
            return "dummy";
        case GraphicsBackend::GL33:
            return "gl33";
        case GraphicsBackend::Wgpu:
            return "wgpu";
        case GraphicsBackend::Auto:
            break;
    }
    return nullptr;
}

bool RegisterBackendImpl(const GraphicsBackendDescriptor& descriptor)
{
    if (descriptor.codeName == nullptr || descriptor.codeName[0] == '\0')
        return false;
    if (descriptor.displayName == nullptr || descriptor.displayName[0] == '\0')
        return false;
    if (descriptor.create == nullptr)
        return false;

    auto& registry = GraphicsBackendRegistry();
    const auto existing = std::find_if(registry.begin(), registry.end(), [&](const RegisteredGraphicsBackend& backend)
                                       { return MatchesCode(descriptor.codeName, backend.descriptor.codeName); });
    if (existing != registry.end())
        return false;

    registry.push_back(RegisteredGraphicsBackend{descriptor, registry.size()});
    return true;
}

Engine* CreateDummyBackend(const GraphicsEngineParams&)
{
    return CreateEngineDummy();
}

bool IsDummyAvailable()
{
    return true;
}
} // namespace

Engine* GraphicsEngineFactory::Create(GraphicsBackend backend, const GraphicsEngineParams& params)
{
    if (backend == GraphicsBackend::Auto)
        return CreateAuto(params);

    const char* codeName = CodeForBackend(backend);
    if (codeName == nullptr)
        return nullptr;

    return Create(codeName, params);
}

Engine* GraphicsEngineFactory::Create(const std::string& requestedBackend, const GraphicsEngineParams& params)
{
    if (requestedBackend.empty() || MatchesCode(requestedBackend, "auto"))
        return CreateAuto(params);

    const RegisteredGraphicsBackend* backend = FindBackendByCode(requestedBackend);
    if (backend == nullptr || !IsDescriptorAvailable(backend->descriptor))
        return nullptr;

    Engine* engine = backend->descriptor.create(params);
    if (engine != nullptr)
        ActiveBackendCodeStorage() = backend->descriptor.codeName;
    return engine;
}

bool GraphicsEngineFactory::Register(const GraphicsBackendDescriptor& descriptor)
{
    return RegisterBackendImpl(descriptor);
}

Engine* GraphicsEngineFactory::CreateAuto(const GraphicsEngineParams& params)
{
    for (const RegisteredGraphicsBackend* backend : PreferredBackends())
    {
        if (!IsDescriptorAvailable(backend->descriptor))
            continue;

        Engine* engine = backend->descriptor.create(params);
        if (engine != nullptr)
        {
            ActiveBackendCodeStorage() = backend->descriptor.codeName;
            return engine;
        }
    }

    return nullptr;
}

const char* GraphicsEngineFactory::ActiveBackendCode()
{
    return ActiveBackendCodeStorage().c_str();
}

bool GraphicsEngineFactory::IsBackendAvailable(GraphicsBackend backend)
{
    if (backend == GraphicsBackend::Auto)
    {
        const auto available = EnumerateAvailable();
        return !available.empty();
    }

    const char* codeName = CodeForBackend(backend);
    const RegisteredGraphicsBackend* registered = codeName ? FindBackendByCode(codeName) : nullptr;
    return registered != nullptr && IsDescriptorAvailable(registered->descriptor);
}

std::vector<GraphicsBackendInfo> GraphicsEngineFactory::EnumerateRegistered()
{
    std::vector<GraphicsBackendInfo> backends;
    for (const RegisteredGraphicsBackend* backend : PreferredBackends())
        backends.push_back(MakeInfo(backend->descriptor));
    return backends;
}

std::vector<GraphicsBackendInfo> GraphicsEngineFactory::EnumerateAvailable()
{
    std::vector<GraphicsBackendInfo> backends;
    for (const GraphicsBackendInfo& backend : EnumerateRegistered())
    {
        if (backend.isAvailable)
            backends.push_back(backend);
    }
    return backends;
}

const char* GraphicsEngineFactory::GetBackendName(GraphicsBackend backend)
{
    const char* codeName = CodeForBackend(backend);
    if (codeName != nullptr)
    {
        const RegisteredGraphicsBackend* registered = FindBackendByCode(codeName);
        if (registered != nullptr)
            return registered->descriptor.displayName;
    }

    switch (backend)
    {
        case GraphicsBackend::Dummy:
            return "Dummy (Headless)";
        case GraphicsBackend::GL33:
            return "OpenGL 3.3 Core (SDL3)";
        case GraphicsBackend::Wgpu:
            return "WGPU (Rust / wgpu)";
        case GraphicsBackend::Auto:
            return "Auto (Best Available)";
    }
    return "Unknown";
}

void GraphicsEngineFactory::ResetForTesting()
{
    GraphicsBackendRegistry().clear();
    ActiveBackendCodeStorage().clear();
}

void RegisterDummyGraphicsBackend()
{
    GraphicsEngineFactory::Register(GraphicsBackendDescriptor{
        "dummy",
        "Dummy (Headless)",
        0,
        &CreateDummyBackend,
        &IsDummyAvailable,
    });
}

} // namespace Poseidon
