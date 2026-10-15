// SPDX-License-Identifier: GPL-3.0-or-later
// RFG-157: see EnfusionDoors.hpp.

#include <Poseidon/World/Terrain/EnfusionDoors.hpp>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float kCell = 16.0f;
constexpr float kSecondsToSwing = 1.0f;

float SwingSign()
{
    // POSEIDON_ENFUSION_DOOR_SIGN=-1 turns every door the other way: kept for verifying the
    // Enfusion yaw convention against the original on screen.
    static const float sign = []
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_DOOR_SIGN");
        return env && std::atof(env) < 0.0 ? -1.0f : 1.0f;
    }();
    return sign;
}

float Smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

bool EnfusionDoorRegistry::Enabled()
{
    static const bool enabled = []
    {
        const char* env = std::getenv("POSEIDON_ENFUSION_DOORS");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void EnfusionDoorRegistry::Clear()
{
    _specs.clear();
    _doors.clear();
    _members.clear();
    _memberOf.clear();
    _cells.clear();
    _animating.clear();
    _nonDefault = 0;
}

int64_t EnfusionDoorRegistry::CellKey(float x, float z)
{
    const int64_t cx = static_cast<int64_t>(std::floor(x / kCell));
    const int64_t cz = static_cast<int64_t>(std::floor(z / kCell));
    return (cx << 32) ^ (cz & 0xffffffffll);
}

uint32_t EnfusionDoorRegistry::AddSpec(const Spec& spec)
{
    for (size_t i = 0; i < _specs.size(); ++i)
    {
        const Spec& s = _specs[i];
        if (s.angleDeg == spec.angleDeg && s.radius == spec.radius && s.action[0] == spec.action[0] &&
            s.action[1] == spec.action[1] && s.action[2] == spec.action[2])
            return static_cast<uint32_t>(i);
    }
    _specs.push_back(spec);
    return static_cast<uint32_t>(_specs.size() - 1);
}

int EnfusionDoorRegistry::AddDoor(uint32_t spec, int leafId, const Matrix4& closed)
{
    if (spec >= _specs.size() || leafId < 0)
        return -1;
    Door door;
    door.leafId = leafId;
    door.spec = spec;
    door.closed = closed;
    const int index = static_cast<int>(_doors.size());
    _doors.push_back(std::move(door));
    AddMember(index, leafId, closed);
    _cells[CellKey(closed.Position().X(), closed.Position().Z())].push_back(static_cast<uint32_t>(index));
    return index;
}

void EnfusionDoorRegistry::AddMember(int door, int id, const Matrix4& closed)
{
    if (door < 0 || static_cast<size_t>(door) >= _doors.size() || id < 0)
        return;
    Member member;
    member.id = id;
    member.door = static_cast<uint32_t>(door);
    member.closed = closed;
    const uint32_t index = static_cast<uint32_t>(_members.size());
    _members.push_back(member);
    _doors[static_cast<size_t>(door)].members.push_back(index);
    _memberOf[id] = index;
}

Matrix4 EnfusionDoorRegistry::FrameAt(const Door& door, const Matrix4& closed, float phase) const
{
    if (phase <= 0.0f)
        return closed;
    const Spec& spec = _specs[door.spec];
    // Enfusion yaw +theta is the engine's MRotationY(-theta) (RFG-068).
    const float angle = -SwingSign() * spec.angleDeg * Smooth(phase) * (3.14159265358979f / 180.0f);
    const Matrix3& basis = door.closed.Orientation();
    const Matrix3 turn = basis * Matrix3(MRotationY, angle) * basis.InverseScaled();
    const Vector3 hinge = door.closed.Position();
    Matrix4 out;
    out.SetOrientation(turn * closed.Orientation());
    out.SetPosition(hinge + turn * (closed.Position() - hinge));
    return out;
}

void EnfusionDoorRegistry::MoveMembers(const Door& door, Landscape& land) const
{
    for (const uint32_t m : door.members)
    {
        const Member& member = _members[m];
        Object* obj = land.GetObject(member.id);
        if (!obj)
            continue; // not resident: AdjustCreateTransform places it when it comes back
        obj->Move(FrameAt(door, member.closed, door.phase));
        if (GScene)
            GScene->GetShadowCache().ShadowChanged(obj);
    }
}

bool EnfusionDoorRegistry::IsOpen(int door) const
{
    return door >= 0 && static_cast<size_t>(door) < _doors.size() && _doors[static_cast<size_t>(door)].target;
}

float EnfusionDoorRegistry::Phase(int door) const
{
    return door >= 0 && static_cast<size_t>(door) < _doors.size() ? _doors[static_cast<size_t>(door)].phase : 0.0f;
}

float EnfusionDoorRegistry::AngleDeg(int door) const
{
    return door >= 0 && static_cast<size_t>(door) < _doors.size()
               ? _specs[_doors[static_cast<size_t>(door)].spec].angleDeg
               : 0.0f;
}

int EnfusionDoorRegistry::LeafId(int door) const
{
    return door >= 0 && static_cast<size_t>(door) < _doors.size() ? _doors[static_cast<size_t>(door)].leafId : -1;
}

Vector3 EnfusionDoorRegistry::Hinge(int door) const
{
    return door >= 0 && static_cast<size_t>(door) < _doors.size()
               ? _doors[static_cast<size_t>(door)].closed.Position()
               : Vector3(0.0f, 0.0f, 0.0f);
}

Vector3 EnfusionDoorRegistry::ActionPoint(int door) const
{
    if (door < 0 || static_cast<size_t>(door) >= _doors.size())
        return Vector3(0.0f, 0.0f, 0.0f);
    const Door& d = _doors[static_cast<size_t>(door)];
    const Spec& spec = _specs[d.spec];
    const Matrix4 frame = FrameAt(d, d.closed, d.phase);
    return frame.Position() + frame.Orientation() * Vector3(spec.action[0], spec.action[1], spec.action[2]);
}

int EnfusionDoorRegistry::FindForAction(Vector3Par pos, Vector3* actionPoint) const
{
    int best = -1;
    float bestDist2 = 1e30f;
    const int64_t cx = static_cast<int64_t>(std::floor(pos.X() / kCell));
    const int64_t cz = static_cast<int64_t>(std::floor(pos.Z() / kCell));
    for (int64_t dz = -1; dz <= 1; ++dz)
        for (int64_t dx = -1; dx <= 1; ++dx)
        {
            const auto cell = _cells.find(((cx + dx) << 32) ^ ((cz + dz) & 0xffffffffll));
            if (cell == _cells.end())
                continue;
            for (const uint32_t index : cell->second)
            {
                const Door& d = _doors[index];
                const Vector3 point = ActionPoint(static_cast<int>(index));
                const float dy = std::fabs(point.Y() - pos.Y());
                if (dy > 1.6f)
                    continue; // another storey
                const float reach = std::max(_specs[d.spec].radius, 0.65f) + 1.0f;
                const float dx2 = point.X() - pos.X();
                const float dz2 = point.Z() - pos.Z();
                const float dist2 = dx2 * dx2 + dz2 * dz2 + dy * dy * 0.25f;
                if (dist2 > reach * reach || dist2 >= bestDist2)
                    continue;
                best = static_cast<int>(index);
                bestDist2 = dist2;
                if (actionPoint)
                    *actionPoint = point;
            }
        }
    return best;
}

int EnfusionDoorRegistry::FindNearest(Vector3Par pos, float radius) const
{
    int best = -1;
    float bestDist2 = radius * radius;
    const int reachCells = std::max(1, static_cast<int>(std::ceil(radius / kCell)));
    const int64_t cx = static_cast<int64_t>(std::floor(pos.X() / kCell));
    const int64_t cz = static_cast<int64_t>(std::floor(pos.Z() / kCell));
    for (int64_t dz = -reachCells; dz <= reachCells; ++dz)
        for (int64_t dx = -reachCells; dx <= reachCells; ++dx)
        {
            const auto cell = _cells.find(((cx + dx) << 32) ^ ((cz + dz) & 0xffffffffll));
            if (cell == _cells.end())
                continue;
            for (const uint32_t index : cell->second)
            {
                const Vector3 hinge = _doors[index].closed.Position();
                const float ex = hinge.X() - pos.X();
                const float ez = hinge.Z() - pos.Z();
                const float dist2 = ex * ex + ez * ez;
                if (dist2 < bestDist2)
                {
                    best = static_cast<int>(index);
                    bestDist2 = dist2;
                }
            }
        }
    return best;
}

void EnfusionDoorRegistry::RequestState(int door, bool open)
{
    if (door < 0 || static_cast<size_t>(door) >= _doors.size())
        return;
    Door& d = _doors[static_cast<size_t>(door)];
    if (d.target == open)
        return;
    d.target = open;
    if (std::find(_animating.begin(), _animating.end(), static_cast<uint32_t>(door)) == _animating.end())
        _animating.push_back(static_cast<uint32_t>(door));
    if (open && d.phase <= 0.0f)
        ++_nonDefault;
}

void EnfusionDoorRegistry::Simulate(float deltaT, Landscape& land)
{
    if (_animating.empty())
        return;
    const float step = std::clamp(deltaT, 0.0f, 0.1f) / kSecondsToSwing;
    for (size_t i = 0; i < _animating.size();)
    {
        Door& d = _doors[_animating[i]];
        const float before = d.phase;
        d.phase = d.target ? std::min(1.0f, d.phase + step) : std::max(0.0f, d.phase - step);
        if (d.phase != before)
            MoveMembers(d, land);
        const bool settled = d.target ? d.phase >= 1.0f : d.phase <= 0.0f;
        if (settled)
        {
            if (!d.target && before > 0.0f && _nonDefault > 0)
                --_nonDefault;
            _animating[i] = _animating.back();
            _animating.pop_back();
            continue;
        }
        ++i;
    }
}

bool EnfusionDoorRegistry::AdjustCreateTransform(int id, Matrix4& transform) const
{
    if (_nonDefault == 0 && _animating.empty())
        return false;
    const auto found = _memberOf.find(id);
    if (found == _memberOf.end())
        return false;
    const Member& member = _members[found->second];
    const Door& d = _doors[member.door];
    if (d.phase <= 0.0f)
        return false;
    transform = FrameAt(d, member.closed, d.phase);
    return true;
}

} // namespace Poseidon
