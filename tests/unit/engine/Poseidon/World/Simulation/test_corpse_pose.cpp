#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Simulation/Animation/CorpsePose.hpp>
#include <Poseidon/World/Simulation/Animation/CorrectedCorpsePose.hpp>
#include <Poseidon/World/Simulation/Animation/StockCorpseRig.hpp>
#include <Poseidon/World/Physics/ProjectileImpulse.hpp>
#include <Poseidon/World/Entities/Infantry/Head.hpp>
#include <Poseidon/World/Physics/CorpseArticulation.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace Poseidon;

// STOCK_CORPSE_CAPABILITY_TESTS_BEGIN: these exercise the production admission helper.
TEST_CASE("Original stock corpse filenames are exact and retain each character family", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    REQUIRE(StockCorpseModels.size() == 38);
    for (auto model : StockCorpseModels)
    {
        CAPTURE(model);
        CHECK(StockCorpseModelKnown(model));
        std::string mixed(model);
        for (char& c : mixed)
            if (c == '\\') c = '/';
            else if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        CHECK(StockCorpseModelKnown(mixed));
        for (auto other : StockCorpseModels)
            if (model != other) CHECK_FALSE(StockCorpseModelNameEqual(model, other));
    }
    for (auto unknown : {"data3d\\vojakw.p3d", "data3d\\mc unknown.p3d", "mod\\mc vojakw2.p3d",
        "o\\char\\char08.p3d", "o\\char\\civilistka02d.p3d", "data3d\\..\\data3d\\mc vojakw2.p3d",
        "data3d\\mc vojakw2.p3d.extra", "data3d\\mc vojakw2.p3d ", ""})
        CHECK_FALSE(StockCorpseModelKnown(unknown));
    CHECK(StockCorpseHasAuthoredHingeEndpoints("DATA3D/MC VOJAKE2.P3D"));
    CHECK(StockCorpseHasAuthoredHingeEndpoints("o_wp\\mc_specg.p3d"));
    for (auto sparse : {"o\\char\\char01.p3d", "o\\char\\civilistka01a.p3d",
        "biscamel\\biscamelpilot2.p3d", "data3d\\angelina.p3d", "unknown.p3d"})
        CHECK_FALSE(StockCorpseHasAuthoredHingeEndpoints(sparse));
}

TEST_CASE("Stock corpse runtime rigs require the complete named body and implemented face ranges", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    std::array<std::string_view,33> bones;
    std::copy(StockCorpseBodyBones.begin(), StockCorpseBodyBones.end(), bones.begin());
    std::copy(StockCorpseFaceBones.begin(), StockCorpseFaceBones.end(), bones.begin()+25);
    auto check = [&](int count = 33) { return StockCorpseSkeletonRefusal(count,
        [&](int bone, std::string_view name) { return bones[bone] == name; }); };
    REQUIRE(check() == nullptr);
    // Every body ordering is supported through name-based production binding.
    for (int offset = 0; offset < 25; ++offset)
    {
        std::rotate(bones.begin(), bones.begin()+1, bones.begin()+25);
        CHECK(check() == nullptr);
    }
    std::reverse(bones.begin()+25, bones.end());
    REQUIRE(check() == nullptr);
    CHECK(std::string_view(check(25)) == "complete-stock-33-bone-palette-required");
    CHECK(std::string_view(check(34)) == "complete-stock-33-bone-palette-required");
    auto valid = bones;
    bones[0] = bones[1];
    CHECK(std::string_view(check()) == "duplicate-stock-body-bone");
    bones = valid; bones[0] = "alternate_head";
    CHECK(std::string_view(check()) == "unknown-stock-body-bone");
    bones = valid; bones[32] = bones[31];
    CHECK(std::string_view(check()) == "duplicate-stock-face-bone");
    bones = valid; bones[32] = "%face_unimplemented";
    CHECK(std::string_view(check()) == "unsupported-synthetic-bone");
    bones = valid; std::swap(bones[0],bones[25]);
    CHECK(std::string_view(check()) == "unknown-stock-body-bone");
}
TEST_CASE("Stock proxy blends require finite normalized weights on one existing solver part", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    // Actual original female RPG helper memberships: hrudnik80/lrameno250/roura255.
    const std::array<std::string_view,3> sourceNames={"hrudnik","lrameno","roura"};
    std::array<int,3> bones{};
    for (int i=0;i<3;++i)
        bones[i]=int(std::find(StockCorpseBodyBones.begin(),StockCorpseBodyBones.end(),sourceNames[i])-StockCorpseBodyBones.begin());
    std::array<float,3> weights={80.f/585,250.f/585,255.f/585};
    auto check=[&](int count=3) { return StockCorpseRigidProxyBone(count,
        [&](int w) { return bones[w]; },[&](int w) { return weights[w]; },
        [&](int bone) { return StockCorpseProxyPart(StockCorpseBodyBones[bone]); }); };
    REQUIRE(check()==bones[0]);
    for (int i=0;i<3;++i) CHECK(StockCorpseProxyPart(sourceNames[i])==1);
    const auto validBones=bones; const auto validWeights=weights;
    bones[1]=int(std::find(StockCorpseBodyBones.begin(),StockCorpseBodyBones.end(),"lbiceps")-StockCorpseBodyBones.begin());
    CHECK(check()==-1); bones=validBones;
    bones[1]=-1; CHECK(check()==-1); bones[1]=25; CHECK(check()==-1); bones=validBones;
    weights[1]=std::numeric_limits<float>::quiet_NaN(); CHECK(check()==-1);
    weights[1]=std::numeric_limits<float>::infinity(); CHECK(check()==-1);
    weights=validWeights; weights[1]=0; CHECK(check()==-1);
    weights=validWeights; weights[1]=-.1f; CHECK(check()==-1);
    weights=validWeights; weights[1]=1.1f; CHECK(check()==-1);
    weights=validWeights; weights[1]+=.01f; CHECK(check()==-1);
    weights=validWeights; CHECK(check(0)==-1); CHECK(check(26)==-1);
    CHECK(StockCorpseRigidProxyBone(1,[](int){return 0;},[](int){return 1.f;},[](int){return -1;})==-1);
    weights[0]=1; REQUIRE(check(1)==bones[0]);
}

TEST_CASE("Captured common-part proxy blends keep exact handoff and owned rigid motion", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    // An authored proxy can already contain a nonrigid weighted native matrix.
    // Ownership preserves it verbatim, then composes the one common body delta.
    auto matrixError=[](Matrix4Val a,Matrix4Val b) {
        float maximum=0;
        for (int row=0;row<3;++row) for (int col=0;col<4;++col)
            maximum=std::max(maximum,std::abs(a(row,col)-b(row,col)));
        return maximum;
    };
    const std::array<int,3> bones={1,3,5};
    const std::array<float,3> weights={.15625f,.48828125f,.35546875f};
    Matrix4 original=MIdentity; original.SetPosition(Vector3(.2f,.4f,-.1f));
    Matrix4 source[3]={Matrix4(MScale,1.1f,.9f,1),MIdentity,Matrix4(MScale,.9f,1.1f,1)};
    source[0].SetPosition(Vector3(.1f,-.2f,.3f)); source[1](0,1)=.2f;
    source[2].SetPosition(Vector3(-.2f,.1f,.2f));
    Matrix4 captured=MZero;
    for (int i=0;i<3;++i) captured+=(source[i]*original)*weights[i];
    CorrectedCorpsePose pose;
    CorrectedCorpseLevel level;
    level.authoredPalette.assign(33,MIdentity); level.palette=level.authoredPalette;
    level.authoredPoints={Vector3(.2f,.1f,.3f)}; level.points=level.authoredPoints;
    level.bindings={{{1},{1.f}}};
    level.proxySelections={0}; level.proxyBones={bones[0]};
    level.authoredProxies={captured}; level.proxies=level.authoredProxies;
    pose.levels.push_back(level); REQUIRE(pose.Valid());
    std::vector<Matrix4> deltas(33,MIdentity); REQUIRE(pose.Apply(deltas));
    CHECK(std::memcmp(&pose.levels[0].proxies[0],&captured,sizeof(Matrix4))==0);
    Matrix4 delta(MDirection,Vector3(.6f,0,.8f),VUp); delta.SetPosition(Vector3(.5f,.2f,-.3f));
    for (int bone : bones) deltas[bone]=delta;
    REQUIRE(pose.Apply(deltas));
    CHECK(matrixError(pose.levels[0].proxies[0],delta*captured)==0);
    Matrix4 nativeMoved=MZero;
    for (int i=0;i<3;++i) nativeMoved+=(delta*source[i]*original)*weights[i];
    CHECK(matrixError(pose.levels[0].proxies[0],nativeMoved)<1e-6f);
    // Repeated updates compose against the retained capture, never last output.
    REQUIRE(pose.Apply(deltas)); CHECK(matrixError(pose.levels[0].proxies[0],delta*captured)==0);
    deltas.assign(33,MIdentity); REQUIRE(pose.Apply(deltas));
    CHECK(std::memcmp(&pose.levels[0].proxies[0],&captured,sizeof(Matrix4))==0);
}

