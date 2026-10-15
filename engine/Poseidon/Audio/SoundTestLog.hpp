#pragma once

// Sound census for unattended test runs (--arma1/--arma2/--arma3-test-seconds).
//
// A wave that loads and plays is logged only at debug level, and a config slot whose sound
// name is empty is skipped without a word, so a run in which nothing could be heard leaves
// no trace in the log. Here every wave the game asks for is counted -- requested, empty,
// failed to load, played -- and the test prints the totals and the names when it ends.

#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace Poseidon::Audio::SoundTestLog
{
struct Entry
{
    int requested = 0;
    int failed = 0;
    int played = 0;
};

inline std::atomic<int>& EnabledFlag()
{
    static std::atomic<int> flag{-1}; // -1 unknown, 0 off, 1 on
    return flag;
}

inline void Enable(bool on) { EnabledFlag().store(on ? 1 : 0); }
inline bool Enabled() { return EnabledFlag().load() == 1; }

inline std::mutex& Mutex()
{
    static std::mutex m;
    return m;
}

inline std::map<std::string, Entry>& Entries()
{
    static std::map<std::string, Entry> entries;
    return entries;
}

inline std::atomic<int>& EmptyRequests()
{
    static std::atomic<int> n{0};
    return n;
}

inline void NoteRequest(const char* name, bool loaded)
{
    if (!Enabled())
        return;
    if (!name || !name[0])
    {
        EmptyRequests()++;
        return;
    }
    std::lock_guard<std::mutex> lock(Mutex());
    Entry& e = Entries()[name];
    e.requested++;
    if (!loaded)
        e.failed++;
}

inline void NotePlay(const char* name)
{
    if (!Enabled() || !name || !name[0])
        return;
    std::lock_guard<std::mutex> lock(Mutex());
    Entries()[name].played++;
}
} // namespace Poseidon::Audio::SoundTestLog
