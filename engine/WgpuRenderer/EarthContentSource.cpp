#include "EarthContentSource.hpp"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Poseidon
{
namespace
{
std::mutex transfers;
std::map<std::string, std::chrono::steady_clock::time_point> failures;
bool Kind(const char* kind)
{
    return kind && (std::string(kind) == "features" || std::string(kind) == "buildings");
}
bool Cancelled(const std::function<bool()>& check)
{
    return check && check();
}
size_t Receive(char* data, size_t size, size_t count, void* target)
{
    auto& bytes = *static_cast<std::string*>(target);
    if (size && count > (EarthContentSource::MaxBytes - bytes.size()) / size)
        return 0;
    try
    {
        bytes.append(data, size * count);
    }
    catch (...)
    {
        return 0;
    }
    return size * count;
}
int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return Cancelled(*static_cast<const std::function<bool()>*>(context)) ? 1 : 0;
}
bool SafeUrl(const std::string& url, bool test)
{
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
        return false;
    auto part = [&](CURLUPart p)
    {
        char* raw = nullptr;
        std::string result;
        if (curl_url_get(parsed.get(), p, &raw, 0) == CURLUE_OK)
            result = raw;
        curl_free(raw);
        return result;
    };
    const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
    return part(CURLUPART_USER).empty() && part(CURLUPART_PASSWORD).empty() && part(CURLUPART_FRAGMENT).empty() &&
           (scheme == "https" || (test && scheme == "http" && (host == "127.0.0.1" || host == "[::1]")));
}
void Replace(std::string& text, const char* token, const std::string& value)
{
    size_t at = 0;
    while ((at = text.find(token, at)) != std::string::npos)
    {
        text.replace(at, std::string(token).size(), value);
        at += value.size();
    }
}
// Only generated streaming files participate in the 128-cell / 256-MiB budget.
// Hand-prepared cells in cache/features and cache/buildings are never evicted.
bool MakeRoom(const std::filesystem::path& root, size_t incoming)
{
    struct File
    {
        std::filesystem::path path;
        uintmax_t bytes;
        std::filesystem::file_time_type time;
    };
    std::vector<File> files;
    uintmax_t total = incoming;
    std::error_code ec;
    for (const auto* kind : {"features", "buildings"})
    {
        const auto dir = root / kind;
        if (!std::filesystem::exists(dir, ec))
            continue;
        if (std::filesystem::is_symlink(dir, ec))
            return false;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            if (!entry.is_symlink() && entry.is_regular_file())
            {
                // A completed write always renames under transfers. Any old
                // partial here is from an interrupted process, never an active writer.
                if (entry.path().extension() == ".partial")
                {
                    if (!std::filesystem::remove(entry.path(), ec))
                        return false;
                    continue;
                }
                if (entry.path().extension() != ".json")
                    continue;
                files.push_back({entry.path(), entry.file_size(), entry.last_write_time()});
                total += files.back().bytes;
            }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    size_t remaining = files.size();
    for (const auto& file : files)
    {
        if (remaining < 128 && total <= 256 * 1024 * 1024)
            break;
        if (std::filesystem::remove(file.path, ec))
        {
            --remaining;
            total -= file.bytes;
        }
    }
    return remaining < 128 && total <= 256 * 1024 * 1024;
}
} // namespace
EarthContentSource::EarthContentSource(std::string urlTemplate, bool loopbackTest) : _loopbackTest(loopbackTest)
{
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (urlTemplate.empty())
        if (const auto* value = std::getenv("POSEIDON_EARTH_CONTENT_URL"))
            urlTemplate = value;
    if (urlTemplate.size() > 2048 || urlTemplate.find("{kind}") == std::string::npos ||
        urlTemplate.find("{lat}") == std::string::npos || urlTemplate.find("{lon}") == std::string::npos)
        return;
    auto example = urlTemplate;
    Replace(example, "{kind}", "features");
    Replace(example, "{lat}", "0");
    Replace(example, "{lon}", "0");
    if (SafeUrl(example, loopbackTest))
        _url = std::move(urlTemplate);
}
std::filesystem::path EarthContentSource::StreamPath(const std::filesystem::path& cache, const char* kind, int lat,
                                                     int lon)
{
    if (!Kind(kind))
        return {};
    return cache / "content-stream" / kind / (std::to_string(lat) + "_" + std::to_string(lon) + ".json");
}
bool EarthContentSource::ValidCell(const std::string& bytes, const char* kind)
{
    if (!Kind(kind) || bytes.empty() || bytes.size() > MaxBytes || bytes.find('\0') != std::string::npos)
        return false;
    const char* end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLengthOpts(bytes.c_str(), bytes.size() + 1, &end, 1), cJSON_Delete);
    if (!json || cJSON_GetObjectItemCaseSensitive(json.get(), "remark"))
        return false;
    const auto* elements = cJSON_GetObjectItemCaseSensitive(json.get(), "elements");
    return cJSON_IsArray(elements) && cJSON_GetArraySize(elements) <= (std::string(kind) == "buildings" ? 2000 : 6000);
}
bool EarthContentSource::Ensure(const std::filesystem::path& cache, const char* kind, int lat, int lon,
                                const std::function<bool()>& cancelled)
{
    if (!Enabled() || !Kind(kind) || lat < -4200 || lat >= 4200 || lon < -9000 || lon >= 9000 || Cancelled(cancelled))
        return false;
    // No more than two presentation workers contend here; the render thread never waits on this mutex.
    std::lock_guard lock(transfers);
    if (Cancelled(cancelled))
        return false;
    const auto path = StreamPath(cache, kind, lat, lon);
    const auto prepared = cache / kind / path.filename();
    std::error_code ec;
    if (std::filesystem::is_regular_file(prepared, ec))
        return true;
    if (std::filesystem::is_regular_file(path, ec))
    {
        const auto length = std::filesystem::file_size(path, ec);
        if (!ec && length > 0 && length <= MaxBytes)
        {
            std::string cached(size_t(length), '\0');
            std::ifstream input(path, std::ios::binary);
            if (input.read(cached.data(), std::streamsize(length)) && ValidCell(cached, kind))
            {
                std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
                return true;
            }
        }
        if (!std::filesystem::remove(path, ec))
            return false;
    }
    auto url = _url;
    Replace(url, "{kind}", kind);
    Replace(url, "{lat}", std::to_string(lat));
    Replace(url, "{lon}", std::to_string(lon));
    const auto now = std::chrono::steady_clock::now();
    if (auto it = failures.find(url); it != failures.end() && now - it->second < std::chrono::seconds(60))
        return false;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return false;
    std::string bytes;
    auto failed = [&]
    {
        if (failures.size() >= 256)
            failures.erase(failures.begin());
        failures[url] = now;
        return false;
    };
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, _loopbackTest ? "https,http" : "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "OpenPoseidon-EarthContent/1");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
    const auto status = curl_easy_perform(curl.get());
    long http = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http);
    if (Cancelled(cancelled))
        return false;
    if (status != CURLE_OK || http != 200 || !ValidCell(bytes, kind))
    {
        return failed();
    }
    const auto temporary = path.string() + ".partial";
    try
    {
        std::filesystem::create_directories(path.parent_path());
        if (!MakeRoom(cache / "content-stream", bytes.size()))
            return failed();
        {
            std::ofstream output(temporary, std::ios::binary);
            output.write(bytes.data(), bytes.size());
            if (!output)
            {
                output.close();
                std::filesystem::remove(temporary, ec);
                return failed();
            }
        }
        if (Cancelled(cancelled))
        {
            std::filesystem::remove(temporary, ec);
            return false;
        }
        std::filesystem::rename(temporary, path);
        failures.erase(url);
        return true;
    }
    catch (const std::filesystem::filesystem_error&)
    {
        std::filesystem::remove(temporary, ec);
        return failed();
    }
}
void EarthContentSource::EnsureNeighborhood(const std::filesystem::path& cache, const char* kind, int lat, int lon,
                                            const std::function<bool()>& cancelled)
{
    if (!Enabled())
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    const auto bounded = [&] { return Cancelled(cancelled) || std::chrono::steady_clock::now() >= deadline; };
    constexpr int offsets[9][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    for (const auto& offset : offsets)
    {
        if (bounded())
            return;
        Ensure(cache, kind, lat + offset[0], ((lon + offset[1] + 27000) % 18000) - 9000, bounded);
    }
}
} // namespace Poseidon