TEST_CASE("Spherical root frame uses full measured midpoint separation without moving its pivot", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    // Actual installed Woman1 joint0 body centres, 20261004 06d41 receipt.
    const Vector3 parent(5500.002f,20.570023f,10000.016f),child(5499.9995f,20.597254f,10000.002f);
    const Vector3 pivot=(parent+child)*.5f;
    REQUIRE((child-pivot).Size()<.025f);
    REQUIRE((child-parent).Size()>=.025f);
    Vector3 axis;
    REQUIRE(CorpseMeasureSphericalRootAxis(parent,child,pivot,axis));
    CHECK((axis-(child-parent).Normalized()).Size()<1e-6f);
    const Vector3 localParent(0,.5f,0),localChild(0,.47f,0),localPivot(0,.485f,0);
    REQUIRE(CorpseMeasureSphericalRootAxis(localParent,localChild,localPivot,axis));
    CHECK((axis-Vector3(0,-1,0)).Size()<1e-6f);
    Matrix4 rotation(MDirection,Vector3(1,0,0),VUp);
    REQUIRE(CorpseMeasureSphericalRootAxis(rotation.Rotate(localParent),rotation.Rotate(localChild),rotation.Rotate(localPivot),axis));
    CHECK((axis-rotation.Rotate(Vector3(0,-1,0))).Size()<1e-6f);
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(localParent,localParent,localParent,axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(VZero,Vector3(0,.024f,0),Vector3(0,.012f,0),axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(localParent,localChild,localPivot+Vector3(.01f,0,0),axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(Vector3(std::numeric_limits<float>::quiet_NaN(),0,0),localChild,localPivot,axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(localParent,Vector3(std::numeric_limits<float>::infinity(),0,0),localPivot,axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(localParent,localChild,Vector3(0,std::numeric_limits<float>::quiet_NaN(),0),axis));
    CHECK_FALSE(CorpseMeasureSphericalRootAxis(VZero,Vector3(3e38f,0,0),Vector3(1.5e38f,0,0),axis));
}
// STOCK_CORPSE_CAPABILITY_TESTS_END

// STOCK_CORPSE_BOUNDARY_TESTS_BEGIN
TEST_CASE("Stock Fire boundary pivots keep distinct paired provenance and actual pose gap", "[corpse-pose][automatic-ragdoll][stock-corpse-capability]")
{
    // Actual decoded Angelina FireGeometry closest chest/head pair. These are
    // source vertices, not Western anatomy or a shared animation intersection.
    const Vector3 a(.538017690f,.173850864f,-.129862994f), b(.538136184f,.234581262f,-.049763508f);
    CorpseAnchorDiagnostic anchor;
    REQUIRE(CorpseMeasureFireBoundary({a},{b},{a},{b},anchor));
    CHECK(anchor.sharedVertices == 0);
    CHECK(anchor.sourcePairCount == 1);
    CHECK(anchor.source == "authored-fire-boundary-pair");
    CHECK(std::abs(anchor.separation-.100519266f)<.000001f);
    CHECK((anchor.modelAnchor-(a+b)*.5f).Size()<.000001f);
    // Transform each OWN source point through its actual part pose. The helper
    // preserves both witnesses; it cannot apply one front palette to both.
    const Vector3 shift(100,25,200);
    REQUIRE(CorpseMeasureFireBoundary({a},{b},{a+shift},{b+shift},anchor));
    CHECK((anchor.firstWorld-(a+shift)).Size()<.000001f);
    CHECK((anchor.secondWorld-(b+shift)).Size()<.000001f);
    CHECK_FALSE(CorpseMeasureFireBoundary({a},{b},{a},{b+Vector3(0,.2f,0)},anchor));
    CHECK_FALSE(CorpseMeasureFireBoundary({a},{b+Vector3(0,.2f,0)},{a},{b},anchor));
    CHECK_FALSE(CorpseMeasureFireBoundary({a},{b},{},{b},anchor));
    CHECK_FALSE(CorpseMeasureFireBoundary({}, {b},{},{b},anchor));
    CHECK_FALSE(CorpseMeasureFireBoundary({Vector3(std::numeric_limits<float>::quiet_NaN(),0,0)},{b},{a},{b},anchor));
    const Vector3 huge(3e38f,0,0);
    CHECK_FALSE(CorpseMeasureFireBoundary({huge},{huge},{VZero},{VZero},anchor));
    CHECK_FALSE(CorpseMeasureFireBoundary({a,a},{b,b},{huge,huge},{huge,huge},anchor));
    // A whole face of tied closest points has a centroid, not whichever vertex
    // happens to appear first. Dispersed unrelated surfaces are refused.
    std::vector<Vector3> first={Vector3(-.05f,0,0),Vector3(.05f,0,0)};
    std::vector<Vector3> second={Vector3(-.05f,.06f,0),Vector3(.05f,.06f,0)};
    REQUIRE(CorpseMeasureFireBoundary(first,second,first,second,anchor));
    CHECK(anchor.sourcePairCount==2);
    CHECK((anchor.modelAnchor-Vector3(0,.03f,0)).Size()<.000001f);
    std::reverse(first.begin(),first.end());
    CorpseAnchorDiagnostic reordered;
    REQUIRE(CorpseMeasureFireBoundary(first,second,first,second,reordered));
    CHECK((anchor.modelAnchor-reordered.modelAnchor).Size()<.000001f);
    first={Vector3(-.4f,0,0),Vector3(.4f,0,0)};
    second={Vector3(-.4f,.06f,0),Vector3(.4f,.06f,0)};
    CHECK_FALSE(CorpseMeasureFireBoundary(first,second,first,second,anchor));
    first.assign(129,a);second={b};
    CHECK_FALSE(CorpseMeasureFireBoundary(first,second,first,second,anchor));
}
// STOCK_CORPSE_BOUNDARY_TESTS_END

TEST_CASE("Corrected corpse consumers retain distinct affine point and proxy arithmetic", "[corpse-pose][automatic-ragdoll]")
{
    CorrectedCorpsePose pose;
    CorrectedCorpseLevel level;
    Matrix4 first = MIdentity, second = MIdentity;
    first(0,1) = .18f; first(1,1) = 1.13f; second(2,0) = -.21f;
    first.SetPosition(Vector3(.23f,.67f,-.29f)); second.SetPosition(Vector3(-.32f,.81f,.47f));
    level.authoredPalette = level.palette = {first,second};
    level.authoredPoints = level.points = {Vector3(.36f,.84f,-.44f),Vector3(.11f,.76f,.38f)};
    level.bindings = {{{0,1},{.25f,.75f}},{{1},{1}}};
    level.proxyBones = {1}; level.proxySelections = {7};
    Matrix4 equipment = MIdentity; equipment.SetPosition(Vector3(.71f,.97f,-.16f));
    level.authoredProxies = level.proxies = {equipment};
    pose.levels.push_back(level);
    REQUIRE(pose.Valid());
    std::vector<Matrix4> identity(2,MIdentity);
    REQUIRE(pose.Apply(identity));
    CHECK(std::memcmp(pose.levels[0].palette.data(),level.palette.data(),2*sizeof(Matrix4)) == 0);
    CHECK(std::memcmp(pose.levels[0].points.data(),level.points.data(),2*sizeof(Vector3)) == 0);
    CHECK(std::memcmp(pose.levels[0].proxies.data(),level.proxies.data(),sizeof(Matrix4)) == 0);
    Matrix4 a(MRotationY,.31f), b(MRotationX,-.27f);
    a.SetPosition(Vector3(.09f,-.07f,.14f)); b.SetPosition(Vector3(-.11f,-.04f,.17f));
    REQUIRE(pose.Apply({a,b}));
    const Vector3 expected = a.FastTransform(level.points[0])*.25f+b.FastTransform(level.points[0])*.75f;
    CHECK((pose.levels[0].points[0]-expected).Size() < .000001f);
    const Matrix4 expectedProxy = b*equipment;
    CHECK(std::memcmp(&pose.levels[0].proxies[0],&expectedProxy,sizeof(Matrix4)) == 0);
    const auto once = pose.levels[0].points;
    REQUIRE(pose.Apply({a,b}));
    CHECK(std::memcmp(once.data(),pose.levels[0].points.data(),2*sizeof(Vector3)) == 0);
    REQUIRE(pose.Apply(identity));
    CHECK(std::memcmp(pose.levels[0].points.data(),level.points.data(),2*sizeof(Vector3)) == 0);
    pose.levels[0].bindings[0].weights[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(pose.Valid()); CHECK_FALSE(pose.Apply(identity));
}

namespace
{
// Real RTM_0101, loaded by the production AnimationRT constructor. The matrices
// intentionally include nonuniform scale, shear and translation in both phases.
struct NonrigidRTM
{
    std::filesystem::path path;
    Ref<Skeleton> skeleton = new Skeleton("corpse-pose-test");
    Ref<AnimationRT> animation;
    NonrigidRTM()
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("corpse-pose-" + std::to_string(stamp) + ".rtm");
        std::ofstream file(path, std::ios::binary);
        REQUIRE(file.good());
        auto write = [&](const void* p, size_t size) { file.write(static_cast<const char*>(p), size); };
        auto name = [&](const char* value)
        {
            char field[32] = {};
            std::strncpy(field, value, sizeof(field) - 1);
            write(field, sizeof(field));
        };
        write("RTM_0101", 8);
        float step[3] = {};
        write(step, sizeof(step));
        int phases = 2, bones = 2;
        write(&phases, sizeof(phases));
        write(&bones, sizeof(bones));
        name("bone_a"); name("bone_b");
        static_assert(sizeof(Matrix4) == sizeof(float) * 12);
        for (int phase = 0; phase < 2; ++phase)
        {
            float time = static_cast<float>(phase);
            write(&time, sizeof(time));
            for (int bone = 0; bone < 2; ++bone)
            {
                name(bone ? "bone_b" : "bone_a");
                Matrix4 matrix(MScale, 1.23f + phase * 0.13f, 0.81f + bone * 0.22f, 1.09f);
                matrix(0, 1) = 0.23f + bone * 0.17f;
                matrix(2, 0) = -0.11f - phase * 0.08f;
                matrix.SetPosition(Vector3(phase * 0.31f, bone * -0.74f, 0.43f));
                write(&matrix, sizeof(matrix));
            }
        }
        file.close();
        AnimationRTName source;
        source.name = path.string().c_str();
        source.skeleton = skeleton;
        animation = new AnimationRT(source, false);
        REQUIRE(animation->GetKeyframeCount() == 2);
        REQUIRE(skeleton->NBones() == 2);
        skeleton->NewBone("synthetic_face_tail");
    }
    ~NonrigidRTM()
    {
        animation = nullptr;
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

void SameMatrix(const Matrix4& a, const Matrix4& b)
{
    REQUIRE(std::memcmp(&a, &b, sizeof(Matrix4)) == 0);
}
}

TEST_CASE("Corpse diagnostic dead-face evaluation is production-equivalent without stealing lip state or counters",
          "[corpse-pose][animation][face-rig]")
{
    HeadType type;
    Head head(type, nullptr);
    head._lipInfo = new ManLipInfo;
    ManLipInfo* lip = head._lipInfo;
    head._mimicPhase = 0.37f;
    head._lBrowOld = Vector3(0.011f, -0.013f, 0.017f);
    head._lBrow = Vector3(-0.019f, 0.023f, 0.029f);
    head._faceBonesActive = true;
    WeightInfo weights;
    weights._faceBoneBase = 1;
    for (int group = 0; group < NFaceGroups; ++group) weights._faceParent[0][group] = 0;
    MATRIX_4_ARRAY(actual, 128);
    MATRIX_4_ARRAY(evaluated, 128);
    actual.Resize(1 + NFaceGroups); evaluated.Resize(1 + NFaceGroups);
    for (int i = 0; i < actual.Size(); ++i)
    {
        actual[i] = Matrix4(MScale, 1.2f, 0.8f, 1.1f);
        actual[i](0, 1) = 0.21f;
        actual[i].SetPosition(Vector3(0.31f, -0.57f, 0.73f));
        evaluated[i] = actual[i];
    }
    const auto offsets = GFaceDiag().offsets;
    REQUIRE(head.EvaluateDeadFaceBones(type, weights, 0, evaluated, true));
    REQUIRE(head._faceBonesActive);
    REQUIRE(static_cast<ManLipInfo*>(head._lipInfo) == lip);
    REQUIRE(GFaceDiag().offsets == offsets);
    REQUIRE(head.ApplyFaceBones(type, weights, 0, actual, true));
    for (int i = 0; i < actual.Size(); ++i) SameMatrix(actual[i], evaluated[i]);
    REQUIRE(static_cast<ManLipInfo*>(head._lipInfo) == lip);

    // Nondead evaluation must never sample GetPhase or mutate the output/state.
    Matrix4 unchanged = evaluated[0];
    REQUIRE_FALSE(head.EvaluateDeadFaceBones(type, weights, 0, evaluated, false));
    SameMatrix(unchanged, evaluated[0]);
    REQUIRE(static_cast<ManLipInfo*>(head._lipInfo) == lip);
    weights._faceBoneBase = evaluated.Size();
    REQUIRE_FALSE(head.EvaluateDeadFaceBones(type, weights, 0, evaluated, true));
    REQUIRE(head._faceBonesActive);
    SameMatrix(unchanged, evaluated[0]);
}

TEST_CASE("Corpse diagnostic measures the actual mixed-weight correction discrepancy rather than assuming final LBS equivalence",
          "[corpse-pose][animation]")
{
    LODShape lod;
    Shape* shape = new Shape;
    shape->Init(1);
    const Vector3 original(0.23f, -0.47f, 0.61f);
    shape->SetPos(0) = original;
    SelInfo a[] = {SelInfo(0, 173)}, b[] = {SelInfo(0, 82)};
    shape->AddNamedSel(NamedSelection("a", a, 1, nullptr, 0));
    shape->AddNamedSel(NamedSelection("b", b, 1, nullptr, 0));
    lod.AddShape(shape, 0);
    WeightInfo weights;
    weights[0].Init(shape);
    weights[0].AddSelection(shape, 0, 0);
    weights[0].AddSelection(shape, 1, 1);
    weights[0].Normalize();
    MATRIX_4_ARRAY(base, 128);
    base.Resize(2);
    base[0] = Matrix4(MScale, 1.2f, 0.8f, 1.1f);
    base[0](0, 1) = 0.27f;
    base[1] = Matrix4(MTranslation, Vector3(0.73f, -0.37f, 0.19f));
    MATRIX_4_ARRAY(corrected, 128);
    corrected.Resize(2); corrected[0] = base[0]; corrected[1] = base[1];
    const Matrix4 correction(MRotationY, 0.43f);
    BlendAnimInfo blend[2];
    blend[0].matrixIndex = 0; blend[0].factor = 1;
    blend[1].matrixIndex = 1; blend[1].factor = 0.23f;
    Vector3 point = AnimationRT::ApplyMatricesPoint(weights[0], &lod, 0, base, 0);
    AnimationRT::TransformPoint(point, weights[0], shape, correction, blend, 2, 0);
    AnimationRT::CombineTransform(weights, &lod, 0, corrected, correction, blend, 2);
    const Vector3 palettePoint = AnimationRT::ApplyMatricesPoint(weights[0], &lod, 0, corrected, 0);
    REQUIRE(point.IsFinite()); REQUIRE(palettePoint.IsFinite());
    REQUIRE((point - palettePoint).Size() > 0.001f);
    REQUIRE(CorpseMatrixIdentityError(MIdentity) == 0);
    REQUIRE(CorpseMatrixIdentityError(base[0]) > 0.2f);
    REQUIRE(CorpseMatrixIdentityError(base[1]) > 0.7f);
    REQUIRE(CorpseMatrixDeterminant(MIdentity) == 1);
    REQUIRE(std::abs(CorpseMatrixDeterminant(base[0]) - 1.056f) < 0.000001f);
}

TEST_CASE("Corpse phase capture preserves actual nonrigid RTM palettes and additive blend order", "[corpse-pose][animation]")
{
    NonrigidRTM fixture;
    for (float time : {0.0f, 0.125f, 0.375f, 0.9f, 1.0f})
    {
        AffinePhasePose primary, secondary;
        REQUIRE(fixture.animation->CapturePhasePose(primary, time));
        REQUIRE(fixture.animation->CapturePhasePose(secondary, 0.615f));
        REQUIRE(primary.PhaseMatrices() == 2);
        REQUIRE(primary.Bones() == 3);
        for (float factor : {0.0f, 0.01f, 0.0101f, 0.37f, 0.99f, 1.0f})
        {
            MATRIX_4_ARRAY(original, 128);
            MATRIX_4_ARRAY(captured, 128);
            // Exact Man primary/secondary gate order; exercise empty and additive arrays.
            if (factor > 0.01f)
            {
                fixture.animation->PrepareMatrices(original, time, factor);
                primary.PrepareMatrices(captured, factor);
            }
            if (factor < 0.99f)
            {
                fixture.animation->PrepareMatrices(original, 0.615f, 1 - factor);
                secondary.PrepareMatrices(captured, 1 - factor);
            }
            REQUIRE(original.Size() == captured.Size());
            for (int i = 0; i < original.Size(); ++i) SameMatrix(original[i], captured[i]);
        }
    }
}

TEST_CASE("Corpse phase points and proxy matrices preserve production multiweight arithmetic and tail fallback", "[corpse-pose][animation]")
{
    NonrigidRTM fixture;
    LODShape lod;
    Shape* shape = new Shape;
    shape->Init(2);
    shape->SetPos(0) = Vector3(1.37f, -0.81f, 0.49f);
    shape->SetPos(1) = Vector3(-0.24f, 0.14f, 0.65f);
    SelInfo a[] = {SelInfo(0, 173)}, b[] = {SelInfo(0, 82)};
    shape->AddNamedSel(NamedSelection("bone_a", a, 1, nullptr, 0));
    shape->AddNamedSel(NamedSelection("bone_b", b, 1, nullptr, 0));
    lod.AddShape(shape, 0);
    WeightInfo weights;
    weights[0].Init(shape);
    weights[0].AddSelection(shape, 0, 0);
    weights[0].AddSelection(shape, 1, 1);
    weights[0].Normalize();
    REQUIRE(weights[0][0].Size() == 2);
    AnimationRTWeight tail;
    tail.Add(AnimationRTPair(2, 0.75f));
    tail.Add(AnimationRTPair(1, 0.25f));
    for (float time : {0.0f, 0.127f, 0.539f, 0.99f, 1.0f})
    {
        AffinePhasePose pose;
        REQUIRE(fixture.animation->CapturePhasePose(pose, time));
        REQUIRE(pose.SupportsPoint(weights[0][0]));
        REQUIRE_FALSE(pose.SupportsPoint(tail));
        REQUIRE(pose.SupportsMatrix(tail));
        Vector3 original = fixture.animation->Point(weights, &lod, 0, time, 0);
        Vector3 captured = pose.Point(weights[0][0], shape->OrigPos(0), shape->Pos(0));
        REQUIRE(original.X() == captured.X());
        REQUIRE(original.Y() == captured.Y());
        REQUIRE(original.Z() == captured.Z());
        // Unweighted Point returns the CURRENT position, not the saved original.
        shape->SetPos(1) = Vector3(2.4f, -1.7f, 0.3f);
        original = fixture.animation->Point(weights, &lod, 0, time, 1);
        captured = pose.Point(weights[0][1], shape->OrigPos(1), shape->Pos(1));
        REQUIRE(original.X() == captured.X());
        REQUIRE(original.Y() == captured.Y());
        REQUIRE(original.Z() == captured.Z());
        const AnimationRTWeight* cases[] = {&weights[0][0], &tail};
        for (const AnimationRTWeight* weight : cases)
        {
            Matrix4 a(MScale, 1.27f, 0.89f, 1.05f);
            a(1, 2) = -0.213f;
            a.SetPosition(Vector3(0.17f, -0.9f, 0.63f));
            Matrix4 b = a;
            fixture.animation->Matrix(a, time, *weight);
            pose.Matrix(b, *weight);
            SameMatrix(a, b);
        }
    }
}

TEST_CASE("Corpse captures own their affine data and reject invalid recapture", "[corpse-pose][animation]")
{
    Matrix4 previous = MIdentity, next(MScale, 1.3f, 0.7f, 1.1f);
    AffinePhasePose pose;
    REQUIRE(pose.Capture(&previous, &next, 1, 1, 0.37f));
    MATRIX_4_ARRAY(before, 128);
    pose.PrepareMatrices(before, 1);
    previous = MZero; next = MZero;
    MATRIX_4_ARRAY(after, 128);
    pose.PrepareMatrices(after, 1);
    SameMatrix(before[0], after[0]);
    next(0, 0) = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(pose.Capture(&previous, &next, 1, 1, 0.5f));
    REQUIRE_FALSE(pose.Valid());
    REQUIRE(pose.PhaseMatrices() == 0);
    REQUIRE_FALSE(pose.Capture(&previous, &previous, 1, 129, 0.5f));
    REQUIRE_FALSE(pose.Capture(&previous, &previous, 1, 1, std::numeric_limits<float>::infinity()));
    AnimationRT empty;
    REQUIRE_FALSE(empty.CapturePhasePose(pose, 0));
    pose.Clear();
    REQUIRE_FALSE(pose.Valid());
}

TEST_CASE("Corpse lease rejects stale worlds models skeletons moves and clock rewinds", "[corpse-pose][lifetime]")
{
    int world = 1, model = 2, skeleton = 3, other = 4;
    CorpsePoseLease lease{&world, &model, &skeleton, 25, 7, 9, 10000, 0.37f};
    auto valid = [&](const void* w, const void* m, const void* s, int bones, int p, int q, float f, int now)
        { return lease.Matches(w, m, s, bones, p, q, f, now); };
    REQUIRE(valid(&world, &model, &skeleton, 25, 7, 9, 0.37f, 10000));
    REQUIRE(valid(&world, &model, &skeleton, 25, 7, 9, 0.37f, 40000));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 7, 9, 0.37f, 40001));
    REQUIRE(lease.MatchesIdentity(&world,&model,&skeleton,25,7,9,.37f,40001));
    REQUIRE(lease.MatchesIdentity(&world,&model,&skeleton,25,7,9,.37f,1000000));
    REQUIRE_FALSE(lease.MatchesIdentity(&other,&model,&skeleton,25,7,9,.37f,40001));
    REQUIRE_FALSE(lease.MatchesIdentity(&world,&other,&skeleton,25,7,9,.37f,40001));
    REQUIRE_FALSE(lease.MatchesIdentity(&world,&model,&other,25,7,9,.37f,40001));
    REQUIRE_FALSE(lease.MatchesIdentity(&world,&model,&skeleton,25,7,9,.37f,9999));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 7, 9, 0.37f, 9999));
    REQUIRE_FALSE(valid(&other, &model, &skeleton, 25, 7, 9, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &other, &skeleton, 25, 7, 9, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &model, &other, 25, 7, 9, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 33, 7, 9, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 8, 9, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 7, 8, 0.37f, 10001));
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 7, 9, 0.38f, 10001));
    lease = {};
    REQUIRE_FALSE(valid(&world, &model, &skeleton, 25, 7, 9, 0.37f, 10001));
}

