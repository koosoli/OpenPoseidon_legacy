// SPDX-License-Identifier: GPL-3.0-or-later
// Conquest's optional LLM commander: see ConquestLlm.hpp.
#include <Poseidon/Game/Conquest/ConquestLlm.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatHttp.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>
#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>

#include <cjson/cJSON.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace Poseidon::Conquest::Llm
{
namespace
{
constexpr int Sides = 3;

struct Job
{
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    Answer answer;
};

struct Slots
{
    std::mutex mutex;
    std::array<std::shared_ptr<Job>, Sides> jobs;
};

Slots& S()
{
    static Slots s;
    return s;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The name in `names` that `wanted` means (case-insensitive), or "".
std::string Match(const std::vector<std::string>& names, const std::string& wanted)
{
    const std::string w = Lower(wanted);
    for (const std::string& n : names)
        if (Lower(n) == w)
            return n;
    return std::string();
}
} // namespace

std::string SystemPrompt()
{
    return "You are the strategic commander of one side in a war for the towns of an island (Cold War, 1985). "
           "You see only what your side knows: the report lists the objectives, who holds them, what your side "
           "has seen of the enemy near each, your squads and your supplies. Your officers carry out the plan: "
           "you decide where the weight of the war goes.\n"
           "Answer with ONE JSON object and nothing else:\n"
           "{\"priorities\": [{\"objective\": \"<objective name>\", \"weight\": <-2..3>}],\n"
           " \"posture\": \"attack\" | \"balanced\" | \"defend\",\n"
           " \"buy\": \"<squad type name>\" or \"\",\n"
           " \"barrage\": \"<objective name>\" or \"\",\n"
           " \"radio\": \"<a situation report for your soldiers, 1-2 short sentences: what is happening and what "
           "you want from them>\",\n"
           " \"reason\": \"<one short sentence a radio operator could read out>\"}\n"
           "Weight 3 = the main effort, 1 = worth pushing, 0 = no opinion, -2 = leave it alone for now. Name only "
           "objectives and squad types from the report. You can only capture an objective next to one you hold; "
           "your HQ falls last. \"barrage\" calls the mortars on the enemy your side has seen at that objective (it "
           "costs supplies and needs an objective of yours in range; leave it empty to keep the supplies). Keep the "
           "reason short.";
}

bool ParseAdvice(const std::string& text, const std::vector<std::string>& objectives,
                 const std::vector<std::string>& squadTypes, Advice& out, std::string& error)
{
    out = Advice();
    const size_t first = text.find('{');
    const size_t last = text.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first)
    {
        error = "the answer holds no JSON object";
        return false;
    }
    cJSON* root = cJSON_Parse(text.substr(first, last - first + 1).c_str());
    if (!root || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        error = "the answer's JSON does not parse";
        return false;
    }
    if (const cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "priorities"); cJSON_IsArray(list))
    {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, list)
        {
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "objective");
            const cJSON* weight = cJSON_GetObjectItemCaseSensitive(item, "weight");
            if (!cJSON_IsString(name) || !cJSON_IsNumber(weight))
                continue;
            const std::string known = Match(objectives, name->valuestring);
            if (known.empty())
                continue; // a town the model made up
            out.priorities.emplace_back(known, std::clamp(static_cast<float>(weight->valuedouble), -2.0f, 3.0f));
        }
    }
    if (const cJSON* posture = cJSON_GetObjectItemCaseSensitive(root, "posture"); cJSON_IsString(posture))
    {
        const std::string p = Lower(posture->valuestring);
        out.posture = (p == "attack" || p == "defend") ? p : "balanced";
    }
    if (const cJSON* buy = cJSON_GetObjectItemCaseSensitive(root, "buy"); cJSON_IsString(buy))
        out.buy = Match(squadTypes, buy->valuestring);
    if (const cJSON* barrage = cJSON_GetObjectItemCaseSensitive(root, "barrage"); cJSON_IsString(barrage))
        out.barrage = Match(objectives, barrage->valuestring);
    if (const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "reason"); cJSON_IsString(reason))
        out.reason = std::string(reason->valuestring).substr(0, 240);
    if (const cJSON* radio = cJSON_GetObjectItemCaseSensitive(root, "radio"); cJSON_IsString(radio))
        out.radio = std::string(radio->valuestring).substr(0, 300);
    cJSON_Delete(root);
    if (out.priorities.empty() && out.posture.empty() && out.buy.empty() && out.barrage.empty())
    {
        error = "the answer names no known objective, posture or squad type";
        return false;
    }
    if (out.posture.empty())
        out.posture = "balanced";
    return true;
}

