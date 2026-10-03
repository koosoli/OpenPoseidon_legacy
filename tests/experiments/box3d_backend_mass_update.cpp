// Explicit experimental executable only; not registered in the default suite
// while the released dependency remains the shipping control.
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <cmath>

using namespace Poseidon::Physics;

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
