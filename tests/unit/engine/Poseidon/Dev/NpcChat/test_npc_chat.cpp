// NPC chat: the pure halves of the feature -- the model's reply contract, the order whitelist,
// every provider's wire format, the WAV/base64 helpers, the settings round trip, and the key
// hygiene (redaction, refusing to store keys inside a git work tree).
//
// No network and no game world: these are the parts where a silent mistake costs the player
// either a broken conversation or a leaked key.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatGame.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatReply.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <cjson/cJSON.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace Poseidon::NpcChat;
using Catch::Matchers::ContainsSubstring;

namespace
{
const HttpPart* FindPart(const HttpRequest& r, const std::string& name)
{
    for (const HttpPart& p : r.parts)
        if (p.name == name)
            return &p;
    return nullptr;
}

std::string Header(const HttpRequest& r, const std::string& name)
{
    for (const auto& [n, v] : r.headers)
        if (n == name)
            return v;
    return {};
}
} // namespace

// ---------------------------------------------------------------- reply contract

TEST_CASE("NpcChat reply: squad answer with an order", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"2","text":"Two, covering."}],
                           "orders":[{"to":["2","3"],"order":"cover"}]})",
                       "HQ", r));
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].speaker == "2");
    CHECK(r.lines[0].text == "Two, covering.");
    REQUIRE(r.orders.size() == 1);
    CHECK(r.orders[0].kind == OrderKind::Cover);
    CHECK(r.orders[0].to == std::vector<std::string>{"2", "3"});
    CHECK_FALSE(r.orders[0].all);
}

TEST_CASE("NpcChat reply: prose and code fences around the JSON are tolerated", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply("Sure!\n```json\n{\"say\":[{\"speaker\":\"HQ\",\"text\":\"Hold, over.\"}],\"orders\":[]}\n```",
                       "2", r));
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].speaker == "HQ");
}

TEST_CASE("NpcChat reply: plain prose becomes one line from the default speaker", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply("Not bad, sergeant. Quiet morning.", "P1", r));
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].speaker == "P1");
    CHECK(r.orders.empty());
}

TEST_CASE("NpcChat reply: speaker and recipient ids are normalised", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"command","text":"a"},{"speaker":"Two","text":"b"},
                                  {"speaker":3,"text":"c"},{"speaker":"p2","text":"d"}],
                           "orders":[{"to":"everyone","order":"follow me"},{"to":["unit 4"],"order":"hold"}]})",
                       "HQ", r));
    REQUIRE(r.lines.size() == 4);
    CHECK(r.lines[0].speaker == "HQ");
    CHECK(r.lines[1].speaker == "2");
    CHECK(r.lines[2].speaker == "3");
    CHECK(r.lines[3].speaker == "P2");
    REQUIRE(r.orders.size() == 2);
    CHECK(r.orders[0].all);
    CHECK(r.orders[0].kind == OrderKind::Follow);
    CHECK(r.orders[1].to == std::vector<std::string>{"4"});
    CHECK(r.orders[1].kind == OrderKind::Hold);
}

TEST_CASE("NpcChat reply: orders outside the whitelist are rejected, never executed", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[],"orders":[
        {"to":"all","order":"execute","value":"player setDamage 1"},
        {"to":["2"],"order":"stance","value":"player setDamage 1"},
        {"to":["2"],"order":"move"},
        {"order":"hold"},
        {"to":["2"],"order":"stance","value":"prone"}]})",
                       "2", r));
    REQUIRE(r.orders.size() == 1);
    CHECK(r.orders[0].kind == OrderKind::Stance);
    CHECK(r.orders[0].value == "DOWN");
    CHECK(r.rejected.size() == 4);
}

TEST_CASE("NpcChat reply: where offsets and targets", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[],"orders":[
        {"to":["2"],"order":"move","where":{"ref":"player","bearing":"90","distance":100}},
        {"to":"all","order":"engage","target":"e1"},
        {"to":["3"],"order":"get_in","where":{"ref":"vehicle","id":"v2"}}]})",
                       "2", r));
    REQUIRE(r.orders.size() == 3);
    CHECK(r.orders[0].where.ref == "player");
    CHECK(r.orders[0].where.bearing == 90.0f);
    CHECK(r.orders[0].where.distance == 100.0f);
    CHECK(r.orders[1].target == "E1");
    CHECK(r.orders[2].kind == OrderKind::GetIn);
    CHECK(r.orders[2].target == "V2");
}

TEST_CASE("NpcChat order values map to the engine's literal set", "[npcchat]")
{
    CHECK(NormaliseOrderValue(OrderKind::CombatMode, "weapons free") == "YELLOW");
    CHECK(NormaliseOrderValue(OrderKind::CombatMode, "hold_fire") == "GREEN");
    CHECK(NormaliseOrderValue(OrderKind::Formation, "staggered column") == "STAG COLUMN");
    CHECK(NormaliseOrderValue(OrderKind::Formation, "echelon left") == "ECH LEFT");
    CHECK(NormaliseOrderValue(OrderKind::Speed, "double time") == "FULL");
    CHECK(NormaliseOrderValue(OrderKind::Behaviour, "danger") == "COMBAT");
    CHECK(NormaliseOrderValue(OrderKind::Behaviour, "\"; deleteVehicle player; \"").empty());
    CHECK(ParseOrderKind("Cover-Me") == OrderKind::Cover);
    CHECK(ParseOrderKind("stay here") == OrderKind::Hold);
    CHECK(ParseOrderKind("dismount") == OrderKind::GetOut);
    CHECK(ParseOrderKind("launch nukes") == OrderKind::Invalid);
}

TEST_CASE("NpcChat format instructions name every order and the three kinds of addressee", "[npcchat]")
{
    const std::string with = ResponseFormatInstructions(true);
    for (const char* needle : {"follow", "hold", "move", "cover", "watch", "engage", "stance", "combat_mode",
                               "behaviour", "formation", "speed", "get_in", "get_out", "HQ", "NEAREST",
                               "LOOKING AT"})
        CHECK_THAT(with, ContainsSubstring(needle));
    CHECK_THAT(ResponseFormatInstructions(false), ContainsSubstring("Orders are disabled"));
}

// ---------------------------------------------------------------- provider wire formats

