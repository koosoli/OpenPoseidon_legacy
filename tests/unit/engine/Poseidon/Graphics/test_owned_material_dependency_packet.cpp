#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/OwnedMaterialDependencyPacket.hpp>

#include <memory>
#include <string_view>

namespace
{
namespace Material = Poseidon::Asset::Material;
using Declarations = Poseidon::render::RetainedMaterialRoleDeclarations;
struct FakeTexture { int incarnation = 0; };
struct FakeLease { int mount = 0; };
using Packet = Poseidon::render::OwnedMaterialDependencyPacket<std::shared_ptr<FakeTexture>, FakeLease>;

Packet::Member Member(uint64_t offset = 8)
{
    Packet::Member m;
    m.volume = 7; m.archiveBytes = 4096; m.offset = offset; m.bytes = 128; m.fileId[0] = 0x42;
    return m;
}
Material::TranslatedSlot Stage(Material::MaterialSlot slot, const char* name)
{
    Material::TranslatedSlot out;
    out.slot = slot; out.present = true; out.texture = Material::RvTextureRef::Parse(name);
    return out;
}
Declarations PhysicalDeclarations()
{
    Material::TranslatedMaterial material;
    material.shaderFamily = "Super";
    material.slots.push_back(Stage(Material::MaterialSlot::BaseColour, R"(dz\wall_co.paa)"));
    material.slots.push_back(Stage(Material::MaterialSlot::NormalMap, R"(dz\wall_co.paa)"));
    return Declarations::Build(&material, {true, false, R"(dz\wall_co.paa)", R"(dz\wall_co.paa)"}, {});
}
Declarations GeneratedDeclarations()
{
    Material::TranslatedMaterial material;
    material.shaderFamily = "Super";
    material.slots.push_back(Stage(Material::MaterialSlot::NormalMap,
        "#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)"));
    return Declarations::Build(&material, {}, {});
}
Packet::Birth PhysicalBirth(uint64_t token, std::shared_ptr<FakeTexture> texture,
                            std::shared_ptr<const FakeLease> lease, Packet::Member member = Member())
{
    Packet::Birth out;
    out.token = token; out.texture = std::move(texture);
    out.actualSourceName = R"(dz\wall_co.paa)";
    out.physicalLease = std::move(lease); out.physicalMember = member;
    return out;
}
Packet::Birth GeneratedBirth(uint64_t token, std::shared_ptr<FakeTexture> texture, bool deferred = true)
{
    Packet::Birth out;
    out.token = token; out.texture = std::move(texture);
    out.actualSourceName = "#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)";
    out.generatedDynamic = true; out.generatedDeferred = deferred;
    return out;
}
}

TEST_CASE("Owner dependency packet deduplicates exact physical births and stages one image",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    REQUIRE(declarations.DeclarationsComplete());
    REQUIRE(declarations.Count() == 3);
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    Packet packet;
    REQUIRE(packet.BeginSection(11, declarations));
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
    REQUIRE(packet.EndSection());
    REQUIRE(packet.SealCapturedRows());
    REQUIRE(packet.RoleUsesCaptured() == 3);
    REQUIRE(packet.UniqueDependencies() == 1);
    REQUIRE_FALSE(packet.FinalRoleClosureProven());

    uint64_t handle = 0;
    uint32_t slot = 0;
    bool mounted = true;
    int uploads = 0;
    const auto inspect = [&](size_t, const auto& held, const auto& birthLease) {
        REQUIRE(held == texture);
        REQUIRE(birthLease == lease);
        Packet::Probe out;
        out.textureIdentity = held.get(); out.actualSourceName = R"(dz\wall_co.paa)";
        out.physicalMember = Member(); out.mountedCurrent = mounted;
        out.handle = handle; out.slotLease = slot;
        return out;
    };
    const auto upload = [&](size_t, const auto&) {
        ++uploads; handle = 41; slot = 7;
        return Packet::UploadWitness{handle, slot, 1, false, true};
    };
    REQUIRE(packet.AdvanceOne(1, inspect, upload));
    REQUIRE(packet.GetState() == Packet::State::DependenciesReady);
    REQUIRE(uploads == 1);
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.StagedHandle(11, i) == 41);
    REQUIRE(packet.AllStagedCurrent(inspect));
    mounted = false;
    REQUIRE_FALSE(packet.AllStagedCurrent(inspect));
}

