// PHY-010: does a physics world actually come up, take our geometry, and step?
//
// These assert the BOUNDARY, not Box3D. Box3D has its own tests; what nobody else
// checks is that our convex pieces survive the trip, that a refusal is counted
// rather than swallowed, and that a stale handle stays dead. Those are the three
// ways this layer can be wrong while looking fine.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/Dev/Diag/ShowcaseCastle.hpp>
#include <Poseidon/Dev/Diag/ShowcaseContact.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/World/Terrain/TerrainEditJournal.hpp>

#include <memory>
#include <cmath>
#include <vector>
#include <limits>
#include <chrono>
#include <cstdio>

using namespace Poseidon::Physics;

TEST_CASE("Native terrain-grid corners retain the landscape diagonal under Box3D ray casts", "[physics][articulation][automatic-terrain]")
{
    // A nonplanar cell distinguishes the landscape's 01--10 diagonal from
    // bilinear interpolation or a 00--11 diagonal. The two-metre intermediate
    // ridge also distinguishes native vertices from coarser LAND sampling.
    constexpr int N = 3;
    constexpr float spacing = 6.25f;
    const float heights[N*N] = {16,18,16,19,25,19,16,18,16};
    auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    TerrainField field; field.heights = heights; field.width = field.height = N;
    field.cellSize = spacing; field.originX = 5500; field.originZ = 10000;
    REQUIRE(backend->SetTerrain(field));
    const QueryFilter roadway{ColliderFlags::Roadway};
    for (int zCell = 0; zCell < 2; ++zCell) for (int xCell = 0; xCell < 2; ++xCell)
        for (const auto& fraction : {Vector3(.2f,0,.25f),Vector3(.75f,0,.8f),Vector3(.6f,0,.4f)})
        {
            const float x = fraction.X(), z = fraction.Z();
            const float y00 = heights[zCell*N+xCell], y01 = heights[zCell*N+xCell+1];
            const float y10 = heights[(zCell+1)*N+xCell], y11 = heights[(zCell+1)*N+xCell+1];
            const float nativeY = x <= 1-z ? y00+(y10-y00)*z+(y01-y00)*x
                : y10+(y01-y11)-(y10-y11)*x-(y01-y11)*z;
            const Vector3 from(field.originX+(xCell+x)*spacing,40,field.originZ+(zCell+z)*spacing);
            const Vector3 to(from.X(),0,from.Z()); Vector3 hit;
            REQUIRE(backend->RayHitAnything(from,to,roadway));
            backend->CastRay(from,to,hit,roadway);
            CHECK(std::abs(hit.Y()-nativeY) < .002f);
        }
    Vector3 ridge;
    const Vector3 from(field.originX+spacing,40,field.originZ+spacing);
    REQUIRE(backend->RayHitAnything(from,Vector3(from.X(),0,from.Z()),roadway));
    backend->CastRay(from,Vector3(from.X(),0,from.Z()),ridge,roadway);
    CHECK(std::abs(ridge.Y()-25) < .002f);
    backend->Destroy();
}

