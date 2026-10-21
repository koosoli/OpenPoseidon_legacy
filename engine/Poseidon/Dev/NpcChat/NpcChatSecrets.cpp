// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatSecrets.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatSettings.hpp>
#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>

#include <cjson/cJSON.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace Poseidon::NpcChat
{
namespace
{
std::mutex s_keysMutex;
std::map<std::string, std::string> s_keys; // slot -> key, session memory
bool s_loaded = false;

const char* kStoreMagic = "OPNPCK1"; // header so a wrong/corrupt file is recognised

std::string EnvValue(const char* name)
{
    if (!name)
        return {};
#ifdef _WIN32
    const std::wstring wide = Utf8PathToWide(name);
    const wchar_t* value = _wgetenv(wide.c_str());
    return value ? WidePathToUtf8(value) : std::string();
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

fs::path StoreDirectory()
{
#ifdef _WIN32
    const std::string appData = EnvValue("APPDATA");
    if (appData.empty())
        return {};
    return FilesystemPathFromUtf8(appData) / "OpenPoseidon" / "npc-chat";
#else
    std::string base = EnvValue("XDG_CONFIG_HOME");
    if (base.empty())
    {
        const std::string home = EnvValue("HOME");
        if (home.empty())
            return {};
        base = home + "/.config";
    }
    return fs::path(base) / "openposeidon" / "npc-chat";
#endif
}

bool Protect(const std::string& plain, std::vector<char>& out)
{
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, L"OpenPoseidon NPC chat keys", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &blob))
        return false;
    out.assign(reinterpret_cast<char*>(blob.pbData), reinterpret_cast<char*>(blob.pbData) + blob.cbData);
    LocalFree(blob.pbData);
    return true;
#else
    out.assign(plain.begin(), plain.end());
    return true;
#endif
}

bool Unprotect(const char* data, size_t size, std::string& out)
{
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(size), reinterpret_cast<BYTE*>(const_cast<char*>(data))};
    DATA_BLOB blob{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob))
        return false;
    out.assign(reinterpret_cast<char*>(blob.pbData), blob.cbData);
    SecureZeroMemory(blob.pbData, blob.cbData);
    LocalFree(blob.pbData);
    return true;
#else
    out.assign(data, size);
    return true;
#endif
}

void Wipe(std::string& s)
{
    for (char& c : s)
        c = 0;
    s.clear();
}
} // namespace

std::string GetKey(const std::string& slot)
{
    if (slot.empty())
        return {};
    {
        std::lock_guard lock(s_keysMutex);
        auto it = s_keys.find(slot);
        if (it != s_keys.end() && !it->second.empty())
            return it->second;
    }
    return EnvValue(KeySlotEnvVar(slot));
}

bool HasPanelKey(const std::string& slot)
{
    std::lock_guard lock(s_keysMutex);
    auto it = s_keys.find(slot);
    return it != s_keys.end() && !it->second.empty();
}

bool HasEnvKey(const std::string& slot)
{
    return !EnvValue(KeySlotEnvVar(slot)).empty();
}

void SetKey(const std::string& slot, const std::string& key)
{
    if (slot.empty())
        return;
    const std::string clean = SanitizeKey(key);
    std::lock_guard lock(s_keysMutex);
    if (clean.empty())
    {
        auto it = s_keys.find(slot);
        if (it != s_keys.end())
        {
            Wipe(it->second);
            s_keys.erase(it);
        }
        return;
    }
    s_keys[slot] = clean;
}

std::string KeyFingerprint(const std::string& slot)
{
    const std::string key = GetKey(slot);
    if (key.empty())
        return {};
    if (key.size() < 16)
        return "(" + std::to_string(key.size()) + " chars -- too short for an API key)";
    return key.substr(0, 8) + "..." + key.substr(key.size() - 4) + " (" + std::to_string(key.size()) + " chars)";
}

std::string SanitizeKey(const std::string& key)
{
    // A paste from a web page can bring whitespace, quotes, a "Bearer " prefix or invisible
    // Unicode (zero-width space, BOM, non-breaking space). Any of those turns a valid key into an
    // unknown one ("401 User not found"). API keys are plain printable ASCII, so keep only that.
    std::string out;
    out.reserve(key.size());
    for (char c : key)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u > ' ' && u < 0x7F && c != '"' && c != '\'' && c != '`')
            out += c;
    }
    if (out.size() > 6 && (out.compare(0, 6, "Bearer") == 0 || out.compare(0, 6, "bearer") == 0))
        out.erase(0, 6);
    return out;
}

void ClearAllKeys()
{
    std::lock_guard lock(s_keysMutex);
    for (auto& [slot, key] : s_keys)
        Wipe(key);
    s_keys.clear();
}

std::string KeyStorePath()
{
    const fs::path dir = StoreDirectory();
    return dir.empty() ? std::string() : FilesystemPathToUtf8(dir / "keys.dat");
}

