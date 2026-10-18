// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/Dev/NpcChat/NpcChatSapi.hpp>

#include <Poseidon/Dev/NpcChat/NpcChatProviders.hpp>

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#endif

namespace Poseidon::NpcChat
{
#ifdef _WIN32
namespace
{
std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* w)
{
    if (!w)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct ComScope
{
    bool owned = false;
    ComScope() { owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

template <class T>
void Release(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

/// Every installed voice token with its description. Caller releases the tokens.
/// Both stores: the classic SAPI one and Windows 10/11's OneCore one, which holds most voices that
/// "Settings -> Speech -> Add voices" installs (David, Mark, Stefan...). SAPI 5 speaks with either.
std::vector<std::pair<ISpObjectToken*, std::string>> EnumerateVoices()
{
    std::vector<std::pair<ISpObjectToken*, std::string>> voices;
    static const wchar_t* const kCategories[] = {
        SPCAT_VOICES, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices"};
    for (const wchar_t* id : kCategories)
    {
        ISpObjectTokenCategory* category = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                    reinterpret_cast<void**>(&category))))
            continue;
        IEnumSpObjectTokens* tokens = nullptr;
        if (SUCCEEDED(category->SetId(id, FALSE)) && SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens)))
        {
            ISpObjectToken* token = nullptr;
            while (tokens->Next(1, &token, nullptr) == S_OK)
            {
                WCHAR* description = nullptr;
                token->GetStringValue(nullptr, &description);
                const std::string name = Narrow(description);
                CoTaskMemFree(description);
                const bool seen = std::any_of(voices.begin(), voices.end(),
                                              [&](const auto& v) { return v.second == name; });
                if (seen || name.empty())
                    token->Release(); // the same voice in both stores
                else
                    voices.emplace_back(token, name);
                token = nullptr;
            }
            Release(tokens);
        }
        Release(category);
    }
    return voices;
}

std::string Attribute(ISpObjectToken* token, const wchar_t* name)
{
    std::string value;
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes)
    {
        WCHAR* text = nullptr;
        if (SUCCEEDED(attributes->GetStringValue(name, &text)) && text)
        {
            value = Narrow(text);
            CoTaskMemFree(text);
        }
        attributes->Release();
    }
    return value;
}
} // namespace

std::vector<std::string> SapiVoices()
{
    std::vector<std::string> names;
    for (const auto& [name, gender] : SapiVoicesWithGender())
        names.push_back(name);
    return names;
}

std::vector<std::pair<std::string, std::string>> SapiVoicesWithGender()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const SapiVoiceInfo& v : SapiVoiceList())
        out.emplace_back(v.name, v.gender);
    return out;
}

std::vector<SapiVoiceInfo> SapiVoiceList()
{
    ComScope com;
    std::vector<SapiVoiceInfo> out;
    for (auto& [token, name] : EnumerateVoices())
    {
        // Attributes\Gender ("Male"/"Female") and Attributes\Language (LCIDs in hex, "409;9" style).
        SapiVoiceInfo info;
        info.name = name;
        info.gender = Attribute(token, L"Gender");
        const std::string language = Attribute(token, L"Language");
        const std::string first = language.substr(0, language.find(';'));
        // Every English LCID ends in 09 (409 US, 809 UK, c09 AU, 1009 CA...).
        info.english = first.size() >= 2 && first.compare(first.size() - 2, 2, "09") == 0;
        out.push_back(info);
        token->Release();
    }
    return out;
}

bool SapiSpeakToWav(const std::string& text, const std::string& voice, int rate, std::string& wav,
                    std::string& error)
{
    ComScope com;
    ISpVoice* sp = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&sp))))
    {
        error = "Windows speech (SAPI) is not available";
        return false;
    }

    if (!voice.empty())
    {
        auto voices = EnumerateVoices();
        for (auto& [token, name] : voices)
            if (name.find(voice) != std::string::npos)
            {
                sp->SetVoice(token);
                break;
            }
        for (auto& [token, name] : voices)
            token->Release();
    }
    sp->SetRate(rate < -10 ? -10 : rate > 10 ? 10 : rate);

    constexpr int kRate = 22050;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = kRate * 2;

    IStream* memory = nullptr;
    ISpStream* stream = nullptr;
    bool ok = false;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)) &&
        SUCCEEDED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                   reinterpret_cast<void**>(&stream))) &&
        SUCCEEDED(stream->SetBaseStream(memory, SPDFID_WaveFormatEx, &format)) &&
        SUCCEEDED(sp->SetOutput(stream, TRUE)))
    {
        // SPF_IS_NOT_XML: the text came from a language model; it must not drive SAPI markup.
        const std::wstring wide = Widen(text);
        if (SUCCEEDED(sp->Speak(wide.c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr)))
        {
            sp->WaitUntilDone(30000);
            HGLOBAL global = nullptr;
            STATSTG stat{};
            if (SUCCEEDED(GetHGlobalFromStream(memory, &global)) && SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)))
            {
                const size_t bytes = static_cast<size_t>(stat.cbSize.QuadPart);
                if (const void* data = GlobalLock(global))
                {
                    wav = MakeWav(static_cast<const int16_t*>(data), bytes / 2, kRate);
                    GlobalUnlock(global);
                    ok = bytes > 0;
                }
            }
        }
    }
    if (!ok)
        error = "Windows speech failed to render";
    sp->SetOutput(nullptr, FALSE);
    Release(stream);
    Release(memory);
    Release(sp);
    return ok;
}
#else
std::vector<std::string> SapiVoices()
{
    return {};
}

std::vector<std::pair<std::string, std::string>> SapiVoicesWithGender()
{
    return {};
}

std::vector<SapiVoiceInfo> SapiVoiceList()
{
    return {};
}

bool SapiSpeakToWav(const std::string&, const std::string&, int, std::string&, std::string& error)
{
    error = "Windows voices are only available on Windows";
    return false;
}
#endif
} // namespace Poseidon::NpcChat