TEST_CASE("NpcChat LLM request: OpenAI-compatible providers", "[npcchat]")
{
    Settings s;
    s.llm = LlmProvider::OpenRouter;
    const HttpRequest r = BuildLlmRequest(s, "test-key", "SYSTEM", {{"user", "hi"}, {"assistant", "yo"}});
    CHECK(r.url == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(Header(r, "Authorization") == "Bearer test-key");
    cJSON* root = cJSON_Parse(r.body.c_str());
    REQUIRE(root);
    const cJSON* messages = cJSON_GetObjectItem(root, "messages");
    REQUIRE(cJSON_GetArraySize(messages) == 3);
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "role")->valuestring) == "system");
    CHECK(std::string(cJSON_GetObjectItem(root, "model")->valuestring) == DefaultLlmModel(LlmProvider::OpenRouter));
    cJSON_Delete(root);

    s.llm = LlmProvider::Custom;
    s.llmBaseUrl = "http://localhost:1234/v1/";
    const HttpRequest local = BuildLlmRequest(s, "", "S", {{"user", "hi"}});
    CHECK(local.url == "http://localhost:1234/v1/chat/completions");
    CHECK(Header(local, "Authorization").empty());
    CHECK(local.body.find("response_format") == std::string::npos); // LM Studio refuses json_object

    std::string text, error;
    REQUIRE(ParseLlmResponse(LlmProvider::OpenAI, R"({"choices":[{"message":{"content":"{\"say\":[]}"}}]})", text,
                             error));
    CHECK(text == "{\"say\":[]}");
}

TEST_CASE("NpcChat LLM request: Anthropic shape and role alternation", "[npcchat]")
{
    Settings s;
    s.llm = LlmProvider::Anthropic;
    // Leading assistant turn is dropped, consecutive user turns are merged.
    const HttpRequest r =
        BuildLlmRequest(s, "k", "SYSTEM", {{"assistant", "x"}, {"user", "a"}, {"user", "b"}, {"assistant", "c"}});
    CHECK(r.url == "https://api.anthropic.com/v1/messages");
    CHECK(Header(r, "x-api-key") == "k");
    CHECK(Header(r, "anthropic-version") == "2023-06-01");
    cJSON* root = cJSON_Parse(r.body.c_str());
    REQUIRE(root);
    CHECK(std::string(cJSON_GetObjectItem(root, "system")->valuestring) == "SYSTEM");
    const cJSON* messages = cJSON_GetObjectItem(root, "messages");
    REQUIRE(cJSON_GetArraySize(messages) == 2);
    CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content")->valuestring) == "a\nb");
    cJSON_Delete(root);

    std::string text, error;
    REQUIRE(ParseLlmResponse(LlmProvider::Anthropic, R"({"content":[{"type":"text","text":"ok"}]})", text, error));
    CHECK(text == "ok");
    CHECK_FALSE(ParseLlmResponse(LlmProvider::Anthropic, R"({"error":{"message":"bad key"}})", text, error));
    CHECK(error == "bad key");
}

TEST_CASE("NpcChat STT requests: multipart Whisper and Google JSON", "[npcchat]")
{
    std::vector<int16_t> pcm(1600, 1000);
    const std::string wav = MakeWav(pcm.data(), pcm.size(), 16000);

    Settings s;
    s.stt = SttProvider::Groq;
    const HttpRequest whisper = BuildSttRequest(s, "g", wav, 16000);
    CHECK(whisper.url == "https://api.groq.com/openai/v1/audio/transcriptions");
    REQUIRE(FindPart(whisper, "file"));
    CHECK(FindPart(whisper, "file")->data == wav);
    CHECK(FindPart(whisper, "model")->data == "whisper-large-v3-turbo");

    s.stt = SttProvider::Google;
    const HttpRequest google = BuildSttRequest(s, "AIzaTEST", wav, 16000);
    CHECK(google.url == "https://speech.googleapis.com/v1/speech:recognize");
    CHECK(Header(google, "X-Goog-Api-Key") == "AIzaTEST");
    CHECK(google.url.find("key=") == std::string::npos); // never in the URL
    cJSON* root = cJSON_Parse(google.body.c_str());
    REQUIRE(root);
    const std::string content = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "audio"), "content")->valuestring;
    CHECK(Base64Decode(content) == wav.substr(44)); // header stripped
    cJSON_Delete(root);

    std::string text, error;
    REQUIRE(ParseSttResponse(SttProvider::Google,
                             R"({"results":[{"alternatives":[{"transcript":"two tangos"}]},
                                             {"alternatives":[{"transcript":"please advise"}]}]})",
                             text, error));
    CHECK(text == "two tangos please advise");
    REQUIRE(ParseSttResponse(SttProvider::OpenAI, R"({"text":"  Command, advise. "})", text, error));
    CHECK(text == "Command, advise.");
    CHECK_FALSE(ParseSttResponse(SttProvider::OpenAI, R"({"text":""})", text, error));
}

TEST_CASE("NpcChat TTS: Google returns base64 WAV; streamed WAV sizes are repaired", "[npcchat]")
{
    std::vector<int16_t> pcm(240, 500);
    std::string wav = MakeWav(pcm.data(), pcm.size(), 24000);

    HttpResponse google;
    google.status = 200;
    google.body = "{\"audioContent\":\"" + Base64Encode(wav) + "\"}";
    std::string out, error;
    REQUIRE(ParseTtsResponse(TtsProvider::Google, google, out, error));
    CHECK(out == wav);

    // OpenAI streams with 0xFFFFFFFF sizes.
    std::string streamed = wav;
    for (size_t at : {size_t(4), size_t(40)})
        for (int i = 0; i < 4; ++i)
            streamed[at + i] = static_cast<char>(0xFF);
    HttpResponse openai;
    openai.status = 200;
    openai.body = streamed;
    REQUIRE(ParseTtsResponse(TtsProvider::OpenAI, openai, out, error));
    CHECK(out == wav);

    HttpResponse junk;
    junk.body = "{\"error\":\"nope\"}";
    CHECK_FALSE(ParseTtsResponse(TtsProvider::OpenAI, junk, out, error));
}

