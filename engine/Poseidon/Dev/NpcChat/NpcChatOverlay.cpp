// SPDX-License-Identifier: GPL-3.0-or-later
// In-game side of the NPC chat UI: the chat box and recent lines drawn while the dev panel is
// closed, and the hotkeys. Not a panel tab, so it uses plain ImGui (the panel-search wrappers
// would hide the chat box whenever the panel search had text in it).
#include <Poseidon/Dev/NpcChat/NpcChatUi.hpp>

#include <Poseidon/Foundation/platform.hpp>
// The engine's DebugLog macro (precompiled prelude) collides with ImGui::DebugLog.
#undef DebugLog
#include <imgui.h>

#include <Poseidon/Dev/NpcChat/NpcChat.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <vector>

namespace Poseidon::NpcChat
{
namespace
{
bool s_chatOpen = false;
bool s_focusChat = false;
char s_chatBuffer[512] = {};
int* s_bindingKey = nullptr; // set while "press a key" is armed

ImVec4 KindColour(LineKind kind)
{
    switch (kind)
    {
        case LineKind::Player: return ImVec4(0.75f, 0.85f, 1.0f, 1.0f);
        case LineKind::Npc: return ImVec4(0.75f, 1.0f, 0.70f, 1.0f);
        case LineKind::Order: return ImVec4(1.0f, 0.85f, 0.45f, 1.0f);
        case LineKind::Error: return ImVec4(1.0f, 0.45f, 0.40f, 1.0f);
        default: return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    }
}

void DrawLineImpl(const TranscriptLine& line, float alpha)
{
    ImVec4 c = KindColour(line.kind);
    c.w *= alpha;
    if (!line.who.empty())
    {
        ImGui::TextColored(c, "%s:", line.who.c_str());
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
}
} // namespace

void DrawTranscriptLine(const TranscriptLine& line, float alpha)
{
    DrawLineImpl(line, alpha);
}

void ArmKeyBinding(int* key)
{
    s_bindingKey = key;
}

bool KeyBindingArmed(const int* key)
{
    return s_bindingKey == key;
}

void DrawOverlay(bool panelVisible)
{
    Update(); // results from the worker, speech playback, microphone
    const Settings& s = CurrentSettings();
    if (!s.enabled || panelVisible)
    {
        if (!s.enabled)
            s_chatOpen = false;
        return;
    }

    const auto& transcript = Transcript();
    const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
    std::vector<const TranscriptLine*> recent;
    for (auto it = transcript.rbegin(); it != transcript.rend() && recent.size() < 6; ++it)
        if (now - it->time < 25.0 || s_chatOpen)
            recent.push_back(&*it);
    const std::string status = StatusLine();
    if (recent.empty() && !s_chatOpen && status.empty())
        return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(720.0f, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.62f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(s_chatOpen ? 0.55f : 0.30f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   (s_chatOpen ? 0 : ImGuiWindowFlags_NoInputs);
    if (ImGui::Begin("##npcChatOverlay", nullptr, flags))
    {
        for (auto it = recent.rbegin(); it != recent.rend(); ++it)
        {
            const float age = static_cast<float>(now - (*it)->time);
            DrawLineImpl(**it, s_chatOpen ? 1.0f : std::clamp(1.0f - (age - 18.0f) / 7.0f, 0.0f, 1.0f));
        }
        if (!status.empty())
        {
            if (IsTalking())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[REC] %s", status.c_str());
            else
                ImGui::TextDisabled("%s%s", status.c_str(), Busy() && s_chatOpen ? "   (Esc: cancel)" : "");
        }
        if (s_chatOpen)
        {
            // Say up front whether anyone is listening, rather than after the first failed line.
            const LinkStatus& link = LlmStatus();
            if (link.state == LinkState::Ok)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 0.9f), "(*) connected");
            else if (link.state == LinkState::Failed)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "(*) model NOT connected -- see dev panel, NPC Chat");
            else if (link.state == LinkState::Checking)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "(*) checking the model...");
            if (s_focusChat)
            {
                ImGui::SetKeyboardFocusHere();
                s_focusChat = false;
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##npcChatInput", "Say something (Enter: send, Esc: close)", s_chatBuffer,
                                         sizeof(s_chatBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (s_chatBuffer[0])
                    SubmitText(s_chatBuffer);
                s_chatBuffer[0] = 0;
                s_chatOpen = false;
            }
        }
    }
    ImGui::End();
}

bool ProcessEvent(const SDL_Event& event)
{
    Settings& s = CurrentSettings();
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        return s_chatOpen && event.type == SDL_EVENT_TEXT_INPUT;
    const int code = static_cast<int>(event.key.scancode);

    if (s_bindingKey && event.type == SDL_EVENT_KEY_DOWN)
    {
        if (code != SDL_SCANCODE_ESCAPE)
        {
            *s_bindingKey = code;
            SaveSettings();
        }
        s_bindingKey = nullptr;
        return true;
    }
    if (!s.enabled)
        return false;

    if (s_chatOpen)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && code == SDL_SCANCODE_ESCAPE)
        {
            if (Busy())
            {
                CancelRequest(); // first Esc cancels the pending answer, the next one closes the box
                return true;
            }
            s_chatOpen = false;
            s_chatBuffer[0] = 0;
        }
        return true; // everything goes to the text box while it is open
    }
    if (ImGui::GetIO().WantTextInput)
        return false; // typing into a dev-panel field

    if (code == s.chatKey && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
    {
        s_chatOpen = true;
        s_focusChat = true;
        s_chatBuffer[0] = 0;
        return true;
    }
    if (code == s.talkKey)
    {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
            BeginTalk();
        else if (event.type == SDL_EVENT_KEY_UP)
            EndTalk();
        return true;
    }
    return false;
}

bool WantsKeyboard()
{
    return s_chatOpen || s_bindingKey != nullptr;
}

} // namespace Poseidon::NpcChat
