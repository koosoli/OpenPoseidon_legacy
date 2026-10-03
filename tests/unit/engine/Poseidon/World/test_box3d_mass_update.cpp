// Actual backend regression: the released dependency's stale inverse inertia
// produced six times the intended spin after convex hull mass rescaling.
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Simulation/Animation/CorpsePose.hpp>
#include <cmath>
#include <limits>

using namespace Poseidon::Physics;
using namespace Poseidon;

TEST_CASE("Bounded corpse contact proxies preserve actual body origin and affine handoff", "[physics][articulation][corpse-contact]")
{
    // Actual installed refusal: right forearm bottom -2.792 mm below support.
    float lift = 0;
    REQUIRE(CorpseProxyContactLift(-.0027923584f,lift));
    CHECK(lift > .0057f);
    CHECK(lift < .010f);
    float rejected = 9;
    CHECK_FALSE(CorpseProxyContactLift(-.02f,rejected)); CHECK(rejected == 0);
    CHECK_FALSE(CorpseProxyContactLift(std::numeric_limits<float>::quiet_NaN(),rejected));
    CHECK(CorpseProxyContactLift(.003f,rejected)); CHECK(rejected == 0);

    auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    float heights[16]; for (auto& y : heights) y = 160.98262f;
    TerrainField field; field.heights = heights; field.width = field.height = 4;
    field.cellSize = .25f; field.originX = 6530.5f; field.originZ = 6467.5f;
    REQUIRE(backend->SetTerrain(field));
    Vector3 points[] = {{.03f,0,0},{-.03f,0,0},{0,.0412f,0},{0,-.0412f,0},{0,0,.05f},{0,0,-.05f}};
    for (auto& point : points) point += VUp*lift;
    ConvexPiece piece{points,6,ColliderFlags::Solid};
    Matrix4 initial = MIdentity; initial.SetPosition(Vector3(6530.8667f,161.021027f,6467.71436f));
    for (const auto& point : points)
    {
        const Vector3 supported = initial*point;
        Vector3 floor;
        REQUIRE(backend->RayHitAnything(supported+VUp*.8f,supported-VUp,QueryFilter{ColliderFlags::Roadway}));
        backend->CastRay(supported+VUp*.8f,supported-VUp,floor,QueryFilter{ColliderFlags::Roadway});
        CHECK(supported.Y()-floor.Y() >= .003f);
    }
    BodyId body = backend->SpawnArticulatedPiece(piece,initial,2); REQUIRE(body.IsValid());
    BodyMotion motion; REQUIRE(backend->GetBodyMotion(body,motion));
    // The local mass center moves with the proxy. Its actual transform origin
    // MUST retain the authored frame, rather than becoming that shifted center.
    CHECK(motion.position == initial.Position());
    CHECK(motion.axisX == VAside); CHECK(motion.axisY == VUp); CHECK(motion.axisZ == VForward);
    Matrix4 previous[2] = {Matrix4(MScale,1.3f,.7f,1.1f),MIdentity};
    Matrix4 next[2] = {MIdentity,Matrix4(MScale,.8f,1.2f,.9f)};
    previous[0](0,1) = .21f; next[1](1,2) = -.13f;
    AffinePhasePose original, held;
    REQUIRE(original.Capture(previous,next,2,2,.37f)); REQUIRE(held.Capture(previous,next,2,2,.37f));
    AnimationRTWeight weight; weight.Add(AnimationRTPair(0,.675f)); weight.Add(AnimationRTPair(1,.325f));
    Matrix4 delta = MIdentity; delta.SetPosition(motion.position-initial.Position());
    REQUIRE(held.ApplyRigidDeltas(std::vector<Matrix4>(2,delta)));
    CHECK(CorpseMatrixIdentityError(delta) == 0);
    CHECK(held.Point(weight,Vector3(.17f,-.29f,.81f),VZero) == original.Point(weight,Vector3(.17f,-.29f,.81f),VZero));
    MATRIX_4_ARRAY(originalPalette,128); MATRIX_4_ARRAY(heldPalette,128);
    original.PrepareMatrices(originalPalette,1); held.PrepareMatrices(heldPalette,1);
    for (int bone=0;bone<2;++bone)
        for (int row=0;row<3;++row)
            for (int col=0;col<4;++col) CHECK(originalPalette[bone](row,col) == heldPalette[bone](row,col));
    Matrix4 proxy = MIdentity, originalProxy = MIdentity;
    held.Matrix(proxy,weight); original.Matrix(originalProxy,weight);
    CHECK(CorpseMatrixIdentityError(proxy-originalProxy+MIdentity) == 0);

    Matrix4 second = initial; second.SetPosition(initial.Position()+VAside*.16f);
    BodyId other = backend->SpawnArticulatedPiece(piece,second,2); REQUIRE(other.IsValid());
    ArticulatedJointDef joint; joint.first = body; joint.second = other;
    joint.firstFrame.SetPosition(Vector3(.08f,0,0)); joint.secondFrame.SetPosition(Vector3(-.08f,0,0));
    JointId linked = backend->AddArticulatedJoint(joint); REQUIRE(linked.IsValid());
    backend->ApplyImpulse(other,second.Position(),Vector3(0,0,.1f));
    for (int i=0;i<120;++i) backend->Step(1.0f/60);
    BodyMotion otherMotion; REQUIRE(backend->GetBodyMotion(body,motion)); REQUIRE(backend->GetBodyMotion(other,otherMotion));
    CHECK(motion.position.IsFinite()); CHECK(otherMotion.position.IsFinite());
    CHECK((motion.position-initial.Position()).Size() < .1f);
    CHECK((motion.position+motion.axisX*.08f-otherMotion.position+otherMotion.axisX*.08f).Size() < .025f);
    backend->RemoveJoint(linked); backend->RemoveBody(body); backend->RemoveBody(other);
    CHECK(backend->GetStats().articulatedBodies == 0); CHECK(backend->GetStats().articulatedJoints == 0);
}