TEST_CASE("NpcChat audio helpers", "[npcchat]")
{
    CHECK(Base64Encode("") == "");
    CHECK(Base64Encode("f") == "Zg==");
    CHECK(Base64Encode("fo") == "Zm8=");
    CHECK(Base64Encode("foobar") == "Zm9vYmFy");
    CHECK(Base64Decode("Zm9vYmFy") == "foobar");
    CHECK(Base64Decode("Zm8=") == "fo");

    std::vector<int16_t> pcm(1000);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(std::sin(i * 0.3) * 20000);
    const std::string wav = MakeWav(pcm.data(), pcm.size(), 16000);
    CHECK(wav.size() == 44 + 2000);
    CHECK(wav.compare(0, 4, "RIFF") == 0);
    const std::string radio = ApplyRadioFilter(wav);
    CHECK(radio.size() == wav.size());
    CHECK(radio.compare(0, 44, wav, 0, 44) == 0); // header untouched
    CHECK(radio != wav);
    CHECK(ApplyRadioFilter("not a wav") == "not a wav");

    CHECK(JoinUrl("https://a/v1/", "/chat") == "https://a/v1/chat");
    CHECK(SplitList(" a, b ,,c ") == std::vector<std::string>{"a", "b", "c"});
}

// ---------------------------------------------------------------- settings and keys

TEST_CASE("NpcChat settings round-trip through JSON and never carry a key", "[npcchat]")
{
    Settings s;
    s.enabled = true;
    s.llm = LlmProvider::Anthropic;
    s.llmModel = "claude-x";
    s.systemPrompt = "Line one\n\"quoted\"";
    s.tts = TtsProvider::Google;
    s.chatKey = 40;
    s.rememberKeys = true;
    const std::string json = SettingsToJson(s);
    CHECK(json.find("key\"") == std::string::npos);

    Settings back;
    REQUIRE(SettingsFromJson(json, back));
    CHECK(back.enabled);
    CHECK(back.llm == LlmProvider::Anthropic);
    CHECK(back.llmModel == "claude-x");
    CHECK(back.systemPrompt == s.systemPrompt);
    CHECK(back.tts == TtsProvider::Google);
    CHECK(back.chatKey == 40);
    CHECK(back.rememberKeys);

    Settings bad;
    CHECK(SettingsFromJson(R"({"llm":99,"stt":-1})", bad));
    CHECK(bad.llm == Settings().llm); // out-of-range enums keep the default
    CHECK_FALSE(SettingsFromJson("not json", bad));
}

TEST_CASE("NpcChat keys: redaction scrubs held keys and key-shaped strings", "[npcchat]")
{
    // Built at run time: a key-shaped literal in this file would (rightly) trip the pre-commit
    // secret scan.
    const std::string held = std::string("sk") + "-test-held-key-0123456789abcdef";
    const std::string google = std::string("AIza") + "SyA0123456789abcdefghijklmnopq";
    SetKey("openai", "  " + held + "\n");
    CHECK(GetKey("openai") == held); // paste whitespace trimmed
    const std::string redacted = RedactSecrets("error: Incorrect API key " + held + " and " + google);
    CHECK(redacted.find(held) == std::string::npos);
    CHECK(redacted.find(google) == std::string::npos);
    CHECK_THAT(redacted, ContainsSubstring("[redacted]"));
    ClearAllKeys();
    CHECK_FALSE(HasPanelKey("openai"));
}

TEST_CASE("NpcChat keys: the store refuses any path inside a git work tree", "[npcchat]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "npcchat_git_guard_test";
    fs::remove_all(root);
    fs::create_directories(root / "repo" / "sub");
    fs::create_directories(root / "repo" / ".git");
    fs::create_directories(root / "plain");
    CHECK(IsInsideGitWorkTree((root / "repo" / "sub" / "keys.dat").string()));
    CHECK_FALSE(IsInsideGitWorkTree((root / "plain" / "keys.dat").string()));
    // A worktree's .git is a FILE, not a directory.
    fs::create_directories(root / "wt");
    std::ofstream(root / "wt" / ".git") << "gitdir: elsewhere\n";
    CHECK(IsInsideGitWorkTree((root / "wt" / "keys.dat").string()));
    fs::remove_all(root);

    const std::string store = KeyStorePath();
    if (!store.empty())
        CHECK_FALSE(IsInsideGitWorkTree(store));
}

TEST_CASE("NpcChat briefing HTML becomes readable text", "[npcchat]")
{
    const std::string html = "<html><body><p><a name=\"Main\"></a>Take the &quot;village&quot;.<br>"
                             "Watch for   BMPs &amp; T-72s.</p><p><a name=\"OBJ_1\"></a>Destroy the radar.</p>"
                             "<a name=\"__BRIEFING\"></a></body></html>";
    const std::string text = HtmlToText(html);
    CHECK_THAT(text, ContainsSubstring("[Main] Take the \"village\"."));
    CHECK_THAT(text, ContainsSubstring("Watch for BMPs & T-72s."));
    CHECK_THAT(text, ContainsSubstring("[OBJ_1] Destroy the radar."));
    CHECK(text.find("__BRIEFING") == std::string::npos);
    CHECK(text.find('<') == std::string::npos);
}

TEST_CASE("NpcChat call signs and tone survive the settings round trip", "[npcchat]")
{
    Settings s;
    s.callsigns[2] = "Hammer";
    s.callsigns[3] = "Papa Bear";
    s.tone = 3;
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.CallsignFor(2) == "Hammer");
    CHECK(back.CallsignFor(3) == "Papa Bear");
    CHECK(back.CallsignFor(4).empty());
    CHECK(back.tone == 3);
    Settings clamped;
    REQUIRE(SettingsFromJson(R"({"tone":42})", clamped));
    CHECK(clamped.tone == kToneCount - 1);
    for (int t = 0; t < kToneCount; ++t)
        CHECK_THAT(ToneInstructions(t), ContainsSubstring("TONE"));
}

TEST_CASE("NpcChat reply: the whole squad answers together; Papa Bear stays a name", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"all","text":"Sir, yes sir!"},{"speaker":"Papa Bear","text":"x"}],
                           "orders":[{"to":["everyone"],"order":"formation","value":"skirmish line"},
                                     {"to":"squad","order":"follow"}]})",
                       "2", r));
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].speaker == "ALL");
    CHECK(r.lines[1].speaker == "PAPA BEAR"); // resolved against squad call signs in-game, else HQ
    REQUIRE(r.orders.size() == 2);
    CHECK(r.orders[0].all);
    CHECK(r.orders[0].value == "LINE");
    CHECK(r.orders[1].all);
    CHECK(NormaliseOrderValue(OrderKind::Formation, "single file") == "COLUMN");
    CHECK_THAT(ResponseFormatInstructions(true), ContainsSubstring("CALL SIGN"));
}

