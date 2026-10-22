// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#pragma once

namespace Poseidon
{

// ---------------------------------------------------------------------------------------
// Live recoil tuning.
//
// THE DEFAULTS BELOW ARE THE PhysicalInventory 17 VALUES (engine build 2026-09-27), which is
// the recoil that shipped before the spring-kickback experiment. An install with no
// recoil_settings.cfg therefore feels exactly like v17.
//
// The knob set is deliberately the v17 MODEL: kickback comes from the authored CfgRecoils
// curve rather than a spring, so there is no kickback period/damping to expose, no lateral
// scale, and no per-weapon MG multipliers. Only what the engine actually reads appears here -
// a slider that scales nothing is worse than no slider.
//
// Two notes carried over, because they are easy to get wrong when retuning:
//  - The period/damping PAIRS control smoothness. Period is how long the motion takes,
//    damping how much it overshoots on the way back; damping at or above 1.0 removes the
//    bounce entirely.
//  - "Rise per shot" and "Upward bias" BOTH produce muzzle climb, by different means. The
//    first never springs back. The second biases every impulse upward, so sustained fire
//    parks the spring above the aim point even though each kick does return. Zeroing one
//    alone leaves the other's climb behind.
struct RecoilTuning
{
    float scale = 0.50f;          //!< overall strength, multiplies almost everything
    float impulse = 0.874f;       //!< vertical kick, rad/s per unit recoil factor
    float permanent = 0.0025f;    //!< rise per shot that does NOT spring back, radians
    float biasUp = 1.00f;         //!< upward bias of the impulse direction (1.0 = always up)
    float springPeriod = 0.50f;   //!< aim spring: seconds for one oscillation
    float springDamping = 0.85f;  //!< aim spring: >=1 removes the overshoot
    float kickVisual = 1.350f;    //!< camera-only punch, rad/s
    float camPeriod = 0.28f;      //!< camera spring period
    float camDamping = 0.70f;     //!< camera spring damping
    float opticsKick = 2.20f;     //!< ADS overlay scale-up per radian of curve rotation
    float mgBracedKeep = 0.55f;   //!< how much unbraced recoil a belt-fed weapon keeps
    // The three below are MULTIPLIERS on top of everything else, and default to 1.00 = no
    // effect. That matters: the build these came from was a different recoil model, so
    // restoring their old values here would quietly undo the v17 revert. At 1.00 a belt-fed
    // weapon behaves exactly like any other until one of these is actually dragged.
    float mgWander = 1.00f;       //!< MG shake (aim spring + camera punch)
    float mgKickback = 1.00f;     //!< MG shove back along the bore
    float mgBipod = 1.00f;        //!< MG recoil multiplier when PRONE, on top of mgBracedKeep
};

extern RecoilTuning GRecoilTune;

// One row per knob. The edit-menu page is driven entirely off this table - label, range and
// formatting all come from here - so adding a knob is one line here and nothing in the UI.
struct RecoilKnob
{
    const char* key;      //!< cfg file key. Stable: renaming one silently drops saved values.
    const char* label;    //!< menu label
    float RecoilTuning::* member;
    float lo, hi;         //!< slider range
    int decimals;         //!< digits shown in the menu
};

extern const RecoilKnob GRecoilKnobs[];
extern const int GRecoilKnobCount;

void LoadRecoilTuning();
void SaveRecoilTuning();
void ResetRecoilTuningToDefault();

} // namespace Poseidon
