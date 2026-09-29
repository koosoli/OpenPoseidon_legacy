#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ResumableModelAdmission.hpp>
#include <array>

using Poseidon::render::ResumableModelAdmission;
namespace {
using Admission = ResumableModelAdmission;
Admission::Member Member(uint64_t offset = 8)
{
    Admission::Member out;
    out.volume = 7; out.archiveBytes = 4096; out.offset = offset; out.bytes = 128;
    out.fileId[0] = 0x42;
    return out;
}
Admission::TextureProof Proof(uint64_t token = 1, uint64_t offset = 8)
{
    return {token, "dz\\structures\\data\\wall_co.paa", Member(offset), 0, 0, true, true};
}
Admission::Limits Limits(size_t textures = 1, size_t meshes = 1, size_t models = 1,
                         uint64_t geometry = 1024, bool complete = true)
{ return {textures, meshes, models, geometry, complete}; }
bool Plan(Admission& a, Admission::Limits limits = Limits())
{
    if (!a.Begin(limits)) return false;
    for (size_t i = 0; i < limits.textures; ++i)
        if (!a.AddTexture(Proof(i + 1, 8 + i * 128))) return false;
    for (size_t i = 0; i < limits.meshes; ++i)
        if (!a.AddMesh(100 + i, limits.duplicateGeometryBytes / limits.meshes)) return false;
    for (size_t i = 0; i < limits.models; ++i)
        if (!a.AddModel(200 + i)) return false;
    return a.Seal();
}
}

TEST_CASE("Resumable admission requires complete bounded final-role and duplicate-geometry plans",
          "[resumable-model-admission]")
{
    Admission incomplete;
    REQUIRE_FALSE(incomplete.Begin(Limits(1, 1, 1, 1024, false)));
    REQUIRE(incomplete.RefusalReason() == Admission::Refusal::IncompleteResolver);
    Admission excessiveTextures;
    REQUIRE_FALSE(excessiveTextures.Begin(Limits(Admission::MaxTextures + 1)));
    REQUIRE(excessiveTextures.RefusalReason() == Admission::Refusal::Capacity);
    Admission excessiveGeometry;
    REQUIRE_FALSE(excessiveGeometry.Begin(Limits(1, 1, 1, Admission::MaxDuplicateGeometryBytes + 1)));
    Admission truncated;
    REQUIRE(truncated.Begin(Limits(2, 1, 1)));
    REQUIRE(truncated.AddTexture(Proof()));
    REQUIRE(truncated.AddMesh(100, 1024));
    REQUIRE(truncated.AddModel(200));
    REQUIRE_FALSE(truncated.Seal());
    REQUIRE(truncated.RefusalReason() == Admission::Refusal::IncompleteResolver);
    Admission unsupported;
    REQUIRE(unsupported.Begin(Limits()));
    auto unknown = Proof(); unknown.birthLeaseHeld = false;
    REQUIRE_FALSE(unsupported.AddTexture(unknown));
    REQUIRE(unsupported.RefusalReason() == Admission::Refusal::InvalidSource);
}

