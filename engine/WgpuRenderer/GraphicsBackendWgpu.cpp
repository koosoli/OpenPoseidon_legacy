#include "EngineWgpu.hpp"

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>

#include <cstdlib>

namespace Poseidon
{
namespace
{
Engine* CreateWgpuBackend(const GraphicsEngineParams& params)
{
    // WGR_FORCE_INIT_FAIL=1 -- make WGPU creation fail on demand.
    //
    // The sole-renderer gate (REN-GL33-001, bullet 7) asks that an intentional WGPU init
    // failure produce a clear error rather than a hidden switch to GL33. The refusal itself
    // is implemented and unambiguous -- GameApplication::CreateGraphicsEngine logs
    // "refusing automatic GL33 fallback" and returns nullptr for an explicit `--render
    // wgpu`. What the gate could not do was TRIGGER it: nothing in the tree forced device
    // bring-up to fail, so the bullet sat at "satisfied in code, wants a run" and the run
    // needed hardware that refuses.
    //
    // This is the trigger, and it is deliberately here rather than deeper in the device
    // bring-up. Failing at the factory exercises the path the gate is about -- the caller's
    // decision not to fall back -- without pretending to simulate a driver fault, which a
    // one-line env check cannot honestly do. A test that says "the refusal works" is what
    // was missing; a test that says "we handle every way a device can die" would be a lie.
    if (const char* force = std::getenv("WGR_FORCE_INIT_FAIL"); force != nullptr && force[0] == '1')
    {
        LOG_WARN(Graphics, "WGR_FORCE_INIT_FAIL=1: refusing to create the WGPU backend (gate bullet 7 probe)");
        return nullptr;
    }
    return CreateEngineWgpu(params);
}

bool IsWgpuAvailable()
{
    // Real availability is decided at create time (device/surface bring-up).
    return true;
}
} // namespace

void RegisterWgpuGraphicsBackend()
{
    GraphicsEngineFactory::Register(GraphicsBackendDescriptor{
        "wgpu",
        "WGPU (Rust / wgpu)",
        50,
        &CreateWgpuBackend,
        &IsWgpuAvailable,
    });
}
} // namespace Poseidon
