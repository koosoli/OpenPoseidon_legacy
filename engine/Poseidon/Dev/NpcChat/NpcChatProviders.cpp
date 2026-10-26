// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Poseidon::NpcChat
{
namespace
{
std::string Print(cJSON* root)
{
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

const char* StringField(const cJSON* obj, const char* name)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

/// Gemini generateContent: {"candidates":[{"content":{"parts":[...]}}]} -> the parts array.
const cJSON* GeminiParts(const cJSON* root)
{
    const cJSON* candidate = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "candidates"), 0);
    const cJSON* content = cJSON_GetObjectItemCaseSensitive(candidate, "content");
    return cJSON_GetObjectItemCaseSensitive(content, "parts");
}

std::string GeminiUrl(const std::string& base, const std::string& model)
{
    return JoinUrl(base, "models/" + model + ":generateContent");
}

bool IsOpenAiCompatible(LlmProvider p)
{
    return p != LlmProvider::Anthropic;
}
} // namespace

std::string JoinUrl(const std::string& base, const std::string& path)
{
    if (base.empty())
        return path;
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    size_t start = 0;
    while (start < path.size() && path[start] == '/')
        ++start;
    return b + "/" + path.substr(start);
}

std::string OllamaNativeBase(const std::string& base)
{
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    if (b.size() >= 3 && b.compare(b.size() - 3, 3, "/v1") == 0)
        b.resize(b.size() - 3);
    return b;
}

std::vector<std::string> SplitList(const std::string& list)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos)
            comma = list.size();
        std::string item = list.substr(pos, comma - pos);
        size_t b = item.find_first_not_of(" \t\r\n");
        size_t e = item.find_last_not_of(" \t\r\n");
        if (b != std::string::npos)
            out.push_back(item.substr(b, e - b + 1));
        pos = comma + 1;
    }
    return out;
}

// ---------------------------------------------------------------- LLM

HttpRequest BuildLlmRequest(const Settings& s, const std::string& key, const std::string& systemPrompt,
                            const std::vector<ChatMessage>& messages)
{
    HttpRequest req;
    req.timeoutSeconds = s.requestTimeoutSeconds;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", s.EffectiveLlmModel().c_str());
    if (s.llm == LlmProvider::Ollama)
    {
        // Ollama's native /api/chat, not its OpenAI layer: only here can the context window be set
        // (options.num_ctx). With the default 2-4k window Ollama silently cuts the START of a request
        // this size, which is the instructions. format=json constrains the output to a JSON object.
        req.url = JoinUrl(OllamaNativeBase(s.EffectiveLlmBaseUrl()), "api/chat");
        if (!key.empty()) // an Ollama behind an authenticating proxy
            req.headers.emplace_back("Authorization", "Bearer " + key);
        cJSON_AddBoolToObject(root, "stream", false);
        cJSON_AddStringToObject(root, "format", "json");
        cJSON_AddStringToObject(root, "keep_alive", "30m"); // no reload between radio calls
        cJSON* options = cJSON_AddObjectToObject(root, "options");
        cJSON_AddNumberToObject(options, "num_ctx", std::clamp(s.localContextTokens, 2048, 131072));
        cJSON_AddNumberToObject(options, "num_predict", s.maxTokens + 2000);
        cJSON_AddNumberToObject(options, "temperature", s.temperature);
        cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
        cJSON* sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", systemPrompt.c_str());
        cJSON_AddItemToArray(msgs, sys);
        for (const ChatMessage& m : messages)
        {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "role", m.role.c_str());
            cJSON_AddStringToObject(item, "content", m.content.c_str());
            cJSON_AddItemToArray(msgs, item);
        }
        req.body = Print(root);
        return req;
    }
    // Thinking models (Gemini 2.5+/3, Nemotron, o-series...) count their hidden reasoning against
    // max_tokens: without headroom the budget goes on thinking and the reply is cut off -- seen as an
    // empty reply, or JSON that stops before its "orders". Anthropic does not think unless asked.
    cJSON_AddNumberToObject(root, "max_tokens", s.llm == LlmProvider::Anthropic ? s.maxTokens : s.maxTokens + 2000);
    cJSON_AddNumberToObject(root, "temperature", s.temperature);

    if (IsOpenAiCompatible(s.llm))
    {
        req.url = JoinUrl(s.EffectiveLlmBaseUrl(), "chat/completions");
        if (!key.empty())
            req.headers.emplace_back("Authorization", "Bearer " + key);
        if (s.llm == LlmProvider::OpenRouter)
        {
            // OpenRouter's optional attribution headers.
            req.headers.emplace_back("HTTP-Referer", "https://github.com/ofpisnotdead-com");
            req.headers.emplace_back("X-Title", "OpenPoseidon NPC chat");
            // A soldier's radio reply needs no long chain of thought: thinking models otherwise reason
            // for a minute before the first word. Models without reasoning ignore this.
            cJSON* reasoning = cJSON_AddObjectToObject(root, "reasoning");
            cJSON_AddStringToObject(reasoning, "effort", "low");
            cJSON_AddBoolToObject(reasoning, "exclude", true);
        }
        cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
        cJSON* sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", systemPrompt.c_str());
        cJSON_AddItemToArray(msgs, sys);
        for (const ChatMessage& m : messages)
        {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "role", m.role.c_str());
            cJSON_AddStringToObject(item, "content", m.content.c_str());
            cJSON_AddItemToArray(msgs, item);
        }
        // Ask for a JSON object where the API understands it; the prompt asks for it anyway, and
        // ParseReply copes with prose around the object for the servers that ignore this.
        if (s.llm == LlmProvider::OpenAI || s.llm == LlmProvider::Gemini)
        {
            cJSON* fmt = cJSON_AddObjectToObject(root, "response_format");
            cJSON_AddStringToObject(fmt, "type", "json_object");
        }
        // A radio reply needs little thought: Gemini's OpenAI layer maps this to its thinking level.
        if (s.llm == LlmProvider::Gemini)
            cJSON_AddStringToObject(root, "reasoning_effort", "low");
    }
    else
    {
        req.url = JoinUrl(s.EffectiveLlmBaseUrl(), "messages");
        req.headers.emplace_back("x-api-key", key);
        req.headers.emplace_back("anthropic-version", "2023-06-01");
        cJSON_AddStringToObject(root, "system", systemPrompt.c_str());
        cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
        // Anthropic requires the first message to be from the user and roles to alternate.
        std::string lastRole;
        for (const ChatMessage& m : messages)
        {
            if (lastRole.empty() && m.role != "user")
                continue;
            if (m.role == lastRole)
            {
                cJSON* last = cJSON_GetArrayItem(msgs, cJSON_GetArraySize(msgs) - 1);
                cJSON* content = cJSON_GetObjectItemCaseSensitive(last, "content");
                const std::string merged = std::string(content->valuestring) + "\n" + m.content;
                cJSON_SetValuestring(content, merged.c_str());
                continue;
            }
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "role", m.role.c_str());
            cJSON_AddStringToObject(item, "content", m.content.c_str());
            cJSON_AddItemToArray(msgs, item);
            lastRole = m.role;
        }
    }
    req.body = Print(root);
    return req;
}

