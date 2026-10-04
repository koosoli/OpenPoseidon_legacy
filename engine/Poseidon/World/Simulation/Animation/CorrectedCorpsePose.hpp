#pragma once
#include <Poseidon/World/Simulation/Animation/CorpsePose.hpp>

namespace Poseidon
{
// Capture AFTER the three Man consumers have performed their own correction and
// interpolation arithmetic. CPU Point corrects a weighted point whereas the
// renderer corrects each bone; neither is reconstructed from the other.
struct CorpsePointBinding
{
    std::vector<int> bones;
    std::vector<float> weights;
};
struct CorrectedCorpseLevel
{
    std::vector<Matrix4> authoredPalette, palette;
    std::vector<Vector3> authoredPoints, points;
    std::vector<CorpsePointBinding> bindings;
    std::vector<int> proxySelections, proxyBones;
    std::vector<Matrix4> authoredProxies, proxies;
};
struct CorrectedCorpsePose
{
    static constexpr int MaximumLevels = 32, MaximumPoints = 4096, MaximumBones = 128;
    std::vector<CorrectedCorpseLevel> levels;
    Matrix4 capturedRoot = MIdentity, root = MIdentity;
    Vector3 initialSpeed = VZero, initialAngularVelocity = VZero;
    Vector3 lastSpeed = VZero, lastAngularVelocity = VZero;
    Vector3 minimum = VZero, maximum = VZero;
    float radius = 0, quietSeconds = 0, simulatedSeconds = 0;
    bool pending = true, frozen = false;
    int collisionFamily = -1;

    bool Valid() const
    {
        if (levels.empty() || levels.size() > MaximumLevels || !root.IsFinite() || !capturedRoot.IsFinite()) return false;
        for (const auto& level : levels)
        {
            if (level.palette.empty() || level.palette.size() > MaximumBones ||
                level.palette.size() != level.authoredPalette.size() || level.points.empty() ||
                level.points.size() > MaximumPoints || level.points.size() != level.authoredPoints.size() ||
                level.points.size() != level.bindings.size() || level.proxies.size() != level.authoredProxies.size() ||
                level.proxies.size() != level.proxySelections.size() || level.proxies.size() != level.proxyBones.size()) return false;
            for (const auto& matrix : level.palette) if (!matrix.IsFinite()) return false;
            for (const auto& matrix : level.authoredPalette) if (!matrix.IsFinite()) return false;
            for (const auto& point : level.points) if (!point.IsFinite() || point.Size() > 8) return false;
            for (const auto& point : level.authoredPoints) if (!point.IsFinite() || point.Size() > 8) return false;
            for (const auto& matrix : level.proxies) if (!matrix.IsFinite()) return false;
            for (const auto& matrix : level.authoredProxies) if (!matrix.IsFinite()) return false;
            for (const auto& binding : level.bindings)
            {
                if (binding.bones.size() != binding.weights.size() || binding.bones.size() > MaximumBones) return false;
                float total = 0;
                for (size_t i = 0; i < binding.bones.size(); ++i)
                {
                    if (binding.bones[i] < 0 || binding.bones[i] >= int(level.palette.size()) ||
                        !std::isfinite(binding.weights[i]) || binding.weights[i] < 0 || binding.weights[i] > 1) return false;
                    total += binding.weights[i];
                }
                if (!binding.bones.empty() && std::abs(total-1) > .001f) return false;
            }
            for (int bone : level.proxyBones) if (bone < 0 || bone >= int(level.palette.size())) return false;
        }
        return true;
    }

    // Always compose against the captured consumer output, never last tick's
    // output. The identity shortcut preserves its exact floating-point bytes.
    bool Apply(const std::vector<Matrix4>& deltas)
    {
        if (!Valid()) return false;
        for (const auto& delta : deltas) if (!delta.IsFinite()) return false;
        for (auto& level : levels)
        {
            if (deltas.size() != level.palette.size()) return false;
            for (size_t bone = 0; bone < deltas.size(); ++bone)
                level.palette[bone] = CorpseMatrixIdentityError(deltas[bone]) == 0 ?
                    level.authoredPalette[bone] : deltas[bone]*level.authoredPalette[bone];
            for (size_t p = 0; p < level.points.size(); ++p)
            {
                const auto& binding = level.bindings[p];
                bool identity = true;
                for (int bone : binding.bones) identity &= CorpseMatrixIdentityError(deltas[bone]) == 0;
                if (identity) { level.points[p] = level.authoredPoints[p]; continue; }
                Vector3 point = VZero;
                for (size_t w = 0; w < binding.bones.size(); ++w)
                    point += deltas[binding.bones[w]].FastTransform(level.authoredPoints[p])*binding.weights[w];
                level.points[p] = point;
            }
            for (size_t p = 0; p < level.proxies.size(); ++p)
                level.proxies[p] = CorpseMatrixIdentityError(deltas[level.proxyBones[p]]) == 0 ?
                    level.authoredProxies[p] : deltas[level.proxyBones[p]]*level.authoredProxies[p];
        }
        return Valid();
    }
};
}
