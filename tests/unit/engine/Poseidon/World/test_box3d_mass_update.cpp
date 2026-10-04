// Actual backend regression: the released dependency's stale inverse inertia
// produced six times the intended spin after convex hull mass rescaling.
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Simulation/Animation/CorpsePose.hpp>
#include <cmath>
#include <limits>
#include <cstdio>

using namespace Poseidon::Physics;
using namespace Poseidon;

TEST_CASE("Passive articulated angular damping preserves handoff momentum and ballistic gravity", "[physics][articulation][corpse-damping]")
{
    auto backend=CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    const Vector3 vertices[]={{-.2f,-.2f,-.2f},{.2f,-.2f,-.2f},{-.2f,.2f,-.2f},{.2f,.2f,-.2f},
        {-.2f,-.2f,.2f},{.2f,-.2f,.2f},{-.2f,.2f,.2f},{.2f,.2f,.2f}};
    ConvexPiece piece{vertices,8,ColliderFlags::Solid};
    Matrix4 initial=MIdentity; initial.SetPosition(Vector3(0,100,0));
    ArticulatedInitialMotion inherited; inherited.linear=Vector3(2,3,0); inherited.angular=Vector3(0,4,0); inherited.collisionFamily=-2;
    auto limb=backend->SpawnArticulatedPiece(piece,initial,4,inherited); REQUIRE(limb.IsValid());
    Matrix4 controlFrame=initial; controlFrame.SetPosition(Vector3(4,100,0));
    auto control=backend->SpawnDynamicPieces(&piece,1,controlFrame,4,.65f,0,inherited.linear); REQUIRE(control.IsValid());
    // Equal and opposite actual impulses form a pure couple: same initial spin
    // without changing the ordinary dynamic body's linear momentum.
    const float impulse=(4.f*(.4f*.4f+.4f*.4f)/12.f)*4.f/.4f;
    backend->ApplyImpulse(control,controlFrame.Position()+Vector3(.2f,0,0),Vector3(0,0,-impulse));
    backend->ApplyImpulse(control,controlFrame.Position()-Vector3(.2f,0,0),Vector3(0,0,impulse));
    BodyMotion a,b; REQUIRE(backend->GetBodyMotion(limb,a)); REQUIRE(backend->GetBodyMotion(control,b));
    CHECK((a.position-initial.Position()).Size()<.00001f);
    CHECK(a.axisX==initial.DirectionAside()); CHECK(a.axisY==initial.DirectionUp()); CHECK(a.axisZ==initial.Direction());
    CHECK((a.linearVelocity-inherited.linear).Size()<.00001f);
    CHECK((a.angularVelocity-inherited.angular).Size()<.00001f);
    CHECK((b.linearVelocity-a.linearVelocity).Size()<.00001f);
    REQUIRE((b.angularVelocity-a.angularVelocity).Size()<.005f);
    // Momentum transfer is immediate; damping acts only during solver steps.
    backend->ApplyImpulse(limb,a.position,Vector3(4,0,0));
    backend->ApplyImpulse(control,b.position,Vector3(4,0,0));
    REQUIRE(backend->GetBodyMotion(limb,a)); REQUIRE(backend->GetBodyMotion(control,b));
    CHECK(std::abs(a.linearVelocity.X()-3)<.0001f);
    CHECK((a.angularVelocity-inherited.angular).Size()<.00001f);
    for (int step=0;step<120;++step) backend->Step(1.f/60);
    REQUIRE(backend->GetBodyMotion(limb,a)); REQUIRE(backend->GetBodyMotion(control,b));
    CHECK(a.angularVelocity.Size()<.6f);
    CHECK(b.angularVelocity.Size()>3.9f); // ordinary body damping is unchanged
    CHECK((a.linearVelocity-b.linearVelocity).Size()<.002f);
    CHECK(std::abs(a.position.Y()-b.position.Y())<.005f);
    CHECK(a.position.Y()<90); CHECK(a.linearVelocity.Y()<-10); // genuine free fall
    CHECK(std::abs((a.position.X()-initial.Position().X())-(b.position.X()-controlFrame.Position().X()))<.005f);
    backend->RemoveBody(limb); backend->RemoveBody(control);
    CHECK(backend->GetStats().articulatedBodies==0);
    backend->Destroy();
}