bool ParseLlmResponse(LlmProvider provider, const std::string& body, std::string& text, std::string& error)
{
    text.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root)
    {
        error = "LLM answer is not JSON";
        return false;
    }
    bool ok = false;
    const cJSON* native = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (provider == LlmProvider::Ollama && cJSON_IsObject(native))
    {
        // Ollama /api/chat: {"message":{"content":..},"done_reason":"stop"}
        if (const char* t = StringField(native, "content"))
            text = t;
        ok = text.find_first_not_of(" \t\r\n") != std::string::npos;
        if (!ok)
        {
            const char* reason = StringField(root, "done_reason");
            error = reason && std::string(reason) == "length"
                        ? "the local model used its whole token budget and wrote no reply. Raise \"Max answer "
                          "tokens\" or pick a model without \"thinking\"."
                        : "the local model returned an empty reply. Try again or pick another model.";
            cJSON_Delete(root);
            return false;
        }
    }
    else if (IsOpenAiCompatible(provider))
    {
        const cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        const cJSON* first = cJSON_GetArrayItem(choices, 0);
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(first, "message");
        const cJSON* content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (cJSON_IsString(content) && content->valuestring)
        {
            text = content->valuestring;
        }
        else if (cJSON_IsArray(content)) // some providers send content as parts
        {
            const cJSON* part = nullptr;
            cJSON_ArrayForEach(part, content)
                if (const char* t = StringField(part, "text"))
                    text += t;
        }
        ok = text.find_first_not_of(" \t\r\n") != std::string::npos;
        if (!ok && message)
        {
            // Say WHY it is empty: the usual case is a thinking model that spent its budget thinking.
            const char* finish = StringField(first, "finish_reason");
            const bool thought = cJSON_GetObjectItemCaseSensitive(message, "reasoning") != nullptr ||
                                 cJSON_GetObjectItemCaseSensitive(message, "reasoning_details") != nullptr;
            if (finish && std::string(finish) == "length")
                error = "the model used its whole token budget" + std::string(thought ? " thinking" : "") +
                        " and wrote no reply. Raise \"Max answer tokens\" or pick a model without \"thinking\".";
            else
                error = std::string("the model returned an empty reply") + (finish ? " (finish_reason: " + std::string(finish) + ")" : "") +
                        ". Free models sometimes do this when overloaded: try again or pick another model.";
            cJSON_Delete(root);
            return false;
        }
    }
    else
    {
        const cJSON* content = cJSON_GetObjectItemCaseSensitive(root, "content");
        const cJSON* block = nullptr;
        cJSON_ArrayForEach(block, content)
        {
            const char* type = StringField(block, "type");
            const char* t = StringField(block, "text");
            if (type && std::strcmp(type, "text") == 0 && t)
            {
                text += t;
                ok = true;
            }
        }
    }
    if (!ok)
    {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char* msg = cJSON_IsString(err) ? err->valuestring : StringField(err, "message"); // Ollama: a string
        error = msg ? msg : "LLM answer has no text";
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------- STT

HttpRequest BuildSttRequest(const Settings& s, const std::string& key, const std::string& wav, int sampleRate)
{
    HttpRequest req;
    req.timeoutSeconds = s.requestTimeoutSeconds;
    if (s.stt == SttProvider::Google)
    {
        req.url = JoinUrl(s.EffectiveSttBaseUrl(), "speech:recognize");
        req.headers.emplace_back("X-Goog-Api-Key", key); // header, so the key never sits in a URL
        cJSON* root = cJSON_CreateObject();
        cJSON* config = cJSON_AddObjectToObject(root, "config");
        cJSON_AddStringToObject(config, "encoding", "LINEAR16");
        cJSON_AddNumberToObject(config, "sampleRateHertz", sampleRate);
        std::string lang = s.sttLanguage.empty() ? "en-US" : s.sttLanguage;
        if (lang.size() == 2)
            lang = lang == "en" ? "en-US" : lang + "-" + lang; // "de" -> "de-DE" style guess
        for (size_t i = 3; i < lang.size(); ++i)
            lang[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(lang[i])));
        cJSON_AddStringToObject(config, "languageCode", lang.c_str());
        const std::string model = s.EffectiveSttModel();
        if (!model.empty())
            cJSON_AddStringToObject(config, "model", model.c_str());
        cJSON_AddBoolToObject(config, "enableAutomaticPunctuation", true);
        cJSON* audio = cJSON_AddObjectToObject(root, "audio");
        // Google wants raw LINEAR16 without the RIFF header.
        const std::string pcm = wav.size() > 44 ? wav.substr(44) : std::string();
        cJSON_AddStringToObject(audio, "content", Base64Encode(pcm).c_str());
        req.body = Print(root);
        return req;
    }

    if (s.stt == SttProvider::Gemini)
    {
        // Gemini hears the clip and writes it down; the prompt keeps it to the words alone.
        req.url = GeminiUrl(s.EffectiveSttBaseUrl(), s.EffectiveSttModel());
        req.headers.emplace_back("x-goog-api-key", key);
        cJSON* root = cJSON_CreateObject();
        cJSON* contents = cJSON_AddArrayToObject(root, "contents");
        cJSON* turn = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, turn);
        cJSON* parts = cJSON_AddArrayToObject(turn, "parts");
        cJSON* instruction = cJSON_CreateObject();
        cJSON_AddStringToObject(instruction, "text",
                                "Transcribe this radio message from a soldier exactly as spoken. Military terms: "
                                "tangos, contact, Papa Bear, HQ, roger, wilco. Reply with the transcript only, no "
                                "quotes or commentary. If nothing is said, reply with nothing.");
        cJSON_AddItemToArray(parts, instruction);
        cJSON* audio = cJSON_CreateObject();
        cJSON* inline_ = cJSON_AddObjectToObject(audio, "inlineData");
        cJSON_AddStringToObject(inline_, "mimeType", "audio/wav");
        cJSON_AddStringToObject(inline_, "data", Base64Encode(wav).c_str());
        cJSON_AddItemToArray(parts, audio);
        cJSON* config = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON_AddNumberToObject(config, "temperature", 0);
        req.body = Print(root);
        return req;
    }
    if (s.stt == SttProvider::ElevenLabs)
    {
        req.url = JoinUrl(s.EffectiveSttBaseUrl(), "speech-to-text");
        req.headers.emplace_back("xi-api-key", key);
        req.parts.push_back({"file", "speech.wav", "audio/wav", wav});
        req.parts.push_back({"model_id", "", "", s.EffectiveSttModel()});
        if (!s.sttLanguage.empty())
            req.parts.push_back({"language_code", "", "", s.sttLanguage.substr(0, 2)});
        return req;
    }

    req.url = JoinUrl(s.EffectiveSttBaseUrl(), "audio/transcriptions");
    if (!key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + key);
    req.parts.push_back({"file", "speech.wav", "audio/wav", wav});
    req.parts.push_back({"model", "", "", s.EffectiveSttModel()});
    if (!s.sttLanguage.empty())
        req.parts.push_back({"language", "", "", s.sttLanguage.substr(0, 2)});
    req.parts.push_back({"response_format", "", "", "json"});
    // Military vocabulary hint: Whisper otherwise writes "tangos" as "tango's" and callsigns badly.
    req.parts.push_back({"prompt", "", "",
                         "Squad radio. Roger, wilco, over, out. Tangos, contact, enemy, Papa Bear, Command, HQ, "
                         "two, three, four, follow me, hold position, cover me, regroup, BMP, T-72, M113."});
    return req;
}

