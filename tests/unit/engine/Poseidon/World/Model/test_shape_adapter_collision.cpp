// test_shape_adapter_collision.cpp -- COL-001.
//
// "In every game except the base game OFP I can walk through walls and
// buildings." The geometry LODs of every imported generation were loading, and
// their ComponentXX selections with them; what was missing was everything the
// collision RESPONSE reads after a hit:
//
//   * mass. Object::IsPassable() is `GetMass() < 10`, GetMass() is the shape's
//     stored mass, and the later-revision ODOL readers (40, 48-54, 73) consumed
//     the ModelInfo mass fields and threw them away. Every A1/A2/A3/DayZ model
//     therefore weighed nothing, and Man::Simulate skips passable objects.
//   * the special-LOD indices, which the IR never received from those readers,
//     so a tool or a test saw no geometry LOD at all (in the game
//     OptimizeShapes happened to rescan them).
//   * on the MLOD path, the `#Mass#` tagg (never carried) and the selection
//     face encoding, which numbered a triangulated stream nothing produced and
//     so lost every quad -- a box component is six quads.
//
// These tests state each of those from a fixture the repo can hold: a synthetic
// ODOL 40/49/54/73 struct model and an authored P3DM MLOD file, both a 2 x 3 x 4 m
// box. The last test reads a real model from a local corpus when
// POSEIDON_COLLISION_FIXTURE names one, and is skipped otherwise: the licensed
// generations (Arma 1/2/3, DayZ, Reforger conversions) cannot live in the repo.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include "../../test_fixtures.hpp"

#include <Poseidon/World/MapTypes.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Formats;
using namespace Poseidon::Asset::Formats::P3D;

namespace
{

// The format's own vector type, named through a member rather than by name:
// Shape.hpp pulls in Math3D.hpp, which #defines `Vector3`, and after that the
// spelling `Poseidon::Asset::Formats::Vector3` may or may not be the identifier
// the struct was declared under. decltype of the member is immune to that.
using BisVector3 = decltype(Odol73StaticLodHeader::bboxMin);
// The engine's vector: the same spelling ShapeAdapter itself uses.
using EngineVector3 = ::Vector3;
// The collision types live in namespace Poseidon; LODShapeWithShadow and Shape
// reach the global scope through the engine's own using-declarations, these do not.
using ConvexComponent = Poseidon::ConvexComponent;
using ConvexComponents = Poseidon::ConvexComponents;

// The box every fixture here describes: 2 m wide, 3 m tall, 4 m deep, corner at
// the origin. Face order is the engine's own convention -- for each quad the
// plane built from (p2-p0) x (p1-p0) faces INTO the box, which is what
// ConvexComponent::IsInside tests against.
const BisVector3 kBoxCorners[8] = {
    {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 4.0f},
    {0.0f, 3.0f, 0.0f}, {2.0f, 3.0f, 0.0f}, {2.0f, 3.0f, 4.0f}, {0.0f, 3.0f, 4.0f},
};
const std::vector<std::vector<uint32_t>> kBoxQuads = {
    {0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0},
};
const EngineVector3 kBoxCentre(1.0f, 1.5f, 2.0f);

Odol73StaticLod BuildLod(uint32_t revision, uint32_t vertexCount, const std::vector<std::vector<uint32_t>>& faces,
                         const std::vector<BisVector3>& positions)
{
    Odol73StaticLod lod;
    lod.header.vertexCount = vertexCount;
    lod.header.bboxMin = BisVector3{0.0f, 0.0f, 0.0f};
    lod.header.bboxMax = BisVector3{2.0f, 3.0f, 4.0f};
    lod.header.bboxCenter = BisVector3{1.0f, 1.5f, 2.0f};
    lod.header.bboxRadius = 2.7f;
    lod.polygons.faceStreamHeaderBytes = revision == 73 ? 4 : 2;
    lod.polygons.faceStreamIndexBytes = revision == 73 ? 4 : 2;
    lod.polygons.faces = faces;
    uint32_t stream = 0;
    for (const auto& face : faces)
        stream +=
            lod.polygons.faceStreamHeaderBytes + lod.polygons.faceStreamIndexBytes * static_cast<uint32_t>(face.size());
    lod.polygons.faceDataSize = stream;
    lod.rest.positions = positions;
    lod.rest.normals.assign(vertexCount, BisVector3{0.0f, 1.0f, 0.0f});
    lod.rest.clip.assign(vertexCount, 0);
    lod.rest.uv0.uv.assign(vertexCount, {0.0f, 0.0f});
    lod.rest.uvSetCount = 1;
    return lod;
}

// A visual LOD (one triangle), a geometry LOD (the box, one component), a land
// contact LOD and a roadway LOD -- the last two empty but for their sentinel,
// because what is under test is that the resolutions become indices.
Odol73StaticModel BuildBoxModel(uint32_t revision)
{
    Odol73StaticModel model;
    auto& info = model.directory.model;
    info.boundingSphere = 2.7f;
    info.geometrySphere = 2.7f;
    info.resolutions = {1.0f, 1.0e13f, 2.0e15f, 3.0e15f};
    info.mass = 1200.0f;
    info.invMass = 1.0f / 1200.0f;
    info.armor = 200.0f;
    info.invArmor = 1.0f / 200.0f;
    info.massArray.assign(8, 150.0f);

    // LOD 0: a triangle.
    model.lods.push_back(BuildLod(revision, 3, {{0, 1, 2}},
                                  {BisVector3{0.0f, 0.0f, 0.0f}, BisVector3{2.0f, 0.0f, 0.0f},
                                   BisVector3{2.0f, 3.0f, 0.0f}}));

    // LOD 1: the geometry box with its component.
    Odol73StaticLod geometry =
        BuildLod(revision, 8, kBoxQuads, std::vector<BisVector3>(std::begin(kBoxCorners), std::end(kBoxCorners)));
    Odol73NamedSelection component;
    component.name = "component01";
    component.selectedFaces = {0, 1, 2, 3, 4, 5};
    component.selectedVertices = {0, 1, 2, 3, 4, 5, 6, 7};
    geometry.namedSelections.push_back(component);
    model.lods.push_back(std::move(geometry));

    // LOD 2 and 3: land contact and roadway, one triangle each so they are not empty.
    for (int i = 0; i < 2; ++i)
        model.lods.push_back(BuildLod(revision, 3, {{0, 1, 2}},
                                      {BisVector3{0.0f, 0.0f, 0.0f}, BisVector3{2.0f, 0.0f, 0.0f},
                                       BisVector3{2.0f, 0.0f, 4.0f}}));
    return model;
}

const Poseidon::Model::NamedSelection& SelectionNamed(const Poseidon::Model::Mesh& mesh, const std::string& name)
{
    for (const auto& selection : mesh.selections)
        if (selection.name == name)
            return selection;
    throw std::runtime_error("selection not found: " + name);
}

} // namespace

