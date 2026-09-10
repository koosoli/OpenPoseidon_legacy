#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/InteriorShellRaster.hpp>
#include <Poseidon/World/Effects/InteriorFaceIndex.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>

#include <utility>
#include <vector>

using namespace Poseidon;

TEST_CASE("Indexed portal neighbours preserve exact legacy breadth-first order", "[BuildingInterior][Smoke][Raster]")
{
    struct Face
    {
        int c[3];
        int axis, side;
    };
    const int dim[3] = {7, 5, 8};
    std::vector<Face> faces;
    for (int x = 0; x < dim[0]; ++x)
        for (int y = 0; y < dim[1]; ++y)
            for (int z = 0; z < dim[2]; ++z)
                for (int axis = 0; axis < 3; ++axis)
                    for (int side : {-1, 1})
                        if ((x * 7 + y * 11 + z * 3 + axis) % 5 != 0)
                            faces.push_back({{x, y, z}, axis, side});
    for (int permutation = 0; permutation < 3; ++permutation)
    {
        const InteriorFaceIndex index(faces, dim);
        auto clusters = [&](bool indexed)
        {
            std::vector<std::vector<int>> all;
            std::vector<char> visited(faces.size(), 0);
            for (int start = 0; start < static_cast<int>(faces.size()); ++start)
            {
                if (visited[start])
                    continue;
                std::vector<int> cluster{start};
                visited[start] = 1;
                for (size_t head = 0; head < cluster.size(); ++head)
                {
                    const auto& a = faces[cluster[head]];
                    auto visit = [&](int i)
                    {
                        if (i >= 0 && !visited[i])
                        {
                            visited[i] = 1;
                            cluster.push_back(i);
                        }
                    };
                    if (indexed)
                    {
                        for (int i : index.Neighbours(a))
                            visit(i);
                    }
                    else
                        for (int i = 0; i < static_cast<int>(faces.size()); ++i)
                        {
                            const auto& b = faces[i];
                            const int t1 = (a.axis + 1) % 3, t2 = (a.axis + 2) % 3;
                            if (a.axis == b.axis && a.side == b.side && a.c[a.axis] == b.c[b.axis] &&
                                std::abs(a.c[t1] - b.c[t1]) + std::abs(a.c[t2] - b.c[t2]) == 1)
                                visit(i);
                        }
                }
                all.push_back(std::move(cluster));
            }
            return all;
        };
        CHECK(clusters(true) == clusters(false));
        std::reverse(faces.begin(), faces.end());
        std::rotate(faces.begin(), faces.begin() + faces.size() / 3, faces.end());
    }
}

TEST_CASE("Component-local interior raster preserves every seven-sample shell cell",
          "[BuildingInterior][Smoke][Raster]")
{
    const int dim[3] = {17, 13, 19};
    const Vector3 gridMin(-2.3f, -1.1f, 0.7f);
    struct Shell
    {
        Vector3 lo, hi;
        bool slope;
    };
    for (float cell : {0.25f, 0.7f})
        for (float offset : {-0.125f, 0.0f, 0.00001f, 0.124999f, 0.25f})
        {
            const Vector3 shift = gridMin + Vector3(offset, offset, offset);
            const Shell shells[] = {{shift + Vector3(0, 0, 0), shift + Vector3(0.3f, 3, 4), false},
                                    {shift + Vector3(0, 2, 0), shift + Vector3(4, 2.001f, 4), false},
                                    {shift + Vector3(1, 0, 1), shift + Vector3(3, 3, 3), true},
                                    {shift + Vector3(-10, -10, -10), shift + Vector3(-9, -9, -9), false},
                                    {shift + Vector3(50, 50, 50), shift + Vector3(51, 51, 51), false}};
            auto inside = [](const Shell& shell, Vector3Par p)
            {
                return p.X() >= shell.lo.X() && p.X() <= shell.hi.X() && p.Y() >= shell.lo.Y() &&
                       p.Y() <= shell.hi.Y() && p.Z() >= shell.lo.Z() && p.Z() <= shell.hi.Z() &&
                       (!shell.slope || (p - shell.lo).DotProduct(Vector3(1, 0.7f, 0.3f)) < 1.3f);
            };
            std::vector<unsigned char> reference(dim[0] * dim[1] * dim[2], 0xFF), actual = reference;
            const float h = cell * 0.5f;
            for (int x = 0; x < dim[0]; ++x)
                for (int y = 0; y < dim[1]; ++y)
                    for (int z = 0; z < dim[2]; ++z)
                    {
                        const Vector3 p = gridMin + Vector3((x + 0.5f) * cell, (y + 0.5f) * cell, (z + 0.5f) * cell);
                        const Vector3 samples[] = {p,
                                                   p + Vector3(h, 0, 0),
                                                   p - Vector3(h, 0, 0),
                                                   p + Vector3(0, h, 0),
                                                   p - Vector3(0, h, 0),
                                                   p + Vector3(0, 0, h),
                                                   p - Vector3(0, 0, h)};
                        for (const auto& sample : samples)
                            for (const auto& shell : shells)
                                if (inside(shell, sample))
                                    reference[(x * dim[1] + y) * dim[2] + z] = 0xFE;
                    }
            for (const auto& shell : shells)
                RasterizeInteriorShellComponent(actual, dim, gridMin, cell, shell.lo, shell.hi,
                                                [&](Vector3Par p) { return inside(shell, p); });
            INFO("cell=" << cell << " offset=" << offset);
            CHECK(actual == reference);
            // Overlapping Fire/View components must be an idempotent union.
            for (const auto& shell : shells)
                RasterizeInteriorShellComponent(actual, dim, gridMin, cell, shell.lo, shell.hi,
                                                [&](Vector3Par p) { return inside(shell, p); });
            CHECK(actual == reference);
        }
}

