// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

// Bullet penetration through materials.
//
// WHY IT IS NOT A BOOLEAN. The obvious model is a per-material passthrough
// flag: a shot either ignores a surface entirely or stops at it. Under that
// model glass is passthrough and brick is not, and a pane and a wall cost a
// bullet exactly the same -- nothing, or everything. Thickness never enters,
// so neither does the angle a round takes through a wall.
//
// This measures instead. A shot that enters a material
// measures how far it must travel INSIDE it (PhysicsWorld::MeasureThickness,
// which needs the physics collider set and is why this could not be written
// before PHY-010), loses energy in proportion, and comes out the far side slower
// -- or does not come out. So a plank slows a rifle round, a brick wall stops it,
// and a glancing shot through the same wall is stopped where a square one is not,
// because it travels further inside.
//
// ON THE NUMBERS. Penetration is a deep field and the honest figures are
// classified, proprietary or absent. What follows is a SINGLE-PARAMETER model
// with openly estimated constants, in the same spirit as the unsourced half of
// docs/ballistics/cwa-airfriction.cpp: ordered correctly by material, far better
// than a boolean, and not to be quoted as data. Anything claiming otherwise is
// inventing evidence.

namespace Poseidon::Penetration
{

/// Energy a material takes from a projectile per metre travelled through it, per
/// square metre of frontal area -- so J/m^3, which is the unit a resistance
/// naturally has. Multiply by thickness and by the round's cross-section.
struct Material
{
    const char* name = "default";
    /// J/m^3. ESTIMATED -- see the file header.
    float resistance = 0.0f;
    /// True for materials a shot passes through with almost no loss, so the
    /// legacy behaviour (glass, foliage, cloth) is reproduced exactly rather than
    /// approximated by a very small resistance.
    bool alwaysPasses = false;
};

/// Looks a material up by texture path. OFP identifies surfaces by texture rather
/// than by a named material, so the mapping is by path fragment -- the stock
/// corpus has no material declarations to read.
const Material& Lookup(const char* texturePath);

struct Result
{
    /// Speed on the far side, m/s. Zero when the round is stopped.
    float exitSpeed = 0.0f;
    /// True when the round came out the other side.
    bool  penetrated = false;
    /// Energy taken by the material, J. For damage and diagnostics.
    float energyLost = 0.0f;
};

/// `speed` m/s, `mass` kg, `calibre` m, `thickness` m along the shot's own line.
Result Compute(const Material& material, float speed, float mass, float calibre, float thickness);

/// Master switch. OFF by default: this changes what bullets do, and no test can
/// judge that.
bool  Enabled();
void  SetEnabled(bool enabled);
/// Scales every resistance at once, so the whole table can be made harder or
/// softer without editing constants that are estimates anyway.
float ResistanceScale();
void  SetResistanceScale(float scale);

} // namespace Poseidon::Penetration