TEST_CASE("COL-001: later-revision ODOL carries mass and the special-LOD indices into the IR",
          "[ShapeAdapter][collision][ODOL][COL-001]")
{
    for (const uint32_t revision : {40u, 49u, 54u, 73u})
    {
        INFO("ODOL revision " << revision);
        const auto model = ODOLLoader::convertStaticModel(BuildBoxModel(revision), "box.p3d", revision);
        REQUIRE(model.lodLevels.size() == 4);

        // The physical body, which used to be read and discarded.
        REQUIRE(model.mass == Catch::Approx(1200.0f));
        REQUIRE(model.invMass == Catch::Approx(1.0f / 1200.0f));
        REQUIRE(model.armor == Catch::Approx(200.0f));
        REQUIRE(model.massArray.size() == 8);

        // The indices, from the resolutions -- the same derivation ScanShapes uses.
        REQUIRE(model.geometryIdx == 1);
        REQUIRE(model.landContactIdx == 2);
        REQUIRE(model.roadwayIdx == 3);
        REQUIRE(model.memoryIdx == -1);
        REQUIRE(model.pathsIdx == -1);
        // ScanShapes' fallbacks: no view geometry -> geometry; no fire -> view.
        REQUIRE(model.geometryViewIdx == 1);
        REQUIRE(model.geometryFireIdx == 1);
        REQUIRE(model.lodLevels[1].purpose == Poseidon::Model::LodPurpose::Geometry);
    }
}

TEST_CASE("COL-001: an ODOL box collides -- geometry level, one convex component, not passable",
          "[ShapeAdapter][collision][ODOL][COL-001]")
{
    for (const uint32_t revision : {40u, 49u, 54u, 73u})
    {
        INFO("ODOL revision " << revision);
        const auto model = ODOLLoader::convertStaticModel(BuildBoxModel(revision), "box.p3d", revision);
        std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false));
        REQUIRE(shape != nullptr);

        // Without GApp OptimizeShapes never rescans, so this used to be -1 here
        // and the shape had no geometry level at all.
        REQUIRE(shape->FindGeometryLevel() == 1);
        REQUIRE(shape->FindLandContactLevel() == 2);
        REQUIRE(shape->FindRoadwayLevel() == 3);
        REQUIRE(shape->GeometryLevel() != nullptr);

        // Object::IsPassable() is `mass < 10`.
        REQUIRE(shape->Mass() == Catch::Approx(1200.0f));
        REQUIRE(shape->Mass() >= 10.0f);

        // What Object::Intersect iterates.
        const ConvexComponents& components = shape->GetGeomComponents();
        REQUIRE(components.Size() == 1);
        const ConvexComponent& box = *components[0];
        REQUIRE(box.Size() == 8);
        REQUIRE(box.NPlanes() == 6);
        // The half-spaces face inward: the box centre is inside, a point a metre
        // outside the +x wall is not. This is the winding contract the collision
        // code relies on, stated once for the ODOL face order.
        REQUIRE(box.IsInside(kBoxCentre));
        REQUIRE_FALSE(box.IsInside(EngineVector3(3.0f, 1.5f, 2.0f)));
        REQUIRE_FALSE(box.IsInside(EngineVector3(1.0f, -1.0f, 2.0f)));
    }
}

