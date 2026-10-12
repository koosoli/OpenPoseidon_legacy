// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/Diag/AudioTab.hpp>

#include <Poseidon/Foundation/platform.hpp>
// The engine's DebugLog macro (precompiled prelude) collides with ImGui::DebugLog.
#undef DebugLog
#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Audio/AudioFactory.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>

#include <cstring>
#include <string>

namespace Poseidon::Dev
{
namespace
{
const char* DisplayName(const std::string& code)
{
    for (const AudioBackendInfo& info : AudioFactory::EnumerateRegistered())
        if (info.codeName && _stricmp(info.codeName, code.c_str()) == 0)
            return info.displayName;
    return code.empty() ? "none" : code.c_str();
}

void DrawEngineSection()
{
    const std::string running = RunningAudioBackend();
    ImGui::Text("Running now: %s", DisplayName(running));

    // The choice is read at start-up (audioEngine.cfg); switching engines live would have to tear
    // down every playing wave, so a change applies on the next start.
    static std::string preferred = LoadPreferredAudioBackend();
    struct Choice
    {
        const char* label;
        const char* code;
    };
    static const Choice choices[] = {
        {"Default (Steam Audio, OpenAL if it cannot start)", ""},
        {"Steam Audio", "steam"},
        {"OpenAL", "openal"},
    };
    ImGui::TextUnformatted("Audio engine at the next start:");
    for (const Choice& choice : choices)
    {
        const bool active = _stricmp(preferred.c_str(), choice.code) == 0;
        if (RadioButton(choice.label, active) && !active)
        {
            preferred = choice.code;
            SavePreferredAudioBackend(preferred);
        }
    }
    const std::string next = preferred.empty() ? std::string("steam") : preferred;
    if (_stricmp(next.c_str(), running.c_str()) != 0)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart the game to switch to %s.", DisplayName(next));
    ImGui::TextDisabled("--audio steam / --audio openal on the command line overrides this.");

    if (ImGui::TreeNode("Registered audio engines"))
    {
        for (const AudioBackendInfo& info : AudioFactory::EnumerateRegistered())
            ImGui::BulletText("%s (%s), priority %d%s", info.displayName, info.codeName, info.priority,
                              info.isAvailable ? "" : ", not available here");
        ImGui::TreePop();
    }
}

void DrawHrtfSection()
{
    if (!GSoundsys)
    {
        ImGui::TextDisabled("No audio system.");
        return;
    }
    // Live: both engines apply it at once (Steam Audio: binaural HRTF for headphones; OpenAL Soft:
    // its own HRTF). Saved with the other audio settings by the Options menu.
    int mode = static_cast<int>(GSoundsys->GetHrtfMode());
    if (Combo("3D audio (HRTF)", &mode, "Off (speakers)\0Auto\0On (headphones)\0"))
        GSoundsys->SetHrtfMode(static_cast<IAudioSystem::HrtfMode>(mode));
    const IAudioSystem::HrtfStatus status = GSoundsys->GetHrtfStatus();
    ImGui::Text("HRTF %s%s%s", status.active ? "active" : "inactive", status.specifier.empty() ? "" : ": ",
                status.specifier.c_str());
}
} // namespace

void DrawAudioTab()
{
    DrawEngineSection();
    ImGui::Spacing();
    DrawHrtfSection();
}

} // namespace Poseidon::Dev