bool ParseSttResponse(SttProvider provider, const std::string& body, std::string& text, std::string& error)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root)
    {
        error = "transcription answer is not JSON";
        return false;
    }
    text.clear();
    if (provider == SttProvider::Google)
    {
        const cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
        const cJSON* result = nullptr;
        cJSON_ArrayForEach(result, results)
        {
            const cJSON* alt = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "alternatives"), 0);
            if (const char* t = StringField(alt, "transcript"))
            {
                if (!text.empty())
                    text += " ";
                text += t;
            }
        }
    }
    else if (provider == SttProvider::Gemini)
    {
        const cJSON* part = nullptr;
        cJSON_ArrayForEach(part, GeminiParts(root))
            if (const char* t = StringField(part, "text"))
                text += t;
    }
    else if (const char* t = StringField(root, "text"))
    {
        text = t;
    }
    cJSON_Delete(root);
    size_t b = text.find_first_not_of(" \t\r\n");
    size_t e = text.find_last_not_of(" \t\r\n");
    text = b == std::string::npos ? std::string() : text.substr(b, e - b + 1);
    if (text.empty())
        error = "nothing was understood";
    return !text.empty();
}

// ---------------------------------------------------------------- TTS

HttpRequest BuildTtsRequest(const Settings& s, const std::string& key, const std::string& text,
                            const std::string& voice, const std::string& instructions)
{
    HttpRequest req;
    req.timeoutSeconds = s.requestTimeoutSeconds;
    cJSON* root = cJSON_CreateObject();
    if (s.tts == TtsProvider::Google)
    {
        req.url = JoinUrl(s.EffectiveTtsBaseUrl(), "text:synthesize");
        req.headers.emplace_back("X-Goog-Api-Key", key);
        cJSON* input = cJSON_AddObjectToObject(root, "input");
        cJSON_AddStringToObject(input, "text", text.c_str());
        cJSON* v = cJSON_AddObjectToObject(root, "voice");
        // The language code is the voice name's prefix ("en-GB-Standard-B" -> "en-GB").
        std::string lang = s.ttsLanguage.empty() ? "en-US" : s.ttsLanguage;
        if (voice.size() > 5 && voice[2] == '-' && voice[5] == '-')
            lang = voice.substr(0, 5);
        cJSON_AddStringToObject(v, "languageCode", lang.c_str());
        if (!voice.empty())
            cJSON_AddStringToObject(v, "name", voice.c_str());
        cJSON* cfg = cJSON_AddObjectToObject(root, "audioConfig");
        cJSON_AddStringToObject(cfg, "audioEncoding", "LINEAR16");
        cJSON_AddNumberToObject(cfg, "sampleRateHertz", 24000);
    }
    else if (s.tts == TtsProvider::Gemini)
    {
        // Gemini speech: the delivery is steered in words, then the line itself.
        req.url = GeminiUrl(s.EffectiveTtsBaseUrl(), s.EffectiveTtsModel());
        req.headers.emplace_back("x-goog-api-key", key);
        cJSON* contents = cJSON_AddArrayToObject(root, "contents");
        cJSON* turn = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, turn);
        cJSON* parts = cJSON_AddArrayToObject(turn, "parts");
        cJSON* part = cJSON_CreateObject();
        // Only the line itself: Gemini's voice model reads a style hint aloud along with the text.
        (void)instructions;
        const std::string& spoken = text;
        cJSON_AddStringToObject(part, "text", spoken.c_str());
        cJSON_AddItemToArray(parts, part);
        cJSON* config = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON* modalities = cJSON_AddArrayToObject(config, "responseModalities");
        cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
        cJSON* speech = cJSON_AddObjectToObject(config, "speechConfig");
        cJSON* voiceConfig = cJSON_AddObjectToObject(speech, "voiceConfig");
        cJSON* prebuilt = cJSON_AddObjectToObject(voiceConfig, "prebuiltVoiceConfig");
        cJSON_AddStringToObject(prebuilt, "voiceName", voice.empty() ? "Charon" : voice.c_str());
    }
    else if (s.tts == TtsProvider::ElevenLabs)
    {
        // Raw PCM: no decoder needed; ParseTtsResponse puts the WAV header on.
        req.url = JoinUrl(s.EffectiveTtsBaseUrl(), "text-to-speech/" + (voice.empty() ? std::string("pNInz6obpgDQGcFmaJgB") : voice)) +
                  "?output_format=pcm_22050";
        req.headers.emplace_back("xi-api-key", key);
        cJSON_AddStringToObject(root, "text", text.c_str());
        cJSON_AddStringToObject(root, "model_id", s.EffectiveTtsModel().c_str());
    }
    else
    {
        req.url = JoinUrl(s.EffectiveTtsBaseUrl(), "audio/speech");
        if (!key.empty())
            req.headers.emplace_back("Authorization", "Bearer " + key);
        cJSON_AddStringToObject(root, "model", s.EffectiveTtsModel().c_str());
        cJSON_AddStringToObject(root, "voice", voice.empty() ? "onyx" : voice.c_str());
        cJSON_AddStringToObject(root, "input", text.c_str());
        cJSON_AddStringToObject(root, "response_format", "wav");
        if (s.tts == TtsProvider::OpenAI && !instructions.empty())
            cJSON_AddStringToObject(root, "instructions", instructions.c_str());
    }
    req.body = Print(root);
    return req;
}