TEST_CASE("Actual articulated hull mass scaling reaches off-center impulse inertia", "[physics][mass-update]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend);
    REQUIRE(backend->Create());
    // Octahedron volume is one sixth of its bounding box. The actual engine
    // consequently invokes SetMassData with a meaningful sixfold rescale.
    const Vector3 points[] = {{.2f,0,0},{-.2f,0,0},{0,.3f,0},{0,-.3f,0},{0,0,.4f},{0,0,-.4f}};
    ConvexPiece piece{points,6,ColliderFlags::Solid};
    Matrix4 frame = MIdentity;
    frame.SetPosition(Vector3(0,3,0));
    BodyId body = backend->SpawnArticulatedPiece(piece,frame,4);
    REQUIRE(body.IsValid());
    backend->ApplyImpulse(body,Vector3(.2f,3,0),Vector3(0,0,1));
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(body,motion));
    CHECK(std::abs(motion.linearVelocity.Z()-.25f) < .001f);
    // Iyy=M(a*a+c*c)/10=.08, torqueY=-.2, hence omegaY=-2.5.
    CHECK(std::abs(motion.angularVelocity.Y()+2.5f) < .005f);
    backend->RemoveBody(body);
    CHECK(backend->GetStats().articulatedBodies == 0);
}

TEST_CASE("Experimental dependency still records and replays actual articulated solver steps", "[physics][update-replay]")
{
    auto backend = CreatePhysicsBackend();
    REQUIRE(backend);
    REQUIRE(backend->Create());
    const Vector3 points[] = {{.2f,0,0},{-.2f,0,0},{0,.3f,0},{0,-.3f,0},{0,0,.4f},{0,0,-.4f}};
    ConvexPiece piece{points,6,ColliderFlags::Solid};
    Matrix4 frame = MIdentity;
    frame.SetPosition(Vector3(0,3,0));
    BodyId body = backend->SpawnArticulatedPiece(piece,frame,4);
    REQUIRE(body.IsValid());
    REQUIRE(backend->BeginReplayRecording());
    backend->ApplyImpulse(body,Vector3(.2f,3,0),Vector3(0,0,1));
    for (int i=0;i<30;++i) backend->Step(1.0f/60);
    BodyMotion motion;
    REQUIRE(backend->GetBodyMotion(body,motion));
    CHECK(motion.position.Y() < 2);
    const auto replay = backend->EndReplayRecordingAndValidate(1);
    REQUIRE(replay.supported);
    REQUIRE(replay.ran);
    CHECK_FALSE(replay.diverged);
    CHECK(replay.frameCount == 30);
    CHECK(replay.recordedBytes > 0);
}