TEST_CASE("Corpse rigid deltas preserve affine residuals and every weighted consumer without accumulation", "[corpse-pose][articulation]")
{
    Matrix4 previous[2] = {Matrix4(MScale,1.3f,0.7f,1.1f), MIdentity};
    Matrix4 next[2] = {MIdentity,Matrix4(MScale,0.8f,1.2f,0.9f)};
    previous[0](0,1) = 0.21f; next[1](1,2) = -0.13f;
    previous[0].SetPosition(Vector3(.2f,.3f,-.4f)); next[1].SetPosition(Vector3(-.3f,.2f,.5f));
    AffinePhasePose pose, original;
    REQUIRE(pose.Capture(previous,next,2,3,.37f)); REQUIRE(original.Capture(previous,next,2,3,.37f));
    AnimationRTWeight weight;
    weight.Add(AnimationRTPair(0,.675f)); weight.Add(AnimationRTPair(1,.325f));
    std::vector<Matrix4> deltas(3,MIdentity);
    REQUIRE(pose.ApplyRigidDeltas(deltas));
    MATRIX_4_ARRAY(a,128); MATRIX_4_ARRAY(b,128);
    original.PrepareMatrices(a,1); pose.PrepareMatrices(b,1);
    for (int i=0;i<3;++i) SameMatrix(a[i],b[i]);
    const Vector3 point(.17f,-.29f,.81f);
    CHECK(pose.Point(weight,point,point) == original.Point(weight,point,point));
    Matrix4 pa = MIdentity, pb = pa;
    original.Matrix(pa,weight); pose.Matrix(pb,weight); SameMatrix(pa,pb);
    deltas[0] = Matrix4(MDirection,Vector3(1,0,0),VUp); deltas[0].SetPosition(Vector3(.1f,.2f,.3f));
    deltas[1] = Matrix4(MDirection,Vector3(0,.6f,.8f),Vector3(0,.8f,-.6f));
    deltas[2] = deltas[0];
    Matrix4 movedPrevious[2], movedNext[2];
    for (int i=0;i<2;++i) { movedPrevious[i]=deltas[i]*previous[i]; movedNext[i]=deltas[i]*next[i]; }
    AffinePhasePose expected;
    REQUIRE(expected.Capture(movedPrevious,movedNext,2,3,.37f));
    REQUIRE(pose.ApplyRigidDeltas(deltas)); REQUIRE(pose.ApplyRigidDeltas(deltas));
    MATRIX_4_ARRAY(c,128); MATRIX_4_ARRAY(d,128);
    pose.PrepareMatrices(c,1); expected.PrepareMatrices(d,1);
    SameMatrix(c[0],d[0]); SameMatrix(c[1],d[1]); SameMatrix(c[2],deltas[2]);
    CHECK(pose.Point(weight,point,point) == expected.Point(weight,point,point));
    pa = pb = MIdentity; pose.Matrix(pa,weight); expected.Matrix(pb,weight); SameMatrix(pa,pb);
    AnimationRTWeight face; face.Add(AnimationRTPair(2,1));
    pa = MIdentity; pose.Matrix(pa,face); SameMatrix(pa,deltas[2]);
    deltas[0](0,0) = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(pose.ApplyRigidDeltas(deltas));
    MATRIX_4_ARRAY(e,128); pose.PrepareMatrices(e,1);
    for (int i=0;i<3;++i) SameMatrix(c[i],e[i]);
    REQUIRE(pose.ApplyRigidDeltas(std::vector<Matrix4>(3,MIdentity)));
    MATRIX_4_ARRAY(f,128); pose.PrepareMatrices(f,1);
    for (int i=0;i<3;++i) SameMatrix(a[i],f[i]);
}

