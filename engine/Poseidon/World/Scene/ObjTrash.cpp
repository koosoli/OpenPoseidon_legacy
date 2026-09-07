
#include <Poseidon/World/Scene/Object.hpp>
#include <cstdlib>
#include <algorithm>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/V3Quads.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon::Foundation
{
template class Ref<ConvexComponent>;
} // namespace Poseidon::Foundation

namespace Poseidon
{
DestructionLook& GDestructionLook()
{
    static DestructionLook look = []
    {
        DestructionLook l;
        if (const char* v = std::getenv("POSEIDON_DESTRUCT_LOOK"))
            l.enabled = !(*v == '0');
        if (const char* v = std::getenv("POSEIDON_DESTRUCT_SQUASH"))
            l.squash = std::clamp(static_cast<float>(std::atof(v)), 1.0f, 6.0f);
        return l;
    }();
    return look;
}
} // namespace Poseidon

void Shape::MakeDestroyed(float yOffset, Vector3Par bCenter, int seed, float coef, float squash)
{
    // hold all points on ground level
    int i;
    for (i = 0; i < NPos(); i++)
    {
        V3& pos = SetPos(i);
        float yFactor = pos[1] + yOffset; // old Y coordinate
        saturateMax(yFactor, 0.1);
        int iSeed = toIntFloor(pos[0] + pos[1] + pos[2]) & 0x3ff + seed;
        // randomize height above ground level
        // DST-003: the 2001 term takes at most 20% of the height off, which is why a
        // wrecked house slumps and shears but never comes down. `squash` scales that term
        // and nothing else -- the lateral scatter below is left exactly as authored, because
        // scaling THAT shreds the walls sideways into spaghetti instead of collapsing them.
        float drop = GRandGen.RandomValue(iSeed) * 0.2 * squash;
        saturate(drop, 0.0f, 0.92f);
        pos[1] = ((1 - drop) * yFactor + (GRandGen.RandomValue(iSeed + 1) - 0.5) * 0.1 * coef - yOffset);
        // randomize position
        pos[0] += (GRandGen.RandomValue(iSeed + 2) - 0.5) * yFactor * 0.25 * coef;
        pos[2] += (GRandGen.RandomValue(iSeed + 3) - 0.5) * yFactor * 0.55 * coef;
        ClipFlags clip = Clip(i);
        clip &= ~ClipLandMask;
        clip |= ClipLandKeep;
        SetClip(i, clip);
    }
    CalculateHints();
    ClearOriginalPos();
    // normal recalculation necessary
    RecalculateNormals(true);
}

void Shape::MakeTreeDestroyed(float yOffset, Vector3Par bCenter, int seed, float coef)
{
    // hold all points on ground level
    int i;
    for (i = 0; i < NPos(); i++)
    {
        V3& pos = SetPos(i);
        float yFactor = pos[1] + yOffset; // old Y coordinate
        saturateMax(yFactor, 0.1);
        int iSeed = toIntFloor(pos[0] + pos[1] + pos[2]) & 0x3ff + seed;
        // randomize height above ground level
        pos[1] = ((GRandGen.RandomValue(iSeed) * 0.1 + 0.05) * yFactor +
                  (GRandGen.RandomValue(iSeed + 1) - 0.5) * 0.05 * coef - yOffset);
        // randomize position
        pos[0] += (GRandGen.RandomValue(iSeed + 2) - 0.5) * yFactor * 0.25 * coef;
        pos[2] += (GRandGen.RandomValue(iSeed + 3) - 0.5) * yFactor * 0.55 * coef;
        pos[0] += (1 - GRandGen.RandomValue(iSeed + 4) * 0.2 * coef) * yFactor;
        ClipFlags clip = Clip(i);
        clip &= ~ClipLandMask;
        clip |= ClipLandKeep;
        SetClip(i, clip);
    }
    CalculateHints();
    ClearOriginalPos();
    // normal recalculation necessary
    RecalculateNormals(true);
}

LODShape* LODShapeWithShadow::MakeDestroyed(float coef, float squash)
{
    if (!_destroyed)
    {
        _destroyed = new LODShape(*this);
        if (NLevels() > 0)
        {
            int seed = toIntFloor(GRandGen.RandomValue() * 2048);
            float yOffset = -Level(0)->Min().Y();
            for (int level = 0; level < NLevels(); level++)
            {
                Shape* shape = _destroyed->Level(level);
                if (shape)
                {
                    shape->MakeDestroyed(yOffset, _destroyed->BoundingCenter(), seed, coef, squash);
#if USE_QUADS
                    if (shape->PosQuad().Size() >= 0)
                    {
                        shape->ConvertToQArray();
                    }
#endif
                }
            }
        }
        _destroyed->OrSpecial(NoShadow);
        _destroyed->LockAutoCenter(true);
        _destroyed->CalculateMinMax(true);
        // _destroyed convex components not used
        _destroyed->_geomComponents->Clear();
    }
    return _destroyed;
}

LODShape* LODShapeWithShadow::MakeTreeDestroyed(float coef)
{
    if (!_destroyed)
    {
        _destroyed = new LODShape(*this);
        if (NLevels() > 0)
        {
            int seed = toIntFloor(GRandGen.RandomValue() * 2048);
            float yOffset = -Level(0)->Min().Y();
            for (int level = 0; level < NLevels(); level++)
            {
                Shape* shape = _destroyed->Level(level);
                if (shape)
                {
                    shape->MakeTreeDestroyed(yOffset, _destroyed->BoundingCenter(), seed, coef);
#if USE_QUADS
                    if (shape->PosQuad().Size() >= 0)
                    {
                        shape->ConvertToQArray();
                    }
#endif
                }
            }
        }
        // leave bounding center as it is

        _destroyed->OrSpecial(NoShadow);
        _destroyed->LockAutoCenter(true);
        _destroyed->CalculateMinMax(true);
        // object disappeared for collisions
        _destroyed->_geomComponents->Clear();
    }
    return _destroyed;
}
