#pragma once

// Minimal blocking HTTPS client (libcurl) for the NPC chat worker thread. Never call it on the
// game thread. Nothing here logs: request headers carry API keys.

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace Poseidon::NpcChat
{
struct HttpPart
{
    std::string name;
    std::string filename; // empty -> a plain form field
    std::string contentType;
    std::string data;
};

struct HttpRequest
{
    std::string url;
    bool get = false; // GET (model / voice lists, key checks); otherwise POST
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                // JSON body (ignored when `parts` is not empty)
    std::vector<HttpPart> parts;     // multipart/form-data
    long timeoutSeconds = 45;
};

struct HttpResponse
{
    long status = 0;
    std::string body;
    std::string error; // transport error (DNS, TLS, timeout...)
    bool Ok() const { return error.empty() && status >= 200 && status < 300; }
};

HttpResponse HttpPost(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr);

/// Opt-in traffic log: every request and response in full (keys hidden, audio summarised) is
/// appended to `path`. Empty turns it off.
void SetTrafficLogPath(const std::string& path);

/// "OpenAI 401: Incorrect API key ..." -- the provider's own message when the body has one.
std::string DescribeHttpFailure(const char* who, const HttpResponse& response);
} // namespace Poseidon::NpcChat
