// HRTF mode plumbing (roadmap Phase 1.4): the persisted setting round-trips, invalid
// values normalize to Auto, non-OAL backends report Unsupported, and on a machine with
// a real OpenAL device the mode actually reaches the device and the status readout
// answers. The audible question -- does it SOUND binaural -- is the owner's; this file
// covers everything short of ears.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Audio/Dummy/SoundSystemDummy.hpp>
#include <Poseidon/Audio/IAudioSystem.hpp>
#include <Poseidon/UI/Settings/AudioConfig.hpp>
#include <PoseidonOpenAL/SoundSystemOAL.hpp>

#include <AL/alc.h>
#include <AL/alext.h>

#include <cstdio>
#include <string>

namespace
{
// Normalize() needs an Environment; device validity is irrelevant to hrtfMode.
struct NoDevices : Poseidon::AudioConfig::Environment
{
    std::vector<std::string> ListOutputDevices() const override { return {}; }
    std::vector<std::string> ListInputDevices() const override { return {}; }
};
} // namespace

TEST_CASE("AudioConfig: hrtfMode persists and normalizes", "[audio][hrtf]")
{
    Poseidon::AudioConfig cfg;
    CHECK(cfg.hrtfMode == 1); // default Auto

    cfg.hrtfMode = 7;
    NoDevices env;
    CHECK(cfg.Normalize(env));
    CHECK(cfg.hrtfMode == 1);

    cfg.hrtfMode = 2;
    const std::string path = std::string(std::tmpnam(nullptr)) + "-hrtf.cfg";
    REQUIRE(cfg.Save(path));
    Poseidon::AudioConfig loaded;
    REQUIRE(loaded.Load(path));
    CHECK(loaded.hrtfMode == 2);
    std::remove(path.c_str());
}

TEST_CASE("Dummy backend: HRTF reports unsupported", "[audio][hrtf]")
{
    Poseidon::SoundSystemDummy dummy;
    Poseidon::IAudioSystem& sys = dummy;
    CHECK_FALSE(sys.SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::On));
    CHECK_FALSE(sys.GetHrtfStatus().supported);
    CHECK_FALSE(sys.GetHrtfStatus().active);
}

TEST_CASE("OpenAL backend: HRTF mode reaches the device when supported", "[audio][hrtf][.device]")
{
    // Tagged [.device]: needs a real playback device, so it is skipped by default and
    // run explicitly (PoseidonTests.exe "[device]") on a workstation.
    Poseidon::IAudioSystem* sys = Poseidon::CreateSoundSystemOAL();
    if (!sys)
    {
        SKIP("no OpenAL playback device");
    }
    const Poseidon::IAudioSystem::HrtfStatus status = sys->GetHrtfStatus();
    if (!status.supported)
    {
        delete sys;
        SKIP("ALC_SOFT_HRTF not supported on this device");
    }
    CHECK(sys->SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::On));
    CHECK(sys->GetHrtfStatus().active);
    CHECK_FALSE(sys->GetHrtfStatus().specifier.empty());
    CHECK(sys->SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::Off));
    CHECK_FALSE(sys->GetHrtfStatus().active);
    CHECK(sys->SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::Auto));
    delete sys;
}

TEST_CASE("OpenAL: Auto (DONT_CARE reset) must equal an untouched device", "[audio][hrtf][.device]")
{
    // The root-cause probe for the 2026-08-30 owner report (too much reverb, no
    // Auto-vs-On difference, slightly lower fps): if resetting the device with
    // ALC_DONT_CARE_SOFT ENGAGES HRTF where a plain nullptr-attrs context leaves it
    // off, then shipping Auto-applies-at-boot silently turned HRTF on for everyone,
    // and "Auto" must mean "do not touch the device" instead.
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device)
    {
        SKIP("no playback device");
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    REQUIRE(context);
    alcMakeContextCurrent(context);

    if (alcIsExtensionPresent(device, "ALC_SOFT_HRTF") != ALC_TRUE)
    {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        alcCloseDevice(device);
        SKIP("ALC_SOFT_HRTF unsupported");
    }
    ALCint untouched = -1;
    alcGetIntegerv(device, ALC_HRTF_SOFT, 1, &untouched);

    auto reset = reinterpret_cast<LPALCRESETDEVICESOFT>(alcGetProcAddress(device, "alcResetDeviceSOFT"));
    REQUIRE(reset != nullptr);
    const ALCint attrs[] = {ALC_HRTF_SOFT, ALC_DONT_CARE_SOFT, 0};
    REQUIRE(reset(device, attrs) == ALC_TRUE);
    ALCint afterDontCare = -1;
    alcGetIntegerv(device, ALC_HRTF_SOFT, 1, &afterDontCare);

    INFO("untouched=" << untouched << " afterDontCare=" << afterDontCare);
    CHECK(afterDontCare == untouched);

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
}