TEST_CASE("Owner dependency packet refuses missing rows, forged births and alias conflicts",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    SECTION("incomplete section")
    {
        Packet packet;
        REQUIRE(packet.BeginSection(1, declarations));
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
        REQUIRE_FALSE(packet.EndSection());
        REQUIRE(packet.RefusalReason() == Packet::Refusal::IncompleteDeclarations);
    }
    SECTION("birth lacks an actual physical lease")
    {
        Packet packet;
        REQUIRE(packet.BeginSection(1, declarations));
        REQUIRE_FALSE(packet.CaptureNext(PhysicalBirth(1, texture, {})));
        REQUIRE(packet.RefusalReason() == Packet::Refusal::InvalidBirth);
    }
    SECTION("same token names a different owner texture")
    {
        Packet packet;
        REQUIRE(packet.BeginSection(1, declarations));
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
        REQUIRE_FALSE(packet.CaptureNext(PhysicalBirth(1, std::make_shared<FakeTexture>(), lease)));
        REQUIRE(packet.RefusalReason() == Packet::Refusal::ConflictingAlias);
    }
    SECTION("same token changes archive member")
    {
        Packet packet;
        REQUIRE(packet.BeginSection(1, declarations));
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
        REQUIRE_FALSE(packet.CaptureNext(PhysicalBirth(1, texture, lease, Member(100))));
        REQUIRE(packet.RefusalReason() == Packet::Refusal::ConflictingAlias);
    }
}

TEST_CASE("Owner dependency packet refuses stale physical mount before upload",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    Packet packet;
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    REQUIRE(packet.BeginSection(1, declarations));
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
    REQUIRE(packet.EndSection()); REQUIRE(packet.SealCapturedRows());
    int uploads = 0;
    const auto stale = [](size_t, const auto& held, const auto&) {
        Packet::Probe out;
        out.textureIdentity = held.get(); out.actualSourceName = R"(dz\wall_co.paa)";
        out.physicalMember = Member(); out.mountedCurrent = false;
        return out;
    };
    REQUIRE_FALSE(packet.AdvanceOne(1, stale, [&](size_t, const auto&) {
        ++uploads; return Packet::UploadWitness{};
    }));
    REQUIRE(uploads == 0);
    REQUIRE(packet.RefusalReason() == Packet::Refusal::StaleSource);
}

TEST_CASE("Owner dependency packet refuses member replacement during its upload",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    Packet packet;
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    REQUIRE(packet.BeginSection(1, declarations));
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
    REQUIRE(packet.EndSection()); REQUIRE(packet.SealCapturedRows());
    Packet::Member current = Member();
    uint64_t handle = 0;
    const auto inspect = [&](size_t, const auto& held, const auto&) {
        Packet::Probe out;
        out.textureIdentity = held.get(); out.actualSourceName = R"(dz\wall_co.paa)";
        out.physicalMember = current; out.mountedCurrent = true;
        out.handle = handle; out.slotLease = handle ? 3 : 0;
        return out;
    };
    REQUIRE_FALSE(packet.AdvanceOne(1, inspect, [&](size_t, const auto&) {
        handle = 51; current = Member(100); // remounted different member before post-check
        return Packet::UploadWitness{51, 3, 1, false, true};
    }));
    REQUIRE(packet.RefusalReason() == Packet::Refusal::StaleSource);
    REQUIRE(packet.DependenciesStaged() == 0);
    REQUIRE_FALSE(packet.StagedHandle(1, 0));
}

