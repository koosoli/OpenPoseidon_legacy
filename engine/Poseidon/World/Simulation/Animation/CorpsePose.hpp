#pragma once

#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <cmath>
#include <vector>

namespace Poseidon
{
// Owned phase data, deliberately not a quaternion or an already blended palette.
// Point, Matrix and PrepareMatrices have different floating-point operation orders.
// Preserve each one, including authored scale/shear and skeleton-tail identity.
class AffinePhasePose
{
    std::vector<Matrix4> _previous, _next;
    int _bones = 0;
    float _interpolation = 0;

public:
    void Clear() { std::vector<Matrix4>().swap(_previous); std::vector<Matrix4>().swap(_next); _bones = 0; }
    bool Valid() const { return _bones > 0 && !_previous.empty(); }
    int PhaseMatrices() const { return static_cast<int>(_previous.size()); }
    int Bones() const { return _bones; }

    bool Capture(const Matrix4* previous, const Matrix4* next, int count, int bones, float interpolation)
    {
        Clear();
        if (!previous || !next || count <= 0 || bones < count || bones > 128 ||
            !std::isfinite(interpolation)) return false;
        for (int i = 0; i < count; ++i)
            if (!previous[i].IsFinite() || !next[i].IsFinite()) return false;
        _previous.assign(previous, previous + count);
        _next.assign(next, next + count);
        _bones = bones;
        _interpolation = interpolation;
        return true;
    }

    bool SupportsPoint(const AnimationRTWeight& weight) const
    {
        if (!Valid()) return false;
        for (int w = 0; w < weight.Size(); ++w)
            if (weight[w].GetSel() < 0 || weight[w].GetSel() >= PhaseMatrices()) return false;
        return true;
    }

    bool SupportsMatrix(const AnimationRTWeight& weight) const
    {
        if (!Valid()) return false;
        for (int w = 0; w < weight.Size(); ++w)
            if (weight[w].GetSel() < 0 || weight[w].GetSel() >= _bones) return false;
        return true;
    }

    void PrepareMatrices(Matrix4Array& matrices, float factor) const
    {
        if (!Valid() || factor < 0.01f) return;
        bool first = matrices.Size() == 0;
        if (first) matrices.Resize(_bones);
        else if (_bones > matrices.Size())
        {
            int oldSize = matrices.Size();
            matrices.Resize(_bones);
            for (int i = oldSize; i < _bones; ++i) matrices[i] = MIdentity;
        }
        float f1 = (1 - _interpolation) * factor, f2 = _interpolation * factor;
        for (int i = 0; i < PhaseMatrices(); ++i)
        {
            if (first) matrices[i].InlineSetMultiply(_previous[i], f1);
            else matrices[i].InlineAddMultiply(_previous[i], f1);
            matrices[i].InlineAddMultiply(_next[i], f2);
        }
        for (int i = PhaseMatrices(); i < _bones; ++i)
        {
            if (first) matrices[i].InlineSetMultiply(MIdentity, factor);
            else matrices[i].InlineAddMultiply(MIdentity, factor);
        }
    }

    Vector3 Point(const AnimationRTWeight& weight, Vector3Val original, Vector3Val current) const
    {
        // Callers must admit SupportsPoint first. Legacy Point has no tail fallback;
        // do not invent one for the synthetic facial bones used only by palettes.
        if (weight.Size() <= 0) return current;
        Vector3 result = VZero;
        float f1 = 1 - _interpolation, f2 = _interpolation;
        for (int w = 0; w < weight.Size(); ++w)
        {
            int i = weight[w].GetSel();
            float weightValue = weight[w].GetWeight();
            Matrix4 pm, nm, cm;
            pm.InlineSetMultiply(_previous[i], f1 * weightValue);
            nm.InlineSetMultiply(_next[i], f2 * weightValue);
            cm.InlineSetAdd(pm, nm);
            result += cm * original;
        }
        return result;
    }

    void Matrix(Matrix4& matrix, const AnimationRTWeight& weight) const
    {
        if (!Valid()) return;
        float f1 = 1 - _interpolation, f2 = _interpolation;
        Matrix4 original = matrix;
        matrix = MZero;
        for (int w = 0; w < weight.Size(); ++w)
        {
            int i = weight[w].GetSel();
            float weightValue = weight[w].GetWeight();
            if (i >= PhaseMatrices()) matrix += original * weightValue;
            else
            {
                Matrix4 pm, nm, cm;
                pm.InlineSetMultiply(_previous[i], f1 * weightValue);
                nm.InlineSetMultiply(_next[i], f2 * weightValue);
                cm.InlineSetAdd(pm, nm);
                matrix += cm * original;
            }
        }
    }
};

// A mission/model/move-scoped lease, not serialized or networked. No pointer alone
// admits reuse: the owning Man also retains its model and skeleton while held.
struct CorpsePoseLease
{
    const void* world = nullptr;
    const void* model = nullptr;
    const void* skeleton = nullptr;
    int bones = 0, primary = -1, secondary = -1, capturedMs = 0;
    float factor = 0;
    bool Matches(const void* w, const void* m, const void* s, int b, int p, int q, float f, int nowMs) const
    {
        return world && world == w && model == m && skeleton == s && bones == b &&
            primary == p && secondary == q && factor == f && nowMs >= capturedMs &&
            static_cast<long long>(nowMs) - capturedMs <= 30000;
    }
};
}
