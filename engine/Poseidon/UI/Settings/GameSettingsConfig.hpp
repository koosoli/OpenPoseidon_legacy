#pragma once

#include <string>
#include <optional>

namespace Poseidon
{
class GameSettingsConfig
{
  public:
    struct Environment
    {
        virtual ~Environment() = default;
        virtual std::string DetectSystemLanguage() const = 0;
    };

    std::string textLanguage;
    std::string voiceLanguage;
    std::string activeProfile;
    bool blood = true;
    float preferredViewDistance = kMaxViewDistance;
    bool respectMissionViewDistance = false;

    static constexpr float kMinViewDistance = 100.0f;
    // Historical VD-001 measurements at the former 10 km maximum follow.
    // VD-002 raises the maximum and factory default to 50 km for whole-island views.
    //
    // Measured before raising it, stock Everon, 1600x900, wgpu, VSync pinned off, commit
    // b83e5cb9, 2 repeats per arm.  Camera 250 m up at 7250/6350 looking along the island,
    // which is the pose where a long view has something to show at all:
    //
    //     VD       CPU ms   GPU ms    fps   objects drawn   main tris
    //      900 m    10.57     8.23   94.6             742      36,516
    //     2000 m    11.09     9.09   90.2           2,943     129,700
    //     5000 m    12.12     9.86   82.5           6,857     214,183
    //    10000 m    13.00    10.77   76.9           6,872     222,907
    //
    // So 10 km costs ~19% of the frame against 900 m, not the collapse it is usually assumed
    // to be -- and 74% of that (+1.75 of +2.36 GPU ms) is the WATER surface reaching further,
    // not objects: the object colour pass moved +0.05 ms while drawing 9x as many.
    //
    // Note the last row: 5 km -> 10 km adds FIFTEEN objects. Everon is 12.8 km across, so a
    // camera near the middle has already run out of island by ~3.3 km and everything past
    // that is sea and terrain. On stock CWA content a 10 km setting buys horizon, not
    // detail; it is worth having, but do not expect it to fill the distance with objects.
    // VD-002: whole-island viewing, also used by the Options slider and resolver.
    static constexpr float kMaxViewDistance = 50000.0f;

    void LoadDefaults();
    void LoadDefaults(const Environment& env);
    bool Normalize();
    bool Normalize(const Environment& env);
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

bool EnsureGameSettingsFile(GameSettingsConfig& cfg, const std::string& path,
                            const GameSettingsConfig::Environment& env, bool* created = nullptr);
void LoadGameSettings();
void SaveGameSettings();
std::string LoadActiveProfile();
void SaveActiveProfile(const std::string& name);

const std::string& GetSelectedVoiceLanguage();
void SetSelectedVoiceLanguage(const std::string& language);
float GetSelectedPreferredViewDistance();
void SetSelectedPreferredViewDistance(float distance);
bool GetRespectMissionViewDistance();
void SetRespectMissionViewDistance(bool enabled);
float ClampPreferredViewDistance(float distance);
float ResolveEffectiveViewDistance(float preferredViewDistance, bool respectMissionViewDistance,
                                   const std::optional<float>& missionViewDistance);

} // namespace Poseidon
