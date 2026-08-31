// Integration side of the voice-handle work (roadmap A.2).
//
// test_voice_handle.cpp proves the TABLE is correct with no device. These prove
// the table is CONNECTED: that SoundSystemOAL really issues a handle per
// registered wave, really retires it when the wave dies, and that
// ResumeMusicForPreview really runs through Resolve rather than the old
// pointer-in-registry check. A pure unit test of a table nothing calls would be
// exactly the "machinery ahead of its consumer" this project keeps paying for.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Audio/IAudioSystem.hpp>
#include <PoseidonOpenAL/SoundSystemOAL.hpp>

#include "../test_fixtures.hpp"

#include <fstream>
#include <vector>

using namespace Poseidon;

namespace
{

std::vector<char> ReadFixtureBytes(const char* relPath)
{
    const char* path = TestFixtures::GetTestFixturePath(relPath);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        return {};
    }
    auto end = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(end));
    f.read(buf.data(), end);
    return buf;
}

} // namespace

TEST_CASE("A.2-OAL: every registered wave holds a voice handle for exactly its registered lifetime",
          "[audio][VoiceHandle][oal]")
{
    auto* sys = dynamic_cast<SoundSystemOAL*>(CreateSoundSystemOAL());
    if (!sys)
    {
        SKIP("OpenAL not available");
    }
    const std::vector<char> wav = ReadFixtureBytes("audio/tone.wav");
    if (wav.empty())
    {
        delete sys;
        SKIP("audio/tone.wav fixture not available");
    }

    const int baseLive = sys->GetCounters().liveVoices;
    const int baseStale = sys->GetCounters().staleVoiceHandles;

    {
        // CreateWaveFromMemory hands back a wave at refcount 0; Ref takes the
        // first reference and the scope exit drops it to 0, which destroys it.
        Ref<IWave> a = sys->CreateWaveFromMemory(wav.data(), wav.size(), ".wav", false);
        REQUIRE(a.GetRef() != nullptr);
        {
            Ref<IWave> b = sys->CreateWaveFromMemory(wav.data(), wav.size(), ".wav", false);
            REQUIRE(b.GetRef() != nullptr);
            // Anti-vacuity: if RegisterWave were not issuing handles this would
            // still read baseLive and the rest of the test would prove nothing.
            CHECK(sys->GetCounters().liveVoices == baseLive + 2);
        }
        CHECK(sys->GetCounters().liveVoices == baseLive + 1);
    }
    CHECK(sys->GetCounters().liveVoices == baseLive);

    // Retiring a handle is not itself a stale op -- only USING a dead one is.
    CHECK(sys->GetCounters().staleVoiceHandles == baseStale);

    delete sys;
}

TEST_CASE("A.2-OAL: a music voice destroyed while suppressed is refused by generation, not by address",
          "[audio][VoiceHandle][oal]")
{
    // The bug the handles were introduced for. Before this change
    // ResumeMusicForPreview held a raw WaveOAL* and proved liveness by finding
    // it again in the _waves registry -- which cannot distinguish "still the
    // same wave" from "a different wave the allocator put at that address".
    //
    // A unit test cannot force the allocator to reuse a specific block, so what
    // is asserted here is the half that IS deterministic: the dead voice's
    // handle is refused, the refusal is counted, and the surviving voice is
    // still restored. The address-reuse half is pinned deterministically in
    // test_voice_handle.cpp with placement new.
    auto* sys = dynamic_cast<SoundSystemOAL*>(CreateSoundSystemOAL());
    if (!sys)
    {
        SKIP("OpenAL not available");
    }
    const std::vector<char> wav = ReadFixtureBytes("audio/tone.wav");
    if (wav.empty())
    {
        delete sys;
        SKIP("audio/tone.wav fixture not available");
    }

    Ref<IWave> doomed = sys->CreateWaveFromMemory(wav.data(), wav.size(), ".wav", false);
    Ref<IWave> survivor = sys->CreateWaveFromMemory(wav.data(), wav.size(), ".wav", false);
    if (!doomed.GetRef() || !survivor.GetRef())
    {
        delete sys;
        SKIP("could not create waves from the tone fixture");
    }
    doomed->SetKind(WaveMusic);
    survivor->SetKind(WaveMusic);
    doomed->Play();
    survivor->Play();
    sys->Commit();

    sys->SuppressMusicForPreview();
    if (sys->CountSuppressedMusicWaves() < 2)
    {
        // The waves did not reach a playing state on this device, so the
        // scenario cannot be built. Asserting on it anyway would be asserting
        // on nothing -- skip loudly instead.
        doomed = nullptr;
        survivor = nullptr;
        delete sys;
        SKIP("music waves did not reach a playing state on this device");
    }

    const int baseStale = sys->GetCounters().staleVoiceHandles;
    doomed = nullptr; // dies while its handle is still in _suppressedMusic

    sys->ResumeMusicForPreview();

    // Exactly one refusal: the dead voice's handle. The survivor resolved.
    CHECK(sys->GetCounters().staleVoiceHandles == baseStale + 1);
    CHECK(sys->CountSuppressedMusicWaves() == 0);

    survivor = nullptr;
    delete sys;
}