namespace Poseidon
{

struct BuildingInteriorTestAccess
{
    static BuildingInterior MakeColumn(std::vector<unsigned char> labels)
    {
        BuildingInterior interior;
        interior._valid = true;
        interior._roomCount = 1;
        interior._gridMin = VZero;
        interior._cell = 0.25f;
        interior._dim[0] = 1;
        interior._dim[1] = static_cast<int>(labels.size());
        interior._dim[2] = 1;
        interior._labels = std::move(labels);
        interior.BuildRoofColumns();
        return interior;
    }

    static BuildingInterior MakeRoomBox(int dimension)
    {
        BuildingInterior interior;
        interior._valid = true;
        interior._roomCount = 1;
        interior._gridMin = VZero;
        interior._cell = 0.25f;
        interior._dim[0] = dimension;
        interior._dim[1] = dimension;
        interior._dim[2] = dimension;
        interior._labels.assign(static_cast<size_t>(dimension * dimension * dimension), 0);
        return interior;
    }

    static void AddPortal(BuildingInterior& interior, const InteriorPortal& portal)
    {
        interior._portals.Add(portal);
    }
};

} // namespace Poseidon

TEST_CASE("Smoke building cache distinguishes nearby instances and outside air", "[BuildingInterior][Smoke]")
{
    const BuildingInterior interior = BuildingInteriorTestAccess::MakeRoomBox(4);
    Ref<ObjectPlain> first = new ObjectPlain(nullptr, 100);
    Ref<ObjectPlain> second = new ObjectPlain(nullptr, 101);
    first->SetPosition(VZero);
    second->SetPosition(Vector3(2, 0, 0));
    const Vector3 inFirst(0.5f, 0.5f, 0.5f);
    const Vector3 inSecond(2.5f, 0.5f, 0.5f);
    CHECK(SmokeCachedBuildingContains(&interior, first, inFirst));
    CHECK_FALSE(SmokeCachedBuildingContains(&interior, first, inSecond));
    CHECK(SmokeCachedBuildingContains(&interior, second, inSecond));
    CHECK_FALSE(SmokeCachedBuildingContains(&interior, second, inFirst));
    CHECK_FALSE(SmokeCachedBuildingContains(&interior, first, Vector3(1.5f, 0.5f, 0.5f)));
    CHECK_FALSE(SmokeCachedBuildingContains(nullptr, first, inFirst));
    CHECK_FALSE(SmokeCachedBuildingContains(&interior, nullptr, inFirst));
    const BuildingInterior invalid;
    CHECK_FALSE(SmokeCachedBuildingContains(&invalid, first, inFirst));
    first->NeverDestroy();
    CHECK_FALSE(SmokeCachedBuildingContains(&interior, first, inFirst));
    CHECK(SmokeCachedBuildingContains(&interior, second, inSecond));
}

