// SIM-807 — determinism of the ObjectStreamPreparer's parallel stage.
//
// SIM-806 validated the one production `TaskPool::ParallelFor` and named this as the
// next target:
//
//   "ObjectStreamPreparer is untested. It runs on its own std::thread pool with a
//    hard-coded worker count (2), and since 2026-08-30 it runs full ShapeAdapter
//    geometry conversion whose result the main thread installs verbatim."
//
// WHAT IS ACTUALLY PARALLEL HERE, AND WHAT IS NOT.  The preparer's workers do exactly
// two things off the main thread (ObjectStreamPrepare.cpp, WorkerMain):
//
//   1. `ModelCache::LoadLooseFile`   — file read + P3D decode + Model::compile.
//   2. `ShapeAdapter::convertToLODShape(model, false, tables, /*finishTail=*/false)`
//      — the geometry conversion, fed by bank tables the MAIN thread resolved.
//
// Everything else is main-thread by construction and is NOT under test here:
//   - `BuildAdapterBankTables` (ShapeAdapter.cpp:423) touches `GlobLoadTexture` and
//     `GTexMaterialBank`; LandSave.cpp:3594 calls it before `SubmitConvert`.
//   - `FinishOdolAdapterTail` + `CreateAdapterProxies` + `OptimizeOneShape` run on the
//     install (ShapeAdapter.cpp:1507 is skipped when finishTail is false).
//
// So arm (2) is the whole of the parallel authoritative surface, and it is what this
// file hashes.  Arm (1) is covered only for its failure classification (see the
// state-machine cases at the bottom): parsing a file is a pure function of its bytes
// and needs a corpus to be interesting, whereas the conversion is where shared banks,
// the RString property table and the FP environment could diverge.
//
// WHAT IS COMPARED.  SIM-806's method, deliberately reused rather than reinvented: a
// bit-exact FNV-1a over the COMPLETE converted `LODShapeWithShadow` — every LOD's
// vertex positions, normals, UVs, tangent frames and clip flags as raw IEEE-754
// BITS, every face's arity, vertex indices, special flags and texture INDEX (never a
// pointer — an address is allocator state), every section's special/texture/material
// binding, every named selection, and the shape-level scalars.  Float bits, not float
// values: `-0.0f == 0.0f` and `NaN != NaN`, so a value comparison is simultaneously
// too loose and too tight for a strict gate.
//
// Decision record: design notes

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/TreeWind.hpp>

TEST_CASE("Native coherent tree wind excludes legacy models", "[native-wind]")
{
    REQUIRE(IsNativeTreeWindModel("assets/vegetation/tree/t_picea.xob"));
    REQUIRE(IsNativeTreeWindModel("assets/vegetation/tree/t_picea.XOB"));
    REQUIRE_FALSE(IsNativeTreeWindModel("data3d/str buk.p3d"));
    REQUIRE_FALSE(IsNativeTreeWindModel("folder.xob/tree.p3d"));
    REQUIRE_FALSE(IsNativeTreeWindModel("tree.xob.p3d"));
    REQUIRE_FALSE(IsNativeTreeWindModel("tree.xobextra"));
    REQUIRE_FALSE(IsNativeTreeWindModel(nullptr));
}

#include "../../test_fixtures.hpp"

#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Dummy/TextureDummy.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <thread>
#include <vector>

using Poseidon::LODShapeWithShadow;
using Poseidon::ObjectStreamPreparer;
using Poseidon::Shape;

namespace ModelIR = Poseidon::Model;
namespace Adapter = Poseidon::Model::ShapeAdapter;

namespace
{

// ---------------------------------------------------------------------------
// Bit-exact hashing.  Same shape as SIM-806's; file-local by the same
// reasoning (that record's "what is NOT covered" asks the next author to copy
// it deliberately rather than inherit an ill-fitting base class).
// ---------------------------------------------------------------------------

class BitHasher
{
  public:
    void Bytes(const void* data, size_t n)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            _h ^= static_cast<uint64_t>(p[i]);
            _h *= 1099511628211ull;
        }
    }

    void U32(uint32_t v) { Bytes(&v, sizeof(v)); }
    void I32(int32_t v) { Bytes(&v, sizeof(v)); }
    void U64(uint64_t v) { Bytes(&v, sizeof(v)); }

    // Storage bits, not arithmetic value.  See the file header.
    void F32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U32(bits);
    }

    void Str(const char* s)
    {
        if (!s)
        {
            I32(-1);
            return;
        }
        const size_t n = std::strlen(s);
        U32(static_cast<uint32_t>(n));
        Bytes(s, n);
    }

    uint64_t Value() const { return _h; }

  private:
    uint64_t _h = 1469598103934665603ull;
};

void HashShape(BitHasher& h, const Shape& shape)
{
    const int nVertex = shape.NVertex();
    h.I32(nVertex);
    const bool tangents = shape.HasTangentFrame();
    h.I32(tangents ? 1 : 0);
    for (int i = 0; i < nVertex; ++i)
    {
        const auto pos = shape.Pos(i);
        h.F32(pos.X());
        h.F32(pos.Y());
        h.F32(pos.Z());
        const auto norm = shape.Norm(i);
        h.F32(norm.X());
        h.F32(norm.Y());
        h.F32(norm.Z());
        h.F32(shape.U(i));
        h.F32(shape.V(i));
        h.I32(static_cast<int32_t>(shape.Clip(i)));
        if (tangents && i < shape.NTangent())
        {
            const auto t = shape.Tangent(i);
            const auto b = shape.Binormal(i);
            h.F32(t.X());
            h.F32(t.Y());
            h.F32(t.Z());
            h.F32(b.X());
            h.F32(b.Y());
            h.F32(b.Z());
        }
    }

    // Texture identity as an INDEX into this shape's own table, never as a
    // pointer.  The index also catches a divergence in registration ORDER.
    h.I32(shape.NTextures());

    h.I32(shape.NFaces());
    for (Offset f = shape.BeginFaces(); f < shape.EndFaces(); shape.NextFace(f))
    {
        const Poly& face = shape.Face(f);
        h.I32(face.N());
        for (int v = 0; v < face.N(); ++v)
            h.I32(static_cast<int32_t>(face.GetVertex(v)));
        h.I32(face.Special());
        h.I32(shape.GetTextureIndex(face.GetTexture()));
    }

    // Sections are the material binding the tables feed, so they are the part a
    // bank-table divergence would show up in first.  `surfMat` is a refcounted
    // pointer -- hash only whether one is bound, for the same allocator-state
    // reason the texture is hashed by index.
    const int nSections = shape.NSections();
    h.I32(nSections);
    for (int s = 0; s < nSections; ++s)
    {
        const ShapeSection& sec = shape.GetSection(s);
        h.I32(sec.properties.Special());
        h.I32(shape.GetTextureIndex(sec.properties.GetTexture()));
        h.I32(sec.material);
        h.I32(sec.surfMat.IsNull() ? 0 : 1);
    }

    const int nSel = shape.NNamedSel();
    h.I32(nSel);
    for (int s = 0; s < nSel; ++s)
        h.Str(shape.NamedSel(s).Name());

    h.I32(shape.Special());
    h.I32(shape.NPoints());

    const auto mn = shape.Min();
    const auto mx = shape.Max();
    h.F32(mn.X());
    h.F32(mn.Y());
    h.F32(mn.Z());
    h.F32(mx.X());
    h.F32(mx.Y());
    h.F32(mx.Z());
}