TEST_CASE("NpcChat addressing: real name, role, call sign and speech-to-text slips", "[npcchat]")
{
    const std::vector<SquadCandidate> squad = {{2, "Bobby Bruning", "Machine Gunner", ""},
                                               {3, "James Pound", "Grenadier", "Hammer"},
                                               {4, "Jim Self", "Medic", ""}};
    CHECK(MatchSquadMember(squad, "James Pound") == 3);
    CHECK(MatchSquadMember(squad, "pound") == 3);
    CHECK(MatchSquadMember(squad, "Bobby") == 2);
    CHECK(MatchSquadMember(squad, "Grenadier") == 3);
    CHECK(MatchSquadMember(squad, "the medic") == 4);
    CHECK(MatchSquadMember(squad, "machine gunner") == 2);
    CHECK(MatchSquadMember(squad, "HAMMER") == 3);
    CHECK(MatchSquadMember(squad, "Granadier") == 3); // speech-to-text slip
    CHECK(MatchSquadMember(squad, "Hamer") == 3);
    CHECK(MatchSquadMember(squad, "Papa Bear") == 0);
    CHECK(MatchSquadMember(squad, "") == 0);
}

TEST_CASE("NpcChat model list: OpenRouter pricing marks free models; the filter finds them", "[npcchat]")
{
    const std::string body = R"json({"data":[
        {"id":"openai/gpt-4.1-mini","name":"GPT-4.1 Mini","pricing":{"prompt":"0.0000004","completion":"0.0000016"}},
        {"id":"nvidia/nemotron-3.5-lightning:free","name":"Nemotron Lightning (free)","pricing":{"prompt":"0","completion":"0"}},
        {"id":"meta-llama/llama-3.3-70b-instruct","name":"Llama 3.3 70B","pricing":{"prompt":"0","completion":"0"}}]})json";
    std::vector<ModelInfo> models;
    std::string error;
    REQUIRE(ParseModelList(LlmProvider::OpenRouter, body, models, error));
    REQUIRE(models.size() == 3);
    const auto free = FilterModels(models, "free");
    CHECK(free.size() == 2); // the :free id and the zero-priced one
    CHECK(FilterModels(models, "free llama").size() == 1);
    CHECK(FilterModels(models, "GPT").size() == 1);
    CHECK(FilterModels(models, "").size() == 3);

    std::vector<ModelInfo> gemini;
    REQUIRE(ParseModelList(LlmProvider::Gemini, R"({"data":[{"id":"models/gemini-2.5-flash"}]})", gemini, error));
    CHECK(gemini[0].id == "gemini-2.5-flash");

    Settings s;
    s.llm = LlmProvider::OpenRouter;
    HttpRequest check;
    REQUIRE(BuildKeyCheckRequest(s, "k", check));
    CHECK(check.get);
    CHECK(check.url == "https://openrouter.ai/api/v1/key");
    CHECK(Header(check, "Authorization") == "Bearer k");
    CHECK_THAT(DescribeKeyCheck(R"({"data":{"is_free_tier":true}})"), ContainsSubstring("free tier"));
    s.llm = LlmProvider::Anthropic;
    const HttpRequest anthropic = BuildModelListRequest(s, "k");
    CHECK(Header(anthropic, "x-api-key") == "k");
    CHECK_FALSE(BuildKeyCheckRequest(s, "k", check));
}

TEST_CASE("NpcChat voices: ElevenLabs and Google lists parse; ElevenLabs speech wire format", "[npcchat]")
{
    std::vector<VoiceInfo> voices;
    std::string error;
    REQUIRE(ParseVoiceList(TtsProvider::ElevenLabs,
                           R"({"voices":[{"voice_id":"abc123","name":"Adam","labels":{"gender":"male","accent":"american"}}]})",
                           voices, error));
    CHECK(voices[0].id == "abc123");
    CHECK(voices[0].name == "Adam");
    CHECK_THAT(voices[0].detail, ContainsSubstring("male"));
    REQUIRE(ParseVoiceList(TtsProvider::Google,
                           R"({"voices":[{"name":"en-GB-Standard-B","languageCodes":["en-GB"],"ssmlGender":"MALE"}]})",
                           voices, error));
    CHECK(voices[0].id == "en-GB-Standard-B");
    CHECK(StaticVoices(TtsProvider::OpenAI).size() >= 10);

    Settings s;
    s.tts = TtsProvider::ElevenLabs;
    const HttpRequest tts = BuildTtsRequest(s, "el", "Roger.", "abc123", "");
    CHECK(tts.url == "https://api.elevenlabs.io/v1/text-to-speech/abc123?output_format=pcm_22050");
    CHECK(Header(tts, "xi-api-key") == "el");
    HttpResponse pcm;
    pcm.status = 200;
    pcm.body = std::string(4410, '\x01');
    std::string wav;
    REQUIRE(ParseTtsResponse(TtsProvider::ElevenLabs, pcm, wav, error));
    CHECK(wav.size() == 44 + 4410);

    s.stt = SttProvider::ElevenLabs;
    const HttpRequest stt = BuildSttRequest(s, "el", wav, 16000);
    CHECK(stt.url == "https://api.elevenlabs.io/v1/speech-to-text");
    REQUIRE(FindPart(stt, "model_id"));
    CHECK(FindPart(stt, "model_id")->data == "scribe_v1");
    HttpRequest voiceList;
    REQUIRE(BuildVoiceListRequest(s, "el", voiceList));
    CHECK(voiceList.get);
}

