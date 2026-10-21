// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChat.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatGame.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatHttp.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatReply.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSapi.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <Poseidon/Audio/GameVoiceMute.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/Audio/Voice/VoiceBackend.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>

namespace Poseidon::NpcChat
{
namespace
{
constexpr int kMicRate = 16000;
constexpr size_t kTranscriptMax = 200;
constexpr float kRadioDistance = 40.0f;

double Now()
{
    return static_cast<double>(SDL_GetTicks()) / 1000.0;
}

// ---------------------------------------------------------------- worker

class Worker
{
  public:
    ~Worker() { Stop(); }

    void Post(std::function<void()> job)
    {
        Start();
        {
            std::lock_guard lock(_mutex);
            _jobs.push_back(std::move(job));
        }
        _cv.notify_one();
    }

    void Stop()
    {
        {
            std::lock_guard lock(_mutex);
            if (!_thread.joinable())
                return;
            _stop = true;
            _jobs.clear();
        }
        cancel = true;
        _cv.notify_all();
        _thread.join();
        _stop = false;
        cancel = false;
    }

    std::atomic<bool> cancel{false};

  private:
    void Start()
    {
        std::lock_guard lock(_mutex);
        if (_thread.joinable())
            return;
        _thread = std::thread([this] { Run(); });
    }

    void Run()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(_mutex);
                _cv.wait(lock, [this] { return _stop || !_jobs.empty(); });
                if (_stop)
                    return;
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            cancel = false; // a "Stop waiting" applies to the request that was running, not the next
            job();
        }
    }

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<std::function<void()>> _jobs;
    bool _stop = false;
};

Worker& TheWorker()
{
    static Worker worker;
    return worker;
}

// Worker -> game thread.
std::mutex s_resultsMutex;
std::deque<std::function<void()>> s_results;

void PostResult(std::function<void()> fn)
{
    std::lock_guard lock(s_resultsMutex);
    s_results.push_back(std::move(fn));
}

// ---------------------------------------------------------------- game-thread state

std::deque<TranscriptLine> s_transcript;
std::vector<ChatMessage> s_history;
std::deque<double> s_requestTimes;
int s_inFlight = 0;
double s_thinkingSince = 0.0;
std::string s_status;
std::string s_missionKey; // conversation resets when the mission changes

struct Speech
{
    int registryId = 0;
    std::string speaker;
    std::string wav;
    bool radio = false;
    int chorus = 0; // >0: part of a squad answering together; all parts start at once
};
struct Playing
{
    Speech speech;
    Ref<IWave> wave;
    double started = 0.0;
    float length = 0.0f;
};
std::deque<Speech> s_speechQueue;
std::vector<Playing> s_playing;
std::map<int, int> s_chorusPending; // chorus id -> voices still being synthesised
int s_nextChorus = 0;
int s_ttsPending = 0;

LinkStatus s_llmLink, s_sttLink, s_ttsLink;
std::vector<ModelInfo> s_models;
std::vector<VoiceInfo> s_voices;
int s_llmGen = 0, s_sttGen = 0, s_ttsGen = 0; // a newer check makes an older answer stale

// Voice service that refused for good this session (quota used up, payment required, key rejected):
// lines go straight to Windows voices instead of failing one by one. Cleared by CheckTts().
TtsProvider s_ttsBlocked = TtsProvider::Off;
// Single voices a provider refused with "payment required" (ElevenLabs library voices on a free plan):
// skipped from then on, while the provider's other voices keep working.
std::set<std::string> s_badVoices;
std::vector<ModelInfo> s_sttModels, s_ttsModels;

/// Installed Windows voices marked male (all of them if none is): the fallback must not turn a squad
/// of soldiers into the system's default female voice.
const std::vector<std::string>& WindowsMaleVoices()
{
    static std::vector<std::string> male;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        // English male voices first; any male voice next (better a German accent than a woman's
        // voice for a soldier); every voice only when no male one is installed.
        const std::vector<SapiVoiceInfo> all = SapiVoiceList();
        for (const SapiVoiceInfo& v : all)
            if (v.english && (v.gender == "Male" || v.gender == "male"))
                male.push_back(v.name);
        if (male.empty())
            for (const SapiVoiceInfo& v : all)
                if (v.gender == "Male" || v.gender == "male")
                    male.push_back(v.name);
        if (male.empty())
            for (const SapiVoiceInfo& v : all)
                male.push_back(v.name);
    }
    return male;
}

std::unique_ptr<IVoiceCaptureBackend> s_capture;
bool s_talking = false;
bool s_sttTest = false; // this recording is the panel's speech-to-text test, not a message
std::vector<int16_t> s_recording;
float s_micLevel = 0.0f;

std::vector<std::string> s_windowsVoices;
bool s_windowsVoicesLoaded = false;

void AddLine(LineKind kind, const std::string& who, const std::string& text)
{
    s_transcript.push_back({kind, who, RedactSecrets(text), Now()});
    while (s_transcript.size() > kTranscriptMax)
        s_transcript.pop_front();
}

void ReportError(const std::string& message)
{
    const std::string clean = RedactSecrets(message);
    static std::string s_lastError;
    static double s_lastErrorTime = -100.0;
    if (clean == s_lastError && Now() - s_lastErrorTime < 20.0)
        return;
    s_lastError = clean;
    s_lastErrorTime = Now();
    s_status = clean;
    AddLine(LineKind::Error, "", clean);
    LOG_WARN(Audio, "NPC chat: {}", clean);
}

bool RateLimited(const Settings& s)
{
    const double now = Now();
    while (!s_requestTimes.empty() && now - s_requestTimes.front() > 60.0)
        s_requestTimes.pop_front();
    if (s.maxRequestsPerMinute > 0 && static_cast<int>(s_requestTimes.size()) >= s.maxRequestsPerMinute)
        return true;
    s_requestTimes.push_back(now);
    return false;
}

