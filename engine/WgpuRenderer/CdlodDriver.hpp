#pragma once

#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>

#include <cmath>
#include <vector>

namespace Poseidon
{
// Per-frame CDLOD selection shared by the wgpu terrain and water renderers: clip
// to the visible land rectangle, frustum-cull each node, and descend by camera
// distance, emitting the selected patches. `extraVisible(node)` is an extra
// per-node predicate on top of the rect + frustum test — terrain passes an
// always-true one; water uses it to reject nodes that sit entirely above sea
// level. `emit(const CdlodSelection&)` receives each patch to draw.
//
// [rx0,rx1) x [rz0,rz1) is the visible world-xz rectangle (already clamped to the
// map + draw distance by the caller). This keeps the C++ CDLOD driver in one place
// so the eventual move of selection to Rust/GPU (Phase 4) is a one-site refactor.
template <typename ExtraVisibleFn, typename EmitFn>
inline void SelectVisibleCdlod(const std::vector<CdlodNode>& tree, int rootIndex, int numLevels,
                               const std::vector<float>& ranges, float morphRegion, Camera& camera, float rx0,
                               float rz0, float rx1, float rz1, ExtraVisibleFn&& extraVisible, EmitFn&& emit,
                               float displacementMargin = 0.0f, bool infiniteFar = false)
{
    if (rootIndex < 0 || numLevels <= 0 || rx1 <= rx0 || rz1 <= rz0)
    {
        return;
    }
    const Vector3 camPos = camera.Position();
    if (!std::isfinite(camPos.X()) || !std::isfinite(camPos.Y()) || !std::isfinite(camPos.Z())) return;

    auto visible = [&](const CdlodNode& n) -> bool
    {
        if (!CdlodIntersectsRect(n, rx0, rz0, rx1, rz1))
        {
            return false;
        }
        if (!extraVisible(n))
        {
            return false;
        }
        const Vector3 center(n.originX + n.size * 0.5f, (n.minY + n.maxY) * 0.5f, n.originZ + n.size * 0.5f);
        const float dy = n.maxY - n.minY;
        const float radius = 0.5f * std::sqrt(2.0f * n.size * n.size + dy * dy);
        const auto clipped = camera.IsClipped(center, radius + displacementMargin, 1);
        return !(clipped & (infiniteFar ? ~ClipBack : ClipAll));
    };

    SelectCdlod(tree, rootIndex, numLevels - 1, camPos.X(), camPos.Y(), camPos.Z(), ranges, morphRegion, visible,
                emit);
}

} // namespace Poseidon
