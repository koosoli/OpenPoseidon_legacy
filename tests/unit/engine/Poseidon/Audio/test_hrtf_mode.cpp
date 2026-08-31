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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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
    // An unimplemented backend must not claim a reason it cannot know.
    CHECK(sys.GetHrtfStatus().deviceStatus == Poseidon::IAudioSystem::HrtfDeviceStatus::Unknown);
    CHECK(sys.ListHrtfSpecifiers().empty());
}

TEST_CASE("HRTF status: the ALC_HRTF_STATUS_SOFT enum matches the ALC tokens", "[audio][hrtf]")
{
    // The engine-layer enum is a hand-copy of the ALC tokens so IAudioSystem.hpp does
    // not need AL headers. If OpenAL ever renumbers, GetHrtfStatus would mis-map every
    // reason silently -- this is the guard.
    using S = Poseidon::IAudioSystem::HrtfDeviceStatus;
    CHECK(static_cast<int>(S::Disabled) == ALC_HRTF_DISABLED_SOFT);
    CHECK(static_cast<int>(S::Enabled) == ALC_HRTF_ENABLED_SOFT);
    CHECK(static_cast<int>(S::Denied) == ALC_HRTF_DENIED_SOFT);
    CHECK(static_cast<int>(S::Required) == ALC_HRTF_REQUIRED_SOFT);
    CHECK(static_cast<int>(S::HeadphonesDetected) == ALC_HRTF_HEADPHONES_DETECTED_SOFT);
    CHECK(static_cast<int>(S::UnsupportedFormat) == ALC_HRTF_UNSUPPORTED_FORMAT_SOFT);
}

TEST_CASE("HRTF status: 'off' and 'refused' are distinguishable", "[audio][hrtf]")
{
    // The whole point of carrying the status code: a log the owner mails in must let
    // us tell "the user turned it off" from "the user asked and the driver said no".
    using Sys = Poseidon::IAudioSystem;
    using S = Sys::HrtfDeviceStatus;

    CHECK_FALSE(Sys::HrtfWasRefused(S::Disabled));
    CHECK_FALSE(Sys::HrtfWasRefused(S::Enabled));
    CHECK_FALSE(Sys::HrtfWasRefused(S::Required));
    CHECK_FALSE(Sys::HrtfWasRefused(S::HeadphonesDetected));
    CHECK_FALSE(Sys::HrtfWasRefused(S::Unknown));
    CHECK(Sys::HrtfWasRefused(S::Denied));
    CHECK(Sys::HrtfWasRefused(S::UnsupportedFormat));

    // Every code must produce its own text, or the log cannot be read back.
    const S all[] = {S::Disabled, S::Enabled,             S::Denied,
                     S::Required, S::HeadphonesDetected,  S::UnsupportedFormat,
                     S::Unknown};
    std::vector<std::string> seen;
    for (S s : all)
    {
        const char* text = Sys::HrtfDeviceStatusText(s);
        REQUIRE(text != nullptr);
        CHECK(text[0] != '\0');
        seen.emplace_back(text);
    }
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());

    // An out-of-range cast (a driver returning something new) must not fall off the end.
    CHECK(std::string(Sys::HrtfDeviceStatusText(static_cast<S>(99))) == "unknown");
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
    using S = Poseidon::IAudioSystem::HrtfDeviceStatus;

    // A supported device must have enumerated at least one dataset by name, otherwise
    // the "0 datasets" warning path should have fired instead.
    const std::vector<std::string> datasets = sys->ListHrtfSpecifiers();
    std::string joined;
    for (const std::string& name : datasets)
        joined += (joined.empty() ? "" : ", ") + name;
    // WARN so the dataset names reach the console: this is the readout an owner is
    // asked for when HRTF "does nothing", and it is otherwise only in the engine log.
    WARN("HRTF datasets (" << datasets.size() << "): " << joined);
    CHECK_FALSE(datasets.empty());
    for (const std::string& name : datasets)
        CHECK_FALSE(name.empty());

    CHECK(sys->SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::On));
    {
        const Poseidon::IAudioSystem::HrtfStatus on = sys->GetHrtfStatus();
        CHECK(on.active);
        CHECK_FALSE(on.specifier.empty());
        // Active must be corroborated by the reason code, and the active dataset must
        // be one the probe enumerated.
        CHECK((on.deviceStatus == S::Enabled || on.deviceStatus == S::Required ||
               on.deviceStatus == S::HeadphonesDetected));
        CHECK(std::find(datasets.begin(), datasets.end(), on.specifier) != datasets.end());
    }

    CHECK(sys->SetHrtfMode(Poseidon::IAudioSystem::HrtfMode::Off));
    {
        const Poseidon::IAudioSystem::HrtfStatus off = sys->GetHrtfStatus();
        CHECK_FALSE(off.active);
        // Off must read as Disabled, NOT as Denied -- that is the distinction the
        // status code exists to make.
        CHECK(off.deviceStatus == S::Disabled);
        CHECK_FALSE(Poseidon::IAudioSystem::HrtfWasRefused(off.deviceStatus));
    }

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