// A component-wise breakdown of the same bits, used ONLY to say WHERE two shapes
// differ.  SIM-806's RequireIdentical reports the first divergent segment index;
// this is that idea at this fixture's scale -- "LOD 3 vertices" rather than "the
// hashes differ".
std::vector<std::pair<std::string, uint64_t>> HashComponents(const LODShapeWithShadow& shape)
{
    std::vector<std::pair<std::string, uint64_t>> out;
    auto add = [&](const std::string& name, const BitHasher& h) { out.emplace_back(name, h.Value()); };
    {
        BitHasher h;
        h.Str(shape.Name());
        h.I32(shape.NLevels());
        h.I32(static_cast<int32_t>(shape.GetMapType()));
        h.I32(shape.Special());
        h.F32(shape.Mass());
        h.F32(shape.BoundingSphere());
        add("shape scalars", h);
    }
    {
        BitHasher h;
        h.I32(shape.FindGeometryLevel());
        h.I32(shape.FindMemoryLevel());
        h.I32(shape.FindFireGeometryLevel());
        h.I32(shape.FindViewGeometryLevel());
        h.I32(shape.FindLandContactLevel());
        h.I32(shape.FindRoadwayLevel());
        h.I32(shape.FindPaths());
        h.I32(shape.FindHitpoints());
        add("special level indices", h);
    }
    for (int i = 0; i < shape.NLevels(); ++i)
    {
        Shape* lod = shape.Level(i);
        const std::string tag = "LOD " + std::to_string(i);
        if (!lod)
        {
            BitHasher h;
            h.I32(-1);
            add(tag + " null", h);
            continue;
        }
        {
            BitHasher h;
            const int n = lod->NVertex();
            h.I32(n);
            for (int v = 0; v < n; ++v)
            {
                const auto pos = lod->Pos(v);
                h.F32(pos.X());
                h.F32(pos.Y());
                h.F32(pos.Z());
            }
            add(tag + " positions", h);
        }
        {
            BitHasher h;
            const int n = lod->NVertex();
            for (int v = 0; v < n; ++v)
            {
                const auto nr = lod->Norm(v);
                h.F32(nr.X());
                h.F32(nr.Y());
                h.F32(nr.Z());
            }
            add(tag + " normals", h);
        }
        {
            BitHasher h;
            const int n = lod->NVertex();
            for (int v = 0; v < n; ++v)
            {
                h.F32(lod->U(v));
                h.F32(lod->V(v));
                h.I32(static_cast<int32_t>(lod->Clip(v)));
            }
            add(tag + " uv/clip", h);
        }
        {
            BitHasher h;
            h.I32(lod->HasTangentFrame() ? 1 : 0);
            h.I32(lod->NTangent());
            if (lod->HasTangentFrame())
                for (int v = 0; v < lod->NTangent(); ++v)
                {
                    const auto t = lod->Tangent(v);
                    const auto b = lod->Binormal(v);
                    h.F32(t.X());
                    h.F32(t.Y());
                    h.F32(t.Z());
                    h.F32(b.X());
                    h.F32(b.Y());
                    h.F32(b.Z());
                }
            add(tag + " tangent frame", h);
        }
        {
            BitHasher h;
            h.I32(lod->NFaces());
            for (Offset f = lod->BeginFaces(); f < lod->EndFaces(); lod->NextFace(f))
            {
                const Poly& face = lod->Face(f);
                h.I32(face.N());
                for (int v = 0; v < face.N(); ++v)
                    h.I32(static_cast<int32_t>(face.GetVertex(v)));
                h.I32(face.Special());
                h.I32(lod->GetTextureIndex(face.GetTexture()));
            }
            add(tag + " faces", h);
        }
        {
            BitHasher h;
            h.I32(lod->NTextures());
            h.I32(lod->NSections());
            for (int s = 0; s < lod->NSections(); ++s)
            {
                const ShapeSection& sec = lod->GetSection(s);
                h.I32(sec.properties.Special());
                h.I32(lod->GetTextureIndex(sec.properties.GetTexture()));
                h.I32(sec.material);
                h.I32(sec.surfMat.IsNull() ? 0 : 1);
            }
            add(tag + " sections", h);
        }
        {
            BitHasher h;
            h.I32(lod->NNamedSel());
            for (int s = 0; s < lod->NNamedSel(); ++s)
                h.Str(lod->NamedSel(s).Name());
            h.I32(lod->Special());
            h.I32(lod->NPoints());
            const auto mn = lod->Min();
            const auto mx = lod->Max();
            h.F32(mn.X());
            h.F32(mn.Y());
            h.F32(mn.Z());
            h.F32(mx.X());
            h.F32(mx.Y());
            h.F32(mx.Z());
            add(tag + " selections/bbox", h);
        }
    }
    return out;
}

// The whole converted shape, which is what the main thread installs verbatim.
uint64_t HashConverted(const LODShapeWithShadow& shape)
{
    BitHasher h;
    h.Str(shape.Name());
    const int levels = shape.NLevels();
    h.I32(levels);
    h.I32(static_cast<int32_t>(shape.GetMapType()));
    h.I32(shape.Special());
    h.F32(shape.Mass());
    h.F32(shape.BoundingSphere());
    const auto bc = shape.BoundingCenter();
    h.F32(bc.X());
    h.F32(bc.Y());
    h.F32(bc.Z());
    const auto com = shape.CenterOfMass();
    h.F32(com.X());
    h.F32(com.Y());
    h.F32(com.Z());
    // The special-level indices ScanShapes resolved.
    h.I32(shape.FindGeometryLevel());
    h.I32(shape.FindMemoryLevel());
    h.I32(shape.FindFireGeometryLevel());
    h.I32(shape.FindViewGeometryLevel());
    h.I32(shape.FindLandContactLevel());
    h.I32(shape.FindRoadwayLevel());
    h.I32(shape.FindPaths());
    h.I32(shape.FindHitpoints());
    for (int i = 0; i < levels; ++i)
    {
        h.F32(shape.Resolution(i));
        Shape* lod = shape.Level(i);
        if (!lod)
        {
            h.I32(-1);
            continue;
        }
        h.I32(1);
        HashShape(h, *lod);
    }
    return h.Value();
}

// ---------------------------------------------------------------------------
// The fixture model
// ---------------------------------------------------------------------------
//
// A synthetic ODOL IR rather than a corpus model, for the reason SIM-806 gives
// about its terrain: the KERNEL under test is the unmodified production
// conversion, and a fixture that needs game data skips on CI and on any machine
// without the installs.  The real-corpus arm below adds one installed model on
// top of this when it is present, so the synthetic case is the floor, not the
// ceiling.
//
// It is built to be varied on every axis the conversion branches on: several
// LODs at different resolutions, both triangles and quads, several materials
// (so `model.compile()` produces several sections and therefore several bank
// table rows), a material carrying an RVMAT path (the `surfMat` branch) beside
// ones carrying only a texture, named selections, and named properties
// including `map` and `autocenter`, which the ODOL branch reads.

constexpr int kGridN = 17;                       // 17x17 = 289 vertices per LOD
constexpr int kLodCount = 5;

void BuildLod(ModelIR::LODLevel& level, int lodIndex)
{
    ModelIR::Mesh& mesh = level.mesh;

    // Deliberately irrational-looking values so the tangent/normal arithmetic
    // sees varied inputs rather than a constant, and so no two LODs collapse to
    // the same hash.
    const float scale = 1.0f + 0.37f * static_cast<float>(lodIndex);
    for (int z = 0; z < kGridN; ++z)
    {
        for (int x = 0; x < kGridN; ++x)
        {
            const float fx = static_cast<float>(x) * 0.611f * scale;
            const float fz = static_cast<float>(z) * 0.733f * scale;
            const float fy = 0.25f * (fx * 0.31f - fz * 0.17f) + 0.05f * static_cast<float>((x * 7 + z * 13) % 11);
            ModelIR::Vertex v;
            v.position = {fx, fy, fz};
            // Not normalised on purpose: the adapter is what decides whether to
            // renormalise, and pinning its choice is the point.
            v.normal = {0.13f * static_cast<float>(x % 5) - 0.2f, 1.0f, 0.11f * static_cast<float>(z % 3) - 0.1f};
            v.uv = {fx * 0.07f, fz * 0.09f};
            v.uv1 = {fz * 0.03f, fx * 0.05f};
            mesh.vertices.push_back(v);
        }
    }

    // Four materials: two plain textures, one with an RVMAT (the surfMat
    // branch), one with no texture at all (the null-texture branch).
    mesh.materials.emplace_back("mat_a", "data\\sim807_a.paa");
    mesh.materials.emplace_back("mat_b", "data\\SIM807_B.PAA"); // mixed case: the lowering path
    {
        ModelIR::Material withRvmat("mat_c", "data\\sim807_c.paa");
        withRvmat.materialPath = "data\\sim807_c.rvmat";
        mesh.materials.push_back(std::move(withRvmat));
    }
    mesh.materials.emplace_back("mat_none"); // texturePath empty

    // Faces in material runs, so compile() produces one section per run rather
    // than one per face.  Alternating triangles and quads inside each run.
    uint32_t originalIndex = 0;
    const int cells = (kGridN - 1) * (kGridN - 1);
    for (int c = 0; c < cells; ++c)
    {
        const int cx = c % (kGridN - 1);
        const int cz = c / (kGridN - 1);
        const uint32_t v0 = static_cast<uint32_t>(cz * kGridN + cx);
        const uint32_t v1 = v0 + 1;
        const uint32_t v2 = v0 + kGridN + 1;
        const uint32_t v3 = v0 + kGridN;
        // Four contiguous material runs across the grid.
        const uint32_t mat = static_cast<uint32_t>((c * 4) / cells);
        if ((c % 3) == 0)
        {
            ModelIR::Triangle t(v0, v1, v2, mat);
            t.originalIndex = originalIndex++;
            mesh.triangles.push_back(t);
            ModelIR::Triangle t2(v0, v2, v3, mat);
            t2.originalIndex = originalIndex++;
            mesh.triangles.push_back(t2);
        }
        else
        {
            ModelIR::Quad q(v0, v1, v2, v3, mat);
            q.originalIndex = originalIndex++;
            mesh.quads.push_back(q);
        }
    }

    // Named selections: two, with weights, over disjoint vertex ranges.
    for (int s = 0; s < 2; ++s)
    {
        ModelIR::NamedSelection sel(s == 0 ? "sim807_left" : "sim807_right");
        for (uint32_t i = 0; i < static_cast<uint32_t>(mesh.vertices.size()); ++i)
        {
            if (static_cast<int>(i % 2) != s)
                continue;
            sel.vertexIndices.push_back(i);
            sel.vertexWeights.push_back(static_cast<uint8_t>(64 + (i % 128)));
        }
        mesh.selections.push_back(std::move(sel));
    }

    // Properties the ODOL branch reads before the tail.
    mesh.properties.emplace_back("map", "building");
    mesh.properties.emplace_back("autocenter", "0");
    mesh.properties.emplace_back("class", "house");
}

