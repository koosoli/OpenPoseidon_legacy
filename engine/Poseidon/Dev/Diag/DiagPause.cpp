#include <Poseidon/Dev/Diag/DiagPause.hpp>

#include <Poseidon/Core/Application.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon::Dev
{

bool g_diagPauseActive = false;

bool DiagPauseAvailable()
{
    if (!GWorld)
    {
        return false;
    }
    // Network games take a different branch in IsSimulationEnabled entirely, so
    // the flag would do nothing there except lie to the user.
    return GWorld->GetMode() != GModeNetware;
}

bool SetDiagPause(bool paused)
{
    if (paused && !DiagPauseAvailable())
    {
        LOG_INFO(Core, "Diag pause refused: no world, or multiplayer");
        return g_diagPauseActive;
    }
    if (g_diagPauseActive != paused)
    {
        g_diagPauseActive = paused;
        LOG_INFO(Core, "Diag pause {}", paused ? "ON" : "OFF");
    }
    return g_diagPauseActive;
}

bool ToggleDiagPause()
{
    return SetDiagPause(!g_diagPauseActive);
}

} // namespace Poseidon::Dev
