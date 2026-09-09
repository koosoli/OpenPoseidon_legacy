#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/CommandDelivery.hpp>
#include <Poseidon/AI/ArcadeTemplate.hpp>
#include <Poseidon/Core/SaveVersion.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace Poseidon;

TEST_CASE("ai compiles", "[ai]")
{
    REQUIRE(sizeof(AIGroup) > 0);
}

TEST_CASE("Only direct offline player orders bypass speech delivery", "[ai][command-delivery]")
{
    CHECK(PlayerOrderBypassesSpeech(Command::CtxUI, false));
    CHECK(PlayerOrderBypassesSpeech(Command::CtxUIWithJoin, false));
    for (int context = Command::CtxUndefined; context <= Command::CtxUIWithJoin; ++context)
    {
        CHECK_FALSE(PlayerOrderBypassesSpeech(static_cast<Command::Context>(context), true));
        if (context < Command::CtxUI)
            CHECK_FALSE(PlayerOrderBypassesSpeech(static_cast<Command::Context>(context), false));
    }
}

namespace
{
class ArcadeMarkerTextFixture : public SerializeClass
{
  public:
    ArcadeMarkerInfo marker;

    LSError Serialize(ParamArchive& ar) override { return marker.Serialize(ar); }
};

class ArcadeWaypointTextFixture : public SerializeClass
{
  public:
    ArcadeWaypointInfo waypoint;

    LSError Serialize(ParamArchive& ar) override { return waypoint.Serialize(ar); }
};

class ArcadeSensorTextFixture : public SerializeClass
{
  public:
    ArcadeSensorInfo sensor;

    LSError Serialize(ParamArchive& ar) override { return sensor.Serialize(ar); }
};
} // namespace

TEST_CASE("mission arcade user text decodes legacy CP1250 on load", "[ai][arcade][encoding]")
{
    const RString oldLanguage = GLanguage;
    REQUIRE(Poseidon::SetLanguage("English"));

    const std::filesystem::path dir = std::filesystem::current_path() / "tmp";
    std::filesystem::create_directories(dir);
    const std::filesystem::path markerPath = dir / "arcade-marker-text-cp1250.bin";
    const std::filesystem::path waypointPath = dir / "arcade-waypoint-text-cp1250.bin";
    const std::filesystem::path sensorPath = dir / "arcade-sensor-text-cp1250.bin";

    {
        ArcadeMarkerTextFixture fixture;
        fixture.marker.position = VZero;
        fixture.marker.name = "dopad";
        fixture.marker.type = "mil_dot";
        fixture.marker.text = "M\xEDsto dopadu";

        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("ArcadeMarker", fixture, 1) == LSOK);
        REQUIRE(ar.SaveBin(markerPath.string().c_str()));
    }

    {
        ArcadeWaypointTextFixture fixture;
        fixture.waypoint.description = "Dob\xFDt z\xE1"
                                       "kladnu";

        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("ArcadeWaypoint", fixture, 1) == LSOK);
        REQUIRE(ar.SaveBin(waypointPath.string().c_str()));
    }

    {
        ArcadeSensorTextFixture fixture;
        fixture.sensor.text = "Z\xE1"
                              "kladna v hor\xE1"
                              "ch";

        ParamArchiveSave ar(WorldSerializeVersion);
        REQUIRE(ar.Serialize("ArcadeSensor", fixture, 1) == LSOK);
        REQUIRE(ar.SaveBin(sensorPath.string().c_str()));
    }

    {
        ArcadeMarkerTextFixture fixture;
        ATSParams params;
        ParamArchiveLoad ar;
        ar.SetParams(&params);
        REQUIRE(ar.LoadBin(markerPath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("ArcadeMarker", fixture, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("ArcadeMarker", fixture, 1) == LSOK);

        CHECK(std::string(fixture.marker.text.Data()) == "M\xC3\xADsto dopadu");
    }

    {
        ArcadeWaypointTextFixture fixture;
        ATSParams params;
        ParamArchiveLoad ar;
        ar.SetParams(&params);
        REQUIRE(ar.LoadBin(waypointPath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("ArcadeWaypoint", fixture, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("ArcadeWaypoint", fixture, 1) == LSOK);

        CHECK(std::string(fixture.waypoint.description.Data()) == "Dob\xC3\xBDt z\xC3\xA1"
                                                                  "kladnu");
    }

    {
        ArcadeSensorTextFixture fixture;
        ATSParams params;
        ParamArchiveLoad ar;
        ar.SetParams(&params);
        REQUIRE(ar.LoadBin(sensorPath.string().c_str()));
        ar.FirstPass();
        REQUIRE(ar.Serialize("ArcadeSensor", fixture, 1) == LSOK);
        ar.SecondPass();
        REQUIRE(ar.Serialize("ArcadeSensor", fixture, 1) == LSOK);

        CHECK(std::string(fixture.sensor.text.Data()) == "Z\xC3\xA1"
                                                         "kladna v hor\xC3\xA1"
                                                         "ch");
    }

    std::filesystem::remove(markerPath);
    std::filesystem::remove(waypointPath);
    std::filesystem::remove(sensorPath);
    if (oldLanguage.GetLength() > 0)
    {
        REQUIRE(Poseidon::SetLanguage(oldLanguage));
    }
}