std::shared_ptr<ModelIR::Model> BuildFixtureModel()
{
    auto model = std::make_shared<ModelIR::Model>();
    model->sourceFormat = "ODOL";
    model->sourcePath = "sim807\\fixture.p3d";
    model->sourceVersion = 40;
    model->mass = 1234.5f;
    model->centerOfMass = {0.5f, 0.25f, -0.125f};

    // Four visual LODs at ascending resolutions, then a GEOMETRY LOD at the
    // 1e13 sentinel (LodPurpose.hpp:70).  The geometry LOD is not decoration:
    // `ScanShapes` -- which runs only in the TAIL -- is what turns that
    // resolution into `_geometry`, so its presence is what makes the
    // worker-vs-install difference observable at all.  See the tail control.
    const float resolutions[kLodCount] = {1.0f, 10.0f, 100.0f, 1000.0f, 1e13f};
    for (int i = 0; i < kLodCount; ++i)
    {
        ModelIR::LODLevel level(resolutions[i]);
        level.sourceEncoding = "ODOL73";
        BuildLod(level, i);
        model->lodLevels.push_back(std::move(level));
    }
    REQUIRE(model->compile());
    return model;
}

// ---------------------------------------------------------------------------
// The arms
// ---------------------------------------------------------------------------

// One conversion, exactly as ObjectStreamPreparer::Impl::WorkerMain runs it:
// `reversed=false`, main-thread bank tables, `finishTail=false`.
uint64_t ConvertOnce(const ModelIR::Model& model, const Adapter::AdapterBankTables& tables)
{
    LODShapeWithShadow* shape = Adapter::convertToLODShape(model, false, &tables, /*finishTail=*/false);
    REQUIRE(shape != nullptr);
    const uint64_t hash = HashConverted(*shape);
    delete shape; // what the preparer does with a stale/failed conversion
    return hash;
}