TEST_CASE("Frozen runtime recipes restore eleven bodies at retained transforms and real local impact moves them", "[corpse-pose][articulation][corpse-wake]")
{
    using namespace Poseidon::Physics;
    auto& world=EnsurePhysicsWorld(); world.Destroy(); REQUIRE(world.Create());
    struct Retire { PhysicsWorld& world; ~Retire(){world.Destroy();} } cleanup{world};
    std::vector<float> floor(33*33,0); REQUIRE(world.SetTerrain(floor.data(),33,33,.5f,-8,-8));
    CorpseArticulation state; state.world=&world; state.epoch=world.Generation();
    state.terrainSerial=world.TerrainMutationSerial(); state.wakeRecipe=true;
    const Vector3 centers[]={{0,1.1f,0},{0,1.5f,0},{0,1.8f,0},{-.25f,1.45f,0},{-.45f,1.35f,0},
        {.25f,1.45f,0},{.45f,1.35f,0},{-.12f,.8f,0},{-.12f,.4f,0},{.12f,.8f,0},{.12f,.4f,0}};
    const Vector3 vertices[]={{-.04f,-.08f,-.04f},{.04f,-.08f,-.04f},{-.04f,.08f,-.04f},{.04f,.08f,-.04f},
        {-.04f,-.08f,.04f},{.04f,-.08f,.04f},{-.04f,.08f,.04f},{.04f,.08f,.04f}};
    for(int part=0;part<11;++part)
    {
        state.retainedHulls[part].assign(vertices,vertices+8); state.retainedMasses[part]=2;
        Matrix4 frame; frame.SetRotationY(.3f); frame.SetPosition(centers[part]);
        state.initial[part]=frame;
        ConvexPiece piece{vertices,8,ColliderFlags::Solid}; ArticulatedInitialMotion motion; motion.collisionFamily=-2;
        state.bodies[part]=world.SpawnArticulatedPiece(piece,frame,2,motion); REQUIRE(state.bodies[part].IsValid());
    }
    const int pairs[][2]={{0,1},{1,2},{1,3},{3,4},{1,5},{5,6},{0,7},{7,8},{0,9},{9,10}};
    for(int j=0;j<10;++j)
    {
        const int a=pairs[j][0],b=pairs[j][1]; Matrix4 pivot=MIdentity; pivot.SetPosition((centers[a]+centers[b])*.5f);
        auto& definition=state.retainedJoints[j]; definition.first=state.bodies[a]; definition.second=state.bodies[b];
        definition.firstFrame=state.initial[a].InverseRotation()*pivot;
        definition.secondFrame=state.initial[b].InverseRotation()*pivot;
        if(j==3 || j==5 || j==7 || j==9) { definition.kind=ArticulatedJointKind::Hinge; definition.lowerAngle=-.2f; definition.upperAngle=.4f; }
        state.joints[j]=world.AddArticulatedJoint(definition); REQUIRE(state.joints[j].IsValid());
    }
    for(int i=0;i<20;++i) world.Step(1.f/60);
    for(int part=0;part<11;++part)
    {
        BodyMotion motion; REQUIRE(world.GetBodyMotion(state.bodies[part],motion));
        auto& frame=state.retainedTransforms[part]; frame=MIdentity; frame.SetDirectionAside(motion.axisX);
        frame.SetDirectionUp(motion.axisY); frame.SetDirection(motion.axisZ); frame.SetPosition(motion.position);
    }
    state.updates=20; state.minimum=Vector3(-1,-1,-1); state.maximum=Vector3(1,1,1); state.radius=1.8f;
    REQUIRE(state.Freeze()); CHECK(world.GetStats().articulatedBodies==0); CHECK(world.GetStats().articulatedJoints==0);
    // Corrected consumers are retained independently of solver reconstruction.
    CorrectedCorpsePose pose; CorrectedCorpseLevel level;
    level.authoredPalette.assign(11,MIdentity); level.authoredPalette[4]=Matrix4(MScale,1.2f,.8f,1.1f);
    level.authoredPalette[4](0,1)=.13f; level.palette=level.authoredPalette;
    level.authoredPoints={Vector3(.1f,-.2f,.3f)}; level.points=level.authoredPoints;
    level.bindings={CorpsePointBinding{{4,2},{.65f,.35f}}};
    level.authoredProxies={level.palette[4]}; level.proxies=level.authoredProxies; level.proxySelections={1}; level.proxyBones={4};
    pose.levels={level}; REQUIRE(pose.Valid());
    const auto before=pose;
    REQUIRE_FALSE(state.Wake(-1)); REQUIRE(state.Wake(-3));
    CHECK(world.GetStats().articulatedBodies==11); CHECK(world.GetStats().articulatedJoints==10);
    CHECK(pose.levels[0].points==before.levels[0].points);
    for(int part=0;part<11;++part)
    {
        BodyMotion motion; REQUIRE(world.GetBodyMotion(state.bodies[part],motion));
        CHECK((motion.position-state.retainedTransforms[part].Position()).Size()<.0001f);
        CHECK((motion.axisX-state.retainedTransforms[part].DirectionAside()).Size()<.0001f);
        CHECK((motion.axisY-state.retainedTransforms[part].DirectionUp()).Size()<.0001f);
        CHECK((motion.axisZ-state.retainedTransforms[part].Direction()).Size()<.0001f);
    }
    REQUIRE(pose.Apply(std::vector<Matrix4>(11,MIdentity)));
    for(int bone=0;bone<11;++bone) SameMatrix(pose.levels[0].palette[bone],before.levels[0].palette[bone]);
    SameMatrix(pose.levels[0].proxies[0],before.levels[0].proxies[0]);
    Vector3 impact; const Vector3 force(0,0,1),center=VZero;
    const Vector3 target=state.retainedTransforms[4].Position();
    REQUIRE(state.ImpactPart(center,force,(target-center).CrossProduct(force),impact)==4);
    CHECK((impact-target).Size()<.0001f);
    const auto starting=state.initial[4].Position(); float appliedLinear,appliedAngular;
    REQUIRE(state.TransferImpulse(center,force,(target-center).CrossProduct(force),force/22,VZero,1,
        appliedLinear,appliedAngular)==4);
    CHECK(state.localTransfers==1); CHECK(state.lastImpactPart==4);
    CHECK(std::abs(appliedLinear-.5f)<.0001f); CHECK(appliedAngular<.001f);
    BodyMotion immediate; REQUIRE(world.GetBodyMotion(state.bodies[4],immediate));
    CHECK(immediate.position==state.initial[4].Position());
    CHECK(std::abs(immediate.linearVelocity.Z()-.5f)<.0001f);
    CHECK(pose.levels[0].points==before.levels[0].points);
    // The same shared production helper refuses unbounded whole-body motion.
    CHECK(state.TransferImpulse(center,force,VZero,Vector3(16,0,0),VZero,1,appliedLinear,appliedAngular)==-2);
    CHECK(state.localTransfers==1);
    for(int i=0;i<12;++i) world.Step(1.f/60);
    BodyMotion moved; REQUIRE(world.GetBodyMotion(state.bodies[4],moved)); CHECK((moved.position-starting).Size()>.01f);
    Vector3 invalid; CHECK(state.ImpactPart(center,VZero,VZero,invalid)==-1);
    REQUIRE(state.Freeze()); REQUIRE(world.PatchTerrain(0,0,1,1,floor.data()));
    CHECK_FALSE(state.Wake(-2)); CHECK(world.GetStats().articulatedBodies==0); CHECK(world.GetStats().articulatedJoints==0);
    world.Destroy(); CHECK_FALSE(state.Wake(-2));
}