TEST_CASE("COL-001: MLOD carries #Mass# and selection faces as source face indices",
          "[ShapeAdapter][collision][MLOD][COL-001]")
{
    const auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    REQUIRE(model.sourceFormat == "MLOD");
    REQUIRE(model.lodLevels.size() == 2);
    const auto& geometry = model.lodLevels[1];
    REQUIRE(geometry.purpose == Poseidon::Model::LodPurpose::Geometry);
    // The loader splits corners per face normal/uv, so 8 positions arrive as up to
    // 24 vertices; the count that matters for collision is faces and planes.
    REQUIRE(geometry.mesh.vertices.size() >= 8);
    REQUIRE(geometry.mesh.quads.size() == 6);
    REQUIRE(geometry.mesh.triangles.empty());

    // The `#Mass#` tagg: 8 x 150 kg, per vertex after the loader's sort.
    REQUIRE(geometry.mesh.vertexMass.size() == geometry.mesh.vertices.size());
    float total = 0.0f;
    for (float m : geometry.mesh.vertexMass)
        total += m;
    REQUIRE(total == Catch::Approx(1200.0f));
    // The visual LOD carried no mass and says so.
    REQUIRE(model.lodLevels[0].mesh.vertexMass.empty());

    // Selection faces are SOURCE FACE INDICES: six quads, six entries 0..5. The
    // triangulated-stream encoding would have produced twelve.
    const auto& component = SelectionNamed(geometry.mesh, "Component01");
    REQUIRE(component.triangleIndices == std::vector<uint32_t>{0, 1, 2, 3, 4, 5});
    REQUIRE(component.vertexIndices.size() >= 8);
}

TEST_CASE("COL-001: an MLOD box collides -- quads reach the component, mass reaches the shape",
          "[ShapeAdapter][collision][MLOD][COL-001]")
{
    const auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape != nullptr);

    REQUIRE(shape->FindGeometryLevel() == 1);
    Shape* geom = shape->GeometryLevel();
    REQUIRE(geom != nullptr);
    REQUIRE(geom->NFaces() == 6);

    // Mass from the tagg, through CalculateMass, so the box is not passable.
    REQUIRE(shape->Mass() == Catch::Approx(1200.0f).margin(0.5f));
    REQUIRE(shape->Mass() >= 10.0f);

    // The MLOD branch's autocenter: this fixture carries no `autocenter` property,
    // so the shape is recentred exactly as LODShape::Load would recentre it, and
    // the offset is RECORDED in _boundingCenter rather than lost. The visual LOD
    // lies inside the box's footprint, so the model bbox is the box and its
    // centre is the box centre. (MLODLoader::load does not run Model::compile,
    // so nothing recentred the IR before the adapter saw it.)
    REQUIRE(shape->BoundingCenter().X() == Catch::Approx(1.0f));
    REQUIRE(shape->BoundingCenter().Y() == Catch::Approx(1.5f));
    REQUIRE(shape->BoundingCenter().Z() == Catch::Approx(2.0f));

    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == 1);
    const ConvexComponent& box = *components[0];
    REQUIRE(box.NPlanes() == 6);
    REQUIRE(box.Size() >= 8);
    // In shape space the box is now centred on the origin.
    const EngineVector3 centre = kBoxCentre - shape->BoundingCenter();
    REQUIRE(centre.SquareSize() == Catch::Approx(0.0f).margin(1e-8f));
    REQUIRE(box.IsInside(centre));
    REQUIRE_FALSE(box.IsInside(centre + EngineVector3(2.0f, 0.0f, 0.0f)));
    REQUIRE_FALSE(box.IsInside(centre - EngineVector3(0.0f, 2.5f, 0.0f)));
    // And the map type is resolved on this branch too, not left to DoClear: no
    // `map` property, no vegetation path -> MapHide, not the zeroth enumerator
    // (MapTree), which is the wind-sway gate.
    REQUIRE(shape->GetMapType() == Poseidon::MapHide);
}

