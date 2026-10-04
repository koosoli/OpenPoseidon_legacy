#pragma once

#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
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
    // Admitted runtime recipes, never backend handles or a guessed rest pose.
    std::array<std::vector<Vector3>,11> retainedHulls;
    std::array<float,11> retainedMasses{};
    std::array<Matrix4,11> retainedTransforms{};
    std::array<Physics::ArticulatedJointDef,10> retainedJoints{};
    std::uint64_t terrainSerial = 0;
    bool wakeRecipe = false;
    unsigned impulseEvents = 0, wakeCount = 0, localTransfers = 0;
    int lastImpactPart = -1;
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
    bool Wake(int family)
    {
        if (!wakeRecipe || !Ready() || state != CorpseSolverState::Frozen ||
            family > -2 || family < -5 || terrainSerial != world->TerrainMutationSerial() ||
            !world->GetStats().terrainRegistered) return false;
        for (int part=0;part<11;++part)
        {
            if (!retainedTransforms[part].IsFinite() || retainedHulls[part].size()<4 ||
                retainedHulls[part].size()>4096 || !std::isfinite(retainedMasses[part]) || retainedMasses[part]<=0)
                return false;
            for (const auto& p:retainedHulls[part]) if (!p.IsFinite() || p.Size()>4) return false;
        }
        const int pairs[][2]={{0,1},{1,2},{1,3},{3,4},{1,5},{5,6},{0,7},{7,8},{0,9},{9,10}};
        auto fail=[&] { ReleaseHandles(); return false; };
        for (int part=0;part<11;++part)
        {
            Physics::ConvexPiece piece{retainedHulls[part].data(),int(retainedHulls[part].size()),Physics::ColliderFlags::Solid};
            Physics::ArticulatedInitialMotion motion; motion.collisionFamily=family;
            bodies[part]=world->SpawnArticulatedPiece(piece,retainedTransforms[part],retainedMasses[part],motion);
            Physics::BodyMotion readback;
            if (!bodies[part].IsValid() || !world->GetBodyMotion(bodies[part],readback)) return fail();
            Matrix4 actual=MIdentity; actual.SetDirectionAside(readback.axisX); actual.SetDirectionUp(readback.axisY);
            actual.SetDirection(readback.axisZ); actual.SetPosition(readback.position);
            if (!actual.IsFinite() || (actual.Position()-retainedTransforms[part].Position()).Size()>.0001f) return fail();
            initial[part]=actual;
        }
        for (int j=0;j<10;++j)
        {
            auto definition=retainedJoints[j];
            if (!definition.firstFrame.IsFinite() || !definition.secondFrame.IsFinite()) return fail();
            definition.first=bodies[pairs[j][0]]; definition.second=bodies[pairs[j][1]];
            joints[j]=world->AddArticulatedJoint(definition);
            if (!joints[j].IsValid()) return fail();
        }
        collisionFamily=family; maximumTravel=0; state=CorpseSolverState::Simulating;
        return true;
    }
    // Aggregate torque identifies an impact LINE (r x F), not its depth along
    // the projectile. Choose the admitted anatomical body closest to that line.
    int ImpactPart(Vector3Par center,Vector3Par force,Vector3Par torque,Vector3& point) const
    {
        if (!wakeRecipe || !center.IsFinite() || !force.IsFinite() || !torque.IsFinite() || force.SquareSize()<1e-10f) return -1;
        const Vector3 direction=force.Normalized(), origin=center+force.CrossProduct(torque)/force.SquareSize();
        float best=4; int result=-1;
        for(int part=0;part<11;++part)
        {
            const Vector3 c=retainedTransforms[part].Position();
            const Vector3 onLine=origin+direction*((c-origin)*direction);
            const float distance=(c-onLine).SquareSize();
            if(std::isfinite(distance) && distance<best) { best=distance; result=part; point=onLine; }
        }
        return result;
    }
    // Same physical transfer for a just-woken frozen recipe and an active rig.
    // Return local part, -1 for aggregate safety fallback, -2 for refusal.
    int TransferImpulse(Vector3Par center,Vector3Par force,Vector3Par torque,
        Vector3Par linear,Vector3Par angular,float rigid,float& localLinear,float& localAngular)
    {
        localLinear=localAngular=-1;
        if(!Ready() || state!=CorpseSolverState::Simulating || !linear.IsFinite() || !angular.IsFinite() ||
            linear.Size()>15 || angular.Size()>20 || !std::isfinite(rigid) || rigid<=0) return -2;
        std::array<Physics::BodyMotion,11> motions;
        for(int p=0;p<11;++p) if(!world->GetBodyMotion(bodies[p],motions[p])) return -2;
        Vector3 point; const int part=ImpactPart(center,force,torque,point);
        const Vector3 impulse=force*rigid;
        if(part>=0 && impulse.IsFinite() && impulse.Size()/retainedMasses[part]<=15)
        {
            world->ApplyImpulse(bodies[part],point,impulse);
            Physics::BodyMotion applied;
            if(world->GetBodyMotion(bodies[part],applied) && applied.linearVelocity.IsFinite() && applied.angularVelocity.IsFinite())
            {
                localLinear=applied.linearVelocity.Size(); localAngular=applied.angularVelocity.Size();
                if(localLinear<=15 && localAngular<=20)
                { ++localTransfers; lastImpactPart=part; return part; }
            }
            if(!world->SetArticulatedMotion(bodies[part],motions[part].linearVelocity,motions[part].angularVelocity)) return -2;
        }
        for(int p=0;p<11;++p)
            if(!world->SetArticulatedMotion(bodies[p],motions[p].linearVelocity+linear+
                angular.CrossProduct(motions[p].position-center),motions[p].angularVelocity+angular)) return -2;
        return -1;
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
