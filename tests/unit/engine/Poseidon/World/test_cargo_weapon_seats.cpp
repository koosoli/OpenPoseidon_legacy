#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Entities/Vehicles/CargoWeaponSeats.hpp>
#include <Poseidon/World/Scene/CargoSurface.hpp>
#include <Poseidon/World/Scene/CargoInsertionCollision.hpp>
#include <Poseidon/World/Terrain/SimulationFireCollision.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <limits>

using namespace Poseidon;

TEST_CASE("AI cargo insertion enhances only Ready coverage and retains every legacy refusal path", "[cargo-weapons][cargo-streaming]")
{
    using Status = Streaming::SimulationResidencyStatus;
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> carrier = new ObjectPlain(shape, 10);
    const Status states[] = {Status::Disabled, Status::Pending, Status::Ready, Status::Unknown,
        Status::Invalid, Status::CapacityExceeded, Status::WrongOwner};
    for (const auto queued : states) for (const auto traced : states)
    {
        CollisionBuffer hits;
        size_t queues = 0, modern = 0, legacy = 0;
        TraceCargoInsertion(true, hits,
            [&] { ++queues; return queued; },
            [&](CollisionBuffer& result) {
                ++modern;
                if (traced == Status::Ready) { CollisionInfo hit{}; hit.object = carrier.GetRef(); hit.component = 20; result.Add(hit); }
                return traced;
            },
            [&](CollisionBuffer& result) {
                ++legacy; CollisionInfo hit{}; hit.object = carrier.GetRef(); hit.component = 10; result.Add(hit);
            });
        const bool tried = queued == Status::Ready || queued == Status::Pending;
        const bool enhanced = tried && traced == Status::Ready;
        REQUIRE(queues == 1); REQUIRE(modern == size_t(tried)); REQUIRE(legacy == size_t(!enhanced));
        REQUIRE(hits.Size() == 1); REQUIRE(hits[0].object.GetRef() == carrier.GetRef());
        REQUIRE(hits[0].component == (enhanced ? 20 : 10));
    }
    // This same disabled dispatcher arm is used for OFF, unusual off-owner
    // callers and read-only clearance probes. None may create/renew interest.
    CollisionBuffer legacyOnly; size_t legacyCalls = 0;
    TraceCargoInsertion(false, legacyOnly,
        [&] { FAIL("Disabled/off-owner/probe acquired Fire interest"); return Status::Ready; },
        [&](CollisionBuffer&) { FAIL("Disabled/off-owner/probe performed modern trace"); return Status::Ready; },
        [&](CollisionBuffer& result) { ++legacyCalls; CollisionInfo hit{}; hit.object = carrier.GetRef(); result.Add(hit); });
    REQUIRE(legacyCalls == 1); REQUIRE(legacyOnly.Size() == 1);
}

TEST_CASE("Ready cargo insertion merges actual contact buffers while retaining shooter and carrier semantics", "[cargo-weapons][cargo-streaming]")
{
    using Status = Streaming::SimulationResidencyStatus;
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> shooter = new ObjectPlain(shape, 10), carrier = new ObjectPlain(shape, 11), wall = new ObjectPlain(shape, 12);
    shooter->SetType(Primary); carrier->SetType(Primary); wall->SetType(Primary);
    Object* candidates[] = {shooter.GetRef(), carrier.GetRef(), wall.GetRef(), wall.GetRef()};
    CollisionBuffer hits; size_t queued = 0, outerLegacy = 0, terrain = 0;
    TraceCargoInsertion(true, hits, [&] { ++queued; return Status::Pending; },
        [&](CollisionBuffer& result) {
            return Streaming::AppendSimulationFireContacts(result, std::span<Object* const>(candidates, 4), shooter.GetRef(), nullptr,
                [&](CollisionBuffer& staged) {
                    CollisionInfo self{}; self.object = shooter.GetRef(); staged.Add(self);
                    CollisionInfo vehicle{}; vehicle.object = carrier.GetRef(); staged.Add(vehicle);
                },
                [&](CollisionBuffer& staged, Object& object) { ++terrain; CollisionInfo obstruction{}; obstruction.object = &object; staged.Add(obstruction); },
                [] { return Status::Ready; }, [] { return true; });
        }, [&](CollisionBuffer&) { ++outerLegacy; });
    REQUIRE(queued == 1); REQUIRE(outerLegacy == 0); REQUIRE(terrain == 1);
    REQUIRE(hits.Size() == 2);
    REQUIRE(hits[0].object.GetRef() == carrier.GetRef());
    REQUIRE(hits[1].object.GetRef() == wall.GetRef());
    // A carrier hit remains an insertion obstruction; it is never the ignored
    // object. The same supplementary wall appears once, not once per candidate.
}

