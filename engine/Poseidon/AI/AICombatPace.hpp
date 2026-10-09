#pragma once

// AI-AIM-001 (owner 2026-10-09: "both"): AI soldiers in combat were slow to put fire on an enemy in plain view.
// Measured (.tw/ai/AIM.Eden): the aim settles in ~0.8 s once still; the time went on dropping prone first (1.7 s,
// no shot possible), on moving to cover while waiting to aim (FireInit waits 0.2-0.7 s and keeps the soldier on
// his normal move), and on slow prone turns. Two switches, off by default:
//
//   class CfgAIFork
//   {
//       shootFirst = 1;    // for 4 s after taking a visible target in range, and not under fire, a soldier aims at
//                          // once (no wait, no move to cover meanwhile) and does not drop prone unless he lies
//                          // already; then stock: down, cover
//       proneSpeed = 1.6;  // AI only: the drop to prone and turning while prone play this much faster
//   };

namespace Poseidon::CombatPace
{
//! CfgAIFork shootFirst
bool ShootFirstEnabled();
//! CfgAIFork proneSpeed (1 = stock)
float ProneSpeed();
//! seconds a firing window lasts after a new target
inline constexpr float kShootFirstWindow = 4.0f;
} // namespace Poseidon::CombatPace