TEST_CASE("Automatic blast travel keeps falling past twelve metres and retirement requires actual ground support", "[physics][articulation][corpse-airborne]")
{
    CHECK_FALSE(CorpseAutomaticRetirementAdmitted(false,true,5,20));
    CHECK_FALSE(CorpseAutomaticRetirementAdmitted(true,false,5,20));
    CHECK_FALSE(CorpseAutomaticRetirementAdmitted(true,true,.2f,4));
    CHECK(CorpseAutomaticRetirementAdmitted(true,true,.8f,4));
    CHECK(CorpseAutomaticRetirementAdmitted(true,true,.2f,20));
    auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    constexpr int N = 33; std::vector<float> heights(N*N,0);
    TerrainField floor; floor.heights = heights.data(); floor.width = floor.height = N;
    floor.cellSize = 2; floor.originX = floor.originZ = -4;
    REQUIRE(backend->SetTerrain(floor));
    const Vector3 vertices[] = {{-.2f,-.2f,-.2f},{.2f,-.2f,-.2f},{-.2f,.2f,-.2f},{.2f,.2f,-.2f},
        {-.2f,-.2f,.2f},{.2f,-.2f,.2f},{-.2f,.2f,.2f},{.2f,.2f,.2f}};
    ConvexPiece piece{vertices,8,ColliderFlags::Solid}; Matrix4 initial = MIdentity;
    initial.SetPosition(Vector3(0,.24f,0));
    ArticulatedInitialMotion blast; blast.linear = Vector3(8,10,0); blast.collisionFamily = -2;
    auto body = backend->SpawnArticulatedPiece(piece,initial,4,blast); REQUIRE(body.IsValid());
    float maximumTravel = 0, quietSeconds = 0; bool passedTwelve = false, recovered = false;
    for (int step = 0; step < 600; ++step)
    {
        backend->Step(1.0f/60); BodyMotion motion;
        REQUIRE(backend->GetBodyMotion(body,motion)); REQUIRE(motion.position.IsFinite());
        Vector3 support; const bool supported = backend->GetArticulatedGroundSupport(body,support);
        const bool quiet = motion.linearVelocity.Size() < .06f && motion.angularVelocity.Size() < .12f;
        quietSeconds = supported && quiet ? quietSeconds+1.0f/60 : 0;
        maximumTravel = std::max(maximumTravel,(motion.position-initial.Position()).Size());
        // The exact old travel cutoff must be crossed while still airborne.
        if (maximumTravel > 12 && motion.position.Y() > 1)
        {
            passedTwelve = true; CHECK_FALSE(supported);
            CHECK_FALSE(CorpseAutomaticRetirementAdmitted(supported,quiet,quietSeconds,20+step/60.0f));
        }
        if (CorpseAutomaticRetirementAdmitted(supported,quiet,quietSeconds,20+step/60.0f))
        { recovered = true; CHECK(std::abs(support.Y()) < .025f); CHECK(motion.position.Y() < .5f); break; }
    }
    INFO("actual blast maximumTravel=" << maximumTravel);
    CHECK(passedTwelve); CHECK(recovered); CHECK(maximumTravel > 12);
    backend->RemoveBody(body); Vector3 stale;
    CHECK_FALSE(backend->GetArticulatedGroundSupport(body,stale));
    CHECK(backend->GetStats().articulatedBodies == 0);

    // A side-wall contact is not a gravity support, even at the old time cap.
    Vector3 wallPoints[8]; for (int c = 0; c < 8; ++c)
        wallPoints[c] = Vector3((c&1)?.2f:-.2f,(c&2)?5.f:-5.f,(c&4)?2.f:-2.f);
    ConvexPiece wallPiece{wallPoints,8,ColliderFlags::Solid}; Matrix4 wall = MIdentity;
    wall.SetPosition(Vector3(0,4,0)); auto wallBody = backend->AddStaticBody(&wallPiece,1,wall); REQUIRE(wallBody.IsValid());
    initial.SetPosition(Vector3(.37f,4,0)); blast.linear = VZero;
    body = backend->SpawnArticulatedPiece(piece,initial,4,blast); REQUIRE(body.IsValid());
    for (int step = 0; step < 5; ++step)
    { backend->Step(1.0f/60); Vector3 support; CHECK_FALSE(backend->GetArticulatedGroundSupport(body,support)); }
    backend->RemoveBody(body); backend->RemoveBody(wallBody); backend->Destroy();
}