bool ParseTtsResponse(TtsProvider provider, const HttpResponse& response, std::string& wav, std::string& error)
{
    if (provider == TtsProvider::Google)
    {
        cJSON* root = cJSON_Parse(response.body.c_str());
        const char* content = StringField(root, "audioContent");
        if (content)
            wav = Base64Decode(content); // LINEAR16 comes back with its WAV header
        cJSON_Delete(root);
    }
    else if (provider == TtsProvider::Gemini)
    {
        // inlineData: base64 16-bit mono PCM, "audio/L16;codec=pcm;rate=24000".
        cJSON* root = cJSON_Parse(response.body.c_str());
        const cJSON* part = nullptr;
        cJSON_ArrayForEach(part, GeminiParts(root))
        {
            const cJSON* data = cJSON_GetObjectItemCaseSensitive(part, "inlineData");
            const char* b64 = StringField(data, "data");
            if (!b64)
                continue;
            int rate = 24000;
            if (const char* mime = StringField(data, "mimeType"))
                if (const char* r = std::strstr(mime, "rate="))
                    rate = std::max(8000, std::atoi(r + 5));
            const std::string pcm = Base64Decode(b64);
            wav = MakeWav(reinterpret_cast<const int16_t*>(pcm.data()), pcm.size() / 2, rate);
            break;
        }
        if (wav.empty())
        {
            const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (const char* msg = StringField(err, "message"))
                error = msg;
        }
        cJSON_Delete(root);
    }
    else if (provider == TtsProvider::ElevenLabs)
    {
        if (response.body.size() >= 2 && response.body[0] != '{')
            wav = MakeWav(reinterpret_cast<const int16_t*>(response.body.data()), response.body.size() / 2, 22050);
    }
    else
    {
        wav = response.body;
    }
    if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0)
    {
        error = "speech answer is not a WAV file";
        wav.clear();
        return false;
    }
    // Streaming encoders (OpenAI) write 0xFFFFFFFF for the RIFF and data sizes; loaders that trust
    // the header then read past the end. Rewrite both from the real length.
    auto put32 = [&](size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            wav[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    auto get32 = [&](size_t at)
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<unsigned char>(wav[at + i])) << (8 * i);
        return v;
    };
    put32(4, static_cast<uint32_t>(wav.size() - 8));
    for (size_t at = 12; at + 8 <= wav.size();)
    {
        const uint32_t size = get32(at + 4);
        if (wav.compare(at, 4, "data") == 0)
        {
            const size_t real = wav.size() - (at + 8);
            if (size > real)
                put32(at + 4, static_cast<uint32_t>(real & ~size_t(1)));
            break;
        }
        if (size > wav.size())
            break;
        at += 8 + size + (size & 1);
    }
    return true;
}