TEST_CASE("Explicit corpse freeze retires real eleven-body ownership and retains both affine phases", "[corpse-pose][articulation][corpse-freeze]")
{
    using namespace Poseidon::Physics;
    auto& world = EnsurePhysicsWorld(); world.Destroy(); REQUIRE(world.Create());
    struct RetireWorld { PhysicsWorld& world; ~RetireWorld() { world.Destroy(); } } cleanup{world};
    auto state = std::make_unique<CorpseArticulation>();
    state->world = &world; state->epoch = world.Generation();
    const Vector3 vertices[] = {{-.04f,-.08f,-.04f},{.04f,-.08f,-.04f},{-.04f,.08f,-.04f},{.04f,.08f,-.04f},
        {-.04f,-.08f,.04f},{.04f,-.08f,.04f},{-.04f,.08f,.04f},{.04f,.08f,.04f}};
    ConvexPiece piece{vertices,8,ColliderFlags::Solid};
    for (int i=0;i<11;++i)
    {
        state->initial[i]=MIdentity; state->initial[i].SetPosition(Vector3(0,5+i*.25f,0));
        state->bodies[i]=world.SpawnArticulatedPiece(piece,state->initial[i],2);
        REQUIRE(state->bodies[i].IsValid());
    }
    for (int i=0;i<10;++i)
    {
        ArticulatedJointDef joint; joint.first=state->bodies[i]; joint.second=state->bodies[i+1];
        joint.firstFrame.SetPosition(Vector3(0,.125f,0)); joint.secondFrame.SetPosition(Vector3(0,-.125f,0));
        if (i<4) { joint.kind=ArticulatedJointKind::Hinge; joint.lowerAngle=-.2f; joint.upperAngle=.4f; }
        state->joints[i]=world.AddArticulatedJoint(joint); REQUIRE(state->joints[i].IsValid());
    }
    CHECK(world.GetStats().articulatedBodies==11); CHECK(world.GetStats().articulatedJoints==10);
    CHECK(world.GetStats().solverBodies==11); CHECK(world.GetStats().solverJoints==10);
    world.ApplyImpulse(state->bodies[5],state->initial[5].Position(),Vector3(.4f,1,0));
    for (int i=0;i<12;++i) world.Step(1.0f/60);
    std::vector<Matrix4> deltas(12,MIdentity);
    for (int i=0;i<11;++i)
    {
        BodyMotion motion; REQUIRE(world.GetBodyMotion(state->bodies[i],motion));
        Matrix4 current=MIdentity; current.SetDirectionAside(motion.axisX); current.SetDirectionUp(motion.axisY);
        current.SetDirection(motion.axisZ); current.SetPosition(motion.position);
        deltas[i]=current*state->initial[i].InverseRotation();
    }
    deltas[11]=deltas[2]; // actual synthetic-tail delta belongs to the owned phase
    CHECK(deltas[5].Position().Size()>.001f);
    Matrix4 previous[11], next[11];
    for (int i=0;i<11;++i) { previous[i]=Matrix4(MScale,1.2f,.8f,1.1f); previous[i](0,1)=.13f; next[i]=MIdentity; }
    AffinePhasePose primary, secondary;
    REQUIRE(primary.Capture(previous,next,11,12,.37f)); REQUIRE(secondary.Capture(next,previous,11,12,.61f));
    REQUIRE(primary.ApplyRigidDeltas(deltas)); REQUIRE(secondary.ApplyRigidDeltas(deltas));
    MATRIX_4_ARRAY(palette,128); MATRIX_4_ARRAY(secondPalette,128);
    primary.PrepareMatrices(palette,1); secondary.PrepareMatrices(secondPalette,1);
    AnimationRTWeight weights; weights.Add(AnimationRTPair(5,.65f)); weights.Add(AnimationRTPair(11,.35f));
    const Vector3 point(.12f,-.07f,.31f); const auto before=primary.Point(weights,point,point);
    const auto secondBefore=secondary.Point(weights,point,point);
    Matrix4 proxy=MIdentity, secondProxy=MIdentity; primary.Matrix(proxy,weights); secondary.Matrix(secondProxy,weights);
    CHECK_FALSE(state->Freeze()); // no validated readback/bounds yet
    state->updates=12; state->minimum=Vector3(-1,-1,-1); state->maximum=Vector3(1,1,1);
    state->radius=std::numeric_limits<float>::quiet_NaN(); CHECK_FALSE(state->Freeze());
    state->radius=1.8f; REQUIRE(state->Freeze()); CHECK(state->state==CorpseSolverState::Frozen);
    CHECK_FALSE(state->Freeze()); REQUIRE(state->Ready());
    CHECK(world.GetStats().articulatedBodies==0); CHECK(world.GetStats().articulatedJoints==0);
    CHECK(world.GetStats().solverBodies==0); CHECK(world.GetStats().solverJoints==0);
    for (auto body:state->bodies) CHECK_FALSE(body.IsValid());
    for (auto joint:state->joints) CHECK_FALSE(joint.IsValid());
    CHECK(state->minimum==Vector3(-1,-1,-1)); CHECK(state->maximum==Vector3(1,1,1)); CHECK(state->radius==1.8f);
    // A >30 second solver advance cannot mutate already owned phase storage.
    for (int i=0;i<1861;++i) world.Step(1.0f/60);
    MATRIX_4_ARRAY(after,128); MATRIX_4_ARRAY(secondAfter,128);
    primary.PrepareMatrices(after,1); secondary.PrepareMatrices(secondAfter,1);
    for (int i=0;i<12;++i) { SameMatrix(palette[i],after[i]); SameMatrix(secondPalette[i],secondAfter[i]); }
    CHECK(primary.Point(weights,point,point)==before); CHECK(secondary.Point(weights,point,point)==secondBefore);
    Matrix4 afterProxy=MIdentity, afterSecondProxy=MIdentity;
    primary.Matrix(afterProxy,weights); secondary.Matrix(afterSecondProxy,weights);
    SameMatrix(proxy,afterProxy); SameMatrix(secondProxy,afterSecondProxy);
    const auto epoch=world.Generation(); world.Destroy(); REQUIRE(world.Generation()>epoch);
    CHECK_FALSE(state->Ready()); REQUIRE(world.Create());
    const auto fresh=world.SpawnArticulatedPiece(piece,MIdentity,2); REQUIRE(fresh.IsValid());
    state.reset(); // an old owner's destructor must not erase a new backend body
    BodyMotion motion; CHECK(world.GetBodyMotion(fresh,motion)); CHECK(world.GetStats().solverBodies==1);
    world.Remove(fresh);
}

