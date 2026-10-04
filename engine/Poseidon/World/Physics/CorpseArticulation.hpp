#pragma once

#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <array>
#include <cmath>
#include <vector>

namespace Poseidon
{
enum class CorpseSolverState { Simulating, Frozen };

// Solver handles and validated bounds belong to one corpse. Retiring the solver
// is separate from discarding Man's owned affine pose. No backend handle is saved.
struct CorpseArticulation
{
    CorpseArticulation() = default;
    CorpseArticulation(const CorpseArticulation&) = delete;
    CorpseArticulation& operator=(const CorpseArticulation&) = delete;
    Physics::PhysicsWorld* world = nullptr;
    std::uint64_t epoch = 0;
    int collisionFamily = -1;
    std::array<Physics::BodyId, 11> bodies{};
    std::array<Physics::JointId, 10> joints{};
    std::array<Matrix4, 11> initial{};
    Matrix4 object = MIdentity;
    std::vector<int> boneParts;
    float minimumClearance = 0;
    int updates = 0, hingeCount = 0;
    float maximumTravel = 0;
    Vector3 minimum = VZero, maximum = VZero;
    float radius = 0;
    CorpseSolverState state = CorpseSolverState::Simulating;
    bool Ready() const
    {
        return world && Physics::GetPhysicsWorld() == world && world->Generation() == epoch;
    }
    void ReleaseHandles()
    {
        if (Ready())
        {
            for (auto joint : joints) world->RemoveJoint(joint);
            for (auto body : bodies) world->Remove(body);
        }
        joints = {}; bodies = {};
    }
    bool Freeze()
    {
        if (!Ready() || state != CorpseSolverState::Simulating || updates <= 0 ||
            !minimum.IsFinite() || !maximum.IsFinite() || !std::isfinite(radius) || radius <= 0 ||
            minimum.X() > maximum.X() || minimum.Y() > maximum.Y() || minimum.Z() > maximum.Z()) return false;
        for (auto body : bodies) if (!body.IsValid()) return false;
        for (auto joint : joints) if (!joint.IsValid()) return false;
        ReleaseHandles();
        state = CorpseSolverState::Frozen;
        return true;
    }
    ~CorpseArticulation() { ReleaseHandles(); }
};
}
