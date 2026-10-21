// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-157 -- doors of native Enfusion (Arma Reforger) buildings that open and close.
//
// A Reforger door leaf is a part hung off its door frame's socket bone (RFG-064). Its prefab
// carries a `DoorComponent` with `AngleRange` (degrees; the sign picks the swing) and
// `DoorAnimationType WholeEntity`: the whole leaf turns about its own origin -- the hinge --
// and its own up axis. The loader keeps every part a plain static object; this registry is
// the only thing that knows which of them are doors.
//
// Nothing is created per door. A door is its leaf object id, the leaf's closed frame and the
// ids and closed frames of the parts that ride on it (its glass, its handle). Only a door
// that is moving does any work per frame: its resident parts are moved with Object::Move,
// which also patches their GPU-driven instance (SceneObjectMoved) and their place in the
// landscape grid, and collision follows the live transform. A part the camera evicted and
// re-admits while its door is open comes back open (AdjustCreateTransform, called from
// Landscape::ObjectCreateFromShape).
//
// POSEIDON_ENFUSION_DOORS=0 keeps the loader from registering anything.

#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class Landscape;

class EnfusionDoorRegistry
{
  public:
    struct Spec
    {
        float angleDeg = 90.0f;                  //!< Enfusion AngleRange
        float action[3] = {1.2f, 1.1f, 0.0f};    //!< action point, leaf space
        float radius = 0.65f;                    //!< action reach around that point
    };

    //! True unless POSEIDON_ENFUSION_DOORS=0.
    static bool Enabled();

    void Clear();
    uint32_t AddSpec(const Spec& spec);
    //! A leaf, closed. Returns the door index.
    int AddDoor(uint32_t spec, int leafId, const Matrix4& closed);
    //! A part riding on door `door` (its glass, handle, ...), closed.
    void AddMember(int door, int id, const Matrix4& closed);

    size_t Count() const { return _doors.size(); }
    size_t MemberCount() const { return _members.size(); }
    bool AnyNonDefault() const { return _nonDefault > 0; }

    //! The door nearest `pos` whose action point is within reach, or -1.
    int FindForAction(Vector3Par pos, Vector3* actionPoint = nullptr) const;
    //! The door whose leaf (or hinge) is nearest `pos` within `radius`, or -1 (test commands).
    int FindNearest(Vector3Par pos, float radius) const;

    bool IsOpen(int door) const;         //!< the state it is going to
    float Phase(int door) const;         //!< 0 closed .. 1 open
    float AngleDeg(int door) const;
    int LeafId(int door) const;
    Vector3 Hinge(int door) const;
    Vector3 ActionPoint(int door) const; //!< at the door's current phase
    void RequestState(int door, bool open);

    //! Advances moving doors and moves their resident parts.
    void Simulate(float deltaT, Landscape& land);
    //! For an object being created: its frame at its door's current phase. False = not a door
    //! part, or its door is closed.
    bool AdjustCreateTransform(int id, Matrix4& transform) const;

  private:
    struct Door
    {
        int leafId = -1;
        uint32_t spec = 0;
        Matrix4 closed;
        std::vector<uint32_t> members; //!< indices into _members (the leaf first)
        float phase = 0.0f;
        bool target = false;
    };
    struct Member
    {
        int id = -1;
        uint32_t door = 0;
        Matrix4 closed;
    };

    static int64_t CellKey(float x, float z);
    Matrix4 FrameAt(const Door& door, const Matrix4& closed, float phase) const;
    void MoveMembers(const Door& door, Landscape& land) const;

    std::vector<Spec> _specs;
    std::vector<Door> _doors;
    std::vector<Member> _members;
    std::unordered_map<int, uint32_t> _memberOf;               //!< object id -> member index
    std::unordered_map<int64_t, std::vector<uint32_t>> _cells; //!< 16 m cells -> doors
    std::vector<uint32_t> _animating;
    uint32_t _nonDefault = 0;
};

} // namespace Poseidon