TEST_CASE("Articulated motion is inherited and only limbs of the same corpse ignore contact", "[physics][articulation][automatic-ragdoll]")
{
    const Vector3 vertices[] = {{-.15f,-.15f,-.15f},{.15f,-.15f,-.15f},{-.15f,.15f,-.15f},{.15f,.15f,-.15f},
        {-.15f,-.15f,.15f},{.15f,-.15f,.15f},{-.15f,.15f,.15f},{.15f,.15f,.15f}};
    ConvexPiece piece{vertices,8,ColliderFlags::Solid};
    for (bool sameCorpse : {true,false})
    {
        auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
        Matrix4 left = MIdentity, right = MIdentity;
        left.SetPosition(Vector3(-.08f,10,0)); right.SetPosition(Vector3(.08f,10,0));
        ArticulatedInitialMotion first, second;
        first.collisionFamily = -2; second.collisionFamily = sameCorpse ? -2 : -3;
        auto a = backend->SpawnArticulatedPiece(piece,left,3,first);
        auto b = backend->SpawnArticulatedPiece(piece,right,3,second);
        REQUIRE(a.IsValid()); REQUIRE(b.IsValid());
        for (int step = 0; step < 30; ++step) backend->Step(1.0f/60);
        BodyMotion ma, mb; REQUIRE(backend->GetBodyMotion(a,ma)); REQUIRE(backend->GetBodyMotion(b,mb));
        if (sameCorpse) CHECK(std::abs(ma.position.X()-mb.position.X()) < .17f);
        else CHECK((ma.position-mb.position).Size() > .28f);
        REQUIRE(backend->SetArticulatedMotion(a,Vector3(2,.3f,-1),Vector3(.1f,.2f,.3f)));
        REQUIRE(backend->GetBodyMotion(a,ma));
        CHECK((ma.linearVelocity-Vector3(2,.3f,-1)).Size() < .00001f);
        CHECK((ma.angularVelocity-Vector3(.1f,.2f,.3f)).Size() < .00001f);
        auto staticBody = backend->AddStaticBody(&piece,1,left);
        CHECK_FALSE(backend->SetArticulatedMotion(staticBody,VZero,VZero));
        backend->RemoveBody(a); CHECK_FALSE(backend->SetArticulatedMotion(a,VZero,VZero));
        first.linear = Vector3(1.7f,.4f,-.6f); first.angular = Vector3(.2f,-.1f,.3f);
        a = backend->SpawnArticulatedPiece(piece,left,3,first); REQUIRE(a.IsValid());
        REQUIRE(backend->GetBodyMotion(a,ma));
        CHECK((ma.linearVelocity-first.linear).Size() < .00001f);
        CHECK((ma.angularVelocity-first.angular).Size() < .00001f);
        first.angular[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(backend->SpawnArticulatedPiece(piece,left,3,first).IsValid());
        backend->RemoveBody(a); backend->RemoveBody(b); backend->RemoveBody(staticBody);
        CHECK(backend->GetStats().articulatedBodies == 0);
    }
}

TEST_CASE("Articulated Box3D parts constrain real motion and survive probe and stale joint cleanup", "[physics][articulation]")
{
    auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    std::vector<Vector3> points;
    for (int i=0;i<8;++i) points.emplace_back((i&1)?.15f:-.15f,(i&2)?.2f:-.2f,(i&4)?.15f:-.15f);
    ConvexPiece piece{points.data(),8,ColliderFlags::Solid};
    Matrix4 first = MIdentity, second = MIdentity;
    first.SetPosition(Vector3(0,2,0)); second.SetPosition(Vector3(0,3,0));
    BodyId a=backend->SpawnArticulatedPiece(piece,first,4), b=backend->SpawnArticulatedPiece(piece,second,4);
    REQUIRE(a.IsValid()); REQUIRE(b.IsValid());
    ArticulatedJointDef definition; definition.first=a; definition.second=b;
    definition.firstFrame.SetPosition(Vector3(0,.5f,0)); definition.secondFrame.SetPosition(Vector3(0,-.5f,0));
    JointId old=backend->AddArticulatedJoint(definition); REQUIRE(old.IsValid());
    CHECK(backend->GetStats().articulatedBodies == 2);
    CHECK(backend->GetStats().articulatedJoints == 1);
    backend->ClearProbes(); CHECK(backend->GetStats().probes == 0);
    backend->ApplyImpulse(b,second.Position(),Vector3(3,0,0));
    auto checkMotion = [&] {
        BodyMotion ma,mb; REQUIRE(backend->GetBodyMotion(a,ma)); REQUIRE(backend->GetBodyMotion(b,mb));
        Vector3 pivotA=ma.position+ma.axisY*.5f, pivotB=mb.position-mb.axisY*.5f;
        CHECK((pivotA-pivotB).Size() < .025f);
    };
    for (int i=0;i<30;++i) backend->Step(1.0f/60);
    checkMotion();
    backend->RemoveBody(a); backend->RemoveJoint(old);
    CHECK(backend->GetStats().articulatedBodies == 1);
    CHECK(backend->GetStats().articulatedJoints == 0);
    a=backend->SpawnArticulatedPiece(piece,first,4); REQUIRE(a.IsValid());
    backend->SetBodyTransform(b,second);
    definition.first=a; JointId fresh=backend->AddArticulatedJoint(definition); REQUIRE(fresh.IsValid());
    CHECK(fresh.generation != old.generation);
    backend->RemoveJoint(old); // must not destroy the replacement joint
    backend->ApplyImpulse(b,second.Position(),Vector3(-3,0,0));
    for (int i=0;i<30;++i) backend->Step(1.0f/60);
    checkMotion();
    backend->RemoveJoint(fresh); backend->RemoveBody(a); backend->RemoveBody(b);
    CHECK(backend->GetStats().bodies == 0);
    CHECK(backend->GetStats().articulatedBodies == 0);
    CHECK(backend->GetStats().articulatedJoints == 0);
    CHECK_FALSE(backend->AddArticulatedJoint(definition).IsValid());
    CHECK_FALSE(backend->SpawnArticulatedPiece(piece,Matrix4(MScale,2,1,1),4).IsValid());
    backend->Destroy(); backend->RemoveJoint(fresh);
    REQUIRE(backend->Create()); backend->RemoveJoint(fresh);
}

TEST_CASE("Physics world epochs retire articulated ownership across backend recreation", "[physics][articulation]")
{
    PhysicsWorld world;
    auto initial = world.Generation(); REQUIRE(world.Create());
    auto created = world.Generation(); CHECK(created > initial);
    REQUIRE(world.Create()); CHECK(world.Generation() == created);
    world.Destroy(); auto retired = world.Generation(); CHECK(retired > created);
    REQUIRE(world.Create()); CHECK(world.Generation() > retired);
}

TEST_CASE("Real revolute seam permits axial flexion blocks lateral bend and enforces signed limits", "[physics][articulation][corpse-hinge]")
{
    for (int drive : {1,-1,0})
    {
        auto backend=CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
        const Vector3 points[]={{-.05f,-.15f,-.05f},{.05f,-.15f,-.05f},{-.05f,.15f,-.05f},{.05f,.15f,-.05f},
            {-.05f,-.15f,.05f},{.05f,-.15f,.05f},{-.05f,.15f,.05f},{.05f,.15f,.05f}};
        ConvexPiece piece{points,8,ColliderFlags::Solid};
        Matrix4 first=MIdentity,second=MIdentity;
        first.SetPosition(Vector3(0,3,0)); second.SetPosition(Vector3(0,2.5f,0));
        BodyId a=backend->AddStaticBody(&piece,1,first),b=backend->SpawnArticulatedPiece(piece,second,2);
        REQUIRE(a.IsValid()); REQUIRE(b.IsValid());
        ArticulatedJointDef definition; definition.kind=ArticulatedJointKind::Hinge;
        definition.first=a; definition.second=b; definition.secondFrame.SetPosition(Vector3(0,.5f,0));
        definition.lowerAngle=-.18f; definition.upperAngle=.38f;
        JointId joint=backend->AddArticulatedJoint(definition); REQUIRE(joint.IsValid());
        BodyMotion initial; REQUIRE(backend->GetBodyMotion(b,initial));
        CHECK(initial.position == second.Position());
        CHECK(initial.axisX == VAside); CHECK(initial.axisY == VUp); CHECK(initial.axisZ == VForward);
        REQUIRE(backend->BeginReplayRecording());
        float maxAngle=0,minAngle=0,maxLateral=0,maxPivotError=0;
        for (int step=0;step<180;++step)
        {
            BodyMotion motion; REQUIRE(backend->GetBodyMotion(b,motion));
            // Equal off-center impulse, once each six frames. Its torque points
            // along hinge Z in one run and perpendicular X in the other.
            if (step%6 == 0)
                backend->ApplyImpulse(b,motion.position+motion.axisY*.1f,drive == 0 ? Vector3(0,0,-.5f) : Vector3(drive*.5f,0,0));
            backend->Step(1.0f/60);
            REQUIRE(backend->GetBodyMotion(b,motion));
            const float angle=std::atan2(motion.axisX.Y(),motion.axisX.X());
            maxAngle=std::max(maxAngle,angle);
            minAngle=std::min(minAngle,angle);
            maxLateral=std::max(maxLateral,std::sqrt(motion.axisZ.X()*motion.axisZ.X()+motion.axisZ.Y()*motion.axisZ.Y()));
            maxPivotError=std::max(maxPivotError,(motion.position+motion.axisY*.5f-first.Position()).Size());
            CHECK(angle >= definition.lowerAngle-.03f); CHECK(angle <= definition.upperAngle+.03f);
        }
        if (drive == 0) { CHECK(maxAngle < .015f); CHECK(minAngle > -.015f); }
        else if (drive > 0) CHECK(maxAngle > .2f);
        else CHECK(minAngle < -.13f);
        CHECK(maxLateral < .02f); CHECK(maxPivotError < .015f);
        const auto replay=backend->EndReplayRecordingAndValidate(1);
        REQUIRE(replay.supported); REQUIRE(replay.ran); CHECK_FALSE(replay.diverged); CHECK(replay.frameCount == 180);
        backend->RemoveJoint(joint); backend->RemoveBody(b); backend->RemoveBody(a);
        CHECK(backend->GetStats().articulatedJoints == 0); CHECK(backend->GetStats().bodies == 0);
    }
}

TEST_CASE("Revolute seam rejects invalid kinds frames and signed limits without leaking handles", "[physics][articulation][corpse-hinge]")
{
    auto backend=CreatePhysicsBackend(); REQUIRE(backend->Create());
    const Vector3 points[]={{.1f,0,0},{-.1f,0,0},{0,.1f,0},{0,-.1f,0},{0,0,.1f},{0,0,-.1f}};
    ConvexPiece piece{points,6,ColliderFlags::Solid};
    Matrix4 at=MIdentity; at.SetPosition(Vector3(0,3,0));
    ArticulatedJointDef d; d.first=backend->SpawnArticulatedPiece(piece,at,2);
    at.SetPosition(Vector3(0,3.5f,0)); d.second=backend->SpawnArticulatedPiece(piece,at,2);
    REQUIRE(d.first.IsValid()); REQUIRE(d.second.IsValid()); d.kind=ArticulatedJointKind::Hinge;
    d.lowerAngle=1; d.upperAngle=-1; CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    d.lowerAngle=-4; d.upperAngle=1; CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    d.lowerAngle=-1; d.upperAngle=4; CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    d.upperAngle=std::numeric_limits<float>::quiet_NaN(); CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    d.upperAngle=1; d.firstFrame(0,0)=2; CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    d.firstFrame=MIdentity; d.kind=static_cast<ArticulatedJointKind>(17); CHECK_FALSE(backend->AddArticulatedJoint(d).IsValid());
    CHECK(backend->GetStats().articulatedJoints == 0);
    backend->RemoveBody(d.first); backend->RemoveBody(d.second);
}

TEST_CASE("showcase block contacts sweep players and projectiles", "[physics][showcase-contact]")
{
    ProbeSample box;
    box.shape = ProbeShape::Box;
    box.position = Vector3(0, 0.4f, 0);
    box.halfExtents = Vector3(1, 0.4f, 0.39f);
    float t = 1;
    Vector3 normal;
    CHECK(Poseidon::Dev::SweepShowcaseBlock(box, Vector3(0,0.85f,-3), Vector3(0,0.85f,3),
                                           0.3f, 0.55f, t, normal));
    CHECK(t > 0.38f);
    CHECK(t < 0.39f);
    CHECK(normal.Z() < -0.99f);
    t = 1;
    CHECK_FALSE(Poseidon::Dev::SweepShowcaseBlock(box, Vector3(2,0.85f,-3), Vector3(2,0.85f,3),
                                                 0.3f, 0.55f, t, normal));
    t = 1;
    CHECK(Poseidon::Dev::SweepShowcaseBlock(box, Vector3(0,0.4f,-100), Vector3(0,0.4f,100),
                                           0, 0, t, normal));
    CHECK(std::abs(t-0.49805f) < 0.00001f);
    box.axisX = Vector3(0,0,1);
    box.axisZ = Vector3(-1,0,0);
    t = 1;
    CHECK(Poseidon::Dev::SweepShowcaseBlock(box, Vector3(-3,0.4f,0), Vector3(3,0.4f,0),
                                           0, 0, t, normal));
    CHECK(normal.X() < -0.99f);
    t = 1;
    CHECK_FALSE(Poseidon::Dev::SweepShowcaseBlock(box, box.position, Vector3(3,0.4f,0),
                                                 0.3f, 0.55f, t, normal));
}

TEST_CASE("showcase blast impulses decay and wake stacked bodies", "[physics][showcase-contact]")
{
    using Poseidon::Dev::ShowcaseBlastImpulse;
    CHECK(ShowcaseBlastImpulse(Vector3(0,0,21), 20, 5).SquareSize() == 0);
    CHECK(ShowcaseBlastImpulse(VZero, 0, 5).SquareSize() == 0);
    CHECK(ShowcaseBlastImpulse(VZero, 20, 0).SquareSize() == 0);
    CHECK(ShowcaseBlastImpulse(VZero, 20, 5).Y() == 40);
    CHECK(ShowcaseBlastImpulse(Vector3(0,0,10), 20, 5).Z() == 10);
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    SphereProbeDef def;
    def.shape = ProbeShape::Box;
    def.halfExtents = Vector3(0.99f,0.4f,0.39f);
    def.position = Vector3(0,2,2);
    def.mass = 8;
    const auto id = backend->SpawnSphereProbe(def);
    REQUIRE(id.IsValid());
    backend->ApplyImpulse(id, def.position, ShowcaseBlastImpulse(Vector3(0,0,2),20,5));
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(id, motion));
    CHECK(motion.linearVelocity.Z() > 4);
}