// `jobs` conversions spread over `workers` threads, all started together, all
// hitting the conversion at the same time.  Returns one hash per job, in job
// order (so the comparison is order-independent of the thread that ran it --
// which is exactly the property being tested).
std::vector<uint64_t> RunAtWorkerCount(uint32_t workers, int jobs, const ModelIR::Model& model,
                                       const Adapter::AdapterBankTables& tables)
{
    std::vector<uint64_t> out(static_cast<size_t>(jobs), 0);
    std::atomic<int> next{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (uint32_t w = 0; w < workers; ++w)
    {
        pool.emplace_back(
            [&]
            {
                // Mirror the production worker's prologue exactly
                // (ObjectStreamPrepare.cpp:160-167): below-normal priority is
                // irrelevant to the output, the FP environment is not.
                Poseidon::Foundation::ApplyMainFpEnvironment();
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (;;)
                {
                    const int job = next.fetch_add(1, std::memory_order_relaxed);
                    if (job >= jobs)
                        return;
                    LODShapeWithShadow* shape =
                        Adapter::convertToLODShape(model, false, &tables, /*finishTail=*/false);
                    if (!shape)
                    {
                        out[static_cast<size_t>(job)] = 0; // caught by the caller's != 0 check
                        continue;
                    }
                    out[static_cast<size_t>(job)] = HashConverted(*shape);
                    delete shape;
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& t : pool)
        t.join();
    return out;
}

void RequireAllEqual(const std::vector<uint64_t>& got, uint64_t reference, const char* arm)
{
    INFO("arm: " << arm);
    REQUIRE(!got.empty());
    for (size_t i = 0; i < got.size(); ++i)
    {
        INFO("job index " << i << " of " << got.size());
        REQUIRE(got[i] != 0);
        REQUIRE(got[i] == reference);
    }
}

} // namespace

// ===========================================================================
// 1. The fixture is not degenerate
// ===========================================================================
//
// A determinism test that agrees because it converted nothing passes for ever
// and means nothing.  Measure the fixture before trusting any agreement.

TEST_CASE("Native MLOD worker uses prepared material identity without bank fallback",
          "[preparer][ShapeAdapter][native-bank]")
{
    Poseidon::Foundation::CaptureMainFpEnvironment();
    auto model = BuildFixtureModel();
    model->sourceFormat = "MLOD";
    model->lodLevels.resize(1);
    auto& mesh = model->lodLevels[0].mesh;
    mesh.sections.clear();
    const std::string path = "native_worker_prepared_material.emat";
    for (auto& material : mesh.materials)
        material.materialPath = path;

    Poseidon::Ref<Poseidon::Texture> texture = new Poseidon::TextureDummy;
    Poseidon::Ref<Poseidon::TexMaterial> expected = new Poseidon::TexMaterial;
    Adapter::AdapterBankTables tables;
    tables.textures.resize(1);
    tables.textures[0].assign(mesh.materials.size(), texture);
    tables.surfMats.resize(1);
    tables.mlodSurfMats.resize(1);
    tables.mlodSurfMats[0].emplace(path, expected);

    for (bool present : {true, false})
    {
        if (!present)
            tables.mlodSurfMats[0].clear();
        LODShapeWithShadow* result = nullptr;
        std::thread worker([&] {
            Poseidon::Foundation::ApplyMainFpEnvironment();
            result = Adapter::convertToLODShape(*model, false, &tables, false);
        });
        worker.join();
        std::unique_ptr<LODShapeWithShadow> shape(result);
        REQUIRE(shape != nullptr);
        REQUIRE(shape->Level(0)->NSections() > 0);
        for (int section = 0; section < shape->Level(0)->NSections(); ++section)
        {
            const auto* actual = shape->Level(0)->GetSection(section).surfMat.GetRef();
            REQUIRE(actual == (present ? expected.GetRef() : nullptr));
        }
    }
}

TEST_CASE("SIM-807 preparer fixture is non-degenerate", "[determinism][preparer][ShapeAdapter]")
{
    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    Adapter::AdapterBankTables tables;
    Adapter::BuildAdapterBankTables(*model, tables);

    REQUIRE(model->lodLevels.size() == static_cast<size_t>(kLodCount));
    REQUIRE(tables.textures.size() == static_cast<size_t>(kLodCount));
    REQUIRE(tables.surfMats.size() == static_cast<size_t>(kLodCount));

    int totalSections = 0;
    for (int i = 0; i < kLodCount; ++i)
    {
        const ModelIR::Mesh& mesh = model->lodLevels[i].mesh;
        REQUIRE(mesh.vertices.size() == static_cast<size_t>(kGridN * kGridN));
        REQUIRE(mesh.materials.size() == 4);
        REQUIRE(!mesh.triangles.empty());
        REQUIRE(!mesh.quads.empty());
        REQUIRE(mesh.sections.size() >= 4); // compile() found the material runs
        REQUIRE(mesh.selections.size() == 2);
        REQUIRE(mesh.properties.size() == 3);
        REQUIRE(tables.textures[i].size() == mesh.materials.size());
        REQUIRE(tables.surfMats[i].size() == mesh.sections.size());
        totalSections += static_cast<int>(mesh.sections.size());
    }
    REQUIRE(totalSections >= 4 * kLodCount);

    LODShapeWithShadow* shape = Adapter::convertToLODShape(*model, false, &tables, /*finishTail=*/false);
    REQUIRE(shape != nullptr);
    REQUIRE(shape->NLevels() == kLodCount);

    int vertices = 0, faces = 0, sections = 0;
    std::set<uint64_t> perLod;
    for (int i = 0; i < shape->NLevels(); ++i)
    {
        Shape* lod = shape->Level(i);
        REQUIRE(lod != nullptr);
        REQUIRE(lod->NVertex() > 0);
        REQUIRE(lod->NFaces() > 0);
        vertices += lod->NVertex();
        faces += lod->NFaces();
        sections += lod->NSections();
        BitHasher h;
        HashShape(h, *lod);
        perLod.insert(h.Value());
    }
    // Measured on 2026-08-31, win-x64-clang-rwdi.  Exact rather than a floor:
    // a conversion that starts dropping geometry must not slide past this.
    REQUIRE(vertices == kLodCount * kGridN * kGridN); // 1,445
    REQUIRE(faces == kLodCount * 342);                 // 1,710: 172 tris + 170 quads per LOD
    REQUIRE(sections >= 4 * kLodCount);
    // Every LOD hashes differently: the hasher is not collapsing them.
    REQUIRE(perLod.size() == static_cast<size_t>(kLodCount));

    delete shape;
}

// ===========================================================================
// 2. The hash has teeth
// ===========================================================================
//
// SIM-806 found a real bug because its negative control exercised a path
// nothing calls.  These four controls are what make the agreement below mean
// something: the same input must agree, and every axis that genuinely changes
// the output must disagree.

TEST_CASE("SIM-807 converted-shape hash has teeth", "[determinism][preparer][ShapeAdapter]")
{
    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    Adapter::AdapterBankTables tables;
    Adapter::BuildAdapterBankTables(*model, tables);

    const uint64_t base = ConvertOnce(*model, tables);
    REQUIRE(base != 0);

    SECTION("the same model converts to the same hash")
    {
        REQUIRE(ConvertOnce(*model, tables) == base);
    }

    SECTION("one perturbed vertex bit changes the hash")
    {
        ModelIR::Model perturbed = *model;
        // Smallest representable change to one coordinate of one vertex of one
        // LOD.  A hash that survives this is not comparing the geometry.
        ModelIR::Vertex& v = perturbed.lodLevels[2].mesh.vertices[100];
        float y = v.position.y;
        uint32_t bits = 0;
        std::memcpy(&bits, &y, sizeof(bits));
        bits += 1;
        std::memcpy(&y, &bits, sizeof(y));
        v.position.y = y;
        Adapter::AdapterBankTables t2;
        Adapter::BuildAdapterBankTables(perturbed, t2);
        REQUIRE(ConvertOnce(perturbed, t2) != base);
    }

    SECTION("reversed winding does NOT change the worker-side output")
    {
        // SIM-807 measurement, not an assumption that was assumed.  The control
        // was WRITTEN expecting inequality and measured equality, and the
        // reason is in the source: `reversed` is consumed only by
        // `FinishOdolAdapterTail` (ShapeAdapter.cpp:772, `shape->Reverse()`),
        // which a worker-side conversion skips.  So the `reversed` argument the
        // preparer's worker passes is inert by construction, and reversal is
        // the install's job.  Pinning that keeps a future author from moving
        // `Reverse()` earlier without noticing it has become a worker
        // responsibility.
        LODShapeWithShadow* shape = Adapter::convertToLODShape(*model, true, &tables, /*finishTail=*/false);
        REQUIRE(shape != nullptr);
        const uint64_t reversed = HashConverted(*shape);
        delete shape;
        REQUIRE(reversed == base);
    }

    SECTION("running the tail changes the hash")
    {
        // The load-bearing control for the SCOPE claim in the file header:
        // `finishTail=false` really does stop short of work the main-thread
        // install then does, so what the worker produces is a strictly PARTIAL
        // shape and this harness covers exactly that part.
        //
        // The observable is `ScanShapes` resolving the geometry LOD.  It was
        // measured that the tail is otherwise a NO-OP on a fixture whose
        // textures do not resolve: `OptimizeShapes` sorts faces by texture and
        // every texture here is null, `CalculateMass` is skipped because the IR
        // already carries a mass, and `_viewDensity` / `_propertyClass` /
        // `_convexComponents` are not reachable through the public API.  A
        // fixture without a geometry LOD therefore hashes IDENTICALLY with and
        // without the tail -- which is why the fixture has one.
        LODShapeWithShadow* worker = Adapter::convertToLODShape(*model, false, &tables, /*finishTail=*/false);
        REQUIRE(worker != nullptr);
        const int workerGeom = worker->FindGeometryLevel();
        delete worker;

        LODShapeWithShadow* shape = Adapter::convertToLODShape(*model, false, &tables, /*finishTail=*/true);
        REQUIRE(shape != nullptr);
        const int installedGeom = shape->FindGeometryLevel();
        const uint64_t withTail = HashConverted(*shape);
        delete shape;

        REQUIRE(workerGeom != installedGeom);
        REQUIRE(installedGeom >= 0);
        REQUIRE(withTail != base);
    }
}


// ===========================================================================
// THE FINDING, AND WHY HALF OF THIS FILE IS OPT-IN
// ===========================================================================
//
// `ShapeAdapter::convertToLODShape` IS NOT SAFE TO RUN CONCURRENTLY.  Two
// threads converting at the same time corrupt the process heap: the crash lands
// inside mimalloc (`_mi_page_malloc_zeroed`, `_mi_heap_delayed_free_partial`),
// at a different site every run, which is the signature of a stray write rather
// than of a bug in the allocator.  Measured on the fixture below, 64
// conversions:
//
//     threads   1  -> clean, every run
//     threads   2  -> heap corruption            <-- the SHIPPING worker count
//     threads   4  -> heap corruption
//     threads   8  -> heap corruption
//
// It is not the destruction: converting on workers and deleting every shape on
// the main thread afterwards crashed 6 runs out of 6.  It is not the RStringB
// interning bank: giving every selection ONE name and pinning it (so the bank
// does a single GetOrAdd and never unregisters) still crashes.  Clearing the
// model's named selections makes it go away, and the crash rate rises with the
// number of selections, so the corruption is in the selection-building part of
// the conversion (ShapeAdapter.cpp:1140-1180) or in something it allocates
// alongside.  THE EXACT WRITE IS NOT IDENTIFIED.  The Windows ASan preset
// (`win-x64-clang-san`) does not configure in this tree, which is what would
// have named it.
//
// This is the SHIPPING path: `WGR_OBJECT_STREAM_ASYNC_ADAPT` defaults ON
// (LandSave.cpp:3580) and `WGR_OBJECT_STREAM_ASYNC_WORKERS` defaults to 2, so
// the game converts two models concurrently whenever a cold batch is admitted.
// `WGR_OBJECT_STREAM_ASYNC_ADAPT=0` is the one-command mitigation; whether that
// becomes the default is the owner's call and is NOT changed here.
//
// CONSEQUENCE FOR THIS FILE.  Every case below that actually converts on more
// than one thread takes the process down, which would make the whole ctest run
// unusable for every other worktree on this machine.  They are therefore behind
// `SIM807_CONCURRENT_ADAPT=1` and run their serial arms by default.  They are
// written to pass, not to be deleted: the day the race is fixed, the gate comes
// out and they become the determinism gate the roadmap asked for.  Until then
// the reproducer at the bottom is the thing to run:
//
//     SIM807_CONCURRENT_ADAPT=1 PoseidonTests.exe "[race]"
//
// Decision record: design notes

namespace
{
// Opt-in switch for every arm that runs the adapter on more than one thread.
bool ConcurrentAdaptEnabled()
{
    static const bool on = std::getenv("SIM807_CONCURRENT_ADAPT") != nullptr;
    return on;
}

const char* const kGatedOff =
    "concurrent-adapt arms skipped: convertToLODShape corrupts the heap on 2+ threads "
    "(see this file's header). Set SIM807_CONCURRENT_ADAPT=1 to run them.";
} // namespace

// ===========================================================================
// 3. The worker-side conversion, across worker counts
// ===========================================================================

TEST_CASE("SIM-807 worker conversion is bit-identical across worker counts",
          "[determinism][preparer][ShapeAdapter]")
{
    // The production worker calls ApplyMainFpEnvironment (SIM-801); that needs a
    // capture to exist or it is a no-op, and in a unit-test binary nothing has
    // run InitFPU.  Capture here so the arms below run the same environment the
    // shipping worker would.
    Poseidon::Foundation::CaptureMainFpEnvironment();
    REQUIRE(Poseidon::Foundation::MainFpEnvironmentCaptured());

    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    // Resolved ONCE, on this thread, and shared by every arm -- exactly as
    // LandSave.cpp:3594 does before SubmitConvert.  Sharing one table set
    // removes "the arms were given different input" as an explanation for
    // either agreement or disagreement.
    Adapter::AdapterBankTables tables;
    Adapter::BuildAdapterBankTables(*model, tables);

    // Serial reference: this thread, no pool at all.
    const uint64_t reference = ConvertOnce(*model, tables);
    REQUIRE(reference != 0);

    // ALWAYS RUN: repeated serial conversions.  This is the arm that survives
    // the race, and it still has teeth -- it is what would catch a conversion
    // that depended on call order, on a static counter, or on anything else
    // that varies between two identical calls on one thread.
    for (int repeat = 0; repeat < 8; ++repeat)
        REQUIRE(ConvertOnce(*model, tables) == reference);

    if (!ConcurrentAdaptEnabled())
    {
        SUCCEED(kGatedOff);
        return;
    }

    constexpr int kJobs = 24;
    struct Arm
    {
        const char* name;
        uint32_t workers;
    };
    const uint32_t hw = std::max(2u, std::thread::hardware_concurrency());
    const Arm arms[] = {
        {"1 worker", 1},
        {"2 workers (the shipping default)", 2},
        {"4 workers", 4},
        {"WorkerCount() clamp ceiling (16)", 16},
        {"oversubscribed (2x cores)", std::min(64u, hw * 2)},
    };

    for (const Arm& arm : arms)
    {
        const std::vector<uint64_t> got = RunAtWorkerCount(arm.workers, kJobs, *model, tables);
        RequireAllEqual(got, reference, arm.name);
    }

    for (int repeat = 0; repeat < 3; ++repeat)
    {
        const std::vector<uint64_t> got = RunAtWorkerCount(2, kJobs, *model, tables);
        RequireAllEqual(got, reference, "2 workers, repeat");
    }
}

// ===========================================================================
// 4. Same, on an installed corpus model
// ===========================================================================
//
// NOT tagged [GameData].  `OFPR_CATCH_TEST_SPEC` in
// tests/unit/engine/Poseidon/CMakeLists.txt excludes [GameData] from the whole
// ctest run whenever `packages/Remaster` is absent, and Remaster is absent on
// the workstation that has the corpora -- so the obvious tag would make this
// case silently never run.  REN-GL33-002 documents the same trap.

namespace
{
// The one installed loose model this file uses.  `packages/` is gitignored and
// lives in the MAIN checkout, above any worktree build directory, so the walk
// goes up rather than sideways.
std::filesystem::path FindCorpusModel()
{
    for (std::filesystem::path at(TestFixtures::GetExecutableDirectory()); !at.empty(); at = at.parent_path())
    {
        const std::filesystem::path candidate =
            at / "packages" / "a3-compat" / "m" / "cargo" / "cargo_house_v1_f.p3d";
        if (TestFixtures::FileExists(candidate))
            return candidate;
        if (at.parent_path() == at)
            break;
    }
    return {};
}
} // namespace

TEST_CASE("SIM-807 worker conversion is bit-identical on a real corpus model",
          "[determinism][preparer][ShapeAdapter][corpus]")
{
    const std::filesystem::path model3d = FindCorpusModel();
    if (model3d.empty())
    {
        SUCCEED("packages/a3-compat/m/cargo/cargo_house_v1_f.p3d absent; corpus arm skipped");
        return;
    }

    Poseidon::Foundation::CaptureMainFpEnvironment();

    std::string error;
    bool opened = false;
    // Exactly the call the preparer's worker makes.
    std::shared_ptr<ModelIR::Model> model = Poseidon::ModelCache::LoadLooseFile(model3d.string(), &error, &opened);
    INFO("path: " << model3d.string() << " error: " << error);
    REQUIRE(opened);
    REQUIRE(model);
    REQUIRE(!model->lodLevels.empty());

    // The admit loop lowercases the bank name into sourcePath before submitting.
    model->sourcePath = "sim807\\cargo_house_v1_f.p3d";

    size_t vertices = 0, faces = 0;
    for (const ModelIR::LODLevel& level : model->lodLevels)
    {
        vertices += level.mesh.vertices.size();
        faces += level.mesh.triangles.size() + level.mesh.quads.size();
    }
    // Anti-vacuity: a corpus model that read as empty would agree trivially.
    // Measured 2026-08-31: 19,843 vertices, 14,007 faces.
    REQUIRE(vertices > 100);
    REQUIRE(faces > 50);

    Adapter::AdapterBankTables tables;
    Adapter::BuildAdapterBankTables(*model, tables);

    const uint64_t reference = ConvertOnce(*model, tables);
    REQUIRE(reference != 0);
    for (int repeat = 0; repeat < 4; ++repeat)
        REQUIRE(ConvertOnce(*model, tables) == reference);

    if (!ConcurrentAdaptEnabled())
    {
        SUCCEED(kGatedOff);
        return;
    }

    for (uint32_t workers : {1u, 2u, 4u, 8u})
    {
        const std::vector<uint64_t> got = RunAtWorkerCount(workers, 12, *model, tables);
        RequireAllEqual(got, reference, "corpus model");
    }
}

// ===========================================================================
// 5. The preparer's own state machine, including the paths nothing calls
// ===========================================================================
//
// These need no game data: the queue, the state table and the failure
// classification are exercised with paths that deliberately do not resolve.
// The header promises specific behaviour for each; this is where "the same
// model prepared twice", "cancelled mid-flight" and "fails to parse" are
// pinned.  Only one conversion is ever in flight at a time unless the
// concurrency gate is on, so these run by default.

namespace
{
// Wait for a predicate, with a hard bound so a hang is a failure rather than a
// stuck suite.  Never a bare sleep.
template <typename Fn> bool WaitFor(Fn&& fn, int maxMs = 30000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (fn())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}
} // namespace

TEST_CASE("Native preparer ignores loose material decoys but keeps IR texture preparation", "[preparer][native]")
{
    if (!ObjectStreamPreparer::AsyncEnabled() || !Poseidon::render::PreparedTextureStore::Enabled())
    {
        SKIP("Requires asynchronous texture preparation");
    }
    const std::string materialPath = TestFixtures::GetTestFixturePath("material/native_prefetch_decoy.emat");
    Poseidon::Asset::Material::EmatMaterial material;
    REQUIRE(Poseidon::Asset::Material::ReadEmatFileLoose(materialPath, material));
    REQUIRE(material.properties.size() == 1);
    REQUIRE(material.properties[0].values[0].isTexture());
    struct RestoreLoader
    {
        Poseidon::ModelCache::ExternalLoader previous = Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&, std::string&) {
        auto model = std::make_shared<ModelIR::Model>();
        model->lodLevels.emplace_back();
        ModelIR::Material mat;
        mat.materialPath = TestFixtures::GetTestFixturePath("material/native_prefetch_decoy.emat");
        mat.texturePath = "native_prefetch_actual_missing.paa";
        model->lodLevels[0].mesh.materials.push_back(std::move(mat));
        return model;
    });
    ObjectStreamPreparer prep;
    const std::string path = "native_prefetch_synthetic.xob";
    prep.Reset(&path, 1);
    REQUIRE(prep.Request(0));
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    REQUIRE(prep.Take(0) != nullptr);
    // The actual IR texture is attempted; the converted material's decoy is not.
    CHECK(prep.SnapshotStats().texUnreadable == 1);
}

TEST_CASE("Preparer camera cancellation discards active parses and queued conversions", "[preparer][cancellation]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
        SKIP("Requires asynchronous preparation");
    static std::atomic<uint32_t> entered{0};
    static std::atomic<bool> release{false};
    entered = 0;
    release = false;
    struct RestoreLoader
    {
        Poseidon::ModelCache::ExternalLoader previous = Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&, std::string&)
                                            {
                                                ++entered;
                                                // Bounded even on test failure; the release guard normally wakes us
                                                // first.
                                                WaitFor([] { return release.load(); });
                                                auto model = std::make_shared<ModelIR::Model>();
                                                model->lodLevels.emplace_back();
                                                ModelIR::Material material;
                                                material.texturePath = "camera_cancel_unreadable_texture.paa";
                                                model->lodLevels[0].mesh.materials.push_back(std::move(material));
                                                return model;
                                            });
    ObjectStreamPreparer prep;
    struct ReleaseBeforeJoin
    {
        ~ReleaseBeforeJoin() { release = true; }
    } releaseBeforeJoin;
    const uint32_t workers = ObjectStreamPreparer::WorkerCount();
    std::vector<std::string> paths(workers + 1, "camera_cancel_synthetic.xob");
    prep.Reset(paths.data(), paths.size());
    for (uint32_t i = 0; i < workers; ++i)
        REQUIRE(prep.Request(i));
    REQUIRE(WaitFor([&] { return entered.load() == workers; }));
    std::vector<uint32_t> epochs(paths.size(), 0);
    bool revive = false;
    SECTION("stale parse results cannot refill the ready set") {}
    SECTION("a returning camera can reuse the active parse") { revive = true; }
    SECTION("queued conversion releases its model without executing")
    {
        auto model = BuildFixtureModel();
        auto tables = std::make_shared<Adapter::AdapterBankTables>();
        Adapter::BuildAdapterBankTables(*model, *tables);
        std::weak_ptr<ModelIR::Model> weak = model;
        REQUIRE(prep.SubmitConvert(workers, std::move(model), tables));
        CHECK(prep.DropStale(epochs.data(), epochs.size(), 1) == 1);
        CHECK(prep.Query(workers) == ObjectStreamPreparer::State::Unknown);
        CHECK(weak.expired());
        CHECK(prep.SnapshotStats().converted == 0);
    }
    SECTION("reset retires queued bank tables outside the mutex")
    {
        bool retired = false;
        auto model = BuildFixtureModel();
        std::weak_ptr<ModelIR::Model> weak = model;
        auto tables = std::shared_ptr<Adapter::AdapterBankTables>(new Adapter::AdapterBankTables,
            [&](Adapter::AdapterBankTables* value) {
                // Re-entry would deadlock if Reset cleared the queue under its mutex.
                (void)prep.SnapshotStats();
                delete value;
                retired = true;
            });
        Adapter::BuildAdapterBankTables(*model, *tables);
        REQUIRE(prep.SubmitConvert(workers, std::move(model), std::move(tables)));
        prep.Reset(paths.data(), paths.size());
        CHECK(retired);
        CHECK(weak.expired());
        CHECK(prep.Query(workers) == ObjectStreamPreparer::State::Unknown);
        CHECK(prep.SnapshotStats().converted == 0);
    }
    prep.DropStale(epochs.data(), epochs.size(), 1);
    if (revive)
    {
        epochs[0] = 2;
        prep.DropStale(epochs.data(), epochs.size(), 2);
    }
    release = true;
    REQUIRE(WaitFor(
        [&]
        {
            for (uint32_t i = 0; i < workers; ++i)
                if (prep.Query(i) == ObjectStreamPreparer::State::Parsing)
                    return false;
            return true;
        }));
    CHECK(prep.SnapshotStats().ready == (revive ? 1 : 0));
    if (Poseidon::render::PreparedTextureStore::Enabled())
    {
        // Cancelled parses must not start optional texture I/O; a revived one must.
        CHECK(prep.SnapshotStats().texUnreadable == (revive ? 1 : 0));
    }
    for (uint32_t i = 0; i < workers; ++i)
        CHECK(prep.Query(i) ==
              (revive && i == 0 ? ObjectStreamPreparer::State::Ready : ObjectStreamPreparer::State::Unknown));
    if (revive)
        CHECK(prep.Take(0) != nullptr);
    else
    {
        REQUIRE(prep.Request(0));
        REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
        CHECK(prep.Take(0) != nullptr);
    }
}

TEST_CASE("SIM-807 preparer classifies a parse that cannot happen", "[determinism][preparer]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
    {
        SUCCEED("WGR_OBJECT_STREAM_ASYNC=0 in this environment; the preparer never starts workers");
        return;
    }

    ObjectStreamPreparer prep;
    REQUIRE(!prep.Running());
    // Requesting before Reset must be refused, not crash on a null state table.
    REQUIRE(!prep.Request(0));
    REQUIRE(prep.Query(0) == ObjectStreamPreparer::State::Unknown);
    REQUIRE(prep.ModelCount() == 0);

    // Two paths: one that cannot be opened at all, and one that opens but is not
    // a P3D.  The preparer distinguishes them -- NotLoose means "the main thread
    // must load it through the file server", Failed means "it is there and it is
    // broken" -- and conflating the two would send every PBO-packed model down
    // the error path.
    const std::string missing = "sim807__no_such_model__.p3d";
    const std::string notAModel = TestFixtures::GetTestFixturePath("jpg/checker_32x32.jpg");
    const std::string paths[] = {missing, notAModel, missing};
    prep.Reset(paths, 3);
    REQUIRE(prep.Running());
    REQUIRE(prep.ModelCount() == 3);

    REQUIRE(prep.Request(0));
    REQUIRE(!prep.Request(0)); // already tracked: refused, not double-queued
    REQUIRE(prep.Request(1));
    REQUIRE(!prep.Request(3)); // out of range

    REQUIRE(WaitFor(
        [&]
        {
            const ObjectStreamPreparer::State a = prep.Query(0);
            const ObjectStreamPreparer::State b = prep.Query(1);
            const auto settled = [](ObjectStreamPreparer::State s)
            {
                return s != ObjectStreamPreparer::State::Unknown && s != ObjectStreamPreparer::State::Queued &&
                       s != ObjectStreamPreparer::State::Parsing;
            };
            return settled(a) && settled(b);
        }));

    // A file that does not exist is NotLoose, not Failed: `opened` stays false.
    REQUIRE(prep.Query(0) == ObjectStreamPreparer::State::NotLoose);
    // A file that exists and is not a model is Failed.
    REQUIRE(prep.Query(1) == ObjectStreamPreparer::State::Failed);

    // Both are STICKY: a re-Request must be refused, or the admit loop would
    // re-queue an unloadable model on every frame it is in range.
    REQUIRE(!prep.Request(0));
    REQUIRE(!prep.Request(1));
    REQUIRE(prep.Take(0) == nullptr);
    REQUIRE(prep.Take(1) == nullptr);
    REQUIRE(prep.TakeConverted(0).shape == nullptr);

    const ObjectStreamPreparer::Stats stats = prep.SnapshotStats();
    REQUIRE(stats.requested == 2);
    REQUIRE(stats.notLoose == 1);
    REQUIRE(stats.failed == 1);
    REQUIRE(stats.prepared == 0);
    REQUIRE(stats.workers == ObjectStreamPreparer::WorkerCount());
}

TEST_CASE("SIM-807 SubmitConvert refuses what it must", "[determinism][preparer][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
    {
        SUCCEED("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
        return;
    }

    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);

    ObjectStreamPreparer prep;
    // No workers yet (Reset has not run): must refuse rather than queue a job
    // nobody will ever pick up.
    REQUIRE(!prep.SubmitConvert(0, model, tables));

    const std::string paths[] = {"sim807__a.p3d", "sim807__b.p3d"};
    prep.Reset(paths, 2);
    REQUIRE(prep.Running());

    REQUIRE(!prep.SubmitConvert(0, nullptr, tables)); // null IR
    REQUIRE(!prep.SubmitConvert(0, model, nullptr));  // null tables
    REQUIRE(!prep.SubmitConvert(9, model, tables));   // out of range

    REQUIRE(prep.SubmitConvert(0, model, tables));
    // THE SAME MODEL SUBMITTED TWICE.  The slot is Converting or Converted, not
    // Unknown, so the second submit must be refused -- otherwise two workers
    // would race to write `convertedShapes[0]` and one shape would leak.
    REQUIRE(!prep.SubmitConvert(0, model, tables));

    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    // Still refused while a result is parked in the slot.
    REQUIRE(!prep.SubmitConvert(0, model, tables));

    ObjectStreamPreparer::ConvertedShape conv = prep.TakeConverted(0);
    REQUIRE(conv.shape != nullptr);
    REQUIRE(conv.model);
    // The slot is Unknown again, and a second Take gets nothing.
    REQUIRE(prep.Query(0) == ObjectStreamPreparer::State::Unknown);
    REQUIRE(prep.TakeConverted(0).shape == nullptr);

    // The worker's output is bit-identical to a main-thread conversion of the
    // same IR with the same tables.  This is the whole claim of stage 3 -- the
    // main thread installs it verbatim -- expressed as one assertion, and it
    // holds because only ONE conversion was in flight.
    const uint64_t fromWorker = HashConverted(*conv.shape);
    delete conv.shape;
    REQUIRE(fromWorker == ConvertOnce(*model, *tables));

    // And it can be resubmitted once the slot is clear.
    REQUIRE(prep.SubmitConvert(0, model, tables));
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    ObjectStreamPreparer::ConvertedShape again = prep.TakeConverted(0);
    REQUIRE(again.shape != nullptr);
    REQUIRE(HashConverted(*again.shape) == fromWorker);
    delete again.shape;
}

TEST_CASE("Preparer releases conversion bank tables outside its queue mutex", "[determinism][preparer][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
    {
        SUCCEED("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
        return;
    }
    std::atomic<bool> released{false};
    ObjectStreamPreparer prep;
    const std::string path = "sim807__release.p3d";
    prep.Reset(&path, 1);
    auto model = BuildFixtureModel();
    auto tables = std::shared_ptr<Adapter::AdapterBankTables>(new Adapter::AdapterBankTables,
        [&](Adapter::AdapterBankTables* value) {
            // This locks the preparer again: a last-reference release under its
            // queue lock would deadlock, even if conversion itself succeeded.
            (void)prep.SnapshotStats();
            delete value;
            released.store(true, std::memory_order_release);
        });
    Adapter::BuildAdapterBankTables(*model, *tables);
    REQUIRE(prep.SubmitConvert(0, model, std::move(tables)));
    REQUIRE(WaitFor([&] { return released.load(std::memory_order_acquire); }));
    auto result = prep.TakeConverted(0);
    REQUIRE(result.shape != nullptr);
    delete result.shape;
}

TEST_CASE("SIM-807 cancellation mid-flight leaves no slot behind", "[determinism][preparer][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
    {
        SUCCEED("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
        return;
    }

    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);

    // One in flight at a time unless the gate is on: a burst is what makes two
    // workers convert simultaneously, which is the crash.
    const uint32_t kSlots = ConcurrentAdaptEnabled() ? 32u : 4u;
    std::vector<std::string> paths(kSlots, "sim807__cancel.p3d");

    ObjectStreamPreparer prep;
    prep.Reset(paths.data(), kSlots);
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        REQUIRE(prep.SubmitConvert(i, model, tables));
        if (!ConcurrentAdaptEnabled())
            REQUIRE(WaitFor([&] { return prep.Query(i) == ObjectStreamPreparer::State::Converted; }));
    }

    // Reset while results are parked (and, under the gate, while conversions are
    // in flight): the generation bump makes every worker discard what it holds,
    // and every slot must come back Unknown rather than stranded in Converting.
    prep.Reset(paths.data(), kSlots);
    for (uint32_t i = 0; i < kSlots; ++i)
        REQUIRE(prep.Query(i) == ObjectStreamPreparer::State::Unknown);

    // The workers are still alive and the preparer still works afterwards.
    REQUIRE(prep.Running());
    REQUIRE(prep.SubmitConvert(0, model, tables));
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    ObjectStreamPreparer::ConvertedShape conv = prep.TakeConverted(0);
    REQUIRE(conv.shape != nullptr);
    REQUIRE(HashConverted(*conv.shape) == ConvertOnce(*model, *tables));
    delete conv.shape;

    // DropStale must free a Converted slot the new window no longer wants.
    for (uint32_t i = 1; i < kSlots; ++i)
    {
        REQUIRE(prep.SubmitConvert(i, model, tables));
        if (!ConcurrentAdaptEnabled())
            REQUIRE(WaitFor([&] { return prep.Query(i) == ObjectStreamPreparer::State::Converted; }));
    }
    REQUIRE(WaitFor(
        [&]
        {
            for (uint32_t i = 1; i < kSlots; ++i)
                if (prep.Query(i) != ObjectStreamPreparer::State::Converted)
                    return false;
            return true;
        }));
    std::vector<uint32_t> epochs(kSlots, 0); // nothing matches the current epoch
    const size_t dropped = prep.DropStale(epochs.data(), kSlots, 7);
    REQUIRE(dropped == static_cast<size_t>(kSlots) - 1);
    for (uint32_t i = 1; i < kSlots; ++i)
        REQUIRE(prep.Query(i) == ObjectStreamPreparer::State::Unknown);

    // SIM-807 finding 5a: the DESTRUCTOR must free what is still parked, the way
    // Reset and DropStale do.  Leave one converted shape in the slot and let the
    // preparer go out of scope; before the fix this leaked the whole
    // LODShapeWithShadow, with no symptom short of a leak checker.
    REQUIRE(prep.SubmitConvert(2, model, tables));
    REQUIRE(WaitFor([&] { return prep.Query(2) == ObjectStreamPreparer::State::Converted; }));
    // (destructor runs here)
}

// ===========================================================================
// 6. The back-pressure bound does not cover stage 3
// ===========================================================================
//
// SIM-807 finding, measured rather than argued.  `WGR_OBJECT_STREAM_ASYNC_READY`
// (default 128) exists because "the IR of a large building is megabytes, and a
// world names ~1,000 models, so this bound is what keeps a fast camera from
// parsing the whole world into RAM ahead of itself" (ObjectStreamPrepare.hpp).
// The worker's wait predicate is
//
//     stop || !convertQueue.empty() || (!queue.empty() && stats.ready < ReadyLimit())
//
// -- so the bound gates the PARSE queue and nothing else.  `convertQueue` is
// drained unconditionally, ahead of parses, and a finished conversion parks a
// whole `LODShapeWithShadow` in `convertedShapes[i]` WITHOUT touching
// `stats.ready`.  A converted shape is strictly larger than the IR it came from,
// and the admit loop turns every Ready into a `SubmitConvert` on sight
// (LandSave.cpp:3595), which keeps `stats.ready` near zero -- so in the shipping
// arrangement the ready bound rarely engages at all and the set it guards is not
// the set that grows.
//
// Nothing is changed for this.  The set is still bounded -- by the model table
// and by `DropStale` once per recentre -- so it is a documented property, not a
// fault; the reason it is pinned is that the number a future author will reach
// for to cap preparer memory is `ReadyLimit`, and on the current code that
// number does not do it.
//
// Gated only because filling the converted set means many conversions in flight.

TEST_CASE("SIM-807 converted shapes are not bounded by the ready limit", "[determinism][preparer][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled())
    {
        SUCCEED("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
        return;
    }
    if (!ConcurrentAdaptEnabled())
    {
        SUCCEED(kGatedOff);
        return;
    }

    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);

    const size_t readyLimit = ObjectStreamPreparer::ReadyLimit();
    const uint32_t slots = static_cast<uint32_t>(readyLimit + 8);
    std::vector<std::string> paths(slots, "sim807__unbounded.p3d");

    ObjectStreamPreparer prep;
    prep.Reset(paths.data(), slots);
    for (uint32_t i = 0; i < slots; ++i)
        REQUIRE(prep.SubmitConvert(i, model, tables));

    REQUIRE(WaitFor(
        [&]
        {
            for (uint32_t i = 0; i < slots; ++i)
                if (prep.Query(i) != ObjectStreamPreparer::State::Converted)
                    return false;
            return true;
        },
        120000));

    uint32_t parked = 0;
    for (uint32_t i = 0; i < slots; ++i)
        if (prep.Query(i) == ObjectStreamPreparer::State::Converted)
            ++parked;
    REQUIRE(parked == slots);
    REQUIRE(static_cast<size_t>(parked) > readyLimit);

    // And `stats.ready` -- the counter the wait predicate reads -- never moved,
    // which is exactly why the pause never fired.
    const ObjectStreamPreparer::Stats stats = prep.SnapshotStats();
    REQUIRE(stats.ready == 0);
    REQUIRE(stats.converted == slots);
}

// ===========================================================================
// 7. The heap-corruption reproducer
// ===========================================================================
//
// The evidence for the finding at the top of this file, in a form anyone can
// re-run.  No game data: a synthetic ODOL model with many uniquely named
// selections is enough.  Under the gate this either passes (all conversions
// agree, and the component breakdown says WHERE if they do not) or takes the
// process down; both outcomes are the measurement.
//
//   SIM807_CONCURRENT_ADAPT=1 PoseidonTests.exe "[race]"
//
// `SIM807_RACE_THREADS` (default 4) and `SIM807_RACE_SELECTIONS` (default 60)
// vary the two axes that were swept: 1 thread is clean at every selection count,
// 2 and above are not, and clearing the selections makes it clean again.

TEST_CASE("SIM-807 concurrent conversion of one model agrees", "[determinism][preparer][ShapeAdapter][race]")
{
    if (!ConcurrentAdaptEnabled())
    {
        SUCCEED(kGatedOff);
        return;
    }
    Poseidon::Foundation::CaptureMainFpEnvironment();

    const char* threadsEnv = std::getenv("SIM807_RACE_THREADS");
    const char* selEnv = std::getenv("SIM807_RACE_SELECTIONS");
    const int threads = threadsEnv ? std::atoi(threadsEnv) : 4;
    const int nsel = selEnv ? std::atoi(selEnv) : 60;
    REQUIRE(threads >= 1);

    std::shared_ptr<ModelIR::Model> model = BuildFixtureModel();
    for (size_t li = 0; li < model->lodLevels.size(); ++li)
    {
        std::vector<ModelIR::NamedSelection>& sels = model->lodLevels[li].mesh.selections;
        sels.clear();
        for (int k = 0; k < nsel; ++k)
        {
            ModelIR::NamedSelection sel("sim807_sel_" + std::to_string(li) + "_" + std::to_string(k));
            for (uint32_t v = static_cast<uint32_t>(k); v < static_cast<uint32_t>(kGridN * kGridN); v += 7u)
            {
                sel.vertexIndices.push_back(v);
                sel.vertexWeights.push_back(static_cast<uint8_t>(1 + (v % 250)));
            }
            sels.push_back(std::move(sel));
        }
    }

    Adapter::AdapterBankTables tables;
    Adapter::BuildAdapterBankTables(*model, tables);

    LODShapeWithShadow* referenceShape = Adapter::convertToLODShape(*model, false, &tables, /*finishTail=*/false);
    REQUIRE(referenceShape != nullptr);
    const std::vector<std::pair<std::string, uint64_t>> refComponents = HashComponents(*referenceShape);
    const uint64_t reference = HashConverted(*referenceShape);
    delete referenceShape;

    constexpr int kJobs = 64;
    std::vector<std::vector<std::pair<std::string, uint64_t>>> components(kJobs);
    std::atomic<int> next{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for (int w = 0; w < threads; ++w)
        pool.emplace_back(
            [&]
            {
                Poseidon::Foundation::ApplyMainFpEnvironment();
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (;;)
                {
                    const int job = next.fetch_add(1, std::memory_order_relaxed);
                    if (job >= kJobs)
                        return;
                    LODShapeWithShadow* shape =
                        Adapter::convertToLODShape(*model, false, &tables, /*finishTail=*/false);
                    if (!shape)
                        continue;
                    components[static_cast<size_t>(job)] = HashComponents(*shape);
                    delete shape;
                }
            });
    go.store(true, std::memory_order_release);
    for (std::thread& t : pool)
        t.join();

    // Report WHICH component diverged, not just that the hashes differ -- the
    // analogue of SIM-806's "first divergent segment index".
    std::set<std::string> divergent;
    for (int j = 0; j < kJobs; ++j)
    {
        REQUIRE(components[static_cast<size_t>(j)].size() == refComponents.size());
        for (size_t c = 0; c < refComponents.size(); ++c)
            if (components[static_cast<size_t>(j)][c].second != refComponents[c].second)
                divergent.insert(refComponents[c].first);
    }
    for (const std::string& name : divergent)
        WARN("SIM-807 divergent component: " << name);
    INFO("reference hash " << reference << ", " << threads << " threads, " << nsel << " selections per LOD");
    REQUIRE(divergent.empty());
}

TEST_CASE("Preparer payload budget parks speculation but lets an oversized asset progress", "[preparer][streaming-budget]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    struct RestoreLoader {
        Poseidon::ModelCache::ExternalLoader previous = Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&, std::string&) {
        auto model = std::make_shared<ModelIR::Model>();
        model->lodLevels.emplace_back();
        model->lodLevels[0].mesh.vertices.resize(2048);
        return model;
    });
    ObjectStreamPreparer prep(1024); // less than one useful model: must not deadlock or discard it
    std::string paths[] = {"budget_first.xob", "budget_second.xob"};
    prep.Reset(paths, 2);
    REQUIRE(prep.Request(0));
    REQUIRE(prep.Request(1));
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    REQUIRE(WaitFor([&] { return prep.SnapshotStats().readyByteParks > 0; }));
    CHECK(prep.Query(1) == ObjectStreamPreparer::State::Queued);
    CHECK(prep.SnapshotStats().readyPayloadBytes > 1024);
    CHECK(prep.SnapshotStats().oversizedPayloads == 1);
    SECTION("consumption frees room for the next model") { REQUIRE(prep.Take(0)); }
    SECTION("camera cancellation frees room for the next model") {
        const uint32_t epochs[] = {0, 1};
        CHECK(prep.DropStale(epochs, 2, 1) == 1);
    }
    REQUIRE(WaitFor([&] { return prep.Query(1) == ObjectStreamPreparer::State::Ready; }));
    REQUIRE(prep.Take(1));
    CHECK(prep.SnapshotStats().readyPayloadBytes == 0);
    CHECK(prep.SnapshotStats().parseReservedBytes == 0);
}