// ---------------------------------------------------------------- discovery

namespace
{
std::string LowerAscii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void AddLlmAuth(const Settings& s, const std::string& key, HttpRequest& req)
{
    if (s.llm == LlmProvider::Anthropic)
    {
        req.headers.emplace_back("x-api-key", key);
        req.headers.emplace_back("anthropic-version", "2023-06-01");
    }
    else if (!key.empty())
    {
        req.headers.emplace_back("Authorization", "Bearer " + key);
    }
}
} // namespace

HttpRequest BuildModelListRequest(const Settings& s, const std::string& key)
{
    HttpRequest req;
    req.get = true;
    req.timeoutSeconds = 20;
    req.url = JoinUrl(s.EffectiveLlmBaseUrl(), s.llm == LlmProvider::Anthropic ? "models?limit=1000" : "models");
    AddLlmAuth(s, key, req);
    return req;
}

bool ParseModelList(LlmProvider provider, const std::string& body, std::vector<ModelInfo>& models,
                    std::string& error)
{
    models.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data))
        data = cJSON_GetObjectItemCaseSensitive(root, "models"); // some local servers
    if (!cJSON_IsArray(data))
    {
        error = "the model list answer has no \"data\" array";
        cJSON_Delete(root);
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, data)
    {
        ModelInfo m;
        if (const char* id = StringField(item, "id"))
            m.id = id;
        else if (const char* name = StringField(item, "name"))
            m.id = name;
        if (m.id.rfind("models/", 0) == 0) // Gemini's OpenAI layer prefixes ids
            m.id.erase(0, 7);
        if (m.id.empty())
            continue;
        if (const char* n = StringField(item, "name"))
            m.name = n;
        else if (const char* d = StringField(item, "display_name"))
            m.name = d;
        if (const cJSON* ctx = cJSON_GetObjectItemCaseSensitive(item, "context_length"); cJSON_IsNumber(ctx))
            m.context = ctx->valueint;
        if (const cJSON* pricing = cJSON_GetObjectItemCaseSensitive(item, "pricing"); cJSON_IsObject(pricing))
        {
            const char* p = StringField(pricing, "prompt");
            const char* c = StringField(pricing, "completion");
            m.free = p && c && std::strtod(p, nullptr) == 0.0 && std::strtod(c, nullptr) == 0.0;
        }
        if (m.id.size() > 5 && m.id.compare(m.id.size() - 5, 5, ":free") == 0)
            m.free = true;
        m.chat = IsChatModelId(m.id);
        models.push_back(std::move(m));
    }
    cJSON_Delete(root);
    std::sort(models.begin(), models.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
    (void)provider;
    if (models.empty())
        error = "the provider returned no models";
    return !models.empty();
}

bool IsChatModelId(const std::string& id)
{
    const std::string l = LowerAscii(id);
    // "-live" / "native-audio" / "realtime": streaming voice models, reachable only over a WebSocket
    // (Gemini's bidiGenerateContent): a chat request to one is refused (HTTP 400) every time.
    for (const char* not_chat : {"safety", "guard", "shield", "moderation", "embed", "rerank", "whisper", "tts",
                                 "transcribe", "image", "imagen", "dall-e", "veo", "lyria", "-live", "live-",
                                 "native-audio", "realtime", "audio-preview"})
        if (l.find(not_chat) != std::string::npos)
            return false;
    return true;
}

std::vector<ModelInfo> FilterModels(const std::vector<ModelInfo>& models, const std::string& filter)
{
    std::string f = LowerAscii(filter);
    while (!f.empty() && f.back() == ' ')
        f.pop_back();
    while (!f.empty() && f.front() == ' ')
        f.erase(0, 1);
    if (f.empty())
    {
        std::vector<ModelInfo> chat;
        for (const ModelInfo& m : models)
            if (m.chat)
                chat.push_back(m);
        return chat;
    }
    // Every space-separated word must match ("free llama").
    std::vector<std::string> words;
    for (size_t pos = 0; pos < f.size();)
    {
        const size_t end = f.find(' ', pos);
        const std::string w = f.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!w.empty())
            words.push_back(w);
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    std::vector<ModelInfo> out;
    const bool askedForNonChat = f.find("guard") != std::string::npos || f.find("safety") != std::string::npos;
    for (const ModelInfo& m : models)
    {
        if (!m.chat && !askedForNonChat)
            continue;
        const std::string hay = LowerAscii(m.id + " " + m.name);
        bool all = true;
        for (const std::string& w : words)
            if (hay.find(w) == std::string::npos && !(w == "free" && m.free))
                all = false;
        if (all)
            out.push_back(m);
    }
    return out;
}

bool BuildKeyCheckRequest(const Settings& s, const std::string& key, HttpRequest& out)
{
    if (s.llm != LlmProvider::OpenRouter)
        return false;
    out = HttpRequest{};
    out.get = true;
    out.timeoutSeconds = 20;
    out.url = JoinUrl(s.EffectiveLlmBaseUrl(), "key");
    AddLlmAuth(s, key, out);
    return true;
}

std::string DescribeKeyCheck(const std::string& body)
{
    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    std::string out = "key accepted";
    if (cJSON_IsObject(data))
    {
        if (const cJSON* freeTier = cJSON_GetObjectItemCaseSensitive(data, "is_free_tier"); cJSON_IsTrue(freeTier))
            out += ", free tier";
        const cJSON* limit = cJSON_GetObjectItemCaseSensitive(data, "limit_remaining");
        if (cJSON_IsNumber(limit))
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), ", $%.2f credit left", limit->valuedouble);
            out += buf;
        }
    }
    cJSON_Delete(root);
    return out;
}

