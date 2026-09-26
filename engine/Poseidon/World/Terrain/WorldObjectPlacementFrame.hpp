#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{

// Shared exact ObjectCreate orientation repair, using the engine's existing float math.
// Metadata callers must supply the ACTUAL object type, shape class and ClipLandKeep hint;
// a guessed class/type or source-only header is not a frame certificate. This intentionally
// performs no numerical validation and preserves historical forest/degenerate behavior.
// ObjectCreate's later terrain-height adjustment remains separate: when authored elevation
// is not preserved, this orientation frame alone is not the final object position.
inline Matrix4 RepairWorldObjectPlacementFrame(const Matrix4& raw, bool isForest, bool isNetwork, bool keepUp)
{
    Matrix4 repaired = raw;
    if (!isForest)
    {
        const float scale = isNetwork ? 1.0f : repaired.Scale();
        if (keepUp)
            repaired.SetUpAndAside(VUp, repaired.DirectionAside());
        else
            repaired.SetUpAndAside(repaired.DirectionUp(), repaired.DirectionAside());
        repaired.SetScale(scale);
    }
    return repaired;
}

} // namespace Poseidon