TEST_CASE("Bounded native boot overlap resolves through real articulated contacts without initial pose displacement", "[physics][articulation][corpse-contact]")
{
    CHECK(CorpseInitialContactAdmitted(true,-.00852966309f));
    CHECK(CorpseInitialContactAdmitted(true,-.0169487f));
    CHECK(CorpseInitialContactAdmitted(true,-.025f));
    CHECK_FALSE(CorpseInitialContactAdmitted(true,-.0251f));
    CHECK_FALSE(CorpseInitialContactAdmitted(false,-.001f));
    CHECK_FALSE(CorpseInitialContactAdmitted(false,.0029f));
    CHECK(CorpseInitialContactAdmitted(false,.003f));
    CHECK_FALSE(CorpseInitialContactAdmitted(true,std::numeric_limits<float>::quiet_NaN()));
    // This is a solver contact-response control, not a stock skin/rig visual
    // proof: two observed native overlaps plus the policy boundary, a boot-sized
    // distal hull, hinged upper segment and connected torso on real terrain.
    for (float overlap : {.00852966309f,.0169487f,.025f})
    {
        auto backend = CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
        float heights[25] = {};
        TerrainField field; field.heights = heights; field.width = field.height = 5;
        field.cellSize = 1; field.originX = field.originZ = -2;
        REQUIRE(backend->SetTerrain(field));
        const Vector3 extents[] = {{.08f,.173f,.13f},{.12f,.225f,.12f},{.22f,.3f,.13f}};
        const float centers[] = {.173f,.573f,1.098f};
        const float masses[] = {4,8,24};
        BodyId bodies[3]; Matrix4 initial[3]; std::vector<Vector3> hulls[3];
        ArticulatedInitialMotion inherited; inherited.collisionFamily = -2;
        for (int part = 0; part < 3; ++part)
        {
            for (int c = 0; c < 8; ++c)
                hulls[part].emplace_back((c&1)?extents[part].X():-extents[part].X(),
                    (c&2)?extents[part].Y():-extents[part].Y(),(c&4)?extents[part].Z():-extents[part].Z());
            initial[part] = MIdentity; initial[part].SetPosition(Vector3(0,centers[part]-overlap,0));
            ConvexPiece piece{hulls[part].data(),8,ColliderFlags::Solid};
            bodies[part] = backend->SpawnArticulatedPiece(piece,initial[part],masses[part],inherited);
            REQUIRE(bodies[part].IsValid()); BodyMotion motion;
            REQUIRE(backend->GetBodyMotion(bodies[part],motion));
            CHECK(motion.position == initial[part].Position());
            CHECK(motion.axisX == VAside); CHECK(motion.axisY == VUp); CHECK(motion.axisZ == VForward);
        }
        JointId joints[2];
        for (int j = 0; j < 2; ++j)
        {
            const float anchor = j == 0 ? .348f-overlap : .798f-overlap;
            ArticulatedJointDef def; def.first = bodies[j]; def.second = bodies[j+1];
            def.firstFrame.SetDirectionAndUp(VAside,VUp); def.secondFrame.SetDirectionAndUp(VAside,VUp);
            def.firstFrame.SetPosition(Vector3(0,anchor-initial[j].Position().Y(),0));
            def.secondFrame.SetPosition(Vector3(0,anchor-initial[j+1].Position().Y(),0));
            if (j == 0) { def.kind = ArticulatedJointKind::Hinge; def.lowerAngle = -.08f; def.upperAngle = 2.4f; }
            joints[j] = backend->AddArticulatedJoint(def); REQUIRE(joints[j].IsValid());
        }
        float maximumSpeed = 0, maximumAnchorError = 0;
        BodyMotion motion[3];
        for (int step = 0; step < 240; ++step)
        {
            backend->Step(1.0f/60);
            for (int part = 0; part < 3; ++part)
            {
                REQUIRE(backend->GetBodyMotion(bodies[part],motion[part]));
                REQUIRE(motion[part].position.IsFinite()); REQUIRE(motion[part].linearVelocity.IsFinite());
                REQUIRE(motion[part].angularVelocity.IsFinite());
                maximumSpeed = std::max(maximumSpeed,motion[part].linearVelocity.Size());
            }
            for (int j = 0; j < 2; ++j)
            {
                const float anchor = j == 0 ? .348f-overlap : .798f-overlap;
                const Vector3 a = motion[j].position+motion[j].axisY*(anchor-initial[j].Position().Y());
                const Vector3 b = motion[j+1].position+motion[j+1].axisY*(anchor-initial[j+1].Position().Y());
                maximumAnchorError = std::max(maximumAnchorError,(a-b).Size());
            }
        }
        float bootBottom = 100;
        for (const auto& p : hulls[0])
            bootBottom = std::min(bootBottom,(motion[0].position+motion[0].axisX*p.X()+motion[0].axisY*p.Y()+motion[0].axisZ*p.Z()).Y());
        INFO("overlap=" << overlap << " bootBottom=" << bootBottom << " maximumSpeed=" << maximumSpeed << " maximumAnchorError=" << maximumAnchorError);
        std::printf("BOOTCONTACT overlap=%.9g finalBottom=%.9g maxSpeed=%.9g maxAnchorError=%.9g\n",
            overlap,bootBottom,maximumSpeed,maximumAnchorError);
        CHECK(bootBottom > -.01f); CHECK(maximumSpeed < 8); CHECK(maximumAnchorError < .025f);
        for (auto joint : joints) backend->RemoveJoint(joint);
        for (auto body : bodies) backend->RemoveBody(body);
        CHECK(backend->GetStats().articulatedBodies == 0); CHECK(backend->GetStats().articulatedJoints == 0);
        backend->Destroy();
    }
}

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