TEST_CASE("NpcChat keys: pasted junk is stripped; setup failures explain the fix", "[npcchat]")
{
    const std::string key = std::string("sk-or") + "-v1-0123456789abcdef";
    CHECK(SanitizeKey("  \"" + key + "\"\n") == key);
    CHECK(SanitizeKey("Bearer " + key) == key);
    CHECK(SanitizeKey("\xE2\x80\x8B" + key + "\xC2\xA0") == key); // zero-width space, nbsp

    HttpResponse disabled;
    disabled.status = 403;
    disabled.body = R"({"error":{"message":"Cloud Text-to-Speech API has not been used in project 1 before or it is disabled."}})";
    CHECK_THAT(DescribeHttpFailure("Text-to-speech", disabled), ContainsSubstring("enable the API"));
    HttpResponse unknown;
    unknown.status = 401;
    unknown.body = R"({"error":{"message":"User not found."}})";
    CHECK_THAT(DescribeHttpFailure("OpenRouter", unknown), ContainsSubstring("Copy the whole key"));
}

TEST_CASE("NpcChat Gemini (AI Studio) speech: the default, one key, both directions", "[npcchat]")
{
    const Settings defaults;
    CHECK(defaults.llm == LlmProvider::Gemini);
    CHECK(defaults.tts == TtsProvider::Gemini);
    CHECK(defaults.stt == SttProvider::Gemini);
    CHECK(std::string(TtsKeySlot(TtsProvider::Gemini)) == LlmKeySlot(LlmProvider::Gemini));
    CHECK(std::string(SttKeySlot(SttProvider::Gemini)) == LlmKeySlot(LlmProvider::Gemini));

    Settings s;
    const HttpRequest tts = BuildTtsRequest(s, "g", "Roger, moving.", "Orus", "a tired soldier on the radio");
    CHECK(tts.url ==
          "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash-tts:generateContent");
    CHECK(Header(tts, "x-goog-api-key") == "g");
    CHECK(tts.url.find("key=") == std::string::npos);
    CHECK_THAT(tts.body, ContainsSubstring("\"voiceName\":\"Orus\""));
    CHECK_THAT(tts.body, ContainsSubstring("AUDIO"));
    CHECK(tts.body.find("tired soldier") == std::string::npos); // style hints are not sent

    const std::string pcm(4800, '\x02');
    HttpResponse audio;
    audio.status = 200;
    audio.body = R"({"candidates":[{"content":{"parts":[{"inlineData":{"mimeType":"audio/L16;codec=pcm;rate=24000","data":")" +
                 Base64Encode(pcm) + R"("}}]}}]})";
    std::string wav, error;
    REQUIRE(ParseTtsResponse(TtsProvider::Gemini, audio, wav, error));
    CHECK(wav.size() == 44 + pcm.size());
    CHECK(static_cast<unsigned char>(wav[24]) == (24000 & 0xFF)); // sample rate taken from the mime type

    std::vector<int16_t> samples(1600, 100);
    const HttpRequest stt = BuildSttRequest(s, "g", MakeWav(samples.data(), samples.size(), 16000), 16000);
    CHECK(stt.url == "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash-lite:generateContent");
    CHECK_THAT(stt.body, ContainsSubstring("audio/wav"));
    std::string text;
    REQUIRE(ParseSttResponse(SttProvider::Gemini,
                             R"({"candidates":[{"content":{"parts":[{"text":"Command, two tangos, advise."}]}}]})", text,
                             error));
    CHECK(text == "Command, two tangos, advise.");

    CHECK(StaticVoices(TtsProvider::Gemini).size() >= 30);
    HttpRequest check;
    CHECK_FALSE(BuildVoiceListRequest(s, "g", check)); // fixed voice set...
    REQUIRE(BuildTtsKeyCheckRequest(s, "g", check));   // ...but the key is still checked
    CHECK(check.get);
}

TEST_CASE("NpcChat keys: the fingerprint shows enough to compare, never the key", "[npcchat]")
{
    const std::string key = std::string("sk-or") + "-v1-0123456789abcdef0123456789abcdef";
    SetKey("openrouter", key);
    const std::string fp = KeyFingerprint("openrouter");
    CHECK(fp.rfind("sk-or-v1...", 0) == 0);
    CHECK_THAT(fp, ContainsSubstring("cdef (" + std::to_string(key.size()) + " chars)"));
    CHECK(fp.find(key.substr(8, 12)) == std::string::npos);
    ClearAllKeys();
    CHECK(KeyFingerprint("openrouter").empty());
}

TEST_CASE("NpcChat voices: only voices known to be male are auto-assigned", "[npcchat]")
{
    CHECK(IsMaleVoice({"x", "Adam", "male, american"}));
    CHECK_FALSE(IsMaleVoice({"x", "Rachel", "female, american"}));
    CHECK(IsMaleVoice({"en-GB-Standard-B", "en-GB-Standard-B", "male, en-GB"}));
    CHECK(IsMaleVoice({"am_adam", "am_adam", ""}));
    CHECK(IsMaleVoice({"bm_george", "bm_george", ""}));
    CHECK_FALSE(IsMaleVoice({"af_bella", "af_bella", ""}));
    CHECK_FALSE(IsMaleVoice({"Fola", "Fola", "new voice"})); // unknown: never guessed

    int male = 0, female = 0;
    for (const VoiceInfo& v : StaticVoices(TtsProvider::Gemini))
        (IsMaleVoice(v) ? male : female)++;
    CHECK(male >= 10);
    CHECK(female >= 10);
    for (const VoiceInfo& v : StaticVoices(TtsProvider::Gemini))
        if (v.id == "Kore")
            CHECK_FALSE(IsMaleVoice(v));
    for (const VoiceInfo& v : StaticVoices(TtsProvider::OpenAI))
        if (v.id == "onyx")
            CHECK(IsMaleVoice(v));

    // Every default Gemini squad voice is one auto-assign would also pick.
    for (const std::string& id : SplitList(DefaultTtsVoices(TtsProvider::Gemini)))
        for (const VoiceInfo& v : StaticVoices(TtsProvider::Gemini))
            if (v.id == id)
                CHECK(IsMaleVoice(v));

    Settings s;
    s.tts = TtsProvider::Gemini;
    s.squadVoicesProvider = TtsProvider::Gemini;
    s.squadVoices[2] = "Orus";
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.SquadVoiceFor(2) == "Orus");
    back.tts = TtsProvider::ElevenLabs;
    CHECK(back.SquadVoiceFor(2).empty()); // a Gemini voice means nothing to ElevenLabs
}