TEST_CASE("Building interior segment classification cannot tunnel through a one-cell roof",
          "[BuildingInterior][Rain][Weather][World]")
{
    // Bottom two cells are room 0, the middle cell is solid shell, and the top
    // two are outside air. A single endpoint lookup would see only outside at
    // both ends; the segment classifier must still observe the roof.
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});

    REQUIRE(interior.ClassifyModel(Vector3(0.125f, 1.125f, 0.125f)) == -1);
    REQUIRE(interior.ClassifyModel(Vector3(0.125f, 0.125f, 0.125f)) == 0);
    REQUIRE(interior.ClassifySegmentModel(Vector3(0.125f, 1.125f, 0.125f),
                                          Vector3(0.125f, 0.125f, 0.125f)) == -2);
}

TEST_CASE("Rain may cross a shell where a portal opening sits, and only there",
          "[BuildingInterior][Rain][Weather][World]")
{
    // Room below, solid shell in the middle, outside air above -- a wall with
    // a door. Without the portal the crossing is fatal; with one centred on
    // the shell cell, segments through the opening pass and segments beside it
    // do not. The portal test is point-to-segment distance against a
    // TIGHTENED radius, so "near the door" is still wall.
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});

    REQUIRE(interior.ClassifySegmentModel(Vector3(0.125f, 0.875f, 0.125f),
                                          Vector3(0.125f, 0.375f, 0.125f)) == -2);
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(Vector3(0.125f, 0.875f, 0.125f),
                                                    Vector3(0.125f, 0.375f, 0.125f)));

    InteriorPortal door;
    door.centre = Vector3(0.125f, 0.625f, 0.125f);
    door.radius = 0.9f;
    door.room = 0;
    BuildingInteriorTestAccess::AddPortal(interior, door);

    // Straight through the opening: passes.
    REQUIRE(interior.SegmentPassesPortalModel(Vector3(0.125f, 0.875f, 0.125f),
                                              Vector3(0.125f, 0.375f, 0.125f)));
    // A crossing that only grazes the tightened radius: wall.
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(Vector3(0.125f + 0.85f, 0.875f, 0.125f),
                                                    Vector3(0.125f + 0.85f, 0.375f, 0.125f)));
    // Far to the side, or far above the shell: wall.
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(Vector3(5.0f, 0.875f, 5.0f),
                                                    Vector3(5.0f, 0.375f, 5.0f)));
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(Vector3(0.125f, 3.0f, 0.125f),
                                                    Vector3(0.125f, 2.0f, 0.125f)));
}

TEST_CASE("The portal filter is what decides how much slack an opening gets",
          "[BuildingInterior][Rain][Weather][World]")
{
    // Same wall-with-an-opening column, but the opening is a WINDOW and a
    // BREACH in turn -- the two kinds the voxel scan infers rather than reads
    // from authored data. Rain's default filter refuses both, and the radius
    // scale is what separates "in through the door" from "in through the wall
    // beside it": at 0.5 a crossing 0.6 radii out is wall, at 1.0 it is not.
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});

    InteriorPortal opening;
    opening.centre = Vector3(0.125f, 0.625f, 0.125f);
    opening.radius = 1.0f;
    opening.room = 0;
    opening.kind = PortalWindow;
    BuildingInteriorTestAccess::AddPortal(interior, opening);

    const Vector3 through(0.125f, 0.875f, 0.125f);
    const Vector3 into(0.125f, 0.375f, 0.125f);

    // Default filter: authored doors only, so an inferred window forgives nothing.
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(through, into));

    InteriorPortalFilter windows;
    windows.windows = true;
    REQUIRE(interior.SegmentPassesPortalModel(through, into, windows));

    // A breach is a separate opt-in from a window.
    InteriorPortalFilter breaches;
    breaches.breaches = true;
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(through, into, breaches));

    // The radius scale, on a crossing 0.6 radii to the side of the centre.
    const Vector3 besideTop(0.125f + 0.6f, 0.875f, 0.125f);
    const Vector3 besideBottom(0.125f + 0.6f, 0.375f, 0.125f);
    InteriorPortalFilter loose;
    loose.windows = true;
    loose.radiusScale = 1.0f;
    REQUIRE(interior.SegmentPassesPortalModel(besideTop, besideBottom, loose));
    InteriorPortalFilter tight;
    tight.windows = true;
    tight.radiusScale = 0.5f;
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(besideTop, besideBottom, tight));

    // Scale 0 is "no opening is ever wide enough", which is what the Weather
    // tab's strict switch means -- and it must not divide by anything.
    InteriorPortalFilter shut;
    shut.windows = true;
    shut.radiusScale = 0.0f;
    REQUIRE_FALSE(interior.SegmentPassesPortalModel(through, into, shut));
}

