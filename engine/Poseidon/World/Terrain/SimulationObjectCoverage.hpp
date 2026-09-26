#pragma once

#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/SimulationCoverageIndex.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace Poseidon::Streaming
{

enum class InstanceCoverageStatus : uint8_t { Unknown, Observed, Invalid, WrongOwner, ExpiredLease };
class OwnerCoverageObservationScope;

// A borrowed, sealed observation of ONE constructed object. It cannot certify
// another placement, a whole model group, collision readiness or a clear query.
// Stored addresses are identity comparisons only, never dereferenced. The caller
// must obtain a live object again for validation. Weak lifetime witnesses retain
// tracker metadata only, never the Object/LODShape. Create, copy and destroy these
// records on the owner thread; intrusive shape links are not worker-safe.
class InstanceCoverageObservation
{
    friend class OwnerCoverageObservationScope;
    struct Sample
    {
        uintptr_t objectIdentity = 0, shapeIdentity = 0;
        uint64_t shapePolicyRevision = 0;
        int id = -1, type = 0;
        std::array<float, 12> frame{};
        float scale = 0, shapeRadius = 0, objectRadius = 0;
        float destruction = 0, damage = 0;
        bool destroyed = false;
        bool operator==(const Sample& other) const
        {
            return objectIdentity == other.objectIdentity && shapeIdentity == other.shapeIdentity &&
                shapePolicyRevision == other.shapePolicyRevision && id == other.id && type == other.type &&
                frame == other.frame && scale == other.scale &&
                shapeRadius == other.shapeRadius && objectRadius == other.objectRadius &&
                destruction == other.destruction && damage == other.damage && destroyed == other.destroyed;
        }
    };
    // Empty refused records must not construct/refcount the global LLinkNil.
    std::optional<OLink<Object>> _objectLifetime;
    Link<LODShape> _shapeLifetime;
    Sample _sample;
    uint64_t _lease = 0;
    InstanceCoverageStatus _status = InstanceCoverageStatus::Unknown;
    std::optional<CoverageEnvelope> _envelope;

public:
    InstanceCoverageStatus Status() const { return _status; }
    int ObjectId() const { return _sample.id; }
    // This envelope is evidence to revalidate, not permission to publish it as
    // persistent coverage. Only Validate within the originating scope grants a
    // current observation of the supplied live object's rejection sphere.
    const std::optional<CoverageEnvelope>& Envelope() const { return _envelope; }
};

// Owner operation lifetime, minted here rather than supplied by a caller as an
// arbitrary freshness number. Use lexical RAII on the already captured owner
// thread. A nested scope suspends its parent; closing restores the parent, whose
// observations still require exact live revalidation. Closed scopes cannot be
// reused, tokens never wrap, and workers cannot observe engine objects.
//
// This is NOT a world/config mutation epoch or an AI worker publication barrier.
// Persistent group certification and background admission need those separately.
class OwnerCoverageObservationScope
{
    inline static thread_local uint64_t _lastToken = 0;
    inline static thread_local uint64_t _activeToken = 0;
    uint64_t _token = 0, _previous = 0;

    static InstanceCoverageObservation Refused(InstanceCoverageStatus status)
    {
        InstanceCoverageObservation out;
        out._status = status;
        return out;
    }

    InstanceCoverageObservation SampleLive(const Object& object) const
    {
        if (!Foundation::IsMainThread())
            return Refused(InstanceCoverageStatus::WrongOwner);
        if (!Active())
            return Refused(InstanceCoverageStatus::ExpiredLease);
        auto* shape = object.GetShape();
        // Only authoritative static terrain objects are supported by this slice.
        // Dynamic/temporary/attached vehicle query ownership remains separate.
        if (!shape || !object.Static() || (object.GetType() != Primary && object.GetType() != Network))
            return Refused(InstanceCoverageStatus::Unknown);

        InstanceCoverageObservation out;
        auto& sample = out._sample;
        sample.objectIdentity = reinterpret_cast<uintptr_t>(&object);
        sample.shapeIdentity = reinterpret_cast<uintptr_t>(shape);
        sample.shapePolicyRevision = shape->QueryPolicyRevision();
        if (sample.shapePolicyRevision == 0)
            return Refused(InstanceCoverageStatus::Unknown);
        sample.id = object.ID();
        sample.type = object.GetType();
        const Matrix4& frame = object.Transform();
        sample.frame = {frame.DirectionAside().X(), frame.DirectionAside().Y(), frame.DirectionAside().Z(),
            frame.DirectionUp().X(), frame.DirectionUp().Y(), frame.DirectionUp().Z(),
            frame.Direction().X(), frame.Direction().Y(), frame.Direction().Z(),
            frame.Position().X(), frame.Position().Y(), frame.Position().Z()};
        for (float value : sample.frame)
            if (!std::isfinite(value))
                return Refused(InstanceCoverageStatus::Invalid);
        // Reject singular frames conservatively; a positive RMS Scale alone does
        // not make a usable engine collision transform.
        const auto& f = sample.frame;
        const double determinant = double(f[0]) * (double(f[4]) * f[8] - double(f[5]) * f[7]) -
            double(f[3]) * (double(f[1]) * f[8] - double(f[2]) * f[7]) +
            double(f[6]) * (double(f[1]) * f[5] - double(f[2]) * f[4]);
        if (!std::isfinite(determinant) || determinant == 0)
            return Refused(InstanceCoverageStatus::Invalid);
        sample.scale = object.Scale();
        sample.shapeRadius = shape->BoundingSphere();
        sample.objectRadius = object.GetRadius();
        sample.destruction = object.GetDestroyed();
        sample.damage = object.GetRawTotalDammage();
        sample.destroyed = object.IsDestroyed();
        if (!std::isfinite(sample.objectRadius) || sample.objectRadius < 0 ||
            !std::isfinite(sample.destruction) || !std::isfinite(sample.damage))
            return Refused(InstanceCoverageStatus::Invalid);
        out._envelope = BuildEngineBroadphaseEnvelope({f[9], f[10], f[11]}, sample.scale, sample.shapeRadius);
        if (!out._envelope)
            return Refused(InstanceCoverageStatus::Invalid);
        // Engine lifetime tracking is weak: destruction clears these witnesses,
        // including when an allocator later gives a replacement the same address.
        // The const cast binds owner-side tracker metadata, not mutable geometry.
        out._objectLifetime.emplace(const_cast<Object*>(&object));
        out._shapeLifetime = Link<LODShape>(shape);
        out._lease = _token;
        out._status = InstanceCoverageStatus::Observed;
        return out;
    }

public:
    OwnerCoverageObservationScope()
    {
        POSEIDON_MAIN_THREAD_ONLY("OwnerCoverageObservationScope -- live coverage observation is owner-thread only");
        if (!Foundation::IsMainThread() || _lastToken == std::numeric_limits<uint64_t>::max())
            return;
        _previous = _activeToken;
        _token = ++_lastToken;
        _activeToken = _token;
    }
    ~OwnerCoverageObservationScope()
    {
        if (_token && Foundation::IsMainThread() && _activeToken == _token)
            _activeToken = _previous;
    }
    OwnerCoverageObservationScope(const OwnerCoverageObservationScope&) = delete;
    OwnerCoverageObservationScope& operator=(const OwnerCoverageObservationScope&) = delete;
    bool Active() const { return Foundation::IsMainThread() && _token != 0 && _activeToken == _token; }

    InstanceCoverageObservation Observe(const Object& object) const { return SampleLive(object); }
    InstanceCoverageObservation Validate(const Object& live, const InstanceCoverageObservation& prior) const
    {
        if (!Foundation::IsMainThread())
            return Refused(InstanceCoverageStatus::WrongOwner);
        if (!Active() || prior._lease != _token)
            return Refused(InstanceCoverageStatus::ExpiredLease);
        // Check surviving lifetimes BEFORE comparing same-valued numeric state.
        // Neither witness keeps its target alive or dereferences a stale address.
        if (!prior._objectLifetime || prior._objectLifetime->GetLink() != &live ||
            prior._shapeLifetime.GetTypeRef() != live.GetShape())
            return Refused(InstanceCoverageStatus::Unknown);
        const auto current = SampleLive(live);
        if (current.Status() != InstanceCoverageStatus::Observed)
            return current;
        // Config constructors can substitute a shape or recompute a shared
        // radius; simulation can move/mutate the object. Never extrapolate a
        // prior observation after any such change, even inside this scope.
        // The local policy revision also catches equal-valued recomputes/reloads;
        // mutable per-LOD geometry and per-object animation still need separate proof.
        if (prior.Status() != InstanceCoverageStatus::Observed || !(prior._sample == current._sample))
            return Refused(InstanceCoverageStatus::Unknown);
        return current;
    }
};

} // namespace Poseidon::Streaming
