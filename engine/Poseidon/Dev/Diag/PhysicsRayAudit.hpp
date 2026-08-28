#pragma once

#include <Poseidon/Core/Types.hpp>

// PHY-030: ask the physics world the same question the 2001 collision path was
// just asked, and count how often the two disagree.
//
// REPLACES NOTHING. The legacy answer is still the one the game acts on; this
// only watches. That is the whole point of the slice -- PHY-001 defers replacing
// the ray path, and a comparison is how you find out whether replacing it would
// ever be safe, without betting anything on the answer.
//
// It also puts a number on the collider set. The corpus census says 82,381
// objects converted; it cannot say whether they converted to the right SHAPE.
// Two rays over the same segment can.
//
// Off by default: it costs a second ray per projectile step.

namespace Poseidon::Dev
{

/// Counts since the last reset.
struct RayAuditStats
{
    std::uint64_t segments = 0;      ///< segments compared
    std::uint64_t agreeHit = 0;      ///< both saw something
    std::uint64_t agreeMiss = 0;     ///< neither did
    std::uint64_t legacyOnly = 0;    ///< the 2001 path hit, physics did not
    std::uint64_t physicsOnly = 0;   ///< physics hit, the 2001 path did not
};

bool RayAuditEnabled();
void SetRayAudit(bool enabled);
RayAuditStats GetRayAuditStats();
void ResetRayAudit();

/// Called from the projectile path with the segment it just tested and what the
/// legacy query concluded.
void AuditProjectileSegment(Vector3Par from, Vector3Par to, bool legacyHit);

} // namespace Poseidon::Dev
