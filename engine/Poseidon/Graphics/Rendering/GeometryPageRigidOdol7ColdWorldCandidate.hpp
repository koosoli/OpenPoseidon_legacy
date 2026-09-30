#pragma once

#include <Poseidon/Graphics/Rendering/RigidOdol7OwnerCapture.hpp>
#include <Poseidon/IO/Streams/ModelCompressedSourceBirth.hpp>

namespace Poseidon::GeometryPages
{
// Candidate from the parser-born source of an already installed Shape. This
// does not create/adapt another Shape, recapture a bank member, resolve a
// texture, or authorize a world/renderer takeover. The owner reserves its own
// source-admission epoch after this transactional snapshot succeeds.
struct RigidOdol7ColdWorldCandidateSnapshot
{
    RigidOdol7Candidate candidate;
    std::shared_ptr<const ModelCompressedSourceBirth> sourceBirth;
    uint64_t shapeBirthId = 0;
    uint64_t shapeQueryRevision = 0;
    uint64_t knownCandidateCapacityBytes = 0;
};

enum class RigidOdol7ColdWorldStatus
{
    Captured, Disabled, WrongOwner, MissingBirth, StaleShape, StaleSource,
    ReadFailed, InvalidSource, Unsupported, Capacity, AllocationFailed
};

namespace RigidOdol7ColdWorldDetail
{
inline constexpr uint64_t MaxCandidateCapacityBytes = 128 * 1024;
static_assert(sizeof(RigidOdol7ColdWorldCandidateSnapshot) < MaxCandidateCapacityBytes);

// Charges owned C++ vector/string capacity in the retained candidate. The
// decoded replay is separately limited to 128 KiB. ODOL parsing and adapter
// library scratch are not represented as hard-bounded by either measurement.
inline bool CandidateCapacityBytes(const RigidOdol7Candidate& candidate, uint64_t& destination)
{
    uint64_t bytes = sizeof(RigidOdol7ColdWorldCandidateSnapshot);
    const auto add = [&](size_t count, size_t element) -> bool {
        if (!element || count > (MaxCandidateCapacityBytes - bytes) / element) return false;
        bytes += uint64_t(count) * element;
        return true;
    };
    if (!add(candidate.source.canonicalPath.capacity() + 1, 1) ||
        !add(candidate.lodEvidence.capacity(), sizeof(RigidOdol7LodEvidence)) ||
        !add(candidate.visual.capacity(), sizeof(RigidOdol7VisualCandidate))) return false;
    for (const auto& visual : candidate.visual)
        if (!add(visual.texturePath.capacity() + 1, 1) ||
            !add(visual.positions.capacity(), sizeof(Position)) ||
            !add(visual.vertices.capacity(), sizeof(RigidOdol7IrVertex)) ||
            !add(visual.indices.capacity(), sizeof(uint32_t)) ||
            !add(visual.triangleMaterials.capacity(), sizeof(uint32_t))) return false;
    destination = bytes;
    return true;
}

inline bool SameBirth(const LODShapeWithShadow* shape,
    const std::shared_ptr<const ModelCompressedSourceBirth>& expectedBirth,
    uint64_t expectedId, uint64_t revision)
{
    return shape && expectedBirth && expectedBirth->Valid() && expectedId &&
        expectedId != UINT64_MAX && revision && revision != UINT64_MAX &&
        shape->GetCompressedModelSourceBirth().get() == expectedBirth.get() &&
        shape->GetCompressedModelSourceBirthId() == expectedId &&
        shape->QueryPolicyRevision() == revision &&
        shape->Name() &&
        RigidOdol7FinalDetail::SamePath(shape->Name(), expectedBirth->logicalName);
}
} // namespace RigidOdol7ColdWorldDetail

inline RigidOdol7ColdWorldStatus CaptureRigidOdol7ColdWorldCandidate(
    const LODShapeWithShadow* shape,
    const std::shared_ptr<const ModelCompressedSourceBirth>& expectedBirth,
    uint64_t expectedShapeBirthId,
    RigidOdol7ColdWorldCandidateSnapshot& destination)
{
    using Status = RigidOdol7ColdWorldStatus;
    if (!RigidOdol7OwnerDetail::Enabled()) return Status::Disabled;
    if (!Foundation::IsMainThread()) return Status::WrongOwner;
    if (!shape || !expectedBirth || !expectedBirth->Valid() ||
        !expectedShapeBirthId || expectedShapeBirthId == UINT64_MAX)
        return Status::MissingBirth;
    const uint64_t revision = shape->QueryPolicyRevision();
    if (!RigidOdol7ColdWorldDetail::SameBirth(shape, expectedBirth,
                                             expectedShapeBirthId, revision))
        return Status::StaleShape;
    if (shape->GetAllowAnimation() || shape->Special() ||
        shape->GetMapType() != MapRock || (shape->Remarks() & REM_REVERSED) ||
        shape->NLevels() <= 0 || shape->NLevels() > 16)
        return Status::Unsupported;

    try {
        auto* bank = RigidOdol7OwnerDetail::ResolveBank();
        if (!bank || !bank->MatchesMountedCompressedMember(
                expectedBirth->lease->CanonicalMember().c_str(), *expectedBirth->lease))
            return Status::StaleSource;

        std::vector<char> decoded;
        std::string hex;
        if (!expectedBirth->lease->ReadDecoded(decoded, hex)) return Status::ReadFailed;
        if (decoded.empty() || decoded.size() != expectedBirth->decodedBytes ||
            decoded.size() > BankCompressedReadRequest::MaxDecodedBytes ||
            decoded.capacity() > BankCompressedReadRequest::MaxDecodedBytes)
            return Status::Capacity;
        RigidOdol7SourceBinding source;
        source.canonicalPath = expectedBirth->logicalName;
        source.decodedBytes = decoded.size();
        if (!RigidOdol7OwnerDetail::ParseHash(hex, source.decodedSha256) ||
            source.decodedSha256 != expectedBirth->decodedSha256)
            return Status::InvalidSource;

        std::string error;
        auto parsed = ModelCache::LoadOwnedBytes(decoded.data(), decoded.size(),
                                                 source.canonicalPath, error);
        if (!parsed) return Status::InvalidSource;
        if (!RigidOdol7OwnerDetail::WithinAdaptBudget(*parsed)) return Status::Capacity;
        RigidOdol7ColdWorldCandidateSnapshot result;
        const auto inspected = InspectRigidOdol7Candidate(*parsed, source, result.candidate);
        if (inspected == RigidOdol7Status::Capacity) return Status::Capacity;
        if (inspected == RigidOdol7Status::AllocationFailed) return Status::AllocationFailed;
        if (inspected == RigidOdol7Status::Unsupported) return Status::Unsupported;
        if (inspected != RigidOdol7Status::Candidate) return Status::InvalidSource;
        const auto* asset=RetailRigidAssetProfile::Selected();
        if (!asset || !asset->MatchesModelPath(source.canonicalPath) ||
            result.candidate.visual.size()!=asset->visualLevels) return Status::Unsupported;
        for(const auto& visual:result.candidate.visual)
            if(!asset->MatchesTexturePath(visual.texturePath)) return Status::Unsupported;
        if (!RigidOdol7ColdWorldDetail::CandidateCapacityBytes(
                result.candidate, result.knownCandidateCapacityBytes))
            return Status::Capacity;

        bank = RigidOdol7OwnerDetail::ResolveBank();
        if (!bank || !bank->MatchesMountedCompressedMember(
                expectedBirth->lease->CanonicalMember().c_str(), *expectedBirth->lease))
            return Status::StaleSource;
        if (!RigidOdol7ColdWorldDetail::SameBirth(shape, expectedBirth,
                                                 expectedShapeBirthId, revision))
            return Status::StaleShape;
        result.sourceBirth = expectedBirth;
        result.shapeBirthId = expectedShapeBirthId;
        result.shapeQueryRevision = revision;
        destination = std::move(result);
        return Status::Captured;
    } catch (const std::bad_alloc&) {
        return Status::AllocationFailed;
    } catch (...) {
        return Status::InvalidSource;
    }
}
} // namespace Poseidon::GeometryPages