TEST_CASE("Resumable admission spends one measured texture and mesh per update and waits for every model ack",
          "[resumable-model-admission]")
{
    Admission a;
    REQUIRE(Plan(a, Limits(2, 2, 2, 2048)));
    std::array<Admission::TextureProof, 2> current{Proof(1, 8), Proof(2, 136)};
    auto inspect = [&](size_t i) { return current[i]; };
    unsigned uploads = 0, meshes = 0, models = 0;
    auto upload = [&](size_t i) {
        ++uploads; current[i].handle = 1000 + i; current[i].slotLease = 10 + unsigned(i);
        return Admission::UploadResult{current[i].handle, current[i].slotLease, 1, true};
    };
    auto mesh = [&](size_t i, uint64_t producer, uint64_t plannedBytes) {
        ++meshes; return Admission::MeshResult{plannedBytes, producer == 100 + i};
    };
    REQUIRE(a.Advance(1, inspect, upload, mesh));
    REQUIRE(uploads == 1); REQUIRE(meshes == 1);
    REQUIRE(a.GetState() == Admission::State::Staging);
    REQUIRE_FALSE(a.PublishIfCurrent(inspect, true));
    a.Cancel();
    Admission::Debt prematureDebt{};
    REQUIRE(a.NextDebt(prematureDebt));
    REQUIRE(a.MarkDebtQueued(prematureDebt));
    REQUIRE(a.SettleDebt(prematureDebt));
    // A premature publication refuses closed; start an independent complete run.
    Admission b; REQUIRE(Plan(b, Limits(2, 2, 2, 2048)));
    current = {Proof(1, 8), Proof(2, 136)}; uploads = meshes = models = 0;
    REQUIRE(b.Advance(1, inspect, upload, mesh));
    REQUIRE(b.Advance(2, inspect, upload, mesh));
    REQUIRE(uploads == 2); REQUIRE(meshes == 2);
    REQUIRE(b.GetState() == Admission::State::ReadyToRegister);
    REQUIRE(b.QueueOneModel(3, inspect, [&](size_t i, uint64_t producer) {
        ++models; return i == 0 && producer == 200;
    }));
    REQUIRE_FALSE(b.GetState() == Admission::State::ReadyToPublish);
    REQUIRE(b.AcknowledgeModel(200, true, true));
    REQUIRE(b.GetState() == Admission::State::AwaitingAck);
    REQUIRE(b.QueueOneModel(4, inspect, [&](size_t i, uint64_t producer) {
        ++models; return i == 1 && producer == 201;
    }));
    REQUIRE(models == 2);
    REQUIRE_FALSE(b.GetState() == Admission::State::ReadyToPublish);
    REQUIRE(b.AcknowledgeModel(201, true, true));
    REQUIRE(b.GetState() == Admission::State::ReadyToPublish);
    REQUIRE(b.PublishIfCurrent(inspect, true));
    REQUIRE(b.GetState() == Admission::State::Published);
}

TEST_CASE("Resumable admission refuses mount or slot changes and never calls a stale upload",
          "[resumable-model-admission]")
{
    Admission changedMount; REQUIRE(Plan(changedMount));
    auto current = Proof(); current.member = Member(9);
    unsigned uploads = 0, meshes = 0;
    auto inspect = [&](size_t) { return current; };
    auto upload = [&](size_t) { ++uploads; return Admission::UploadResult{1, 1, 1, true}; };
    auto mesh = [&](size_t, uint64_t, uint64_t plannedBytes) {
        ++meshes; return Admission::MeshResult{plannedBytes, true};
    };
    REQUIRE_FALSE(changedMount.Advance(1, inspect, upload, mesh));
    REQUIRE(changedMount.RefusalReason() == Admission::Refusal::StaleSource);
    REQUIRE(uploads == 0); REQUIRE(meshes == 0);

    Admission changedLease; REQUIRE(Plan(changedLease)); current = Proof();
    auto successfulUpload = [&](size_t) {
        current.handle = 17; current.slotLease = 3;
        return Admission::UploadResult{17, 3, 1, true};
    };
    REQUIRE(changedLease.Advance(1, inspect, successfulUpload, mesh));
    current.slotLease = 4; // TrimResidency/reupload before ModelRegister
    REQUIRE_FALSE(changedLease.QueueOneModel(2, inspect, [](size_t, uint64_t) { return true; }));
    REQUIRE(changedLease.RefusalReason() == Admission::Refusal::StaleSource);
    changedLease.Cancel();
    Admission::Debt debt{};
    REQUIRE(changedLease.NextDebt(debt));
    REQUIRE(debt.kind == Admission::DebtKind::MeshDestroy);
    REQUIRE(debt.producer == 100);
}

