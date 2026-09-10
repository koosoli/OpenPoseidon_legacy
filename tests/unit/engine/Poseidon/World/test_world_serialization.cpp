#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/SaveVersion.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Visibility.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <catch2/matchers/catch_matchers.hpp>
#include <string>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

using Poseidon::Foundation::Time;

using namespace Poseidon;
LSError SerializeWorldSimulationTime(ParamArchive& ar);

namespace
{
class SimulationTimeArchiveFixture : public SerializeClass
{
  public:
    LSError Serialize(ParamArchive& ar) override { return SerializeWorldSimulationTime(ar); }
};

class ScopedSimulationTime
{
  public:
    explicit ScopedSimulationTime(Time value) : _saved(Glob.time) { Glob.time = value; }

    ~ScopedSimulationTime() { Glob.time = _saved; }

  private:
    Time _saved;
};
} // namespace

TEST_CASE("world save serialization preserves simulation time", "[world][save][load]")
{
    const std::filesystem::path dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const std::filesystem::path archivePath = dir / "world-simulation-time.bin";
    SimulationTimeArchiveFixture fixture;

    {
        ScopedSimulationTime timeGuard(Time(0));
        Glob.time += 42.5f;

        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("World", fixture, 1) == LSOK);
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }

    {
        ScopedSimulationTime timeGuard(Time(0));

        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("World", fixture, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("World", fixture, 1) == LSOK);

        CHECK_THAT(Glob.time - Time(0), Catch::Matchers::WithinAbs(42.5f, 0.001f));
    }

    std::filesystem::remove(archivePath);
}

TEST_CASE("world archive rejects newer versions before field mutation", "[world][save][load][save-version]")
{
    const auto dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const auto path = dir / "world-version-gate.bin";
    for (const int version : {1, WorldSerializeVersion - 1, WorldSerializeVersion, WorldSerializeVersion + 1})
    {
        INFO(version);
        {
            ParamArchiveSave writer(version);
            REQUIRE(writer.CheckMaxVersion(WorldSerializeVersion) == LSOK);
            int value = 77;
            REQUIRE(writer.Serialize("value", value, 1) == LSOK);
            REQUIRE(writer.SaveBin(path.string().c_str()));
        }
        ParamArchiveLoad reader;
        REQUIRE(reader.LoadBin(path.string().c_str()));
        int value = 19;
        for (int pass = 0; pass < 2; ++pass)
        {
            if (pass == 0) reader.FirstPass(); else reader.SecondPass();
            const auto result = reader.CheckMaxVersion(WorldSerializeVersion);
            if (result == LSOK)
                REQUIRE(reader.Serialize("value", value, 1) == LSOK);
            if (version > WorldSerializeVersion)
            {
                REQUIRE(result == LSVersionTooNew);
                REQUIRE(value == 19);
                REQUIRE(std::string(ParamArchive::GetErrorContext()).find("version") != std::string::npos);
            }
            else
            {
                REQUIRE(result == LSOK);
                REQUIRE(value == 77);
            }
        }
    }
    std::filesystem::remove(path);
    ParamArchive::CancelError();
}

namespace
{
// Save-file quirk: `enableRadio false` set by a mission (e.g. the
// CWC "camping" intro) was lost across save/load. World::Serialize never wrote
// World::_enableRadio, so on load it reverted to the mission-init default (true)
// and the radio sentences the mission had silenced played again. The runtime
// audibility is driven entirely off _enableRadio every frame
// (World::SetActiveChannels -> _radio->SetAudible(_enableRadio)), so the missing
// serialize is the whole bug. The fix is one line in World::Serialize, adopted
// verbatim from ArmA1/Futura:  ar.Serialize("enableRadio", _enableRadio, 1, true).
// Default true means pre-fix saves (no entry) still load as radio-on. This
// fixture replays that one line; `writeKey=false` reproduces the pre-fix save
// shape (the entry is simply absent).
class EnableRadioFixture : public SerializeClass
{
  public:
    bool enableRadio = true;
    bool writeKey = true; // false = pre-fix save: World::Serialize wrote no entry

    LSError Serialize(ParamArchive& ar) override
    {
        if (ar.IsSaving() && !writeKey)
        {
            return LSOK;
        }
        return ar.Serialize("enableRadio", enableRadio, 1, true);
    }
};
} // namespace

TEST_CASE("enableRadio false round-trips through save/load", "[world][save][load][radio]")
{
    const std::filesystem::path dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const std::filesystem::path archivePath = dir / "world-enable-radio.bin";

    // Fixed engine: a mission that ran `enableRadio false`, saved, then reloaded.
    {
        EnableRadioFixture saver;
        saver.enableRadio = false;
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("World", saver, 1) == LSOK);
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }
    {
        EnableRadioFixture loader;
        loader.enableRadio = true; // post-load default before deserialize (EnableRadio() at mission init)
        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        REQUIRE(loader.enableRadio == false);
    }

    // Pre-fix engine: World::Serialize wrote no "enableRadio" entry, so the loader
    // falls back to the default true -- the bug (silenced radio audible again).
    {
        EnableRadioFixture saver;
        saver.enableRadio = false;
        saver.writeKey = false;
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("World", saver, 1) == LSOK);
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }
    {
        EnableRadioFixture loader;
        loader.enableRadio = true;
        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        CHECK(loader.enableRadio == true); // backward compat: old save -> radio on
    }

    std::filesystem::remove(archivePath);
}