bool BuildVoiceListRequest(const Settings& s, const std::string& key, HttpRequest& out)
{
    out = HttpRequest{};
    out.get = true;
    out.timeoutSeconds = 20;
    switch (s.tts)
    {
        case TtsProvider::Google:
            out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "voices?languageCode=" +
                                                           (s.ttsLanguage.empty() ? std::string("en") : s.ttsLanguage.substr(0, 2)));
            out.headers.emplace_back("X-Goog-Api-Key", key);
            return true;
        case TtsProvider::ElevenLabs:
            out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "voices");
            out.headers.emplace_back("xi-api-key", key);
            return true;
        case TtsProvider::Custom:
            out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "audio/voices"); // Kokoro-FastAPI and friends
            if (!key.empty())
                out.headers.emplace_back("Authorization", "Bearer " + key);
            return true;
        default: return false;
    }
}

bool ParseVoiceList(TtsProvider provider, const std::string& body, std::vector<VoiceInfo>& voices,
                    std::string& error)
{
    voices.clear();
    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "voices");
    if (!cJSON_IsArray(list))
    {
        error = "the voice list answer has no \"voices\" array";
        cJSON_Delete(root);
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list)
    {
        VoiceInfo v;
        if (cJSON_IsString(item) && item->valuestring)
        {
            v.id = v.name = item->valuestring;
        }
        else if (provider == TtsProvider::ElevenLabs)
        {
            if (const char* id = StringField(item, "voice_id"))
                v.id = id;
            if (const char* n = StringField(item, "name"))
                v.name = n;
            if (const char* category = StringField(item, "category"); category && category[0])
                v.detail = category;
            if (const cJSON* labels = cJSON_GetObjectItemCaseSensitive(item, "labels"); cJSON_IsObject(labels))
            {
                for (const char* field : {"gender", "age", "accent", "description", "use_case"})
                    if (const char* l = StringField(labels, field); l && l[0])
                        v.detail += (v.detail.empty() ? "" : ", ") + std::string(l);
            }
        }
        else
        {
            if (const char* n = StringField(item, "name"))
                v.id = v.name = n;
            else if (const char* id = StringField(item, "id"))
                v.id = v.name = id;
            if (const char* g = StringField(item, "ssmlGender"))
                v.detail = LowerAscii(g);
            if (const cJSON* langs = cJSON_GetObjectItemCaseSensitive(item, "languageCodes"); cJSON_IsArray(langs))
                if (const cJSON* l = cJSON_GetArrayItem(langs, 0); cJSON_IsString(l))
                    v.detail += (v.detail.empty() ? "" : ", ") + std::string(l->valuestring);
        }
        if (!v.id.empty())
            voices.push_back(std::move(v));
    }
    cJSON_Delete(root);
    std::sort(voices.begin(), voices.end(), [](const VoiceInfo& a, const VoiceInfo& b) { return a.name < b.name; });
    if (voices.empty())
        error = "the provider returned no voices";
    return !voices.empty();
}

std::vector<VoiceInfo> StaticVoices(TtsProvider provider)
{
    std::vector<VoiceInfo> out;
    if (provider == TtsProvider::Gemini)
    {
        // Google's prebuilt voices with their one-word character. Google publishes no gender through
        // the API; this is the commonly listed one (male / female), "" where unknown (never auto-assigned).
        static const struct
        {
            const char* name;
            const char* gender;
            const char* character;
        } gemini[] = {
            {"Achernar", "female", "soft"},       {"Achird", "male", "friendly"},
            {"Algenib", "male", "gravelly"},      {"Algieba", "male", "smooth"},
            {"Alnilam", "male", "firm"},          {"Aoede", "female", "breezy"},
            {"Autonoe", "female", "bright"},      {"Callirrhoe", "female", "easy-going"},
            {"Charon", "male", "informative"},    {"Despina", "female", "smooth"},
            {"Enceladus", "male", "breathy"},     {"Erinome", "female", "clear"},
            {"Fenrir", "male", "excitable"},      {"Fola", "", "new voice"},
            {"Gacrux", "female", "mature"},       {"Iapetus", "male", "clear"},
            {"Kore", "female", "firm"},           {"Laomedeia", "female", "upbeat"},
            {"Leda", "female", "youthful"},       {"Orus", "male", "firm"},
            {"Puck", "male", "upbeat"},           {"Pulcherrima", "female", "forward"},
            {"Rasalgethi", "male", "informative"}, {"Sadachbia", "male", "lively"},
            {"Sadaltager", "male", "knowledgeable"}, {"Schedar", "male", "even"},
            {"Sulafat", "female", "warm"},        {"Umbriel", "male", "easy-going"},
            {"Vindemiatrix", "female", "gentle"}, {"Zephyr", "female", "bright"},
            {"Zubenelgenubi", "male", "casual"}};
        for (const auto& v : gemini)
            out.push_back({v.name, v.name, v.gender[0] ? std::string(v.gender) + ", " + v.character : v.character});
        return out;
    }
    if (provider == TtsProvider::OpenAI)
    {
        static const std::pair<const char*, const char*> openai[] = {
            {"alloy", "neutral"}, {"ash", "male"},      {"ballad", "male"},  {"coral", "female"},
            {"echo", "male"},     {"fable", "male"},    {"nova", "female"},  {"onyx", "male, deep"},
            {"sage", "female"},   {"shimmer", "female"}, {"verse", "male"}};
        for (const auto& [name, detail] : openai)
            out.push_back({name, name, detail});
    }
    return out;
}

