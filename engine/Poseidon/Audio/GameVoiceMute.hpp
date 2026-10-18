#pragma once

// Silences the game's own spoken radio (the CfgVoice word-by-word messages: "Two, roger",
// "Form line", radio static) without touching anything else that uses the speech channel.
//
// The waves are still created and still "play" at zero volume: the radio queue waits for each
// word to finish before it passes a message on (and marks it transmitted on play), so not
// playing them at all would stall or reorder the radio. The chat text still appears.
//
// Set by the NPC chat (its voices replace these); read in BasicSpeaker::Say and
// RadioChannel::Say.

#include <atomic>

namespace Poseidon
{
inline std::atomic<bool>& GameVoicesMutedFlag()
{
    static std::atomic<bool> muted{false};
    return muted;
}

inline bool GameVoicesMuted()
{
    return GameVoicesMutedFlag().load(std::memory_order_relaxed);
}

inline void SetGameVoicesMuted(bool muted)
{
    GameVoicesMutedFlag().store(muted, std::memory_order_relaxed);
}
} // namespace Poseidon