TEST_CASE("Building interior movement cannot turn a solid wall into an implicit portal",
          "[BuildingInterior][Smoke][World]")
{
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF});
    const InteriorStepResult step = interior.ContainedStepModel(Vector3(0.125f, 0.125f, 0.125f),
                                                                Vector3(0.125f, 0.875f, 0.125f), 0);

    REQUIRE(step.blocked);
    REQUIRE(step.room == 0);
    REQUIRE(step.position.Y() < 0.5f);
}

TEST_CASE("Building interior clearance bounds an indoor smoke billboard",
          "[BuildingInterior][Smoke][World]")
{
    BuildingInterior interior = BuildingInteriorTestAccess::MakeRoomBox(9);

    const float centre = interior.ClearanceModel(Vector3(1.125f, 1.125f, 1.125f), 0, 5.0f);
    const float nearRoof = interior.ClearanceModel(Vector3(1.125f, 2.0f, 1.125f), 0, 5.0f);

    REQUIRE(centre > 0.85f);
    REQUIRE(centre < 1.05f);
    REQUIRE(nearRoof > 0.10f);
    REQUIRE(nearRoof < 0.30f);
}

TEST_CASE("Smoke source room remains authoritative when the destination is outside",
          "[BuildingInterior][Smoke][SmokeSourceRoom]")
{
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});
    const Vector3 source(0.125f, 0.125f, 0.125f);
    const Vector3 current(0.125f, 0.375f, 0.125f);
    const Vector3 outside(0.125f, 1.125f, 0.125f);
    REQUIRE(interior.ClassifyModel(outside) == -1);
    const auto actual = interior.ContainedStepModel(source, current, 0);
    REQUIRE_FALSE(actual.blocked);
    REQUIRE(actual.room == 0);
    REQUIRE(actual.position.Y() == Catch::Approx(current.Y()));
    const auto crossing = interior.ContainedStepModel(source, outside, 0);
    REQUIRE(crossing.blocked);
    REQUIRE(crossing.room == 0);
    REQUIRE(crossing.position.Y() < 0.5f);
}

TEST_CASE("Particle rain follows wind speed and direction and relaxes after a gust change",
          "[Rain][Weather][Wind]")
{
    const Vector3 east = RainTargetVelocity(10.0f, 8.0f, 0.0f, 0.75f);
    const Vector3 north = RainTargetVelocity(10.0f, 0.0f, 12.0f, 0.75f);

    REQUIRE(east.X() == Catch::Approx(6.0f));
    REQUIRE(east.Y() == Catch::Approx(-10.0f));
    REQUIRE(east.Z() == Catch::Approx(0.0f));
    REQUIRE(north.X() == Catch::Approx(0.0f));
    REQUIRE(north.Z() == Catch::Approx(9.0f));

    const Vector3 afterOneTimeConstant = RainRelaxVelocity(Vector3(6.0f, -10.0f, 0.0f), north, 5.0f, 0.2f);
    REQUIRE(afterOneTimeConstant.X() < 3.0f);
    REQUIRE(afterOneTimeConstant.Z() > 5.0f);
    REQUIRE(afterOneTimeConstant.Y() == Catch::Approx(-10.0f));

    const Vector3 windIgnored = RainTargetVelocity(10.0f, 8.0f, 12.0f, 0.0f);
    REQUIRE(windIgnored.X() == Catch::Approx(0.0f));
    REQUIRE(windIgnored.Z() == Catch::Approx(0.0f));
}

TEST_CASE("The rain shelter probe reaches at least as far as the drop will fall before its next probe",
          "[Rain][Weather][World]")
{
    // The bug this replaces: the probe asked "is a roof already above me",
    // which can only be true once the drop is through. At the shipping 0.4 s
    // cadence and 9 m/s that is 3.6 m of rain inside the room before anything
    // kills it. The probe now covers the drop's own next interval, so a roof
    // anywhere in it is seen while the drop is still outside.
    const Vector3 position(10.0f, 20.0f, -5.0f);
    const Vector3 velocity(1.5f, -9.0f, 0.0f);

    const Vector3 exact = RainProbeTarget(position, velocity, 0.4f, 1.0f);
    REQUIRE(position.Y() - exact.Y() == Catch::Approx(3.6f));
    REQUIRE(exact.X() == Catch::Approx(10.6f));

    // Slack is slack: it may only ever lengthen the probe.
    const Vector3 slack = RainProbeTarget(position, velocity, 0.4f, 1.35f);
    REQUIRE(slack.Y() < exact.Y());

    // A lookahead below 1 would leave a gap no probe ever covers -- exactly the
    // hole the old upward probe was. Clamped, not honoured.
    const Vector3 clamped = RainProbeTarget(position, velocity, 0.4f, 0.25f);
    REQUIRE(clamped.Y() == Catch::Approx(exact.Y()));
    // A degenerate interval degenerates to the drop's own position, not behind it.
    REQUIRE(RainProbeTarget(position, velocity, -1.0f, 2.0f).Y() == Catch::Approx(position.Y()));
}