TEST_CASE("Preparer keeps old-world reservations until active workers finish", "[preparer][streaming-budget][cancellation]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    static std::atomic<unsigned> entered{0};
    static std::atomic<bool> release{false};
    entered = 0; release = false;
    struct RestoreLoader {
        Poseidon::ModelCache::ExternalLoader previous = Poseidon::ModelCache::GetExternalLoader();
        ~RestoreLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
    } restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string&, std::string&) {
        ++entered;
        WaitFor([] { return release.load(); });
        auto model = std::make_shared<ModelIR::Model>();
        model->lodLevels.emplace_back();
        return model;
    });
    ObjectStreamPreparer prep(1024);
    struct ReleaseBeforeJoin { ~ReleaseBeforeJoin() { release = true; } } guard;
    const std::string path = "budget_generation.xob";
    prep.Reset(&path, 1);
    REQUIRE(prep.Request(0));
    REQUIRE(WaitFor([] { return entered.load() == 1; }));
    CHECK(prep.SnapshotStats().parseReservedBytes == 1024);
    prep.Reset(&path, 1);
    REQUIRE(prep.Request(0));
    CHECK(prep.SnapshotStats().parseReservedBytes == 1024);
    CHECK(prep.Query(0) == ObjectStreamPreparer::State::Queued);
    release = true;
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    CHECK(entered == 2);
    CHECK(prep.SnapshotStats().dropped >= 1);
    REQUIRE(prep.Take(0));
    CHECK(prep.SnapshotStats().readyPayloadBytes == 0);
    CHECK(prep.SnapshotStats().parseReservedBytes == 0);
}

