// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatHttp.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace Poseidon::NpcChat
{
namespace
{
size_t AppendBody(char* data, size_t size, size_t count, void* user)
{
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto* cancel = static_cast<const std::atomic<bool>*>(user);
    return cancel && cancel->load() ? 1 : 0; // non-zero aborts the transfer
}
} // namespace

namespace
{
HttpResponse HttpSend(const HttpRequest& request, const std::atomic<bool>* cancel)
{
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        response.error = "curl_easy_init failed";
        return response;
    }

    curl_slist* headers = nullptr;
    for (const auto& [name, value] : request.headers)
        headers = curl_slist_append(headers, (name + ": " + value).c_str());

    curl_mime* mime = nullptr;
    if (request.get)
    {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    else if (!request.parts.empty())
    {
        mime = curl_mime_init(curl);
        for (const HttpPart& part : request.parts)
        {
            curl_mimepart* field = curl_mime_addpart(mime);
            curl_mime_name(field, part.name.c_str());
            curl_mime_data(field, part.data.data(), part.data.size());
            if (!part.filename.empty())
                curl_mime_filename(field, part.filename.c_str());
            if (!part.contentType.empty())
                curl_mime_type(field, part.contentType.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    }
    else
    {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OpenPoseidon-NpcChat/1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeoutSeconds > 0 ? request.timeoutSeconds : 45L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // never replay an Authorization header elsewhere
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
#ifdef _WIN32
    // Use the Windows certificate store like the rest of the system (schannel builds ignore it).
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    if (const char* caFile = std::getenv("SSL_CERT_FILE"); caFile && caFile[0])
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile);

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK)
        response.error = cancel && cancel->load() ? std::string("cancelled") : std::string(curl_easy_strerror(code));
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    if (mime)
        curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ---- traffic log (opt-in): everything sent to the providers and everything that came back ----

std::mutex s_trafficMutex;
std::string s_trafficPath; // empty = off

/// Long strings (base64 audio) become a size note; the rest of the JSON stays readable.
void ShortenLongStrings(cJSON* node)
{
    for (cJSON* item = node ? node->child : nullptr; item; item = item->next)
    {
        if (cJSON_IsString(item) && item->valuestring && std::strlen(item->valuestring) > 4000)
        {
            const std::string note = "[" + std::to_string(std::strlen(item->valuestring)) + " characters, base64 audio]";
            cJSON_SetValuestring(item, note.c_str());
        }
        else
        {
            ShortenLongStrings(item);
        }
    }
}

std::string Readable(const std::string& body)
{
    if (body.empty())
        return "(empty)";
    if (cJSON* root = cJSON_Parse(body.c_str()))
    {
        ShortenLongStrings(root);
        char* text = cJSON_Print(root);
        std::string out = text ? text : body;
        cJSON_free(text);
        cJSON_Delete(root);
        return out;
    }
    size_t binary = 0;
    for (unsigned char c : body.substr(0, 512))
        if (c < 9 || (c > 13 && c < 32))
            ++binary;
    if (binary > 8)
        return "[" + std::to_string(body.size()) + " bytes of binary data (audio)]";
    return body;
}

void LogTraffic(const HttpRequest& request, const HttpResponse& response, double seconds)
{
    std::lock_guard lock(s_trafficMutex);
    if (s_trafficPath.empty())
        return;
    const char nl = '\n';
    std::string entry;
    char head[160];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(head, sizeof(head), "%Y-%m-%d %H:%M:%S", &tm);
    entry += nl;
    entry += "================================================================ " + std::string(head) + nl;
    entry += std::string(request.get ? "GET " : "POST ") + request.url + nl;
    for (const auto& [name, value] : request.headers)
    {
        const bool secret = name == "Authorization" || name == "x-api-key" || name == "xi-api-key" ||
                            name == "X-Goog-Api-Key" || name == "x-goog-api-key";
        entry += "  " + name + ": " + (secret ? std::string("[hidden]") : value) + nl;
    }
    if (!request.parts.empty())
    {
        entry += std::string("--- request (multipart form)") + nl;
        for (const HttpPart& part : request.parts)
            entry += "  " + part.name + " = " +
                     (part.filename.empty() ? part.data
                                            : "[" + std::to_string(part.data.size()) + " bytes, " + part.filename + "]") +
                     nl;
    }
    else if (!request.get)
    {
        entry += "--- request body (" + std::to_string(request.body.size()) + " bytes)" + nl + Readable(request.body) + nl;
    }
    char took[64];
    std::snprintf(took, sizeof(took), " after %.1f s", seconds);
    entry += "--- response: " + (response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error) +
             took + nl + Readable(response.body) + nl;
    std::FILE* f = OpenFileUtf8(s_trafficPath.c_str(), "ab");
    if (!f)
        return;
    const std::string clean = RedactSecrets(entry);
    std::fwrite(clean.data(), 1, clean.size(), f);
    std::fclose(f);
}
} // namespace

HttpResponse HttpPost(const HttpRequest& request, const std::atomic<bool>* cancel)
{
    const auto t0 = std::chrono::steady_clock::now();
    HttpResponse response = HttpSend(request, cancel);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LogTraffic(request, response, seconds);
    return response;
}

void SetTrafficLogPath(const std::string& path)
{
    std::lock_guard lock(s_trafficMutex);
    s_trafficPath = path;
}

std::string DescribeHttpFailure(const char* who, const HttpResponse& response)
{
    if (!response.error.empty())
    {
        std::string text = std::string(who) + ": " + response.error;
        if (response.error.find("Timeout") != std::string::npos || response.error.find("timed out") != std::string::npos)
            text += "\nFIX: the connection worked but no answer came in time. Free models are often queued and "
                    "\"thinking\" models reason before they answer. Pick a faster model (filter: free, then try "
                    "another), or raise \"Answer timeout\" in the Language model section. A local model on a "
                    "slow GPU or the CPU can need a minute for its first reply while it loads.";
        else if (response.error.find("onnect") != std::string::npos) // curl: "Couldn't connect to server"
            text += "\nFIX: nothing answers at that address. For a local server, start it first (Ollama: run "
                    "\"ollama serve\" or open the Ollama app; LM Studio: Developer -> Start Server) and check the "
                    "port in the address.";
        return text;
    }
    std::string message;
    if (cJSON* root = cJSON_Parse(response.body.c_str()))
    {
        // OpenAI/OpenRouter/Groq/Anthropic: {"error":{"message":..}}; Google: {"error":{"message":..}}
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* msg = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "message") : nullptr;
        if (cJSON_IsString(msg) && msg->valuestring)
            message = msg->valuestring;
        // OpenRouter wraps the upstream provider's own words: who refused, and why.
        const cJSON* meta = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "metadata") : nullptr;
        const cJSON* provider = cJSON_GetObjectItemCaseSensitive(meta, "provider_name");
        const cJSON* raw = cJSON_GetObjectItemCaseSensitive(meta, "raw");
        if (cJSON_IsString(provider) && provider->valuestring)
            message += std::string(" (") + provider->valuestring + ")";
        if (cJSON_IsString(raw) && raw->valuestring)
            message += std::string(": ") + std::string(raw->valuestring).substr(0, 300);
        else if (cJSON_IsString(err) && err->valuestring)
            message = err->valuestring;
        cJSON_Delete(root);
    }
    if (message.empty())
        message = response.body.substr(0, 300);
    std::string text = std::string(who) + " HTTP " + std::to_string(response.status) + ": " + message;

    // The two failures a first-time setup almost always hits, spelled out.
    const auto has = [&](const char* needle) { return message.find(needle) != std::string::npos; };
    if (has("has not been used in project") || has("SERVICE_DISABLED") || has("is disabled"))
        text += "\nFIX: this Google API is switched off for the Google Cloud project your key belongs to. Open the "
                "link above (or console.cloud.google.com -> APIs & Services -> Library), enable the API, wait a "
                "couple of minutes, then press Check again. A Gemini (AI Studio) key only works for speech if its "
                "project has the Text-to-Speech / Speech-to-Text API enabled.";
    else if (response.status == 401 || response.status == 403)
        text += "\nFIX: the provider does not accept this key. Copy the whole key again from the provider's site "
                "(OpenRouter keys start with sk-or-v1-, OpenAI with sk-, Anthropic with sk-ant-, ElevenLabs with "
                "sk_), or create a new one, then press Set.";
    else if (response.status == 402)
        text += "\nFIX: the account has no credit left for this model. Pick a free model (filter: free) or add "
                "credit.";
    else if (response.status == 404 && (has("guardrail") || has("data policy")))
        text += "\nFIX: your OpenRouter privacy settings exclude every provider of this model. Allow more "
                "providers at openrouter.ai/settings/privacy, or pick another model.";
    else if (has("try pulling"))
        text += "\nFIX: this model is not downloaded in Ollama. Run \"ollama pull <model>\" with the name in the "
                "Model field, or pick one from the loaded list (\"ollama list\" shows what you have).";
    else if (response.status == 404)
        text +="\nFIX: unknown model or address. Pick a model from the loaded list.";
    else if (response.status == 429)
        text += "\nFIX: rate limited by the provider. Wait a moment; free models are limited per minute.";
    return RedactSecrets(text);
}

} // namespace Poseidon::NpcChat