TEST_CASE("Cargo insertion departure releases only owner interest while valid blocked attempts retain it", "[cargo-weapons][cargo-streaming]")
{
    size_t releases = 0;
    ReleaseCargoInsertionInterest(false, false, [&] { ++releases; });
    REQUIRE(releases == 0); // OFF/off-owner/read-only probe
    ReleaseCargoInsertionInterest(true, true, [&] { ++releases; });
    REQUIRE(releases == 0); // Valid attempted insertion remains interested, even if blocked.
    ReleaseCargoInsertionInterest(true, false, [&] { ++releases; });
    REQUIRE(releases == 1); // Invalid weapon/seat, noncargo or player transition.
}

TEST_CASE("Cargo shell ray hits either face without filling cabin space", "[cargo-weapons]")
{
    const Vector3 a(-1,-1,0), b(1,-1,0), c(0,1,0);
    float t = -1;
    REQUIRE(CargoTriangleHit(Vector3(0,0,-1), Vector3(0,0,1), a,b,c,t));
    REQUIRE(t == .5f);
    REQUIRE(CargoTriangleHit(Vector3(0,0,1), Vector3(0,0,-1), a,b,c,t));
    REQUIRE(t == .5f);
    REQUIRE_FALSE(CargoTriangleHit(Vector3(0,0,1), Vector3(0,0,2), a,b,c,t));
    REQUIRE_FALSE(CargoTriangleHit(Vector3(2,0,-1), Vector3(2,0,1), a,b,c,t));
    REQUIRE_FALSE(CargoTriangleHit(Vector3(0,0,1), Vector3(0,0,1), a,b,c,t));
    REQUIRE_FALSE(CargoTriangleHit(Vector3(0,0,-1), Vector3(0,0,1), a,a,a,t));
}

TEST_CASE("Player cargo free aim does not change NPC arc policy", "[cargo-weapons]")
{
    CargoWeaponSeat npc;
    npc.cargoIndex = 0;
    npc.halfAngle = 25;
    npc.minElevation = -15;
    npc.maxElevation = 20;
    CargoWeaponSeat player = npc;
    player.playerFreeAim = true;
    REQUIRE_FALSE(npc.ContainsDirection(Vector3(0, 0, -1)));
    REQUIRE(player.ContainsDirection(Vector3(0, 0, -1)));
    REQUIRE(player.ContainsDirection(Vector3(1, 0, 0)));
    REQUIRE(player.ContainsDirection(Vector3(0, 1, 0)));
    REQUIRE_FALSE(player.ContainsDirection(Vector3(0, 0, 0)));
    REQUIRE_FALSE(player.ContainsDirection(Vector3(std::numeric_limits<float>::quiet_NaN(), 0, 1)));
    REQUIRE_FALSE(npc.ContainsDirection(Vector3(0, 0, -1)));
}

TEST_CASE("Cargo rifle family accepts renamed rifles but rejects other use modes", "[cargo-weapons]")
{
    CargoWeaponSeat seat;
    seat.rifleFamily = true;
    REQUIRE(seat.AcceptsWeapon("M16", true, true));
    REQUIRE(seat.AcceptsWeapon("AK74", true, true));
    REQUIRE(seat.AcceptsWeapon("UserRifle", true, true));
    REQUIRE_FALSE(seat.AcceptsWeapon("M16GrenadeLauncher", true, false));
    REQUIRE_FALSE(seat.AcceptsWeapon("Pistol", false, true));
    REQUIRE_FALSE(seat.AcceptsWeapon("Turret", false, true));
    REQUIRE_FALSE(seat.AcceptsWeapon(nullptr, true, true));
    seat.rifleFamily = false;
    seat.weapon = "M16";
    REQUIRE(seat.AcceptsWeapon("m16", true, true));
    REQUIRE_FALSE(seat.AcceptsWeapon("AK74", true, true));
    REQUIRE_FALSE(seat.AcceptsWeapon("M16", true, false));
}

namespace
{
ParamClass* AddSeat(ParamEntry& seats, const char* name, int index)
{
    auto* seat = seats.AddClass(name);
    seat->Add("cargoIndex", index);
    seat->Add("weapon", "M16");
    seat->Add("action", "OP_JeepRifle");
    seat->Add("azimuth", 90);
    seat->Add("halfAngle", 30);
    seat->Add("minElevation", -10);
    seat->Add("maxElevation", 35);
    return seat;
}
}

TEST_CASE("Cargo personal weapon table defaults to no supported seat", "[cargo-weapons]")
{
    ParamFile vehicle;
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Error().empty());
    REQUIRE(table.Find(0) == nullptr);
    REQUIRE(table.Find(-1) == nullptr);
}