bool IsMaleVoice(const VoiceInfo& voice)
{
    const std::string detail = LowerAscii(voice.detail);
    if (detail.find("female") != std::string::npos)
        return false;
    if (detail.find("male") != std::string::npos)
        return true;
    // Kokoro-style ids: a/b (American / British) then m/f ("am_adam", "bf_emma").
    const std::string id = LowerAscii(voice.id);
    return id.size() > 3 && id[2] == '_' && (id[0] == 'a' || id[0] == 'b') && id[1] == 'm';
}

bool BuildTtsKeyCheckRequest(const Settings& s, const std::string& key, HttpRequest& out)
{
    if (s.tts == TtsProvider::Gemini)
    {
        // The model list proves the key; the voice model must be on it.
        out = HttpRequest{};
        out.get = true;
        out.timeoutSeconds = 20;
        out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "models?pageSize=1000");
        out.headers.emplace_back("x-goog-api-key", key);
        return true;
    }
    if (s.tts != TtsProvider::OpenAI)
        return false;
    out = HttpRequest{};
    out.get = true;
    out.timeoutSeconds = 20;
    out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "models");
    out.headers.emplace_back("Authorization", "Bearer " + key);
    return true;
}

HttpRequest BuildSttCheckRequest(const Settings& s, const std::string& key)
{
    HttpRequest req;
    req.timeoutSeconds = 20;
    if (s.stt == SttProvider::OpenAI || s.stt == SttProvider::Groq)
    {
        req.get = true;
        req.url = JoinUrl(s.EffectiveSttBaseUrl(), "models");
        req.headers.emplace_back("Authorization", "Bearer " + key);
        return req;
    }
    if (s.stt == SttProvider::ElevenLabs)
    {
        req.get = true;
        req.url = JoinUrl(s.EffectiveSttBaseUrl(), "models");
        req.headers.emplace_back("xi-api-key", key);
        return req;
    }
    if (s.stt == SttProvider::Gemini)
    {
        req.get = true;
        req.url = JoinUrl(s.EffectiveSttBaseUrl(), "models?pageSize=1000");
        req.headers.emplace_back("x-goog-api-key", key);
        return req;
    }
    // Google and local servers have no cheap listing: transcribe 0.2 s of silence.
    const std::vector<int16_t> silence(3200, 0);
    return BuildSttRequest(s, key, MakeWav(silence.data(), silence.size(), 16000), 16000);
}

// ---------------------------------------------------------------- speech model lists

namespace
{
std::vector<ModelInfo> AnyModelList(const std::string& body)
{
    std::vector<ModelInfo> models;
    std::string error;
    if (ParseModelList(LlmProvider::Custom, body, models, error))
        return models;
    // ElevenLabs: a bare array of {"model_id", "name", "can_do_text_to_speech"}.
    cJSON* root = cJSON_Parse(body.c_str());
    if (cJSON_IsArray(root))
    {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root)
        {
            const char* id = StringField(item, "model_id");
            if (!id)
                continue;
            const cJSON* tts = cJSON_GetObjectItemCaseSensitive(item, "can_do_text_to_speech");
            if (cJSON_IsBool(tts) && !cJSON_IsTrue(tts))
                continue;
            ModelInfo m;
            m.id = id;
            if (const char* n = StringField(item, "name"))
                m.name = n;
            models.push_back(std::move(m));
        }
    }
    cJSON_Delete(root);
    return models;
}

bool Has(const std::string& id, const char* needle)
{
    return LowerAscii(id).find(needle) != std::string::npos;
}

std::vector<ModelInfo> Fixed(std::initializer_list<const char*> ids)
{
    std::vector<ModelInfo> out;
    for (const char* id : ids)
        out.push_back({id, "", false, 0, true});
    return out;
}
} // namespace

std::vector<ModelInfo> SttModelChoices(SttProvider provider, const std::string& body)
{
    switch (provider)
    {
        case SttProvider::Google:
            return Fixed({"latest_short", "latest_long", "command_and_search", "default", "phone_call", "video"});
        case SttProvider::ElevenLabs: return Fixed({"scribe_v1"});
        default: break;
    }
    std::vector<ModelInfo> out;
    for (const ModelInfo& m : AnyModelList(body))
    {
        bool keep = true;
        if (provider == SttProvider::OpenAI || provider == SttProvider::Groq)
            keep = Has(m.id, "whisper") || Has(m.id, "transcribe");
        else if (provider == SttProvider::Gemini)
            keep = Has(m.id, "gemini") && !Has(m.id, "tts") && !Has(m.id, "image") && !Has(m.id, "embed") &&
                   !Has(m.id, "live") && !Has(m.id, "native-audio");
        if (keep)
            out.push_back(m);
    }
    return out;
}

std::vector<ModelInfo> TtsModelChoices(TtsProvider provider, const std::string& body)
{
    if (provider == TtsProvider::WindowsSapi || provider == TtsProvider::Google || provider == TtsProvider::Off)
        return {}; // the voice is the whole choice there
    std::vector<ModelInfo> out;
    for (const ModelInfo& m : AnyModelList(body))
    {
        bool keep = true;
        if (provider == TtsProvider::OpenAI || provider == TtsProvider::Gemini)
            keep = Has(m.id, "tts");
        if (keep)
            out.push_back(m);
    }
    return out;
}

