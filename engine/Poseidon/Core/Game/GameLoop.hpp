#pragma once

namespace Poseidon
{

extern int gUserFpsCap;

void ProcessMessagesNoWait();
void RenderFrame(float deltaT, bool enableDraw);
bool AppIdle();

// SIM-815: true when POSEIDON_LOCKSTEP_HZ pins the frame's deltaT. Subsystems whose timing
// otherwise depends on a real-time device swap to their deterministic path in this mode --
// today that is the radio drain, which normally waits on audio playback completion.
bool LockstepPacingActive();

} // namespace Poseidon