std::string PickVoice(const Settings& s, const SpeakerInfo& who)
{
    std::vector<std::string> pool = SplitList(s.EffectiveTtsVoices());
    if (pool.empty() && s.tts == TtsProvider::WindowsSapi)
        pool = WindowsMaleVoices();
    if (pool.empty() && s.tts != TtsProvider::WindowsSapi)
    {
        // No pool set (ElevenLabs by default): the account's own voices, male ones first.
        for (const VoiceInfo& v : s_voices)
            if (IsMaleVoice(v))
                pool.push_back(v.id);
        if (pool.empty())
            for (const VoiceInfo& v : s_voices)
                pool.push_back(v.id);
    }
    if (pool.empty())
        return {};
    pool.erase(std::remove_if(pool.begin(), pool.end(), [](const std::string& v) { return s_badVoices.count(v) > 0; }),
               pool.end());
    if (pool.empty())
        return {};
    if (who.inSquad)
        if (std::string chosen = s.SquadVoiceFor(std::atoi(who.id.c_str())); !chosen.empty() && !s_badVoices.count(chosen))
            return chosen; // picked in the panel (or by Auto-assign)
    if (who.isHq || pool.size() == 1)
        return pool[0];
    // HQ keeps voice 0 to itself so it is always recognisable.
    const size_t n = pool.size() - 1;
    return pool[1 + static_cast<size_t>(who.voiceSeed < 0 ? -who.voiceSeed : who.voiceSeed) % n];
}

std::string VoiceInstructions(const SpeakerInfo& who)
{
    if (who.isHq)
        return "Calm, clipped radio operator at a 1985 military headquarters. Terse and authoritative.";
    if (who.inSquad)
        return "A soldier in the field talking to his squad leader. Natural, a little out of breath, terse.";
    return "A person talking face to face in the countryside. Natural and conversational.";
}

/// Queue text-to-speech for one line (game thread); the WAV comes back into s_speechQueue.
/// Returns false when nothing was queued.
bool Speak(const Settings& s, int registryId, const std::string& speakerId, const std::string& text, int chorus = 0,
           const std::string& voiceOverride = {})
{
    if (s.tts == TtsProvider::Off || text.empty())
        return false;
    const SpeakerInfo who = ResolveSpeaker(registryId, speakerId);
    if (!who.valid)
        return false;
    // A voice preview (voiceOverride) plays clean, so the player hears the voice itself.
    const bool radio = voiceOverride.empty() && (who.isHq || (s.ttsRadioWhenFar && who.distance > kRadioDistance));
    const std::string voice = voiceOverride.empty() ? PickVoice(s, who) : voiceOverride;
    const std::string instructions =
        chorus ? std::string("A soldier in a squad shouting in unison with the others, loud and sharp.")
               : VoiceInstructions(who);
    const std::string key = GetKey(TtsKeySlot(s.tts));
    // Without a key the line is still voiced: by a Windows voice, one per soldier, until a key is set
    // (the panel's voice light says so). A cloud failure falls back the same way, per line.
    const bool cloud = s.tts != TtsProvider::WindowsSapi && s.tts != TtsProvider::Custom;
    const bool useWindows =
        s.tts == TtsProvider::WindowsSapi || (cloud && key.empty()) || (cloud && s_ttsBlocked == s.tts);
    std::string windowsVoice = s.tts == TtsProvider::WindowsSapi ? voice : std::string();
    if (s.tts != TtsProvider::WindowsSapi && !WindowsMaleVoices().empty())
    {
        // Fallback: a male Windows voice, the same one for the same soldier every time.
        const std::vector<std::string>& male = WindowsMaleVoices();
        windowsVoice = male[static_cast<size_t>(who.voiceSeed < 0 ? -who.voiceSeed : who.voiceSeed) % male.size()];
    }
    ++s_ttsPending;
    if (chorus)
        ++s_chorusPending[chorus];
    TheWorker().Post(
        [s, registryId, speakerId, text, voice, windowsVoice, instructions, key, radio, chorus, useWindows]
        {
            std::string wav, error;
            bool block = false;
            bool badVoice = false;
            if (!useWindows)
            {
                const HttpResponse response =
                    HttpPost(BuildTtsRequest(s, key, text, voice, instructions), &TheWorker().cancel);
                if (!response.Ok())
                    error = DescribeHttpFailure("Text-to-speech", response);
                else
                    ParseTtsResponse(s.tts, response, wav, error);
                // Refusals that will not change within the session: stop asking.
                const bool quota = response.status == 429 && (response.body.find("quota") != std::string::npos ||
                                                              response.body.find("Quota") != std::string::npos);
                // 402 is about one voice (a library voice on a free plan), not the whole provider.
                block = response.status == 401 || response.status == 403 || quota;
                badVoice = response.status == 402;
            }
            if (wav.empty())
            {
                std::string sapiError;
                SapiSpeakToWav(text, windowsVoice, chorus ? 2 : 1, wav, sapiError);
                LOG_INFO(Audio, "NPC chat: Windows voice '{}' for speaker {}", windowsVoice.empty() ? "default" : windowsVoice,
                         speakerId);
                if (!error.empty() && !wav.empty())
                    error += "\n(This line used a Windows voice instead.)";
                else if (error.empty())
                    error = sapiError;
            }
            if (radio && !wav.empty())
                wav = ApplyRadioFilter(wav);
            PostResult(
                [registryId, speakerId, wav = std::move(wav), error, radio, chorus, block, badVoice, voice,
                 provider = s.tts]() mutable
                {
                    --s_ttsPending;
                    if (chorus)
                        --s_chorusPending[chorus];
                    if (block && s_ttsBlocked != provider)
                    {
                        s_ttsBlocked = provider;
                        const std::string why = error.substr(0, error.find("\n(This line"));
                        s_ttsLink = {LinkState::Failed, why + "\nUsing male Windows voices for the rest of this "
                                                              "session. Press \"Check and load voices\" to try again."};
                        ReportError(std::string(TtsProviderName(provider)) +
                                    " refused for this session; male Windows voices take over. " + why);
                    }
                    else if (badVoice)
                    {
                        if (s_badVoices.insert(voice).second)
                            ReportError("The voice \"" + voice + "\" needs a paid " + TtsProviderName(provider) +
                                        " plan (a library voice); it is skipped from now on. Your other voices "
                                        "keep working. Pick \"premade\" or your own voices for free use.");
                    }
                    else if (!error.empty() && !block)
                        ReportError(error);
                    if (wav.empty())
                    {
                        if (error.empty())
                            ReportError("Text-to-speech returned no audio");
                        return;
                    }
                    s_speechQueue.push_back({registryId, speakerId, std::move(wav), radio, chorus});
                });
        });
    return true;
}

