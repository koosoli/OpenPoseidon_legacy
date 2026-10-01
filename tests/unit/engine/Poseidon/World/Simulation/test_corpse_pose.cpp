#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Simulation/Animation/CorpsePose.hpp>
#include <Poseidon/World/Entities/Infantry/Head.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace Poseidon;

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