TEST_CASE("regional terrain collision edits cross tile seams and survive replacement", "[physics][terrain-patch]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    constexpr int n = 257;
    std::vector<float> flat(n*n, 0.0f);
    TerrainField field;
    field.heights = flat.data();
    field.width = field.height = n;
    field.cellSize = 1;
    REQUIRE(backend->SetTerrain(field));
    std::vector<float> patch(17*17, -4.0f);
    REQUIRE(backend->PatchTerrain(120, 120, 17, 17, patch.data()));
    CHECK(backend->GetStats().terrainRegistered);
    for (float x : {127.5f, 128.0f, 128.5f})
        for (float z : {127.5f, 128.0f, 128.5f})
        {
            CHECK_FALSE(backend->RayHitAnything(Vector3(x,1,z), Vector3(x,-1,z), QueryFilter{}));
            CHECK(backend->RayHitAnything(Vector3(x,1,z), Vector3(x,-6,z), QueryFilter{}));
        }
    CHECK(backend->RayHitAnything(Vector3(64,1,64), Vector3(64,-1,64), QueryFilter{}));
    CHECK_FALSE(backend->PatchTerrain(250, 120, 17, 17, patch.data()));
    patch[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(backend->PatchTerrain(120, 120, 17, 17, patch.data()));
    CHECK_FALSE(backend->RayHitAnything(Vector3(128,1,128), Vector3(128,-1,128), QueryFilter{}));
    std::fill(patch.begin(), patch.end(), 0.0f);
    REQUIRE(backend->PatchTerrain(120, 120, 17, 17, patch.data()));
    CHECK(backend->RayHitAnything(Vector3(128,1,128), Vector3(128,-1,128), QueryFilter{}));
    REQUIRE(backend->SetTerrain(field));
    REQUIRE(backend->PatchTerrain(120, 120, 17, 17, patch.data()));
    backend->Destroy();
    CHECK_FALSE(backend->GetStats().terrainRegistered);
    REQUIRE(backend->Create());
    CHECK_FALSE(backend->PatchTerrain(0, 0, 1, 1, patch.data()));
    field.cellSize = 2;
    field.originX = 1000;
    field.originZ = -1000;
    REQUIRE(backend->SetTerrain(field));
    std::fill(patch.begin(),patch.end(),-4.0f);
    REQUIRE(backend->PatchTerrain(120,120,17,17,patch.data()));
    CHECK_FALSE(backend->RayHitAnything(Vector3(1256,1,-744),Vector3(1256,-1,-744),QueryFilter{}));
    CHECK(backend->RayHitAnything(Vector3(1256,1,-744),Vector3(1256,-6,-744),QueryFilter{}));
}

TEST_CASE("regional terrain edit timing separates initial partition from repeat edits", "[physics][terrain-patch-perf]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    constexpr int n = 1025;
    std::vector<float> heights(n*n, 0.0f);
    TerrainField field;
    field.heights = heights.data();
    field.width = field.height = n;
    field.cellSize = 12.5f;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    REQUIRE(backend->SetTerrain(field));
    const auto full = Clock::now();
    std::vector<float> patch(17*17, -2.0f);
    REQUIRE(backend->PatchTerrain(504,504,17,17,patch.data()));
    const auto partition = Clock::now();
    for (int i=0; i<8; ++i)
    {
        std::fill(patch.begin(),patch.end(), -2.0f-0.1f*i);
        REQUIRE(backend->PatchTerrain(504,504,17,17,patch.data()));
    }
    const auto done = Clock::now();
    std::printf("Terrain collision 1025^2: full %.3f ms, first partition %.3f ms, four-tile patch mean %.3f ms\n",
        std::chrono::duration<double,std::milli>(full-start).count(),
        std::chrono::duration<double,std::milli>(partition-full).count(),
        std::chrono::duration<double,std::milli>(done-partition).count()/8.0);
}

TEST_CASE("sparse terrain history keeps first heights across overlapping strokes", "[physics][terrain-journal]")
{
    Poseidon::TerrainEditJournal journal;
    REQUIRE(journal.Empty());
    int reads = 0;
    REQUIRE(journal.Remember(8193,15,15,3,3,[&](int x,int z) { ++reads; return float(x+z*100); }));
    CHECK(journal.Tiles() == 4);
    CHECK(journal.Samples() == 9);
    CHECK(reads == 9);
    REQUIRE(journal.Remember(8193,16,16,3,3,[&](int,int) { ++reads; return -99.0f; }));
    CHECK(journal.Samples() == 14);
    CHECK(reads == 14);
    int restored = 0;
    journal.ForEach([&](int x,int z,float before) {
        ++restored;
        CHECK(before == (x <= 17 && z <= 17 ? float(x+z*100) : -99.0f));
    });
    CHECK(restored == 14);
    journal.Clear();
    CHECK(journal.Empty());
    CHECK(journal.Tiles() == 0);
    CHECK(journal.Samples() == 0);
}

TEST_CASE("terrain journal refuses invalid or over-budget regions before reading", "[physics][terrain-journal]")
{
    Poseidon::TerrainEditJournal journal;
    const auto read = [](int,int) { return 7.0f; };
    REQUIRE(journal.Remember(8193,8192,8192,1,1,read));
    int reads = 0;
    const auto unexpected = [&](int,int) { ++reads; return 0.0f; };
    CHECK_FALSE(journal.Remember(8193,0,0,8193,8193,unexpected));
    CHECK_FALSE(journal.Remember(8193,-1,0,2,2,unexpected));
    CHECK_FALSE(journal.Remember(8193,8192,0,2,2,unexpected));
    CHECK_FALSE(journal.Remember(8193,0,8192,2,2,unexpected));
    CHECK_FALSE(journal.Remember(8193,0,0,0,2,unexpected));
    CHECK(reads == 0);
    CHECK(journal.Tiles() == 1);
    CHECK(journal.Samples() == 1);
    journal.ForEach([](int x,int z,float before) {
        CHECK(x == 8192); CHECK(z == 8192); CHECK(before == 7.0f);
    });
}

TEST_CASE("terrain crater is compact smooth and finite", "[physics][crater]")
{
    using Poseidon::TerrainCraterDepth;
    CHECK(TerrainCraterDepth(0,0,24,8) == 8);
    CHECK(TerrainCraterDepth(24,0,24,8) == 0);
    CHECK(TerrainCraterDepth(25,0,24,8) == 0);
    CHECK(TerrainCraterDepth(0,0,0,8) == 0);
    CHECK(TerrainCraterDepth(0,0,24,-1) == 0);
    CHECK(TerrainCraterDepth(23.99f,0,24,8) < 0.00001f);
    CHECK(TerrainCraterDepth(4,7,24,8) == TerrainCraterDepth(-7,4,24,8));
}

TEST_CASE("Rocket craters reject airbursts and submerged ground", "[physics][crater]")
{
    using Poseidon::RocketCraterGroundContact;
    CHECK(RocketCraterGroundContact(20.1f,20.0f,0.0f));
    CHECK(RocketCraterGroundContact(19.5f,20.0f,0.0f));
    CHECK_FALSE(RocketCraterGroundContact(25.0f,20.0f,0.0f));
    CHECK_FALSE(RocketCraterGroundContact(0.0f,-1.0f,0.0f));
    CHECK_FALSE(RocketCraterGroundContact(-5.0f,-5.0f,0.0f));
    CHECK_FALSE(RocketCraterGroundContact(NAN,20.0f,0.0f));
}

TEST_CASE("Showcase castle leaves a gate and a bounded projectile budget", "[physics][showcase]")
{
    const auto blocks = Poseidon::Dev::ShowcaseCastleLayout();
    CHECK(blocks.size() == 485);
    CHECK(blocks.size() + 16 < 512);
    float maxHeight = 0;
    for (const auto& b : blocks)
    {
        CHECK_FALSE((b.z == -9 && b.height < 3.2f && b.x >= -1 && b.x <= 1));
        maxHeight = std::max(maxHeight, b.height + 0.4f);
    }
    CHECK(maxHeight > 11);
    CHECK(maxHeight < 12);
}

TEST_CASE("a newly defined probe has no launch velocity", "[physics][showcase]")
{
    SphereProbeDef def;
    REQUIRE(def.velocity[0] == 0.0f);
    REQUIRE(def.velocity[1] == 0.0f);
    REQUIRE(def.velocity[2] == 0.0f);
}

namespace
{
int waterEntries = 0;
bool FlatProbeWater(Vector3Par p, ProbeWaterSample& water)
{
    water.height = 0;
    water.velocity = Vector3(0, 0, 0);
    return p[0] >= 0;
}
void CountProbeImpact(Vector3Par, Vector3Par, float, float, bool entering)
{
    if (entering) ++waterEntries;
}
}

TEST_CASE("probe water supports displaced mass and damps fast entries", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.velocity = Vector3(0, 0, 0);
    def.radius = 0.5f; // 524 kg of displaced fresh water when fully submerged.
    def.position = Vector3(5, 2, 0);
    def.mass = 100;
    const auto floating = backend->SpawnSphereProbe(def);
    def.position = Vector3(10, 2, 0);
    def.mass = 1000;
    const auto sinking = backend->SpawnSphereProbe(def);
    def.position = Vector3(-5, 2, 0);
    const auto dry = backend->SpawnSphereProbe(def);
    def.position = Vector3(15, 2, 0);
    def.velocity = Vector3(0, -100, 0);
    def.mass = 0.01f;
    const auto fast = backend->SpawnSphereProbe(def);
    for (int i = 0; i < 1200; ++i) backend->Step(1.0f / 60.0f);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(floating, motion));
    CHECK(motion.position[1] > 0.1f);
    CHECK(motion.position[1] < 0.5f);
    CHECK(std::abs(motion.linearVelocity[1]) < 0.3f);
    REQUIRE(backend->GetBodyMotion(sinking, motion));
    CHECK(motion.position[1] < -5);
    REQUIRE(backend->GetBodyMotion(dry, motion));
    CHECK(motion.position[1] < -100);
    REQUIRE(backend->GetBodyMotion(fast, motion));
    CHECK(std::isfinite(motion.position[1]));
    CHECK(std::abs(motion.linearVelocity[1]) < 1);
    CHECK(waterEntries >= 3);
    CHECK(waterEntries < 20);
}