bool IsInsideGitWorkTree(const std::string& path)
{
    std::error_code ec;
    fs::path p = FilesystemPathFromUtf8(path);
    if (p.is_relative())
        p = fs::absolute(p, ec);
    for (fs::path dir = p.parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        if (fs::exists(dir / ".git", ec))
            return true;
        if (dir == dir.root_path())
            break;
    }
    return false;
}

void LoadRememberedKeys()
{
    {
        std::lock_guard lock(s_keysMutex);
        if (s_loaded)
            return;
        s_loaded = true;
    }
    const std::string path = KeyStorePath();
    if (path.empty() || !FileExistsUtf8(path.c_str()))
        return;
    const std::vector<char> raw = ReadFileUtf8(path.c_str());
    const size_t magic = std::char_traits<char>::length(kStoreMagic);
    if (raw.size() <= magic || std::string(raw.data(), magic) != kStoreMagic)
        return;
    std::string json;
    if (!Unprotect(raw.data() + magic, raw.size() - magic, json))
        return; // another account's / machine's file: unreadable by design
    cJSON* root = cJSON_Parse(json.c_str());
    Wipe(json);
    if (cJSON_IsObject(root))
    {
        std::lock_guard lock(s_keysMutex);
        for (cJSON* item = root->child; item; item = item->next)
            if (cJSON_IsString(item) && item->string && item->valuestring && item->valuestring[0])
                s_keys.emplace(item->string, item->valuestring);
    }
    cJSON_Delete(root);
}

bool PersistKeys(bool remember, std::string& error)
{
    const std::string path = KeyStorePath();
    if (path.empty())
    {
        error = "No per-user application-data directory (APPDATA/HOME is not set).";
        return false;
    }
    std::error_code ec;
    if (!remember)
    {
        fs::remove(FilesystemPathFromUtf8(path), ec);
        return true;
    }
    if (IsInsideGitWorkTree(path))
    {
        error = "Refusing to store keys inside a git repository: " + path;
        return false;
    }

    std::string json;
    {
        cJSON* root = cJSON_CreateObject();
        std::lock_guard lock(s_keysMutex);
        for (const auto& [slot, key] : s_keys)
            if (!key.empty())
                cJSON_AddStringToObject(root, slot.c_str(), key.c_str());
        char* text = cJSON_PrintUnformatted(root);
        json = text ? text : "{}";
        if (text)
        {
            for (char* c = text; *c; ++c)
                *c = 0;
            cJSON_free(text);
        }
        cJSON_Delete(root);
    }

    std::vector<char> blob;
    const bool ok = Protect(json, blob);
    Wipe(json);
    if (!ok)
    {
        error = "Windows could not encrypt the key file (CryptProtectData failed).";
        return false;
    }
    const fs::path file = FilesystemPathFromUtf8(path);
    fs::create_directories(file.parent_path(), ec);
    std::vector<char> data(kStoreMagic, kStoreMagic + std::char_traits<char>::length(kStoreMagic));
    data.insert(data.end(), blob.begin(), blob.end());
    if (!WriteFileUtf8(path.c_str(), data.data(), data.size()))
    {
        error = "Could not write " + path;
        return false;
    }
#ifndef _WIN32
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

std::string RedactSecrets(const std::string& text)
{
    std::string out = text;
    std::vector<std::string> secrets;
    {
        std::lock_guard lock(s_keysMutex);
        for (const auto& [slot, key] : s_keys)
            if (key.size() >= 8)
                secrets.push_back(key);
    }
    for (const char* slot : {"openai", "openrouter", "anthropic", "gemini", "google", "groq"})
    {
        const std::string env = EnvValue(KeySlotEnvVar(slot));
        if (env.size() >= 8)
            secrets.push_back(env);
    }
    for (const std::string& secret : secrets)
    {
        for (size_t pos = out.find(secret); pos != std::string::npos; pos = out.find(secret, pos))
            out.replace(pos, secret.size(), "[redacted]");
    }
    // Anything shaped like a provider key even if it is not one we hold (an error body echoing
    // a different key, a pasted key in a chat line).
    static const char* prefixes[] = {"sk-", "sk_", "AIza", "gsk_", "xai-"};
    for (const char* prefix : prefixes)
    {
        const size_t plen = std::char_traits<char>::length(prefix);
        for (size_t pos = out.find(prefix); pos != std::string::npos; pos = out.find(prefix, pos + 1))
        {
            size_t end = pos + plen;
            while (end < out.size() && (std::isalnum(static_cast<unsigned char>(out[end])) || out[end] == '-' ||
                                        out[end] == '_'))
                ++end;
            if (end - pos >= 16)
            {
                out.replace(pos, end - pos, "[redacted]");
            }
        }
    }
    return out;
}

} // namespace Poseidon::NpcChat