void StartSpeech(Speech speech, float volume)
{
    // 3D only when the speaker has a place in the world (not HQ, not a voice preview); decided before
    // the wave exists, because a 3D wave without a position never starts.
    float x = 0, y = 0, z = 0;
    const bool positional =
        !speech.radio && SpeakerPosition(speech.registryId, speech.speaker, x, y, z);
    // The extension needs its dot: SoundLoadMemory reads anything that is not ".wav"/".ogg" as a raw
    // WSS stream, which turned every voice line into a 0-second silent wave.
    IWave* wave = GSoundsys->CreateWaveFromMemory(speech.wav.data(), speech.wav.size(), ".wav", positional);
    if (!wave || wave->GetLength() <= 0.0f)
    {
        ReportError("The audio engine could not play the speech (unreadable WAV).");
        return;
    }
    Playing playing{std::move(speech), wave, Now(), wave->GetLength()};
    playing.wave->SetKind(WaveSpeech);
    playing.wave->SetVolume(std::clamp(volume, 0.0f, 4.0f));
    if (positional)
        playing.wave->SetPosition(Vector3(x, y, z), VZero, true);
    playing.wave->Repeat(1);
    playing.wave->Play();
    GSoundsys->Commit(); // start it this frame rather than waiting for the world's own commit
    LOG_INFO(Audio, "NPC chat: speech start speaker={} bytes={} length={:.2f}s 3d={} radio={} volume={:.2f}",
             playing.speech.speaker, playing.speech.wav.size(), playing.length, playing.wave->Get3D(),
             playing.speech.radio, playing.wave->GetVolume());
    s_playing.push_back(std::move(playing));
}