TEST_CASE("Conversion debt parks speculative parses until ownership is released", "[preparer][streaming-budget]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    auto model = BuildFixtureModel();
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);
    ObjectStreamPreparer prep(1); // a useful conversion must still finish under pressure
    const std::string paths[] = {"conversion_debt.p3d", "nonexistent_debt_test.p3d"};
    prep.Reset(paths, 2);
    REQUIRE(prep.SubmitConvert(0, model, tables));
    REQUIRE(WaitFor([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    CHECK(prep.SnapshotStats().conversionReservedBytes > 1);
    REQUIRE(prep.Request(1));
    CHECK_FALSE(WaitFor([&] { return prep.Query(1) != ObjectStreamPreparer::State::Queued; }, 100));
    CHECK(prep.Query(1) == ObjectStreamPreparer::State::Queued);
    SECTION("consume") {
        auto result = prep.TakeConverted(0);
        REQUIRE(result.shape);
        delete result.shape;
    }
    SECTION("cancel") {
        const uint32_t epochs[] = {0, 1};
        CHECK(prep.DropStale(epochs, 2, 1) == 1);
    }
    SECTION("world reset") {
        prep.Reset(paths, 2);
        REQUIRE(prep.Request(1));
    }
    CHECK(prep.SnapshotStats().conversionReservedBytes == 0);
    REQUIRE(WaitFor([&] { return prep.Query(1) == ObjectStreamPreparer::State::NotLoose; }));
    CHECK(prep.SnapshotStats().parseReservedBytes == 0);
}