TEST_CASE("box water balance uses box volume and the requested kilograms", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    SphereProbeDef def;
    def.shape = ProbeShape::Box;
    def.halfExtents = Vector3(0.5f, 0.5f, 0.5f);
    def.radius = 0.1f; // Unrelated sphere slider must not change the box mass.
    def.mass = 600;
    def.position = Vector3(5, 1, 0);
    def.velocity = Vector3(0, 0, 0);
    const auto box = backend->SpawnSphereProbe(def);
    for (int i = 0; i < 900; ++i) backend->Step(1.0f / 60.0f);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(box, motion));
    CHECK(motion.position[1] > -0.15f);
    CHECK(motion.position[1] < -0.05f); // 60% of a 1 m cube submerged.
}

TEST_CASE("sideways shore entry emits without a downward throw", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.radius = 0.5f;
    def.mass = 100;
    def.position = Vector3(-0.5f, 0.4f, 0);
    def.velocity = Vector3(5, 0, 0);
    REQUIRE(backend->SpawnSphereProbe(def).IsValid());
    for (int i = 0; i < 60; ++i) backend->Step(1.0f / 60.0f);
    CHECK(waterEntries == 1);
}

TEST_CASE("already submerged probes do not fabricate a surface impact", "[physics][ProbeWater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    backend->SetWaterEnvironment({FlatProbeWater, CountProbeImpact});
    waterEntries = 0;
    SphereProbeDef def;
    def.position = Vector3(5, -10, 0);
    def.velocity = Vector3(0, -2, 0);
    REQUIRE(backend->SpawnSphereProbe(def).IsValid());
    for (int i = 0; i < 60; ++i) backend->Step(1.0f / 60.0f);
    CHECK(waterEntries == 0);
}