TEST_CASE("NpcChat OpenRouter asks thinking models for a short think; old timeouts are raised", "[npcchat]")
{
    Settings s;
    s.llm = LlmProvider::OpenRouter;
    const HttpRequest r = BuildLlmRequest(s, "k", "S", {{"user", "hi"}});
    CHECK_THAT(r.body, ContainsSubstring("\"reasoning\":{\"effort\":\"low\""));
    CHECK(r.timeoutSeconds == 90);
    s.llm = LlmProvider::OpenAI;
    CHECK(BuildLlmRequest(s, "k", "S", {{"user", "hi"}}).body.find("reasoning") == std::string::npos);

    Settings v1;
    REQUIRE(SettingsFromJson(R"({"version":1,"requestTimeoutSeconds":45})", v1));
    CHECK(v1.requestTimeoutSeconds == 90);
    Settings v2;
    REQUIRE(SettingsFromJson(R"({"version":2,"requestTimeoutSeconds":45})", v2));
    CHECK(v2.requestTimeoutSeconds == 45); // chosen on purpose after the upgrade: kept

    HttpResponse timeout;
    timeout.error = "Timeout was reached";
    CHECK_THAT(DescribeHttpFailure("OpenRouter", timeout), ContainsSubstring("Pick a faster model"));
}

TEST_CASE("NpcChat: an empty reply from a thinking model says why", "[npcchat]")
{
    std::string text, error;
    CHECK_FALSE(ParseLlmResponse(LlmProvider::OpenRouter,
                                 R"({"choices":[{"finish_reason":"length","message":{"content":null,"reasoning":"..."}}]})",
                                 text, error));
    CHECK_THAT(error, ContainsSubstring("token budget thinking"));
    CHECK_FALSE(ParseLlmResponse(LlmProvider::OpenRouter,
                                 R"({"choices":[{"finish_reason":"stop","message":{"content":"   "}}]})", text, error));
    CHECK_THAT(error, ContainsSubstring("empty reply"));
    REQUIRE(ParseLlmResponse(LlmProvider::OpenRouter,
                             R"({"choices":[{"message":{"content":[{"type":"text","text":"Roger."}]}}]})", text, error));
    CHECK(text == "Roger.");

    Settings s;
    s.llm = LlmProvider::OpenRouter;
    s.maxTokens = 500;
    CHECK_THAT(BuildLlmRequest(s, "k", "S", {{"user", "hi"}}).body, ContainsSubstring("\"max_tokens\":2500"));
    s.llm = LlmProvider::Anthropic; // the one provider that does not think unless asked
    CHECK_THAT(BuildLlmRequest(s, "k", "S", {{"user", "hi"}}).body, ContainsSubstring("\"max_tokens\":500"));
}

TEST_CASE("NpcChat: OpenRouter's upstream reason is shown on a 429", "[npcchat]")
{
    HttpResponse busy;
    busy.status = 429;
    busy.body = R"({"error":{"message":"Provider returned error","code":429,"metadata":{"provider_name":"Google AI Studio","raw":"google/gemma-4-26b-a4b-it:free is temporarily rate-limited upstream."}}})";
    const std::string text = DescribeHttpFailure("OpenRouter", busy);
    CHECK_THAT(text, ContainsSubstring("(Google AI Studio)"));
    CHECK_THAT(text, ContainsSubstring("temporarily rate-limited upstream"));
}

TEST_CASE("NpcChat: the game's own radio voices are muted by default, and old settings are upgraded", "[npcchat]")
{
    CHECK(Settings().muteGameVoices);
    Settings v2;
    REQUIRE(SettingsFromJson(R"({"version":2,"muteGameVoices":false})", v2));
    CHECK(v2.muteGameVoices);
    Settings v3;
    REQUIRE(SettingsFromJson(R"({"version":3,"muteGameVoices":false})", v3));
    CHECK_FALSE(v3.muteGameVoices); // switched off on purpose after the upgrade: kept
    Settings s;
    s.muteGameVoices = true;
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.muteGameVoices);
}

TEST_CASE("NpcChat: soldiers obey; fire and engage-anyone orders parse", "[npcchat]")
{
    const std::string rules = ResponseFormatInstructions(true);
    CHECK_THAT(rules, ContainsSubstring("Never refuse, argue"));
    CHECK_THAT(rules, ContainsSubstring("video game"));
    CHECK_THAT(rules, ContainsSubstring("NEVER by HQ"));
    CHECK_THAT(rules, ContainsSubstring("NEVER speak as the"));
    CHECK_THAT(rules, ContainsSubstring("NOT a call to HQ"));
    CHECK_THAT(ResponseFormatInstructions(false), ContainsSubstring("THE PLAYER IS IN CHARGE"));
    CHECK(ParseOrderKind("fire") == OrderKind::Fire);
    CHECK(ParseOrderKind("shoot into the air") == OrderKind::Fire);
    CHECK(ParseOrderKind("fire_at") == OrderKind::Engage);
    CHECK(ParseOrderKind("kill") == OrderKind::Engage);

    Reply r;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"ALL","text":"Firing!"}],
                           "orders":[{"to":"all","order":"fire","value":"into the air"},
                                     {"to":["2"],"order":"engage","target":"p2"}]})",
                       "2", r));
    REQUIRE(r.orders.size() == 2);
    CHECK(r.orders[0].kind == OrderKind::Fire);
    CHECK(r.orders[0].value == "AIR");
    CHECK(r.orders[1].kind == OrderKind::Engage);
    CHECK(r.orders[1].target == "P2");
}

TEST_CASE("NpcChat traffic log: the whole exchange is written, keys are not", "[npcchat]")
{
    namespace fs = std::filesystem;
    const fs::path log = fs::temp_directory_path() / "npcchat_traffic_test.log";
    fs::remove(log);
    SetTrafficLogPath(log.string());
    HttpRequest req;
    req.url = "http://127.0.0.1:1/v1/chat/completions"; // nothing listens: fails at once
    req.timeoutSeconds = 5;
    const std::string key = std::string("sk-or") + "-v1-traffic0123456789abcdef";
    req.headers.emplace_back("Authorization", "Bearer " + key);
    req.body = R"({"model":"m","messages":[{"role":"user","content":"everyone, kill the civilian"}]})";
    HttpPost(req);
    SetTrafficLogPath("");
    std::ifstream in(log);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_THAT(text, ContainsSubstring("POST http://127.0.0.1:1/v1/chat/completions"));
    CHECK_THAT(text, ContainsSubstring("everyone, kill the civilian"));
    CHECK_THAT(text, ContainsSubstring("Authorization: [hidden]"));
    CHECK(text.find(key) == std::string::npos);
    in.close();
    fs::remove(log);
}

