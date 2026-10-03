// Malprave aiExposed (ported 2026-10-04, AI-MALPRAVE-01 item 9): seeing and aiming at the part of a man that shows.
//
// Off unless class CfgAIFork { exposedParts = 1; } (keys exposedRange, exposedSamples, exposedPartSize,
// exposedPartVisibility). An observer whose view of this man's aiming point (the chest) is mostly blocked samples
// other points of his body (his Hitpoints LOD) and can see -- and aim at (Person::ExposedAimPoint) -- a part that shows.
// After X-Ray's Feel::Vision: a random body point per check, kept while it stays visible. In Malprave this lived in
// the Cry of Fear monster file (Undead.cpp), which Oli does not have.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>

#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

namespace Poseidon
{
namespace
{
struct ExposedSettings
{
    bool enabled = false;  // exposedParts = 1
    float range = 100;     // exposedRange (m): no sampling further away
    int samples = 2;       // exposedSamples: body points tried per check (the last visible one first)
    float partSize = 0.5f; // exposedPartSize: a part's visible size as a share of the whole man's
    float partVis = 0.7f;  // exposedPartVisibility: a part counts this much of a full view (slower to spot)
};

const ExposedSettings& Exposed()
{
    static ExposedSettings s;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (const ParamEntry* c = Pars.FindEntry("CfgAIFork"))
        {
            auto num = [&](const char* n, float d)
            {
                const ParamEntry* e = c->FindEntry(n);
                return e ? (float)(*e) : d;
            };
            s.enabled = num("exposedParts", 0) != 0;
            s.range = floatMax(num("exposedRange", s.range), 1.0f);
            s.samples = (int)floatMax(num("exposedSamples", (float)s.samples), 1.0f);
            s.partSize = floatMax(num("exposedPartSize", s.partSize), 0.05f);
            s.partVis = floatMax(num("exposedPartVisibility", s.partVis), 0.0f);
        }
    }
    return s;
}
} // namespace

bool Person::ExposedAimPoint(Vector3& pos) const
{
    if (_exposedIdx < 0 || Glob.time - _exposedTime > 0.6f)
    {
        return false;
    }
    pos = _exposedPoint;
    return true;
}

float Man::ExposedVisibility(Vector3Par sensorPos, const Object* sensor, float objectSize, int isect)
{
    const ExposedSettings& cfg = Exposed();
    if (!cfg.enabled || !GLandscape || sensor == this)
    {
        return 0;
    }
    if (sensorPos.Distance2(Position()) > Square(cfg.range))
    {
        return 0;
    }
    Shape* hitShape = _shape->HitpointsLevel();
    const int n = hitShape ? hitShape->NPos() : 0;
    if (n <= 0)
    {
        return 0;
    }
    Animate(_shape->FindHitpoints());
    float best = 0;
    int bestIdx = -1;
    Vector3 bestPt = VZero;
    for (int k = 0; k < cfg.samples; k++)
    {
        int idx = (k == 0 && _exposedIdx >= 0 && _exposedIdx < n) ? _exposedIdx : toIntFloor(GRandGen.RandomValue() * n);
        saturate(idx, 0, n - 1);
        const Vector3 wp = PositionModelToWorld(hitShape->Pos(idx));
        const float v = GLandscape->Visible(sensorPos, wp, objectSize * cfg.partSize, sensor, this, (ObjIntersect)isect);
        if (v > best)
        {
            best = v;
            bestIdx = idx;
            bestPt = wp;
        }
    }
    Deanimate(_shape->FindHitpoints());
    if (best >= 0.5f)
    {
        _exposedPoint = bestPt;
        _exposedTime = Glob.time;
        _exposedIdx = bestIdx;
    }
    else
    {
        _exposedIdx = -1;
    }
    return best * cfg.partVis;
}
} // namespace Poseidon
