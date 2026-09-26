#pragma once

#include <Poseidon/World/Model/Model.hpp>

namespace Poseidon::Model
{
// Source decoding evidence only. Empty IR pose arrays cannot replace these
// facts: some ODOL readers deliberately skip authored keyframe payloads.
// Missing/old derived-cache audit data fails closed, without changing rendering.
inline bool HasStaticOdolSourceAudit(const Model& model)
{
    if (model.sourceFormat != "ODOL") return false;
    const auto& audit = model.sourceAudit;
    if (audit.producerVersion != 1 || audit.sourceRevision != model.sourceVersion ||
        audit.geometryCoverage != SourceGeometryCoverage::DeclaredLodsDecoded ||
        audit.observations != SourceAuditAllObservations || !audit.declaredLods ||
        audit.declaredLods != audit.decodedLods || audit.decodedLods != model.lodLevels.size())
        return false;
    switch (audit.sourceRevision) {
        case 7: case 40: case 48: case 49: case 50: case 52: case 54: case 73: break;
        default: return false;
    }
    return !audit.directoryHasAnimations && !audit.skeletonDeclared &&
        !audit.skeletonBones && !audit.directoryAnimationClasses && !audit.keyframeCount &&
        !audit.vertexBoneReferenceCount && !audit.neighbourBoneReferenceCount &&
        !audit.keyframePayloadDiscarded;
}
}
