// SPDX-License-Identifier: GPL-3.0-or-later
// RFG-158: see EnfusionGlass.hpp.

#include <Poseidon/World/Terrain/EnfusionGlass.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp> // _modernObjectLightOwners holds Ref<LightPoint>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp> // Ref<LightPoint> in _modernObjectLightOwners
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Audio/SoundScene.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Random/randomGen.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float kCell = 8.0f;

int64_t CellOf(float v)
{
    return static_cast<int64_t>(std::floor(v / kCell));
}
} // namespace

bool EnfusionGlassRegistry::Enabled()
{
    static const bool enabled = []
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_GLASS_BREAK");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void EnfusionGlassRegistry::Clear()
{
    _panes.clear();
    _cells.clear();
    _broken = 0;
}

void EnfusionGlassRegistry::AddPane(uint32_t placement, int id, uint32_t brokenModel, Vector3Par position)
{
    Pane pane;
    pane.placement = placement;
    pane.id = id;
    pane.brokenModel = brokenModel;
    pane.position[0] = position.X();
    pane.position[1] = position.Y();
    pane.position[2] = position.Z();
    _cells[Key(CellOf(position.X()), CellOf(position.Z()))].push_back(static_cast<uint32_t>(_panes.size()));
    _panes.push_back(pane);
}

void EnfusionGlassRegistry::MarkBroken(uint32_t index)
{
    if (index < _panes.size() && !_panes[index].broken)
    {
        _panes[index].broken = true;
        ++_broken;
    }
}

void EnfusionGlassRegistry::Candidates(Vector3Par from, Vector3Par to, float pad, std::vector<uint32_t>& out) const
{
    out.clear();
    if (_panes.empty())
        return;
    const int64_t x0 = CellOf(std::min(from.X(), to.X()) - pad), x1 = CellOf(std::max(from.X(), to.X()) + pad);
    const int64_t z0 = CellOf(std::min(from.Z(), to.Z()) - pad), z1 = CellOf(std::max(from.Z(), to.Z()) + pad);
    // A tick of a bullet is a few tens of metres: a handful of cells. Anything longer (a
    // teleport, a first tick) is not worth walking cell by cell.
    if ((x1 - x0 + 1) * (z1 - z0 + 1) > 400)
        return;
    for (int64_t cz = z0; cz <= z1; ++cz)
        for (int64_t cx = x0; cx <= x1; ++cx)
        {
            const auto cell = _cells.find(Key(cx, cz));
            if (cell == _cells.end())
                continue;
            for (const uint32_t index : cell->second)
                if (!_panes[index].broken)
                    out.push_back(index);
        }
}

void EnfusionGlassRegistry::InRadius(Vector3Par centre, float radius, std::vector<uint32_t>& out) const
{
    out.clear();
    if (_panes.empty() || radius <= 0.0f)
        return;
    const int64_t x0 = CellOf(centre.X() - radius), x1 = CellOf(centre.X() + radius);
    const int64_t z0 = CellOf(centre.Z() - radius), z1 = CellOf(centre.Z() + radius);
    for (int64_t cz = z0; cz <= z1; ++cz)
        for (int64_t cx = x0; cx <= x1; ++cx)
        {
            const auto cell = _cells.find(Key(cx, cz));
            if (cell == _cells.end())
                continue;
            for (const uint32_t index : cell->second)
            {
                const Pane& pane = _panes[index];
                if (pane.broken)
                    continue;
                const float dx = pane.position[0] - centre.X();
                const float dy = pane.position[1] - centre.Y();
                const float dz = pane.position[2] - centre.Z();
                if (dx * dx + dy * dy + dz * dz <= radius * radius)
                    out.push_back(index);
            }
        }
}

int EnfusionGlassRegistry::Nearest(Vector3Par pos, float radius) const
{
    int best = -1;
    float bestDist2 = radius * radius;
    const int64_t x0 = CellOf(pos.X() - radius), x1 = CellOf(pos.X() + radius);
    const int64_t z0 = CellOf(pos.Z() - radius), z1 = CellOf(pos.Z() + radius);
    for (int64_t cz = z0; cz <= z1; ++cz)
        for (int64_t cx = x0; cx <= x1; ++cx)
        {
            const auto cell = _cells.find(Key(cx, cz));
            if (cell == _cells.end())
                continue;
            for (const uint32_t index : cell->second)
            {
                const Pane& pane = _panes[index];
                const float dx = pane.position[0] - pos.X();
                const float dy = pane.position[1] - pos.Y();
                const float dz = pane.position[2] - pos.Z();
                const float dist2 = dx * dx + dy * dy + dz * dz;
                if (dist2 < bestDist2)
                {
                    bestDist2 = dist2;
                    best = static_cast<int>(index);
                }
            }
        }
    return best;
}

// --- Landscape side ----------------------------------------------------------------------------

namespace
{
//! Does the segment a..b (world) pass through the object's box, padded on its thin axis?
//! Returns the hit parameter in [0, 1], or a negative value.
float SegmentThroughPane(const Object& obj, Vector3Par a, Vector3Par b)
{
    const LODShape* shape = obj.GetShape();
    if (!shape)
        return -1.0f;
    Vector3 lo = shape->Min();
    Vector3 hi = shape->Max();
    // A pane is a few millimetres thick: pad its thinnest axis so a fast round's tick, which
    // samples the segment exactly, cannot slip between the faces.
    int thin = 0;
    float extent[3] = {hi.X() - lo.X(), hi.Y() - lo.Y(), hi.Z() - lo.Z()};
    for (int i = 1; i < 3; ++i)
        if (extent[i] < extent[thin])
            thin = i;
    float pad[3] = {0.005f, 0.005f, 0.005f};
    pad[thin] = 0.04f;
    lo = Vector3(lo.X() - pad[0], lo.Y() - pad[1], lo.Z() - pad[2]);
    hi = Vector3(hi.X() + pad[0], hi.Y() + pad[1], hi.Z() + pad[2]);
    const Matrix4 inv = obj.Transform().InverseScaled();
    const Vector3 p = inv.FastTransform(a);
    const Vector3 q = inv.FastTransform(b);
    float t0 = 0.0f, t1 = 1.0f;
    const float ps[3] = {p.X(), p.Y(), p.Z()};
    const float qs[3] = {q.X(), q.Y(), q.Z()};
    const float los[3] = {lo.X(), lo.Y(), lo.Z()};
    const float his[3] = {hi.X(), hi.Y(), hi.Z()};
    for (int i = 0; i < 3; ++i)
    {
        const float d = qs[i] - ps[i];
        if (std::fabs(d) < 1e-7f)
        {
            if (ps[i] < los[i] || ps[i] > his[i])
                return -1.0f;
            continue;
        }
        float ta = (los[i] - ps[i]) / d;
        float tb = (his[i] - ps[i]) / d;
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return -1.0f;
    }
    return t0;
}

void PlayBreakSound(Vector3Par pos, const AmmoType* type)
{
    static const char* overrideName = std::getenv("POSEIDON_ENFUSION_GLASS_SOUND");
    if (!GSoundScene)
        return;
    IWave* sound = nullptr;
    if (overrideName && *overrideName)
        sound = GSoundScene->OpenAndPlayOnce(overrideName, pos, VZero, 1.0f, 1.0f);
    else if (type)
    {
        // No Reforger audio is decoded here: the round's own building-hit sound, pitched up,
        // stands in for the shatter.
        // POSITIONAL random (by where it broke): the sequential stream is part of the simulation
        // hash, and only resident panes break, so drawing from it here would diverge replays.
        const SoundPars& pars = type->_hitBuilding.SelectSound(
            GRandGen.RandomValue(static_cast<int>(std::floor(pos.X() * 4.0f)), static_cast<int>(std::floor(pos.Z() * 4.0f)),
                                 static_cast<int>(std::floor(pos.Y() * 4.0f))));
        if (pars.name.GetLength() > 0)
            sound = GSoundScene->OpenAndPlayOnce(pars.name, pos, VZero, pars.vol, pars.freq * 1.6f);
    }
    if (sound)
    {
        GSoundScene->SimulateSpeedOfSound(sound);
        GSoundScene->AddSound(sound);
    }
}
} // namespace

bool Landscape::BreakNativeGlassPane(uint32_t index, const AmmoType* type)
{
    if (index >= _enfusionGlass.Count())
        return false;
    EnfusionGlassRegistry::Pane& pane = _enfusionGlass.Get(index);
    if (pane.broken)
        return false;
    _enfusionGlass.MarkBroken(index);
    const Vector3 where(pane.position[0], pane.position[1], pane.position[2]);
    static const bool fx = []
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_GLASS_FX");
        return !(env && env[0] == '0');
    }();
    if (fx)
        PlayBreakSound(where, type);

    if (pane.placement < _modernObjectPlacements.size())
    {
        ModernObjectPlacement& placement = _modernObjectPlacements[pane.placement];
        if (pane.brokenModel < _modernObjectModels.size())
            placement.modelIndex = pane.brokenModel;
        if (Object* object = placement.resident)
        {
            Ref<Object> keepAlive = object;
            placement.resident = nullptr;
            RemoveObject(keepAlive);
            _objectIds.erase(placement.id);
            _modernObjectLightOwners.erase(placement.id);
            if (_modernResidentObjectCount > 0)
                --_modernResidentObjectCount;
        }
        // Hand the cell back to the stream: its next pass admits the pane again, now as the
        // broken model (cells already admitted are not rescanned otherwise).
        if (_landRange > 0 && !_modernObjectCellActive.empty())
        {
            const int cx = std::clamp(static_cast<int>(std::floor(placement.rows[9] * _invLandGrid)), 0, _landRange - 1);
            const int cz = std::clamp(static_cast<int>(std::floor(placement.rows[11] * _invLandGrid)), 0, _landRange - 1);
            const size_t cell = static_cast<size_t>(cz) * _landRange + cx;
            if (cell < _modernObjectCellActive.size())
            {
                _modernObjectCellActive[cell] = 0;
                if (cell < _modernObjectCellCursor.size())
                    _modernObjectCellCursor[cell] = 0;
            }
        }
        return true;
    }
    // A pane placed by the radius path (POSEIDON_REFORGER_STREAM=0): no stream to re-admit it
    // as its broken phase, so it simply goes.
    if (Object* object = GetObject(pane.id))
    {
        Ref<Object> keepAlive = object;
        RemoveObject(keepAlive);
        _objectIds.erase(pane.id);
    }
    return true;
}

