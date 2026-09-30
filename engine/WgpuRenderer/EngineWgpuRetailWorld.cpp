#include "EngineWgpu.hpp"
#include "RetailPagePilotAdmission.hpp"
#include "TextureWgpu.hpp"

#include <Poseidon/Graphics/Rendering/GeometryPageRigidOdol7ColdWorldCandidate.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalHierarchyProduct.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

namespace Poseidon
{
namespace
{
std::string RetailWorldHashHex(const std::array<uint8_t, 32>& hash)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < hash.size(); ++i)
    {
        result[i * 2] = digits[hash[i] >> 4];
        result[i * 2 + 1] = digits[hash[i] & 15];
    }
    return result;
}
} // namespace

Engine::GeometryPageFixtureReport EngineWgpu::ProbeRetailColdWorldRecords()
{
    using namespace GeometryPages;
    GeometryPageFixtureReport refusal;
    refusal.status = GeometryReportStatus::Invalid;
    refusal.retailSourceProbe = true;
    refusal.pageWorkState = "RetailColdWorldRecordsRefused";
    // The global bank-owned texture and installed Shape stay on the ordinary
    // world path. This action creates only private records after exact capture.
    if (!Foundation::IsMainThread() || !ModelCompressedSourceBirth::ReadScope::Enabled() ||
        !RigidOdol7OwnerDetail::Enabled() || !WgpuTextureReuseOnlyBinding::Enabled() ||
        !_renderer || !_gpuDriven || !_retailWorldSource || _retailPagePilot ||
        _geometryPageFixture || !_geometryOwnerEpoch || _geometryOwnerEpoch == UINT64_MAX ||
        !_nextRetailWorldSourceEpoch || _nextRetailWorldSourceEpoch == UINT64_MAX)
        return refusal;

    try
    {
        const auto* asset = RetailRigidAssetProfile::Selected();
        if (!asset) return refusal;
        const auto shape = _retailWorldSource->shape;
        const auto primary = _retailWorldSource->texture;
        const auto textureBirth = _retailWorldSource->textureBirth;
        const uint64_t shapeBirthId = _retailWorldSource->shapeBirth;
        const uint64_t handle = _retailWorldSource->textureHandle;
        const uint32_t slot = _retailWorldSource->textureSlot;
        const std::string rawSha = _retailWorldSource->rawSha256;
        const std::string uploadedSha = _retailWorldSource->uploadedSha256;
        auto* texture = static_cast<TextureWgpu*>(primary.GetRef());
        if (!shape || !texture || !textureBirth || !shapeBirthId ||
            shapeBirthId == UINT64_MAX || !handle || !slot ||
            _retailWorldSource->placements == 0 || rawSha.size() != 64 ||
            uploadedSha.size() != 64 || rawSha.capacity() > 128 ||
            uploadedSha.capacity() > 128 ||
            !PatnikArchiveReason::Matches(RigidOdol7OwnerDetail::PrimaryPac, texture->SourceName()) ||
            texture->GpuHandle() != handle || texture->SlotLease() != slot ||
            texture->IsDynamicTexture() || texture->HasAttemptedGpuUpload() == false)
            return refusal;
        auto sourceBirth = shape->GetCompressedModelSourceBirth();
        if (!sourceBirth || !sourceBirth->Valid() ||
            shape->GetCompressedModelSourceBirthId() != shapeBirthId ||
            !RigidOdol7FinalDetail::SamePath(shape->Name(), sourceBirth->logicalName))
            return refusal;
        const auto normalModel = _gpuModels.find(shape.GetRef());
        if (normalModel == _gpuModels.end() || normalModel->second == WGR_INVALID_MODEL)
            return refusal;
        const uint32_t originalProducerModel = normalModel->second;
        const auto currentTexture = texture->CurrentArchiveSourceBinding();
        if (!currentTexture ||
            !currentTexture->Request().SameArchiveMember(textureBirth->Request()))
            return refusal;
        refusal.retailPixelSourceVerified = true;
        refusal.retailPixelSourceSha256 = rawSha;

        RigidOdol7ColdWorldCandidateSnapshot snapshot;
        const auto captured = CaptureRigidOdol7ColdWorldCandidate(
            shape.GetRef(), sourceBirth, shapeBirthId, snapshot);
        refusal.retailSourcePrepareStatus = uint32_t(captured);
        if (captured != RigidOdol7ColdWorldStatus::Captured)
        {
            refusal.pageWorkState = "RetailColdWorldCandidateRefused";
            return refusal;
        }
        refusal.knownPayloadBytes = snapshot.knownCandidateCapacityBytes;
        // Mint only after parser-born source, live Shape incarnation, decoded
        // bytes and candidate capacity have passed their transactional gate.
        const uint64_t sourceEpoch = _nextRetailWorldSourceEpoch++;
        auto* modelBank = RigidOdol7OwnerDetail::ResolveBank();
        if (!modelBank || !modelBank->MatchesMountedCompressedMember(
                sourceBirth->lease->CanonicalMember().c_str(), *sourceBirth->lease))
        {
            refusal.pageWorkState = "RetailColdWorldModelMountChanged";
            return refusal;
        }
        const RigidOdol7FinalSourceHold sourceHold{
            sourceBirth->lease, modelBank, snapshot.candidate.source.decodedSha256,
            sourceEpoch, _geometryOwnerEpoch, shapeBirthId};
        if (!RigidOdol7FinalDetail::SourceCurrent(snapshot.candidate, sourceHold))
            return refusal;

        WgpuTextureReuseOnlyBinding reuse;
        if (!reuse.Add(primary, {handle, slot, texture->IsDynamicTexture(), false}) ||
            !reuse.Seal())
        {
            refusal.pageWorkState = "RetailColdWorldReuseRefused";
            return refusal;
        }
        auto inspect = [this](int level, const ShapeSection& section)
        {
            return InspectRetailWorldSection(level, section);
        };
        RigidOdol7FinalExport actual;
        RigidOdol7FinalStatus exported;
        {
            WgpuTextureReuseOnlyBinding::Scope scope(reuse, true, true);
            exported = ExportRigidOdol7FinalShape(
                *shape, snapshot.candidate, asset->coarseSourceLod, asset->fineSourceLod, sourceHold, inspect, actual);
        }
        refusal.retailSourceExportStatus = uint32_t(exported);
        refusal.retailSourceExported = exported == RigidOdol7FinalStatus::Exported;
        refusal.retailSourceOtherPassesRequired = actual.requiresOriginalOtherPasses;
        if (!refusal.retailSourceExported || reuse.Read().failed ||
            actual.actualNoShadow != asset->selectedNoShadow ||
            actual.modelBirth != shapeBirthId ||
            actual.shapeQueryRevision != snapshot.shapeQueryRevision)
        {
            refusal.pageWorkState = "RetailColdWorldFinalShapeRefused";
            return refusal;
        }
        refusal.sourceAdmissionEpoch = sourceEpoch;
        refusal.originalSourceBytes = sourceBirth->decodedBytes;
        refusal.sourceSha256 = RetailWorldHashHex(actual.actual.source.sourceSha256);
        refusal.coarseTriangles = uint32_t(actual.actual.coarse.indices.size() / 3);
        refusal.fineTriangles = uint32_t(actual.actual.fine.indices.size() / 3);

        RigidFinalHierarchyProduct product;
        const auto built = BuildRigidFinalHierarchyProduct(actual, product);
        refusal.retailHierarchyProductStatus = uint32_t(built);
        if (built != RigidFinalHierarchyStatus::Produced)
        {
            refusal.pageWorkState = "RetailColdWorldHierarchyRefused";
            return refusal;
        }
        const char* autoFlag = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_WORLD_AUTO");
        const bool autoEnabled = autoFlag && autoFlag[0] == '1' && !autoFlag[1];
        std::shared_ptr<const RigidFinalSurface::Proof> certifiedSurface;
        uint64_t proofCharge = 0;
        if (autoEnabled)
        {
            RigidFinalSurface::Proof proof;
            const auto status = RigidFinalSurface::Build(actual, product, proof);
            refusal.retailWorldSurfaceStatus = uint32_t(status);
            if (status != RigidFinalSurface::Status::Certified || !proof.certificate ||
                !proof.root || !proof.fine || !proof.certificate->rootTriangles ||
                !proof.certificate->fineTriangles)
            {
                refusal.pageWorkState = "RetailColdWorldSurfaceCertificateRefused";
                return refusal;
            }
            proofCharge = sizeof(RigidFinalSurface::Proof) + sizeof(RigidFinalSurface::Certificate) +
                proof.root->knownCapacityBytes + proof.fine->knownCapacityBytes + 512;
            if (proofCharge > 512 * 1024)
            {
                refusal.pageWorkState = "RetailColdWorldSurfaceCapacity";
                return refusal;
            }
            certifiedSurface = std::make_shared<const RigidFinalSurface::Proof>(std::move(proof));
            refusal.retailWorldSurfaceCertified = true;
            refusal.retailWorldSurfaceUpper = certifiedSurface->certificate->hausdorffUpper;
        }
        render::RetailPagePilotAdmission admission;
        const int fineLevel = actual.finalLevels[1];
        {
            WgpuTextureReuseOnlyBinding::Scope scope(reuse, true, true);
            if (!ClassifyRetailWorldMaterial(fineLevel, admission.section, admission.material) ||
                reuse.Read().failed)
            {
                refusal.pageWorkState = "RetailColdWorldMaterialRefused";
                return refusal;
            }
        }
        if (admission.material.texture_id != handle || admission.material.alpha_ref != 0 ||
            admission.section.variant != 0 || admission.section.flags != 0)
            return refusal;

        admission.actual = std::make_shared<const RigidOdol7FinalExport>(std::move(actual));
        admission.product = std::make_shared<const RigidFinalHierarchyProduct>(std::move(product));
        admission.certifiedSurface = std::move(certifiedSurface);
        admission.knownProofBytes = proofCharge;
        admission.modelLease = sourceBirth->lease;
        admission.texture = primary;
        admission.textureBirth = textureBirth;
        admission.textureHandle = handle;
        admission.textureSlotLease = slot;
        admission.pixelSourceVerified = true; // strict upload witness retained by actual-world owner
        if (!RigidOdol7OwnerDetail::ParseHash(rawSha, admission.pixelSourceSha256))
            return refusal;
        for (const auto* mesh : {&admission.actual->actual.coarse, &admission.actual->actual.fine})
            for (const auto& p : mesh->positions)
                admission.sphereRadius = std::max(admission.sphereRadius,
                    std::nextafter(float(std::sqrt(double(p.x) * p.x + double(p.y) * p.y +
                                                   double(p.z) * p.z)),
                                   std::numeric_limits<float>::infinity()));

        const uint64_t revision = snapshot.shapeQueryRevision;
        const uint64_t ownerEpoch = _geometryOwnerEpoch;
        const auto exportHold = admission.actual;
        // The existing Shape's geometry is not copied into this charge. Strong
        // Ref handles, both source metadata leases, lambda captures/control and
        // admission fields are; exported/page payloads have their own caps.
        const auto current = [this, shape, sourceBirth, primary, textureBirth,
                              handle, slot, shapeBirthId, revision, ownerEpoch,
                              originalProducerModel, rawSha, uploadedSha, exportHold]()
        {
            if (!Foundation::IsMainThread() || !_renderer || !_gpuDriven ||
                !RigidOdol7OwnerDetail::Enabled() ||
                !ModelCompressedSourceBirth::ReadScope::Enabled() ||
                _geometryOwnerEpoch != ownerEpoch || !_retailWorldSource ||
                _retailWorldSource->shape.GetRef() != shape.GetRef() ||
                _retailWorldSource->texture.GetRef() != primary.GetRef() ||
                _retailWorldSource->shapeBirth != shapeBirthId ||
                _retailWorldSource->textureBirth.get() != textureBirth.get() ||
                _retailWorldSource->textureHandle != handle ||
                _retailWorldSource->textureSlot != slot ||
                _retailWorldSource->placements == 0 ||
                _retailWorldSource->rawSha256 != rawSha ||
                _retailWorldSource->uploadedSha256 != uploadedSha ||
                !RigidOdol7ColdWorldDetail::SameBirth(
                    shape.GetRef(), sourceBirth, shapeBirthId, revision) ||
                exportHold->shapeQueryRevision != revision ||
                exportHold->modelBirth != shapeBirthId ||
                exportHold->ownerEpoch != ownerEpoch ||
                exportHold->actual.source.sourceSha256 != sourceBirth->decodedSha256)
                return false;
            const auto model = _gpuModels.find(shape.GetRef());
            if (model == _gpuModels.end() || model->second != originalProducerModel)
                return false;
            auto* bank = RigidOdol7OwnerDetail::ResolveBank();
            if (!bank || !bank->MatchesMountedCompressedMember(
                    sourceBirth->lease->CanonicalMember().c_str(), *sourceBirth->lease))
                return false;
            BankReadMemberIdentity member;
            if (!sourceBirth->lease->Encoded().CopyMemberIdentity(member) ||
                !(member == exportHold->sourceMember))
                return false;
            auto* t = static_cast<TextureWgpu*>(primary.GetRef());
            auto init = t ? t->CurrentArchiveSourceBinding() : nullptr;
            return init && init->Request().SameArchiveMember(textureBirth->Request()) &&
                PatnikArchiveReason::Matches(RigidOdol7OwnerDetail::PrimaryPac, t->SourceName()) &&
                t->GpuHandle() == handle && t->SlotLease() == slot;
        };
        admission.knownAuthorityBytes = sizeof(admission) + sizeof(current) +
            sizeof(RetailWorldSource) + sourceBirth->KnownCppBytes() +
            textureBirth->KnownCppBytes() +
            2 * (rawSha.capacity() + uploadedSha.capacity()) + proofCharge + 512;
        if (admission.knownAuthorityBytes > 1024 * 1024)
        {
            refusal.pageWorkState = "RetailColdWorldAuthorityCapacity";
            return refusal;
        }
        admission.sourceCurrent = current;
        if (!admission.sourceCurrent())
        {
            refusal.pageWorkState = "RetailColdWorldSourceChanged";
            return refusal;
        }
        LOG_INFO(Graphics,
            "Retail actual-world record source: shapeBirth={} originalProducerModel={} sourceAdmissionEpoch={} sourceCurrent=true scope=original-model-unchanged-private-records-only",
            shapeBirthId, originalProducerModel, sourceEpoch);
        auto records = BeginRetailPageRecordPilot(std::move(admission));
        records.retailSourceProbe = true;
        records.retailSourceExported = true;
        records.retailSourceOtherPassesRequired = true;
        records.retailPixelSourceVerified = refusal.retailPixelSourceVerified;
        records.retailPixelSourceSha256 = refusal.retailPixelSourceSha256;
        records.retailSourcePrepareStatus = uint32_t(captured);
        records.retailSourceExportStatus = uint32_t(exported);
        records.retailHierarchyProductStatus = uint32_t(built);
        records.originalSourceBytes = sourceBirth->decodedBytes;
        records.sourceSha256 = refusal.sourceSha256;
        LOG_INFO(Graphics,
            "Retail actual-world records: shapeBirth={} originalProducerModel={} sourceAdmissionEpoch={} status={} scope=record-only-no-instance-or-world-takeover",
            shapeBirthId, originalProducerModel, sourceEpoch, uint32_t(records.status));
        return records;
    }
    catch (...)
    {
        refusal.status = GeometryReportStatus::Failed;
        refusal.pageWorkState = "RetailColdWorldAllocationOrOwnerFailure";
        return refusal;
    }
}
} // namespace Poseidon