TEST_CASE("Cargo personal weapon contract rejects invalid tables atomically", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 1);
    auto* seats = config->AddClass("Seats");
    auto* seat = AddSeat(*seats, "Passenger", 0);
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0) != nullptr);
    REQUIRE(table.Find(1) == nullptr);
    REQUIRE(table.Find(0)->AITrackingRate() == 0.2f);

    SECTION("unknown version") { config->Add("version", 99); }
    SECTION("invalid index") { seat->Add("cargoIndex", 3); }
    SECTION("fractional index") { seat->Delete("cargoIndex"); seat->Add("cargoIndex", 0.5f); }
    SECTION("duplicate") { AddSeat(*seats, "Duplicate", 0); }
    SECTION("empty weapon") { seat->Add("weapon", ""); }
    SECTION("missing action") { seat->Delete("action"); }
    SECTION("too wide") { seat->Add("halfAngle", 180); }
    SECTION("nonfinite") { seat->Delete("azimuth"); seat->Add("azimuth", std::numeric_limits<float>::quiet_NaN()); }
    SECTION("reversed elevation") { seat->Add("minElevation", 40); }
    SECTION("negative tracking speed") { seat->Add("aiAimSpeed", -1); }
    SECTION("excessive tracking speed") { seat->Add("aiAimSpeed", 91); }
    SECTION("nonfinite tracking speed") { seat->Add("aiAimSpeed", std::numeric_limits<float>::infinity()); }
    SECTION("text tracking speed") { seat->Add("aiAimSpeed", "fast"); }
    REQUIRE_FALSE(table.Load(vehicle, 3));
    REQUIRE_FALSE(table.Error().empty());
    REQUIRE(table.Find(0) == nullptr);
}

TEST_CASE("Cargo tracking speed is explicit content, not a global infantry change", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 1);
    auto* entry = AddSeat(*config->AddClass("Seats"), "Passenger", 0);
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0)->AITrackingRate() == 0.2f);
    entry->Add("aiAimSpeed", 45);
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(std::abs(table.Find(0)->AITrackingRate() - H_PI / 4) < 1e-6f);
    REQUIRE(table.Find(0)->halfAngle == 30);
    REQUIRE(table.Find(1) == nullptr);
    entry->Add("aiAimSpeed", 0);
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0)->AITrackingRate() == 0.2f);
}

TEST_CASE("Cargo transition contract requires complete distinct authored states", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 2);
    auto* seat = AddSeat(*config->AddClass("Seats"), "Passenger", 0);
    seat->Add("idleAction", "OP_JeepIdle");
    seat->Add("raiseAction", "OP_JeepRaise");
    seat->Add("reloadAction", "OP_JeepReload");
    seat->Add("lowerAction", "OP_JeepLower");
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0)->HasTransitions());
    REQUIRE(table.Find(0)->OwnsAction("op_jeeprifle"));
    REQUIRE(table.Find(0)->OwnsAction("OP_JeepReload"));
    REQUIRE_FALSE(table.Find(0)->OwnsAction("JeepCoDriver"));
    REQUIRE_FALSE(table.Find(0)->OwnsAction(""));
    REQUIRE_FALSE(table.Find(0)->OwnsAction(nullptr));

    SECTION("missing reload") { seat->Delete("reloadAction"); }
    SECTION("empty idle") { seat->Add("idleAction", ""); }
    SECTION("numeric transition") { seat->Delete("raiseAction"); seat->Add("raiseAction", 2); }
    SECTION("duplicate ignoring case") { seat->Add("lowerAction", "op_jeepraise"); }
    SECTION("same as aim") { seat->Add("reloadAction", "OP_JeepRifle"); }
    REQUIRE_FALSE(table.Load(vehicle, 3));
    REQUIRE_FALSE(table.Find(0));
}

TEST_CASE("Cargo version three family profiles fail closed on ambiguous selectors", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 3);
    auto* entry = AddSeat(*config->AddClass("Seats"), "Passenger", 0);
    entry->Delete("weapon");
    entry->Add("weaponFamily", "rifle");
    entry->Add("idleAction", "Idle");
    entry->Add("raiseAction", "Raise");
    entry->Add("reloadAction", "Reload");
    entry->Add("lowerAction", "Lower");
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0)->rifleFamily);
    REQUIRE(table.Find(0)->HasTransitions());
    SECTION("legacy contract cannot silently gain family permission") { config->Add("version", 2); }
    SECTION("unknown family") { entry->Add("weaponFamily", "all"); }
    SECTION("two selectors") { entry->Add("weapon", "M16"); }
    SECTION("missing selector") { entry->Delete("weaponFamily"); }
    SECTION("missing action") { entry->Delete("reloadAction"); }
    REQUIRE_FALSE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0) == nullptr);
}

