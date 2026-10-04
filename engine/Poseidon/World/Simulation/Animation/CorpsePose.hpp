#pragma once

#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace Poseidon
{
// Calibrate only an inset contact proxy, never the authored pose or body frame.
// Larger penetrations require a different fit; they cannot be hidden by lifting.
inline bool CorpseProxyContactLift(float minimumClearance, float& lift)
{
    lift = 0;
    if (!std::isfinite(minimumClearance)) return false;
    if (minimumClearance >= 0.003f) return true;
    const float required = 0.00325f-minimumClearance; // 0.25 mm rounding reserve
    if (required > 0.010f) return false;
    lift = required;
    return true;
}

// Ordinary stock stance can embed its boot in the native floor. Permit only a
// bounded boot contact for the solver to resolve, not a displaced skin pose.
// The explicit settled diagnostic and every other part retain positive clearance.
inline bool CorpseGroundedContactRecoveryAdmitted(bool pendingGroundedAutomatic, float rootClearance)
{
    return pendingGroundedAutomatic && std::isfinite(rootClearance) &&
        rootClearance >= -.025f && rootClearance <= .3f;
}

// A grounded automatic handoff may start with authored prone contact. The real
// solver resolves at most 100 mm only after every body origin and native/solver
// floor agree. No skin/root displacement, collider resize or retirement change.
inline bool CorpseInitialContactAdmitted(bool ordinaryStockBoot, float clearance,
    bool groundedAutomaticRecovery = false)
{
    return std::isfinite(clearance) && clearance >=
        (groundedAutomaticRecovery ? -.1f : ordinaryStockBoot ? -.025f : .003f);
}

inline bool CorpseAutomaticRetirementAdmitted(bool supported, bool quiet,
    float quietSeconds, float simulatedSeconds)
{
    return supported && quiet && std::isfinite(quietSeconds) && std::isfinite(simulatedSeconds) &&
        (quietSeconds >= .75f || simulatedSeconds >= 12);
}

// The generated pelvis/chest pivot is their midpoint: measuring only its
// distal half unnecessarily halves the available orientation evidence. Use
// the actual complete centre-to-centre direction for that spherical root joint
// only, preserving its midpoint and requiring 25 mm measured separation.
inline bool CorpseMeasureSphericalRootAxis(Vector3Val proximalCenter, Vector3Val distalCenter,
    Vector3Val pivot, Vector3& axis)
{
    axis=VZero;
    if (!proximalCenter.IsFinite() || !distalCenter.IsFinite() || !pivot.IsFinite()) return false;
    const Vector3 midpoint=proximalCenter*.5f+distalCenter*.5f;
    if (!midpoint.IsFinite() || (midpoint-pivot).Size() > .001f) return false;
    const Vector3 direction=distalCenter-proximalCenter;
    const float length=direction.Size();
    if (!direction.IsFinite() || !std::isfinite(length) || length < .025f) return false;
    axis=direction/length;
    return axis.IsFinite();
}

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
    std::vector<Vector3> palettePoints, consumerPoints;
    std::vector<int> consumerPointIndices;
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
    std::string source = "shared-graphical-boundary";
    int sourceLevel = 0, sourcePairCount = 0;
    Vector3 modelAnchor = VZero, firstWorld = VZero, secondWorld = VZero;
    float separation = 0;
};