bool Ask(int side, const std::string& report, const std::vector<std::string>& objectives,
         const std::vector<std::string>& squadTypes, std::string* why)
{
    if (side < 0 || side >= Sides)
        return false;
    const NpcChat::Settings settings = NpcChat::CurrentSettings(); // a copy for the worker
    const std::string key = NpcChat::GetKey(NpcChat::LlmKeySlot(settings.llm));
    if (key.empty() && settings.llm != NpcChat::LlmProvider::Ollama && settings.llm != NpcChat::LlmProvider::Custom)
    {
        if (why)
            *why = std::string("no API key for ") + NpcChat::LlmProviderName(settings.llm);
        return false;
    }
    auto job = std::make_shared<Job>();
    {
        std::lock_guard lock(S().mutex);
        if (S().jobs[side] && !S().jobs[side]->done)
        {
            if (why)
                *why = "a request is already in flight";
            return false;
        }
        S().jobs[side] = job;
    }
    std::thread(
        [job, settings, key, report, objectives, squadTypes]()
        {
            const auto t0 = std::chrono::steady_clock::now();
            Answer answer;
            // A model that cannot chat (a live / realtime voice model, picked from the list) is never asked:
            // the provider's default model commands instead, and the war log says so.
            NpcChat::Settings use = settings;
            const std::string chosen = settings.EffectiveLlmModel();
            if (!chosen.empty() && !NpcChat::IsChatModelId(chosen))
            {
                use.llmModel.clear();
                answer.note = "the model " + chosen + " cannot chat; " + use.EffectiveLlmModel() + " commands instead";
            }
            const auto post = [&](const NpcChat::Settings& s)
            {
                NpcChat::HttpRequest request = NpcChat::BuildLlmRequest(s, key, SystemPrompt(), {{"user", report}});
                request.timeoutSeconds = std::max(10, s.requestTimeoutSeconds);
                return NpcChat::HttpPost(request, &job->cancel);
            };
            NpcChat::HttpResponse response = post(use);
            // The provider refused the model itself (unknown, retired, or not for chat): once more with
            // its default model, rather than an LLM commander that never answers.
            const bool modelRefused =
                (response.status == 400 || response.status == 404) && !use.llmModel.empty() &&
                (response.body.find("model") != std::string::npos || response.body.find("Model") != std::string::npos);
            if (modelRefused)
            {
                const std::string refused = use.EffectiveLlmModel();
                use.llmModel.clear();
                response = post(use);
                answer.note = "the model " + refused + " was refused; " + use.EffectiveLlmModel() + " commands instead";
            }
            if (!response.Ok())
                answer.error = NpcChat::DescribeHttpFailure(NpcChat::LlmProviderName(settings.llm), response);
            else
            {
                std::string text, error;
                if (!NpcChat::ParseLlmResponse(settings.llm, response.body, text, error))
                    answer.error = error.empty() ? std::string("the provider's answer could not be read") : error;
                else if (!ParseAdvice(text, objectives, squadTypes, answer.advice, error))
                    answer.error = error;
                else
                    answer.ok = true;
            }
            // One line for the war log: transport errors arrive with a trailing newline.
            std::replace(answer.error.begin(), answer.error.end(), '\n', ' ');
            std::replace(answer.error.begin(), answer.error.end(), '\r', ' ');
            while (!answer.error.empty() && answer.error.back() == ' ')
                answer.error.pop_back();
            answer.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            job->answer = std::move(answer);
            job->done = true;
        })
        .detach();
    return true;
}

bool InFlight(int side)
{
    if (side < 0 || side >= Sides)
        return false;
    std::lock_guard lock(S().mutex);
    return S().jobs[side] && !S().jobs[side]->done;
}

bool TakeAnswer(int side, Answer& answer)
{
    if (side < 0 || side >= Sides)
        return false;
    std::lock_guard lock(S().mutex);
    std::shared_ptr<Job>& job = S().jobs[side];
    if (!job || !job->done)
        return false;
    answer = job->answer;
    job.reset();
    return true;
}

void CancelAll()
{
    std::lock_guard lock(S().mutex);
    for (std::shared_ptr<Job>& job : S().jobs)
        if (job)
        {
            job->cancel = true;
            job.reset(); // the worker keeps its own reference and finishes into nothing
        }
}
} // namespace Poseidon::Conquest::Llm
