#pragma once

// Experimental "talk to your squad" feature (dev panel -> NPC Chat).
//
// Settings are split in two on purpose:
//   * npcChat.json in the user profile directory: providers, models, prompt, hotkeys.
//     Never contains a key.
//   * API keys: held in memory for the session. Only when the player ticks "Remember keys"
//     are they written, and then to a per-user file OUTSIDE the profile directory
//     (%APPDATA%\OpenPoseidon\npc-chat\keys.dat, encrypted with Windows DPAPI so the file
//     is unreadable to any other Windows account or machine). See NpcChatSecrets.hpp.

#include <map>
#include <string>

namespace Poseidon::NpcChat
{
enum class LlmProvider : int
{
    OpenAI = 0,
    OpenRouter,
    Anthropic,
    Gemini,
    Ollama, // local, no key
    Custom, // any OpenAI-compatible /chat/completions endpoint
    Count
};

enum class SttProvider : int
{
    Off = 0,
    OpenAI,
    Groq,   // OpenAI-compatible Whisper, generous free tier
    Google, // Cloud Speech-to-Text v1 (API key)
    Custom, // OpenAI-compatible /audio/transcriptions (e.g. a local whisper.cpp server)
    ElevenLabs, // Scribe speech-to-text
    Gemini,     // AI Studio key: Gemini listens to the clip and writes it down (free tier)
    Count
};

enum class TtsProvider : int
{
    Off = 0,
    WindowsSapi, // built-in Windows voices: free, offline, no key
    OpenAI,
    Google, // Cloud Text-to-Speech v1 (API key)
    Custom, // OpenAI-compatible /audio/speech (e.g. a local Kokoro/Piper server)
    ElevenLabs,
    Gemini, // AI Studio key: Gemini's own speech models, free tier with small limits
    Count
};

const char* LlmProviderName(LlmProvider p);
const char* SttProviderName(SttProvider p);
const char* TtsProviderName(TtsProvider p);

/// Key slot used for a provider ("openai", "openrouter", "anthropic", "gemini", "google",
/// "groq", "custom-llm", ...). OpenAI's slot is shared by its LLM, STT and TTS.
const char* LlmKeySlot(LlmProvider p);
const char* SttKeySlot(SttProvider p);
const char* TtsKeySlot(TtsProvider p);

/// Environment variable checked when no key is set in the panel (e.g. OPENAI_API_KEY).
const char* KeySlotEnvVar(const std::string& slot);

std::string DefaultLlmModel(LlmProvider p);
std::string DefaultLlmBaseUrl(LlmProvider p);
std::string DefaultSttModel(SttProvider p);
std::string DefaultSttBaseUrl(SttProvider p);
std::string DefaultTtsModel(TtsProvider p);
std::string DefaultTtsBaseUrl(TtsProvider p);
std::string DefaultTtsVoices(TtsProvider p);
std::string DefaultSystemPrompt();

/// A one-line persona from the soldier's role ("Medic", "Machine Gunner"...), varied by squad slot so
/// a default squad is a mixed bunch rather than five copies of one man.
std::string DefaultPersona(int slot, const std::string& role);
/// Seconds between two spontaneous reactions for a chattiness level (0 = never).
double EventCooldownSeconds(int eventChatter);

constexpr int kToneCount = 4;
const char* ToneName(int tone);
/// Paragraph appended after the (editable) system prompt; separate so changing the tone never
/// overwrites a custom prompt.
std::string ToneInstructions(int tone);

struct Settings
{
    bool enabled = false;

    // Defaults: one free Google AI Studio key covers the model, the voices and voice input.
    LlmProvider llm = LlmProvider::Gemini;
    std::string llmModel;   // empty -> DefaultLlmModel
    std::string llmBaseUrl; // empty -> DefaultLlmBaseUrl
    float temperature = 0.7f;
    int maxTokens = 500;
    int historyTurns = 12; // user+assistant exchanges kept as conversation memory
    // Ollama's own default window (2-4k tokens) is smaller than one request here, and it silently
    // drops the START of the prompt -- the instructions. Sent as options.num_ctx on Ollama's native API.
    int localContextTokens = 8192;
    std::string systemPrompt;
    int tone = 1; // how casual the squad is with the player: 0 strict .. 3 buddies (ToneInstructions)

