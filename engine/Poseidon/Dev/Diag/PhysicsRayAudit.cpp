#include <Poseidon/Dev/Diag/PhysicsRayAudit.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Poseidon::Dev
{
namespace
{

std::atomic<bool>          s_enabled{[]
{
    const char* v = std::getenv("POSEIDON_PHYSICS_RAY_AUDIT");
    return v && std::strcmp(v, "0") != 0;
}()};

// Relaxed: counters read by a human at panel refresh rate, with nothing to order
// against. Atomic only because projectiles are not guaranteed to be one thread.
std::atomic<std::uint64_t> s_segments{0};
std::atomic<std::uint64_t> s_agreeHit{0};
std::atomic<std::uint64_t> s_agreeMiss{0};
std::atomic<std::uint64_t> s_legacyOnly{0};
std::atomic<std::uint64_t> s_physicsOnly{0};

} // namespace

bool RayAuditEnabled() { return s_enabled.load(std::memory_order_relaxed); }
void SetRayAudit(bool enabled) { s_enabled.store(enabled, std::memory_order_relaxed); }

RayAuditStats GetRayAuditStats()
{
    RayAuditStats stats;
    stats.segments = s_segments.load(std::memory_order_relaxed);
    stats.agreeHit = s_agreeHit.load(std::memory_order_relaxed);
    stats.agreeMiss = s_agreeMiss.load(std::memory_order_relaxed);
    stats.legacyOnly = s_legacyOnly.load(std::memory_order_relaxed);
    stats.physicsOnly = s_physicsOnly.load(std::memory_order_relaxed);
    return stats;
}

void ResetRayAudit()
{
    s_segments.store(0, std::memory_order_relaxed);
    s_agreeHit.store(0, std::memory_order_relaxed);
    s_agreeMiss.store(0, std::memory_order_relaxed);
    s_legacyOnly.store(0, std::memory_order_relaxed);
    s_physicsOnly.store(0, std::memory_order_relaxed);
}

void AuditProjectileSegment(Vector3Par from, Vector3Par to, bool legacyHit)
{
    if (!RayAuditEnabled())
    {
        return;
    }
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated())
    {
        return;
    }

    // FIRE GEOMETRY, because that is the question the other side asked.
    // `Landscape::ObjectCollision` defaults to `ObjIntersectFire`, so an unfiltered
    // physics ray was comparing "did anything at all stop this" against "did the
    // fire geometry stop this" -- and every model whose View Geometry LOD is a
    // coarser box than its Fire Geometry counted as a physics-only disagreement for
    // a reason that was ours, not the corpus's.
    const Physics::QueryFilter filter{Physics::ColliderFlags::BulletCollision};
    Vector3                    point;
    const bool                 physicsHit =
        world->CastRay(from, to, point, filter).IsValid() || world->RayHitAnything(from, to, filter);

    s_segments.fetch_add(1, std::memory_order_relaxed);
    if (legacyHit && physicsHit)
    {
        s_agreeHit.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!legacyHit && !physicsHit)
    {
        s_agreeMiss.fetch_add(1, std::memory_order_relaxed);
    }
    else if (legacyHit)
    {
        // The 2001 path saw geometry the physics world does not have. Either a
        // model that did not convert, or one that converted to a smaller shape
        // than it draws -- both are collider-set findings.
        s_legacyOnly.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // Physics saw something the legacy path did not. Usually the terrain,
        // which the projectile path tests separately, so this counter is expected
        // to be non-zero and is not by itself a fault.
        s_physicsOnly.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace Poseidon::Dev
