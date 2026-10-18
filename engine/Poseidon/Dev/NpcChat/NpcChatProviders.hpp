#pragma once

// Request builders and response parsers for every supported LLM / STT / TTS provider.
// Pure functions (no I/O, no engine state) so the wire formats are unit-tested.

#include <Poseidon/Dev/NpcChat/NpcChatHttp.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon::NpcChat
{
struct ChatMessage
{
    std::string role; // "user" or "assistant"
    std::string content;
};

HttpRequest BuildLlmRequest(const Settings& settings, const std::string& key, const std::string& systemPrompt,
                            const std::vector<ChatMessage>& messages);
bool ParseLlmResponse(LlmProvider provider, const std::string& body, std::string& text, std::string& error);

HttpRequest BuildSttRequest(const Settings& settings, const std::string& key, const std::string& wav,
                            int sampleRate);
bool ParseSttResponse(SttProvider provider, const std::string& body, std::string& text, std::string& error);

/// `instructions` is a speaking-style hint (OpenAI gpt-4o-mini-tts only; ignored elsewhere).
HttpRequest BuildTtsRequest(const Settings& settings, const std::string& key, const std::string& text,
                            const std::string& voice, const std::string& instructions);
/// Produces WAV bytes.
bool ParseTtsResponse(TtsProvider provider, const HttpResponse& response, std::string& wav, std::string& error);

// ---- discovery: model lists, voice lists, key checks ----
struct ModelInfo
{
    std::string id;
    std::string name;
    bool free = false; // OpenRouter: zero prompt and completion price
    bool chat = true;  // false for moderation / safety classifiers and embedding models: they cannot talk
    int context = 0;
};
HttpRequest BuildModelListRequest(const Settings& settings, const std::string& key);
bool ParseModelList(LlmProvider provider, const std::string& body, std::vector<ModelInfo>& models,
                    std::string& error);
/// Content-safety classifiers ("...-content-safety", "llama-guard", "shieldgemma") and embedding
/// models answer with a verdict or a vector, never with dialogue.
bool IsChatModelId(const std::string& id);
/// Case-insensitive substring over id and name; "free" also matches models flagged free. Models that
/// cannot chat are left out unless the filter names them ("guard", "safety").
std::vector<ModelInfo> FilterModels(const std::vector<ModelInfo>& models, const std::string& filter);
/// OpenRouter's model list is public, so it proves nothing about the key: GET /key does.
bool BuildKeyCheckRequest(const Settings& settings, const std::string& key, HttpRequest& out);
std::string DescribeKeyCheck(const std::string& body);

struct VoiceInfo
{
    std::string id;     // what the API wants (ElevenLabs voice_id, Google voice name, ...)
    std::string name;   // what the player sees
    std::string detail; // gender / accent / language
};
/// False when the provider has no voice endpoint (use StaticVoices).
bool BuildVoiceListRequest(const Settings& settings, const std::string& key, HttpRequest& out);
bool ParseVoiceList(TtsProvider provider, const std::string& body, std::vector<VoiceInfo>& voices,
                    std::string& error);
std::vector<VoiceInfo> StaticVoices(TtsProvider provider);
/// From the provider's gender label (ElevenLabs, Google, Windows, the tables above) or a Kokoro
/// "am_"/"bm_" id. Unknown counts as not male: auto-assign never guesses.
bool IsMaleVoice(const VoiceInfo& voice);
/// A cheap authenticated call proving the TTS key works when there is no voice list (OpenAI).
bool BuildTtsKeyCheckRequest(const Settings& settings, const std::string& key, HttpRequest& out);
/// STT key check: a model list where one exists, else a 0.2 s silent transcription.
HttpRequest BuildSttCheckRequest(const Settings& settings, const std::string& key);

/// Speech models a provider offers, from its model-list answer (`body`; may be empty) filtered to the
/// ones that do this job, plus the fixed names of providers without a list. Pure; unit-tested.
std::vector<ModelInfo> SttModelChoices(SttProvider provider, const std::string& body);
std::vector<ModelInfo> TtsModelChoices(TtsProvider provider, const std::string& body);
/// The model to switch to when `current` is not offered: the provider's sensible default kind
/// (Gemini: a "flash-lite", then "flash" model; TTS: a "flash-tts" one). Empty when none fits.
std::string PickSpeechModel(const std::vector<ModelInfo>& offered, const std::string& current, bool forTts);
/// ElevenLabs lists its models at /v1/models as a bare array: a separate GET for the TTS list.
bool BuildTtsModelListRequest(const Settings& settings, const std::string& key, HttpRequest& out);

// ---- helpers (also used by the audio path) ----
std::string Base64Encode(const std::string& data);
std::string Base64Decode(const std::string& text);
/// 16-bit mono PCM -> RIFF/WAVE.
std::string MakeWav(const int16_t* samples, size_t count, int sampleRate);
/// Band-limit 16-bit PCM WAV to a 300-3000 Hz radio voice with light clipping. Returns the input
/// unchanged when it is not 16-bit PCM.
std::string ApplyRadioFilter(const std::string& wav);
/// Join a base URL and a path with exactly one slash.
std::string JoinUrl(const std::string& base, const std::string& path);
/// "http://host:11434/v1" -> "http://host:11434": the root of Ollama's native API.
std::string OllamaNativeBase(const std::string& base);
/// Split "a, b ,c" -> {"a","b","c"}.
std::vector<std::string> SplitList(const std::string& list);
} // namespace Poseidon::NpcChat
