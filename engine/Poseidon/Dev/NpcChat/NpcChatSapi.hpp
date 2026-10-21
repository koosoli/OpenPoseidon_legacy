#pragma once

// Windows' built-in speech voices (SAPI 5): free, offline, no key. Renders to WAV bytes in
// memory so the game plays it through its own audio engine (3D, at the speaker).
// Call from the worker thread; each call initialises COM for that thread.

#include <string>
#include <utility>
#include <vector>

namespace Poseidon::NpcChat
{
/// Installed voice descriptions ("Microsoft David Desktop - English (United States)").
std::vector<std::string> SapiVoices();
/// The same voices with their gender from the voice token ("Male" / "Female" / "").
std::vector<std::pair<std::string, std::string>> SapiVoicesWithGender();
struct SapiVoiceInfo
{
    std::string name;
    std::string gender; // "Male" / "Female" / ""
    bool english = false;
};
/// Classic and OneCore voices with gender and language.
std::vector<SapiVoiceInfo> SapiVoiceList();
/// `voice`: a description substring from SapiVoices() (empty = system default).
/// `rate`: -10..10.
bool SapiSpeakToWav(const std::string& text, const std::string& voice, int rate, std::string& wav,
                    std::string& error);
} // namespace Poseidon::NpcChat