TEST_CASE("COL-001: MLOD `autocenter=0` is honoured by the adapter, not only by the IR",
          "[ShapeAdapter][collision][MLOD][COL-001]")
{
    // The xob converter's --fix-origin writes `autocenter=0` on every LOD. Model.cpp
    // honoured it (no IR-level recentring) and the adapter then recentred anyway
    // through CalculateBoundingSphere, moving every vertex to the bbox centre and
    // recording the shift in _boundingCenter -- which the authored-elevation
    // placement path never reads. That is a house sunk by half its height.
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    for (auto& lod : model.lodLevels)
    {
        Poseidon::Model::NamedProperty property;
        property.name = "autocenter";
        property.value = "0";
        lod.mesh.properties.push_back(property);
    }
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape != nullptr);
    REQUIRE(shape->BoundingCenter().SquareSize() == Catch::Approx(0.0f).margin(1e-8f));
    // The geometry stayed where it was authored: the box corner is still at the origin.
    Shape* geom = shape->GeometryLevel();
    REQUIRE(geom != nullptr);
    REQUIRE(geom->Min().X() == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(geom->Min().Y() == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(geom->Max().Y() == Catch::Approx(3.0f).margin(1e-5f));
    // Collision is unaffected by where the origin sits.
    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == 1);
    REQUIRE(components[0]->IsInside(kBoxCentre));
    REQUIRE(shape->Mass() >= 10.0f);
}

TEST_CASE("SINKHOLE W1: MLOD `autocenter=0` on the GEOMETRY LOD only survives Model::compile",
          "[ShapeAdapter][MLOD][autocenter][sinkhole]")
{
    // Oxygen and OFP-era models keep `autocenter=0` on the geometry LOD alone. The game loads MLOD through
    // ModelCache, which runs Model::compile before the adapter; compile checked only the first LOD, recentred
    // the vertices, and the adapter (which reads the geometry LOD) recorded no offset -- a shift baked into the
    // model. The fixture box spans y 0..3; with the property honoured it must stay there.
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    bool tagged = false;
    for (auto& lod : model.lodLevels)
    {
        if (lod.purpose != Poseidon::Model::LodPurpose::Geometry)
            continue;
        Poseidon::Model::NamedProperty property;
        property.name = "autocenter";
        property.value = "0";
        lod.mesh.properties.push_back(property);
        tagged = true;
    }
    REQUIRE(tagged);
    REQUIRE(model.compile());
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape != nullptr);
    REQUIRE(shape->BoundingCenter().SquareSize() == Catch::Approx(0.0f).margin(1e-8f));
    Shape* geom = shape->GeometryLevel();
    REQUIRE(geom != nullptr);
    REQUIRE(geom->Min().Y() == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(geom->Max().Y() == Catch::Approx(3.0f).margin(1e-5f));
}

TEST_CASE("COL-001: a real model from a local corpus collides",
          "[ShapeAdapter][collision][COL-001][corpus]")
{
    // POSEIDON_COLLISION_FIXTURE=<path to a .p3d with a geometry LOD> -- any
    // generation the loaders read (ODOL 7/40/48-54/73, MLOD). Set
    // POSEIDON_COLLISION_FIXTURE_MASSLESS=1 if the model is known to carry no mass
    // (e.g. a Reforger conversion), which relaxes the mass check to "components only".
    const char* path = std::getenv("POSEIDON_COLLISION_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_COLLISION_FIXTURE to a model with a geometry LOD");
    if (!std::filesystem::exists(path))
        SKIP("POSEIDON_COLLISION_FIXTURE does not exist");

    Poseidon::ModelCache cache;
    auto model = cache.load(path);
    REQUIRE(model != nullptr);
    INFO("format " << model->sourceFormat << " revision " << model->sourceVersion);
    // ShapeBank keys the shape by the virtual name; a plain path serves here.
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(*model, false));
    REQUIRE(shape != nullptr);

    const int geomLevel = shape->FindGeometryLevel();
    REQUIRE(geomLevel >= 0);
    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() > 0);

    // Every component's half-spaces must face inward: the centroid of a convex
    // hull's own vertices is inside it. A failure here is the source winding not
    // being the engine's, and no amount of mass would make that model collide.
    for (int c = 0; c < components.Size(); ++c)
    {
        const ConvexComponent& component = *components[c];
        Shape* geom = component.GetShape();
        REQUIRE(geom != nullptr);
        REQUIRE(component.NPlanes() >= 4);
        EngineVector3 centroid(VZero);
        for (int i = 0; i < component.Size(); ++i)
            centroid += geom->Pos(component[i]);
        centroid = centroid * (1.0f / static_cast<float>(component.Size()));
        INFO("component " << c << " of " << components.Size());
        REQUIRE(component.IsInside(centroid));
    }

    const char* massless = std::getenv("POSEIDON_COLLISION_FIXTURE_MASSLESS");
    if (!massless || !*massless || std::string(massless) == "0")
        REQUIRE(shape->Mass() >= 10.0f); // Object::IsPassable() is `mass < 10`
}
