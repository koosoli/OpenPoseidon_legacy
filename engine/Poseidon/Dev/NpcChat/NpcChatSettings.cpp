// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <Poseidon/Foundation/Common/GamePaths.hpp>

#include <cjson/cJSON.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Poseidon::NpcChat
{
const char* LlmProviderName(LlmProvider p)
{
    switch (p)
    {
        case LlmProvider::OpenAI: return "OpenAI";
        case LlmProvider::OpenRouter: return "OpenRouter";
        case LlmProvider::Anthropic: return "Anthropic (Claude)";
        case LlmProvider::Gemini: return "Google Gemini";
        case LlmProvider::Ollama: return "Ollama (local)";
        case LlmProvider::Custom: return "Custom (OpenAI-compatible)";
        default: return "?";
    }
}

const char* SttProviderName(SttProvider p)
{
    switch (p)
    {
        case SttProvider::Off: return "Off (type only)";
        case SttProvider::OpenAI: return "OpenAI";
        case SttProvider::Groq: return "Groq Whisper";
        case SttProvider::Google: return "Google Cloud Speech (Cloud project + billing)";
        case SttProvider::Custom: return "Custom (OpenAI-compatible)";
        case SttProvider::ElevenLabs: return "ElevenLabs Scribe";
        case SttProvider::Gemini: return "Google Gemini (AI Studio key, free tier)";
        default: return "?";
    }
}

const char* TtsProviderName(TtsProvider p)
{
    switch (p)
    {
        case TtsProvider::Off: return "Off (text only)";
        case TtsProvider::WindowsSapi: return "Windows voices (free, offline)";
        case TtsProvider::OpenAI: return "OpenAI";
        case TtsProvider::Google: return "Google Cloud TTS (Cloud project + billing)";
        case TtsProvider::Custom: return "Custom (OpenAI-compatible)";
        case TtsProvider::ElevenLabs: return "ElevenLabs";
        case TtsProvider::Gemini: return "Google Gemini voices (AI Studio key, free tier)";
        default: return "?";
    }
}

const char* LlmKeySlot(LlmProvider p)
{
    switch (p)
    {
        case LlmProvider::OpenAI: return "openai";
        case LlmProvider::OpenRouter: return "openrouter";
        case LlmProvider::Anthropic: return "anthropic";
        case LlmProvider::Gemini: return "gemini";
        case LlmProvider::Ollama: return "";
        case LlmProvider::Custom: return "custom-llm";
        default: return "";
    }
}

const char* SttKeySlot(SttProvider p)
{
    switch (p)
    {
        case SttProvider::OpenAI: return "openai";
        case SttProvider::Groq: return "groq";
        case SttProvider::Google: return "google";
        case SttProvider::Custom: return "custom-stt";
        case SttProvider::ElevenLabs: return "elevenlabs";
        case SttProvider::Gemini: return "gemini";
        default: return "";
    }
}

const char* TtsKeySlot(TtsProvider p)
{
    switch (p)
    {
        case TtsProvider::OpenAI: return "openai";
        case TtsProvider::Google: return "google";
        case TtsProvider::Custom: return "custom-tts";
        case TtsProvider::ElevenLabs: return "elevenlabs";
        case TtsProvider::Gemini: return "gemini";
        default: return "";
    }
}

const char* KeySlotEnvVar(const std::string& slot)
{
    if (slot == "openai") return "OPENAI_API_KEY";
    if (slot == "openrouter") return "OPENROUTER_API_KEY";
    if (slot == "anthropic") return "ANTHROPIC_API_KEY";
    if (slot == "gemini") return "GEMINI_API_KEY";
    if (slot == "google") return "GOOGLE_API_KEY";
    if (slot == "groq") return "GROQ_API_KEY";
    if (slot == "elevenlabs") return "ELEVENLABS_API_KEY";
    return nullptr;
}

std::string DefaultLlmModel(LlmProvider p)
{
    switch (p)
    {
        case LlmProvider::OpenAI: return "gpt-4.1-mini";
        case LlmProvider::OpenRouter: return "openai/gpt-4.1-mini";
        case LlmProvider::Anthropic: return "claude-haiku-5-5";
        case LlmProvider::Gemini: return "gemini-2.5-flash";
        case LlmProvider::Ollama: return "llama3.1:8b";
        default: return "";
    }
}

std::string DefaultLlmBaseUrl(LlmProvider p)
{
    switch (p)
    {
        case LlmProvider::OpenAI: return "https://api.openai.com/v1";
        case LlmProvider::OpenRouter: return "https://openrouter.ai/api/v1";
        case LlmProvider::Anthropic: return "https://api.anthropic.com/v1";
        case LlmProvider::Gemini: return "https://generativelanguage.googleapis.com/v1beta/openai";
        case LlmProvider::Ollama: return "http://localhost:11434/v1";
        case LlmProvider::Custom: return "http://localhost:8080/v1";
        default: return "";
    }
}

std::string DefaultSttModel(SttProvider p)
{
    switch (p)
    {
        case SttProvider::OpenAI: return "gpt-4o-mini-transcribe";
        case SttProvider::Groq: return "whisper-large-v3-turbo";
        case SttProvider::Google: return "latest_short";
        case SttProvider::Custom: return "whisper-1";
        case SttProvider::ElevenLabs: return "scribe_v1";
        case SttProvider::Gemini: return "gemini-3.5-flash-lite"; // 2.5-flash: "no longer available to new users"
        default: return "";
    }
}

std::string DefaultSttBaseUrl(SttProvider p)
{
    switch (p)
    {
        case SttProvider::OpenAI: return "https://api.openai.com/v1";
        case SttProvider::Groq: return "https://api.groq.com/openai/v1";
        case SttProvider::Google: return "https://speech.googleapis.com/v1";
        case SttProvider::Custom: return "http://localhost:8080/v1";
        case SttProvider::ElevenLabs: return "https://api.elevenlabs.io/v1";
        case SttProvider::Gemini: return "https://generativelanguage.googleapis.com/v1beta";
        default: return "";
    }
}

std::string DefaultTtsModel(TtsProvider p)
{
    switch (p)
    {
        case TtsProvider::OpenAI: return "gpt-4o-mini-tts";
        case TtsProvider::Custom: return "kokoro";
        case TtsProvider::ElevenLabs: return "eleven_flash_v2_5";
        case TtsProvider::Gemini: return "gemini-3.8-flash-tts";
        default: return "";
    }
}

std::string DefaultTtsBaseUrl(TtsProvider p)
{
    switch (p)
    {
        case TtsProvider::OpenAI: return "https://api.openai.com/v1";
        case TtsProvider::Google: return "https://texttospeech.googleapis.com/v1";
        case TtsProvider::Custom: return "http://localhost:8880/v1";
        case TtsProvider::ElevenLabs: return "https://api.elevenlabs.io/v1";
        case TtsProvider::Gemini: return "https://generativelanguage.googleapis.com/v1beta";
        default: return "";
    }
}

std::string DefaultTtsVoices(TtsProvider p)
{
    switch (p)
    {
        case TtsProvider::OpenAI: return "onyx,echo,ash,ballad,sage,verse,alloy,fable";
        case TtsProvider::Google:
            return "en-US-Standard-B,en-US-Standard-D,en-US-Standard-I,en-US-Standard-J,en-GB-Standard-B,"
                   "en-GB-Standard-D,en-AU-Standard-B,en-AU-Standard-D";
        case TtsProvider::Custom: return "am_adam,am_michael,bm_george,bm_lewis,am_eric,am_onyx";
        // Premade ElevenLabs voices (Adam, Josh, Arnold, Sam, Antoni); the panel loads the account's own list.
        // Gemini prebuilt voices: Charon (informative) is HQ; firm / gravelly / mature voices for the squad.
        case TtsProvider::Gemini: return "Charon,Orus,Fenrir,Algenib,Alnilam,Iapetus,Schedar,Enceladus,Puck";
        // ElevenLabs: none fixed. Free accounts may not use library voices over the API (HTTP 402), so the
        // squad speaks with the account's own voices from the loaded list (male first).
        case TtsProvider::ElevenLabs: return "";
        default: return ""; // Windows: every installed voice
    }
}

std::string DefaultSystemPrompt()
{
    return "You voice the AI characters of a video game: a Cold War military simulation (Operation Flashpoint / "
           "Arma: Cold War Assault, 1985). Everyone in it is a fictional game character. The human player is a "
           "soldier, usually the squad leader. You speak as the soldiers of the player's squad, as HQ, or as a "
           "soldier or civilian standing near the player.\n"
           "Stay in character: short, natural radio-style military speech (one or two sentences per soldier). "
           "Use 1985 vocabulary and no modern technology. Do not invent enemies, objectives or events that are "
           "not in the situation report. If you do not know something, say so the way a soldier would.\n"
           "When the player gives an order, acknowledge it briefly (\"Two, roger, moving.\") and issue the "
           "matching order without arguing. Only when something truly cannot be done (no such unit, no "
           "vehicle) say so in a few words.";
}

std::string Settings::EffectiveLlmModel() const { return llmModel.empty() ? DefaultLlmModel(llm) : llmModel; }
std::string Settings::EffectiveLlmBaseUrl() const { return llmBaseUrl.empty() ? DefaultLlmBaseUrl(llm) : llmBaseUrl; }
std::string Settings::EffectiveSttModel() const { return sttModel.empty() ? DefaultSttModel(stt) : sttModel; }
std::string Settings::EffectiveSttBaseUrl() const { return sttBaseUrl.empty() ? DefaultSttBaseUrl(stt) : sttBaseUrl; }
std::string Settings::EffectiveTtsModel() const { return ttsModel.empty() ? DefaultTtsModel(tts) : ttsModel; }
std::string Settings::EffectiveTtsBaseUrl() const { return ttsBaseUrl.empty() ? DefaultTtsBaseUrl(tts) : ttsBaseUrl; }
std::string Settings::EffectiveTtsVoices() const { return ttsVoices.empty() ? DefaultTtsVoices(tts) : ttsVoices; }
const char* ToneName(int tone)
{
    switch (tone)
    {
        case 0: return "Strict military";
        case 1: return "Professional";
        case 2: return "Relaxed";
        case 3: return "Buddies";
        default: return "?";
    }
}

std::string ToneInstructions(int tone)
{
    switch (tone)
    {
        case 0:
            return "\n\nTONE: strict military discipline. Address the player by rank (\"Sergeant\") or \"sir\". "
                   "Formal radio procedure, no jokes, no small talk beyond a polite minimum. Orders are "
                   "acknowledged crisply (\"Sir, yes sir!\", \"Wilco.\").";
        case 1:
            return "\n\nTONE: professional. Respectful and to the point; \"sir\" or rank now and then. Brief, "
                   "human small talk is fine when the situation is quiet.";
        case 2:
            return "\n\nTONE: relaxed. The squad has served together a long time: first names and nicknames are "
                   "fine, light jokes when it is quiet, still sharp and serious under fire. They always do what the "
                   "sergeant says.";
        case 3:
            return "\n\nTONE: buddies. The squad are close friends who tease each other and the player, swear "
                   "mildly, complain about rations and the weather, and tell stories when it is quiet. Under "
                   "fire they still follow orders without arguing.";
        default:
            return {};
    }
}

std::string DefaultPersona(int slot, const std::string& role)
{
    std::string r = role;
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (r.find("medic") != std::string::npos)
        return "Doc. Calm Bostonian, worries about everyone, hates being shot at.";
    if (r.find("machine") != std::string::npos || r.find("mg") != std::string::npos)
        return "Big Polish-American from Pittsburgh, loud, talks to his gun.";
    if (r.find("grenadier") != std::string::npos)
        return "Texan, slow drawl, unflappable, dry humour.";
    if (r.find("anti") != std::string::npos || r.find("at ") != std::string::npos)
        return "Quiet Navajo from Arizona, patient, best eyes in the squad.";
    if (r.find("sniper") != std::string::npos)
        return "Few words, very precise, from rural Maine.";
    if (r.find("officer") != std::string::npos || r.find("leader") != std::string::npos)
        return "Experienced and steady, been in since Vietnam.";
    static const char* const riflemen[] = {
        "Brooklyn kid, talks fast, never shuts up about the Mets.",
        "Puerto Rican from Chicago, warm, quick with a comeback.",
        "Farm boy from Iowa, homesick, utterly reliable.",
        "Old hand from Georgia who has seen it all.",
        "Detroit kid, eager, writes letters to his girl every night."};
    return riflemen[(slot < 0 ? 0 : slot) % 5];
}

double EventCooldownSeconds(int eventChatter)
{
    switch (eventChatter)
    {
        case 1: return 90.0;
        case 2: return 40.0;
        case 3: return 15.0;
        default: return 0.0;
    }
}

std::string Settings::PersonaFor(int slot, const std::string& role) const
{
    auto it = personas.find(slot);
    return it != personas.end() && !it->second.empty() ? it->second : DefaultPersona(slot, role);
}

std::string Settings::CallsignFor(int slot) const
{
    auto it = callsigns.find(slot);
    return it == callsigns.end() ? std::string() : it->second;
}

std::string Settings::SquadVoiceFor(int slot) const
{
    if (squadVoicesProvider != tts)
        return {};
    auto it = squadVoices.find(slot);
    return it == squadVoices.end() ? std::string() : it->second;
}

std::string Settings::EffectiveSystemPrompt() const
{
    return systemPrompt.empty() ? DefaultSystemPrompt() : systemPrompt;
}

namespace
{
void ReadBool(const cJSON* root, const char* name, bool& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsBool(item))
        out = cJSON_IsTrue(item);
}

void ReadInt(const cJSON* root, const char* name, int& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item))
        out = item->valueint;
}