TEST_CASE("Prepared fixture proof distinguishes terrain player proxy and mutation attempts", "[corpse-freeze][physics]")
{
    using namespace Poseidon::Physics;
    PhysicsWorld world; REQUIRE(world.Create());
    const float heights[9]={}; const auto initial=world.TerrainMutationSerial();
    REQUIRE(world.SetTerrain(heights,3,3,.25f,0,0)); CHECK(world.TerrainMutationSerial()==initial+1);
    CHECK(world.GetStats().bodies==0); CHECK(world.GetStats().solverBodies==1);
    CHECK_FALSE(world.GetStats().kinematicProxyRegistered);
    world.SetKinematicProxy(Vector3(0,2,0),.35f,1.8f);
    CHECK(world.GetStats().bodies==0); CHECK(world.GetStats().solverBodies==2);
    CHECK(world.GetStats().kinematicProxyRegistered); CHECK(world.GetStats().solverJoints==0);
    world.SetKinematicProxy(VZero,.35f,0);
    CHECK(world.GetStats().solverBodies==1); CHECK_FALSE(world.GetStats().kinematicProxyRegistered);
    const auto serial=world.TerrainMutationSerial(); REQUIRE(world.PatchTerrain(0,0,1,1,heights));
    CHECK(world.TerrainMutationSerial()==serial+1);
    CHECK_FALSE(world.PatchTerrain(-1,0,1,1,heights)); CHECK(world.TerrainMutationSerial()==serial+2);
    CHECK_FALSE(world.SetTerrain(heights,3,3,std::numeric_limits<float>::quiet_NaN(),0,0));
    CHECK(world.TerrainMutationSerial()==serial+3);
    CHECK_FALSE(world.SetTerrain(heights,3,3,.25f,std::numeric_limits<float>::infinity(),0));
    CHECK_FALSE(world.SetTerrain(heights,3,3,.25f,0,std::numeric_limits<float>::quiet_NaN()));
    CHECK(world.TerrainMutationSerial()==serial+5);
    CHECK(world.GetStats().solverBodies==1); // invalid replacements preserve the live terrain
    const auto epoch=world.Generation(); world.Destroy(); CHECK(world.Generation()>epoch);
    CHECK(world.GetStats().solverBodies==0); CHECK(world.GetStats().solverJoints==0);
}