    bool includeBriefing = true;
    bool includeSquad = true;
    bool includeEnemies = true;
    bool includeEnvironment = true;
    int briefingMaxChars = 6000;
    bool allowOrders = true;

    SttProvider stt = SttProvider::Gemini;
    std::string sttModel;
    std::string sttBaseUrl;
    std::string sttLanguage = "en";

    TtsProvider tts = TtsProvider::Gemini; // falls back to Windows voices without a key
    std::string ttsModel;
    std::string ttsBaseUrl;
    std::string ttsVoices; // comma-separated pool; each squad member keeps one voice
    std::string ttsLanguage = "en-US";
    float ttsVolume = 1.0f;
    bool ttsRadioWhenFar = true; // beyond 40 m the reply plays as radio (2D) rather than 3D
    bool muteGameVoices = true; // silence the game's own radio speech ("Roger", "Form line") while NPC voices speak
    bool logTraffic = false;    // write every request/response to TrafficLogPath() (keys hidden)

    int chatKey = 73; // SDL_SCANCODE_INSERT: open the text box
    int talkKey = 24; // SDL_SCANCODE_U: hold to talk. Not Caps Lock: that is the game's own voice-over-net key
                      // (VoiceOverNetPushToTalk), and U is bound to nothing in play.

    int maxRequestsPerMinute = 12; // protects the player's API bill from a runaway loop
    int requestTimeoutSeconds = 90; // free models are often queued, and thinking models think first

    bool rememberKeys = false;

    // Optional radio call signs for squad slots ("2" -> "Hammer"). Addressing a call sign makes
    // that soldier answer; a squad call sign of "Papa Bear" takes the name away from HQ.
    std::map<int, std::string> callsigns;
    std::string CallsignFor(int slot) const;

    // Per-soldier voices ("2" -> "Orus"), valid for the text-to-speech provider they were picked
    // with (voice ids differ between providers). Empty -> the voice pool decides.
    std::map<int, std::string> squadVoices;
    TtsProvider squadVoicesProvider = TtsProvider::Off;
    std::string SquadVoiceFor(int slot) const;

    // Per-soldier persona ("2" -> "Gruff Texan, hates the cold"). Empty -> DefaultPersona(slot, role).
    std::map<int, std::string> personas;
    std::string PersonaFor(int slot, const std::string& role) const;

    // Spontaneous reactions to what happens (a squad mate hit or killed, a new contact, a kill):
    // 0 off, 1 rare, 2 normal, 3 chatty. Each reaction is one model request, so it has a cooldown.
    int eventChatter = 2;
    // The squad joker: -1 = automatic (the second squad mate), 0 = nobody, else a squad number.
    int jokerSlot = -1;

    std::string EffectiveLlmModel() const;
    std::string EffectiveLlmBaseUrl() const;
    std::string EffectiveSttModel() const;
    std::string EffectiveSttBaseUrl() const;
    std::string EffectiveTtsModel() const;
    std::string EffectiveTtsBaseUrl() const;
    std::string EffectiveTtsVoices() const;
    std::string EffectiveSystemPrompt() const;
};

/// JSON round trip (pure; unit-tested). Unknown fields are ignored, missing ones keep defaults.
std::string SettingsToJson(const Settings& s);
bool SettingsFromJson(const std::string& json, Settings& out);

/// Process-wide settings (loaded lazily from the profile directory).
Settings& CurrentSettings();
void SaveSettings();
std::string SettingsPath();
/// npcChat-traffic.log in the profile directory (the opt-in traffic log).
std::string TrafficLogPath();

} // namespace Poseidon::NpcChat