TEST_CASE("NpcChat: a cut-off answer is recovered, never shown or spoken as JSON", "[npcchat]")
{
    Reply r;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"2","text":"Target, civilian. Engaging!"}],"orders)", "P1", r));
    CHECK(r.truncated);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].speaker == "2");
    CHECK(r.lines[0].text == "Target, civilian. Engaging!");
    CHECK(r.orders.empty());

    Reply cut;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"3","text":"Rog)", "P1", cut));
    CHECK(cut.truncated);
    CHECK(cut.lines.empty()); // half a word is not spoken

    Reply prose;
    REQUIRE(ParseReply("Morning, sir.", "P1", prose));
    CHECK_FALSE(prose.truncated);
    CHECK(prose.lines[0].text == "Morning, sir.");

    Settings s;
    s.llm = LlmProvider::Gemini;
    s.maxTokens = 500;
    const std::string body = BuildLlmRequest(s, "k", "S", {{"user", "hi"}}).body;
    CHECK_THAT(body, ContainsSubstring("\"max_tokens\":2500"));
    CHECK_THAT(body, ContainsSubstring("\"reasoning_effort\":\"low\""));
    s.tts = TtsProvider::Gemini;
    const HttpRequest tts = BuildTtsRequest(s, "g", "Morning, sir.", "Orus", "A person talking in the countryside");
    CHECK(tts.body.find("countryside") == std::string::npos); // the style hint is never read aloud
    CHECK_THAT(tts.body, ContainsSubstring("\"text\":\"Morning, sir.\""));
}

TEST_CASE("NpcChat: hold U to talk by default; the old End and Caps Lock defaults are upgraded", "[npcchat]")
{
    // Not Caps Lock: that is the game's own voice-over-net key (VoiceOverNetPushToTalk).
    CHECK(Settings().talkKey == 24); // SDL_SCANCODE_U
    Settings old;
    REQUIRE(SettingsFromJson(R"({"version":3,"talkKey":77})", old));
    CHECK(old.talkKey == 24);
    Settings capsLock;
    REQUIRE(SettingsFromJson(R"({"version":4,"talkKey":57})", capsLock));
    CHECK(capsLock.talkKey == 24);
    Settings custom;
    REQUIRE(SettingsFromJson(R"({"version":3,"talkKey":42})", custom));
    CHECK(custom.talkKey == 42); // a key the player chose stays
}

TEST_CASE("NpcChat: live (realtime voice) models are not offered for chat", "[npcchat]")
{
    // gemini-3.8-live answered every chat request with HTTP 400 (bidiGenerateContent only), 2026-10-26.
    CHECK_FALSE(IsChatModelId("gemini-3.8-live"));
    CHECK_FALSE(IsChatModelId("models/gemini-2.5-flash-native-audio-preview"));
    CHECK_FALSE(IsChatModelId("gpt-4o-realtime-preview"));
    CHECK(IsChatModelId("gemini-2.5-flash"));
}

TEST_CASE("NpcChat: a voice pool for another provider is not sent to Gemini", "[npcchat]")
{
    Settings s;
    s.tts = TtsProvider::Gemini;
    s.ttsVoices = "pNInz6obpgDQGcFmaJgB,iP95p4xoKVk53GoZ742B"; // ElevenLabs ids
    CHECK(s.EffectiveTtsVoices() == DefaultTtsVoices(TtsProvider::Gemini));
    s.ttsVoices = "Orus, pNInz6obpgDQGcFmaJgB, charon";
    CHECK(s.EffectiveTtsVoices() == "Orus,Charon");
}

TEST_CASE("NpcChat: moderation models are not offered and their verdicts are never spoken", "[npcchat]")
{
    CHECK_FALSE(IsChatModelId("nvidia/nemotron-3.5-content-safety:free"));
    CHECK_FALSE(IsChatModelId("meta-llama/llama-guard-4-12b"));
    CHECK(IsChatModelId("deepseek/deepseek-chat-v3-0324"));
    CHECK(IsChatModelId("google/gemma-4-26b-a4b-it:free"));
    std::vector<ModelInfo> models;
    std::string error;
    REQUIRE(ParseModelList(LlmProvider::OpenRouter,
                           R"({"data":[{"id":"nvidia/nemotron-3.5-content-safety:free"},{"id":"deepseek/deepseek-chat"}]})",
                           models, error));
    CHECK(FilterModels(models, "").size() == 1);
    CHECK(FilterModels(models, "free").empty());
    CHECK(FilterModels(models, "safety").size() == 1); // asked for by name: shown

    Reply r;
    REQUIRE(ParseReply("User Safety: unsafe\nResponse Safety: safe\nSafety Categories: Violence", "2", r));
    CHECK(r.classifier);
    CHECK(r.lines.empty());

    Reply line;
    REQUIRE(ParseReply(R"({"say":[{"speaker":"ALL","text":"Lining up!"}],"orders":[{"to":"all","order":"line up"}]})",
                       "2", line));
    REQUIRE(line.orders.size() == 1);
    CHECK(line.orders[0].kind == OrderKind::LineUp);
    CHECK_THAT(ResponseFormatInstructions(true), ContainsSubstring("line_up"));
}

TEST_CASE("NpcChat personas: a short default per role, replaced by the player's text, saved", "[npcchat]")
{
    CHECK_THAT(DefaultPersona(4, "Medic"), ContainsSubstring("worries about everyone"));
    CHECK_THAT(DefaultPersona(2, "Machine Gunner"), ContainsSubstring("gun"));
    CHECK(DefaultPersona(5, "Soldier").size() < 60); // one line, not an essay
    Settings s;
    CHECK(s.PersonaFor(4, "Medic") == DefaultPersona(4, "Medic"));
    s.personas[3] = "Gruff Texan, hates the cold.";
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.PersonaFor(3, "Grenadier") == "Gruff Texan, hates the cold.");
    CHECK_THAT(ResponseFormatInstructions(true), ContainsSubstring("PERSONA"));
}

