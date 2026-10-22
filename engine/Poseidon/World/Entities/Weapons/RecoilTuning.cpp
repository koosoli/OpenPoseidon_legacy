// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#include <Poseidon/World/Entities/Weapons/RecoilTuning.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace Poseidon
{

RecoilTuning GRecoilTune;

// Ranges are chosen so the shipped default sits somewhere useful inside them rather than at
// an end, and so no setting can be dragged somewhere that breaks the simulation: the spring
// periods never reach zero (which would divide by zero in the frequency), and the dampings
// stay positive (negative damping is a spring that grows without bound).
const RecoilKnob GRecoilKnobs[] = {
    // key              label                    member                          lo      hi     dp
    {"scale",          "Overall strength",       &RecoilTuning::scale,          0.05f,  2.00f, 2},
    {"impulse",        "Vertical kick",          &RecoilTuning::impulse,        0.00f,  4.00f, 3},
    {"permanent",      "Rise per shot",          &RecoilTuning::permanent,      0.00f,  0.006f, 4},
    {"biasUp",         "Upward bias",            &RecoilTuning::biasUp,         0.00f,  1.00f, 2},
    {"springPeriod",   "Aim spring period",      &RecoilTuning::springPeriod,   0.10f,  1.20f, 2},
    {"springDamping",  "Aim spring damping",     &RecoilTuning::springDamping,  0.20f,  1.20f, 2},
    {"kickVisual",     "Camera kick",            &RecoilTuning::kickVisual,     0.00f,  6.00f, 3},
    {"camPeriod",      "Camera period",          &RecoilTuning::camPeriod,      0.05f,  1.00f, 2},
    {"camDamping",     "Camera damping",         &RecoilTuning::camDamping,     0.20f,  1.20f, 2},
    {"opticsKick",     "Optic punch",            &RecoilTuning::opticsKick,     0.00f,  8.00f, 2},
    {"mgBracedKeep",   "MG unbraced penalty",    &RecoilTuning::mgBracedKeep,   0.00f,  1.00f, 2},
    {"mgWander",       "MG shake",               &RecoilTuning::mgWander,       0.00f,  2.00f, 2},
    {"mgKickback",     "MG kickback",            &RecoilTuning::mgKickback,     0.20f,  4.00f, 2},
    {"mgBipod",        "MG prone (bipod)",       &RecoilTuning::mgBipod,        0.05f,  2.00f, 2},
};

const int GRecoilKnobCount = (int)(sizeof(GRecoilKnobs) / sizeof(GRecoilKnobs[0]));

// PIM-RECOIL port: next to inventory_settings.cfg in the user folder, where the inventory port
// keeps its settings (PhysicalInventory wrote Remastered\recoil_settings.cfg in the game folder).
static FILE* OpenRecoilCfg(const char* mode)
{
    const auto& userDir = Foundation::GamePaths::Instance().UserDir();
    if (userDir.empty())
        return nullptr;
    std::filesystem::path directory(userDir);
    if (mode[0] == 'w')
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
            return nullptr;
    }
    return fopen((directory / "recoil_settings.cfg").string().c_str(), mode);
}

void ResetRecoilTuningToDefault()
{
    GRecoilTune = RecoilTuning();
}

void LoadRecoilTuning()
{
    FILE* f = OpenRecoilCfg("r");
    if (!f)
    {
        return; // no file yet - defaults stand
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[128];
        double val;
        if (sscanf(line, " %127[^= ] = %lf", key, &val) != 2)
        {
            continue;
        }
        // EXACT key match, unlike the inventory loader's strstr: several keys here are
        // prefixes of others ("scale" is inside "lateralScale", "camPeriod" shares a tail
        // with "backPeriod"), and a substring match would assign the wrong field.
        for (int i = 0; i < GRecoilKnobCount; i++)
        {
            const RecoilKnob& k = GRecoilKnobs[i];
            if (strcmp(key, k.key) != 0)
            {
                continue;
            }
            float v = (float)val;
            // Clamp on load too: a hand-edited file is as able to name a broken value as a
            // slider is unable to.
            if (v < k.lo) v = k.lo;
            if (v > k.hi) v = k.hi;
            GRecoilTune.*(k.member) = v;
            break;
        }
    }
    fclose(f);
}

void SaveRecoilTuning()
{
    FILE* f = OpenRecoilCfg("w");
    if (!f)
    {
        return;
    }
    fprintf(f, "// Recoil tuning (written by the Recoil page of the in-game edit menu).\n");
    fprintf(f, "// Delete this file to go back to the built-in defaults.\n");
    fprintf(f, "// Values are clamped to the slider range on load.\n");
    for (int i = 0; i < GRecoilKnobCount; i++)
    {
        const RecoilKnob& k = GRecoilKnobs[i];
        fprintf(f, "%-14s = %-10.5f // %s  (%.4f .. %.4f)\n", k.key, GRecoilTune.*(k.member), k.label, k.lo,
                k.hi);
    }
    fclose(f);
}

} // namespace Poseidon