namespace
{

/// Unit cube corners: the smallest input that is unambiguously a volume.
std::vector<Vector3> UnitCube()
{
    return {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
}

} // namespace

TEST_CASE("physics backend creates and destroys a world", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend != nullptr);
    REQUIRE(backend->Create());
    // Idempotent: a second Create must not leak a second world.
    REQUIRE(backend->Create());
    backend->Destroy();
    // And destroying twice must not fault -- shutdown paths run more than once.
    backend->Destroy();
}

TEST_CASE("a convex piece becomes a body with a shape", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId id = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(id.IsValid());

    const PhysicsStats stats = backend->GetStats();
    CHECK(stats.bodies == 1);
    CHECK(stats.shapes == 1);
    CHECK(stats.hullFailures == 0);

    backend->Destroy();
}

TEST_CASE("a piece with too few points is refused and counted", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    // A triangle is a sheet, not a hull. The point of the assertion is that the
    // refusal is VISIBLE: PHY-010's abort condition is about how much of the stock
    // corpus converts, and a silently skipped component makes a half-built world
    // indistinguishable from a working one.
    const std::vector<Vector3> triangle = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    ConvexPiece                piece{triangle.data(), static_cast<int>(triangle.size())};

    const BodyId id = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    CHECK_FALSE(id.IsValid());
    CHECK(backend->GetStats().hullFailures == 1);
    // A body with no usable shape must not be left behind as an empty one.
    CHECK(backend->GetStats().bodies == 0);

    backend->Destroy();
}

