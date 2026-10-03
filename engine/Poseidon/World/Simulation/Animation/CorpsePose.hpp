#pragma once

#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Poseidon
{
// Read-only admission measurements, never a solver or a serialized pose.
struct CorpseLevelDiagnostic
{
    int level = -1, points = 0, compared = 0, skippedPointTails = 0, unweighted = 0;
    std::string role;
    bool faceEvaluated = false;
    bool pointOnly = false;
    float maxPointPaletteError = 0, minAbsDeterminant = 0;
    Vector3 paletteMin = VZero, paletteMax = VZero;
    std::vector<Matrix4> palette;
};
struct CorpseProxyDiagnostic
{
    int level = -1;
    std::string selection;
    Matrix4 matrix = MIdentity;
};
struct CorpseHullDiagnostic
{
    std::string name, bone;
    int points = 0;
    bool exclusiveFullWeight = false;
    Vector3 worldMin = VZero, worldMax = VZero;
    float minRoadSupportClearance = 0;
    int externalCenterRayHits = 0;
};
struct CorpseAnchorDiagnostic
{
    std::string first, second;
    int sharedVertices = 0;
    Vector3 modelAnchor = VZero, firstWorld = VZero, secondWorld = VZero;
    float separation = 0;
};
struct CorpsePoseDiagnostic
{
    bool articulated = false;
    std::string model, entity;
    int capturedMs = 0, measuredMs = 0;
    bool headIdentityFlag = false, gunIdentityFlag = false;
    float headIdentityError = 0, gunIdentityError = 0, legIdentityError = 0;
    Matrix4 headCorrection = MIdentity, gunCorrection = MIdentity, legCorrection = MIdentity;
    std::vector<std::string> bones;
    std::vector<CorpseLevelDiagnostic> levels;
    std::vector<CorpseProxyDiagnostic> proxies;
    std::vector<CorpseHullDiagnostic> hulls;
    std::vector<CorpseAnchorDiagnostic> anchors;
};

inline float CorpseMatrixIdentityError(Matrix4Val matrix)
{
    float error = 0;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            error = std::max(error, std::abs(matrix(row, col) - float(row == col)));
    return error;
}

inline float CorpseMatrixDeterminant(Matrix4Val matrix)
{
    return matrix.DirectionAside() * matrix.DirectionUp().CrossProduct(matrix.Direction());
}

// Owned phase data, deliberately not a quaternion or an already blended palette.
// Point, Matrix and PrepareMatrices have different floating-point operation orders.
// Preserve each one, including authored scale/shear and skeleton-tail identity.
class AffinePhasePose
{
    std::vector<Matrix4> _previous, _next, _authoredPrevious, _authoredNext;
    std::vector<Matrix4> _deltas;
    int _bones = 0;
    float _interpolation = 0;

public:
    void Clear()
    {
        std::vector<Matrix4>().swap(_previous); std::vector<Matrix4>().swap(_next);
        std::vector<Matrix4>().swap(_authoredPrevious); std::vector<Matrix4>().swap(_authoredNext); _bones = 0;
        std::vector<Matrix4>().swap(_deltas);
    }
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
        _authoredPrevious = _previous;
        _authoredNext = _next;
        _bones = bones;
        _interpolation = interpolation;
        return true;
    }

    // Left-multiply the ORIGINAL affine phase by the solver's model-space rigid
    // delta. Never accumulate previous ticks, quaternionize skin deformation or
    // preblend phases: all three consumers retain their weight arithmetic.
    bool ApplyRigidDeltas(const std::vector<Matrix4>& deltas)
    {
        if (!Valid() || deltas.size() != static_cast<size_t>(_bones)) return false;
        for (const Matrix4& delta : deltas)
            if (!delta.IsFinite()) return false;
        _deltas = deltas;
        for (int i = 0; i < PhaseMatrices(); ++i)
        {
            if (CorpseMatrixIdentityError(deltas[i]) == 0)
            {
                _previous[i] = _authoredPrevious[i]; _next[i] = _authoredNext[i];
            }
            else
            {
                _previous[i] = deltas[i] * _authoredPrevious[i];
                _next[i] = deltas[i] * _authoredNext[i];
            }
        }
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
            const Matrix4& tail = _deltas.empty() ? MIdentity : _deltas[i];
            if (first) matrices[i].InlineSetMultiply(tail, factor);
            else matrices[i].InlineAddMultiply(tail, factor);
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
            if (i >= PhaseMatrices())
            {
                if (_deltas.empty() || CorpseMatrixIdentityError(_deltas[i]) == 0) matrix += original * weightValue;
                else matrix += (_deltas[i] * original) * weightValue;
            }
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
