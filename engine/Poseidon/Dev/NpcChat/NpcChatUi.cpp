// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatUi.hpp>

#include <Poseidon/Foundation/platform.hpp>
// The engine's DebugLog macro (precompiled prelude) collides with ImGui::DebugLog.
#undef DebugLog
#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChat.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatGame.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>
#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>
#include <string>

namespace Poseidon::NpcChat
{
namespace
{
char s_panelBuffer[512] = {};
std::string s_keyMessage;

// Text fields mirror std::string settings through a fixed buffer.
bool EditString(const char* label, std::string& value, const char* hint = nullptr)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    const bool changed = hint ? Dev::InputTextWithHint(label, hint, buf, sizeof(buf))
                              : Dev::InputText(label, buf, sizeof(buf));
    if (changed)
        value = buf;
    return changed;
}

const char* ScancodeName(int code)
{
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    return name && name[0] ? name : "(none)";
}

template <class E>
bool EnumCombo(const char* label, E& value, const char* (*name)(E))
{
    bool changed = false;
    if (Dev::BeginCombo(label, name(value)))
    {
        for (int i = 0; i < static_cast<int>(E::Count); ++i)
        {
            const E e = static_cast<E>(i);
            if (ImGui::Selectable(name(e), e == value))
            {
                value = e;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool KeyRow(const std::string& slot, const char* forWhat)
{
    if (slot.empty())
        return false;
    bool changed = false;
    ImGui::PushID(slot.c_str());
    static std::map<std::string, std::string> entry;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", entry[slot].c_str());
    ImGui::SetNextItemWidth(260);
    // Masked; the typed value lives only in this buffer until "Set" hands it to the key store.
    const bool enter = Dev::InputTextWithHint("##key", "paste API key", buf, sizeof(buf),
                                                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    entry[slot] = buf;
    std::fill(std::begin(buf), std::end(buf), '\0');
    Dev::PanelSameLine();
    if (Dev::Button("Set") || enter)
    {
        changed = true;
        SetKey(slot, entry[slot]);
        std::fill(entry[slot].begin(), entry[slot].end(), '\0');
        entry[slot].clear();
        std::string error;
        if (CurrentSettings().rememberKeys && !PersistKeys(true, error))
            s_keyMessage = error;
        else
            s_keyMessage.clear();
    }
    Dev::PanelSameLine();
    // Straight from the clipboard into the key store: nothing typed, nothing shown.
    if (Dev::Button("Paste key from clipboard"))
    {
        const char* clip = ImGui::GetClipboardText();
        if (clip && clip[0])
        {
            changed = true;
            SetKey(slot, clip);
            std::string error;
            if (CurrentSettings().rememberKeys && !PersistKeys(true, error))
                s_keyMessage = error;
        }
    }
    Dev::PanelSameLine();
    if (Dev::Button("Clear"))
    {
        changed = true;
        SetKey(slot, "");
        std::string error;
        if (CurrentSettings().rememberKeys)
            PersistKeys(true, error);
    }
    const char* env = KeySlotEnvVar(slot);
    if (HasPanelKey(slot) || HasEnvKey(slot))
        ImGui::TextDisabled("stored: %s -- compare with the provider's key page", KeyFingerprint(slot).c_str());
    if (HasPanelKey(slot))
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s key set (%s)", slot.c_str(), forWhat);
    else if (HasEnvKey(slot))
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "using %s (%s)", env, forWhat);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "no %s key (%s)", slot.c_str(), forWhat);
    ImGui::PopID();
    return changed;
}

/// A coloured dot + the provider's own words: what "am I connected?" looks like.
void StatusLight(const char* what, const LinkStatus& status)
{
    ImVec4 colour(0.6f, 0.6f, 0.6f, 1.0f);
    const char* state = "not checked";
    switch (status.state)
    {
        case LinkState::Checking:
            colour = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            state = "checking";
            break;
        case LinkState::Ok:
            colour = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            state = "CONNECTED";
            break;
        case LinkState::Failed:
            colour = ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
            state = "NOT CONNECTED";
            break;
        default: break;
    }
    ImGui::TextColored(colour, "(*) %s: %s", what, state);
    if (!status.message.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s", status.message.c_str());
        ImGui::PopStyleColor();
    }
}

/// An address field that shows the known default as real text, so nothing needs typing for the
/// big providers. Stored empty while it equals the default (a later default change still applies).
bool EditUrl(const char* label, std::string& value, const std::string& fallback)
{
    std::string shown = value.empty() ? fallback : value;
    if (!EditString(label, shown))
        return false;
    value = shown == fallback ? std::string() : shown;
    return true;
}

/// Providers whose address never changes keep it under a fold; local servers show it openly.
bool AddressRow(const char* label, std::string& value, const std::string& fallback, bool fixed)
{
    if (!fixed)
        return EditUrl(label, value, fallback);
    bool changed = false;
    if (ImGui::TreeNode(label, "Address: %s", (value.empty() ? fallback : value).c_str()))
    {
        changed = EditUrl("##address", value, fallback);
        if (!value.empty() && Dev::Button("Use the default address"))
        {
            value.clear();
            changed = true;
        }
        ImGui::TreePop();
    }
    return changed;
}

/// A model field with the provider's list beside it: pick from the list or type any name.
bool SpeechModelPicker(const char* label, std::string& value, const std::string& fallback,
                       const std::vector<ModelInfo>& offered)
{
    bool changed = EditString(label, value, fallback.c_str());
    if (offered.empty())
        return changed;
    Dev::PanelSameLine();
    const std::string current = value.empty() ? fallback : value;
    std::string combo = std::string("##list") + label;
    ImGui::SetNextItemWidth(140);
    if (Dev::BeginCombo(combo.c_str(), (std::to_string(offered.size()) + " available").c_str()))
    {
        for (const ModelInfo& m : offered)
        {
            const std::string text = m.id + (m.name.empty() || m.name == m.id ? "" : "   " + m.name);
            if (ImGui::Selectable(text.c_str(), m.id == current))
            {
                value = m.id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool InPool(const std::string& pool, const std::string& id)
{
    for (const std::string& v : SplitList(pool))
        if (v == id)
            return true;
    return false;
}

std::string TogglePool(const std::string& pool, const std::string& id)
{
    std::vector<std::string> ids = SplitList(pool);
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end())
        ids.erase(it);
    else
        ids.push_back(id);
    std::string out;
    for (const std::string& v : ids)
        out += (out.empty() ? "" : ",") + v;
    return out;
}


void DrawTranscript(float height)
{
    ImGui::BeginChild("##npcTranscript", ImVec2(0, height), ImGuiChildFlags_Borders);
    for (const TranscriptLine& line : Transcript())
        DrawTranscriptLine(line);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
/// The chat itself: transcript, input, Send / Cancel / Talk. Shared by the tab and the compact window.
void DrawConversation(float transcriptHeight)
{
    DrawTranscript(transcriptHeight);
    ImGui::SetNextItemWidth(-150);
    const bool send = Dev::InputTextWithHint("##npcSay", "\"Two, cover me\" / \"Command, two tangos, advise\"",
                                               s_panelBuffer, sizeof(s_panelBuffer),
                                               ImGuiInputTextFlags_EnterReturnsTrue);
    Dev::PanelSameLine();
    if ((Dev::Button("Send") || send) && s_panelBuffer[0])
    {
        SubmitText(s_panelBuffer);
        s_panelBuffer[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    Dev::PanelSameLine();
    if (Busy())
    {
        Dev::PanelSameLine();
        if (Dev::Button("Cancel##waiting"))
            CancelRequest();
    }
    Dev::PanelSameLine();
    if (Dev::Button(IsTalking() ? "Stop##talk" : "Talk"))
        IsTalking() ? EndTalk() : BeginTalk();
    const std::string status = StatusLine();
    if (!status.empty())
        ImGui::TextDisabled("%s", status.c_str());
    if (Dev::Button("Copy conversation"))
        ImGui::SetClipboardText(TranscriptText().c_str());
    Dev::PanelSameLine();
    if (Dev::Button("Clear conversation"))
        ClearConversation();
    Dev::PanelSameLine();
    if (Dev::Button("Stop speech"))
        StopSpeech();
}

bool s_compact = false; // dev panel shrunk to just the chat

// ---- settings export / import (system file dialogs; their callbacks may run on another thread) ----
std::mutex s_dialogMutex;
std::string s_pendingExport, s_pendingImport, s_settingsMessage;
int s_settingsGeneration = 0; // bumped by an import so the prompt editor reloads

const SDL_DialogFileFilter kSettingsFilter[] = {{"NPC chat settings", "json"}};

void SDLCALL OnExportChosen(void*, const char* const* files, int)
{
    if (files && files[0])
    {
        std::lock_guard lock(s_dialogMutex);
        s_pendingExport = files[0];
    }
}

void SDLCALL OnImportChosen(void*, const char* const* files, int)
{
    if (files && files[0])
    {
        std::lock_guard lock(s_dialogMutex);
        s_pendingImport = files[0];
    }
}

void ApplyPendingDialogs()
{
    std::string exportPath, importPath;
    {
        std::lock_guard lock(s_dialogMutex);
        exportPath.swap(s_pendingExport);
        importPath.swap(s_pendingImport);
    }
    if (!exportPath.empty())
    {
        if (exportPath.find('.') == std::string::npos)
            exportPath += ".json";
        // The settings never hold a key, so the file is safe to share.
        const std::string json = SettingsToJson(CurrentSettings());
        s_settingsMessage = WriteFileUtf8(exportPath.c_str(), json.data(), json.size())
                                ? "Exported to " + exportPath + " (no API keys in it)."
                                : "Could not write " + exportPath;
    }
    if (!importPath.empty())
    {
        const std::vector<char> raw = ReadFileUtf8(importPath.c_str());
        Settings loaded;
        if (!raw.empty() && SettingsFromJson(std::string(raw.begin(), raw.end()), loaded))
        {
            loaded.rememberKeys = CurrentSettings().rememberKeys; // key storage stays this computer's choice
            CurrentSettings() = loaded;
            SaveSettings();
            ++s_settingsGeneration;
            CheckLlm();
            CheckStt();
            CheckTts();
            s_settingsMessage = "Loaded " + importPath + ".";
        }
        else
        {
            s_settingsMessage = "Not an NPC chat settings file: " + importPath;
        }
    }
}
} // namespace

void DrawTab()
{
    Settings& s = CurrentSettings();
    LoadRememberedKeys();
    bool dirty = false;

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "EXPERIMENTAL: talk to and command NPCs with a language model.");
    ImGui::TextWrapped("Typed or spoken lines, a live situation report (squad, nearby people, known contacts, "
                       "vehicles) and the mission briefing are sent to the provider you choose. Single-player only.");
    dirty |= Dev::Checkbox("Enable NPC chat", &s.enabled);
    if (s.enabled)
    {
        Dev::PanelSameLine();
        ImGui::TextDisabled("%s: type   |   hold %s: talk", ScancodeName(s.chatKey), ScancodeName(s.talkKey));
    }

    std::string why;
    if (!GameAvailable(&why))
        ImGui::TextDisabled("%s", why.c_str());
    dirty |= Dev::Checkbox("Mute the game's own radio voices while NPC voices speak", &s.muteGameVoices);
    Dev::PanelSameLine();
    ImGui::TextDisabled("(\"Roger\", \"Form line\": text stays in the chat)");
    ApplyPendingDialogs();
    if (Dev::Button("Export settings..."))
        SDL_ShowSaveFileDialog(OnExportChosen, nullptr, SDL_GetKeyboardFocus(), kSettingsFilter, 1,
                               "npc-chat-settings.json");
    Dev::PanelSameLine();
    if (Dev::Button("Import settings..."))
        SDL_ShowOpenFileDialog(OnImportChosen, nullptr, SDL_GetKeyboardFocus(), kSettingsFilter, 1, nullptr, false);
    Dev::PanelSameLine();
    ImGui::TextDisabled("providers, models, prompt, tone, call signs, voices -- never keys");
    if (!s_settingsMessage.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", s_settingsMessage.c_str());
    dirty |= Dev::Checkbox("Log everything sent to and received from the providers", &s.logTraffic);
    if (s.logTraffic)
    {
        Dev::PanelSameLine();
        if (Dev::Button("Open log"))
        {
            std::string url = "file:///" + TrafficLogPath();
            for (char& c : url)
                if (c == '\\')
                    c = '/';
            SDL_OpenURL(url.c_str());
        }
        ImGui::TextDisabled("%s (keys hidden, audio summarised)", TrafficLogPath().c_str());
    }
    if (s.enabled && ImGui::BeginTable("##npcLinks", 3))
    {
        // One glance: green = connected, red = not, yellow = checking, grey = not checked / off.
        const auto dot = [](const char* label, const LinkStatus& st)
        {
            ImGui::TableNextColumn();
            const ImVec4 c = st.state == LinkState::Ok         ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                             : st.state == LinkState::Failed   ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f)
                             : st.state == LinkState::Checking ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                                               : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            ImGui::TextColored(c, "(*) %s", label);
        };
        dot("model", LlmStatus());
        dot("voice input", SttStatus());
        dot("voices", TtsStatus());
        ImGui::EndTable();
    }

    // ---- conversation
    if (ImGui::CollapsingHeader("Conversation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawConversation(220.0f);
        if (Dev::Button("Compact: chat only"))
            s_compact = true;
        Dev::PanelSameLine();
        ImGui::TextDisabled("shrinks the dev panel to this chat; \"Full dev panel\" brings it back");
    }

    // ---- language model
    if (ImGui::CollapsingHeader("Language model", ImGuiTreeNodeFlags_DefaultOpen))
    {
        static bool checkedOnce = false;
        if (EnumCombo("Provider##llm", s.llm, LlmProviderName))
        {
            s.llmModel.clear();
            s.llmBaseUrl.clear();
            dirty = true;
            checkedOnce = false;
        }
        const bool keyless = s.llm == LlmProvider::Ollama || s.llm == LlmProvider::Custom;
        if (KeyRow(LlmKeySlot(s.llm), "language model"))
            CheckLlm();
        if (!checkedOnce && (keyless || !GetKey(LlmKeySlot(s.llm)).empty()))
        {
            checkedOnce = true;
            CheckLlm();
        }
        dirty |= AddressRow("##llmaddr", s.llmBaseUrl, DefaultLlmBaseUrl(s.llm), !keyless);
        StatusLight("Model provider", LlmStatus());
        if (Dev::Button("Check key and load models"))
            CheckLlm();
        Dev::PanelSameLine();
        if (Dev::Button("Test model"))
            TestLlm();
        if (Busy())
        {
            Dev::PanelSameLine();
            if (Dev::Button("Cancel##model"))
                CancelRequest();
        }
        dirty |= Dev::SliderInt("Answer timeout (s)", &s.requestTimeoutSeconds, 10, 300);
        ImGui::TextDisabled("How long to wait for a reply. Free and \"thinking\" models can need a minute or more.");

        // Model picker: the provider's own list, filtered ("free", "llama", "free claude").
        static char filter[96] = {};
        ImGui::SetNextItemWidth(260);
        Dev::InputTextWithHint("##modelFilter", "filter models: free, llama, gemini ...", filter, sizeof(filter));
        const std::vector<ModelInfo> shown = FilterModels(Models(), filter);
        Dev::PanelSameLine();
        ImGui::TextDisabled("%zu of %zu models", shown.size(), Models().size());
        if (!Models().empty())
        {
            ImGui::BeginChild("##models", ImVec2(0, 150), ImGuiChildFlags_Borders);
            const std::string current = s.EffectiveLlmModel();
            for (const ModelInfo& m : shown)
            {
                std::string label = m.id;
                if (m.free)
                    label += "   [free]";
                if (!m.name.empty() && m.name != m.id)
                    label += "   " + m.name;
                if (ImGui::Selectable(label.c_str(), m.id == current))
                {
                    s.llmModel = m.id;
                    dirty = true;
                }
            }
            ImGui::EndChild();
        }
        dirty |= EditString("Model##llm", s.llmModel, DefaultLlmModel(s.llm).c_str());
        dirty |= Dev::SliderFloat("Temperature", &s.temperature, 0.0f, 1.5f, "%.2f");
        dirty |= Dev::SliderInt("Max answer tokens", &s.maxTokens, 100, 2000);
        dirty |= Dev::SliderInt("Conversation memory (exchanges)", &s.historyTurns, 1, 40);
        ImGui::TextDisabled("Free to start: Google Gemini with an AI Studio key (aistudio.google.com/apikey); the same "
                            "key also drives Gemini voices and voice input. Or OpenRouter (filter: free); Ollama runs "
                            "locally without a key.");
    }

    // ---- prompt and context
    if (ImGui::CollapsingHeader("System prompt and context"))
    {
        static char prompt[16384];
        static int loadedGeneration = -1;
        if (loadedGeneration != s_settingsGeneration)
        {
            std::snprintf(prompt, sizeof(prompt), "%s", s.EffectiveSystemPrompt().c_str());
            loadedGeneration = s_settingsGeneration;
        }
        ImGui::TextWrapped("Who the NPCs are and how they talk. The answer format, the order list and the live "
                           "situation report are appended automatically.");
        if (ImGui::InputTextMultiline("##prompt", prompt, sizeof(prompt), ImVec2(-1, 180)))
        {
            s.systemPrompt = prompt;
            dirty = true;
        }
        if (Dev::Button("Reset prompt to default"))
        {
            s.systemPrompt.clear();
            std::snprintf(prompt, sizeof(prompt), "%s", DefaultSystemPrompt().c_str());
            dirty = true;
        }
        dirty |= Dev::Checkbox("Send the mission briefing", &s.includeBriefing);
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(120);
        dirty |= Dev::InputInt("max chars", &s.briefingMaxChars, 500, 2000);
        s.briefingMaxChars = std::clamp(s.briefingMaxChars, 0, 40000);
        dirty |= Dev::Checkbox("Send squad state", &s.includeSquad);
        Dev::PanelSameLine();
        dirty |= Dev::Checkbox("Send known enemy contacts", &s.includeEnemies);
        Dev::PanelSameLine();
        dirty |= Dev::Checkbox("Send time and weather", &s.includeEnvironment);
        dirty |= Dev::Checkbox("Allow orders (squad leader only)", &s.allowOrders);
        if (ImGui::TreeNode("Preview the situation report now"))
        {
            static std::string preview;
            if (Dev::Button("Refresh"))
            {
                const ReportResult r = BuildSituationReport(s);
                preview = r.ok ? r.text + "\n(default speaker: " + r.defaultSpeaker + ")" : r.error;
            }
            ImGui::BeginChild("##preview", ImVec2(0, 220), ImGuiChildFlags_Borders);
            ImGui::Text("%s", preview.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
    }

    // ---- speech to text
    if (ImGui::CollapsingHeader("Speech to text (push to talk)"))
    {
        static bool checkedOnce = false;
        if (EnumCombo("Provider##stt", s.stt, SttProviderName))
        {
            s.sttModel.clear();
            s.sttBaseUrl.clear();
            dirty = true;
            checkedOnce = false;
        }
        if (s.stt != SttProvider::Off)
        {
            const bool keyless = s.stt == SttProvider::Custom;
            if (KeyRow(SttKeySlot(s.stt), "speech to text"))
                CheckStt();
            if (!checkedOnce && (keyless || !GetKey(SttKeySlot(s.stt)).empty()))
            {
                checkedOnce = true;
                CheckStt();
            }
            dirty |= AddressRow("##sttaddr", s.sttBaseUrl, DefaultSttBaseUrl(s.stt), !keyless);
            StatusLight("Speech to text", SttStatus());
            if (Dev::Button("Check##stt"))
                CheckStt();
            Dev::PanelSameLine();
            // Hold, speak, release: shows what the provider heard; nothing is sent to the model.
            Dev::Button(IsTalking() ? "Recording... release to transcribe" : "Hold to test: say a sentence");
            if (ImGui::IsItemActivated())
                BeginSttTest();
            if (ImGui::IsItemDeactivated() && IsTalking())
                EndTalk();
            for (auto it = Transcript().rbegin(); it != Transcript().rend(); ++it)
                if (it->who == "Speech-to-text test")
                {
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", it->text.c_str());
                    break;
                }
            dirty |= SpeechModelPicker("Model##stt", s.sttModel, DefaultSttModel(s.stt), SttModels());
            dirty |= EditString("Language##stt", s.sttLanguage, "en");
            if (IsTalking())
                ImGui::ProgressBar(std::clamp(MicLevel(), 0.0f, 1.0f), ImVec2(200, 0), "mic");
        }
        ImGui::TextDisabled("Free options: Groq (free tier) or a local whisper.cpp server via Custom.");
    }

    // ---- text to speech
    if (ImGui::CollapsingHeader("Text to speech (NPC voices)"))
    {
        static bool checkedOnce = false;
        if (EnumCombo("Provider##tts", s.tts, TtsProviderName))
        {
            s.ttsModel.clear();
            s.ttsBaseUrl.clear();
            s.ttsVoices.clear();
            dirty = true;
            checkedOnce = false;
        }
        if (s.tts != TtsProvider::Off)
        {
            const bool keyless = s.tts == TtsProvider::WindowsSapi || s.tts == TtsProvider::Custom;
            if (s.tts != TtsProvider::WindowsSapi)
            {
                if (KeyRow(TtsKeySlot(s.tts), "voices"))
                    CheckTts();
                dirty |= AddressRow("##ttsaddr", s.ttsBaseUrl, DefaultTtsBaseUrl(s.tts), s.tts != TtsProvider::Custom);
                if (s.tts != TtsProvider::Google)
                    dirty |= SpeechModelPicker("Model##tts", s.ttsModel, DefaultTtsModel(s.tts), TtsModels());
            }
            if (!checkedOnce && (keyless || !GetKey(TtsKeySlot(s.tts)).empty()))
            {
                checkedOnce = true;
                CheckTts();
            }
            if (s.tts == TtsProvider::Gemini)
                ImGui::TextDisabled("Uses the same AI Studio key as the Gemini model (aistudio.google.com/apikey). "
                                    "The free tier allows only a few voice lines a minute; past that a Windows "
                                    "voice speaks the line.");
            else if (s.tts == TtsProvider::Google)
                ImGui::TextDisabled("Google Cloud TTS needs the Text-to-Speech API enabled on a Cloud project. For a "
                                    "free AI Studio key, choose \"Google Gemini voices\" instead.");
            StatusLight("Voices", TtsStatus());
            if (Dev::Button("Check and load voices"))
                CheckTts();
            Dev::PanelSameLine();
            if (Dev::Button("Test voice"))
                TestVoice();

            // Voice browser: tick the voices the squad uses, press Test to hear one.
            static char filter[96] = {};
            ImGui::SetNextItemWidth(260);
            Dev::InputTextWithHint("##voiceFilter", "filter voices: male, british, deep ...", filter,
                                   sizeof(filter));
            const std::string pool = s.ttsVoices.empty() ? s.EffectiveTtsVoices() : s.ttsVoices;
            Dev::PanelSameLine();
            ImGui::TextDisabled("%zu voices in use (first = HQ)", SplitList(pool).size());
            if (!Voices().empty())
            {
                std::string f = filter;
                for (char& c : f)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                ImGui::BeginChild("##voices", ImVec2(0, 170), ImGuiChildFlags_Borders);
                int row = 0;
                for (const VoiceInfo& v : Voices())
                {
                    std::string hay = v.name + " " + v.detail + " " + v.id;
                    for (char& c : hay)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!f.empty() && hay.find(f) == std::string::npos)
                        continue;
                    ImGui::PushID(row++);
                    bool use = InPool(pool, v.id);
                    if (Dev::Checkbox("##use", &use))
                    {
                        s.ttsVoices = TogglePool(pool, v.id);
                        dirty = true;
                    }
                    Dev::PanelSameLine();
                    if (Dev::Button("Test"))
                        TestVoiceId(v.id);
                    Dev::PanelSameLine();
                    ImGui::Text("%s", v.name.c_str());
                    if (!v.detail.empty())
                    {
                        Dev::PanelSameLine();
                        ImGui::TextDisabled("%s", v.detail.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            // Voices the list does not know yet (Google adds Gemini voices; there is no list endpoint).
            static char extraVoice[64] = {};
            ImGui::SetNextItemWidth(200);
            const bool addEnter = Dev::InputTextWithHint("##addVoice", "voice name or id, e.g. Fola", extraVoice,
                                                         sizeof(extraVoice), ImGuiInputTextFlags_EnterReturnsTrue);
            Dev::PanelSameLine();
            if ((Dev::Button("Add voice") || addEnter) && extraVoice[0] && !InPool(pool, extraVoice))
            {
                s.ttsVoices = TogglePool(pool, extraVoice);
                dirty = true;
                extraVoice[0] = 0;
            }
            Dev::PanelSameLine();
            if (Dev::Button("Test it") && extraVoice[0])
                TestVoiceId(extraVoice);
            if (!s.ttsVoices.empty() && Dev::Button("Use the default voices"))
            {
                s.ttsVoices.clear();
                dirty = true;
            }
            dirty |= Dev::SliderFloat("Volume##tts", &s.ttsVolume, 0.0f, 3.0f, "%.2f");
            dirty |= Dev::Checkbox("Distant squad mates and HQ sound like a radio", &s.ttsRadioWhenFar);
        }
    }

    // ---- squad: tone and call signs
    if (ImGui::CollapsingHeader("Squad: tone, call signs and voices", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int tone = s.tone;
        if (Dev::SliderInt("How casual the squad is", &tone, 0, kToneCount - 1, ToneName(tone)))
        {
            s.tone = tone;
            dirty = true;
        }
        ImGui::TextDisabled("Added after your system prompt; the prompt itself is not changed.");

        const std::vector<SquadSlot> squad = CurrentSquad();
        if (squad.empty())
        {
            ImGui::TextDisabled("Start a mission to see your squad. Call signs are kept per squad number.");
        }
        else
        {
            ImGui::TextWrapped("Optional radio call signs: call one (\"Hammer, ...\") and that soldier answers. "
                               "Giving someone \"Papa Bear\" takes that name away from HQ. Each soldier can keep his "
                               "own voice.");
            static std::string s_assignMessage;
            if (Dev::Button("Auto-assign male voices"))
            {
                if (Voices().empty())
                    CheckTts(); // load them; the next click assigns
                s_assignMessage = AutoAssignMaleVoices();
            }
            Dev::PanelSameLine();
            if (Dev::Button("Assign my voice pool"))
                s_assignMessage = AssignPoolVoices();
            Dev::PanelSameLine();
            if (Dev::Button("Clear voices"))
            {
                s.squadVoices.clear();
                dirty = true;
                s_assignMessage = "Voices come from the voice pool again.";
            }
            if (!s_assignMessage.empty())
                ImGui::TextDisabled("%s", s_assignMessage.c_str());

            const bool voicesFit = s.squadVoicesProvider == s.tts;
            if (ImGui::BeginTable("##squad", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22);
                ImGui::TableSetupColumn("Soldier");
                ImGui::TableSetupColumn("Call sign");
                ImGui::TableSetupColumn("Voice");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44);
                ImGui::TableHeadersRow();
                for (const SquadSlot& slot : squad)
                {
                    ImGui::PushID(slot.id);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", slot.id);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s, %s%s", slot.name.c_str(), slot.role.c_str(),
                                slot.isPlayer ? " (you)" : slot.dead ? " (dead)" : "");
                    if (!slot.isPlayer)
                    {
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        std::string sign = s.CallsignFor(slot.id);
                        if (EditString("##callsign", sign, "call sign"))
                        {
                            if (sign.empty())
                                s.callsigns.erase(slot.id);
                            else
                                s.callsigns[slot.id] = sign;
                            dirty = true;
                        }
                        ImGui::TableNextColumn();
                        const std::string current = voicesFit ? s.SquadVoiceFor(slot.id) : std::string();
                        std::string preview = current.empty() ? std::string("(automatic)") : current;
                        for (const VoiceInfo& v : Voices())
                            if (v.id == current)
                                preview = v.name + (v.detail.empty() ? "" : "  (" + v.detail + ")");
                        ImGui::SetNextItemWidth(-1);
                        if (Dev::BeginCombo("##voice", preview.c_str()))
                        {
                            if (ImGui::Selectable("(automatic)", current.empty()))
                            {
                                s.squadVoices.erase(slot.id);
                                dirty = true;
                            }
                            for (const VoiceInfo& v : Voices())
                            {
                                const std::string label = v.name + (v.detail.empty() ? "" : "  (" + v.detail + ")");
                                if (ImGui::Selectable(label.c_str(), v.id == current))
                                {
                                    if (!voicesFit)
                                        s.squadVoices.clear(); // ids from another provider mean nothing here
                                    s.squadVoicesProvider = s.tts;
                                    s.squadVoices[slot.id] = v.id;
                                    dirty = true;
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::TableNextColumn();
                        if (Dev::Button("Test"))
                        {
                            if (!current.empty())
                                TestVoiceId(current);
                            else
                                TestVoice();
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (Voices().empty())
                ImGui::TextDisabled("Voice lists load in Text to speech -> Check and load voices.");

            // Personas: folded away so the tab stays short. Each soldier has a one-line default from
            // his role; whatever is typed here replaces it.
            if (ImGui::TreeNode("Personas (how each soldier talks)"))
            {
                static const char* const kChatter[] = {"off", "rare", "normal", "chatty"};
                int chatter = s.eventChatter;
                if (Dev::SliderInt("Reactions to events", &chatter, 0, 3, kChatter[std::clamp(chatter, 0, 3)]))
                {
                    s.eventChatter = chatter;
                    dirty = true;
                }
                ImGui::TextDisabled("Hits, deaths, new contacts, kills: one short remark at most (one request each).");
                std::string jokerLabel = s.jokerSlot < 0 ? "automatic" : s.jokerSlot == 0 ? "nobody"
                                                                                           : std::to_string(s.jokerSlot);
                if (Dev::BeginCombo("Squad joker", jokerLabel.c_str()))
                {
                    if (ImGui::Selectable("automatic (second squad mate)", s.jokerSlot < 0))
                    {
                        s.jokerSlot = -1;
                        dirty = true;
                    }
                    if (ImGui::Selectable("nobody", s.jokerSlot == 0))
                    {
                        s.jokerSlot = 0;
                        dirty = true;
                    }
                    for (const SquadSlot& slot : squad)
                        if (!slot.isPlayer)
                        {
                            const std::string text = std::to_string(slot.id) + " " + slot.name;
                            if (ImGui::Selectable(text.c_str(), s.jokerSlot == slot.id))
                            {
                                s.jokerSlot = slot.id;
                                dirty = true;
                            }
                        }
                    ImGui::EndCombo();
                }
                for (const SquadSlot& slot : squad)
                {
                    if (slot.isPlayer)
                        continue;
                    ImGui::PushID(slot.id + 1000);
                    ImGui::Text("%d %s, %s", slot.id, slot.name.c_str(), slot.role.c_str());
                    std::string persona = s.personas.count(slot.id) ? s.personas[slot.id] : std::string();
                    ImGui::SetNextItemWidth(-90);
                    if (EditString("##persona", persona, DefaultPersona(slot.id, slot.role).c_str()))
                    {
                        if (persona.empty())
                            s.personas.erase(slot.id);
                        else
                            s.personas[slot.id] = persona;
                        dirty = true;
                    }
                    if (!persona.empty())
                    {
                        Dev::PanelSameLine();
                        if (Dev::Button("Default"))
                        {
                            s.personas.erase(slot.id);
                            dirty = true;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::TextDisabled("Grey text is the default. Keep it short: one line is plenty.");
                ImGui::TreePop();
            }
        }
    }

    // ---- keys, limits, storage
    if (ImGui::CollapsingHeader("Hotkeys, limits and key storage"))
    {
        auto bindRow = [&](const char* label, int& key)
        {
            ImGui::PushID(label);
            ImGui::Text("%s: %s", label, ScancodeName(key));
            Dev::PanelSameLine();
            if (Dev::Button(KeyBindingArmed(&key) ? "press a key... (Esc cancels)" : "Change"))
                ArmKeyBinding(&key);
            ImGui::PopID();
        };
        bindRow("Open chat box", s.chatKey);
        bindRow("Hold to talk", s.talkKey);
        dirty |= Dev::SliderInt("Max requests per minute", &s.maxRequestsPerMinute, 1, 60);
        dirty |= Dev::SliderInt("Request timeout (s)", &s.requestTimeoutSeconds, 10, 300);

        ImGui::Separator();
        bool remember = s.rememberKeys;
        if (Dev::Checkbox("Remember API keys on this computer", &remember))
        {
            std::string error;
            if (PersistKeys(remember, error))
            {
                s.rememberKeys = remember;
                dirty = true;
                s_keyMessage = remember ? "Keys saved (encrypted for this Windows account)." : "Saved keys deleted.";
            }
            else
            {
                s_keyMessage = error;
            }
        }
        ImGui::TextWrapped("Keys are kept in memory only, unless this is ticked. Then they are encrypted with "
                           "Windows DPAPI (readable only by this Windows account on this computer) and stored at:");
        ImGui::TextDisabled("%s", KeyStorePath().c_str());
        ImGui::TextWrapped("They are never written to the game folder, the profile folder, logs, or settings "
                           "files, and never into a git repository. Keys are sent only to the provider you choose.");
        if (Dev::Button("Forget all keys now"))
        {
            ClearAllKeys();
            std::string error;
            PersistKeys(false, error);
            s.rememberKeys = false;
            dirty = true;
            s_keyMessage = "All keys forgotten.";
        }
        if (!s_keyMessage.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", s_keyMessage.c_str());
        ImGui::TextDisabled("Settings (no keys): %s", SettingsPath().c_str());
    }

    if (dirty)
        SaveSettings();
}


bool CompactPanel()
{
    return s_compact;
}

void DrawCompactPanel()
{
    ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("NPC Chat##compact"))
    {
        if (Dev::Button("Full dev panel"))
            s_compact = false;
        Dev::PanelSameLine();
        const LinkStatus& link = LlmStatus();
        const ImVec4 c = link.state == LinkState::Ok         ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                         : link.state == LinkState::Failed   ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f)
                         : link.state == LinkState::Checking ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(c, "(*) %s / %s", LlmProviderName(CurrentSettings().llm),
                           CurrentSettings().EffectiveLlmModel().c_str());
        const float height = std::max(80.0f, ImGui::GetContentRegionAvail().y - 90.0f);
        DrawConversation(height);
    }
    ImGui::End();
}

} // namespace Poseidon::NpcChat