TEST_CASE("Actual stock neutral geometry and captured affine palette admit signed anatomical hinges", "[corpse-pose][articulation][corpse-hinge]")
{
    // ODOL7 mc vojakw2.p3d decoded SHA256 baf90043df1b561c6099f3c8fec546ed58e43e00853d70f386cfb2f81853d221.
    // Runtime OrigPos X/Z sign conversion is established by all nine measured
    // shared boundaries; translations cancel in these tangent measurements.
    // Actual post-correction palette: final-pose-installed-20261001-052726-193320
    // result.json, gates.finalPose.activeHold.actual.levels[0].palette.
    const Vector3 forward(-0.997177193f,0.0556384612f,0.0504183212f);
    const Vector3 endpoints[4][3] = {
        {{-0.408487626f,0.0955670712f,0.524068727f},{-0.400908136f,-0.212169901f,0.523606247f},{-0.398237315f,-0.431132811f,0.542944739f}},
        {{-0.406447581f,0.0911346157f,0.167404996f},{-0.400908136f,-0.215609224f,0.152138341f},{-0.398237315f,-0.431132811f,0.136096053f}},
        {{-0.3915233f,-0.36885282f,0.472260184f},{-0.399644616f,-0.821461531f,0.477296657f},{-0.433392412f,-1.2196249f,0.564433799f}},
        {{-0.424557763f,-0.352624238f,0.202896609f},{-0.399158677f,-0.81897919f,0.206840143f},{-0.432597316f,-1.22280186f,0.105136569f}},
    };
    const float affine[4][2][12] = {
        {{0.0379340872f,-0.779791594f,0.624887049f,0.264855146f,-0.600917935f,0.481835544f,0.637753069f,-0.130186692f,-0.798405886f,-0.399697632f,-0.450310051f,-0.138869241f},{0.735122621f,-0.388136834f,0.555828393f,0.266513497f,-0.645362198f,-0.149572283f,0.749088883f,-0.125471339f,-0.207606986f,-0.909380138f,-0.36044082f,-0.130773187f}},
        {{0.387846559f,0.899583399f,0.20081526f,-0.485077262f,-0.177596688f,-0.14084743f,0.973972976f,0.21095714f,0.904447794f,-0.413417876f,0.105135322f,-0.107519351f},{-0.436281502f,0.877980828f,0.19699499f,-0.481219977f,-0.140003383f,-0.2825059f,0.948993206f,0.211839303f,0.888853788f,0.386442512f,0.246173367f,-0.113382034f}},
        {{0.758001804f,0.413689613f,0.504277945f,0.0609275065f,-0.555642068f,0.00463545276f,0.831408024f,0.124795079f,0.34160763f,-0.910405874f,0.233376622f,0.0768911317f},{0.702856183f,-0.322408736f,0.63406837f,-0.374524385f,-0.43846181f,0.505545199f,0.743086815f,0.420706153f,-0.560127854f,-0.800300241f,0.213957056f,0.146714494f}},
        {{-0.0677606016f,0.964595199f,0.254872084f,0.0608757213f,-0.223536268f,-0.263644308f,0.93836087f,0.150186226f,0.97233665f,0.00660747616f,0.233484477f,0.183228835f},{0.9324103f,0.33178246f,0.143290922f,-0.321139008f,-0.157056317f,0.0149006508f,0.987476647f,0.315695554f,0.325495392f,-0.943236947f,0.0659987107f,-0.378971606f}},
    };
    const float expected[] = {.957975283f,.850857967f,.901439732f,1.16555946f};
    for (int limb=0;limb<4;++limb)
    {
        Matrix4 first=MIdentity,second=MIdentity;
        for (int row=0;row<3;++row) for (int col=0;col<4;++col)
        { first(row,col)=affine[limb][0][row*4+col]; second(row,col)=affine[limb][1][row*4+col]; }
        const Matrix4 originalFirst=first,originalSecond=second;
        CorpseHingeMeasurement h;
        REQUIRE(CorpseMeasureHinge(endpoints[limb][0],endpoints[limb][1],endpoints[limb][2],forward,first,second,limb>=2,h));
        CHECK(std::abs(h.capturedAngle-expected[limb]) < .0001f);
        CHECK(h.referenceAgreement > .97f);
        CHECK(std::abs(h.axis*h.proximal) < .00001f);
        CHECK(std::abs(h.axis*h.distal) < .00001f);
        CHECK(h.lower < 0); CHECK(h.upper > 0);
        SameMatrix(first,originalFirst); SameMatrix(second,originalSecond);
        // Rotate the whole captured world; anatomy follows it, never a world axis.
        Matrix4 rotate(MDirection,Vector3(1,0,0),VUp);
        CorpseHingeMeasurement transformed;
        REQUIRE(CorpseMeasureHinge(endpoints[limb][0],endpoints[limb][1],endpoints[limb][2],forward,rotate*first,rotate*second,limb>=2,transformed));
        CHECK(std::abs(transformed.capturedAngle-h.capturedAngle) < .0001f);
        CHECK((transformed.axis-rotate.Rotate(h.axis)).Size() < .00001f);
        CHECK_FALSE(CorpseMeasureHinge(endpoints[limb][0],endpoints[limb][1],endpoints[limb][2],-forward,first,second,limb>=2,transformed));
        CHECK(std::string(transformed.refusal) == "captured-bend-outside-policy");
    }
}