TEST_CASE("Version one cargo contract does not enable transition or automatic entry", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 1);
    auto* seat = AddSeat(*config->AddClass("Seats"), "Passenger", 0);
    seat->Add("idleAction", "Unused");
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE_FALSE(table.Find(0)->HasTransitions());
    REQUIRE(table.Find(0)->OwnsAction("OP_JeepRifle"));
    REQUIRE_FALSE(table.Find(0)->OwnsAction("Unused"));
}

TEST_CASE("Cargo seat arc excludes windshield roof and opposite side", "[cargo-weapons]")
{
    ParamFile vehicle;
    auto* config = vehicle.AddClass("OPCargoWeapons");
    config->Add("version", 1);
    auto* seats = config->AddClass("Seats");
    AddSeat(*seats, "Passenger", 0);
    CargoWeaponSeats table;
    REQUIRE(table.Load(vehicle, 3));
    const auto* seat = table.Find(0);
    REQUIRE(seat);
    REQUIRE(seat->ContainsDirection(Vector3(1, 0, 0)));
    REQUIRE(seat->ContainsDirection(Vector3(100, 10, 0)));
    REQUIRE_FALSE(seat->ContainsDirection(Vector3(0, 0, 1)));
    REQUIRE_FALSE(seat->ContainsDirection(Vector3(-1, 0, 0)));
    REQUIRE_FALSE(seat->ContainsDirection(Vector3(1, 1, 0)));
    REQUIRE_FALSE(seat->ContainsDirection(Vector3(1, -1, 0)));
    REQUIRE_FALSE(seat->ContainsDirection(VZero));
    REQUIRE_FALSE(seat->ContainsDirection(VUp));
    REQUIRE_FALSE(seat->ContainsDirection(Vector3(std::numeric_limits<float>::infinity(), 0, 0)));
    REQUIRE(table.Load(vehicle, 3));
    vehicle.Delete("OPCargoWeapons");
    REQUIRE(table.Load(vehicle, 3));
    REQUIRE(table.Find(0) == nullptr);
}

TEST_CASE("Protected original vehicle uses exact external content mapping", "[cargo-weapons]")
{
    ParamFile root;
    auto* vehicles = root.AddClass("CfgVehicles");
    auto* jeep = vehicles->AddClass("Jeep");
    auto* variant = vehicles->AddClass("JeepMG");
    auto* mappings = root.AddClass("CfgOPVehicleActions");
    auto* mappedJeep = mappings->AddClass("Jeep");
    auto* config = mappedJeep->AddClass("OPCargoWeapons");
    config->Add("version", 1);
    auto* seats = config->AddClass("Seats");
    AddSeat(*seats, "Passenger", 0);
    jeep->SetAccessMode(PAReadOnlyVerified);
    CargoWeaponSeats table;
    REQUIRE(table.Load(*jeep, 3, &root));
    REQUIRE(table.Find(0));
    REQUIRE(jeep->GetAccessMode() == PAReadOnlyVerified);
    REQUIRE(jeep->FindEntry("OPCargoWeapons") == nullptr);
    REQUIRE(table.Load(*variant, 3, &root));
    REQUIRE_FALSE(table.Find(0));
    REQUIRE(table.Load(*jeep, 3));
    REQUIRE_FALSE(table.Find(0));
    config->Add("version", 99);
    REQUIRE_FALSE(table.Load(*jeep, 3, &root));
    REQUIRE_FALSE(table.Find(0));
}

TEST_CASE("Vehicle-owned cargo table overrides external default mapping without fallback", "[cargo-weapons]")
{
    ParamFile root;
    auto* vehicle = root.AddClass("Jeep");
    auto* mappings = root.AddClass("CfgOPVehicleActions");
    auto* mapped = mappings->AddClass("Jeep")->AddClass("OPCargoWeapons");
    mapped->Add("version", 1);
    AddSeat(*mapped->AddClass("Seats"), "Passenger", 0);
    auto* direct = vehicle->AddClass("OPCargoWeapons");
    direct->Add("version", 1);
    direct->AddClass("Seats"); // Explicit empty override disables the default.
    CargoWeaponSeats table;
    REQUIRE(table.Load(*vehicle, 3, &root));
    REQUIRE_FALSE(table.Find(0));
    direct->Add("version", 99);
    REQUIRE_FALSE(table.Load(*vehicle, 3, &root));
    REQUIRE_FALSE(table.Find(0));
    vehicle->Delete("OPCargoWeapons");
    REQUIRE(table.Load(*vehicle, 3, &root));
    REQUIRE(table.Find(0));
}
