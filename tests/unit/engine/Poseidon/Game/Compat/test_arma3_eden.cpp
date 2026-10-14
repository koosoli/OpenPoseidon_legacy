// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Game/Compat/Arma3Eden.hpp>

#include <string>

// A synthetic Eden mission (no game data): one group of two -- a driver and his car, the
// car a crew link away -- a waypoint activated by a trigger, a marker and a logic.
namespace
{
const char* kEden = R"(version=53;
addons[]={"A3_Soft_F"};
randomSeed=7;
class Mission
{
	class Intel { briefingName="Test"; hour=12; };
	class Entities
	{
		items=4;
		class Item0
		{
			dataType="Group"; side="West";
			class Entities
			{
				items=3;
				class Item0
				{
					dataType="Object";
					class PositionInfo { position[]={100,5,200}; angles[]={0,1.5707964,0}; };
					side="West"; flags=7;
					class Attributes { name="driver1"; init="hint ""hi"""; isPlayer=1; rank="SERGEANT"; };
					id=1; type="B_Soldier_F";
				};
				class Item1
				{
					dataType="Object";
					class PositionInfo { position[]={102,5,200}; };
					side="West"; flags=4;
					id=2; type="B_MRAP_01_F";
				};
				class Item2
				{
					dataType="Waypoint"; position[]={300,5,400}; type="SeekAndDestroy"; showWP="NEVER";
					class Effects {}; id=3;
				};
			};
			class Attributes { name="grpAlpha"; behaviour="AWARE"; };
			class CrewLinks { class Links { items=1; class Item0 { linkID=0; item0=1; item1=2; class CustomData { role=1; }; }; }; };
			id=0;
		};
		class Item1
		{
			dataType="Trigger"; position[]={50,0,60}; angle=3.1415927;
			class Attributes { sizeA=100; sizeB=50; isRectangle=1; activationBy="ANYPLAYER"; repeatable=1; onActivation="hint ""in"""; timeout[]={1,2,3}; };
			id=4; type="EmptyDetector";
		};
		class Item2 { dataType="Marker"; position[]={1,2,3}; name="m1"; type="hd_dot"; colorName="ColorWEST"; id=5; };
		class Item3
		{
			dataType="Logic"; class PositionInfo { position[]={9,0,9}; }; id=6; type="Logic";
			class CustomAttributes { class Attribute0 { property="Logic_Radius"; expression="_this setVariable ['Radius',_value]"; class Value { class data { class type { type[]={"SCALAR"}; }; value=25; }; }; }; nAttributes=1; };
		};
	};
	class Connections { class Links { items=1; class Item0 { linkID=0; item0=3; item1=4; class CustomData { type="WaypointActivation"; }; }; }; };
};
)";

bool Has(const std::string& text, const char* what)
{
    return text.find(what) != std::string::npos;
}
} // namespace

TEST_CASE("Arma 3 Eden mission becomes the Arma 2 layout", "[arma3][mission]")
{
    using namespace Poseidon::Arma1Compat;
    std::string out;
    EdenReport report;
    REQUIRE(ConvertArma3MissionText(kEden, out, report));
    CHECK(report.eden);
    CHECK(report.groups == 2); // the group and the logic's own
    CHECK(report.units == 2);  // driver + logic; the car went to Vehicles
    CHECK(report.vehicles == 1);
    CHECK(report.waypoints == 1);
    CHECK(report.triggers == 1);
    CHECK(report.markers == 1);
    CHECK(report.links == 1);

    CHECK(Has(out, "version=12;"));
    CHECK(Has(out, "side=\"WEST\";"));
    CHECK(Has(out, "vehicle=\"B_Soldier_F\";"));
    CHECK(Has(out, "player=\"PLAYER COMMANDER\";"));
    CHECK(Has(out, "azimut=90"));
    CHECK(Has(out, "this moveInDriver op_eden_v2"));
    CHECK(Has(out, "grpAlpha = group this"));
    CHECK(Has(out, "text=\"op_eden_v2\";"));
    CHECK(Has(out, "type=\"SAD\";"));
    CHECK(Has(out, "a=100;"));
    CHECK(Has(out, "rectangular=1;"));
    CHECK(Has(out, "activationBy=\"ANY\";"));
    CHECK(Has(out, "repeating=1;"));
    CHECK(Has(out, "timeoutMid=2;"));
    CHECK(Has(out, "angle=180"));
    CHECK(Has(out, "synchronizations[]={0};"));
    CHECK(Has(out, "_this setVariable ['Radius',_value]"));
    CHECK(Has(out, "side=\"LOGIC\";"));
}

TEST_CASE("Arma 2 layout: pairwise synchronisations become groups of two", "[arma3][mission]")
{
    using namespace Poseidon::Arma1Compat;
    const char* legacy = R"(version=12;
class Mission
{
	class Groups { items=1; class Item0 { side="WEST";
		class Vehicles { items=1; class Item0 { position[]={0,0,0}; id=0; side="WEST"; vehicle="B_Soldier_F"; leader=1; }; };
		class Waypoints { items=1; class Item0 { position[]={1,0,1}; type="MOVE"; synchronizations[]={5,6}; syncId=4; }; };
	}; };
	class Sensors { items=2;
		class Item0 { position[]={0,0,0}; synchronizations[]={4}; syncId=5; };
		class Item1 { position[]={0,0,0}; synchronizations[]={4}; syncId=6; };
	};
};
)";
    std::string out;
    EdenReport report;
    REQUIRE(ConvertArma3MissionText(legacy, out, report));
    CHECK_FALSE(report.eden);
    CHECK(Has(out, "synchronizations[]={0,1};")); // the waypoint, linked to both triggers
    CHECK(Has(out, "synchronizations[]={0};"));
    CHECK(Has(out, "synchronizations[]={1};"));
    CHECK_FALSE(Has(out, "syncId"));
}