TEST_CASE("a removed body's handle stops resolving", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId first = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(first.IsValid());
    backend->RemoveBody(first);
    CHECK(backend->GetStats().bodies == 0);

    // The slot is recycled, so `second` may share an index with `first`. The
    // generation counter is what keeps them distinct -- without it, moving the
    // stale handle would move the NEW body, and nothing would report an error.
    const BodyId second = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(second.IsValid());
    CHECK(second != first);

    backend->SetBodyTransform(first, Matrix4(MIdentity)); // must be a no-op, not a crash
    backend->RemoveBody(first);                           // likewise
    CHECK(backend->GetStats().bodies == 1);

    backend->Destroy();
}

TEST_CASE("a heightfield is accepted and a degenerate one is not", "[physics]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    constexpr int      N = 8;
    std::vector<float> heights(N * N, 0.0f);
    for (int z = 0; z < N; ++z)
    {
        for (int x = 0; x < N; ++x)
        {
            heights[z * N + x] = static_cast<float>(x) * 0.5f; // a ramp, so it is not a plane
        }
    }

    TerrainField field;
    field.heights = heights.data();
    field.width = N;
    field.height = N;
    field.cellSize = 50.0f; // OFP's landscape grid
    field.originX = 0.0f;
    field.originZ = 0.0f;
    REQUIRE(backend->SetTerrain(field));

    const PhysicsStats stats = backend->GetStats();
    CHECK(stats.terrainRegistered);
    CHECK(stats.terrainWidth == N);
    CHECK(stats.terrainHeight == N);

    // A single row is not a surface.
    TerrainField tooSmall = field;
    tooSmall.height = 1;
    CHECK_FALSE(backend->SetTerrain(tooSmall));

    backend->Destroy();
}