namespace
{
// Save-file quirk: `disableAI` state was both lost across load AND silently wiped on
// every save. AIUnit::Serialize ran an unconditional `_disabledAI = 0;` where the
// serialize belonged -- so saving a frozen sentry (disableAI "MOVE"/"TARGET")
// zeroed its live _disabledAI mid-save, and loading always re-enabled all AI.
// The fix replaces that reset with  ar.Serialize("disabledAI", _disabledAI, 1, 0)
// (adopted from ArmA1/Futura). _disabledAI is a pure read-only behavioral gate --
// DAMove/DATarget/DAAutoTarget/DAAnim are only ever tested in the pilot, target,
// and animation paths -- so restoring the raw value needs no subsystem re-init.
// Default 0 keeps pre-fix saves loading fully-enabled. `legacyReset=true` replays
// the pre-fix code path.
class DisabledAIFixture : public SerializeClass
{
  public:
    int disabledAI = 0;
    bool legacyReset = false; // true = pre-fix `_disabledAI = 0;` in place of the serialize

    LSError Serialize(ParamArchive& ar) override
    {
        if (legacyReset)
        {
            disabledAI = 0; // ran on both save and load passes -- clobbers live state on save
            return LSOK;
        }
        return ar.Serialize("disabledAI", disabledAI, 1, 0);
    }
};
} // namespace

TEST_CASE("disableAI state round-trips and is not wiped on save", "[world][save][load][ai]")
{
    const int frozen = 0x2 | 0x1; // AIUnit::DAMove | DATarget -- sentry that must not move or pick targets

    // Broken-state delta: the pre-fix unconditional reset zeroes the live unit
    // during a save pass (saving a frozen unit silently un-freezes it).
    {
        DisabledAIFixture saver;
        saver.disabledAI = frozen;
        saver.legacyReset = true;
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("Unit", saver, 1) == LSOK);
        CHECK(saver.disabledAI == 0);
    }

    const std::filesystem::path dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const std::filesystem::path archivePath = dir / "ai-disabled.bin";

    // Fixed engine: save preserves live state, and load restores it.
    {
        DisabledAIFixture saver;
        saver.disabledAI = frozen;
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("Unit", saver, 1) == LSOK);
        CHECK(saver.disabledAI == frozen); // not clobbered by the save
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }
    {
        DisabledAIFixture loader;
        loader.disabledAI = 0; // a freshly-built unit starts fully enabled
        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("Unit", loader, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("Unit", loader, 1) == LSOK);
        REQUIRE(loader.disabledAI == frozen);
    }

    // Backward compat: a save with no "disabledAI" entry (pre-fix, or a unit that
    // was fully enabled) loads as 0 rather than leaking the loader's prior value.
    {
        DisabledAIFixture saver;
        saver.disabledAI = frozen;
        saver.legacyReset = true; // writes nothing
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("Unit", saver, 1) == LSOK);
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }
    {
        DisabledAIFixture loader;
        loader.disabledAI = 999;
        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("Unit", loader, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("Unit", loader, 1) == LSOK);
        CHECK(loader.disabledAI == 0);
    }

    std::filesystem::remove(archivePath);
}

namespace
{
// Mirrors World::Serialize's SensorList handling (engine/.../World/WorldImpl.cpp). The world
// serializes its sensor list as an SRef<SensorList>. ParamArchive deserializes an SRef as
// null when the named subclass is absent from the save (older / partially-written saves), and
// World::Simulate dereferences GetSensorList() every frame -- so a null sensor list is a
// latent crash (SensorList::CheckPos with this == null; the re-mount/load AV reported on the
// mods branch). The fix restores an empty list on the final load pass; this fixture replays
// the same serialize+restore. `omitOnSave` produces the bad save shape.
class SensorListSaveFixture : public SerializeClass
{
  public:
    SRef<SensorList> sensorList;
    bool omitOnSave = false;

    LSError Serialize(ParamArchive& ar) override
    {
        // Plain LSError propagation (the engine's CHECK macro collides with Catch2's CHECK).
        if (ar.IsSaving())
        {
            return omitOnSave ? LSOK : ar.Serialize("SensorList", sensorList, 1);
        }
        const LSError err = ar.Serialize("SensorList", sensorList, 1);
        if (err == LSOK && ar.GetPass() == ParamArchive::PassSecond && !sensorList)
        {
            sensorList = new SensorList;
        }
        return err;
    }
};
} // namespace

TEST_CASE("world load without a SensorList entry yields a non-null sensor list", "[world][save][load][sensor]")
{
    const std::filesystem::path dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const std::filesystem::path archivePath = dir / "world-missing-sensorlist.bin";

    // Write a save that omits the "SensorList" subclass -- the shape of an older / partial save.
    {
        SensorListSaveFixture saver;
        saver.sensorList = new SensorList; // present in memory...
        saver.omitOnSave = true;           // ...but deliberately not written to the archive
        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("World", saver, 1) == LSOK);
        REQUIRE(ar.SaveBin(archivePath.string().c_str()));
    }

    // Load it back through the two-pass loader the engine uses (World::LoadBin).
    {
        SensorListSaveFixture loader;
        loader.sensorList = new SensorList; // a fresh world starts with a list (World ctor)

        ParamArchiveLoad ar;
        REQUIRE(ar.LoadBin(archivePath.string().c_str()));

        ar.FirstPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        // Broken-state delta: the absent entry nulls the SRef on the first pass. Without the
        // restore below, World::Simulate -> GetSensorList()->SmartUpdateAll() then dereferences
        // a null this (SensorList::CheckPos, read at +0x14 of a null base).
        CHECK(!loader.sensorList);

        ar.SecondPass();
        REQUIRE(ar.Serialize("World", loader, 1) == LSOK);
        // Fixed state: the final-pass restore re-establishes the world invariant.
        REQUIRE(static_cast<bool>(loader.sensorList));
    }

    std::filesystem::remove(archivePath);
}