TEST_CASE("Owner dependency packet requires generated owner-stage witness and exact birth",
          "[owned-material-dependencies]")
{
    auto declarations = GeneratedDeclarations();
    REQUIRE(declarations.DeclarationsComplete());
    REQUIRE(declarations.Count() == 1);
    auto texture = std::make_shared<FakeTexture>();
    Packet packet;
    REQUIRE(packet.BeginSection(2, declarations));
    REQUIRE(packet.CaptureNext(GeneratedBirth(9, texture)));
    REQUIRE(packet.EndSection()); REQUIRE(packet.SealCapturedRows());
    uint64_t handle = 0;
    bool pending = true;
    const auto inspect = [&](size_t, const auto& held, const auto&) {
        Packet::Probe out;
        out.textureIdentity = held.get(); out.actualSourceName = declarations.At(0).name.data();
        out.handle = handle; out.generatedDynamic = true; out.generatedDeferred = pending;
        return out;
    };
    REQUIRE_FALSE(packet.AdvanceOne(1, inspect, [&](size_t, const auto&) {
        handle = 77; pending = false;
        return Packet::UploadWitness{77, 0, 1, false, true};
    }));
    REQUIRE(packet.RefusalReason() == Packet::Refusal::UploadWitness);

    Packet escaped;
    REQUIRE(escaped.BeginSection(3, declarations));
    REQUIRE(escaped.CaptureNext(GeneratedBirth(10, texture)));
    REQUIRE(escaped.EndSection()); REQUIRE(escaped.SealCapturedRows());
    REQUIRE_FALSE(escaped.AdvanceOne(1, inspect, [](size_t, const auto&) { return Packet::UploadWitness{}; }));
    REQUIRE(escaped.RefusalReason() == Packet::Refusal::EscapedGenerated);

    handle = 0; pending = true;
    Packet staged;
    REQUIRE(staged.BeginSection(4, declarations));
    REQUIRE(staged.CaptureNext(GeneratedBirth(11, texture)));
    REQUIRE(staged.EndSection()); REQUIRE(staged.SealCapturedRows());
    REQUIRE(staged.AdvanceOne(1, inspect, [&](size_t, const auto&) {
        handle = 99; pending = false;
        return Packet::UploadWitness{99, 0, 1, true, true};
    }));
    REQUIRE(staged.StagedHandle(4, 0) == 99);
    REQUIRE(staged.AllStagedCurrent(inspect));
    REQUIRE_FALSE(staged.FinalRoleClosureProven());
}

TEST_CASE("Owner dependency packet keeps refs and leases until explicit cancel",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    Packet packet;
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    std::weak_ptr<FakeTexture> weakTexture = texture;
    std::weak_ptr<const FakeLease> weakLease = lease;
    REQUIRE(packet.BeginSection(1, declarations));
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.CaptureNext(PhysicalBirth(1, texture, lease)));
    REQUIRE(packet.EndSection()); REQUIRE(packet.SealCapturedRows());
    texture.reset(); lease.reset();
    REQUIRE_FALSE(weakTexture.expired());
    REQUIRE_FALSE(weakLease.expired());
    packet.Cancel();
    REQUIRE(weakTexture.expired());
    REQUIRE(weakLease.expired());
    REQUIRE(packet.GetState() == Packet::State::Cancelled);
}

TEST_CASE("Owner dependency packet reuses resident image without an upload callback",
          "[owned-material-dependencies]")
{
    auto declarations = PhysicalDeclarations();
    auto texture = std::make_shared<FakeTexture>();
    auto lease = std::make_shared<const FakeLease>();
    Packet packet;
    REQUIRE(packet.BeginSection(8, declarations));
    for (size_t i = 0; i < declarations.Count(); ++i)
        REQUIRE(packet.CaptureNext(PhysicalBirth(18, texture, lease)));
    REQUIRE(packet.EndSection()); REQUIRE(packet.SealCapturedRows());
    int uploads = 0;
    const auto resident = [](size_t, const auto& held, const auto&) {
        Packet::Probe out;
        out.textureIdentity = held.get(); out.actualSourceName = R"(dz\wall_co.paa)";
        out.physicalMember = Member(); out.mountedCurrent = true;
        out.handle = 44; out.slotLease = 12;
        return out;
    };
    REQUIRE(packet.AdvanceOne(5, resident, [&](size_t, const auto&) {
        ++uploads; return Packet::UploadWitness{};
    }));
    REQUIRE(uploads == 0);
    REQUIRE(packet.StagedHandle(8, 2) == 44);
    REQUIRE(packet.AllStagedCurrent(resident));
}

TEST_CASE("Owner dependency packet bounds captured sections without claiming closure",
          "[owned-material-dependencies]")
{
    const auto empty = Declarations::Build(nullptr, {}, {});
    REQUIRE(empty.DeclarationsComplete());
    REQUIRE(empty.Count() == 0);
    Packet packet;
    for (size_t i = 1; i <= Packet::MaxSections; ++i)
    {
        REQUIRE(packet.BeginSection(i, empty));
        REQUIRE(packet.EndSection());
    }
    REQUIRE_FALSE(packet.BeginSection(Packet::MaxSections + 1, empty));
    REQUIRE(packet.RefusalReason() == Packet::Refusal::Capacity);
    REQUIRE_FALSE(packet.FinalRoleClosureProven());
}