TEST_CASE("Snowflakes retain projected area when looking along their motion", "[Rain][Snow][Weather]")
{
    CHECK(SnowflakeAlpha(0, 0) == 1.0f);
    CHECK(SnowflakeAlpha(1, 0) == 0.0f);
    CHECK(SnowflakeAlpha(1, 1) == 0.0f);
    CHECK(SnowflakeAlpha(0.9f, 0) > 0.0f);
    CHECK(SnowflakeAlpha(0.9f, 0) < 1.0f);
    CHECK(SnowflakeAlpha(0.52f, 0.30f) < SnowflakeAlpha(0.6f, 0));
    CHECK(SnowParticleInViewVolume(Vector3(1,2,3), VZero, 12, 8));
    CHECK_FALSE(SnowParticleInViewVolume(Vector3(1,2,3), Vector3(1000,2,3), 12, 8));
    CHECK_FALSE(SnowParticleInViewVolume(Vector3(1,2,3), Vector3(1,100,3), 12, 8));
    CHECK_FALSE(SnowParticleInViewVolume(Vector3(13,0,0), VZero, 12, 8));
    CHECK(PrecipitationSpriteLength(true, 0.0f, 3.0f) == 3.0f);
    CHECK(PrecipitationSpriteLength(true, 0.2f, 3.0f) == 3.0f);
    CHECK(PrecipitationSpriteLength(true, 5.0f, 3.0f) == 3.0f);
    CHECK(PrecipitationSpriteLength(false, 5.0f, 3.0f) == 5.0f);
    CHECK(PrecipitationSpriteLength(false, 0.0f, 3.0f) == 0.0f);
}

TEST_CASE("Snow shelter columns include roofs without requiring path-defined rooms", "[Snow][BuildingInterior]")
{
    const auto roof = BuildingInteriorTestAccess::MakeColumn({0xff, 0xff, 0xfe, 0xff});
    CHECK(roof.CoveredFromAboveModel(Vector3(0.125f, -3, 0.125f)));
    CHECK(roof.CoveredFromAboveModel(Vector3(0.125f, 0.25f, 0.125f)));
    CHECK_FALSE(roof.CoveredFromAboveModel(Vector3(0.125f, 0.9f, 0.125f)));
    CHECK_FALSE(roof.CoveredFromAboveModel(Vector3(0.3f, 0.25f, 0.125f)));
    const auto air = BuildingInteriorTestAccess::MakeColumn({0xff, 0xff, 0xff});
    CHECK_FALSE(air.CoveredFromAboveModel(Vector3(0.125f, 0.25f, 0.125f)));
}

TEST_CASE("The rain density the sea is rained on is the one the drops are falling at",
          "[Rain][Weather][World]")
{
    // WaterWgpu feeds this to the water interaction pass. It used to read
    // Landscape's own density, which is a slow random walk capped by overcast
    // and sits far below whatever the particle layer was pinned to -- so the
    // sea got a tenth of the rain that was visibly falling into it.
    RainSystem rain;
    RainParams params = rain.Params();
    params.densityOverride = 0.75f;
    rain.SetParams(params);

    rain.SetMode(RainParticle);
    REQUIRE(rain.EffectiveDensity() == Catch::Approx(0.75f));
    rain.SetMode(RainBoth);
    REQUIRE(rain.EffectiveDensity() == Catch::Approx(0.75f));

    // No particle layer, no particle density: the sea falls back to the
    // weather, which with no landscape loaded is no rain at all.
    rain.SetMode(RainLegacy);
    REQUIRE(rain.EffectiveDensity() == Catch::Approx(0.0f));
    rain.SetMode(RainOff);
    REQUIRE(rain.EffectiveDensity() == Catch::Approx(0.0f));

    // A negative override means "follow the weather", not "rain backwards".
    params.densityOverride = -1.0f;
    rain.SetParams(params);
    rain.SetMode(RainParticle);
    REQUIRE(rain.EffectiveDensity() == Catch::Approx(0.0f));
}