TEST_CASE("removing a body gives its shapes back to the count", "[physics]")
{
    // Regression: _shapeCount only ever counted up. b3DestroyBody takes the
    // body's shapes with it silently, so after any removal -- streaming, a
    // mission reload -- the diagnostic reported more shapes than existed. A
    // number that can only grow is not a diagnostic.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    // Two pieces on one body, so the test would still fail if removal subtracted
    // a flat 1 per body instead of what the body actually owned.
    const ConvexPiece pieces[2] = {
        {cube.data(), static_cast<int>(cube.size())},
        {cube.data(), static_cast<int>(cube.size())},
    };

    const BodyId id = backend->AddStaticBody(pieces, 2, Matrix4(MIdentity));
    REQUIRE(id.IsValid());
    CHECK(backend->GetStats().shapes == 2);

    backend->RemoveBody(id);
    CHECK(backend->GetStats().bodies == 0);
    CHECK(backend->GetStats().shapes == 0);

    backend->Destroy();
}

TEST_CASE("a destroy/create cycle does not reuse stale slots", "[physics]")
{
    // Regression: Destroy() cleared _slots but not _free, so the next Store()
    // popped a stale index and indexed an empty vector. Silent memory corruption
    // one mission reload away, and nothing in the earlier tests touched it because
    // they never destroyed and recreated the same backend.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};

    const BodyId first = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(first.IsValid());
    backend->RemoveBody(first); // leaves a free slot behind

    backend->Destroy();
    REQUIRE(backend->Create());

    // Counters must start from zero too, or two worlds accumulate into one report.
    CHECK(backend->GetStats().bodies == 0);
    CHECK(backend->GetStats().shapes == 0);
    CHECK(backend->GetStats().hullFailures == 0);
    CHECK_FALSE(backend->GetStats().terrainRegistered);

    const BodyId afterCycle = backend->AddStaticBody(&piece, 1, Matrix4(MIdentity));
    REQUIRE(afterCycle.IsValid());
    CHECK(backend->GetStats().bodies == 1);

    backend->Destroy();
}

TEST_CASE("terrain survives a replacement and a destroy", "[physics]")
{
    // b3CreateHeightFieldShape holds a REFERENCE to the height field for the
    // lifetime of the shape, so the world must be destroyed before the field.
    // This exercises both orders: replace-while-live, then destroy.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    constexpr int      N = 8;
    std::vector<float> heights(N * N);
    for (int i = 0; i < N * N; ++i)
    {
        heights[static_cast<std::size_t>(i)] = static_cast<float>(i % N) * 0.25f;
    }

    TerrainField field;
    field.heights = heights.data();
    field.width = N;
    field.height = N;
    field.cellSize = 50.0f;
    field.originX = 0.0f;
    field.originZ = 0.0f;

    REQUIRE(backend->SetTerrain(field));
    REQUIRE(backend->SetTerrain(field)); // replace the live one
    backend->Step(1.0f / 60.0f);
    CHECK(backend->GetStats().terrainRegistered);

    backend->Destroy();
    CHECK_FALSE(backend->GetStats().terrainRegistered);
}

TEST_CASE("replacing excavated terrain wakes a resting body and preserves its handle", "[physics][crater]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());
    constexpr int n = 33;
    std::vector<float> heights(n*n, 0.0f);
    TerrainField field;
    field.heights = heights.data();
    field.width = field.height = n;
    field.cellSize = 1;
    field.originX = field.originZ = 0;
    REQUIRE(backend->SetTerrain(field));
    SphereProbeDef def;
    def.position = Vector3(16,1,16);
    def.radius = 0.25f;
    const auto id = backend->SpawnSphereProbe(def);
    REQUIRE(id.IsValid());
    for (int i=0; i<900; ++i) backend->Step(1.0f/60);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(id,motion));
    REQUIRE(motion.position.Y() > 0.1f);
    REQUIRE(motion.position.Y() < 0.5f);
    CHECK_FALSE(motion.awake);
    for (int z=0; z<n; ++z) for (int x=0; x<n; ++x)
        heights[z*n+x] = -Poseidon::TerrainCraterDepth(float(x-16),float(z-16),8,4);
    REQUIRE(backend->SetTerrain(field));
    REQUIRE(backend->GetBodyMotion(id,motion));
    CHECK(motion.awake);
    for (int i=0; i<600; ++i) backend->Step(1.0f/60);
    REQUIRE(backend->GetBodyMotion(id,motion));
    CHECK(motion.position.Y() < -3.0f);
    CHECK(motion.position.Y() > -4.1f);
}

TEST_CASE("a world with no backend answers safely", "[physics]")
{
    // PhysicsWorld must be usable before Create() and after Destroy(): the engine
    // is allowed to run with no physics at all, and every call has to be inert
    // rather than a null dereference waiting for the first caller who forgets.
    PhysicsWorld world;
    CHECK_FALSE(world.IsCreated());
    CHECK(std::string(world.BackendName()) == "none");
    world.Step(1.0f / 60.0f);
    CHECK_FALSE(world.RegisterStatic(nullptr, Matrix4(MIdentity)).IsValid());
    CHECK_FALSE(world.SetTerrain(nullptr, 0, 0, 0.0f, 0.0f, 0.0f));
    CHECK(world.GetStats().bodies == 0);
}