int Landscape::BreakNativeGlassAlongSegment(Vector3Par from, Vector3Par to, const AmmoType* type)
{
    if (_enfusionGlass.Count() == 0 || _enfusionGlass.BrokenCount() >= _enfusionGlass.Count())
        return 0;
    std::vector<uint32_t> candidates;
    _enfusionGlass.Candidates(from, to, 2.0f, candidates);
    int broken = 0;
    for (const uint32_t index : candidates)
    {
        const EnfusionGlassRegistry::Pane& pane = _enfusionGlass.Get(index);
        Object* object = pane.placement < _modernObjectPlacements.size()
                             ? static_cast<Object*>(_modernObjectPlacements[pane.placement].resident)
                             : GetObject(pane.id);
        if (!object)
            continue; // not resident: nothing on screen to break
        if (SegmentThroughPane(*object, from, to) < 0.0f)
            continue;
        if (BreakNativeGlassPane(index, type))
            ++broken;
    }
    return broken;
}

int Landscape::BreakNativeGlassInRadius(Vector3Par centre, float radius, const AmmoType* type)
{
    if (_enfusionGlass.Count() == 0 || radius <= 0.0f)
        return 0;
    std::vector<uint32_t> candidates;
    _enfusionGlass.InRadius(centre, radius, candidates);
    if (candidates.size() > 256)
        candidates.resize(256);
    int broken = 0;
    bool first = true;
    for (const uint32_t index : candidates)
    {
        // One shatter sound for the whole blast, not one per pane.
        if (BreakNativeGlassPane(index, first ? type : nullptr))
            ++broken;
        first = false;
    }
    return broken;
}

} // namespace Poseidon