TEST_CASE("Resumable admission retains uncertain queued debt until ordered renderer settlement",
          "[resumable-model-admission]")
{
    Admission a; REQUIRE(Plan(a));
    auto current = Proof();
    auto inspect = [&](size_t) { return current; };
    REQUIRE(a.Advance(1, inspect, [&](size_t) {
        current.handle = 40; current.slotLease = 5;
        return Admission::UploadResult{40, 5, 1, true};
    }, [](size_t, uint64_t, uint64_t plannedBytes) {
        return Admission::MeshResult{plannedBytes, true};
    }));
    REQUIRE(a.QueueOneModel(2, inspect, [](size_t, uint64_t) { return true; }));
    REQUIRE_FALSE(a.AcknowledgeModel(200, false, true));
    REQUIRE(a.RefusalReason() == Admission::Refusal::AckFailed);
    a.Cancel();
    Admission::Debt model{}, mesh{};
    REQUIRE(a.NextDebt(model));
    REQUIRE(model.kind == Admission::DebtKind::ModelRetire);
    REQUIRE(model.producer == 200);
    REQUIRE(a.MarkDebtQueued(model));
    REQUIRE(a.NextDebt(mesh));
    REQUIRE(mesh.kind == Admission::DebtKind::MeshDestroy);
    REQUIRE_FALSE(a.SettleDebt(mesh)); // mesh destroy has not yet been queued
    REQUIRE(a.MarkDebtQueued(mesh));
    Admission::Debt extra{};
    REQUIRE_FALSE(a.NextDebt(extra)); // both retire operations remain owed until settled
    REQUIRE(a.SettleDebt(model));
    REQUIRE(a.GetState() == Admission::State::Cancelling);
    REQUIRE(a.SettleDebt(mesh));
    REQUIRE(a.GetState() == Admission::State::Cancelled);
}

TEST_CASE("Resumable admission rejects a resolver that hides multiple uploads in one role",
          "[resumable-model-admission]")
{
    Admission a; REQUIRE(Plan(a));
    const auto current = Proof();
    unsigned meshCalls = 0;
    REQUIRE_FALSE(a.Advance(1, [&](size_t) { return current; },
        [](size_t) { return Admission::UploadResult{9, 2, 2, true}; },
        [&](size_t, uint64_t, uint64_t plannedBytes) {
            ++meshCalls; return Admission::MeshResult{plannedBytes, true};
        }));
    REQUIRE(a.RefusalReason() == Admission::Refusal::QuotaViolation);
    REQUIRE(meshCalls == 0);
    a.Cancel();
    REQUIRE(a.GetState() == Admission::State::Cancelled);
}

TEST_CASE("Resumable admission retains mesh debt when enqueue reports a different allocation",
          "[resumable-model-admission]")
{
    Admission a; REQUIRE(Plan(a));
    auto current = Proof();
    REQUIRE_FALSE(a.Advance(1, [&](size_t) { return current; },
        [&](size_t) {
            current.handle = 9; current.slotLease = 2;
            return Admission::UploadResult{9, 2, 1, true};
        },
        [](size_t, uint64_t, uint64_t plannedBytes) {
            return Admission::MeshResult{plannedBytes + 1, true};
        }));
    REQUIRE(a.RefusalReason() == Admission::Refusal::QuotaViolation);
    a.Cancel();
    Admission::Debt debt{};
    REQUIRE(a.NextDebt(debt));
    REQUIRE(debt.kind == Admission::DebtKind::MeshDestroy);
    REQUIRE(a.MarkDebtQueued(debt));
    REQUIRE(a.SettleDebt(debt));
    REQUIRE(a.GetState() == Admission::State::Cancelled);
}

TEST_CASE("Resumable admission rechecks current source after the last model acknowledgment",
          "[resumable-model-admission]")
{
    Admission a; REQUIRE(Plan(a));
    auto current = Proof();
    const auto inspect = [&](size_t) { return current; };
    REQUIRE(a.Advance(1, inspect, [&](size_t) {
        current.handle = 9; current.slotLease = 2;
        return Admission::UploadResult{9, 2, 1, true};
    }, [](size_t, uint64_t, uint64_t plannedBytes) {
        return Admission::MeshResult{plannedBytes, true};
    }));
    REQUIRE(a.QueueOneModel(2, inspect, [](size_t, uint64_t) { return true; }));
    REQUIRE(a.AcknowledgeModel(200, true, true));
    current.mountedCurrent = false;
    REQUIRE_FALSE(a.PublishIfCurrent(inspect, true));
    REQUIRE(a.RefusalReason() == Admission::Refusal::StaleSource);
    a.Cancel();
    Admission::Debt debt{};
    REQUIRE(a.NextDebt(debt));
    REQUIRE(debt.kind == Admission::DebtKind::ModelRetire);
}