void ReadFloat(const cJSON* root, const char* name, float& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item))
        out = static_cast<float>(item->valuedouble);
}

void ReadString(const cJSON* root, const char* name, std::string& out)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring)
        out = item->valuestring;
}

template <class E>
void ReadEnum(const cJSON* root, const char* name, E& out)
{
    int v = static_cast<int>(out);
    ReadInt(root, name, v);
    if (v >= 0 && v < static_cast<int>(E::Count))
        out = static_cast<E>(v);
}
} // namespace

std::string SettingsToJson(const Settings& s)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 4);
    cJSON_AddBoolToObject(root, "enabled", s.enabled);
    cJSON_AddNumberToObject(root, "llm", static_cast<int>(s.llm));
    cJSON_AddStringToObject(root, "llmModel", s.llmModel.c_str());
    cJSON_AddStringToObject(root, "llmBaseUrl", s.llmBaseUrl.c_str());
    cJSON_AddNumberToObject(root, "temperature", s.temperature);
    cJSON_AddNumberToObject(root, "maxTokens", s.maxTokens);
    cJSON_AddNumberToObject(root, "historyTurns", s.historyTurns);
    cJSON_AddStringToObject(root, "systemPrompt", s.systemPrompt.c_str());
    cJSON_AddNumberToObject(root, "tone", s.tone);
    cJSON_AddBoolToObject(root, "includeBriefing", s.includeBriefing);
    cJSON_AddBoolToObject(root, "includeSquad", s.includeSquad);
    cJSON_AddBoolToObject(root, "includeEnemies", s.includeEnemies);
    cJSON_AddBoolToObject(root, "includeEnvironment", s.includeEnvironment);
    cJSON_AddNumberToObject(root, "briefingMaxChars", s.briefingMaxChars);
    cJSON_AddBoolToObject(root, "allowOrders", s.allowOrders);
    cJSON_AddNumberToObject(root, "stt", static_cast<int>(s.stt));
    cJSON_AddStringToObject(root, "sttModel", s.sttModel.c_str());
    cJSON_AddStringToObject(root, "sttBaseUrl", s.sttBaseUrl.c_str());
    cJSON_AddStringToObject(root, "sttLanguage", s.sttLanguage.c_str());
    cJSON_AddNumberToObject(root, "tts", static_cast<int>(s.tts));
    cJSON_AddStringToObject(root, "ttsModel", s.ttsModel.c_str());
    cJSON_AddStringToObject(root, "ttsBaseUrl", s.ttsBaseUrl.c_str());
    cJSON_AddStringToObject(root, "ttsVoices", s.ttsVoices.c_str());
    cJSON_AddStringToObject(root, "ttsLanguage", s.ttsLanguage.c_str());
    cJSON_AddNumberToObject(root, "ttsVolume", s.ttsVolume);
    cJSON_AddBoolToObject(root, "ttsRadioWhenFar", s.ttsRadioWhenFar);
    cJSON_AddBoolToObject(root, "muteGameVoices", s.muteGameVoices);
    cJSON_AddBoolToObject(root, "logTraffic", s.logTraffic);
    cJSON_AddNumberToObject(root, "chatKey", s.chatKey);
    cJSON_AddNumberToObject(root, "talkKey", s.talkKey);
    cJSON_AddNumberToObject(root, "maxRequestsPerMinute", s.maxRequestsPerMinute);
    cJSON_AddNumberToObject(root, "requestTimeoutSeconds", s.requestTimeoutSeconds);
    cJSON_AddBoolToObject(root, "rememberKeys", s.rememberKeys);
    cJSON_AddNumberToObject(root, "squadVoicesProvider", static_cast<int>(s.squadVoicesProvider));
    cJSON* voices = cJSON_AddObjectToObject(root, "squadVoices");
    for (const auto& [slot, voice] : s.squadVoices)
        if (!voice.empty())
            cJSON_AddStringToObject(voices, std::to_string(slot).c_str(), voice.c_str());
    cJSON_AddNumberToObject(root, "eventChatter", s.eventChatter);
    cJSON_AddNumberToObject(root, "jokerSlot", s.jokerSlot);
    cJSON* personas = cJSON_AddObjectToObject(root, "personas");
    for (const auto& [slot, persona] : s.personas)
        if (!persona.empty())
            cJSON_AddStringToObject(personas, std::to_string(slot).c_str(), persona.c_str());
    cJSON* callsigns = cJSON_AddObjectToObject(root, "callsigns");
    for (const auto& [slot, sign] : s.callsigns)
        if (!sign.empty())
            cJSON_AddStringToObject(callsigns, std::to_string(slot).c_str(), sign.c_str());
    char* text = cJSON_Print(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

bool SettingsFromJson(const std::string& json, Settings& s)
{
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return false;
    }
    ReadBool(root, "enabled", s.enabled);
    ReadEnum(root, "llm", s.llm);
    ReadString(root, "llmModel", s.llmModel);
    ReadString(root, "llmBaseUrl", s.llmBaseUrl);
    ReadFloat(root, "temperature", s.temperature);
    ReadInt(root, "maxTokens", s.maxTokens);
    ReadInt(root, "historyTurns", s.historyTurns);
    ReadString(root, "systemPrompt", s.systemPrompt);
    ReadInt(root, "tone", s.tone);
    s.tone = s.tone < 0 ? 0 : s.tone >= kToneCount ? kToneCount - 1 : s.tone;
    ReadBool(root, "includeBriefing", s.includeBriefing);
    ReadBool(root, "includeSquad", s.includeSquad);
    ReadBool(root, "includeEnemies", s.includeEnemies);
    ReadBool(root, "includeEnvironment", s.includeEnvironment);
    ReadInt(root, "briefingMaxChars", s.briefingMaxChars);
    ReadBool(root, "allowOrders", s.allowOrders);
    ReadEnum(root, "stt", s.stt);
    ReadString(root, "sttModel", s.sttModel);
    ReadString(root, "sttBaseUrl", s.sttBaseUrl);
    ReadString(root, "sttLanguage", s.sttLanguage);
    ReadEnum(root, "tts", s.tts);
    ReadString(root, "ttsModel", s.ttsModel);
    ReadString(root, "ttsBaseUrl", s.ttsBaseUrl);
    ReadString(root, "ttsVoices", s.ttsVoices);
    ReadString(root, "ttsLanguage", s.ttsLanguage);
    ReadFloat(root, "ttsVolume", s.ttsVolume);
    ReadBool(root, "ttsRadioWhenFar", s.ttsRadioWhenFar);
    ReadBool(root, "muteGameVoices", s.muteGameVoices);
    ReadBool(root, "logTraffic", s.logTraffic);
    ReadInt(root, "chatKey", s.chatKey);
    ReadInt(root, "talkKey", s.talkKey);
    ReadInt(root, "maxRequestsPerMinute", s.maxRequestsPerMinute);
    ReadInt(root, "requestTimeoutSeconds", s.requestTimeoutSeconds);
    int version = 1;
    ReadInt(root, "version", version);
    if (version < 2 && s.requestTimeoutSeconds == 45)
        s.requestTimeoutSeconds = 90; // v1's default was too short for queued free models
    if (version < 3)
        s.muteGameVoices = true; // v3: the game's own radio voices are muted by default under NPC voices
    if (version < 4 && s.talkKey == 77)
        s.talkKey = 57; // v4: hold Caps Lock to talk (was End)
    ReadBool(root, "rememberKeys", s.rememberKeys);
    ReadEnum(root, "squadVoicesProvider", s.squadVoicesProvider);
    if (const cJSON* voices = cJSON_GetObjectItemCaseSensitive(root, "squadVoices"); cJSON_IsObject(voices))
    {
        s.squadVoices.clear();
        for (const cJSON* item = voices->child; item; item = item->next)
            if (item->string && cJSON_IsString(item) && item->valuestring && item->valuestring[0])
                s.squadVoices[std::atoi(item->string)] = item->valuestring;
    }
    ReadInt(root, "eventChatter", s.eventChatter);
    s.eventChatter = s.eventChatter < 0 ? 0 : s.eventChatter > 3 ? 3 : s.eventChatter;
    ReadInt(root, "jokerSlot", s.jokerSlot);
    if (const cJSON* personas = cJSON_GetObjectItemCaseSensitive(root, "personas"); cJSON_IsObject(personas))
    {
        s.personas.clear();
        for (const cJSON* item = personas->child; item; item = item->next)
            if (item->string && cJSON_IsString(item) && item->valuestring && item->valuestring[0])
                s.personas[std::atoi(item->string)] = item->valuestring;
    }
    if (const cJSON* callsigns = cJSON_GetObjectItemCaseSensitive(root, "callsigns"); cJSON_IsObject(callsigns))
    {
        s.callsigns.clear();
        for (const cJSON* item = callsigns->child; item; item = item->next)
            if (item->string && cJSON_IsString(item) && item->valuestring && item->valuestring[0])
                s.callsigns[std::atoi(item->string)] = item->valuestring;
    }
    cJSON_Delete(root);
    return true;
}

std::string TrafficLogPath()
{
    return GamePaths::Instance().UserDir() + "npcChat-traffic.log";
}

std::string SettingsPath()
{
    return GamePaths::Instance().UserDir() + "npcChat.json"; // UserDir ends with a slash
}

Settings& CurrentSettings()
{
    static Settings settings = []
    {
        Settings s;
        std::ifstream in(SettingsPath(), std::ios::binary);
        if (in)
        {
            std::stringstream buffer;
            buffer << in.rdbuf();
            SettingsFromJson(buffer.str(), s);
        }
        return s;
    }();
    return settings;
}

void SaveSettings()
{
    std::ofstream out(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (out)
        out << SettingsToJson(CurrentSettings());
}

} // namespace Poseidon::NpcChat