std::string PickSpeechModel(const std::vector<ModelInfo>& offered, const std::string& current, bool forTts)
{
    if (offered.empty() || std::any_of(offered.begin(), offered.end(), [&](const ModelInfo& m) { return m.id == current; }))
        return {};
    // The list is sorted by id, so the newest version comes last: search from the end.
    const char* const prefs[] = {forTts ? "flash-tts" : "flash-lite", forTts ? "tts" : "flash", ""};
    for (const char* want : prefs)
        for (auto it = offered.rbegin(); it != offered.rend(); ++it)
            if (Has(it->id, want) && !Has(it->id, "preview")) // stable before preview
                return it->id;
    for (const char* want : prefs)
        for (auto it = offered.rbegin(); it != offered.rend(); ++it)
            if (Has(it->id, want))
                return it->id;
    return offered.back().id;
}

bool BuildTtsModelListRequest(const Settings& s, const std::string& key, HttpRequest& out)
{
    if (s.tts != TtsProvider::ElevenLabs && s.tts != TtsProvider::Custom)
        return false;
    out = HttpRequest{};
    out.get = true;
    out.timeoutSeconds = 20;
    out.url = JoinUrl(s.EffectiveTtsBaseUrl(), "models");
    if (s.tts == TtsProvider::ElevenLabs)
        out.headers.emplace_back("xi-api-key", key);
    else if (!key.empty())
        out.headers.emplace_back("Authorization", "Bearer " + key);
    return true;
}

// ---------------------------------------------------------------- helpers

std::string Base64Encode(const std::string& data)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3)
    {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) |
                           (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < data.size())
    {
        unsigned v = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size())
            v |= static_cast<unsigned char>(data[i + 1]) << 8;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < data.size() ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string Base64Decode(const std::string& text)
{
    auto value = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned buffer = 0;
    int bits = 0;
    for (char c : text)
    {
        const int v = value(c);
        if (v < 0)
            continue; // padding, whitespace
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

std::string ApplyRadioFilter(const std::string& wav)
{
    // Walk the RIFF chunks for "fmt " and "data".
    if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0)
        return wav;
    auto u32 = [&](size_t at)
    {
        return static_cast<uint32_t>(static_cast<unsigned char>(wav[at])) |
               (static_cast<uint32_t>(static_cast<unsigned char>(wav[at + 1])) << 8) |
               (static_cast<uint32_t>(static_cast<unsigned char>(wav[at + 2])) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(wav[at + 3])) << 24);
    };
    auto u16 = [&](size_t at)
    {
        return static_cast<uint16_t>(static_cast<unsigned char>(wav[at]) |
                                     (static_cast<unsigned char>(wav[at + 1]) << 8));
    };
    int channels = 0, bits = 0, rate = 0, format = 0;
    size_t dataAt = 0, dataSize = 0;
    for (size_t at = 12; at + 8 <= wav.size();)
    {
        const uint32_t size = u32(at + 4);
        if (wav.compare(at, 4, "fmt ") == 0 && at + 24 <= wav.size())
        {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = static_cast<int>(u32(at + 12));
            bits = u16(at + 22);
        }
        else if (wav.compare(at, 4, "data") == 0)
        {
            dataAt = at + 8;
            // Streaming encoders write 0xFFFFFFFF as the size: take the rest of the file.
            dataSize = std::min<size_t>(size, wav.size() - dataAt);
            break;
        }
        at += 8 + size + (size & 1);
    }
    if (format != 1 || bits != 16 || channels < 1 || rate <= 0 || dataAt == 0)
        return wav;

    std::string out = wav;
    const size_t frames = dataSize / (2 * static_cast<size_t>(channels));
    // One-pole high-pass at 300 Hz and two one-pole low-passes at 3 kHz, per channel.
    const float dt = 1.0f / static_cast<float>(rate);
    const float rcHigh = 1.0f / (2.0f * 3.14159265f * 300.0f);
    const float rcLow = 1.0f / (2.0f * 3.14159265f * 3000.0f);
    const float aHigh = rcHigh / (rcHigh + dt);
    const float aLow = dt / (rcLow + dt);
    for (int c = 0; c < channels; ++c)
    {
        float prevIn = 0, prevHigh = 0, low1 = 0, low2 = 0;
        for (size_t f = 0; f < frames; ++f)
        {
            const size_t at = dataAt + (f * static_cast<size_t>(channels) + static_cast<size_t>(c)) * 2;
            const float in = static_cast<float>(static_cast<int16_t>(u16(at))) / 32768.0f;
            const float high = aHigh * (prevHigh + in - prevIn);
            prevIn = in;
            prevHigh = high;
            low1 += aLow * (high - low1);
            low2 += aLow * (low1 - low2);
            float y = low2 * 2.2f;              // make up the band-limit loss, then drive into...
            y = y / (1.0f + std::fabs(y)) * 1.3f; // ...a soft clip: the compressed radio sound
            if (y > 1.0f) y = 1.0f;
            if (y < -1.0f) y = -1.0f;
            const int16_t s = static_cast<int16_t>(y * 32000.0f);
            out[at] = static_cast<char>(s & 0xFF);
            out[at + 1] = static_cast<char>((s >> 8) & 0xFF);
        }
    }
    return out;
}

std::string MakeWav(const int16_t* samples, size_t count, int sampleRate)
{
    const uint32_t dataBytes = static_cast<uint32_t>(count * 2);
    std::string wav;
    wav.reserve(44 + dataBytes);
    auto u32 = [&](uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            wav += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    auto u16 = [&](uint16_t v)
    {
        wav += static_cast<char>(v & 0xFF);
        wav += static_cast<char>(v >> 8);
    };
    wav += "RIFF";
    u32(36 + dataBytes);
    wav += "WAVEfmt ";
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * 2);
    u16(2);
    u16(16);
    wav += "data";
    u32(dataBytes);
    wav.append(reinterpret_cast<const char*>(samples), dataBytes); // little-endian hosts only (x64)
    return wav;
}

} // namespace Poseidon::NpcChat