TEST_CASE("Measured hinges refuse ambiguous planes and retain a straight affine reference", "[corpse-pose][articulation][corpse-hinge]")
{
    CorpseHingeMeasurement h;
    const Vector3 proximal(0,1,0),joint(0,.5f,0),distal(0,0,0),forward(1,0,0);
    Matrix4 nonrigid(MScale,1.1f,.7f,.9f); nonrigid(0,1)=.12f;
    REQUIRE(CorpseMeasureHinge(proximal,joint,distal,forward,nonrigid,nonrigid,false,h));
    CHECK(std::abs(h.capturedAngle) < .00001f);
    CHECK(std::abs(h.axis*h.proximal) < .00001f);
    CHECK(h.lower < 0); CHECK(h.upper > 0);
    CHECK_FALSE(CorpseMeasureHinge(joint,joint,distal,forward,MIdentity,MIdentity,false,h));
    CHECK(std::string(h.refusal) == "short-transformed-segment");
    CHECK_FALSE(CorpseMeasureHinge(proximal,joint,distal,VUp,MIdentity,MIdentity,false,h));
    CHECK(std::string(h.refusal) == "neutral-flexion-plane-degenerate");
    Matrix4 side(MDirection,Vector3(0,.7f,.7f),Vector3(0,.7f,-.7f));
    CHECK_FALSE(CorpseMeasureHinge(proximal,joint,distal,forward,MIdentity,side,false,h));
    CHECK(std::string(h.refusal) == "anatomical-plane-disagreement");
    side=MIdentity; side(2,2)=0;
    CHECK_FALSE(CorpseMeasureHinge(proximal,joint,distal,forward,MIdentity,side,false,h));
    CHECK(std::string(h.refusal) == "singular-affine-frame");
    side(0,0)=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(CorpseMeasureHinge(proximal,joint,distal,forward,MIdentity,side,false,h));
    CHECK(std::string(h.refusal) == "nonfinite-input");
}

TEST_CASE("Actual rifle momentum uses ballistic mass and the struck point once", "[physics][corpse-pose][corpse-projectile-impulse]")
{
    Vector3 force,torque;
    const Vector3 velocity(800,0,0),center(4,5,6),hit(4,5.1f,6);
    REQUIRE(RifleProjectileImpulse(velocity,.004f,hit,center,force,torque));
    CHECK((force-Vector3(3.2f,0,0)).Size()<1e-6f);
    CHECK((torque-Vector3(0,0,-.32f)).Size()<1e-6f);
    const Vector3 oldForce=force,oldTorque=torque;
    const Vector3 translate(5000,10,10000);
    REQUIRE(RifleProjectileImpulse(velocity,.004f,hit+translate,center+translate,force,torque));
    CHECK(force==oldForce); CHECK((torque-oldTorque).Size()<.00001f);
    REQUIRE(RifleProjectileImpulse(velocity,.008f,center,center,force,torque));
    CHECK((force-Vector3(6.4f,0,0)).Size()<1e-6f); CHECK(torque==VZero);
    CHECK_FALSE(RifleProjectileImpulse(velocity,-.004f,hit,center,force,torque));
    CHECK(force==VZero); CHECK(torque==VZero);
    CHECK_FALSE(RifleProjectileImpulse(velocity,std::numeric_limits<float>::infinity(),hit,center,force,torque));
    CHECK_FALSE(RifleProjectileImpulse(Vector3(std::numeric_limits<float>::quiet_NaN(),0,0),.004f,hit,center,force,torque));
    CHECK_FALSE(RifleProjectileImpulse(velocity,.004f,Vector3(0,std::numeric_limits<float>::infinity(),0),center,force,torque));
    CHECK_FALSE(RifleProjectileImpulse(velocity,.004f,hit,Vector3(std::numeric_limits<float>::quiet_NaN(),0,0),force,torque));
    CHECK_FALSE(RifleProjectileImpulse(Vector3(3e38f,0,0),1,hit,center,force,torque));
}

TEST_CASE("Actual articulated limb rifle response conserves resolved projectile momentum", "[physics][articulation][corpse-projectile-impulse]")
{
    using namespace Poseidon::Physics;
    auto backend=CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    const Vector3 vertices[]={{-.2f,-.2f,-.2f},{.2f,-.2f,-.2f},{-.2f,.2f,-.2f},{.2f,.2f,-.2f},
        {-.2f,-.2f,.2f},{.2f,-.2f,.2f},{-.2f,.2f,.2f},{.2f,.2f,.2f}};
    ConvexPiece piece{vertices,8,ColliderFlags::Solid}; Matrix4 transform=MIdentity;
    transform.SetPosition(Vector3(0,100,0));
    const auto body=backend->SpawnArticulatedPiece(piece,transform,2); REQUIRE(body.IsValid());
    Vector3 force,torque;
    const Vector3 hit=transform.Position()+Vector3(0,.1f,0);
    REQUIRE(RifleProjectileImpulse(Vector3(800,0,0),.004f,hit,transform.Position(),force,torque));
    backend->ApplyImpulse(body,hit,force*.6f); // existing Man::Rigid applied once
    BodyMotion readback; REQUIRE(backend->GetBodyMotion(body,readback));
    CHECK(std::abs(readback.linearVelocity.X()-.96f)<.0001f);
    CHECK(std::abs(readback.angularVelocity.Z()+3.6f)<.001f);
    CHECK(readback.linearVelocity.Size()<15); CHECK(readback.angularVelocity.Size()<20);
    CHECK((readback.position-transform.Position()).Size()<.0001f);
    backend->RemoveBody(body); CHECK(backend->GetStats().articulatedBodies==0); backend->Destroy();
}

TEST_CASE("Rejected authored hinge extension retains pose in constrained spherical control", "[physics][articulation][corpse-stance-fallback]")
{
    using namespace Poseidon::Physics;
    const Vector3 proximal(0,.2f,0),joint=VZero,distal(0,-.2f,0),forward(0,0,1);
    const Matrix4 proximalAffine=MIdentity,distalAffine(MRotationX,-.2427f);
    const Matrix4 originalDistal=distalAffine;
    CorpseHingeMeasurement hinge;
    REQUIRE_FALSE(CorpseMeasureHinge(proximal,joint,distal,forward,proximalAffine,distalAffine,true,hinge));
    CHECK(std::string_view(hinge.refusal)=="captured-bend-outside-policy");
    CHECK(std::memcmp(&distalAffine,&originalDistal,sizeof(Matrix4))==0);
    auto backend=CreatePhysicsBackend(); REQUIRE(backend); REQUIRE(backend->Create());
    const Vector3 points[]={{-.05f,-.05f,-.05f},{.05f,-.05f,-.05f},{-.05f,.05f,-.05f},{.05f,.05f,-.05f},
        {-.05f,-.05f,.05f},{.05f,-.05f,.05f},{-.05f,.05f,.05f},{.05f,.05f,.05f}};
    const ConvexPiece piece{points,8,ColliderFlags::Solid};
    const Vector3 pivot(0,100,0);
    Matrix4 parent=MIdentity,child=MIdentity;
    parent.SetPosition(pivot+proximal); child.SetPosition(pivot+distalAffine.Rotate(distal));
    const auto first=backend->SpawnArticulatedPiece(piece,parent,2);
    const auto second=backend->SpawnArticulatedPiece(piece,child,2);
    REQUIRE(first.IsValid()); REQUIRE(second.IsValid());
    const Vector3 axis=(child.Position()-pivot).Normalized();
    Matrix4 frame(MDirection,axis,VAside); frame.SetPosition(pivot);
    ArticulatedJointDef definition; definition.first=first; definition.second=second;
    definition.firstFrame=parent.InverseRotation()*frame; definition.secondFrame=child.InverseRotation()*frame;
    definition.coneAngle=.30f; definition.twistAngle=.20f;
    const auto constraint=backend->AddArticulatedJoint(definition); REQUIRE(constraint.IsValid());
    BodyMotion beforeFirst,beforeSecond;
    REQUIRE(backend->GetBodyMotion(first,beforeFirst)); REQUIRE(backend->GetBodyMotion(second,beforeSecond));
    CHECK(beforeFirst.position==parent.Position()); CHECK(beforeSecond.position==child.Position());
    backend->ApplyImpulse(second,child.Position(),Vector3(.2f,0,0));
    for (int step=0;step<30;++step) backend->Step(1.f/60);
    BodyMotion afterFirst,afterSecond;
    REQUIRE(backend->GetBodyMotion(first,afterFirst)); REQUIRE(backend->GetBodyMotion(second,afterSecond));
    CHECK(afterFirst.position.IsFinite()); CHECK(afterSecond.position.IsFinite());
    CHECK(afterFirst.linearVelocity.Size()<15); CHECK(afterSecond.linearVelocity.Size()<15);
    CHECK(afterFirst.angularVelocity.Size()<20); CHECK(afterSecond.angularVelocity.Size()<20);
    CHECK((afterSecond.position-afterFirst.position).Size()<.6f);
    CHECK((afterSecond.position-beforeSecond.position).Size()>.01f);
    backend->RemoveJoint(constraint); backend->RemoveBody(first); backend->RemoveBody(second);
    CHECK(backend->GetStats().articulatedBodies==0); CHECK(backend->GetStats().articulatedJoints==0);
    backend->Destroy();
}