// A separate provenance for stock Angelina, whose source skin has no shared
// neck/shoulder/knee boundary. Use clustered nearest vertices of the two OWN
// authored single-bone Fire hulls. This never claims a shared skin vertex or a
// measured hinge, and preserves the actual <=12cm joint separation gate.
inline bool CorpseMeasureFireBoundary(const std::vector<Vector3>& firstModel,
    const std::vector<Vector3>& secondModel, const std::vector<Vector3>& firstWorld,
    const std::vector<Vector3>& secondWorld, CorpseAnchorDiagnostic& anchor)
{
    if (firstModel.empty() || secondModel.empty() || firstModel.size()>128 || secondModel.size()>128 ||
        firstWorld.size()!=firstModel.size() || secondWorld.size()!=secondModel.size()) return false;
    for (const auto* points : {&firstModel,&secondModel,&firstWorld,&secondWorld})
        for (const auto& point : *points) if (!point.IsFinite()) return false;
    for (const auto* points : {&firstModel,&secondModel})
        for (const auto& point : *points) if (!std::isfinite(point.Size()) || point.Size()>8) return false;
    float gap=std::numeric_limits<float>::infinity();
    for (const auto& a:firstModel) for (const auto& b:secondModel) gap=std::min(gap,(a-b).Size());
    if (!std::isfinite(gap) || gap>.12f) return false;
    Vector3 first=VZero,second=VZero,worldA=VZero,worldB=VZero;
    int count=0;
    for (std::size_t a=0;a<firstModel.size();++a) for (std::size_t b=0;b<secondModel.size();++b)
        if ((firstModel[a]-secondModel[b]).Size()<=gap+.005f)
        { first+=firstModel[a]; second+=secondModel[b]; worldA+=firstWorld[a]; worldB+=secondWorld[b]; ++count; }
    if (!count) return false;
    first*=1.f/count; second*=1.f/count; worldA*=1.f/count; worldB*=1.f/count;
    if (!first.IsFinite() || !second.IsFinite() || !worldA.IsFinite() || !worldB.IsFinite()) return false;
    // Reject a dispersed nearest-pair cluster rather than placing a pivot
    // between unrelated surfaces. Every contributing point remains bounded.
    for (std::size_t a=0;a<firstModel.size();++a) for (std::size_t b=0;b<secondModel.size();++b)
        if ((firstModel[a]-secondModel[b]).Size()<=gap+.005f &&
            ((firstModel[a]-first).Size()>.25f || (secondModel[b]-second).Size()>.25f ||
             (firstWorld[a]-worldA).Size()>.25f || (secondWorld[b]-worldB).Size()>.25f)) return false;
    const float separation=(worldA-worldB).Size();
    if (!std::isfinite(separation) || separation>.12f || (first-second).Size()>.12f) return false;
    anchor.sharedVertices=0; anchor.source="authored-fire-boundary-pair"; anchor.sourcePairCount=count;
    const Vector3 midpoint=first*.5f+second*.5f;
    if (!midpoint.IsFinite()) return false;
    anchor.modelAnchor=midpoint; anchor.firstWorld=worldA; anchor.secondWorld=worldB;
    anchor.separation=separation;
    return true;
}
struct CorpsePoseDiagnostic
{
    bool articulated = false;
    bool frozen = false, retainedBounds = false;
    Vector3 retainedMinimum = VZero, retainedMaximum = VZero;
    float retainedRadius = 0;
    Matrix4 object = MIdentity;
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

struct CorpseHingeMeasurement
{
    Vector3 proximal = VZero, distal = VZero, reference = VZero, axis = VZero;
    float capturedAngle = 0, referenceAgreement = 0;
    float lower = 0, upper = 0;
    const char* refusal = "nonfinite-input";
};

// Real neutral shared-selection endpoints and their captured affine transforms.
// Translations locate neither axis nor angle: only transformed geometry does.
// The affine matrices are read here, never replaced in the authored skin pose.
inline bool CorpseMeasureHinge(Vector3Val proximalPoint, Vector3Val jointPoint,
    Vector3Val distalPoint, Vector3Val neutralForward, Matrix4Val proximalAffine,
    Matrix4Val distalAffine, bool knee, CorpseHingeMeasurement& result)
{
    result = {};
    if (!proximalPoint.IsFinite() || !jointPoint.IsFinite() || !distalPoint.IsFinite() ||
        !neutralForward.IsFinite() || !proximalAffine.IsFinite() || !distalAffine.IsFinite()) return false;
    const float proximalDeterminant = CorpseMatrixDeterminant(proximalAffine);
    const float distalDeterminant = CorpseMatrixDeterminant(distalAffine);
    result.refusal = "singular-affine-frame";
    if (!std::isfinite(proximalDeterminant) || !std::isfinite(distalDeterminant) ||
        std::abs(proximalDeterminant) < .001f || std::abs(distalDeterminant) < .001f) return false;
    Vector3 u = proximalAffine.Rotate(jointPoint-proximalPoint);
    Vector3 v = distalAffine.Rotate(distalPoint-jointPoint);
    Vector3 forward = proximalAffine.Rotate(neutralForward*(knee ? -1.0f : 1.0f));
    result.proximal = u; result.distal = v; result.reference = forward;
    result.refusal = "nonfinite-transformed-segment";
    if (!u.IsFinite() || !v.IsFinite() || !forward.IsFinite() ||
        !std::isfinite(u.Size()) || !std::isfinite(v.Size()) || !std::isfinite(forward.Size())) return false;
    result.refusal = "short-transformed-segment";
    if (u.Size() < .08f || v.Size() < .08f || forward.Size() < .1f) return false;
    u.Normalize(); v.Normalize(); forward.Normalize();
    Vector3 reference = u.CrossProduct(forward);
    result.refusal = "neutral-flexion-plane-degenerate";
    if (reference.Size() < .25f) return false;
    reference.Normalize();
    Vector3 bend = u.CrossProduct(v);
    float cosine = std::clamp(u*v, -1.0f, 1.0f);
    float angle = std::atan2(bend.Size(), cosine);
    Vector3 axis = reference;
    float agreement = 1;
    result.proximal = u; result.distal = v; result.reference = reference; result.capturedAngle = angle;
    if (angle > .15f)
    {
        result.refusal = "folded-plane-degenerate";
        if (bend.Size() < .02f) return false; // almost folded: plane not determined
        axis = bend; axis.Normalize();
        agreement = axis*reference;
        result.axis = axis; result.referenceAgreement = agreement;
        result.refusal = "anatomical-plane-disagreement";
        if (std::abs(agreement) < .7f) return false; // anatomical plane ambiguous
        if (agreement < 0) { axis *= -1; angle *= -1; }
    }
    else
    {
        // Straight limbs have no stable cross-product plane. Carry the measured
        // neutral forward plane with the actual proximal affine instead.
        result.refusal = "straight-limb-lateral-disagreement";
        if (std::abs(v*reference) > .15f) return false;
        angle = std::atan2(reference*bend, cosine);
    }
    constexpr float degrees = 3.14159265358979323846f/180;
    const float extension = -5*degrees, flexion = (knee ? 145 : 150)*degrees;
    result.capturedAngle = angle;
    result.refusal = "captured-bend-outside-policy";
    if (!std::isfinite(angle) || angle < extension || angle > flexion) return false;
    result.proximal = u; result.distal = v; result.reference = reference; result.axis = axis;
    result.capturedAngle = angle; result.referenceAgreement = agreement;
    // Initial solver frames coincide: relative twist is exactly zero. Translate
    // the anatomical policy to that captured zero without a spring or pose snap.
    result.lower = extension-angle; result.upper = flexion-angle;
    result.refusal = "";
    return result.lower <= 0 && result.upper >= 0;
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
    bool MatchesIdentity(const void* w, const void* m, const void* s, int b, int p, int q, float f, int nowMs) const
    {
        return world && world == w && model == m && skeleton == s && bones == b &&
            primary == p && secondary == q && factor == f && nowMs >= capturedMs;
    }
    bool Matches(const void* w, const void* m, const void* s, int b, int p, int q, float f, int nowMs) const
    {
        return MatchesIdentity(w,m,s,b,p,q,f,nowMs) && static_cast<long long>(nowMs)-capturedMs <= 30000;
    }
};
}