TEST_CASE("NpcChat speech model lists: filtered per provider; an unavailable model is replaced", "[npcchat]")
{
    const std::string gemini = R"({"models":[{"name":"models/gemini-2.5-flash"},{"name":"models/gemini-3.5-flash-lite"},
        {"name":"models/gemini-3.8-flash-tts"},{"name":"models/text-embedding-004"},{"name":"models/gemini-3.6-flash"}]})";
    const auto stt = SttModelChoices(SttProvider::Gemini, gemini);
    CHECK(stt.size() == 3); // no TTS, no embedding model
    const auto tts = TtsModelChoices(TtsProvider::Gemini, gemini);
    REQUIRE(tts.size() == 1);
    CHECK(tts[0].id == "gemini-3.8-flash-tts");
    CHECK(PickSpeechModel(stt, "gemini-3.5-flash-lite", false).empty()); // offered: keep it
    CHECK(PickSpeechModel(stt, "gemini-1.0-pro", false) == "gemini-3.5-flash-lite");
    CHECK(PickSpeechModel(tts, "gemini-2.5-flash-preview-tts", true) == "gemini-3.8-flash-tts");

    const auto openai = SttModelChoices(SttProvider::OpenAI,
        R"({"data":[{"id":"gpt-4o-mini-transcribe"},{"id":"whisper-1"},{"id":"gpt-4.1-mini"}]})");
    CHECK(openai.size() == 2);
    const auto eleven = TtsModelChoices(TtsProvider::ElevenLabs,
        R"([{"model_id":"eleven_flash_v2_5","name":"Flash v2.5","can_do_text_to_speech":true},
            {"model_id":"eleven_english_sts_v2","can_do_text_to_speech":false}])");
    REQUIRE(eleven.size() == 1);
    CHECK(eleven[0].id == "eleven_flash_v2_5");
    CHECK(SttModelChoices(SttProvider::Google, "").size() >= 3); // fixed names, no list endpoint

    HttpResponse privacy;
    privacy.status = 404;
    privacy.body = R"({"error":{"message":"0 endpoints out of 1 requested are available matching your guardrail restrictions and data policy."}})";
    CHECK_THAT(DescribeHttpFailure("OpenRouter", privacy), ContainsSubstring("openrouter.ai/settings/privacy"));
}

TEST_CASE("NpcChat events: a mixed default squad, one joker, reactions with a cooldown", "[npcchat]")
{
    std::set<std::string> personas;
    for (int slot = 2; slot <= 6; ++slot)
        personas.insert(DefaultPersona(slot, "Soldier"));
    CHECK(personas.size() == 5); // five riflemen, five different men
    CHECK(DefaultPersona(3, "Grenadier") != DefaultPersona(3, "Medic"));

    CHECK(EventCooldownSeconds(0) == 0.0);
    CHECK(EventCooldownSeconds(1) > EventCooldownSeconds(2));
    CHECK(EventCooldownSeconds(2) > EventCooldownSeconds(3));
    Settings s;
    CHECK(s.eventChatter == 2);
    CHECK(s.jokerSlot == -1);
    s.eventChatter = 3;
    s.jokerSlot = 4;
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.eventChatter == 3);
    CHECK(back.jokerSlot == 4);
    Settings wild;
    REQUIRE(SettingsFromJson(R"({"eventChatter":9})", wild));
    CHECK(wild.eventChatter == 3);

    const std::string rules = ResponseFormatInstructions(true);
    CHECK_THAT(rules, ContainsSubstring("[GAME EVENT]"));
    CHECK_THAT(rules, ContainsSubstring("SQUAD JOKER"));
    CHECK_THAT(rules, ContainsSubstring("never a joke"));
}

TEST_CASE("NpcChat local models: Ollama's native API with a context window big enough for the prompt", "[npcchat]")
{
    CHECK(OllamaNativeBase("http://localhost:11434/v1/") == "http://localhost:11434");
    CHECK(OllamaNativeBase("http://gpu-box:11434") == "http://gpu-box:11434");

    Settings s;
    s.llm = LlmProvider::Ollama;
    s.llmBaseUrl = "http://localhost:11434/v1/";
    const HttpRequest r = BuildLlmRequest(s, "", "SYSTEM", {{"user", "hi"}});
    CHECK(r.url == "http://localhost:11434/api/chat");
    CHECK(Header(r, "Authorization").empty());
    cJSON* root = cJSON_Parse(r.body.c_str());
    REQUIRE(root);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "stream")));
    CHECK(std::string(cJSON_GetObjectItem(root, "format")->valuestring) == "json");
    const cJSON* options = cJSON_GetObjectItem(root, "options");
    CHECK(cJSON_GetObjectItem(options, "num_ctx")->valueint == 8192);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")) == 2);
    cJSON_Delete(root);

    std::string text, error;
    REQUIRE(ParseLlmResponse(LlmProvider::Ollama,
                             R"({"model":"llama3.1:8b","message":{"role":"assistant","content":"{\"say\":[]}"},"done":true})",
                             text, error));
    CHECK(text == "{\"say\":[]}");
    CHECK_FALSE(ParseLlmResponse(LlmProvider::Ollama, R"({"message":{"content":""},"done_reason":"length"})", text, error));
    CHECK_THAT(error, ContainsSubstring("token budget"));
    CHECK_FALSE(ParseLlmResponse(LlmProvider::Ollama, R"({"error":"model \"x\" not found, try pulling it first"})", text, error));
    CHECK_THAT(error, ContainsSubstring("try pulling"));

    HttpResponse missing;
    missing.status = 404;
    missing.body = R"({"error":"model \"llama3.1:8b\" not found, try pulling it first"})";
    CHECK_THAT(DescribeHttpFailure("Ollama", missing), ContainsSubstring("ollama pull"));
    HttpResponse down;
    down.error = "Couldn't connect to server";
    CHECK_THAT(DescribeHttpFailure("Ollama", down), ContainsSubstring("start it first"));

    // The settings survive; the local speech-to-text default no longer collides with llama-server's 8080.
    s.localContextTokens = 16384;
    Settings back;
    REQUIRE(SettingsFromJson(SettingsToJson(s), back));
    CHECK(back.localContextTokens == 16384);
    CHECK(DefaultSttBaseUrl(SttProvider::Custom) != DefaultLlmBaseUrl(LlmProvider::Custom));
}