void UpdateSpeech()
{
    if (!GSoundsys)
    {
        s_speechQueue.clear();
        s_playing.clear();
        return;
    }
    // Finished voices drop out; the rest follow their speaker as he moves.
    for (size_t i = 0; i < s_playing.size();)
    {
        Playing& p = s_playing[i];
        if (p.wave->IsTerminated() || p.wave->IsStopped())
        {
            const double ran = Now() - p.started;
            // A wave that "ends" long before its length never really played: say so in the log.
            if (ran + 0.25 < p.length)
                LOG_WARN(Audio, "NPC chat: speech ended after {:.2f}s of {:.2f}s (state {}) -- not audible",
                         ran, p.length, static_cast<int>(p.wave->State()));
            else
                LOG_INFO(Audio, "NPC chat: speech finished after {:.2f}s", ran);
            s_playing.erase(s_playing.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        float x, y, z;
        if (!p.speech.radio && SpeakerPosition(p.speech.registryId, p.speech.speaker, x, y, z))
            p.wave->SetPosition(Vector3(x, y, z), VZero);
        ++i;
    }
    if (!s_playing.empty() || s_speechQueue.empty())
        return; // one line (or one chorus) at a time, in order

    const Settings& s = CurrentSettings();
    const int chorus = s_speechQueue.front().chorus;
    if (chorus == 0)
    {
        Speech next = std::move(s_speechQueue.front());
        s_speechQueue.pop_front();
        StartSpeech(std::move(next), s.ttsVolume);
        return;
    }
    // A chorus waits for every voice, then all of them start in the same frame.
    if (s_chorusPending[chorus] > 0)
        return;
    s_chorusPending.erase(chorus);
    for (auto it = s_speechQueue.begin(); it != s_speechQueue.end();)
    {
        if (it->chorus == chorus)
        {
            StartSpeech(std::move(*it), s.ttsVolume * 0.85f);
            it = s_speechQueue.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::string SystemPrompt(const Settings& s, const std::string& report)
{
    return s.EffectiveSystemPrompt() + ToneInstructions(s.tone) + ResponseFormatInstructions(s.allowOrders) + "\n\n" +
           report;
}

void TrimHistory(const Settings& s)
{
    const size_t keep = static_cast<size_t>(std::max(1, s.historyTurns)) * 2;
    if (s_history.size() > keep)
        s_history.erase(s_history.begin(), s_history.end() - static_cast<std::ptrdiff_t>(keep));
}

/// The model's answer arrives (game thread).
void HandleReply(const Settings& s, int registryId, const std::string& defaultSpeaker, const std::string& text,
                 bool remember, bool eventTurn = false)
{
    if (remember)
    {
        s_history.push_back({"assistant", text});
        TrimHistory(s);
    }

    Reply reply;
    ParseReply(text, defaultSpeaker, reply);
    for (const SpokenLine& line : reply.lines)
    {
        if (line.speaker == "ALL")
        {
            // The whole squad at once ("Sir, yes sir!"): every living squad mate, in his own voice.
            AddLine(LineKind::Npc, "Squad (all)", line.text);
            PostChatLine(registryId, "ALL", line.text, false);
            const int chorus = ++s_nextChorus;
            int voices = 0;
            for (const SquadSlot& slot : CurrentSquad())
                if (!slot.isPlayer && !slot.dead && voices < (s.tts == TtsProvider::Gemini ? 2 : 6) &&
                    Speak(s, registryId, std::to_string(slot.id), line.text, chorus))
                    ++voices;
            continue;
        }
        // A speaker the report does not know -- or the player himself ("1"), whom ResolveSpeaker never
        // returns -- is replaced by the default answerer, so nobody ever talks with the player's mouth.
        SpeakerInfo who = ResolveSpeaker(registryId, line.speaker);
        const std::string speaker = who.valid ? line.speaker : defaultSpeaker;
        if (!who.valid)
            who = ResolveSpeaker(registryId, defaultSpeaker);
        std::string label = who.valid ? who.name : speaker;
        if (who.inSquad)
            label = who.id + " " + label;
        AddLine(LineKind::Npc, label, line.text);
        PostChatLine(registryId, speaker, line.text, false);
        Speak(s, registryId, speaker, line.text);
    }
    if (s.allowOrders && !eventTurn) // a remark about an event never moves anyone
    {
        for (const std::string& summary : ExecuteOrders(registryId, reply.orders))
            AddLine(LineKind::Order, "order", summary);
    }
    for (const std::string& rejected : reply.rejected)
        AddLine(LineKind::System, "", "ignored an order the game does not understand: " + rejected);
    if (reply.classifier)
        AddLine(LineKind::Error, "",
                "The model \"" + s.EffectiveLlmModel() + "\" is a content-safety classifier: it only answers "
                "\"safe\" / \"unsafe\" and cannot play the squad. Pick a chat model in the Language model list.");
    if (reply.truncated)
        AddLine(LineKind::Error, "",
                "The model's answer was cut off before it finished, so any orders in it were lost. Raise \"Max answer "
                "tokens\" (Language model), or pick a model that does not \"think\" first.");
    s_status.clear();
}

void SendToModel(const Settings& s, const std::string& system, const std::vector<ChatMessage>& messages,
                 int registryId, const std::string& defaultSpeaker, bool remember = true, bool eventTurn = false)
{
    const std::string key = GetKey(LlmKeySlot(s.llm));
    if (key.empty() && s.llm != LlmProvider::Ollama && s.llm != LlmProvider::Custom)
    {
        ReportError(std::string("No API key for ") + LlmProviderName(s.llm) +
             ". Enter it in the dev panel (NPC Chat tab) or set " +
             (KeySlotEnvVar(LlmKeySlot(s.llm)) ? KeySlotEnvVar(LlmKeySlot(s.llm)) : "the key") + ".");
        return;
    }
    ++s_inFlight;
    s_status = "Thinking...";
    s_thinkingSince = Now();
    const double started = Now();
    TheWorker().Post(
        [s, system, messages, key, registryId, defaultSpeaker, remember, started, eventTurn]
        {
            const HttpRequest request = BuildLlmRequest(s, key, system, messages);
            HttpResponse response;
            for (int attempt = 1; attempt <= 2; ++attempt)
            {
                const double t0 = Now();
                response = HttpPost(request, &TheWorker().cancel);
                // One line per request, so "are we flooding the provider?" is answerable from the log.
                LOG_INFO(Audio, "NPC chat: request {} to {} / {}: {} in {:.1f} s ({} bytes sent)", attempt,
                         LlmProviderName(s.llm), s.EffectiveLlmModel(),
                         response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error,
                         Now() - t0, request.body.size());
                // Free models are often briefly overloaded upstream: one retry after a short pause.
                const bool transient = response.error.empty() &&
                                       (response.status == 429 || response.status == 502 || response.status == 503 ||
                                        response.status == 504);
                if (!transient || attempt == 2)
                    break;
                for (int i = 0; i < 40 && !TheWorker().cancel.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (TheWorker().cancel.load())
                    break;
            }
            std::string text, error;
            if (!response.Ok())
                error = DescribeHttpFailure(LlmProviderName(s.llm), response);
            else if (!ParseLlmResponse(s.llm, response.body, text, error))
                error = std::string(LlmProviderName(s.llm)) + ": " + error;
            // Only a rejected key or an unreachable server is "not connected"; a busy or confused model
            // is a connected one that failed this time.
            const bool connectionFailed =
                response.status == 401 || response.status == 403 ||
                (!response.error.empty() && response.error.find("Timeout") == std::string::npos &&
                 response.error != "cancelled");
            PostResult(
                [s, registryId, defaultSpeaker, text, error, remember, started, connectionFailed, eventTurn]
                {
                    --s_inFlight;
                    char took[96];
                    std::snprintf(took, sizeof(took), " -- answered in %.1f s", Now() - started);
                    if (error.find(": cancelled") != std::string::npos)
                    {
                        // The player pressed Stop waiting: not a connection failure.
                        if (remember && !s_history.empty() && s_history.back().role == "user")
                            s_history.pop_back();
                        AddLine(LineKind::System, "", "Stopped waiting for the answer.");
                        s_status.clear();
                        return;
                    }
                    if (error.empty())
                        s_llmLink = {LinkState::Ok, std::string("Connected: ") + LlmProviderName(s.llm) + " / " +
                                                        s.EffectiveLlmModel() + took};
                    else if (connectionFailed)
                        s_llmLink = {LinkState::Failed, error};
                    else
                        s_llmLink = {LinkState::Ok, std::string("Connected to ") + LlmProviderName(s.llm) +
                                                        ", but the last answer failed: " + error};
                    if (!error.empty())
                    {
                        // Drop the unanswered user turn so the conversation stays well-formed.
                        if (remember && !s_history.empty() && s_history.back().role == "user")
                            s_history.pop_back();
                        ReportError(error);
                        return;
                    }
                    HandleReply(s, registryId, defaultSpeaker, text, remember, eventTurn);
                });
        });
}

void ResetIfMissionChanged()
{
    const std::string key = MissionKey();
    if (key.empty())
        return;
    if (s_missionKey != key)
    {
        if (!s_missionKey.empty())
            ClearConversation();
        s_missionKey = key;
    }
}

void UpdateCapture()
{
    if (!s_capture || !s_capture->isCapturing())
        return;
    int16_t buffer[4096];
    for (;;)
    {
        const int available = s_capture->availableSamples();
        if (available <= 0)
            break;
        const int got = s_capture->read(buffer, std::min(available, 4096));
        if (got <= 0)
            break;
        s_recording.insert(s_recording.end(), buffer, buffer + got);
    }
    s_micLevel = s_capture->lastFramePeak();
    // Hard cap at 30 s: a stuck key must not upload minutes of audio.
    if (s_recording.size() > static_cast<size_t>(kMicRate) * 30)
        EndTalk();
}
} // namespace

// ---------------------------------------------------------------- public API

// Spontaneous reactions: events are gathered twice a second and, when the model is free, the
// cooldown has passed and the rate limit allows, sent as one [GAME EVENT] turn.
std::vector<std::string> s_pendingEvents;
double s_lastEventPoll = 0.0;
double s_lastEventTurn = -1000.0;

void UpdateEvents()
{
    const Settings& s = CurrentSettings();
    const double now = Now();
    if (!s.enabled || now - s_lastEventPoll < 0.5)
        return;
    s_lastEventPoll = now;
    const std::vector<std::string> events = PollSquadEvents(); // keeps its baseline even when muted
    if (s.eventChatter <= 0)
    {
        s_pendingEvents.clear();
        return;
    }
    s_pendingEvents.insert(s_pendingEvents.end(), events.begin(), events.end());
    if (s_pendingEvents.size() > 4)
        s_pendingEvents.erase(s_pendingEvents.begin(), s_pendingEvents.end() - 4);
    if (s_pendingEvents.empty() || s_inFlight > 0 || s_talking)
        return;
    const bool death = std::any_of(s_pendingEvents.begin(), s_pendingEvents.end(),
                                   [](const std::string& e) { return e.find("was killed") != std::string::npos; });
    if (!death && now - s_lastEventTurn < EventCooldownSeconds(s.eventChatter))
    {
        // Too soon: keep only the latest few, and drop stale news after a while.
        if (now - s_lastEventTurn > 120.0)
            s_pendingEvents.clear();
        return;
    }
    const std::string key = GetKey(LlmKeySlot(s.llm));
    if (key.empty() && s.llm != LlmProvider::Ollama && s.llm != LlmProvider::Custom)
        return; // not set up yet: say nothing rather than an error per event
    if (RateLimited(s))
        return;
    ReportResult report = BuildSituationReport(s);
    if (!report.ok)
        return;
    std::string text = "[GAME EVENT]";
    for (const std::string& e : s_pendingEvents)
        text += " " + e;
    s_pendingEvents.clear();
    s_lastEventTurn = now;
    AddLine(LineKind::System, "event", text.substr(13));
    s_history.push_back({"user", text});
    TrimHistory(s);
    const std::string speaker = JokerSlot() > 0 ? std::to_string(JokerSlot()) : report.defaultSpeaker;
    SendToModel(s, SystemPrompt(s, report.text), s_history, report.registryId, speaker, true, true);
}

void Update()
{
    {
        const Settings& s = CurrentSettings();
        SetGameVoicesMuted(s.enabled && s.tts != TtsProvider::Off && s.muteGameVoices);
        static bool loggingWas = false;
        if (s.logTraffic != loggingWas)
        {
            loggingWas = s.logTraffic;
            SetTrafficLogPath(s.logTraffic ? TrafficLogPath() : std::string());
        }
    }
    std::deque<std::function<void()>> results;
    {
        std::lock_guard lock(s_resultsMutex);
        results.swap(s_results);
    }
    for (auto& fn : results)
        fn();
    UpdateCapture();
    UpdateSpeech();
    UpdateEvents();
}

void Shutdown()
{
    TheWorker().Stop();
    std::lock_guard lock(s_resultsMutex);
    s_results.clear();
    s_playing.clear();
    s_speechQueue.clear();
    if (s_capture)
    {
        s_capture->stop();
        s_capture->close();
        s_capture.reset();
    }
    ClearAllKeys();
}

void SubmitText(const std::string& raw)
{
    std::string text = raw;
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
        text.pop_back();
    if (text.empty())
        return;
    const Settings& s = CurrentSettings();
    if (!s.enabled)
    {
        ReportError("NPC chat is switched off (dev panel -> NPC Chat).");
        return;
    }
    LoadRememberedKeys();
    ResetIfMissionChanged();
    if (s_inFlight > 0)
    {
        ReportError("Still waiting for the last answer.");
        return;
    }
    if (RateLimited(s))
    {
        ReportError("Rate limit reached (" + std::to_string(s.maxRequestsPerMinute) + " per minute). Wait a moment.");
        return;
    }
    ReportResult report = BuildSituationReport(s);
    if (!report.ok)
    {
        ReportError(report.error);
        return;
    }
    AddLine(LineKind::Player, "You", text);
    PostChatLine(report.registryId, "", text, true);
    s_history.push_back({"user", text});
    TrimHistory(s);
    SendToModel(s, SystemPrompt(s, report.text), s_history, report.registryId, report.defaultSpeaker);
}

void BeginTalkImpl(bool test);

void BeginTalk()
{
    BeginTalkImpl(false);
}

void BeginSttTest()
{
    BeginTalkImpl(true);
}

void BeginTalkImpl(bool test)
{
    const Settings& s = CurrentSettings();
    if ((!s.enabled && !test) || s_talking)
        return;
    s_sttTest = test;
    if (s.stt == SttProvider::Off)
    {
        ReportError("Speech-to-text is off: choose a provider in the NPC Chat tab, or type instead.");
        return;
    }
    StopSpeech(); // talking over a soldier interrupts him, as on a real radio
    if (!s_capture)
    {
        const VoiceBackendDescriptor& backend = GetSelectedVoiceBackend();
        if (backend.createCapture)
            s_capture = backend.createCapture();
    }
    if (!s_capture)
    {
        ReportError("No microphone backend available.");
        return;
    }
    if (!s_capture->isOpen() && !s_capture->open(nullptr, kMicRate, kMicRate * 2))
    {
        s_capture.reset();
        ReportError("Could not open the microphone.");
        return;
    }
    s_recording.clear();
    s_capture->start();
    s_talking = true;
    s_status = test ? "Listening (speech-to-text test)..." : "Listening...";
}

void EndTalk(bool cancel)
{
    if (!s_talking)
        return;
    s_talking = false;
    s_micLevel = 0.0f;
    if (s_capture)
    {
        UpdateCapture();
        s_capture->stop();
    }
    s_status.clear();
    if (cancel)
        return;
    if (s_recording.size() < static_cast<size_t>(kMicRate) * 3 / 10)
    {
        s_status = "Too short -- hold the talk key while speaking.";
        return;
    }
    const Settings& s = CurrentSettings();
    LoadRememberedKeys();
    const std::string key = GetKey(SttKeySlot(s.stt));
    if (key.empty() && s.stt != SttProvider::Custom)
    {
        ReportError(std::string("No API key for ") + SttProviderName(s.stt) + " (speech-to-text).");
        return;
    }
    std::string wav = MakeWav(s_recording.data(), s_recording.size(), kMicRate);
    s_recording.clear();
    ++s_inFlight;
    s_status = "Transcribing...";
    const bool test = s_sttTest;
    const double seconds = static_cast<double>(wav.size() - 44) / (2.0 * kMicRate);
    TheWorker().Post(
        [s, key, wav = std::move(wav), test, seconds]
        {
            const double started = Now();
            HttpResponse response = HttpPost(BuildSttRequest(s, key, wav, kMicRate), &TheWorker().cancel);
            if ((response.status == 429 || response.status == 503) && !TheWorker().cancel.load())
            {
                std::this_thread::sleep_for(std::chrono::seconds(3)); // "high demand": one retry
                response = HttpPost(BuildSttRequest(s, key, wav, kMicRate), &TheWorker().cancel);
            }
            std::string text, error;
            if (!response.Ok())
                error = DescribeHttpFailure(SttProviderName(s.stt), response);
            else
                ParseSttResponse(s.stt, response.body, text, error);
            const double took = Now() - started;
            PostResult(
                [s, text, error, test, seconds, took]
                {
                    --s_inFlight;
                    s_status.clear();
                    if (!error.empty())
                    {
                        s_sttLink = {LinkState::Failed, error};
                        ReportError("Speech-to-text: " + error);
                        return;
                    }
                    char how[128];
                    std::snprintf(how, sizeof(how), " (%.1f s of speech, transcribed in %.1f s)", seconds, took);
                    s_sttLink = {LinkState::Ok, std::string("Connected to ") + SttProviderName(s.stt) +
                                                    " -- last transcription" + how};
                    if (test)
                    {
                        // The test stops here: the player sees what was heard; nothing goes to the model.
                        AddLine(LineKind::System, "Speech-to-text test", "heard: \"" + text + "\"" + how);
                        return;
                    }
                    SubmitText(text);
                });
        });
}

bool IsTalking()
{
    return s_talking;
}

float MicLevel()
{
    return s_micLevel;
}

bool Busy()
{
    return s_inFlight > 0;
}

std::string StatusLine()
{
    if (s_talking)
        return "Listening...";
    if (s_status == "Thinking..." && s_inFlight > 0)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Thinking... %.0f s (gives up after %d s)", Now() - s_thinkingSince,
                      CurrentSettings().requestTimeoutSeconds);
        return buf;
    }
    if (!s_status.empty())
        return s_status;
    if (s_ttsPending > 0)
        return "Voicing...";
    return {};
}

const std::deque<TranscriptLine>& Transcript()
{
    return s_transcript;
}

void ClearConversation()
{
    s_history.clear();
    s_transcript.clear();
    s_status.clear();
    StopSpeech();
}

void CancelRequest()
{
    // Aborts the network call in flight; its result arrives as "cancelled" and is dropped quietly.
    if (s_inFlight > 0)
        TheWorker().cancel = true;
}

void StopSpeech()
{
    s_speechQueue.clear();
    s_chorusPending.clear();
    for (Playing& p : s_playing)
        p.wave->Stop();
    s_playing.clear();
}

void TestLlm()
{
    const Settings& s = CurrentSettings();
    LoadRememberedKeys();
    if (s_inFlight > 0)
        return;
    AddLine(LineKind::System, "", std::string("Testing ") + LlmProviderName(s.llm) + " / " + s.EffectiveLlmModel() +
                                      "...");
    const std::vector<ChatMessage> messages{{"user", "Radio check, how do you read me?"}};
    const std::string system = "You are HQ in a 1985 military simulation." + ResponseFormatInstructions(false) +
                               "\nSpeaker ids available: \"HQ\".";
    // Registry 0: no world needed; HQ resolves without one.
    SendToModel(s, system, messages, 0, "HQ", false); // a test is not part of the mission conversation
}

void TestVoice()
{
    const Settings& s = CurrentSettings();
    LoadRememberedKeys();
    if (s.tts == TtsProvider::Off)
    {
        ReportError("Text-to-speech is off.");
        return;
    }
    AddLine(LineKind::System, "", std::string("Testing voice: ") + TtsProviderName(s.tts));
    Speak(s, 0, "HQ", "Papa Bear to all units. Radio check, over.");
}

const std::vector<std::string>& WindowsVoices()
{
    if (!s_windowsVoicesLoaded)
    {
        s_windowsVoicesLoaded = true;
        s_windowsVoices = SapiVoices();
    }
    return s_windowsVoices;
}


// ---------------------------------------------------------------- connection status and discovery

const LinkStatus& LlmStatus()
{
    return s_llmLink;
}

const LinkStatus& SttStatus()
{
    return s_sttLink;
}

const LinkStatus& TtsStatus()
{
    return s_ttsLink;
}

const std::vector<ModelInfo>& Models()
{
    return s_models;
}

const std::vector<VoiceInfo>& Voices()
{
    return s_voices;
}

const std::vector<ModelInfo>& SttModels()
{
    return s_sttModels;
}

const std::vector<ModelInfo>& TtsModels()
{
    return s_ttsModels;
}

void CheckLlm()
{
    const Settings s = CurrentSettings();
    LoadRememberedKeys();
    const int gen = ++s_llmGen;
    const std::string key = GetKey(LlmKeySlot(s.llm));
    if (key.empty() && s.llm != LlmProvider::Ollama && s.llm != LlmProvider::Custom)
    {
        s_llmLink = {LinkState::Failed, std::string("No API key for ") + LlmProviderName(s.llm) + " yet."};
        return;
    }
    s_llmLink = {LinkState::Checking, std::string("Checking ") + LlmProviderName(s.llm) + "..."};
    TheWorker().Post(
        [s, key, gen]
        {
            LinkStatus status{LinkState::Ok, ""};
            std::string keyNote;
            HttpRequest check;
            if (BuildKeyCheckRequest(s, key, check))
            {
                const HttpResponse r = HttpPost(check, &TheWorker().cancel);
                if (!r.Ok())
                    status = {LinkState::Failed, DescribeHttpFailure(LlmProviderName(s.llm), r)};
                else
                    keyNote = DescribeKeyCheck(r.body);
            }
            std::vector<ModelInfo> models;
            std::string pick;
            if (status.state == LinkState::Ok)
            {
                const HttpResponse r = HttpPost(BuildModelListRequest(s, key), &TheWorker().cancel);
                std::string error;
                if (!r.Ok())
                    status = {LinkState::Failed, DescribeHttpFailure(LlmProviderName(s.llm), r)};
                else if (!ParseModelList(s.llm, r.body, models, error))
                    status = {LinkState::Failed, std::string(LlmProviderName(s.llm)) + ": " + error};
            }
            if (status.state == LinkState::Ok)
            {
                const std::string model = s.EffectiveLlmModel();
                const bool known = std::any_of(models.begin(), models.end(),
                                               [&](const ModelInfo& m) { return m.id == model; });
                status.message = std::string("Connected to ") + LlmProviderName(s.llm) +
                                 (keyNote.empty() ? std::string() : " (" + keyNote + ")") + " -- " +
                                 std::to_string(models.size()) + " models available";
                // A local server lists exactly what is installed: use one of those rather than a
                // default name the player never downloaded.
                const bool local = s.llm == LlmProvider::Ollama || s.llm == LlmProvider::Custom;
                if (!known && local)
                    for (const ModelInfo& m : models)
                        if (m.id.find("embed") == std::string::npos)
                        {
                            pick = m.id;
                            break;
                        }
                if (!pick.empty())
                    status.message += ". Using \"" + pick + "\"" +
                                      (model.empty() ? std::string() : " (\"" + model + "\" is not installed)") + ".";
                else if (!known && !model.empty())
                    status.message += ". WARNING: \"" + model + "\" is not one of them; pick one from the list.";
            }
            PostResult(
                [gen, status, pick, models = std::move(models)]() mutable
                {
                    if (gen != s_llmGen)
                        return;
                    if (!pick.empty())
                    {
                        CurrentSettings().llmModel = pick;
                        SaveSettings();
                    }
                    s_llmLink = status;
                    if (!models.empty())
                        s_models = std::move(models);
                });
        });
}

void CheckStt()
{
    const Settings s = CurrentSettings();
    LoadRememberedKeys();
    const int gen = ++s_sttGen;
    if (s.stt == SttProvider::Off)
    {
        s_sttLink = {};
        return;
    }
    const std::string key = GetKey(SttKeySlot(s.stt));
    if (key.empty() && s.stt != SttProvider::Custom)
    {
        s_sttLink = {LinkState::Failed, std::string("No API key for ") + SttProviderName(s.stt) + " yet."};
        return;
    }
    s_sttLink = {LinkState::Checking, std::string("Checking ") + SttProviderName(s.stt) + "..."};
    TheWorker().Post(
        [s, key, gen]
        {
            HttpResponse r = HttpPost(BuildSttCheckRequest(s, key), &TheWorker().cancel);
            LinkStatus status =
                r.Ok() ? LinkStatus{LinkState::Ok, std::string("Connected to ") + SttProviderName(s.stt) +
                                                       " -- hold the talk key and speak"}
                       : LinkStatus{LinkState::Failed, DescribeHttpFailure(SttProviderName(s.stt), r)};
            std::string listBody = r.Ok() ? r.body : std::string();
            if (s.stt == SttProvider::Custom) // the check was a silent transcription: ask for the list too
            {
                HttpRequest list;
                list.get = true;
                list.timeoutSeconds = 20;
                list.url = JoinUrl(s.EffectiveSttBaseUrl(), "models");
                if (!key.empty())
                    list.headers.emplace_back("Authorization", "Bearer " + key);
                const HttpResponse lr = HttpPost(list, &TheWorker().cancel);
                listBody = lr.Ok() ? lr.body : std::string();
            }
            std::vector<ModelInfo> models = SttModelChoices(s.stt, listBody);
            PostResult(
                [gen, status, models = std::move(models)]() mutable
                {
                    if (gen != s_sttGen)
                        return;
                    s_sttModels = std::move(models);
                    Settings& live = CurrentSettings();
                    const std::string current = live.EffectiveSttModel();
                    // A self-hosted server's list may be partial: never switch its model by itself.
                    if (const std::string pick = live.stt == SttProvider::Custom
                                                     ? std::string()
                                                     : PickSpeechModel(s_sttModels, current, false);
                        !pick.empty())
                    {
                        live.sttModel = pick;
                        SaveSettings();
                        status.message += "\n\"" + current + "\" is not available to this key; switched to \"" +
                                          pick + "\".";
                    }
                    s_sttLink = status;
                });
        });
}

void CheckTts()
{
    const Settings s = CurrentSettings();
    LoadRememberedKeys();
    s_ttsBlocked = TtsProvider::Off;
    const int gen = ++s_ttsGen;
    s_voices.clear();
    if (s.tts == TtsProvider::Off)
    {
        s_ttsLink = {};
        return;
    }
    if (s.tts == TtsProvider::WindowsSapi)
    {
        for (const auto& [v, gender] : SapiVoicesWithGender())
        {
            std::string g = gender;
            for (char& c : g)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s_voices.push_back({v, v, g.empty() ? std::string("Windows") : g + ", Windows"});
        }
        s_ttsLink = s_voices.empty()
                        ? LinkStatus{LinkState::Failed, "No Windows voices are installed."}
                        : LinkStatus{LinkState::Ok, std::to_string(s_voices.size()) + " Windows voices, no key needed"};
        return;
    }
    const std::string key = GetKey(TtsKeySlot(s.tts));
    if (key.empty() && s.tts != TtsProvider::Custom)
    {
        s_ttsLink = {LinkState::Failed, std::string("No API key for ") + TtsProviderName(s.tts) +
                                            " yet -- Windows voices speak until you set one."};
        return;
    }
    s_ttsLink = {LinkState::Checking, std::string("Checking ") + TtsProviderName(s.tts) + "..."};
    TheWorker().Post(
        [s, key, gen]
        {
            LinkStatus status{LinkState::Ok, ""};
            std::vector<VoiceInfo> voices = StaticVoices(s.tts);
            HttpRequest req;
            if (BuildVoiceListRequest(s, key, req))
            {
                const HttpResponse r = HttpPost(req, &TheWorker().cancel);
                std::string error;
                if (!r.Ok())
                    status = {LinkState::Failed, DescribeHttpFailure(TtsProviderName(s.tts), r)};
                else if (!ParseVoiceList(s.tts, r.body, voices, error))
                    status = {LinkState::Failed, std::string(TtsProviderName(s.tts)) + ": " + error};
            }
            std::string modelBody;
            if (BuildTtsKeyCheckRequest(s, key, req)) // OpenAI / Gemini: the key check is the model list
            {
                const HttpResponse r = HttpPost(req, &TheWorker().cancel);
                if (!r.Ok())
                    status = {LinkState::Failed, DescribeHttpFailure(TtsProviderName(s.tts), r)};
                else
                    modelBody = r.body;
            }
            if (BuildTtsModelListRequest(s, key, req)) // ElevenLabs / Custom
            {
                const HttpResponse r = HttpPost(req, &TheWorker().cancel);
                if (r.Ok())
                    modelBody = r.body;
            }
            std::vector<ModelInfo> models = TtsModelChoices(s.tts, modelBody);
            if (status.state == LinkState::Ok)
                status.message = std::string("Connected to ") + TtsProviderName(s.tts) + " -- " +
                                 std::to_string(voices.size()) + " voices";
            PostResult(
                [gen, status, voices = std::move(voices), models = std::move(models)]() mutable
                {
                    if (gen != s_ttsGen)
                        return;
                    s_voices = std::move(voices);
                    s_ttsModels = std::move(models);
                    Settings& live = CurrentSettings();
                    const std::string current = live.EffectiveTtsModel();
                    // A self-hosted server's list may be partial: never switch its model by itself.
                    if (const std::string pick = live.tts == TtsProvider::Custom
                                                     ? std::string()
                                                     : PickSpeechModel(s_ttsModels, current, true);
                        !pick.empty())
                    {
                        live.ttsModel = pick;
                        SaveSettings();
                        status.message += "\n\"" + current + "\" is not available to this key; switched to \"" +
                                          pick + "\".";
                    }
                    s_ttsLink = status;
                });
        });
}

void TestVoiceId(const std::string& voiceId)
{
    const Settings& s = CurrentSettings();
    LoadRememberedKeys();
    if (s.tts == TtsProvider::Off)
        return;
    StopSpeech();
    // "HQ" resolves without a world; the voice override replaces HQ's own voice. No radio filter,
    // so the player hears the voice as it really is.
    Settings plain = s;
    plain.ttsRadioWhenFar = false;
    Speak(plain, 0, "HQ", "Sergeant, second squad is in position. Awaiting your orders.", 0, voiceId);
}

std::string AutoAssignMaleVoices()
{
    Settings& s = CurrentSettings();
    std::vector<std::string> male;
    for (const VoiceInfo& v : s_voices)
        if (IsMaleVoice(v))
            male.push_back(v.id);
    if (male.empty())
        return s_voices.empty() ? "Load the voices first (Text to speech -> Check and load voices)."
                                : "This provider lists no voice marked male.";
    // Keep HQ's voice for HQ when there are enough others, so the radio voice stays distinct.
    const std::string hq = PickVoice(s, ResolveSpeaker(0, "HQ"));
    if (male.size() > 1)
        male.erase(std::remove(male.begin(), male.end(), hq), male.end());
    const std::vector<SquadSlot> squad = CurrentSquad();
    if (squad.empty())
        return "Start a mission first: the squad is not known yet.";
    s.squadVoices.clear();
    s.squadVoicesProvider = s.tts;
    size_t next = 0;
    int assigned = 0;
    for (const SquadSlot& slot : squad)
    {
        if (slot.isPlayer)
            continue;
        s.squadVoices[slot.id] = male[next++ % male.size()]; // distinct until the list runs out
        ++assigned;
    }
    SaveSettings();
    return "Assigned " + std::to_string(assigned) + " male voices from " + std::to_string(male.size()) + " available" +
           (static_cast<size_t>(assigned) > male.size() ? " (some soldiers share a voice)." : ".");
}

std::string AssignPoolVoices()
{
    Settings& s = CurrentSettings();
    std::vector<std::string> pool = SplitList(s.ttsVoices);
    if (pool.empty())
        return "Tick the voices you want in Text to speech first; they form your voice pool.";
    if (pool.size() > 1)
        pool.erase(pool.begin()); // the first one stays HQ's radio voice
    const std::vector<SquadSlot> squad = CurrentSquad();
    if (squad.empty())
        return "Start a mission first: the squad is not known yet.";
    s.squadVoices.clear();
    s.squadVoicesProvider = s.tts;
    size_t next = 0;
    int assigned = 0;
    for (const SquadSlot& slot : squad)
        if (!slot.isPlayer)
        {
            s.squadVoices[slot.id] = pool[next++ % pool.size()];
            ++assigned;
        }
    SaveSettings();
    return "Assigned your " + std::to_string(pool.size()) + " pool voices to " + std::to_string(assigned) +
           " soldiers (HQ keeps the first one).";
}

std::string TranscriptText()
{
    std::string out;
    for (const TranscriptLine& line : s_transcript)
    {
        if (!line.who.empty())
            out += line.who + ": ";
        out += line.text + "\n";
    }
    return out;
}

} // namespace Poseidon::NpcChat