TEST_CASE("removing a probe body individually leaves no dangling readback", "[physics]")
{
    // The path nothing called. `ClearProbes()` removes probes wholesale and is
    // what the dev panel uses, so `RemoveBody()` on a body that is ALSO a probe
    // had never run -- yet it is exactly what `LooseObjects::Release` does, via
    // `PhysicsWorld::Remove`, from `Thing::~Thing`.
    //
    // Before the fix this crashed: `RemoveBody` freed the slot and destroyed the
    // body but left the entry in `_probes`, and `GetProbeSamples` calls
    // b3Body_GetTransform on every entry without a validity check. Three probes,
    // remove the middle one, next readback = SIGSEGV.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    std::vector<BodyId> ids;
    for (int i = 0; i < 3; ++i)
    {
        SphereProbeDef def;
        def.position = Vector3(0.0f, 5.0f + static_cast<float>(i), 0.0f);
        def.radius = 0.2f;
        const BodyId id = backend->SpawnSphereProbe(def);
        REQUIRE(id.IsValid());
        ids.push_back(id);
    }
    REQUIRE(backend->GetStats().probes == 3);
    REQUIRE(backend->GetStats().bodies == 3);

    backend->RemoveBody(ids[1]);

    // The count is the cheap tell, and it was wrong before anything crashed.
    CHECK(backend->GetStats().bodies == 2);
    CHECK(backend->GetStats().probes == 2);

    std::vector<ProbeSample> samples;
    backend->GetProbeSamples(samples); // used to fault here
    REQUIRE(samples.size() == 2);
    // Every surviving sample still resolves to a live slot. A sample whose id
    // came back invalid would mean the entry outlived its slot -- the same defect
    // wearing a quieter symptom.
    for (const ProbeSample& s : samples)
    {
        CHECK(s.id.IsValid());
        BodyMotion motion;
        CHECK(backend->GetBodyMotion(s.id, motion));
    }
    CHECK_FALSE(samples[0].id == ids[1]);
    CHECK_FALSE(samples[1].id == ids[1]);

    // Stepping must not touch the removed body either -- `Step` walks `_probes`
    // to apply wind.
    WindSettings wind;
    wind.enabled = true;
    wind.velocity = Vector3(5.0f, 0.0f, 0.0f);
    backend->SetWind(wind);
    backend->Step(1.0f / 60.0f);
    CHECK(backend->GetStats().probes == 2);

    // The quieter half: Box3D reuses body ids, so a stale `_probes` entry can
    // come to ALIAS a body created later. `ClearProbes` destroys everything in
    // `_probes`, so the alias would delete a collider nobody asked it to touch.
    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};
    Matrix4                    at(MIdentity);
    at.SetPosition(Vector3(20.0f, 1.0f, 20.0f));
    const BodyId building = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(building.IsValid());

    // A step before the ray: Box3D holds a newly created proxy in a move buffer
    // until the next broadphase update, so a cast straight after AddStaticBody
    // misses a body that is genuinely there.
    backend->Step(1.0f / 60.0f);

    Vector3       hit;
    const Vector3 from(20.5f, 10.0f, 20.5f);
    const Vector3 to(20.5f, -5.0f, 20.5f);
    REQUIRE(backend->CastRay(from, to, hit, QueryFilter{}) == building);

    backend->ClearProbes();
    CHECK(backend->GetStats().probes == 0);
    // Still there. This is the assertion the whole test exists for.
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == building);

    backend->Destroy();
}

TEST_CASE("a ray identifies a body that moved into a recycled slot", "[physics]")
{
    // `CastRay` is the only place that read the generation counter as a live/dead
    // PARITY flag -- slots start at 1, `RemoveBody` increments, so a freed slot is
    // even. `Store` then reuses that slot WITHOUT incrementing again, so the next
    // occupant keeps the even generation and this refused to name it. The ray hit;
    // the answer came back "no body".
    //
    // It matters because the answer feeds `ShotShell::TryPenetrate` through
    // `MeasureThickness`, and because a slot is freed by every `ClearProbes()` and
    // every `LooseObjects::Release`. Nothing in the suite removed a body and then
    // cast a ray at its replacement, which is why an inconsistency between `Find()`
    // (exact compare, always right) and this (parity, wrong on every reuse) sat
    // there unnoticed.
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend->Create());

    const std::vector<Vector3> cube = UnitCube();
    ConvexPiece                piece{cube.data(), static_cast<int>(cube.size())};
    Matrix4                    at(MIdentity);
    at.SetPosition(Vector3(20.0f, 1.0f, 20.0f));

    // First occupant of slot 0. A ray names it -- this always worked.
    const BodyId first = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(first.IsValid());
    backend->Step(1.0f / 60.0f); // let the broadphase take the new proxy

    Vector3       hit;
    const Vector3 from(20.5f, 10.0f, 20.5f);
    const Vector3 to(20.5f, -5.0f, 20.5f);
    REQUIRE(backend->RayHitAnything(from, to, QueryFilter{}));
    REQUIRE(backend->CastRay(from, to, hit, QueryFilter{}) == first);

    // Free the slot and put a different body in it.
    backend->RemoveBody(first);
    CHECK_FALSE(backend->RayHitAnything(from, to, QueryFilter{}));

    const BodyId second = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(second.IsValid());
    CHECK(second != first);          // the generation moved on
    CHECK(second.index == first.index); // and it is the same slot, so this is the reuse case
    backend->Step(1.0f / 60.0f);

    // The ray still hits -- that half was never broken, which is what made the
    // other half hard to see.
    REQUIRE(backend->RayHitAnything(from, to, QueryFilter{}));
    // And it names the CURRENT occupant. This is the assertion that used to fail.
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == second);
    // Never the stale handle.
    CHECK_FALSE(backend->CastRay(from, to, hit, QueryFilter{}) == first);

    // Third occupant, so the fix is not merely "even generations also work".
    backend->RemoveBody(second);
    const BodyId third = backend->AddStaticBody(&piece, 1, at);
    REQUIRE(third.IsValid());
    backend->Step(1.0f / 60.0f);
    CHECK(backend->CastRay(from, to, hit, QueryFilter{}) == third);

    backend->Destroy();
}
